// Brick OS look for YTC (brick-os DESIGN.md 7.5): the browse views other than TV (grid, carousel, 3D carousel,
// coverflow) share the TV view's top (status bar, search pill, tabs), its cards, its focus ring and its hint bar;
// and the small screens drawn the same way: the resume question, the cast picker, linked devices and the remote.
// Sizes are the 1024x768 mockup's px.
#include "ui.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

std::string humanize_age(const std::string& p);   // ui.cpp
int length_seconds(const std::string& s);          // ui.cpp

namespace {
const gfx::Color kBg{0, 0, 0, 1}, kWhite{1, 1, 1, 1};
const gfx::Color kSurf = gfx::Color::rgb(0x1C1C1F), kSurf2 = gfx::Color::rgb(0x2C2C30),
                 kText2 = gfx::Color::rgb(0xB4B4BA), kSep = gfx::Color::rgb(0x38383A),
                 kDanger = gfx::Color::rgb(0xFF453A);

// view_label_ is an internal English identity key; translate it only for display.
std::string tr_view_label(const std::string& label) {
    using S = i18n::Str;
    if (label == "Favorite Channels")  return i18n::tr(S::FavoriteChannels);
    if (label == "Favorite Playlists") return i18n::tr(S::FavoritePlaylists);
    if (label == "Watch Later")        return i18n::tr(S::WatchLater);
    if (label == "History")            return i18n::tr(S::History);
    if (label == "Downloads")          return i18n::tr(S::Downloads);
    return label;
}
}  // namespace

float App::browse_rows_top() const {
    const float u = win_->height() / 768.f;
    return 64 * u + 4 * u + (in_channel_view_ ? 102 * u : 0.f) + 48 * u;
}

// "author · views · age" (a channel: its subscribers)
std::string App::card_meta(const yt::SearchResult& v) {
    std::string meta;
    auto add = [&](const std::string& p) { if (!p.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + p; };
    if (v.is_channel()) add(v.subs_text);
    else {
        std::string author = v.author;
        if (author.empty() && !v.channel_id.empty()) author = chan_meta_.name(v.channel_id);
        add(author); add(v.view_count_text); add(humanize_age(v.published_text));
    }
    return meta;
}

// The picture of one item in r: the cover (a channel: its avatar), the watched bar, the duration pill, the corners.
void App::design_thumb(gfx::Renderer& rn, const yt::SearchResult& v, const gfx::Rect& tr, float rad, float alpha) {
    const float u = win_->height() / 768.f;
    rn.quad(tr, kSurf.with_a(alpha));
    gfx::Color tint{1, 1, 1, alpha};
    std::string url = v.thumbnail_url;
    if (url.empty() && v.is_channel()) url = chan_meta_.avatar(v.channel_id);
    if (!url.empty()) thumbs_.request(url);
    if (auto* tex = url.empty() ? nullptr : thumbs_.get(url)) {
        if (v.is_channel()) {
            float av = tr.h * 0.82f;
            gfx::Rect ar{tr.x + (tr.w - av) / 2, tr.y + (tr.h - av) / 2, av, av};
            rn.textured_cover(ar, *tex, tint);
            rn.round_corners(ar, av / 2, kSurf.with_a(alpha));
        } else rn.textured_cover(tr, *tex, tint);
    } else if (v.is_post()) {
        const gfx::Font& mf = dz(DZ_META);
        rn.text(mf, mf.ellipsize(v.title.substr(0, v.title.find('\n')), tr.w - 24 * u), tr.x + 12 * u, tr.y + 12 * u,
                kText2.with_a(alpha));
    }
    // watched: a 5 high track (white 25 %) along the bottom, red (youtube) as far as the saved position
    int len = length_seconds(v.length_text);
    if (len > 0 && !v.video_id.empty()) {
        double pos = it_.resume_pos(v.video_id);
        if (pos > 0) {
            rn.quad({tr.x, tr.y + tr.h - 5 * u, tr.w, 5 * u}, kWhite.with_a(0.25f * alpha));
            rn.quad({tr.x, tr.y + tr.h - 5 * u, (float)(tr.w * std::min(1.0, pos / len)), 5 * u},
                    gfx::Color::rgb(0xFF0033).with_a(alpha));
        }
    }
    std::string pill = v.is_short ? std::string() : v.is_playlist() ? v.view_count_text : v.length_text;
    if (!pill.empty() && tr.w > 120 * u) {   // duration: 18/600 on black 80 %, radius 8, padding 2 8, 10 from the corner
        const gfx::Font& pf = dz(DZ_PILL);
        float pw = pf.text_width(pill) + 16 * u, ph = pf.line_height() + 4 * u;
        gfx::Rect pr{tr.x + tr.w - 10 * u - pw, tr.y + tr.h - 10 * u - ph, pw, ph};
        rn.round_rect(pr, 8 * u, kBg.with_a(0.8f * alpha));
        rn.text(pf, pill, pr.x + 8 * u, pr.y + 2 * u, kWhite.with_a(alpha));
    }
    rn.round_corners(tr, rad, kBg);
}

// One card (the TV view's): the picture cardw x thumbh (focused: x1.04 with the ring), the title 22, the meta 18.
void App::design_card(gfx::Renderer& rn, int i, float x, float ty, float cardw, float thumbh, bool foc) {
    const float u = win_->height() / 768.f, rad = 18 * u;
    const yt::SearchResult& v = results_[i];
    gfx::Rect tr = foc ? gfx::Rect{x - cardw * 0.02f, ty - thumbh * 0.02f, cardw * 1.04f, thumbh * 1.04f}
                       : gfx::Rect{x, ty, cardw, thumbh};
    design_thumb(rn, v, tr, rad, 1.f);
    if (foc) design_ring(rn, tr, rad);
    const gfx::Font &cf = dz(DZ_CARD), &tf = dz(foc ? DZ_CARD_FOC : DZ_CARD), &mf = dz(DZ_META);
    std::string title = v.is_post() ? v.title.substr(0, v.title.find('\n')) : v.title;
    float ly = ty + thumbh + 12 * u;
    rn.text(tf, tf.ellipsize(title, cardw), x, ly, kWhite);
    rn.text(mf, mf.ellipsize(card_meta(v), cardw), x, ly + cf.line_height() + 2 * u, kText2);
}

// The top of every browse view: the status bar, a channel's header, the search pill, the tab chips (or the page's
// name) and the quality badge. cx0: where the content starts (the TV view leaves room for its rail).
void App::design_browse_top(gfx::Renderer& rn, float cx0) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bar = 64 * u, head_h = in_channel_view_ ? 102 * u : 0.f;
    const float top_y = bar + 4 * u + head_h, pill_h = 48 * u, rows_top = top_y + pill_h;
    const gfx::Font& rf = dz(DZ_ROW);
    rn.quad({0, 0, (float)W, rows_top + 4 * u}, kBg);   // + 4: the marks of the row scrolled above
    design_status_bar(rn);
    if (in_channel_view_) {   // avatar 96, name 44/700, "@handle · subscribers · videos" 24, the follow pill
        const float ay = bar + 4 * u, a = 96 * u;
        std::string cid = channel_info_.channel_id;
        std::string av = channel_info_.avatar_url.empty() ? chan_meta_.avatar(cid) : channel_info_.avatar_url;
        rn.round_rect({cx0, ay, a, a}, a / 2, bos_.primary);
        gfx::Texture* at = av.empty() ? nullptr : (thumbs_.request(av), thumbs_.get(av));
        if (at) { rn.textured_cover({cx0, ay, a, a}, *at, kWhite); rn.round_corners({cx0, ay, a, a}, a / 2, kBg); }
        else {
            std::string nm = channel_info_.name.empty() ? query_ : channel_info_.name;
            size_t e = nm.empty() ? 0 : 1;
            while (e < nm.size() && ((unsigned char)nm[e] & 0xC0) == 0x80) e++;
            const gfx::Font& lf = dz(DZ_CH_NAME);
            rn.text(lf, nm.substr(0, e), cx0 + (a - lf.text_width(nm.substr(0, e))) / 2,
                    ay + (a - lf.line_height()) / 2, kWhite);
        }
        bool fav = !cid.empty() && fav_ids_.count(cid);
        const gfx::Font& pf = dz(DZ_CM_NAME);
        std::string pl = i18n::tr(fav ? S::Subscribed : S::Subscribe);
        float pw = pf.text_width(pl) + (fav ? 24 * u + 10 * u : 0) + 48 * u, ph = 52 * u;
        gfx::Rect pr{W - 32 * u - pw, ay + 22 * u, pw, ph};
        rn.round_rect(pr, ph / 2, fav ? kSurf2 : kWhite);
        float px = pr.x + 24 * u;
        if (fav) { bos_icon(rn, "checkc", px, pr.y + (ph - 24 * u) / 2, 24 * u, gfx::Color::rgb(0x30D158)); px += 34 * u; }
        rn.text(pf, pl, px, pr.y + (ph - pf.line_height()) / 2, fav ? kWhite : kBg);
        const gfx::Font &nf = dz(DZ_CH_NAME), &hf = dz(DZ_SHEET_M);
        float nx = cx0 + a + 22 * u, nw = pr.x - 20 * u - nx;
        std::string name = channel_info_.name.empty() ? query_ : channel_info_.name;
        rn.text(nf, nf.ellipsize(name, nw), nx, ay + 4 * u, kWhite);
        std::string line;
        for (const std::string& p : {channel_info_.handle, channel_info_.subs_text, channel_info_.video_count_text})
            if (!p.empty()) line += (line.empty() ? "" : " \xC2\xB7 ") + p;
        rn.text(hf, hf.ellipsize(line, nw), nx, ay + 4 * u + nf.line_height(), kText2);
    }
    rn.round_rect({cx0, top_y, 220 * u, pill_h}, 24 * u, kSurf);
    bos_icon(rn, "search", cx0 + 18 * u, top_y + (pill_h - 24 * u) / 2, 24 * u, kText2);
    std::string q = (!query_.empty() && !in_channel_view_) ? query_ : std::string(i18n::tr(S::Search));
    const gfx::Font& sf = dz(DZ_SEARCH);
    rn.text(sf, sf.ellipsize(q, 220 * u - 70 * u), cx0 + 52 * u, top_y + (pill_h - sf.line_height()) / 2, kText2);
    float chip_x = cx0 + 220 * u + 12 * u, right = W - 32 * u;
    {   // quality badge: 20/600 text2, 1px #38383A, radius 10, padding 4 10
        int mh = effective_max_height();
        std::string qb = mh > 0 ? std::to_string(mh) + "p" : std::string("Auto");
        const gfx::Font& bf = dz(DZ_BADGE);
        float bw = bf.text_width(qb) + 20 * u, bh = bf.line_height() + 8 * u;
        gfx::Rect br{right - bw, top_y + (pill_h - bh) / 2, bw, bh};
        rn.round_rect(br, 10 * u, kSep);
        rn.round_rect({br.x + u, br.y + u, br.w - 2 * u, br.h - 2 * u}, 9 * u, kBg);
        rn.text(bf, qb, br.x + 10 * u, br.y + 4 * u, kText2);
        right = br.x - 12 * u;
    }
    if (channel_tabs_active() || home_tabs_active() || search_tabs_active()) {
        int active = channel_tabs_active() ? chan_tab_ : search_tabs_active() ? search_tab_ : home_tab_;
        const S kTabs[]       = {S::TabAll, S::TabVideos, S::TabShorts, S::TabPosts, S::TabPlaylists};
        const S kSearchTabs[] = {S::TabAll, S::TabVideos, S::TabShorts, S::FilterChannels, S::TabPlaylists};
        const S* tabs = search_tabs_active() ? kSearchTabs : kTabs;
        float ch = 48 * u, cy = top_y, words = 0;
        for (int i = 0; i < 5; ++i) words += dz(i == active ? DZ_CHIP_SEL : DZ_CHIP).text_width(i18n::tr(tabs[i]));
        // padding 22 as drawn; less when the five do not fit, down to 12
        float padx = std::max(12 * u, std::min(22 * u, (right - chip_x - words - 4 * 10 * u) / 10));
        for (int i = 0; i < 5; ++i) {
            bool st = i == active;
            const gfx::Font& f = dz(st ? DZ_CHIP_SEL : DZ_CHIP);
            const char* label = i18n::tr(tabs[i]);
            float cw = f.text_width(label) + 2 * padx;
            if (chip_x + cw > right) break;
            rn.round_rect({chip_x, cy, cw, ch}, 24 * u, st ? kWhite : kSurf);
            rn.text(f, label, chip_x + padx, cy + (ch - f.line_height()) / 2, st ? kBg : kText2);
            chip_x += cw + 10 * u;
        }
    } else {
        std::string page = in_channel_view_ ? query_ : tr_view_label(view_label_);
        if (refresh_running_) page = i18n::tr(S::Loading);
        rn.text(rf, rf.ellipsize(page, right - chip_x), chip_x, top_y + (pill_h - rf.line_height()) / 2, kWhite);
    }
}

void App::design_browse_hints(gfx::Renderer& rn) {
    using S = i18n::Str;
    if (results_.empty()) {
        design_hint_bar(rn, {{"B", i18n::tr(S::HintBack)}, {"Y", i18n::tr(S::HintSearch)}},
                        {{"START", i18n::tr(S::HintMenu)}});
    } else if (in_channel_view_) {
        bool fav = !channel_info_.channel_id.empty() && fav_ids_.count(channel_info_.channel_id);
        design_hint_bar(rn, {{"A", i18n::tr(S::HintPlay)}, {"B", i18n::tr(S::HintBack)},
                             {"X", i18n::tr(fav ? S::Unfollow : S::Subscribe)}},
                        {{"L1 R1", i18n::tr(S::HintTab)}});
    } else {
        design_hint_bar(rn, {{"A", i18n::tr(S::HintPlay)}, {"B", i18n::tr(S::HintBack)},
                             {"X", i18n::tr(S::HintFollow)}, {"Y", i18n::tr(S::HintSearch)}},
                        {{"SELECT", i18n::tr(S::HintOptions)}});
    }
}

// Nothing to show: one line 36/700 white and up to two 26 text2, centred in the content area.
bool App::browse_empty_overlay(gfx::Renderer& rn) {
    if (!results_.empty()) return false;
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    const char* l1; const char* l2 = ""; const char* l3 = "";
    if (retry_pending_ || (refresh_running_ && retry_attempt_ > 0)) {
        l1 = i18n::tr(S::WaitingNetwork); l2 = i18n::tr(S::Reconnecting);
    } else if (refresh_running_) {
        l1 = i18n::tr(S::Loading);
    } else if (channel_tabs_active() || (home_tabs_active() && !home_items_.empty()) ||
               (search_tabs_active() && !search_base_.empty())) {
        l1 = i18n::tr(S::NothingInTab); l2 = i18n::tr(S::SwitchTabsHint);
    } else if (view_label_ == "Favorite Channels") {
        l1 = i18n::tr(S::NoFavorites); l2 = i18n::tr(S::FavHint2); l3 = i18n::tr(S::FavHint3);
    } else if (view_label_ == "Favorite Playlists") {
        l1 = i18n::tr(S::NoFavPlaylists); l2 = i18n::tr(S::FavPlHint2);
    } else if (view_label_ == "Watch Later") {
        l1 = i18n::tr(S::WatchLaterEmpty); l2 = i18n::tr(S::WlHint2); l3 = i18n::tr(S::WlHint3);
    } else if (view_label_ == "History") {
        l1 = i18n::tr(S::NoHistory); l2 = i18n::tr(S::HistHint2); l3 = i18n::tr(S::HistHint3);
    } else {
        l1 = query_.empty() ? i18n::tr(S::NoVideosYet) : i18n::tr(S::NoResults);
        l2 = i18n::tr(S::SearchHint);
        l3 = query_.empty() ? i18n::tr(S::HomeHint3) : i18n::tr(S::SearchHint3);
    }
    const gfx::Font &tf = dz(DZ_PANEL_T), &bf = dz(DZ_DESC);
    float top = browse_rows_top(), bottom = H - 64 * u;
    float bh = tf.line_height() + (l2[0] ? 12 * u + bf.line_height() : 0) + (l3[0] ? 4 * u + bf.line_height() : 0);
    float y = top + (bottom - top - bh) / 2;
    std::string s1 = tf.ellipsize(l1, W - 80 * u);
    rn.text(tf, s1, (W - tf.text_width(s1)) / 2, y, kWhite);
    y += tf.line_height() + 12 * u;
    for (const char* l : {l2, l3}) {
        if (!l[0]) continue;
        std::string s = bf.ellipsize(l, W - 80 * u);
        rn.text(bf, s, (W - bf.text_width(s)) / 2, y, kText2);
        y += bf.line_height() + 4 * u;
    }
    return true;
}

App::GridMetrics App::grid_metrics() const {
    GridMetrics m;
    const float u = win_->height() / 768.f;
    m.s = u;
    m.hbar = browse_rows_top();
    m.pad = 32 * u;
    m.gutter = 22 * u;
    m.cardw = (win_->width() - m.pad * 2 - m.gutter * (cols_ - 1)) / cols_;
    m.thumbh = m.cardw * 9.f / 16.f;
    m.meta_h = 12 * u + dz(DZ_CARD).line_height() + 2 * u + dz(DZ_META).line_height();
    m.cardh = m.thumbh + m.meta_h;
    m.rowstep = m.cardh + 28 * u;
    m.tabs_h = 0;
    m.top = m.hbar + 20 * u;
    m.fh = 64 * u;
    return m;
}

void App::render_grid(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(kBg);
    if (!browse_empty_overlay(rn)) {
        GridMetrics m = grid_metrics();
        int n = (int)results_.size();
        int first_row = std::max(0, (int)((scroll_ - m.top) / m.rowstep) - 3);
        int last_row = (int)((scroll_ - m.top + H) / m.rowstep) + 3;
        for (int i = first_row * cols_; i < std::min(n, (last_row + 1) * cols_); ++i) {
            float x = m.pad + (i % cols_) * (m.cardw + m.gutter);
            float y = m.top + (i / cols_) * m.rowstep - scroll_;
            if (y + m.cardh > -2 * m.rowstep && y < H + 2 * m.rowstep) {   // fetch two rows around the screen
                const yt::SearchResult& pv = results_[i];
                if (!pv.thumbnail_url.empty()) thumbs_.request(pv.thumbnail_url);
                else if (pv.is_channel()) { std::string a = chan_meta_.avatar(pv.channel_id);
                                            if (!a.empty()) thumbs_.request(a); }
            }
            if (y + m.cardh < m.hbar - 20 * u || y > H) continue;
            design_card(rn, i, x, y, m.cardw, m.thumbh, i == sel_ && !tab_focus_);
        }
    }
    design_browse_top(rn, 32 * u);
    design_browse_hints(rn);
    design_toast(rn);
    rn.end();
}

// Under the centred picture of the strips: the title 30/700, the meta 24 text2, "n / N" 18 text2.
void App::design_center_meta(gfx::Renderer& rn, float y) {
    const int W = win_->width();
    const float u = win_->height() / 768.f;
    const yt::SearchResult& v = results_[sel_];
    const gfx::Font &tf = dz(DZ_ROW), &mf = dz(DZ_SHEET_M), &pf = dz(DZ_META);
    std::string t = tf.ellipsize(v.is_post() ? v.title.substr(0, v.title.find('\n')) : v.title, W * 0.8f);
    rn.text(tf, t, (W - tf.text_width(t)) / 2, y, kWhite);
    y += tf.line_height() + 4 * u;
    std::string m = mf.ellipsize(card_meta(v), W * 0.8f);
    rn.text(mf, m, (W - mf.text_width(m)) / 2, y, kText2);
    y += mf.line_height() + 10 * u;
    std::string pos = std::to_string(sel_ + 1) + " / " + std::to_string(results_.size());
    rn.text(pf, pos, (W - pf.text_width(pos)) / 2, y, kText2);
}

// The strips' visible items, farthest first (the near ones draw over them).
void App::strip_visible(float reach) {
    int n = (int)results_.size();
    vis_.clear();
    int lo = std::max(0, (int)std::floor(carousel_pos_ - reach));
    int hi = std::min(n - 1, (int)std::ceil(carousel_pos_ + reach));
    for (int i = lo; i <= hi; ++i)
        if (std::abs(i - carousel_pos_) <= reach) vis_.push_back(i);
    std::sort(vis_.begin(), vis_.end(), [&](int a, int b) {
        return std::abs(a - carousel_pos_) > std::abs(b - carousel_pos_); });
    for (int i = std::max(0, sel_ - 5); i < std::min(n, sel_ + 6); ++i)
        if (!results_[i].thumbnail_url.empty()) thumbs_.request(results_[i].thumbnail_url);
}

// Carousel (§5.3): the focused picture big in the middle with the ring, the others smaller and dimmer beside it,
// apart (no overlap), so every one keeps its rounded corners.
void App::render_carousel(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(kBg);
    if (!browse_empty_overlay(rn)) {
        update_carousel_anim();
        float top = browse_rows_top();
        float bw = W * 0.44f, bh = bw * 9.f / 16.f, cx = W / 2.f, cy = top + 40 * u + bh / 2;
        float step = bw * 0.91f;
        strip_visible(2.6f);
        for (int i : vis_) {
            float d = i - carousel_pos_, dist = std::abs(d);
            float sc = std::max(0.5f, 1.f - dist * 0.3f), a = std::max(0.35f, 1.f - dist * 0.45f);
            float w = bw * sc, h = bh * sc;
            gfx::Rect r{cx + d * step - w / 2, cy - h / 2, w, h};
            bool foc = i == sel_ && dist < 0.5f && !tab_focus_;
            design_thumb(rn, results_[i], r, 18 * u * sc, a);
            if (foc) design_ring(rn, r, 18 * u);
        }
        design_center_meta(rn, cy + bh / 2 + 32 * u);
    }
    design_browse_top(rn, 32 * u);
    design_browse_hints(rn);
    design_toast(rn);
    rn.end();
}

// 3D carousel: the centre picture flat and large with the ring, the side ones turned inward and receding.
void App::render_carousel3d(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(kBg);
    if (!browse_empty_overlay(rn)) {
        update_carousel_anim();
        float top = browse_rows_top();
        float cw = W * 0.42f, chh = cw * 9.f / 16.f, cx = W / 2.f, cy = top + 40 * u + chh / 2;
        float spacing = cw * 0.56f;
        strip_visible(4.5f);
        const float kMaxAng = 1.05f, kPersp = 0.32f;
        for (int i : vis_) {
            float d = i - carousel_pos_, dist = std::abs(d);
            float sc = std::max(0.62f, 1.f - dist * 0.10f);
            float hw = cw * 0.5f * sc, hh = chh * 0.5f * sc, x = cx + d * spacing;
            float alpha = std::max(0.25f, 1.f - dist * 0.24f);
            if (dist < 0.12f) {
                gfx::Rect r{x - hw, cy - hh, hw * 2, hh * 2};
                design_thumb(rn, results_[i], r, 18 * u, 1.f);
                if (!tab_focus_) design_ring(rn, r, 18 * u);
                continue;
            }
            float ang = std::max(-kMaxAng, std::min(kMaxAng, d)) * 0.9f;
            float cosA = std::cos(ang), sinA = std::sin(ang);
            float lX = x - hw * cosA, rX = x + hw * cosA, lY = hh * (1.f + kPersp * sinA), rY = hh * (1.f - kPersp * sinA);
            const auto& v = results_[i];
            gfx::Texture* tex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url);
            if (tex) rn.textured_quad4(lX, cy - lY, rX, cy - rY, rX, cy + rY, lX, cy + lY, *tex, gfx::Color{1, 1, 1, alpha});
            else rn.quad4(lX, cy - lY, rX, cy - rY, rX, cy + rY, lX, cy + lY, kSurf.with_a(alpha));
        }
        design_center_meta(rn, cy + chh / 2 + 32 * u);
    }
    design_browse_top(rn, 32 * u);
    design_browse_hints(rn);
    design_toast(rn);
    rn.end();
}

// Coverflow: side pictures parked at a fixed inward tilt, stacked close; the moving one folds flat at the centre.
void App::render_coverflow(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(kBg);
    if (!browse_empty_overlay(rn)) {
        update_carousel_anim();
        float top = browse_rows_top();
        float cw = W * 0.42f, chh = cw * 9.f / 16.f, cx = W / 2.f, cy = top + 40 * u + chh / 2;
        const float kTilt = 1.15f;
        float parkX = cw * 0.62f, stackX = cw * 0.28f;
        strip_visible(6.5f);
        for (int i : vis_) {
            float d = i - carousel_pos_, ad = std::abs(d), sgn = d < 0 ? -1.f : 1.f;
            float slide = std::min(ad, 1.f), extra = std::max(0.f, ad - 1.f);
            float ang = kTilt * (d < 0 ? -slide : slide);
            float scale = 1.f - 0.12f * slide - 0.03f * std::min(extra, 4.f);
            float hw = cw * 0.5f * scale, hh = chh * 0.5f * scale;
            float x = cx + sgn * (parkX * slide + stackX * extra);
            float alpha = std::max(0.28f, 1.f - 0.10f * slide - 0.18f * std::min(extra, 4.f));
            if (ad < 0.10f) {
                gfx::Rect r{x - hw, cy - hh, hw * 2, hh * 2};
                design_thumb(rn, results_[i], r, 18 * u, 1.f);
                if (!tab_focus_) design_ring(rn, r, 18 * u);
                continue;
            }
            float cosA = std::cos(ang), sinA = std::sin(ang), persp = 0.35f;
            float lX = x - hw * cosA, rX = x + hw * cosA, lY = hh * (1.f - persp * sinA), rY = hh * (1.f + persp * sinA);
            const auto& v = results_[i];
            gfx::Texture* tex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url);
            if (tex) rn.textured_quad4(lX, cy - lY, rX, cy - rY, rX, cy + rY, lX, cy + lY, *tex, gfx::Color{1, 1, 1, alpha});
            else rn.quad4(lX, cy - lY, rX, cy - rY, rX, cy + rY, lX, cy + lY, kSurf.with_a(alpha));
        }
        design_center_meta(rn, cy + chh / 2 + 32 * u);
    }
    design_browse_top(rn, 32 * u);
    design_browse_hints(rn);
    design_toast(rn);
    rn.end();
}

// A §5.7 sheet of plain rows over the page (60 % black behind): title 34/700, a note 24 text2, rows 88 (40/500),
// the focused row on the focus colour; a danger row's words in red.
void App::design_sheet(gfx::Renderer& rn, const std::string& heading, const std::string& note,
                       const std::vector<SheetRow>& rows, int sel, const Hints& right, const Hints& left) {
    bos_refresh();
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, bottom = H - 64 * u, rh = 88 * u, padx = 40 * u;
    rn.quad({0, 64 * u, (float)W, bottom - 64 * u}, kBg.with_a(0.6f));
    const gfx::Font &hf = dz(DZ_SHEET_T), &nf = dz(DZ_SHEET_M);
    int n = (int)rows.size();
    float head = 24 * u + hf.line_height() + (note.empty() ? 0 : nf.line_height()) + 16 * u;
    float max_list = bottom - 104 * u - head - 28 * u;
    int vis = std::max(1, std::min(n, (int)(max_list / rh)));
    float sh = head + vis * rh + 28 * u, top = bottom - sh;
    rn.round_rect({0, top, (float)W, sh + 40 * u}, 32 * u, kSurf);
    rn.quad({0, bottom, (float)W, (float)H - bottom}, kBg);
    rn.text(hf, hf.ellipsize(heading, W - 2 * padx), padx, top + 24 * u, kWhite);
    if (!note.empty()) rn.text(nf, nf.ellipsize(note, W - 2 * padx), padx, top + 24 * u + hf.line_height(), kText2);
    int first = n > vis ? std::min(std::max(0, sel - vis / 2), n - vis) : 0;
    float y = top + head;
    for (int k = 0; k < vis && first + k < n; ++k) {
        int i = first + k;
        gfx::Rect r{padx - 8 * u, y + k * rh, W - 2 * padx + 16 * u, rh};
        bool foc = i == sel;
        if (foc) {
            float dw = r.w * 0.02f, dh = r.h * 0.02f;
            rn.round_rect({r.x - dw, r.y - dh, r.w + 2 * dw, r.h + 2 * dh}, 22 * u, bos_.focus);
        } else if (k + 1 < vis && i + 1 < n) rn.quad({r.x + 28 * u, r.y + r.h - 1, r.w - 28 * u, std::max(1.f, u)}, kSep);
        gfx::Color tc = foc ? bos_.onfocus : rows[i].danger ? kDanger : kWhite;
        const gfx::Font &lf = dz(foc ? DZ_SET_LABEL_F : DZ_SET_LABEL), &vf = dz(DZ_SET_VALUE);
        float lx = r.x + 28 * u, rx = r.x + r.w - 24 * u, cy = r.y + r.h / 2;
        if (rows[i].icon) {
            bos_icon(rn, rows[i].icon, lx, cy - 16 * u, 32 * u, tc);
            lx += 48 * u;
        }
        if (!rows[i].value.empty()) {
            std::string v = vf.ellipsize(rows[i].value, r.w * 0.4f);
            rn.text(vf, v, rx - vf.text_width(v), cy - vf.line_height() / 2, foc ? bos_.onfocus : kText2);
            rx -= vf.text_width(v) + 20 * u;
        }
        rn.text(lf, lf.ellipsize(rows[i].label, rx - lx), lx, cy - lf.line_height() / 2, tc);
    }
    design_hint_bar(rn, right, left);
}

// Opening a video: the page dims to 60 % black between the bars; the spinner in the focus colour, "Loading" 26
// white and the title 24 text2 under it.
void App::render_loading(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.quad({0, 64 * u, (float)W, (float)H - 128 * u}, kBg.with_a(0.6f));
    float cy = H / 2.f - 30 * u;
    draw_spinner(rn, W / 2.f, cy, 34 * u, bos_.focus);
    const gfx::Font &tf = dz(DZ_TOAST), &mf = dz(DZ_SHEET_M);
    std::string msg = i18n::tr(i18n::Str::Loading);
    float y = cy + 56 * u;
    rn.text(tf, msg, (W - tf.text_width(msg)) / 2, y, kWhite);
    std::string t = mf.ellipsize(loading_title_, W * 0.7f);
    rn.text(mf, t, (W - mf.text_width(t)) / 2, y + tf.line_height() + 4 * u, kText2);
    rn.end();
}

// ---------- the resume question ----------
void App::render_resume_prompt(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    rn.begin(W, H);
    int m = (int)resume_prompt_pos_ / 60, sec = (int)resume_prompt_pos_ % 60;
    char tb[16];
    std::snprintf(tb, sizeof tb, "%d:%02d", m, sec);
    design_sheet(rn, resume_prompt_item_.title, "",
                 {{std::string(i18n::tr(S::ResumeFrom)) + " " + tb, "", false, "play"},
                  {i18n::tr(S::StartOver), "", false, "repeat"}},
                 resume_prompt_sel_, {{"A", i18n::tr(S::HintSelect)}, {"B", i18n::tr(S::HintClose)}});
    rn.end();
}

// B on Home: leave YouTube? (the first row leaves, red; B or the second row stays)
void App::render_quit_prompt(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    rn.begin(W, H);
    design_sheet(rn, i18n::tr(S::QuitAsk), "",
                 {{i18n::tr(S::QuitLeave), "", true, "power"}, {i18n::tr(S::QuitStay), "", false, "play"}},
                 quit_prompt_sel_, {{"A", i18n::tr(S::HintSelect)}, {"B", i18n::tr(S::HintClose)}});
    rn.end();
}

// ---------- cast: the device picker, linked devices, the remote ----------
void App::render_cast_picker(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    bool link = cast_picker_mode_ == PickerMode::Link;
    rn.begin(W, H);
    std::vector<SheetRow> rows;
    for (const auto& d : cast_devices_)
        rows.push_back({d.name, (!link && d.kind == yt::Cast::Kind::CastDevice && d.screen_id.empty()) ? "Chromecast" : "",
                        false, "tv"});
    if (!link) rows.push_back({i18n::tr(S::CastAddDevice), "", false, "plus"});
    const char* note = cast_disc_running_ ? i18n::tr(S::CastSearching)
                     : cast_play_running_ ? i18n::tr(S::CastConnecting)
                     : cast_devices_.empty() ? i18n::tr(S::CastNoDevices) : "";
    design_sheet(rn, i18n::tr(link ? S::CastAddDevice : S::MenuCastToDevice), note, rows, cast_sel_,
                 {{"A", i18n::tr(S::HintSelect)}, {"B", i18n::tr(S::HintClose)}});
    rn.end();
}

void App::render_manage_devices(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    rn.begin(W, H);
    bool confirm = cast_confirm_remove_ && cast_manage_sel_ >= 0 && cast_manage_sel_ < (int)cast_paired_.size();
    if (confirm) {   // "Remove device?" No / Yes, Yes in red
        design_sheet(rn, i18n::tr(S::CastRemoveConfirm), cast_paired_[cast_manage_sel_].name,
                     {{i18n::tr(S::No), "", false, nullptr}, {i18n::tr(S::Yes), "", true, "trash"}}, cast_confirm_sel_,
                     {{"A", i18n::tr(S::HintSelect)}, {"B", i18n::tr(S::HintBack)}});
    } else {
        std::vector<SheetRow> rows;
        for (const auto& d : cast_paired_) rows.push_back({d.name, "", false, "tv"});
        rows.push_back({i18n::tr(S::CastAddDevice), "", false, "plus"});
        bool add = cast_manage_sel_ == (int)cast_paired_.size();
        design_sheet(rn, i18n::tr(S::SetLinkedDevices), cast_paired_.empty() ? i18n::tr(S::CastNoLinked) : "", rows,
                     cast_manage_sel_,
                     {{"A", i18n::tr(add ? S::HintSelect : S::HintRemove)}, {"B", i18n::tr(S::HintBack)}});
    }
    rn.end();
}

// The remote: "Casting to <TV>" 24 text2, the title 36/700, a 128 primary circle with play / pause, the time,
// the volume pill; the hint bar.
void App::render_remote(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(kBg);
    design_status_bar(rn);
    const gfx::Font &mf = dz(DZ_SHEET_M), &tf = dz(DZ_PANEL_T), &vf = dz(DZ_SET_VALUE), &pf = dz(DZ_TOAST);
    float y = 150 * u;
    std::string to = mf.ellipsize(std::string(i18n::tr(S::CastingTo)) + "  " + cast_name_, W - 80 * u);
    bos_icon(rn, "cast", (W - mf.text_width(to)) / 2 - 40 * u, y + (mf.line_height() - 28 * u) / 2, 28 * u, kText2);
    rn.text(mf, to, (W - mf.text_width(to)) / 2, y, kText2);
    y += mf.line_height() + 14 * u;
    std::string t = tf.ellipsize(cast_target_title_, W - 120 * u);
    rn.text(tf, t, (W - tf.text_width(t)) / 2, y, kWhite);
    y += tf.line_height() + 40 * u;
    float d = 128 * u, cx = W / 2.f;
    rn.round_rect({cx - d / 2, y, d, d}, d / 2, bos_.primary);
    bos_icon(rn, cast_paused_ ? "play" : "pause", cx - 32 * u, y + d / 2 - 32 * u, 64 * u, bos_.onprimary);
    y += d + 28 * u;
    int p = (int)cast_est_pos();
    char tb[16];
    std::snprintf(tb, sizeof tb, "%d:%02d", p / 60, p % 60);
    rn.text(vf, tb, (W - vf.text_width(tb)) / 2, y, kWhite);
    y += vf.line_height() + 16 * u;
    std::string vol = std::to_string(cast_vol_) + "%";
    float pw = 32 * u + 12 * u + pf.text_width(vol) + 48 * u, ph = 52 * u;
    gfx::Rect pr{(W - pw) / 2, y, pw, ph};
    rn.round_rect(pr, ph / 2, kSurf2);
    bos_icon(rn, cast_vol_ > 0 ? "sound" : "mute", pr.x + 24 * u, pr.y + (ph - 32 * u) / 2, 32 * u, kWhite);
    rn.text(pf, vol, pr.x + 24 * u + 32 * u + 12 * u, pr.y + (ph - pf.line_height()) / 2, kWhite);
    design_hint_bar(rn, {{"A", i18n::tr(S::HintPlayPause)}, {"B", i18n::tr(S::HintStopCast)}},
                    {{"\xE2\x86\x90\xE2\x86\x92", i18n::tr(S::HintSeek)}, {"\xE2\x86\x91\xE2\x86\x93", i18n::tr(S::HintVolume)}});
    design_toast(rn);
    rn.end();
}

}  // namespace ui
