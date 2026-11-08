"""Add Thai to YTC (Brick OS): a Thai font with mark placement, the Thai UI translation and a
Kedmanee on-screen keyboard. Usage: python3 thai_patch.py <ytc repo>"""
import json
import os
import re
import shutil
import sys

R = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))


def patch(rel, pairs):
    p = os.path.join(R, rel)
    s = open(p, encoding="utf-8").read()
    for old, new in pairs:
        n = s.count(old)
        assert n == 1, f"{rel}: expected 1 of {old[:70]!r}, found {n}"
        s = s.replace(old, new)
    open(p, "w", encoding="utf-8").write(s)


# ---- build: text_thai.c is C ----
patch("CMakeLists.txt", [
    ("project(ytc CXX)", "project(ytc C CXX)"),
    ("add_library(ytgfx STATIC src/gfx.cpp src/ui.cpp src/player.cpp src/stream.cpp src/remux.cpp src/i18n.cpp)",
     "add_library(ytgfx STATIC src/gfx.cpp src/ui.cpp src/player.cpp src/stream.cpp src/remux.cpp src/i18n.cpp src/text_thai.c)"),
    ("add_executable(yt_setup src/main_setup.cpp src/gfx.cpp)",
     "add_executable(yt_setup src/main_setup.cpp src/gfx.cpp src/text_thai.c)"),
])
for f in ("text_thai.c", "text_thai.h"):
    shutil.copy(os.path.join(HERE, f), os.path.join(R, "src", f))

# ---- gfx: Thai font + mark placement ----
patch("src/gfx.h", [
    ("""    static std::unique_ptr<Font> load(const std::string& ttf_path, float pixel_h,
                                      const std::string& fallback_path = "");""",
     """    // thai_path: optional Thai font (Brick OS: res/font1-thai.ttf, marks pre-positioned in its PUA).
    static std::unique_ptr<Font> load(const std::string& ttf_path, float pixel_h,
                                      const std::string& fallback_path = "",
                                      const std::string& thai_path = "");"""),
    ("    std::shared_ptr<const std::vector<uint8_t>> ttf_, fb_ttf_;  // file bytes (fontinfos point here)",
     "    std::shared_ptr<const std::vector<uint8_t>> ttf_, fb_ttf_, thai_ttf_;  // file bytes (fontinfos point here)"),
    ("    void* fb_info_ = nullptr;                 // fallback stbtt_fontinfo* or nullptr",
     "    void* fb_info_ = nullptr;                 // fallback stbtt_fontinfo* or nullptr\n"
     "    void* thai_info_ = nullptr;               // Thai stbtt_fontinfo* or nullptr\n"
     "    float thai_scale_ = 0;"),
])

patch("src/gfx.cpp", [
    ("""std::unique_ptr<Font> Font::load(const std::string& path, float pixel_h,
                                 const std::string& fallback_path) {""",
     """// Thai: SDL-less stb_truetype has no shaping, so Thai marks are swapped for glyphs the
// Thai font has pre-positioned (text_thai.c). Idempotent: shaped text shapes to itself.
static std::string thai_shape(const std::string& s) {
    if (!TextThai_has(s.c_str())) return s;
    std::vector<char> out(s.size() * 2 + 16);
    if (TextThai_toGlyphs(s.c_str(), out.data(), (int)out.size()) <= 0) return s;
    return std::string(out.data());
}
static bool thai_cp(uint32_t cp) {
    return (cp >= 0x0E00 && cp <= 0x0E7F) || (cp >= 0xF700 && cp <= 0xF72F);
}

std::unique_ptr<Font> Font::load(const std::string& path, float pixel_h,
                                 const std::string& fallback_path, const std::string& thai_path) {"""),
    ("""            } else {
                delete fi;
            }
        }
    }
    return font;
}""",
     """            } else {
                delete fi;
            }
        }
    }
    if (!thai_path.empty()) {
        if (auto th = read_font_file(thai_path)) {
            auto* ti = new stbtt_fontinfo();
            if (stbtt_InitFont(ti, th->data(), stbtt_GetFontOffsetForIndex(th->data(), 0))) {
                float em_px = font->scale_ / stbtt_ScaleForMappingEmToPixels(info, 1.0f);
                font->thai_scale_ = stbtt_ScaleForMappingEmToPixels(ti, em_px);
                font->thai_ttf_ = std::move(th);
                font->thai_info_ = ti;
            } else {
                delete ti;
            }
        }
    }
    return font;
}"""),
    ("""    delete static_cast<stbtt_fontinfo*>(fb_info_);
}""",
     """    delete static_cast<stbtt_fontinfo*>(fb_info_);
    delete static_cast<stbtt_fontinfo*>(thai_info_);
}"""),
    ("""    int gi = stbtt_FindGlyphIndex(info, (int)cp);
    if (gi == 0 && fb_info_) {""",
     """    int gi = 0;
    if (thai_info_ && thai_cp(cp)) {   // Thai and its mark glyphs always from the Thai font
        auto* th = static_cast<const stbtt_fontinfo*>(thai_info_);
        if (int tg = stbtt_FindGlyphIndex(th, (int)cp)) { info = th; gi = tg; scale = thai_scale_; }
    }
    if (gi == 0) gi = stbtt_FindGlyphIndex(info, (int)cp);
    if (gi == 0 && fb_info_) {"""),
    ("""float Font::text_width(const std::string& s) const {
    float w = 0;""",
     """float Font::text_width(const std::string& s0) const {
    const std::string s = thai_shape(s0);
    float w = 0;"""),
    ("""    if (text_width(s) <= max_w) return s;
    float ell = text_width("...");""",
     """    const std::string s = thai_shape(s0);
    if (text_width(s) <= max_w) return s;
    float ell = text_width("...");"""),
    ("""void Renderer::text(const Font& f, const std::string& s, float x, float y, Color c) {""",
     """void Renderer::text(const Font& f, const std::string& s0, float x, float y, Color c) {
    const std::string s = thai_shape(s0);"""),
])
# ellipsize's parameter is renamed so the shaped copy can take the old name
p = os.path.join(R, "src/gfx.cpp")
s = open(p, encoding="utf-8").read()
s, n = re.subn(r"std::string Font::ellipsize\(const std::string& s, float max_w\) const \{",
               "std::string Font::ellipsize(const std::string& s0, float max_w) const {", s)
assert n == 1, "ellipsize signature"
s = s.replace('#include "gfx.h"', '#include "gfx.h"\nextern "C" {\n#include "text_thai.h"\n}', 1)
assert 'extern "C"' in s
open(p, "w", encoding="utf-8").write(s)

# ---- fonts: load the Thai one too ----
patch("src/ui.cpp", [
    ("""    font_title_ = gfx::Font::load(ttf, 34 * s, cjk);
    font_body_  = gfx::Font::load(ttf, 22 * s, cjk);
    font_small_ = gfx::Font::load(ttf, 18 * s, cjk);""",
     """    // Thai (Brick OS): data/fonts/BrickOSThai.ttf, IBM Plex Sans Thai with pre-placed marks.
    std::string thai;
    for (const std::string& c : {platform::exe_dir() + "/data/fonts/BrickOSThai.ttf", std::string("data/fonts/BrickOSThai.ttf")})
        if (std::ifstream(c).good()) { thai = c; break; }
    font_title_ = gfx::Font::load(ttf, 34 * s, cjk, thai);
    font_body_  = gfx::Font::load(ttf, 22 * s, cjk, thai);
    font_small_ = gfx::Font::load(ttf, 18 * s, cjk, thai);"""),
])

# ---- keyboard: UTF-8 keys, Thai Kedmanee layout, language key ----
KB_TH = r'''// Brick OS: Thai on the standard Kedmanee layout (TIS 820-2538), as on Thai PC and phone
// keyboards. A lone mark is shown on a dotted circle (label) and types the mark (text).
#define TK(t) {t, KCHAR, 0, 1, t}
#define TM(t) {"\xE2\x97\x8C" t, KCHAR, 0, 1, t}
const std::vector<std::vector<Key>> KB_TH = {
    {TK("ๅ"),TK("/"),TK("-"),TK("ภ"),TK("ถ"),TM("ุ"),TM("ึ"),TK("ค"),TK("ต"),TK("จ"),TK("ข"),TK("ช")},
    {TK("ๆ"),TK("ไ"),TK("ำ"),TK("พ"),TK("ะ"),TM("ั"),TM("ี"),TK("ร"),TK("น"),TK("ย"),TK("บ"),TK("ล"),TK("ฃ")},
    {TK("ฟ"),TK("ห"),TK("ก"),TK("ด"),TK("เ"),TM("้"),TM("่"),TK("า"),TK("ส"),TK("ว"),TK("ง")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},
     TK("ผ"),TK("ป"),TK("แ"),TK("อ"),TM("ิ"),TM("ื"),TK("ท"),TK("ม"),TK("ใ"),TK("ฝ"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',4},{"-",KCHAR,'-',1},{"\xE0\xB8\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
const std::vector<std::vector<Key>> KB_TH_SHIFT = {
    {TK("+"),TK("๑"),TK("๒"),TK("๓"),TK("๔"),TM("ู"),TK("฿"),TK("๕"),TK("๖"),TK("๗"),TK("๘"),TK("๙")},
    {TK("๐"),TK("\""),TK("ฎ"),TK("ฑ"),TK("ธ"),TM("ํ"),TM("๊"),TK("ณ"),TK("ฯ"),TK("ญ"),TK("ฐ"),TK(","),TK("ฅ")},
    {TK("ฤ"),TK("ฆ"),TK("ฏ"),TK("โ"),TK("ฌ"),TM("็"),TM("๋"),TK("ษ"),TK("ศ"),TK("ซ"),TK(".")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},
     TK("("),TK(")"),TK("ฉ"),TK("ฮ"),TM("ฺ"),TM("์"),TK("?"),TK("ฒ"),TK("ฬ"),TK("ฦ"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',4},{"-",KCHAR,'-',1},{"\xE0\xB8\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
#undef TK
#undef TM
'''
patch("src/ui.cpp", [
    ("enum KType { KCHAR, KSPACE, KDEL, KSUBMIT, KSHIFT, KLEFT, KRIGHT };\nstruct Key { const char* label; KType type; char ch; int span; };",
     "enum KType { KCHAR, KSPACE, KDEL, KSUBMIT, KSHIFT, KLEFT, KRIGHT, KLANG };\n"
     "// text: what a KCHAR key types when it isn't the one ASCII ch (Brick OS: Thai keys, UTF-8)\n"
     "struct Key { const char* label; KType type; char ch; int span; const char* text = nullptr; };"),
    ("""     {"space",KSPACE,' ',4},{"-",KCHAR,'-',1},{"_",KCHAR,'_',1},
     {"Search",KSUBMIT,0,2}},
};""",
     """     {"space",KSPACE,' ',4},{"-",KCHAR,'-',1},{"\\xE0\\xB8\\x81/A",KLANG,0,1},   // Brick OS: ก/A
     {"Search",KSUBMIT,0,2}},
};
""" + KB_TH),
])

# every place that picks the layout
p = os.path.join(R, "src/ui.cpp")
s = open(p, encoding="utf-8").read()
n = s.count("const auto& kb = kb_numeric() ? KB_NUM : KB;")
assert n == 3, f"layout picks: {n}"
s = s.replace("const auto& kb = kb_numeric() ? KB_NUM : KB;",
              "const auto& kb = kb_numeric() ? KB_NUM : kb_thai_ ? (kb_shift_ ? KB_TH_SHIFT : KB_TH) : KB;")

# typing: whole UTF-8 key text; backspace / left / right by character
s = s.replace("""        case KCHAR: {
            char c = k.ch;
            if (kb_shift_ && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            query_input_.insert(query_input_.begin() + kb_caret_, c);
            kb_caret_++;
            kb_shift_ = false;                       // one-shot capital
            break;
        }""", """        case KCHAR: {
            std::string t = k.text ? std::string(k.text) : std::string(1, k.ch);
            if (kb_shift_ && t.size() == 1 && t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 'a' + 'A');
            query_input_.insert((size_t)kb_caret_, t);
            kb_caret_ += (int)t.size();
            kb_shift_ = false;                       // one-shot capital (Thai: one-shot SHIFT layer)
            break;
        }
        case KLANG:   kb_thai_ = !kb_thai_; kb_shift_ = false; break;""", 1)
s = s.replace("""        case KLEFT:   if (kb_caret_ > 0) kb_caret_--; break;
        case KRIGHT:  if (kb_caret_ < (int)query_input_.size()) kb_caret_++; break;""",
              """        case KLEFT:   while (kb_caret_ > 0 && ((unsigned char)query_input_[--kb_caret_] & 0xC0) == 0x80) {} break;
        case KRIGHT:  if (kb_caret_ < (int)query_input_.size()) {
                          kb_caret_++;
                          while (kb_caret_ < (int)query_input_.size() &&
                                 ((unsigned char)query_input_[kb_caret_] & 0xC0) == 0x80) kb_caret_++;
                      }
                      break;""", 1)
s = s.replace("""    if (kb_caret_ > 0) { query_input_.erase(query_input_.begin() + (kb_caret_-1)); kb_caret_--; }""",
              """    if (kb_caret_ > 0) {   // one whole UTF-8 character
        int b = kb_caret_ - 1;
        while (b > 0 && ((unsigned char)query_input_[b] & 0xC0) == 0x80) b--;
        query_input_.erase((size_t)b, (size_t)(kb_caret_ - b));
        kb_caret_ = b;
    }""", 1)

# caret window: step by whole characters
s = s.replace("""    while (start < caret &&
           font_body_->text_width(query_input_.substr(start, caret - start)) > avail)
        start++;
    std::string shown;   // extend from start while it fits
    for (int i = start; i < csz; ++i) {
        std::string cand = shown; cand += query_input_[i];
        if (font_body_->text_width(cand) > avail) break;
        shown = std::move(cand);
    }""", """    auto next_char = [&](int i) { i++; while (i < csz && ((unsigned char)query_input_[i] & 0xC0) == 0x80) i++; return i; };
    while (start < caret &&
           font_body_->text_width(query_input_.substr(start, caret - start)) > avail)
        start = next_char(start);
    std::string shown;   // extend from start while it fits, a whole character at a time
    for (int i = start; i < csz;) {
        int j = next_char(i);
        std::string cand = shown + query_input_.substr(i, j - i);
        if (font_body_->text_width(cand) > avail) break;
        shown = std::move(cand);
        i = j;
    }""", 1)

# up/down: rows are drawn centered, so the spatial match has to include each row's indent
s = s.replace("""        auto xcenter = [&](int row, int col) {
            float x = 0; for (int j = 0; j < col; ++j) x += kb[row][j].span;
            return x + kb[row][col].span / 2.f;
        };
        auto col_at = [&](int row, float x) {
            float acc = 0;""", """        int units = 0;   // Brick OS: rows of different widths are centered (Thai Kedmanee)
        for (const auto& r : kb) { int u = 0; for (const auto& k : r) u += k.span; units = std::max(units, u); }
        auto indent = [&](int row) {
            int u = 0; for (const auto& k : kb[row]) u += k.span;
            return (units - u) / 2.f;
        };
        auto xcenter = [&](int row, int col) {
            float x = indent(row); for (int j = 0; j < col; ++j) x += kb[row][j].span;
            return x + kb[row][col].span / 2.f;
        };
        auto col_at = [&](int row, float x) {
            float acc = indent(row);""", 1)

# key width: rows total 10 units in English, up to 13 in Thai
s = s.replace("""    float unit = numeric ? keyh                    // square digit keys
                         : (W * 0.94f - 9*gap) / 10.f;   // qwerty: every row totals 10 units""",
              """    int units = 0;   // the widest row (English rows total 10 units; Thai Kedmanee up to 13)
    for (const auto& row : kb) { int u = 0; for (const auto& k : row) u += k.span; units = std::max(units, u); }
    float unit = numeric ? keyh                    // square digit keys
                         : (W * 0.94f - (units - 1)*gap) / (float)units;""", 1)
s = s.replace("""                std::string lbl = (k.type == KSUBMIT) ? i18n::tr(i18n::Str::KbEnter) : k.label;""",
              """                std::string lbl = (k.type == KSUBMIT) ? i18n::tr(i18n::Str::KbEnter) : k.label;
                if (k.type == KLANG && kb_thai_) lbl = "A/\\xE0\\xB8\\x81";""", 1)
open(p, "w", encoding="utf-8").write(s)

patch("src/ui.h", [
    ("    bool kb_shift_ = false;    // one-shot uppercase for the next letter",
     "    bool kb_shift_ = false;    // one-shot uppercase for the next letter\n"
     "    bool kb_thai_ = false;     // Brick OS: Thai Kedmanee layout instead of QWERTY"),
])

# ---- Thai UI language ----
shutil.copy(os.path.join(HERE, "th.json"), os.path.join(R, "data/lang/th.json"))
lp = os.path.join(R, "data/lang/languages.json")
L = json.load(open(lp, encoding="utf-8"))
if not any(l["code"] == "th" for l in L["languages"]):
    L["languages"].append({"code": "th", "name": "ไทย", "hl": "th", "gl": "TH"})
json.dump(L, open(lp, "w", encoding="utf-8"), ensure_ascii=False, indent=2)

os.makedirs(os.path.join(R, "data/fonts"), exist_ok=True)
shutil.copy(os.path.join(HERE, "BrickOSThai.ttf"), os.path.join(R, "data/fonts/BrickOSThai.ttf"))
print("patched", R)
