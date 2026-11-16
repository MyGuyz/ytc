// Brick OS look for YTC, the player's overlay (brick-os DESIGN.md 7.5, board v2-YouTubePlayer): the status bar,
// the name and channel, chips (quality, speed, captions, follow), the red seek bar with chapter marks and the
// chapter's name between the times, the hint bar. Sizes are the 1024x768 mockup's px.
#include "ui.h"
#include "i18n.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ui {

namespace {
const gfx::Color kBg{0, 0, 0, 1}, kWhite{1, 1, 1, 1}, kText2 = gfx::Color::rgb(0xB4B4BA);
std::string tstr(double t) {
    if (t < 0) t = 0;
    int h = (int)t / 3600, m = (int)t / 60 % 60, s = (int)t % 60;
    char b[24];
    if (h) std::snprintf(b, sizeof b, "%d:%02d:%02d", h, m, s);
    else std::snprintf(b, sizeof b, "%d:%02d", m, s);
    return b;
}
// the chapter's name: the description line holding its timestamp, without the timestamp and the dashes around it
std::string chapter_name(const std::string& desc, double t) {
    int h = (int)t / 3600, m = (int)t / 60 % 60, s = (int)t % 60;
    char a[24], b[24], c[24];
    std::snprintf(a, sizeof a, "%d:%02d", h * 60 + m, s);
    std::snprintf(b, sizeof b, "%02d:%02d", h * 60 + m, s);
    std::snprintf(c, sizeof c, "%d:%02d:%02d", h, m, s);
    size_t at = 0;
    while (at < desc.size()) {
        size_t end = desc.find('\n', at);
        if (end == std::string::npos) end = desc.size();
        std::string line = desc.substr(at, end - at);
        for (const char* tok : {c, b, a}) {
            size_t p = line.find(tok);
            if (p == std::string::npos) continue;
            size_t q = p + std::string(tok).size();
            if (q < line.size() && (std::isdigit((unsigned char)line[q]) || line[q] == ':')) continue;
            if (p > 0 && (std::isdigit((unsigned char)line[p - 1]) || line[p - 1] == ':')) continue;
            std::string name = line.substr(0, p) + line.substr(q);
            auto junk = [](char ch) { return ch == ' ' || ch == '-' || ch == '|' || ch == ':' || ch == '(' ||
                                             ch == ')' || ch == '[' || ch == ']' || ch == '\t' || ch == '\r'; };
            while (!name.empty() && junk(name.front())) name.erase(name.begin());
            while (!name.empty() && junk(name.back())) name.pop_back();
            // an en/em dash at the start (UTF-8 E2 80 93 / 94)
            while (name.size() >= 3 && (unsigned char)name[0] == 0xE2 && (unsigned char)name[1] == 0x80 &&
                   ((unsigned char)name[2] == 0x93 || (unsigned char)name[2] == 0x94)) {
                name.erase(0, 3);
                while (!name.empty() && junk(name.front())) name.erase(name.begin());
            }
            return name;
        }
        at = end + 1;
    }
    return "";
}
}  // namespace

void App::player_overlay(gfx::Renderer& rn, double pos, double dur, float alpha) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bottom = H - 64 * u;
    const gfx::Color text2 = gfx::Color::rgb(0xB4B4BA).with_a(alpha), white = kWhite.with_a(alpha);
    // top: the status bar, then the name 34/700 and "channel · views" 24 on black 55 %
    const gfx::Font &tf = dz(DZ_SHEET_T), &mf = dz(DZ_SHEET_M);
    rn.quad({0, 64 * u, (float)W, tf.line_height() + mf.line_height() + 20 * u}, kBg.with_a(0.55f * alpha));
    design_status_bar(rn);
    rn.text(tf, tf.ellipsize(now_playing_title_, W - 64 * u), 32 * u, 64 * u + 4 * u, white);
    std::string meta;
    auto add = [&](const std::string& p) { if (!p.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + p; };
    add(now_playing_item_.author);
    add(now_playing_item_.view_count_text);
    rn.text(mf, mf.ellipsize(meta, W - 64 * u), 32 * u, 64 * u + 4 * u + tf.line_height(), text2);

    // centre: paused = a 120 circle (black 50 %) with the pause sign
    if (player_.paused()) {
        float d = 120 * u, cx = W / 2.f, cy = H * 0.5f;
        rn.round_rect({cx - d / 2, cy - d / 2, d, d}, d / 2, kBg.with_a(0.5f * alpha));
        bos_icon(rn, "pause", cx - 30 * u, cy - 30 * u, 60 * u, white);
    }

    // bottom: chips, the seek bar, the times; on black 60 %
    const float chip_h = 52 * u, bar_y = bottom - 68 * u, chips_y = bar_y - 22 * u - chip_h;
    rn.quad({0, chips_y - 16 * u, (float)W, bottom - chips_y + 16 * u}, kBg.with_a(0.6f * alpha));
    {
        int mh = effective_max_height();
        char sp[16];
        std::snprintf(sp, sizeof sp, "%gx", playback_speed_);
        std::string cc = cc_sel_ > 0 && cc_sel_ <= (int)cc_tracks_.size() ? cc_tracks_[cc_sel_ - 1].name
                                                                           : std::string(i18n::tr(S::Off));
        bool fav = !now_playing_item_.channel_id.empty() && fav_ids_.count(now_playing_item_.channel_id);
        struct Chip { const char* icon; std::string label, value; } chips[4] = {
            {"hd", i18n::tr(S::OptQuality), mh > 0 ? std::to_string(mh) + "p" : std::string("Auto")},
            {"speed", i18n::tr(S::OptSpeed), sp},
            {"cc", i18n::tr(S::SetCaptionsMenu), cc},
            {"heart", "", i18n::tr(fav ? S::Subscribed : S::Subscribe)}};
        const gfx::Font &lf = dz(DZ_SHEET_M), &vf = dz(DZ_CHIP_VAL);
        float x = 40 * u;
        for (auto& c : chips) {
            std::string v = vf.ellipsize(c.value, 180 * u);
            float w = 22 * u + 24 * u + 10 * u + (c.label.empty() ? 0 : lf.text_width(c.label) + 10 * u) +
                      vf.text_width(v) + 22 * u;
            if (x + w > W - 32 * u) break;
            rn.round_rect({x, chips_y, w, chip_h}, chip_h / 2, gfx::Color::rgb(0x2C2C30).with_a(0.9f * alpha));
            float cx = x + 22 * u, cy = chips_y + chip_h / 2;
            bos_icon(rn, c.icon, cx, cy - 12 * u, 24 * u, white);
            cx += 24 * u + 10 * u;
            if (!c.label.empty()) {
                rn.text(lf, c.label, cx, cy - lf.line_height() / 2, text2);
                cx += lf.text_width(c.label) + 10 * u;
            }
            rn.text(vf, v, cx, cy - vf.line_height() / 2, white);
            x += w + 14 * u;
        }
    }
    float frac = dur > 0 ? (float)std::min(1.0, std::max(0.0, pos / dur)) : 0.f;
    const float px = 40 * u, pw = W - 80 * u, ph = 8 * u;
    rn.round_rect({px, bar_y - ph / 2, pw, ph}, ph / 2, kWhite.with_a(0.3f * alpha));
    if (frac > 0) rn.round_rect({px, bar_y - ph / 2, std::max(ph, pw * frac), ph}, ph / 2,
                                gfx::Color::rgb(0xFF0033).with_a(alpha));
    std::string chapter;
    if (dur > 0 && !now_playing_chapters_.empty()) {
        int cur = 0;
        for (int i = 0; i < (int)now_playing_chapters_.size(); ++i) {
            double t = now_playing_chapters_[i];
            if (t <= pos + 0.5) cur = i;
            if (i > 0 && t < dur)
                rn.quad({px + pw * (float)(t / dur) - 1.5f * u, bar_y - ph / 2, 3 * u, ph},
                        gfx::Color::rgb(0xFFD60A).with_a(alpha));
        }
        chapter = chapter_name(now_playing_desc_, now_playing_chapters_[cur]);
    }
    rn.round_rect({px + pw * frac - 12 * u, bar_y - 12 * u, 24 * u, 24 * u}, 12 * u, white);
    const gfx::Font &timef = dz(DZ_CHIP_VAL), &chf = dz(DZ_CHAPTER);
    float ty = bar_y + 16 * u;
    rn.text(timef, tstr(pos), px, ty, white);
    if (dur > 0) {
        std::string rem = "\xE2\x88\x92" + tstr(dur - pos);   // U+2212
        rn.text(timef, rem, px + pw - timef.text_width(rem), ty, text2);
    }
    if (!chapter.empty()) {
        std::string c = chf.ellipsize(i18n::tr(S::ChapterLabel) + chapter, pw * 0.6f);
        rn.text(chf, c, W / 2.f - chf.text_width(c) / 2, ty + 2 * u, text2);
    }
    // DESIGN 7.7: L1 R1 clips on the left; X captions, B back, A play/pause
    design_hint_bar(rn, {{"A", i18n::tr(S::HintPlayPause)}, {"B", i18n::tr(S::HintBack)},
                         {"X", i18n::tr(S::HintCaptions)}}, {{"L1 R1", i18n::tr(S::HintPrevNext)}});
}

// the volume (Up/Down in the player): a pill (surface2) with the sound icon, a bar in primary and the number
void App::player_volume(gfx::Renderer& rn, float a) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bw = 420 * u, bh = 72 * u, bx = (W - bw) / 2, by = H * 0.3f;
    rn.round_rect({bx, by, bw, bh}, bh / 2, gfx::Color::rgb(0x2C2C30).with_a(0.95f * a));
    bos_icon(rn, volume_ == 0 ? "mute" : "sound", bx + 28 * u, by + (bh - 32 * u) / 2, 32 * u, kWhite.with_a(a));
    const gfx::Font& f = dz(DZ_CHIP_VAL);
    std::string pct = std::to_string(volume_) + "%";
    float tw = f.text_width("150%");
    float tx = bx + 28 * u + 32 * u + 20 * u, tr = bx + bw - 28 * u - tw - 16 * u;
    rn.round_rect({tx, by + bh / 2 - 5 * u, tr - tx, 10 * u}, 5 * u, kWhite.with_a(0.25f * a));
    float fr = std::min(1.f, volume_ / 150.f);
    if (fr > 0) rn.round_rect({tx, by + bh / 2 - 5 * u, std::max(10 * u, (tr - tx) * fr), 10 * u}, 5 * u,
                              bos_.primary.with_a(a));
    rn.quad({tx + (tr - tx) * (100.f / 150.f) - u, by + bh / 2 - 10 * u, 2 * u, 20 * u}, kWhite.with_a(0.6f * a));
    rn.text(f, pct, bx + bw - 28 * u - f.text_width(pct), by + (bh - f.line_height()) / 2, kWhite.with_a(a));
}

// ---------- DESIGN 7.7: the "next" row (Down in the player) ----------
void App::render_upnext(gfx::Renderer& rn) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bottom = H - 64 * u;
    const float cw = 240 * u, ch = 135 * u, gap = 24 * u;
    const gfx::Font &lf = dz(DZ_SHEET_M), &tf = dz(DZ_CARD), &tff = dz(DZ_CARD_FOC);
    float top = bottom - 24 * u - tf.line_height() - 12 * u - ch - 16 * u - lf.line_height() - 20 * u;
    rn.quad({0, top, (float)W, (float)H - top}, kBg.with_a(0.75f));
    rn.text(lf, i18n::tr(S::UpNext), 40 * u, top + 20 * u, kWhite);
    int n = (int)upnext_idx_.size();
    float y = top + 20 * u + lf.line_height() + 16 * u;
    // keep the focused card on screen: scroll by whole cards
    int first = std::max(0, upnext_sel_ - (int)((W - 80 * u) / (cw + gap)) + 1);
    for (int k = first; k < n; ++k) {
        float x = 40 * u + (k - first) * (cw + gap);
        if (x > W) break;
        const yt::SearchResult& v = results_[upnext_idx_[k]];
        gfx::Rect r{x, y, cw, ch};
        bool foc = k == upnext_sel_;
        design_thumb(rn, v, r, 18 * u, foc ? 1.f : 0.6f);
        if (foc) design_ring(rn, r, 18 * u);   // DESIGN 1: the ring last
        const gfx::Font& f = foc ? tff : tf;
        rn.text(f, f.ellipsize(v.title, cw), x, y + ch + 12 * u, foc ? kWhite : kText2);
    }
    design_hint_bar(rn, {{"A", i18n::tr(S::HintPlay)}, {"B", i18n::tr(S::HintClose)}}, {});
}

// ---------- DESIGN 7.8: Shorts ----------
// the title in at most two lines of w (Thai has no spaces: break between characters, never before a mark)
static std::vector<std::string> two_lines(const gfx::Font& f, const std::string& s, float w) {
    auto mark = [](unsigned cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E); };
    std::vector<std::string> out;
    size_t start = 0, i = 0, last_ok = 0, last_space = std::string::npos;
    while (i < s.size() && out.size() < 1) {
        unsigned char c0 = s[i];
        size_t len = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
        unsigned cp = c0;
        if (len == 3) cp = ((c0 & 15) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63);
        if (!mark(cp)) last_ok = i;
        if (c0 == ' ') last_space = i;
        if (f.text_width(s.substr(start, i + len - start)) > w) {
            size_t cut = last_space != std::string::npos && last_space > start ? last_space : last_ok;
            if (cut <= start) cut = i;
            out.push_back(s.substr(start, cut - start));
            start = cut;
            while (start < s.size() && s[start] == ' ') ++start;
            break;
        }
        i += len;
    }
    if (out.empty()) { out.push_back(s); return out; }
    if (start < s.size()) out.push_back(f.ellipsize(s.substr(start), w));
    return out;
}

void App::shorts_column(gfx::Renderer& rn, const yt::SearchResult& v, float a) {
    const int H = win_->height();
    const float u = H / 768.f, x = 760 * u, w = 240 * u;
    const gfx::Font &nf = dz(DZ_CARD_FOC), &tf = dz(DZ_SHEET_M), &mf = dz(DZ_META);
    float y = 40 * u;
    std::string ch = v.author.empty() ? now_playing_item_.author : v.author;
    rn.round_rect({x, y, 48 * u, 48 * u}, 24 * u, gfx::Color::rgb(0x3A3A3E).with_a(a));
    std::string av_url = v.channel_id.empty() ? std::string() : chan_meta_.avatar(v.channel_id);
    if (!av_url.empty()) thumbs_.request(av_url);
    if (auto* av = av_url.empty() ? nullptr : thumbs_.get(av_url)) {
        rn.textured_cover({x, y, 48 * u, 48 * u}, *av, {1, 1, 1, a});
        rn.round_corners({x, y, 48 * u, 48 * u}, 24 * u, kBg);
    }
    rn.text(nf, nf.ellipsize(ch, w - 60 * u), x + 60 * u, y + (48 * u - nf.line_height()) / 2, kWhite.with_a(a));
    y += 48 * u + 18 * u;
    for (auto& l : two_lines(tf, v.title, w)) { rn.text(tf, l, x, y, kWhite.with_a(a)); y += tf.line_height(); }
    y += 8 * u;
    if (!v.view_count_text.empty()) { rn.text(mf, mf.ellipsize(v.view_count_text, w), x, y, kText2.with_a(a)); y += mf.line_height(); }
    std::vector<int> list = shorts_list();
    int at = 0;
    for (int k = 0; k < (int)list.size(); ++k) if (list[k] == now_playing_index_) at = k;
    if (!list.empty()) {
        std::string pos = std::to_string(at + 1) + " / " + std::to_string(list.size());
        rn.text(nf, pos, x, y + 32 * u, kWhite.with_a(a));
    }
    // the dots on the left: where in the list (at most 7 around the current one)
    int n = (int)list.size(), lo = std::max(0, std::min(at - 3, n - 7)), hi = std::min(n, lo + 7);
    float dy = H / 2.f - ((hi - lo) * 18 * u + 20 * u) / 2;
    for (int k = lo; k < hi; ++k) {
        bool cur = k == at;
        float hgt = cur ? 28 * u : 8 * u;
        rn.round_rect({272 * u, dy, 8 * u, hgt}, 4 * u, (cur ? kWhite : gfx::Color::rgb(0x48484A)).with_a(a));
        dy += hgt + 10 * u;
    }
}

void App::render_shorts(gfx::Renderer& rn, double pos, double dur, float alpha) {
    using S = i18n::Str;
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, vw = H * 9.f / 16.f, vx = (W - vw) / 2;
    // the watched line along the clip's bottom: always there
    float frac = dur > 0 ? (float)std::min(1.0, std::max(0.0, pos / dur)) : 0.f;
    rn.quad({vx, H - 5 * u, vw, 5 * u}, kWhite.with_a(0.25f));
    rn.quad({vx, H - 5 * u, vw * frac, 5 * u}, gfx::Color::rgb(0xFF0033));
    if (player_.paused()) {
        float d = 120 * u, cx = W / 2.f, cy = H * 0.5f;
        rn.round_rect({cx - d / 2, cy - d / 2, d, d}, d / 2, kBg.with_a(0.5f));
        bos_icon(rn, "pause", cx - 30 * u, cy - 30 * u, 60 * u, kWhite);
    }
    if (alpha <= 0.001f) return;
    shorts_column(rn, now_playing_item_, alpha);
    // the arrows on the left
    const gfx::Font& hf = dz(DZ_SHEET_M);
    auto arrow = [&](const char* key, S label, float y) {
        float d = 40 * u, kw = std::max(d, hf.text_width(key) + 20 * u);
        rn.round_rect({40 * u, y, kw, d}, 12 * u, gfx::Color::rgb(0x73E6F5).with_a(alpha));
        rn.text(hf, key, 40 * u + (kw - hf.text_width(key)) / 2, y + (d - hf.line_height()) / 2, kBg.with_a(alpha));
        std::string t = hf.ellipsize(i18n::tr(label), 250 * u - kw - 10 * u);
        rn.text(hf, t, 40 * u + kw + 10 * u, y + (d - hf.line_height()) / 2, kWhite.with_a(alpha));
    };
    arrow("\xE2\x96\xB2\xE2\x96\xBC", S::HintPrevNext, 40 * u);
    design_hint_bar(rn, {{"A", i18n::tr(S::HintPlayPause)}, {"B", i18n::tr(S::HintBack)},
                         {"Y", i18n::tr(S::HintComments)}}, {});
}

// a Short on its way: the last one slides out, this one in (ease-out 180 ms), its picture until it plays
void App::render_shorts_loading(gfx::Renderer& rn) {
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, vw = H * 9.f / 16.f, vx = (W - vw) / 2;
    rn.begin(W, H);
    rn.clear(kBg);
    float t = 1;
    if (shorts_anim_at_) {
        t = (SDL_GetTicks() - shorts_anim_at_) / 180.f;
        if (t >= 1) { t = 1; }
        t = 1 - (1 - t) * (1 - t) * (1 - t);   // ease-out
    }
    auto pic = [&](const std::string& url, float y) {
        rn.quad({vx, y, vw, (float)H}, gfx::Color::rgb(0x1C1C1F));
        if (url.empty()) return;
        thumbs_.request(url);
        if (auto* tex = thumbs_.get(url)) rn.textured_cover({vx, y, vw, (float)H}, *tex, {1, 1, 1, 1});
    };
    float d = shorts_anim_dir_ < 0 ? -1.f : 1.f;
    if (shorts_anim_at_ && t < 1) pic(shorts_prev_thumb_, -d * H * t);
    pic(now_playing_item_.thumbnail_url, d * H * (1 - t));
    rn.quad({0, 0, vx, (float)H}, kBg);              // the sides stay black over the sliding pictures
    rn.quad({vx + vw, 0, W - vx - vw, (float)H}, kBg);
    if (t >= 1) {
        rn.round_rect({W / 2.f - 50 * u, H / 2.f - 50 * u, 100 * u, 100 * u}, 50 * u, kBg.with_a(0.5f));
        draw_spinner(rn, W / 2.f, H / 2.f, 30 * u, kWhite);
    }
    shorts_column(rn, now_playing_item_, 1.f);
    rn.end();
}

}  // namespace ui
