#!/usr/bin/env python3
"""Brick OS "TV" browse view for YTC (ViewMode::Shelves, YTC_VIEW=4): a Google TV-style
layout for a handheld held close. A left icon rail (Search, Home, Favorite channels,
Watch later, History, Settings) that expands with labels when focused, a search pill on
top, and the results as horizontal rows grouped by kind (Videos, Shorts, Channels,
Playlists, Posts; 12 tiles a row). The focused row slides to the top, the focused tile
gets a white ring. Also larger UI text everywhere (the 4" 1024x768 panel is read at arm's
length, not across a room). Run from the ytc checkout root, after modern_patch.py."""
import sys
from pathlib import Path


def edit(path, pairs):
    p = Path(path)
    s = p.read_text()
    for pair in pairs:
        old, new = pair[0], pair[1]
        want = pair[2] if len(pair) > 2 else 1
        n = s.count(old)
        if n != want:
            sys.exit(f"{path}: expected {want} of {old[:70]!r}, found {n}")
        s = s.replace(old, new)
    p.write_text(s)


if "render_shelves" in Path("src/ui.h").read_text():
    sys.exit("already patched")

edit("src/ui.h", [
    ("""    enum class ViewMode { Grid, Carousel, Carousel3D, Coverflow };""",
     """    enum class ViewMode { Grid, Carousel, Carousel3D, Coverflow, Shelves };"""),
    ("""    void render_grid(gfx::Renderer& rn);""",
     """    void render_grid(gfx::Renderer& rn);
    // TV view (ViewMode::Shelves): icon rail + rows of tiles grouped by kind.
    struct Shelf { std::string title; std::vector<int> idx; };   // idx: into results_
    void rebuild_shelves();
    bool locate_shelf(int sel, int& row, int& col) const;
    int rail_page() const;              // rail item of the page on screen
    void rail_activate();
    void shelves_input(Action a);
    void render_shelves(gfx::Renderer& rn);
    void draw_rail_icon(gfx::Renderer& rn, int item, float cx, float cy, float u, gfx::Color c);"""),
    ("""    std::unique_ptr<gfx::Font> font_title_, font_body_, font_small_;""",
     """    std::unique_ptr<gfx::Font> font_title_, font_body_, font_small_;
    std::unique_ptr<gfx::Font> font_icon_;   // DejaVu symbols for the TV view's rail
    std::vector<Shelf> shelves_;
    size_t shelves_src_n_ = (size_t)-1;      // results_.size() the shelves were built from
    std::vector<float> shelf_off_;           // eased horizontal scroll per row
    std::vector<int> shelf_col_;             // last focused tile per row
    float shelves_scroll_ = 0;               // eased vertical scroll
    bool rail_focus_ = false;
    int rail_sel_ = 1;"""),
])

edit("src/ui.cpp", [
    # larger text
    ("""    font_title_ = gfx::Font::load(ttf, 34 * s, cjk, thai);
    font_body_  = gfx::Font::load(ttf, 22 * s, cjk, thai);
    font_small_ = gfx::Font::load(ttf, 18 * s, cjk, thai);""",
     """    font_title_ = gfx::Font::load(ttf, 40 * s, cjk, thai);
    font_body_  = gfx::Font::load(ttf, 28 * s, cjk, thai);
    font_small_ = gfx::Font::load(ttf, 23 * s, cjk, thai);
    {   // rail symbols (house, heart, clock, arrow, gear) from the bundled DejaVu
        std::string dv = platform::exe_dir() + "/data/DejaVuSans.ttf";
        if (!std::ifstream(dv).good()) dv = "data/DejaVuSans.ttf";
        font_icon_ = gfx::Font::load(dv, 30 * s);
    }"""),
    ("""    m.meta_h = 100 * m.s;                 // title + author + "views · age\"""",
     """    m.meta_h = font_body_->line_height() + 2 * font_small_->line_height() + 18 * m.s;   // title + author + "views · age\""""),
    # duration pill sized from the font
    ("""            float pw = font_small_->text_width(pill) + 12*s;
            gfx::Rect pr{r.x + r.w - pw - 8*s, r.y + r.h - 26*s, pw, 22*s};
            rn.round_rect(pr, 6*s, gfx::Color{0,0,0,0.75f*alpha});
            rn.text(*font_small_, pill, pr.x + 6*s, pr.y + 2*s, A(theme_.text));""",
     """            float pw = font_small_->text_width(pill) + 14*s, ph = font_small_->line_height() + 2*s;
            gfx::Rect pr{r.x + r.w - pw - 8*s, r.y + r.h - ph - 8*s, pw, ph};
            rn.round_rect(pr, 7*s, gfx::Color{0,0,0,0.75f*alpha});
            rn.text(*font_small_, pill, pr.x + 7*s, pr.y + 1*s, A(theme_.text));"""),
    # view selection: YTC_VIEW may pick the TV view; settings cycle includes it
    ("""    const S vname[] = {S::ViewGrid, S::ViewCarousel, S::View3DCarousel, S::ViewCoverflow};
    menu_items_.push_back({tr(S::SetView) + std::string(":  ") + tr(vname[(int)view_mode_]),""",
     """    const S vname[] = {S::ViewGrid, S::ViewCarousel, S::View3DCarousel, S::ViewCoverflow};
    menu_items_.push_back({tr(S::SetView) + std::string(":  ") +
                           (view_mode_ == ViewMode::Shelves ? std::string("TV") : std::string(tr(vname[(int)view_mode_]))),"""),
    ("""        int nv = ((int)view_mode_ + (dir < 0 ? 3 : 1)) % 4;""",
     """        int nv = ((int)view_mode_ + (dir < 0 ? 4 : 1)) % 5;"""),
    ("""    view_mode_ = (ViewMode)(((int)view_mode_ + 1) % 4);   // Grid->Carousel->3D->Coverflow""",
     """    view_mode_ = (ViewMode)(((int)view_mode_ + 1) % 5);   // Grid->Carousel->3D->Coverflow->TV"""),
    # render dispatch
    ("""            case ViewMode::Coverflow:  render_coverflow(rn); break;""",
     """            case ViewMode::Coverflow:  render_coverflow(rn); break;
            case ViewMode::Shelves:    render_shelves(rn); break;"""),
    # input: Back leaves the rail first; the TV view has its own d-pad handling
    ("""    if (a == Action::Back) {
        // Stage 1: B jumps to the top of the list (first tile selected).""",
     """    if (a == Action::Back && rail_focus_) { rail_focus_ = false; return; }
    if (a == Action::Back) {
        // Stage 1: B jumps to the top of the list (first tile selected)."""),
    ("""    // Tabs (channel views AND Home) are switched only with the L/R shoulders
    // (cycle_tab); the d-pad stays entirely in the grid.
    int n = (int)results_.size();""",
     """    if (view_mode_ == ViewMode::Shelves) { shelves_input(a); return; }
    // Tabs (channel views AND Home) are switched only with the L/R shoulders
    // (cycle_tab); the d-pad stays entirely in the grid.
    int n = (int)results_.size();"""),
    # rows rebuild whenever the tile lines are (results_ changed)
    ("""void App::build_tile_lines() {
    tile_lines_.clear();""", """void App::build_tile_lines() {
    shelves_src_n_ = (size_t)-1;   // TV view rows follow results_
    tile_lines_.clear();"""),
    # implementation, before the grid renderer
    ("""void App::render_grid(gfx::Renderer& rn) {""", r'''// ---- TV view (ViewMode::Shelves) ------------------------------------------------
static int shelf_kind(const yt::SearchResult& v) {
    return v.is_channel() ? 2 : v.is_playlist() ? 3 : v.is_post() ? 4 : v.is_short ? 1 : 0;
}
static constexpr int kRailItems = 6;   // Search, Home, Favorites, Watch later, History, Settings

void App::rebuild_shelves() {
    using S = i18n::Str;
    static const S kNames[5] = {S::TabVideos, S::TabShorts, S::FilterChannels, S::TabPlaylists, S::TabPosts};
    static const size_t kRow = 12;
    std::vector<int> groups[5];
    for (int i = 0; i < (int)results_.size(); ++i) groups[shelf_kind(results_[i])].push_back(i);
    shelves_.clear();
    for (int k = 0; k < 5; ++k)
        for (size_t at = 0; at < groups[k].size(); at += kRow) {
            Shelf sh;
            if (at == 0) sh.title = i18n::tr(kNames[k]);
            sh.idx.assign(groups[k].begin() + at,
                          groups[k].begin() + std::min(groups[k].size(), at + kRow));
            shelves_.push_back(std::move(sh));
        }
    shelf_off_.resize(shelves_.size(), 0.f);
    shelf_col_.resize(shelves_.size(), 0);
    shelves_src_n_ = results_.size();
}

bool App::locate_shelf(int sel, int& row, int& col) const {
    for (int r = 0; r < (int)shelves_.size(); ++r)
        for (int c = 0; c < (int)shelves_[r].idx.size(); ++c)
            if (shelves_[r].idx[c] == sel) { row = r; col = c; return true; }
    return false;
}

int App::rail_page() const {
    if (in_channel_view_) return 1;
    if (view_label_ == "Favorite Channels") return 2;
    if (view_label_ == "Watch Later") return 3;
    if (view_label_ == "History") return 4;
    return query_.empty() ? 1 : 0;
}

void App::rail_activate() {
    rail_focus_ = false;
    if (rail_sel_ == 0) { open_search(); return; }
    if (rail_sel_ == 5) { rail_focus_ = true; open_main_menu(); open_settings(); return; }
    in_channel_view_ = false;
    if      (rail_sel_ == 1) load_home();
    else if (rail_sel_ == 2) load_favorites();
    else if (rail_sel_ == 3) load_watch_later();
    else                     load_history();
}

void App::shelves_input(Action a) {
    if (shelves_src_n_ != results_.size()) rebuild_shelves();
    if (rail_focus_) {
        switch (a) {
            case Action::Up:     if (rail_sel_ > 0) rail_sel_--; break;
            case Action::Down:   if (rail_sel_ < kRailItems - 1) rail_sel_++; break;
            case Action::Right:  if (!shelves_.empty()) rail_focus_ = false; break;
            case Action::Select: rail_activate(); break;
            default: break;
        }
        return;
    }
    int r = 0, c = 0;
    if (!locate_shelf(sel_, r, c)) {
        if (a == Action::Left || shelves_.empty()) { rail_focus_ = true; rail_sel_ = rail_page(); }
        else sel_ = shelves_[0].idx[0];
        return;
    }
    switch (a) {
        case Action::Left:
            if (c > 0) c--;
            else { rail_focus_ = true; rail_sel_ = rail_page(); return; }
            break;
        case Action::Right: if (c + 1 < (int)shelves_[r].idx.size()) c++; break;
        case Action::Up:
        case Action::Down: {
            int nr = r + (a == Action::Up ? -1 : 1);
            if (nr < 0 || nr >= (int)shelves_.size()) return;
            shelf_col_[r] = c;   // each row remembers its tile, like a TV launcher
            r = nr;
            c = std::min(shelf_col_[r], (int)shelves_[r].idx.size() - 1);
            break;
        }
        case Action::Select: {
            const yt::SearchResult* v = selected();
            if (v && v->is_channel()) open_channel(*v);
            else if (v && v->is_playlist()) open_playlist(*v);
            else if (v && v->is_post()) open_post(*v);
            else request_playback();
            return;
        }
        default: return;
    }
    shelf_col_[r] = c;
    sel_ = shelves_[r].idx[c];
}

// Rail symbols: a drawn magnifier for Search, DejaVu glyphs for the rest.
void App::draw_rail_icon(gfx::Renderer& rn, int item, float cx, float cy, float u, gfx::Color c) {
    if (item == 0) {
        float r = 10*u, ox = cx - 3*u, oy = cy - 3*u, t = 3*u;
        rn.round_rect({ox - r, oy - r, 2*r, 2*r}, r, c);
        rn.round_rect({ox - r + t, oy - r + t, 2*(r - t), 2*(r - t)}, r - t, theme_.bg);
        float hx = ox + r*0.62f, hy = oy + r*0.62f, L = 8*u, w = 1.6f*u;
        rn.quad4(hx - w, hy + w, hx + w, hy - w, hx + L + w, hy + L - w, hx + L - w, hy + L + w, c);
        return;
    }
    static const char* kGlyph[] = {"", "\xE2\x8C\x82", "\xE2\x99\xA5", "\xE2\x97\xB7",
                                   "\xE2\x86\xBA", "\xE2\x9A\x99"};   // house heart clock arrow gear
    const gfx::Font& f = *font_icon_;
    const char* g = kGlyph[item];
    rn.text(f, g, cx - f.text_width(g) / 2, cy - f.line_height() / 2, c);
}

void App::render_shelves(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f, s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    if (shelves_src_n_ != results_.size()) rebuild_shelves();

    const float rail_w = 88*u, cx0 = rail_w + 12*u, top = 96*u;
    const float cardw = 282*u, gap = 20*u, thumbh = cardw * 9.f / 16.f, rad = 14*u;
    const float title_h = font_body_->line_height() + 14*u;
    const float meta_h = font_body_->line_height() + 2*font_small_->line_height() + 14*u;
    auto row_h = [&](const Shelf& sh) { return (sh.title.empty() ? 6*u : title_h) + thumbh + 12*u + meta_h + 14*u; };

    int fr = 0, fc = 0;
    bool have = locate_shelf(sel_, fr, fc);
    float target = 0;
    for (int r = 0; r < fr && have; ++r) target += row_h(shelves_[r]);
    shelves_scroll_ += (target - shelves_scroll_) * 0.3f;
    if (std::abs(target - shelves_scroll_) < 0.5f) shelves_scroll_ = target;

    // Rows (drawn first; the top bar and the rail cover what scrolls under them).
    if (results_.empty()) browse_empty_overlay(rn);
    float y = top - shelves_scroll_;
    for (int r = 0; r < (int)shelves_.size(); ++r) {
        const Shelf& sh = shelves_[r];
        float rh = row_h(sh);
        if (y > H) break;
        int n = (int)sh.idx.size();
        int col = (have && r == fr) ? fc : std::min(shelf_col_[r], n - 1);
        float content_w = n * (cardw + gap) - gap, view_w = W - cx0 - 20*u;
        float tx = content_w <= view_w ? 0.f : std::min(col * (cardw + gap), content_w - view_w);
        shelf_off_[r] += (tx - shelf_off_[r]) * 0.3f;
        if (std::abs(tx - shelf_off_[r]) < 0.5f) shelf_off_[r] = tx;
        if (y + rh < top - 40*u) { y += rh; continue; }
        float ty = y + (sh.title.empty() ? 6*u : title_h);
        if (!sh.title.empty()) rn.text(*font_body_, sh.title, cx0, y, theme_.text);
        for (int c = 0; c < n; ++c) {
            float x = cx0 + c * (cardw + gap) - shelf_off_[r];
            int i = sh.idx[c];
            const yt::SearchResult& v = results_[i];
            if (x + cardw < -2*cardw || x > W + 2*cardw) continue;
            if (!v.thumbnail_url.empty()) thumbs_.request(v.thumbnail_url);
            else if (v.is_channel()) { std::string av = chan_meta_.avatar(v.channel_id);
                                       if (!av.empty()) thumbs_.request(av); }
            if (x + cardw < 0 || x > W) continue;
            bool foc = have && !rail_focus_ && r == fr && c == fc;
            gfx::Rect tr{x, ty, cardw, thumbh};
            if (foc) rn.round_rect({x - 5*u, ty - 5*u, cardw + 10*u, thumbh + 10*u}, rad + 5*u, theme_.ring);
            draw_thumb(rn, v, tr, s, 1.0f);
            rn.round_corners(tr, rad, foc ? theme_.ring : theme_.bg);
            draw_meta(rn, v, i, x, ty + thumbh + 12*u, cardw, s, foc ? 1.0f : 0.8f);
        }
        y += rh;
    }

    // Top bar: search pill, then the tab chips (Home, search, channel pages).
    rn.quad({0, 0, (float)W, top - 10*u}, theme_.bg);
    float ph = 54*u, py = 22*u, pw = 300*u;
    rn.round_rect({cx0, py, pw, ph}, ph / 2, theme_.chip);
    draw_rail_icon(rn, 0, cx0 + 30*u, py + ph / 2, u, theme_.text_dim);
    std::string q = (!query_.empty() && !in_channel_view_) ? query_ : std::string(i18n::tr(S::Search));
    rn.text(*font_small_, font_small_->ellipsize(q, pw - 74*u), cx0 + 54*u,
            py + (ph - font_small_->line_height()) / 2, theme_.text_dim);
    float chip_x = cx0 + pw + 18*u;
    if (channel_tabs_active() || home_tabs_active() || search_tabs_active()) {
        int active = channel_tabs_active() ? chan_tab_ : search_tabs_active() ? search_tab_ : home_tab_;
        const S kTabs[]       = {S::TabAll, S::TabVideos, S::TabShorts, S::TabPosts, S::TabPlaylists};
        const S kSearchTabs[] = {S::TabAll, S::TabVideos, S::TabShorts, S::FilterChannels, S::TabPlaylists};
        const S* tabs = search_tabs_active() ? kSearchTabs : kTabs;
        float ch = 44*u, cy = py + (ph - ch) / 2;
        for (int i = 0; i < 5; ++i) {
            const char* label = i18n::tr(tabs[i]);
            float cw = font_small_->text_width(label) + 30*u;
            if (chip_x + cw > W - 16*u) break;
            bool st = i == active;
            rn.round_rect({chip_x, cy, cw, ch}, ch / 2, st ? theme_.ring : theme_.chip);
            rn.text(*font_small_, label, chip_x + 15*u, cy + (ch - font_small_->line_height()) / 2,
                    st ? theme_.bg : theme_.text);
            chip_x += cw + 8*u;
        }
    } else {
        std::string page = in_channel_view_ ? query_ : tr_view_label(view_label_);
        if (refresh_running_) page = i18n::tr(S::Loading);
        rn.text(*font_body_, font_body_->ellipsize(page, W - chip_x - 24*u), chip_x,
                py + (ph - font_body_->line_height()) / 2, theme_.text);
    }

    // Rail: icons; focused, it widens over the dimmed rows and shows labels.
    static const S kLabels[kRailItems] = {S::Search, S::Home, S::FavoriteChannels, S::WatchLater,
                                         S::History, S::Settings};
    float rw = rail_w;
    if (rail_focus_) {
        rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.55f));
        for (auto l : kLabels) rw = std::max(rw, 92*u + font_small_->text_width(i18n::tr(l)) + 28*u);
    }
    rn.quad({0, 0, rw, (float)H}, rail_focus_ ? theme_.panel : theme_.bg);
    int page = rail_page();
    float step = 72*u, iy0 = H / 2.f - step * (kRailItems - 1) / 2.f;
    for (int k = 0; k < kRailItems; ++k) {
        float cy = iy0 + k * step, cx = rail_w / 2;
        bool hot = rail_focus_ && k == rail_sel_;
        if (hot) rn.round_rect({10*u, cy - 28*u, rw - 20*u, 56*u}, 28*u, theme_.ring);
        else if (k == page) rn.round_rect({cx - 28*u, cy - 28*u, 56*u, 56*u}, 28*u, theme_.chip);
        gfx::Color ic = hot ? theme_.bg : (k == page ? theme_.text : theme_.text_dim);
        draw_rail_icon(rn, k, cx, cy, u, ic);
        if (rail_focus_)
            rn.text(*font_small_, i18n::tr(kLabels[k]), 92*u, cy - font_small_->line_height() / 2,
                    hot ? theme_.bg : theme_.text);
    }

    draw_status_banner(rn, top, s);
    rn.end();
}

void App::render_grid(gfx::Renderer& rn) {'''),
])
print("patched")
