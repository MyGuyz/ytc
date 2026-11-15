// Brick OS look for YTC, the menus (brick-os DESIGN.md 7.5): START's menu = the drawer; the options (SELECT) = the
// §6.10 sheet of two columns of tiles; Settings = Settings' two columns (§6.6); search filters and the player's
// playback rows = a sheet with a list. Sizes are the 1024x768 mockup's px.
#include "ui.h"
#include "i18n.h"
#include <algorithm>

namespace ui {

namespace {
const gfx::Color kBg{0, 0, 0, 1}, kWhite{1, 1, 1, 1};
const gfx::Color kSurf = gfx::Color::rgb(0x1C1C1F), kSurf2 = gfx::Color::rgb(0x2C2C30), kText2 = gfx::Color::rgb(0xB4B4BA),
                 kSep = gfx::Color::rgb(0x38383A), kDanger = gfx::Color::rgb(0xFF453A);
// "Label:  Value" (YTC's value rows) -> label, value
void split_value(const std::string& s, std::string& label, std::string& value) {
    size_t p = s.find(":  ");
    if (p == std::string::npos) { label = s; value.clear(); return; }
    label = s.substr(0, p);
    value = s.substr(p + 3);
}
}  // namespace

int App::settings_cat() const {   // the Settings category a page belongs to (-1: the top level)
    switch (menu_kind_) {
        case MenuKind::SettingsAudio: return 0;
        case MenuKind::SettingsVideo: return 1;
        case MenuKind::SettingsCaptions: return 2;
        case MenuKind::SettingsPlayback: return 3;
        case MenuKind::SettingsBrowsing: case MenuKind::SettingsHomeFeed:
        case MenuKind::FeedManage: case MenuKind::FeedRemoveConfirm: return 4;
        default: return -1;
    }
}

bool App::settings_page() const {
    return settings_kind(menu_kind_) || menu_kind_ == MenuKind::FeedManage || menu_kind_ == MenuKind::FeedRemoveConfirm;
}

// One list row (Settings' right column, the list sheet): label 40, value 30 text2 at the right; a switch for
// On/Off; "‹ value ›" while focused; a chevron for a row that opens a page. Focused: the focus colour x1.04.
void App::design_menu_row(gfx::Renderer& rn, const MenuItem& it, gfx::Rect r, bool foc, bool sep) {
    const float u = win_->height() / 768.f;
    if (foc) {
        float dw = r.w * 0.02f, dh = r.h * 0.02f;
        rn.round_rect({r.x - dw, r.y - dh, r.w + 2 * dw, r.h + 2 * dh}, 22 * u, bos_.focus);
    } else if (sep) rn.quad({r.x + 28 * u, r.y + r.h - 1, r.w - 28 * u, std::max(1.f, u)}, kSep);
    gfx::Color tc = foc ? bos_.onfocus : kWhite, vc = foc ? bos_.onfocus : kText2;
    std::string label, value;
    bool val = is_value_action(it.action);
    if (val) split_value(it.label, label, value);
    else label = it.label;
    const gfx::Font &lf = dz(foc ? DZ_SET_LABEL_F : DZ_SET_LABEL), &vf = dz(DZ_SET_VALUE);
    float right = r.x + r.w - 24 * u, cy = r.y + r.h / 2;
    bool on = val && value == i18n::tr(i18n::Str::On), off = val && value == i18n::tr(i18n::Str::Off);
    if (on || off) {   // switch 80x46: on = primary with the knob right
        gfx::Rect sw{right - 80 * u, cy - 23 * u, 80 * u, 46 * u};
        rn.round_rect(sw, 23 * u, on ? bos_.primary : gfx::Color::rgb(0x39393D));
        rn.round_rect({on ? sw.x + sw.w - 42 * u : sw.x + 4 * u, sw.y + 4 * u, 38 * u, 38 * u}, 19 * u, kWhite);
        right = sw.x - 20 * u;
    } else if (val) {
        std::string v = vf.ellipsize(value, r.w * 0.45f);
        if (foc) {
            const char *lch = "\xE2\x80\xB9", *rch = "\xE2\x80\xBA";
            float cw = vf.text_width(rch);
            rn.text(vf, rch, right - cw, cy - vf.line_height() / 2, vc);
            right -= cw + 12 * u;
            rn.text(vf, v, right - vf.text_width(v), cy - vf.line_height() / 2, vc);
            right -= vf.text_width(v) + 12 * u;
            rn.text(vf, lch, right - vf.text_width(lch), cy - vf.line_height() / 2, vc);
            right -= vf.text_width(lch) + 20 * u;
        } else {
            rn.text(vf, v, right - vf.text_width(v), cy - vf.line_height() / 2, vc);
            right -= vf.text_width(v) + 20 * u;
        }
    } else if (is_submenu_action(it.action)) {
        bos_icon(rn, "chev", right - 28 * u, cy - 14 * u, 28 * u, vc);
        right -= 44 * u;
    }
    float lx = r.x + 28 * u;
    rn.text(lf, lf.ellipsize(label, right - lx), lx, cy - lf.line_height() / 2, tc);
}

void App::render_menu(gfx::Renderer& rn) {
    if (menu_kind_ == MenuKind::Main) { render_main_drawer(rn); return; }
    if (settings_page()) { render_settings_page(rn); return; }
    if (menu_kind_ == MenuKind::Context) { render_options_sheet(rn); return; }
    render_list_sheet(rn);
}

// the options: header (thumb 160x90, title 34/700, meta 24) and tiles 80 high in two columns (icon 30, label 28/600)
void App::render_options_sheet(gfx::Renderer& rn) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bottom = H - 64 * u;
    rn.begin(W, H);
    rn.quad({0, 64 * u, (float)W, bottom - 64 * u}, kBg.with_a(0.6f));
    const yt::SearchResult& t = menu_target_;
    bool head = !t.title.empty();
    int n = (int)menu_items_.size(), rows = (n + 1) / 2;
    const float th = 80 * u, gy = 12 * u, gx = 16 * u, padx = 40 * u;
    int vis_rows = std::min(rows, 4);
    float grid_h = vis_rows * th + (vis_rows - 1) * gy;
    float sh = 24 * u + (head ? 90 * u + 20 * u : 0) + grid_h + 28 * u;
    float top = bottom - sh;
    rn.round_rect({0, top, (float)W, sh + 40 * u}, 32 * u, kSurf);
    rn.quad({0, bottom, (float)W, (float)H - bottom}, kBg);
    float y = top + 24 * u;
    if (head) {
        gfx::Rect tr{padx, y, 160 * u, 90 * u};
        rn.quad(tr, kSurf2);
        std::string url = t.thumbnail_url;
        if (url.empty() && t.is_channel()) url = chan_meta_.avatar(t.channel_id);
        if (!url.empty()) {
            thumbs_.request(url);
            if (auto* tex = thumbs_.get(url)) rn.textured_cover(tr, *tex, kWhite);
        }
        rn.round_corners(tr, 12 * u, kSurf);
        const gfx::Font &tf = dz(DZ_SHEET_T), &mf = dz(DZ_SHEET_M);
        float tx = padx + 160 * u + 20 * u, tw = W - padx - tx;
        std::string meta;
        auto add = [&](const std::string& p) { if (!p.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + p; };
        if (t.is_channel()) add(t.subs_text);
        else { add(t.author); add(t.view_count_text); add(t.published_text); }
        float bh = tf.line_height() + (meta.empty() ? 0 : mf.line_height());
        float ty = y + (90 * u - bh) / 2;
        rn.text(tf, tf.ellipsize(t.title.substr(0, t.title.find('\n')), tw), tx, ty, kWhite);
        if (!meta.empty()) rn.text(mf, mf.ellipsize(meta, tw), tx, ty + tf.line_height(), kText2);
        y += 90 * u + 20 * u;
    }
    int sel_row = menu_sel_ / 2;
    int first = rows > vis_rows ? std::min(std::max(0, sel_row - vis_rows / 2), rows - vis_rows) : 0;
    float tw = (W - 2 * padx - gx) / 2;
    const gfx::Font& f = dz(DZ_TILE);
    for (int k = 0; k < vis_rows * 2; ++k) {
        int i = first * 2 + k;
        if (i >= n) break;
        gfx::Rect r{padx + (k % 2) * (tw + gx), y + (k / 2) * (th + gy), tw, th};
        bool foc = i == menu_sel_;
        const MenuItem& it = menu_items_[i];
        const char* ic = "list";
        uint32_t col = 0x8E8E93;
        bool danger = false;
        switch (it.action) {
            case MenuAction::WatchLaterToggle: ic = "clock"; col = 0xFF9F0A; break;
            case MenuAction::FavoriteToggle: ic = "heart"; col = 0xFF375F;
                danger = fav_ids_.count(t.channel_id) > 0; break;
            case MenuAction::FavoritePlaylistToggle: ic = "heart"; col = 0xFF375F;
                danger = fav_pl_ids_.count(t.playlist_id) > 0; break;
            case MenuAction::ShowDescription: case MenuAction::ShowPlaylistDescription:
                ic = "text"; col = 0x64D2FF; break;
            case MenuAction::ShowComments: ic = "comment"; col = 0x30D158; break;
            case MenuAction::ShowChannelDescription: ic = "info"; col = 0x8E8E93; break;
            case MenuAction::OpenChannel: ic = "user"; col = 0xBF5AF2; break;
            case MenuAction::CastToDevice: ic = "cast"; col = 0x0A84FF; break;
            case MenuAction::DownloadVideo: ic = "download"; col = 0x30D158; break;
            case MenuAction::RemoveDownload: case MenuAction::ClearHistory: ic = "trash"; danger = true; break;
            case MenuAction::GoPlayback: case MenuAction::OpenSearchFilters: ic = "sliders"; col = 0xFF9F0A; break;
            case MenuAction::AddSearchToFeed: ic = "plus"; col = 0x30D158; break;
            case MenuAction::PlayPostVideo: ic = "play"; col = 0xFF0033; break;
            case MenuAction::CycleSpeed: ic = "speed"; col = 0x64D2FF; break;
            default: break;
        }
        if (danger) col = 0xFF453A;
        gfx::Color tc = foc ? bos_.onfocus : danger ? kDanger : kWhite;
        gfx::Color icol = foc ? bos_.onfocus : gfx::Color::rgb(col);
        if (foc) {
            float dw = r.w * 0.02f, dh = r.h * 0.02f;
            rn.round_rect({r.x - dw, r.y - dh, r.w + 2 * dw, r.h + 2 * dh}, 22 * u, bos_.focus);
        } else rn.round_rect(r, 22 * u, kSurf2);
        bos_icon(rn, ic, r.x + 22 * u, r.y + (th - 30 * u) / 2, 30 * u, icol);
        std::string label = it.label;
        if (is_value_action(it.action)) {
            std::string l, v;
            split_value(label, l, v);
            label = l + "  " + v;
        }
        float lx = r.x + 22 * u + 30 * u + 16 * u;
        rn.text(f, f.ellipsize(label, r.x + r.w - 20 * u - lx), lx, r.y + (th - f.line_height()) / 2, tc);
    }
    bool val = menu_sel_ < n && is_value_action(menu_items_[menu_sel_].action);
    design_hint_bar(rn, {{"A", i18n::tr(S::HintSelect)}, {"B", i18n::tr(S::HintClose)}},
                    val ? Hints{{"\xE2\x86\x90\xE2\x86\x92", i18n::tr(S::HintChange)}} : Hints{});
    rn.end();
}

// search filters, the player's playback rows: a sheet with a heading (34/700) and list rows 88
void App::render_list_sheet(gfx::Renderer& rn) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bottom = H - 64 * u;
    rn.begin(W, H);
    rn.quad({0, 64 * u, (float)W, bottom - 64 * u}, kBg.with_a(0.6f));
    std::string heading = menu_kind_ == MenuKind::PlayerPlayback ? i18n::tr(S::SetPlaybackMenu)
                        : menu_kind_ == MenuKind::SearchFilters ? i18n::tr(S::MenuSearchFilters) : menu_target_.title;
    const gfx::Font& hf = dz(DZ_SHEET_T);
    int n = (int)menu_items_.size();
    const float rh = 88 * u, padx = 40 * u;
    float max_list = bottom - 104 * u - 24 * u - hf.line_height() - 16 * u - 28 * u;
    int vis = std::max(1, std::min(n, (int)(max_list / rh)));
    float sh = 24 * u + hf.line_height() + 16 * u + vis * rh + 28 * u, top = bottom - sh;
    rn.round_rect({0, top, (float)W, sh + 40 * u}, 32 * u, kSurf);
    rn.quad({0, bottom, (float)W, (float)H - bottom}, kBg);
    rn.text(hf, hf.ellipsize(heading, W - 2 * padx), padx, top + 24 * u, kWhite);
    int first = n > vis ? std::min(std::max(0, menu_sel_ - vis / 2), n - vis) : 0;
    float y = top + 24 * u + hf.line_height() + 16 * u;
    for (int k = 0; k < vis && first + k < n; ++k) {
        int i = first + k;
        design_menu_row(rn, menu_items_[i], {padx - 8 * u, y + k * rh, W - 2 * padx + 16 * u, rh}, i == menu_sel_,
                        k + 1 < vis && first + k + 1 < n);
    }
    bool val = menu_sel_ < n && is_value_action(menu_items_[menu_sel_].action);
    Hints right;
    if (menu_kind_ != MenuKind::SearchFilters) right.push_back({"A", i18n::tr(S::HintSelect)});
    right.push_back({"B", i18n::tr(S::HintClose)});
    design_hint_bar(rn, right, val ? Hints{{"\xE2\x86\x90\xE2\x86\x92", i18n::tr(S::HintChange)}} : Hints{});
    rn.end();
}

// Settings (§6.6): categories left (380, rows 80, a 48 squircle per icon, 40/500), the page right (heading 56/700,
// a group of rows 96). On the top level the focused category's rows show on the right.
void App::render_settings_page(gfx::Renderer& rn) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, top = 64 * u + 8 * u, bottom = H - 64 * u;
    rn.begin(W, H);
    rn.clear(kBg);
    design_status_bar(rn, i18n::tr(S::SettingsTitle));
    static const struct { const char* icon; uint32_t col; S name; } kCats[7] = {
        {"sound", 0xFF375F, S::SetAudioMenu}, {"display", 0x5E5CE6, S::SetVideoMenu},
        {"cc", 0x0A84FF, S::SetCaptionsMenu}, {"play", 0x30D158, S::SetPlaybackMenu},
        {"list", 0xFF9F0A, S::SetBrowsingMenu}, {"globe", 0x30B0C7, S::SetLanguage},
        {"cast", 0x8E8E93, S::SetLinkedDevices}};
    bool top_level = menu_kind_ == MenuKind::Settings;
    int cat = top_level ? menu_sel_ : settings_cat();
    // left: the categories
    const float lx = 24 * u, lw = 380 * u, lrh = 80 * u;
    rn.round_rect({lx, top, lw, 7 * lrh}, 24 * u, kSurf);
    const gfx::Font &cf = dz(DZ_SET_LABEL), &cff = dz(DZ_SET_LABEL_F);
    for (int i = 0; i < 7; ++i) {
        gfx::Rect r{lx, top + i * lrh, lw, lrh};
        bool foc = top_level && i == menu_sel_, cur = !top_level && i == cat;
        if (foc) {
            float dw = r.w * 0.02f, dh = r.h * 0.02f;
            rn.round_rect({r.x - dw, r.y - dh, r.w + 2 * dw, r.h + 2 * dh}, 20 * u, bos_.focus);
        } else if (cur) rn.round_rect(r, 20 * u, kSurf2);
        else if (i < 6 && i + 1 != cat)
            rn.quad({r.x + 88 * u, r.y + lrh - 1, lw - 88 * u, std::max(1.f, u)}, kSep);
        rn.round_rect({r.x + 20 * u, r.y + 16 * u, 48 * u, 48 * u}, 12.5f * u, gfx::Color::rgb(kCats[i].col));
        bos_icon(rn, kCats[i].icon, r.x + 29 * u, r.y + 25 * u, 30 * u, kWhite);
        const gfx::Font& f = foc ? cff : cf;
        rn.text(f, f.ellipsize(i18n::tr(kCats[i].name), lw - 110 * u), r.x + 90 * u, r.y + (lrh - f.line_height()) / 2,
                foc ? bos_.onfocus : kWhite);
    }
    // right: the page
    const float rx = lx + lw + 28 * u, rw = W - 32 * u - rx;
    std::string heading = i18n::tr(kCats[std::max(0, std::min(6, cat))].name);
    if (menu_kind_ == MenuKind::SettingsHomeFeed) heading = i18n::tr(S::SetHomeFeed);
    else if (menu_kind_ == MenuKind::FeedManage) heading = i18n::tr(S::SetCustomFeed);
    else if (menu_kind_ == MenuKind::FeedRemoveConfirm) heading = i18n::tr(S::FeedRemoveConfirm);
    const gfx::Font& hf = dz(DZ_SET_HEAD);
    rn.text(hf, hf.ellipsize(heading, rw), rx + 8 * u, top - 4 * u, kWhite);
    std::vector<MenuItem> rows;
    int rsel = -1;
    if (top_level) {   // the focused category's rows, read-only
        if (cat == 5 && cat < (int)menu_items_.size()) rows.push_back(menu_items_[cat]);
        else if (cat >= 0 && cat <= 4) {
            MenuKind k = menu_kind_;
            auto items = menu_items_;
            int s = menu_sel_;
            bool o = menu_open_;
            switch (cat) {
                case 0: open_settings_audio(); break;
                case 1: open_settings_video(); break;
                case 2: open_settings_captions(); break;
                case 3: open_settings_playback(); break;
                default: open_settings_browsing(); break;
            }
            rows = menu_items_;
            menu_kind_ = k; menu_items_ = items; menu_sel_ = s; menu_open_ = o;
        }
    } else { rows = menu_items_; rsel = menu_sel_; }
    const float gy = top + hf.line_height() + 6 * u, rh = 96 * u;
    int n = (int)rows.size();
    int vis = std::max(1, (int)((bottom - 16 * u - gy) / rh));
    int first = (n > vis && rsel >= 0) ? std::min(std::max(0, rsel - vis / 2), n - vis) : 0;
    int shown = std::min(vis, n - first);
    if (shown > 0) rn.round_rect({rx, gy, rw, shown * rh}, 24 * u, kSurf);
    for (int k = 0; k < shown; ++k)
        design_menu_row(rn, rows[first + k], {rx, gy + k * rh, rw, rh}, first + k == rsel, k + 1 < shown);
    int n_items = (int)menu_items_.size();
    bool val = menu_sel_ < n_items && is_value_action(menu_items_[menu_sel_].action);
    Hints right;
    if (menu_sel_ < n_items && !val) right.push_back({"A", i18n::tr(S::HintSelect)});
    right.push_back({"B", i18n::tr(S::HintBack)});
    design_hint_bar(rn, right, val ? Hints{{"\xE2\x86\x90\xE2\x86\x92", i18n::tr(S::HintChange)}} : Hints{});
    rn.end();
}

}  // namespace ui
