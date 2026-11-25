#!/usr/bin/env python3
"""Patch libass 0.14: characters no selected font has come from the fonts of YTC_SUB_FONTS (colon-separated paths, in
order). Built without fontconfig, libass has no fallback of its own. Usage: libass_fallback.py <libass source dir>"""
import sys
f = sys.argv[1] + '/libass/ass_font.c'
s = open(f).read()
a = '/**\n * ' + chr(92) + 'brief Select a face with the given charcode and add it to ASS_Font\n'
b = '    if (!path)\n        return -1;\n\n    for (i = 0; i < font->n_faces; i++) {'
assert s.count(a) == 1 and s.count(b) == 1
s = s.replace(a, r"""// Brick OS: the first font of YTC_SUB_FONTS that has ch (the first that opens for ch 0)
static int add_fallback_face(ASS_Font *font, uint32_t ch)
{
    const char *p = getenv("YTC_SUB_FONTS");
    for (int uid = -2; p && *p; uid--) {
        char path[1024];
        size_t n = strcspn(p, ":");
        int used = 0;
        FT_Face face;
        for (int i = 0; i < font->n_faces; i++)
            used |= font->faces_uid[i] == uid;
        if (!used && n && n < sizeof(path)) {
            memcpy(path, p, n);
            path[n] = 0;
            if (!FT_New_Face(font->ftlibrary, path, 0, &face)) {   // with FreeType's charmap: full Unicode when there is one
                if (!ch || FT_Get_Char_Index(face, ass_font_index_magic(face, ch))) {
                    buggy_font_workaround(face);
                    if (font->n_faces) {   // the same em size as the first face: libass sizes a font by its line height
                        FT_Face f0 = font->faces[0];
                        face->ascender = FT_MulDiv(f0->ascender, face->units_per_EM, f0->units_per_EM);
                        face->descender = FT_MulDiv(f0->descender, face->units_per_EM, f0->units_per_EM);
                    }
                    font->faces[font->n_faces] = face;
                    font->faces_uid[font->n_faces++] = uid;
                    ass_face_set_size(face, font->size);
                    return font->n_faces - 1;
                }
                FT_Done_Face(face);
            }
        }
        p += n + (p[n] == ':');
    }
    return -1;
}

""" + a)
s = s.replace(b, b.replace('return -1;', 'return add_fallback_face(font, ch);'))
open(f, 'w').write(s)
