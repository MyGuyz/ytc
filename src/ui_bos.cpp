// Brick OS look for YTC (brick-os DESIGN.md §5, §7.5): the OS's theme colours and status bar settings, its icons,
// the status bar, the hint bar, the toast, the focus ring and the drawer. Sizes are the 1024x768 mockup's px.
#include "ui.h"
#include "platform.h"
#include "i18n.h"
#include <SDL.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

namespace ui {

namespace {
const gfx::Color kBg{0, 0, 0, 1}, kWhite{1, 1, 1, 1};
double lum(uint32_t c) {   // WCAG relative luminance (bos_ui.c)
    double v[3];
    for (int i = 0; i < 3; i++) {
        double x = ((c >> (16 - 8 * i)) & 255) / 255.0;
        v[i] = x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}
double contrast(uint32_t a, uint32_t b) {
    double la = lum(a), lb = lum(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}
uint32_t text_on(uint32_t bg, int mode) {   // BOS_textOn: 0 auto, 1 white, 2 black, 0x1000000|rgb
    if (mode & 0x1000000) return (uint32_t)mode & 0xFFFFFF;
    if (mode == 1) return 0xFFFFFF;
    if (mode == 2) return 0x0B0B0D;
    return contrast(bg, 0x0B0B0D) >= contrast(bg, 0xFFFFFF) ? 0x0B0B0D : 0xFFFFFF;
}
}  // namespace

// ธีมสี and แถบสถานะ live in the OS's shared settings file; read it again every 2 s so a change shows up
void App::bos_refresh() {
    unsigned now = SDL_GetTicks();
    if (bos_read_ms_ && now - bos_read_ms_ < 2000) return;
    bos_read_ms_ = now ? now : 1;
    static const char* keys[10] = {"themefocus", "themefocustext", "themeprimary", "themeprimarytext", "themehint",
                                   "themehinttext", "themeicon", "themebg", "themepreset", "themecustom"};
    int th[10] = {-1, 0, -1, 0, -1, 0, 0, 0, 0, 0}, bits = 63, c24 = 1;
    std::ifstream f("/mnt/SDCARD/.userdata/shared/minuisettings.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        long v = std::strtol(line.c_str() + eq + 1, nullptr, 0);
        for (int i = 0; i < 10; i++)
            if (k == keys[i]) th[i] = (int)v;
        if (k == "statusbar") bits = (int)v & 63;
        else if (k == "clock24h") c24 = (int)v;
    }
    static const uint32_t P[6][2] = {{0xF2F2F7, 0x4A9BFF}, {0x64B5FF, 0x64B5FF}, {0xFF8FC0, 0xFF6FAE},
                                     {0x5EE08A, 0x30D158}, {0xFFB04A, 0xFF9F0A}, {0xC79BFF, 0xBF5AF2}};
    int pi = (th[8] < 0 || th[8] > 5) ? 0 : th[8];
    uint32_t focus, onf, prim, onp, hint, onh;
    if (th[9]) {
        focus = th[0] < 0 ? 0xF2F2F7 : (uint32_t)th[0] & 0xFFFFFF;
        prim = th[2] < 0 ? 0x4A9BFF : (uint32_t)th[2] & 0xFFFFFF;
        hint = th[4] < 0 ? 0xF2F2F7 : (uint32_t)th[4] & 0xFFFFFF;
        onf = text_on(focus, th[1]); onp = text_on(prim, th[3]); onh = text_on(hint, th[5]);
    } else {
        focus = hint = P[pi][0]; prim = P[pi][1];
        onf = onh = text_on(focus, 0); onp = text_on(prim, 0);
    }
    bos_.focus = gfx::Color::rgb(focus); bos_.onfocus = gfx::Color::rgb(onf);
    bos_.primary = gfx::Color::rgb(prim); bos_.onprimary = gfx::Color::rgb(onp);
    bos_.hint = gfx::Color::rgb(hint); bos_.onhint = gfx::Color::rgb(onh);
    bos_.bar_bits = bits; bos_.clock24 = c24 != 0;
}

// The OS's line icons (white, tinted when drawn): YTC's own 64 px copies first, then the OS's
void App::bos_icon(gfx::Renderer& rn, const char* name, float x, float y, float size, gfx::Color c) {
    auto it = bos_icons_.find(name);
    if (it == bos_icons_.end()) {
        std::unique_ptr<gfx::Texture> t;
        for (const std::string& dir : {platform::exe_dir() + "/data/icons/bos/", std::string("data/icons/bos/"),
                                       std::string("/mnt/SDCARD/.system/res/bos/icons/")}) {
            std::ifstream f(dir + name + ".png", std::ios::binary);
            if (!f) continue;
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!b.empty() && (t = gfx::Texture::from_encoded(b.data(), b.size()))) break;
        }
        it = bos_icons_.emplace(name, std::move(t)).first;
    }
    if (it->second) rn.textured({x, y, size, size}, *it->second, c);
}

// §5.1: 64 high; the page's name left (30/600); right to left the battery (46x24 glyph, % in its colour, a bolt
// while charging), the clock, WiFi while connected — each as ตั้งค่า › หน้าตา › แถบสถานะ says
void App::design_status_bar(gfx::Renderer& rn, const std::string& title) {
    bos_refresh();
    const int W = win_->width();
    const float u = win_->height() / 768.f, h = 64 * u, cy = h / 2, pad = 32 * u;
    rn.quad({0, 0, (float)W, h}, kBg);
    const gfx::Font& f = dz(DZ_STATUS);
    if (!title.empty()) rn.text(f, f.ellipsize(title, W * 0.55f), pad, cy - f.line_height() / 2, kWhite);

    static unsigned last = 0;
    static int pct = -1;
    static bool chg = false, online = false;
    if (pct < 0 || SDL_GetTicks() - last > 5000) {
        last = SDL_GetTicks();
        for (const char* d : {"/sys/class/power_supply/axp2202-battery/", "/sys/class/power_supply/battery/"}) {
            std::ifstream fc(std::string(d) + "capacity");
            int v;
            if (!(fc >> v)) continue;
            pct = v < 0 ? 0 : v > 100 ? 100 : v;
            std::ifstream fs(std::string(d) + "status");
            std::string st;
            fs >> st;
            chg = st == "Charging" || st == "Full";
            break;
        }
        std::ifstream fw("/sys/class/net/wlan0/operstate");
        std::string ws;
        fw >> ws;
        online = ws == "up";
    }
    float x = W - pad;
    if (pct >= 0) {
        gfx::Color col = chg ? gfx::Color::rgb(0x30D158) : pct <= 10 ? gfx::Color::rgb(0xFF453A)
                       : pct <= 20 ? gfx::Color::rgb(0xFFD60A) : kWhite;
        float bw = 46 * u, bh = 24 * u, b = 3 * u, nub = 4 * u;
        float bx = x - nub - bw, by = cy - bh / 2;
        rn.round_rect({bx, by, bw, bh}, 7 * u, col);
        rn.round_rect({bx + b, by + b, bw - 2 * b, bh - 2 * b}, 4 * u, kBg);
        float inner = bw - 4 * b, fill = inner * pct / 100.f;
        if (fill > 0) rn.round_rect({bx + 2 * b, by + 2 * b, fill, bh - 4 * b}, 2 * u, col);
        rn.round_rect({bx + bw, cy - 4 * u, nub, 8 * u}, 2 * u, col);
        x = bx;
        if (bos_.bar_bits & 32) {
            std::string p = std::to_string(pct) + "%";
            x -= 8 * u + f.text_width(p);
            rn.text(f, p, x, cy - f.line_height() / 2, col);
        }
        if (chg) {
            x -= 8 * u + 26 * u;
            bos_icon(rn, "bolt", x, cy - 13 * u, 26 * u, col);
        }
    }
    if (bos_.bar_bits & 16) {
        char clock[16];
        time_t now = time(nullptr);
        strftime(clock, sizeof clock, bos_.clock24 ? "%H:%M" : "%I:%M", localtime(&now));
        x -= 22 * u + f.text_width(clock);
        rn.text(f, clock, x, cy - f.line_height() / 2, kWhite);
    }
    if ((bos_.bar_bits & 8) && online) {
        x -= 22 * u + 32 * u;
        bos_icon(rn, "wifi", x, cy - 16 * u, 32 * u, kWhite);
    }
}

// §5.2: 64 high; A B X Y a 40 circle (24/700), other buttons a pill (22/700, radius 10), in the hint colour;
// the word 30 text2. `right` starts with A, drawn rightmost; `left` (at most 2) from the left edge.
void App::design_hint_bar(gfx::Renderer& rn, const Hints& right, const Hints& left) {
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, top = H - 64 * u, cy = top + 32 * u;
    rn.quad({0, top, (float)W, 64 * u}, kBg);
    const gfx::Font &lf = dz(DZ_HINT), &ff = dz(DZ_HINTKEY), &wf = dz(DZ_HKEYW);
    auto face = [](const std::string& b) { return b.size() == 1 && std::strchr("ABXY", b[0]); };
    auto cap_w = [&](const std::string& b) { return face(b) ? 40 * u : wf.text_width(b) + 24 * u; };
    auto width = [&](const std::pair<std::string, std::string>& h) {
        return cap_w(h.first) + 12 * u + lf.text_width(h.second);
    };
    auto draw = [&](const std::pair<std::string, std::string>& h, float x) {
        float cw = cap_w(h.first);
        const gfx::Font& kf = face(h.first) ? ff : wf;
        rn.round_rect({x, cy - 20 * u, cw, 40 * u}, face(h.first) ? 20 * u : 10 * u, bos_.hint);
        rn.text(kf, h.first, x + (cw - kf.text_width(h.first)) / 2, cy - kf.line_height() / 2, bos_.onhint);
        rn.text(lf, h.second, x + cw + 12 * u, cy - lf.line_height() / 2, gfx::Color::rgb(0xB4B4BA));
    };
    float x = 32 * u;
    for (size_t i = 0; i < left.size() && i < 2; ++i) {
        draw(left[i], x);
        x += width(left[i]) + 28 * u;
    }
    x = W - 32 * u;
    for (size_t i = 0; i < right.size() && i < 4; ++i) {
        x -= width(right[i]);
        draw(right[i], x);
        x -= 28 * u;
    }
}

// §5.9 toast: a pill (surface2, radius 28, 26 text) centred above the hint bar
void App::design_toast(gfx::Renderer& rn) {
    if (status_msg_.empty() || SDL_GetTicks() >= status_until_) return;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    const gfx::Font& f = dz(DZ_TOAST);
    std::string msg = f.ellipsize(status_msg_, W - 160 * u);
    float bw = f.text_width(msg) + 56 * u, bh = 56 * u, bx = (W - bw) / 2, by = H - 64 * u - 20 * u - bh;
    rn.round_rect({bx, by, bw, bh}, bh / 2, gfx::Color::rgb(0x2C2C30));
    rn.text(f, msg, bx + 28 * u, by + (bh - f.line_height()) / 2, kWhite);
}

// §4 pictures: a 4 px ring in the focus colour outside a 4 px gap
void App::design_ring(gfx::Renderer& rn, const gfx::Rect& r, float radius) {
    const float u = win_->height() / 768.f;
    rn.ring({r.x - 8 * u, r.y - 8 * u, r.w + 16 * u, r.h + 16 * u}, radius + 8 * u, 4 * u, bos_.focus);
}

// §7.5 drawer: 380 wide from the left over the page at 60 % black, rows 72 (icon 30, name 30/600)
void App::design_drawer(gfx::Renderer& rn, const std::vector<std::pair<const char*, std::string>>& rows, int sel,
                        int current) {
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, top = 64 * u, bottom = H - 64 * u;
    rn.quad({0, top, (float)W, bottom - top}, kBg.with_a(0.6f));
    float pw = 412 * u, rh = 72 * u;
    rn.round_rect({-40 * u, top, pw + 40 * u, bottom - top}, 28 * u, gfx::Color::rgb(0x1C1C1F));
    int n = (int)rows.size();
    int vis = std::max(1, (int)((bottom - top - 32 * u) / rh));
    int first = n > vis ? std::min(std::max(0, sel - vis / 2), n - vis) : 0;
    const gfx::Font& f = dz(DZ_DRAWER);
    for (int k = 0; k < vis && first + k < n; ++k) {
        int i = first + k;
        float y = top + 16 * u + k * rh;
        bool hot = i == sel;
        if (hot) rn.round_rect({8 * u, y, pw - 16 * u, rh - 2 * u}, 16 * u, bos_.focus);
        gfx::Color c = hot ? bos_.onfocus : (i == current ? kWhite : gfx::Color::rgb(0xF2F2F7));
        bos_icon(rn, rows[i].first, 36 * u, y + (rh - 30 * u) / 2, 30 * u, c);
        rn.text(f, f.ellipsize(rows[i].second, pw - 120 * u), 88 * u, y + (rh - f.line_height()) / 2, c);
    }
}

}  // namespace ui
