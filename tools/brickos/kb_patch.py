#!/usr/bin/env python3
# Brick OS YTC: bigger keyboard + full gamepad typing + search suggestions; bigger UI;
# no rendering while the screen is off (power-tap listening).
# Run in ~/ytc after tv_patch.py.
import re, sys, json, pathlib

def sub(path, old, new, count=1):
    p = pathlib.Path(path); t = p.read_text()
    n = t.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} of {old[:70]!r}, found {n}")
    p.write_text(t.replace(old, new))

# ---------------- ui.h ----------------
sub("src/ui.h", "    void backspace();\n",
"""    void backspace();
    // Brick OS: gamepad typing on the on-screen keyboard (B delete, Y space, X clear,
    // L Thai/English, R shift, Start search, Select close). False = not handled here.
    bool kb_pad(int sdl_button);
    void kb_trigger(bool right, int value);   // L2/R2 move the caret
    bool screen_dark() const { return screen_dark_; }
""")
sub("src/ui.h", "    bool kb_thai_ = false;     // Brick OS: Thai Kedmanee layout instead of QWERTY\n",
"""    bool kb_thai_ = false;     // Brick OS: Thai Kedmanee layout instead of QWERTY
    // Brick OS: YouTube autocomplete shown as chips above the keyboard (Up from the
    // top row focuses them, A searches one). Fetched off-thread, debounced.
    std::vector<std::string> sugg_;
    bool sugg_focus_ = false;
    int  sugg_col_ = 0;
    std::string sugg_seen_, sugg_asked_;
    unsigned sugg_seen_ms_ = 0;
    std::thread sugg_thread_;
    std::atomic<bool> sugg_running_{false}, sugg_done_{false};
    std::mutex sugg_m_;
    std::string sugg_res_q_;
    std::vector<std::string> sugg_res_;
    int trig_l_ = 0, trig_r_ = 0;
    void pump_suggestions();
    void kb_clamp();
""")

# ---------------- ui.cpp ----------------
U = "src/ui.cpp"
sub(U, '#include "hwdetect.h"\n', '#include "hwdetect.h"\n#include "http.h"\n#include "../third_party/json.hpp"\n')
sub(U, "    if (rel_thread_.joinable()) rel_thread_.join();\n}\n",
       "    if (rel_thread_.joinable()) rel_thread_.join();\n    if (sugg_thread_.joinable()) sugg_thread_.join();\n}\n")

# bigger text everywhere
sub(U, "    font_title_ = gfx::Font::load(ttf, 40 * s, cjk, thai);\n    font_body_  = gfx::Font::load(ttf, 28 * s, cjk, thai);\n    font_small_ = gfx::Font::load(ttf, 23 * s, cjk, thai);\n",
       "    font_title_ = gfx::Font::load(ttf, 44 * s, cjk, thai);\n    font_body_  = gfx::Font::load(ttf, 32 * s, cjk, thai);\n    font_small_ = gfx::Font::load(ttf, 27 * s, cjk, thai);\n")
sub(U, "        font_icon_ = gfx::Font::load(dv, 30 * s);\n", "        font_icon_ = gfx::Font::load(dv, 36 * s);\n")
sub(U, "    const float rail_w = 88*u, cx0 = rail_w + 12*u, top = 96*u;\n    const float cardw = 282*u,",
       "    const float rail_w = 96*u, cx0 = rail_w + 12*u, top = 100*u;\n    const float cardw = 330*u,")

# open_search resets the suggestions
sub(U, "    kb_row_ = 1; kb_col_ = 0;   // start on 'q'\n    status_msg_.clear();\n}\n",
"""    kb_row_ = 1; kb_col_ = 0;   // start on 'q'
    status_msg_.clear();
    sugg_.clear(); sugg_focus_ = false; sugg_col_ = 0;
    sugg_seen_ = query_input_; sugg_asked_ = "\\x01"; sugg_seen_ms_ = 0;
}
""")

# Search-mode navigation: the suggestion chips sit above the top key row
sub(U, """        switch (a) {
            case Action::Left:  kb_col_ = (kb_col_ > 0) ? kb_col_ - 1 : (int)kb[kb_row_].size()-1; break;""",
"""        if (sugg_focus_ && sugg_.empty()) sugg_focus_ = false;
        if (sugg_focus_) {
            int n = (int)sugg_.size();
            switch (a) {
                case Action::Left:  if (sugg_col_ > 0) sugg_col_--; break;
                case Action::Right: if (sugg_col_ < n - 1) sugg_col_++; break;
                case Action::Down:  sugg_focus_ = false; break;
                case Action::Select: query_input_ = sugg_[std::min(sugg_col_, n - 1)];
                                     kb_caret_ = (int)query_input_.size();
                                     submit_search(); break;
                case Action::Search: submit_search(); break;
                case Action::Sort:   query_input_.clear(); kb_caret_ = 0; break;
                case Action::Back:   mode_ = Mode::Grid; kb_mode_ = KbMode::Search; break;
                default: break;
            }
            return;
        }
        switch (a) {
            case Action::Left:  kb_col_ = (kb_col_ > 0) ? kb_col_ - 1 : (int)kb[kb_row_].size()-1; break;""")
sub(U, """            case Action::Up:    if (kb_row_ > 0) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(--kb_row_, c); } break;""",
"""            case Action::Up:    if (kb_row_ > 0) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(--kb_row_, c); }
                                else if (!kb_numeric() && !sugg_.empty()) {
                                    sugg_focus_ = true;
                                    if (sugg_col_ >= (int)sugg_.size()) sugg_col_ = 0;
                                } break;""")

# pump: suggestions
sub(U, "void App::pump_async() {\n", "void App::pump_async() {\n    pump_suggestions();\n")

# render: skip while the screen is off is in main_ui; here the new search screen
start = "void App::render_search(gfx::Renderer& rn) {\n"
end = "void App::request_playback() {\n"
t = pathlib.Path(U).read_text()
i, j = t.index(start), t.index(end)
NEW_RENDER = r'''void App::render_search(gfx::Renderer& rn) {
    // Brick OS: big-key search screen. Query field on top (the title is its
    // placeholder), autocomplete chips under it, then the keyboard filling the rest.
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    const bool numeric = kb_numeric();

    const gfx::Font& qf = *font_title_;
    float bx = 24*s, by = 16*s, bw = W - 48*s, bh = qf.line_height() + 20*s;
    rn.round_rect({bx, by, bw, bh}, bh / 2, theme_.chip);
    float tx0 = bx + 30*s, avail = bw - 60*s, ty = by + (bh - qf.line_height()) / 2;
    int csz = (int)query_input_.size();
    int caret = std::max(0, std::min(kb_caret_, csz));
    int start = 0;
    auto next_char = [&](int i) { i++; while (i < csz && ((unsigned char)query_input_[i] & 0xC0) == 0x80) i++; return i; };
    while (start < caret && qf.text_width(query_input_.substr(start, caret - start)) > avail)
        start = next_char(start);
    std::string shown;
    for (int i = start; i < csz;) {
        int j = next_char(i);
        std::string cand = shown + query_input_.substr(i, j - i);
        if (qf.text_width(cand) > avail) break;
        shown = std::move(cand);
        i = j;
    }
    rn.text(qf, shown, tx0, ty, theme_.text);
    if (query_input_.empty()) {
        const std::string& ph = kb_placeholder_.empty() ? kb_title_ : kb_placeholder_;
        rn.text(qf, ph, tx0, ty, theme_.text_dim);
    }
    float caret_x = tx0 + qf.text_width(query_input_.substr(start, caret - start));
    if ((SDL_GetTicks() / 500) % 2 == 0)
        rn.quad({caret_x + 1*s, by + 12*s, 3*s, bh - 24*s}, theme_.text);

    float gy = by + bh + 22*s;
    if (!numeric) {   // autocomplete chips
        const gfx::Font& f = *font_body_;
        float ch = f.line_height() + 16*s, cgap = 12*s, pad = 22*s;
        float sy = by + bh + 12*s;
        if (sugg_.empty()) {
            rn.text(*font_small_, query_input_.empty() ? "" : "\xE2\x80\xA6", bx + 12*s,
                    sy + (ch - font_small_->line_height()) / 2, theme_.text_dim);
        } else {
            std::vector<float> xs; float x = 0;
            for (const auto& q : sugg_) { xs.push_back(x); x += f.text_width(q) + 2*pad + cgap; }
            float shift = 0;
            if (sugg_focus_ && sugg_col_ < (int)sugg_.size()) {
                float right = xs[sugg_col_] + f.text_width(sugg_[sugg_col_]) + 2*pad;
                shift = std::max(0.f, right - (W - 48*s));
            }
            for (int k = 0; k < (int)sugg_.size(); ++k) {
                float cx = bx + xs[k] - shift, cw = f.text_width(sugg_[k]) + 2*pad;
                if (cx > W) break;
                if (cx + cw < 0) continue;
                bool sel = sugg_focus_ && k == sugg_col_;
                rn.round_rect({cx, sy, cw, ch}, ch / 2, sel ? theme_.text : theme_.chip);
                rn.text(f, sugg_[k], cx + pad, sy + (ch - f.line_height()) / 2, sel ? theme_.bg : theme_.text);
            }
        }
        gy = sy + ch + 16*s;
    }

    float fh = font_small_->line_height() + 18*s;
    const auto& kb = numeric ? KB_NUM : kb_thai_ ? (kb_shift_ ? KB_TH_SHIFT : KB_TH) : KB;
    int nrows = (int)kb.size();
    float gap = 10*s;
    float keyh = (H - fh - 12*s - gy - (nrows - 1) * gap) / nrows;
    keyh = std::min(keyh, numeric ? 110*s : 120*s);
    int units = 0;
    for (const auto& row : kb) { int u = 0; for (const auto& k : row) u += k.span; units = std::max(units, u); }
    float unit = numeric ? keyh : (W * 0.97f - (units - 1) * gap) / (float)units;
    for (int r = 0; r < nrows; ++r) {
        float roww = -gap;
        for (const auto& k : kb[r]) roww += unit * k.span + gap * k.span;
        float x = (W - roww) / 2;
        float ky = gy + r * (keyh + gap);
        for (int c = 0; c < (int)kb[r].size(); ++c) {
            const Key& k = kb[r][c];
            float kw = unit * k.span + gap * (k.span - 1);
            bool sel = !sugg_focus_ && r == kb_row_ && c == kb_col_;
            gfx::Rect kr{x, ky, kw, keyh};
            gfx::Color bg = sel ? theme_.text : (k.type == KSUBMIT) ? theme_.accent : theme_.chip;
            rn.round_rect(kr, 12*s, bg);
            if (k.type == KSPACE) {
                float bar = kw * 0.42f;
                rn.quad({kr.x + (kw - bar) / 2, kr.y + keyh * 0.68f, bar, 4*s}, sel ? theme_.bg : theme_.text_dim);
            } else {
                std::string lbl = (k.type == KSUBMIT) ? i18n::tr(i18n::Str::KbEnter) : k.label;
                if (k.type == KLANG && kb_thai_) lbl = "A/\xE0\xB8\x81";
                if (k.type == KCHAR && kb_shift_ && lbl.size() == 1 && lbl[0] >= 'a' && lbl[0] <= 'z')
                    lbl[0] = (char)(lbl[0] - 'a' + 'A');
                const gfx::Font* f = font_title_.get();
                if (f->text_width(lbl) > kw - 10*s) f = font_body_.get();
                if (f->text_width(lbl) > kw - 8*s) f = font_small_.get();
                gfx::Color tc = sel ? theme_.bg : (k.type == KSHIFT && kb_shift_) ? theme_.accent : theme_.text;
                rn.text(*f, lbl, kr.x + (kw - f->text_width(lbl)) / 2,
                        kr.y + (keyh - f->line_height()) / 2, tc);
            }
            x += kw + gap;
        }
    }

    rn.quad({0, H - fh, (float)W, fh}, theme_.panel);
    std::string hint = i18n::tr(numeric ? i18n::Str::FooterCastCode : i18n::Str::FooterSearch);
    hint = font_small_->ellipsize(hint, W - 40*s);
    rn.text(*font_small_, hint, (W - font_small_->text_width(hint)) / 2,
            H - fh + (fh - font_small_->line_height()) / 2, theme_.text_dim);
    rn.end();
}

void App::kb_clamp() {
    const auto& kb = kb_numeric() ? KB_NUM : kb_thai_ ? (kb_shift_ ? KB_TH_SHIFT : KB_TH) : KB;
    if (kb_row_ >= (int)kb.size()) kb_row_ = (int)kb.size() - 1;
    if (kb_row_ < 0) kb_row_ = 0;
    if (kb_col_ >= (int)kb[kb_row_].size()) kb_col_ = (int)kb[kb_row_].size() - 1;
}

bool App::kb_pad(int b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_B:
            if (query_input_.empty()) { mode_ = Mode::Grid; kb_mode_ = KbMode::Search; }
            else backspace();
            return true;
        case SDL_CONTROLLER_BUTTON_Y:
            query_input_.insert(query_input_.begin() + kb_caret_, ' '); kb_caret_++;
            return true;
        case SDL_CONTROLLER_BUTTON_X:
            query_input_.clear(); kb_caret_ = 0;
            return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (!kb_numeric()) { kb_thai_ = !kb_thai_; kb_shift_ = false; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (!kb_numeric()) { kb_shift_ = !kb_shift_; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_START:
            submit_search();
            return true;
        case SDL_CONTROLLER_BUTTON_BACK:
            mode_ = Mode::Grid; kb_mode_ = KbMode::Search;
            return true;
        default:
            return false;
    }
}

void App::kb_trigger(bool right, int v) {
    int& st = right ? trig_r_ : trig_l_;
    int now = v > 16000 ? 1 : (v < 8000 ? 0 : st);
    if (now == st) return;
    st = now;
    if (!now) return;
    if (!right) { while (kb_caret_ > 0 && ((unsigned char)query_input_[--kb_caret_] & 0xC0) == 0x80) {} }
    else if (kb_caret_ < (int)query_input_.size()) {
        kb_caret_++;
        while (kb_caret_ < (int)query_input_.size() && ((unsigned char)query_input_[kb_caret_] & 0xC0) == 0x80) kb_caret_++;
    }
}

// Autocomplete: 250 ms after the query stops changing, ask YouTube's suggest endpoint
// (one request in flight; a stale answer is dropped).
void App::pump_suggestions() {
    if (sugg_done_) {
        sugg_done_ = false;
        std::lock_guard<std::mutex> lk(sugg_m_);
        if (sugg_res_q_ == query_input_) {
            sugg_ = sugg_res_;
            if (sugg_col_ >= (int)sugg_.size()) sugg_col_ = 0;
            if (sugg_.empty()) sugg_focus_ = false;
        }
    }
    if (mode_ != Mode::Search || kb_mode_ != KbMode::Search) return;
    unsigned now = SDL_GetTicks();
    if (query_input_ != sugg_seen_) { sugg_seen_ = query_input_; sugg_seen_ms_ = now; }
    if (sugg_seen_ == sugg_asked_ || sugg_running_ || now - sugg_seen_ms_ < 250) return;
    std::string q = sugg_seen_;
    sugg_asked_ = q;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) { sugg_.clear(); sugg_focus_ = false; return; }
    if (sugg_thread_.joinable()) sugg_thread_.join();
    sugg_running_ = true;
    std::string hl = i18n::language_hl(i18n::language());
    std::string full = sugg_seen_;
    sugg_thread_ = std::thread([this, q, hl, full]() {
        std::vector<std::string> out;
        try {
            std::string enc;
            static const char* hex = "0123456789ABCDEF";
            for (unsigned char c : q) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
                else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
            }
            HttpClient http;
            auto r = http.get("https://suggestqueries.google.com/complete/search?client=firefox&ds=yt&oe=utf-8&hl="
                              + hl + "&q=" + enc, {"User-Agent: Mozilla/5.0"}, 6);
            if (r.ok()) {
                auto j = nlohmann::json::parse(r.body, nullptr, false);
                if (j.is_array() && j.size() > 1 && j[1].is_array())
                    for (auto& e : j[1]) if (e.is_string() && out.size() < 10) out.push_back(e.get<std::string>());
            }
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(sugg_m_); sugg_res_q_ = full; sugg_res_ = std::move(out); }
        sugg_running_ = false;
        sugg_done_ = true;
    });
}

'''
t = t[:i] + NEW_RENDER + t[j:]
pathlib.Path(U).write_text(t)

# ---------------- main_ui.cpp ----------------
M = "src/main_ui.cpp"
sub(M, """                if (b == SDL_CONTROLLER_BUTTON_Y) app.input(A::Search); // open/submit""",
"""                if (app.mode() == M::Search && !app.menu_open() && app.kb_pad(b)) continue;
                if (b == SDL_CONTROLLER_BUTTON_Y) app.input(A::Search); // open/submit""")
sub(M, """                    st = &stick_y; lo = SDL_CONTROLLER_BUTTON_DPAD_UP;   hi = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
                } else continue;""",
"""                    st = &stick_y; lo = SDL_CONTROLLER_BUTTON_DPAD_UP;   hi = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
                } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                           e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    if (app.mode() == M::Search && !app.menu_open())
                        app.kb_trigger(e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, e.caxis.value);
                    continue;
                } else continue;""")
sub(M, """        app.pump_async();
        app.render(rn);
        win->swap();
        SDL_Delay(16);
    }
    SDL_StopTextInput();""",
"""        app.pump_async();
        if (app.screen_dark()) { SDL_Delay(100); continue; }   // screen off: audio only, draw nothing
        app.render(rn);
        win->swap();
        SDL_Delay(16);
    }
    SDL_StopTextInput();""")

# ---------------- footer text ----------------
EN = "A type   B delete   Y space   X clear   L ABC/ก   R shift   L2/R2 cursor   START search"
TH = "A พิมพ์   B ลบ   Y เว้นวรรค   X ล้าง   L ไทย/EN   R Shift   L2/R2 เลื่อน   START ค้นหา"
for f in pathlib.Path("data/lang").glob("*.json"):
    if f.name == "languages.json": continue
    lines = f.read_text().split("\n")
    out = []
    for ln in lines:
        if ln.strip().startswith('"FooterSearch":'):
            if f.name == "en.json": ln = '  "FooterSearch": ' + json.dumps(EN, ensure_ascii=False) + ","
            elif f.name == "th.json": ln = '  "FooterSearch": ' + json.dumps(TH, ensure_ascii=False) + ","
            else: continue   # falls back to English (the old hints are wrong now)
        out.append(ln)
    f.write_text("\n".join(out))
print("kb_patch ok")
