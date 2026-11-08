#!/usr/bin/env python3
"""Brick OS look for YTC: iOS-style dark palette, rounded cards/thumbnails/menus,
pill tabs with a white selection, translated menu headings, and a resolution
ceiling (YTC_QUALITY_CEIL) so the Max Quality setting cannot pick what the CPU
cannot decode. Run from the ytc checkout root. Idempotent: refuses to run twice."""
import sys
from pathlib import Path


def edit(path, pairs, count=1):
    p = Path(path)
    s = p.read_text()
    for pair in pairs:
        old, new = pair[0], pair[1]
        want = pair[2] if len(pair) > 2 else count
        n = s.count(old)
        if n != want:
            sys.exit(f"{path}: expected {want} of {old[:70]!r}, found {n}")
        s = s.replace(old, new)
    p.write_text(s)


if "round_rect" in Path("src/gfx.h").read_text():
    sys.exit("already patched")

# --- gfx: rounded rectangles from an anti-aliased disc mask -------------------
edit("src/gfx.h", [
    ("""    void textured_quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3,
                        const Texture& t, Color tint = {});
    void end();                            // flush
private:
""", """    void textured_quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3,
                        const Texture& t, Color tint = {});
    // Filled rectangle with rounded corners (radius clamps to half the shorter side).
    void round_rect(const Rect& r, float radius, Color c);
    // Paint the corners of r OUTSIDE a radius in `outside`: rounds an image already
    // drawn over a solid background. corners: 1 TL, 2 TR, 4 BR, 8 BL.
    void round_corners(const Rect& r, float radius, Color outside, int corners = 15);
    void end();                            // flush
private:
    void corner(const Rect& r, float radius, int which, const Texture& t, Color c);
    std::unique_ptr<Texture> disc_, notch_;   // 128px circle coverage and its inverse
"""),
])

edit("src/gfx.cpp", [
    ("""    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
Renderer::~Renderer() {""", """    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Corner masks: white with alpha = coverage of a radius-64 circle centred in a
    // 128px square (disc) and its complement (notch); each corner samples a quadrant.
    std::vector<uint8_t> disc(128 * 128 * 4), notch(128 * 128 * 4);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            float dx = x + 0.5f - 64, dy = y + 0.5f - 64;
            float cov = 64.5f - std::sqrt(dx * dx + dy * dy);
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            uint8_t* d = &disc[(y * 128 + x) * 4];
            uint8_t* n = &notch[(y * 128 + x) * 4];
            d[0] = d[1] = d[2] = n[0] = n[1] = n[2] = 255;
            d[3] = (uint8_t)(cov * 255 + 0.5f);
            n[3] = (uint8_t)(255 - d[3]);
        }
    disc_ = Texture::from_rgba(disc.data(), 128, 128);
    notch_ = Texture::from_rgba(notch.data(), 128, 128);
}
Renderer::~Renderer() {"""),
    ("""void Renderer::text(const Font& f, const std::string& s0, float x, float y, Color c) {""",
     """void Renderer::corner(const Rect& r, float rad, int which, const Texture& t, Color c) {
    // which: 0 TL, 1 TR, 2 BR, 3 BL — the matching quadrant of the 128px mask.
    float x = (which == 1 || which == 2) ? r.x + r.w - rad : r.x;
    float y = (which >= 2) ? r.y + r.h - rad : r.y;
    float u0 = (which == 1 || which == 2) ? 0.5f : 0.f, v0 = (which >= 2) ? 0.5f : 0.f;
    float u1 = u0 + 0.5f, v1 = v0 + 0.5f;
    set_texture(t.id());
    push(x, y, u0,v0, c);         push(x+rad, y, u1,v0, c);     push(x+rad, y+rad, u1,v1, c);
    push(x, y, u0,v0, c);         push(x+rad, y+rad, u1,v1, c); push(x, y+rad, u0,v1, c);
}
void Renderer::round_rect(const Rect& r, float rad, Color c) {
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    if (rad < 1 || !disc_) { quad(r, c); return; }
    quad({r.x + rad, r.y, r.w - 2*rad, r.h}, c);
    quad({r.x, r.y + rad, rad, r.h - 2*rad}, c);
    quad({r.x + r.w - rad, r.y + rad, rad, r.h - 2*rad}, c);
    for (int k = 0; k < 4; ++k) corner(r, rad, k, *disc_, c);
}
void Renderer::round_corners(const Rect& r, float rad, Color outside, int corners) {
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    if (rad < 1 || !notch_) return;
    for (int k = 0; k < 4; ++k)
        if (corners & (1 << k)) corner(r, rad, k, *notch_, outside);
}
void Renderer::text(const Font& f, const std::string& s0, float x, float y, Color c) {"""),
])
g = Path("src/gfx.cpp").read_text()
if "#include <cmath>" not in g:
    Path("src/gfx.cpp").write_text("#include <cmath>\n#include <algorithm>\n" + g)

# --- palette -------------------------------------------------------------------
edit("src/ui.h", [
    ("""    gfx::Color bg        = gfx::Color::rgb(0x0f1116);
    gfx::Color panel     = gfx::Color::rgb(0x181b22);
    gfx::Color card      = gfx::Color::rgb(0x1e222b);
    gfx::Color card_sel  = gfx::Color::rgb(0x2b3140);
    gfx::Color accent    = gfx::Color::rgb(0xff3b30); // YouTube-ish red
    gfx::Color text       = gfx::Color::rgb(0xf2f4f8);
    gfx::Color text_dim  = gfx::Color::rgb(0x9aa3b2);
    gfx::Color thumb_bg  = gfx::Color::rgb(0x0a0c10);
""", """    // Brick OS: iOS-style dark (system background / grouped cells / labels).
    gfx::Color bg        = gfx::Color::rgb(0x000000);
    gfx::Color panel     = gfx::Color::rgb(0x1c1c1e);
    gfx::Color card      = gfx::Color::rgb(0x1c1c1e);
    gfx::Color card_sel  = gfx::Color::rgb(0x3a3a3c);
    gfx::Color accent    = gfx::Color::rgb(0xff3b30); // YouTube-ish red
    gfx::Color text       = gfx::Color::rgb(0xffffff);
    gfx::Color text_dim  = gfx::Color::rgb(0x98989f);
    gfx::Color thumb_bg  = gfx::Color::rgb(0x2c2c2e);
    gfx::Color ring      = gfx::Color::rgb(0xffffff); // focus ring / active tab
    gfx::Color chip      = gfx::Color::rgb(0x2c2c2e); // idle tab
"""),
    ("""    int quality_override_ = -1;     // max_height for THIS video (0 = Auto)
""", """    int quality_override_ = -1;     // max_height for THIS video (0 = Auto)
    int quality_ceil_ = 0;          // YTC_QUALITY_CEIL: highest choosable height (0 = none)
"""),
])

# --- ui.cpp ----------------------------------------------------------------------
edit("src/ui.cpp", [
    # quality ceiling
    ("""        int def = mh ? atoi(mh) : 1080;
        play_prefs_.max_height = it_.setting_int("max_height", def);
    }
""", """        int def = mh ? atoi(mh) : 1080;
        play_prefs_.max_height = it_.setting_int("max_height", def);
        // A device ceiling (Brick OS: 720 on the A133P) clamps a saved choice too.
        if (const char* c = getenv("YTC_QUALITY_CEIL")) quality_ceil_ = atoi(c);
        if (quality_ceil_ > 0 &&
            (play_prefs_.max_height <= 0 || play_prefs_.max_height > quality_ceil_))
            play_prefs_.max_height = quality_ceil_;
    }
"""),
    ("""        static const int steps[] = {360, 480, 720, 1080, 1440, 2160, 0 /* Auto */};
        int n = (int)(sizeof(steps)/sizeof(steps[0]));
""", """        static const int all_steps[] = {360, 480, 720, 1080, 1440, 2160, 0 /* Auto */};
        int steps[7], n = 0;
        for (int q : all_steps)   // the device ceiling drops what it cannot decode (and Auto)
            if (quality_ceil_ <= 0 || (q > 0 && q <= quality_ceil_)) steps[n++] = q;
"""),
    # thumbnail duration pill
    ("""            rn.quad(pr, gfx::Color{0,0,0,0.75f*alpha});""",
     """            rn.round_rect(pr, 6*s, gfx::Color{0,0,0,0.75f*alpha});"""),
    # header: no bar, no red rule
    ("""    rn.quad({0, hy, (float)W, hbar}, theme_.panel);
    rn.quad({0, hy+hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, "YTC", 32*s, hy+24*s, theme_.text);""",
     """    rn.text(*font_title_, "YTC", 32*s, hy+24*s, theme_.text);"""),
    # tabs: pills, white when active
    ("""        rn.quad({0, ty0, (float)W, tm.tabs_h}, theme_.panel.with_a(0.6f));
""", ""),
    ("""            if (st) rn.quad(chip, theme_.accent);
            rn.text(*font_body_, label, tx + 18 * s,
                    ty0 + (tm.tabs_h - font_body_->line_height()) / 2 + 2 * s,
                    st ? theme_.text : theme_.text_dim);""",
     """            rn.round_rect(chip, chip_h / 2, st ? theme_.ring : theme_.chip);
            rn.text(*font_body_, label, tx + 18 * s,
                    ty0 + (tm.tabs_h - font_body_->line_height()) / 2 + 2 * s,
                    st ? theme_.bg : theme_.text);"""),
    # grid cards
    ("""        if (sel) rn.quad({x-4*s, y-4*s, cardw+8*s, cardh+8*s}, theme_.accent);
        rn.quad(card, theme_.card);

        const auto& v = results_[i];
        draw_thumb(rn, v, {x, y, cardw, thumbh}, s, 1.0f);
""", """        float rad = 14*s;
        if (sel) rn.round_rect({x-5*s, y-5*s, cardw+10*s, cardh+10*s}, rad+5*s, theme_.ring);
        rn.round_rect(card, rad, theme_.card);

        const auto& v = results_[i];
        draw_thumb(rn, v, {x, y, cardw, thumbh}, s, 1.0f);
        rn.round_corners({x, y, cardw, thumbh}, rad, sel ? theme_.ring : theme_.bg, 1|2);
"""),
    # menu panel + rows
    ("""    rn.quad({px-24*s, py-24*s, iw+48*s, ph+48*s}, theme_.panel);
    rn.quad({px-24*s, py-24*s, iw+48*s, 4*s}, theme_.accent);""",
     """    rn.round_rect({px-24*s, py-24*s, iw+48*s, ph+48*s}, 24*s, theme_.panel);""", 3),
    ("""        rn.quad({px, iy, iw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);""",
     """        rn.round_rect({px, iy, iw, ih}, 12*s, sel ? theme_.card_sel : theme_.card);""", 3),
    ("""    std::string heading = (menu_kind_ == MenuKind::Main) ? "Menu"
                        : (menu_kind_ == MenuKind::Settings) ? "Settings\"""",
     """    std::string heading = (menu_kind_ == MenuKind::Main) ? std::string("YTC")
                        : (menu_kind_ == MenuKind::Settings) ? std::string(i18n::tr(i18n::Str::Settings))"""),
])
edit("src/ui.cpp", [
    ("""std::string("Settings \\xE2\\x80\\xA3 ")""",
     """(std::string(i18n::tr(i18n::Str::Settings)) + " \\xE2\\x80\\xA3 ")"""),
], count=6)
print("patched")

# --- search keyboard -------------------------------------------------------------
edit("src/ui.cpp", [
    ("""    rn.quad({0, 0, (float)W, hbar}, theme_.panel);
    rn.quad({0, hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, kb_title_, 32*s, 24*s, theme_.text);""",
     """    rn.text(*font_title_, kb_title_, 32*s, 24*s, theme_.text);"""),
    ("""    rn.quad({bx, by, bw, bh}, theme_.card);
    rn.quad({bx, by, 4*s, bh}, theme_.accent);""",
     """    rn.round_rect({bx, by, bw, bh}, 14*s, theme_.chip);"""),
    ("""                          : (k.type == KSUBMIT) ? theme_.accent : theme_.card;
            rn.quad(kr, bg);""",
     """                          : (k.type == KSUBMIT) ? theme_.accent : theme_.chip;
            rn.round_rect(kr, 10*s, bg);"""),
])
print("keyboard patched")
