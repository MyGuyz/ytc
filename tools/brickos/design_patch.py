#!/usr/bin/env python3
# Brick OS YTC: the TV view (DESIGN.md 6.4) and the search keyboard (6.5, symmetric Thai rows as the user asked
# 2026-11-09) in the design's fonts (Inter + the Thai font, em px) and colours. Run in ~/ytc after kb_patch.py.
import pathlib, sys, re

def sub(path, old, new, count=1):
    p = pathlib.Path(path); t = p.read_text()
    if t.count(old) != count:
        sys.exit(f"{path}: expected {count} of {old[:80]!r}, found {t.count(old)}")
    p.write_text(t.replace(old, new))

pathlib.Path("data/fonts/LICENSE.txt").rename("data/fonts/Inter-LICENSE.txt") if pathlib.Path("data/fonts/LICENSE.txt").exists() else None

# ---------- gfx: em-sized fonts (pixel_h < 0) with Latin from the primary ----------
sub("src/gfx.h", "    float thai_scale_ = 0;\n", "    float thai_scale_ = 0;\n    bool latin_primary_ = false;               // em-sized (DESIGN.md) fonts: Latin from the primary (Inter)\n")
sub("src/gfx.cpp", """    font->info_ = info;
    font->pixel_h_ = pixel_h;
    font->scale_ = stbtt_ScaleForPixelHeight(info, pixel_h);""",
"""    font->info_ = info;
    bool em = pixel_h < 0;   // Brick OS: a negative size is a CSS-style em size (DESIGN.md px)
    if (em) pixel_h = -pixel_h;
    font->latin_primary_ = em;
    font->pixel_h_ = pixel_h;
    font->scale_ = em ? stbtt_ScaleForMappingEmToPixels(info, pixel_h) : stbtt_ScaleForPixelHeight(info, pixel_h);""")
sub("src/gfx.cpp", "if (thai_info_ && (thai_cp(cp) || (cp >= 0x20 && cp < 0x2070))) {",
    "if (thai_info_ && (thai_cp(cp) || (!latin_primary_ && cp >= 0x20 && cp < 0x2070))) {")

# ---------- i18n: the keyboard label and the TV hints ----------
sub("src/i18n.h", "    FilterRelevance, FilterPopularity, FooterSearchEmpty,",
    "    FilterRelevance, FilterPopularity, FooterSearchEmpty,\n    SearchYouTube, KbSpace, KbDelete, HintPlay, HintFollow, HintTypeSearch, HintTabs,")
sub("src/i18n.cpp", '"FilterRelevance","FilterPopularity","FooterSearchEmpty",',
    '"FilterRelevance","FilterPopularity","FooterSearchEmpty",\n    "SearchYouTube","KbSpace","KbDelete","HintPlay","HintFollow","HintTypeSearch","HintTabs",')
import json
EN = {"SearchYouTube": "Search YouTube", "KbSpace": "space", "KbDelete": "delete", "HintPlay": "Play",
      "HintFollow": "Follow channel", "HintTypeSearch": "Type to search", "HintTabs": "Switch tabs",
      "FooterSearch": "A type   B delete   X space   Y ABC/ก   L1 shift   START search"}
TH = {"SearchYouTube": "ค้นหาใน YouTube", "KbSpace": "เว้นวรรค", "KbDelete": "ลบ", "HintPlay": "เล่น",
      "HintFollow": "ติดตามช่อง", "HintTypeSearch": "พิมพ์ค้นหา", "HintTabs": "สลับแท็บ",
      "FooterSearch": "A พิมพ์   B ลบ   X เว้นวรรค   Y ก/A   L1 Shift   START ค้นหา"}
for f, table in (("data/lang/en.json", EN), ("data/lang/th.json", TH)):
    p = pathlib.Path(f); lines = p.read_text().split("\n")
    lines = [l for l in lines if not any(l.strip().startswith(f'"{k}":') for k in table)]
    i = next(i for i, l in enumerate(lines) if l.strip().startswith('"FooterSearchEmpty"'))
    add = ['  ' + json.dumps(k) + ': ' + json.dumps(v, ensure_ascii=False) + ',' for k, v in table.items()]
    lines[i + 1:i + 1] = add
    p.write_text("\n".join(lines))

# ---------- ui.h ----------
sub("src/ui.h", "    void pump_suggestions();\n",
"""    void pump_suggestions();
    // Brick OS DESIGN.md: its fonts (Inter + the Thai font at em px, 768-high screen) and pieces
    enum DZ { DZ_STATUS, DZ_PCT, DZ_ROW, DZ_CARD, DZ_META, DZ_CHIP, DZ_CHIP_SEL, DZ_PILL, DZ_HINT, DZ_HINTKEY,
              DZ_LABEL, DZ_QUERY, DZ_KEY, DZ_KBTN, DZ_KHINT, DZ_KCHIP, DZ_BADGE, DZ_N };
    std::unique_ptr<gfx::Font> dz_[DZ_N];
    std::unique_ptr<gfx::Texture> rail_tex_[6];
    void load_design_fonts();
    const gfx::Font& dz(int i) const { return dz_[i] ? *dz_[i] : *font_body_; }
    void design_status_bar(gfx::Renderer& rn);
    void design_hint_bar(gfx::Renderer& rn, const std::vector<std::pair<std::string, std::string>>& hints);
""")

# ---------- ui.cpp ----------
U = "src/ui.cpp"
sub(U, "    fonts_baked_h_ = win_->height();\n", "    fonts_baked_h_ = win_->height();\n    load_design_fonts();\n")

# Thai rows: 12 keys each (the mockup's rows; ⇧ and ⌫ close the fourth), marks without the dotted circle
t = pathlib.Path(U).read_text()
a = t.index("#define TK(t) {t, KCHAR, 0, 1, t}")
b = t.index("#undef TK")
t = t[:a] + '''#define TK(t) {t, KCHAR, 0, 1, t}
#define TM(t) {t, KCHAR, 0, 1, t}
// Brick OS: every Thai row is 12 keys wide (the mockup's rows; ฃ/ฅ end the third), so the keys line up in columns
// edge to edge like the English rows; a mark key shows the mark alone.
const std::vector<std::vector<Key>> KB_TH = {
    {TK("ๅ"),TK("/"),TK("-"),TK("ภ"),TK("ถ"),TM("ุ"),TM("ึ"),TK("ค"),TK("ต"),TK("จ"),TK("ข"),TK("ช")},
    {TK("ๆ"),TK("ไ"),TK("ำ"),TK("พ"),TK("ะ"),TM("ั"),TM("ี"),TK("ร"),TK("น"),TK("ย"),TK("บ"),TK("ล")},
    {TK("ฟ"),TK("ห"),TK("ก"),TK("ด"),TK("เ"),TM("้"),TM("่"),TK("า"),TK("ส"),TK("ว"),TK("ง"),TK("ฃ")},
    {{"\\xE2\\x87\\xA7",KSHIFT,0,1},
     TK("ผ"),TK("ป"),TK("แ"),TK("อ"),TM("ิ"),TM("ื"),TK("ท"),TK("ม"),TK("ใ"),TK("ฝ"),
     {"\\xE2\\x8C\\xAB",KDEL,0,1}},
    {{"\\xE2\\x97\\x80",KLEFT,0,1},{"\\xE2\\x96\\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',6},{"-",KCHAR,'-',1},{"\\xE0\\xB8\\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
const std::vector<std::vector<Key>> KB_TH_SHIFT = {
    {TK("+"),TK("๑"),TK("๒"),TK("๓"),TK("๔"),TM("ู"),TK("฿"),TK("๕"),TK("๖"),TK("๗"),TK("๘"),TK("๙")},
    {TK("๐"),TK("\\""),TK("ฎ"),TK("ฑ"),TK("ธ"),TM("ํ"),TM("๊"),TK("ณ"),TK("ฯ"),TK("ญ"),TK("ฐ"),TK(",")},
    {TK("ฤ"),TK("ฆ"),TK("ฏ"),TK("โ"),TK("ฌ"),TM("็"),TM("๋"),TK("ษ"),TK("ศ"),TK("ซ"),TK("."),TK("ฅ")},
    {{"\\xE2\\x87\\xA7",KSHIFT,0,1},
     TK("("),TK(")"),TK("ฉ"),TK("ฮ"),TM("ฺ"),TM("์"),TK("?"),TK("ฒ"),TK("ฬ"),TK("ฦ"),
     {"\\xE2\\x8C\\xAB",KDEL,0,1}},
    {{"\\xE2\\x97\\x80",KLEFT,0,1},{"\\xE2\\x96\\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',6},{"-",KCHAR,'-',1},{"\\xE0\\xB8\\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
''' + t[b:]
pathlib.Path(U).write_text(t)

# open_search: the design's label
sub(U, "    kb_title_ = i18n::tr(i18n::Str::Search);\n    kb_placeholder_ = i18n::tr(i18n::Str::TypeToSearch);",
       "    kb_title_ = i18n::tr(i18n::Str::SearchYouTube);\n    kb_placeholder_.clear();")

# pad: the mockup's buttons (A type, B delete, X space, Y ก/A, L1 shift, START search)
sub(U, """        case SDL_CONTROLLER_BUTTON_Y:
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
            return true;""",
"""        case SDL_CONTROLLER_BUTTON_X:
            query_input_.insert(query_input_.begin() + kb_caret_, ' '); kb_caret_++;
            return true;
        case SDL_CONTROLLER_BUTTON_Y:
            if (!kb_numeric()) { kb_thai_ = !kb_thai_; kb_shift_ = false; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (!kb_numeric()) { kb_shift_ = !kb_shift_; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return true;""")

# render_search: DESIGN.md 6.5
t = pathlib.Path(U).read_text()
a = t.index("void App::render_search(gfx::Renderer& rn) {")
b = t.index("void App::kb_clamp() {")
t = t[:a] + r'''// DESIGN.md colours (section 2)
namespace dzc {
const gfx::Color bg{0, 0, 0, 1}, s1{0x1C/255.f, 0x1C/255.f, 0x1E/255.f, 1}, s2{0x2C/255.f, 0x2C/255.f, 0x2E/255.f, 1},
    s3{0x3A/255.f, 0x3A/255.f, 0x3C/255.f, 1}, kb{0x12/255.f, 0x12/255.f, 0x14/255.f, 1},
    text{0xF2/255.f, 0xF2/255.f, 0xF7/255.f, 1}, text2{0xA1/255.f, 0xA1/255.f, 0xA6/255.f, 1},
    text3{0xC7/255.f, 0xC7/255.f, 0xCC/255.f, 1}, accent{0x0A/255.f, 0x84/255.f, 0xFF/255.f, 1},
    white{1, 1, 1, 1}, red{0xFF/255.f, 0x45/255.f, 0x3A/255.f, 1};
}

void App::load_design_fonts() {
    const float u = win_->height() / 768.f;
    std::string dir = platform::exe_dir() + "/data/fonts/";
    if (!std::ifstream(dir + "Inter-Regular.ttf").good()) dir = "data/fonts/";
    std::string thai = dir + "BrickOSThai.ttf";
    if (!std::ifstream(thai).good()) thai.clear();
    const std::string cjk = cjk_font_path();
    struct { int id; const char* w; float px; } spec[] = {
        {DZ_STATUS, "SemiBold", 21}, {DZ_PCT, "SemiBold", 19}, {DZ_ROW, "Bold", 28}, {DZ_CARD, "SemiBold", 20},
        {DZ_META, "Regular", 16}, {DZ_CHIP, "Medium", 19}, {DZ_CHIP_SEL, "SemiBold", 19}, {DZ_PILL, "SemiBold", 15},
        {DZ_HINT, "Regular", 19}, {DZ_HINTKEY, "Bold", 16}, {DZ_LABEL, "Regular", 21}, {DZ_QUERY, "Medium", 32},
        {DZ_KEY, "Regular", 34}, {DZ_KBTN, "SemiBold", 22}, {DZ_KHINT, "Regular", 18}, {DZ_KCHIP, "Regular", 20},
        {DZ_BADGE, "Regular", 16}};
    for (auto& sp : spec)
        dz_[sp.id] = gfx::Font::load(dir + "Inter-" + sp.w + ".ttf", -sp.px * u, cjk, thai);
    static const char* rail[6] = {"search", "home", "fav", "later", "history", "settings"};
    std::string idir = platform::exe_dir() + "/data/icons/";
    if (!std::ifstream(idir + "rail_home.png").good()) idir = "data/icons/";
    for (int k = 0; k < 6; ++k) {
        std::ifstream f(idir + "rail_" + rail[k] + ".png", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) rail_tex_[k] = gfx::Texture::from_encoded(bytes.data(), bytes.size());
    }
}

// DESIGN.md 5.1: time left (21/600), battery % right (19/600); 56 high
void App::design_status_bar(gfx::Renderer& rn) {
    const int W = win_->width(); const float u = win_->height() / 768.f;
    const float h = 56*u;
    char clock[16]; time_t now = time(nullptr); strftime(clock, sizeof clock, "%H:%M", localtime(&now));
    rn.text(dz(DZ_STATUS), clock, 32*u, (h - dz(DZ_STATUS).line_height()) / 2, dzc::text);
    static unsigned last = 0; static int pct = -1;
    if (pct < 0 || SDL_GetTicks() - last > 30000) {
        last = SDL_GetTicks(); pct = -1;
        for (const char* p : {"/sys/class/power_supply/axp2202-battery/capacity", "/sys/class/power_supply/battery/capacity"}) {
            std::ifstream f(p); int v; if (f >> v) { pct = v; break; }
        }
    }
    if (pct >= 0) {
        std::string s = std::to_string(pct) + "%";
        rn.text(dz(DZ_PCT), s, W - 32*u - dz(DZ_PCT).text_width(s), (h - dz(DZ_PCT).line_height()) / 2, dzc::text);
    }
}

// DESIGN.md 5.2: 64 high, 1px #1C1C1E top line, 32x32 #2C2C2E circles (wide buttons a pill), labels 19 #A1A1A6
void App::design_hint_bar(gfx::Renderer& rn, const std::vector<std::pair<std::string, std::string>>& hints) {
    const int W = win_->width(), H = win_->height(); const float u = H / 768.f;
    const float top = H - 64*u, cy = top + 32*u;
    rn.quad({0, top, (float)W, 64*u}, dzc::bg);
    rn.quad({0, top, (float)W, std::max(1.f, u)}, dzc::s1);
    float x = 40*u;
    for (auto& h : hints) {
        const gfx::Font& kf = dz(DZ_HINTKEY);
        bool wide = h.first.size() > 1;
        float bw = wide ? kf.text_width(h.first) + 24*u : 32*u;
        rn.round_rect({x, cy - 16*u, bw, 32*u}, 16*u, dzc::s2);
        rn.text(kf, h.first, x + (bw - kf.text_width(h.first)) / 2, cy - kf.line_height() / 2, dzc::text);
        x += bw + 10*u;
        rn.text(dz(DZ_HINT), h.second, x, cy - dz(DZ_HINT).line_height() / 2, dzc::text2);
        x += dz(DZ_HINT).text_width(h.second) + 32*u;
    }
}

void App::render_search(gfx::Renderer& rn) {
    // DESIGN.md 6.5: label, query field and suggestion chips on top; the key panel at the bottom
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    rn.begin(W, H);
    rn.clear(dzc::bg);
    const bool numeric = kb_numeric();

    float x0 = 48*u, y = 36*u;
    rn.text(dz(DZ_LABEL), kb_title_, x0, y, dzc::text2);
    y += dz(DZ_LABEL).line_height() + 14*u;
    const gfx::Font& qf = dz(DZ_QUERY);
    float bw = W - 2*x0, bh = 80*u;
    rn.round_rect({x0, y, bw, bh}, 20*u, dzc::s1);
    float tx0 = x0 + 24*u, avail = bw - 48*u, ty = y + (bh - qf.line_height()) / 2;
    int csz = (int)query_input_.size();
    int caret = std::max(0, std::min(kb_caret_, csz));
    int start = 0;
    auto next_char = [&](int i) { i++; while (i < csz && ((unsigned char)query_input_[i] & 0xC0) == 0x80) i++; return i; };
    while (start < caret && qf.text_width(query_input_.substr(start, caret - start)) > avail) start = next_char(start);
    std::string shown;
    for (int i = start; i < csz;) {
        int j = next_char(i);
        std::string cand = shown + query_input_.substr(i, j - i);
        if (qf.text_width(cand) > avail) break;
        shown = std::move(cand); i = j;
    }
    rn.text(qf, shown, tx0, ty, dzc::text);
    if (query_input_.empty() && !kb_placeholder_.empty()) rn.text(qf, kb_placeholder_, tx0, ty, dzc::text2);
    float caret_x = tx0 + qf.text_width(query_input_.substr(start, caret - start));
    if ((SDL_GetTicks() / 500) % 2 == 0) rn.quad({caret_x + 2*u, y + (bh - 38*u) / 2, 3*u, 38*u}, dzc::accent);
    y += bh + 14*u;

    if (!numeric && !sugg_.empty()) {   // chips: padding 10 18, radius 12, #1C1C1E, 20px, gap 12
        const gfx::Font& f = dz(DZ_KCHIP);
        float ch = f.line_height() + 20*u, pad = 18*u, gap = 12*u;
        std::vector<float> xs; float cx = 0;
        for (const auto& q : sugg_) { xs.push_back(cx); cx += f.text_width(q) + 2*pad + gap; }
        float shift = 0;
        if (sugg_focus_ && sugg_col_ < (int)sugg_.size())
            shift = std::max(0.f, xs[sugg_col_] + f.text_width(sugg_[sugg_col_]) + 2*pad - (W - 2*x0));
        for (int k = 0; k < (int)sugg_.size(); ++k) {
            float x = x0 + xs[k] - shift, w = f.text_width(sugg_[k]) + 2*pad;
            if (x > W) break;
            if (x + w < 0) continue;
            bool sel = sugg_focus_ && k == sugg_col_;
            rn.round_rect({x, y, w, ch}, 12*u, sel ? dzc::accent : dzc::s1);
            rn.text(f, sugg_[k], x + pad, y + (ch - f.line_height()) / 2, sel ? dzc::white : dzc::text);
        }
    }

    // the panel: #121214, 1px #1C1C1E top line, padding 18 24 14, rows gap 10, keys 66 high, gap 8
    const auto& kb = numeric ? KB_NUM : kb_thai_ ? (kb_shift_ ? KB_TH_SHIFT : KB_TH) : KB;
    int nrows = (int)kb.size();
    float keyh = 66*u, gap = 8*u, rgap = 10*u;
    const gfx::Font& hf = dz(DZ_KHINT);
    float panel_h = 18*u + nrows * keyh + (nrows - 1) * rgap + 4*u + hf.line_height() + 14*u + rgap;
    float py = H - panel_h;
    rn.quad({0, py, (float)W, panel_h}, dzc::kb);
    rn.quad({0, py, (float)W, std::max(1.f, u)}, dzc::s1);
    int units = 0;
    for (const auto& row : kb) { int n = 0; for (const auto& k : row) n += k.span; units = std::max(units, n); }
    float inner = W - 48*u;
    float unit = numeric ? keyh : (inner - (units - 1) * gap) / (float)units;
    float gy = py + 18*u;
    for (int r = 0; r < nrows; ++r) {
        float roww = -gap;
        for (const auto& k : kb[r]) roww += unit * k.span + gap * k.span;
        float x = (W - roww) / 2, ky = gy + r * (keyh + rgap);
        for (int c = 0; c < (int)kb[r].size(); ++c) {
            const Key& k = kb[r][c];
            float kw = unit * k.span + gap * (k.span - 1);
            bool sel = !sugg_focus_ && r == kb_row_ && c == kb_col_;
            gfx::Rect kr{x, ky, kw, keyh};
            rn.round_rect(kr, 14*u, (sel || k.type == KSUBMIT) ? dzc::accent : dzc::s2);
            std::string lbl = k.label; const gfx::Font* f = &dz(DZ_KEY); gfx::Color tc = sel ? dzc::white : dzc::text;
            switch (k.type) {
                case KSUBMIT: lbl = i18n::tr(i18n::Str::KbEnter); f = &dz(DZ_KBTN); tc = dzc::white; break;
                case KSPACE:  lbl = i18n::tr(i18n::Str::KbSpace); f = &dz(DZ_LABEL); if (!sel) tc = dzc::text3; break;
                case KLANG:   lbl = kb_thai_ ? "A / \xE0\xB8\x81" : "\xE0\xB8\x81 / A"; f = &dz(DZ_KBTN); break;
                case KCHAR:
                    if (kb_shift_ && lbl.size() == 1 && lbl[0] >= 'a' && lbl[0] <= 'z') lbl[0] = (char)(lbl[0] - 'a' + 'A');
                    break;
                default: break;
            }
            if (k.type == KSHIFT && kb_shift_ && !sel) tc = dzc::accent;
            if (f->text_width(lbl) > kw - 8*u) f = &dz(DZ_KBTN);
            rn.text(*f, lbl, kr.x + (kw - f->text_width(lbl)) / 2, kr.y + (keyh - f->line_height()) / 2, tc);
            x += kw + gap;
        }
    }
    std::string hint = i18n::tr(numeric ? i18n::Str::FooterCastCode : i18n::Str::FooterSearch);
    rn.text(hf, hint, (W - hf.text_width(hint)) / 2, gy + nrows * keyh + (nrows - 1) * rgap + rgap + 4*u, dzc::text2);
    rn.end();
}

''' + t[b:]
pathlib.Path(U).write_text(t)
print("design_patch part 1 ok")
