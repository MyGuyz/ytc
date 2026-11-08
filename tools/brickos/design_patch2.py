#!/usr/bin/env python3
# Brick OS YTC part 2: the TV view (DESIGN.md 6.4) and X to follow a channel. Run in ~/ytc after design_patch.py.
import pathlib, sys

def sub(path, old, new, count=1):
    p = pathlib.Path(path); t = p.read_text()
    if t.count(old) != count:
        sys.exit(f"{path}: expected {count} of {old[:80]!r}, found {t.count(old)}")
    p.write_text(t.replace(old, new))

U = "src/ui.cpp"
sub("src/ui.h", "    void design_status_bar(gfx::Renderer& rn);\n",
    "    void design_status_bar(gfx::Renderer& rn);\n    void follow_selected();   // X in the TV view: follow / unfollow the tile's channel\n")
sub(U, "    if (a == Action::Sort && !query_.empty()) { toggle_search_sort(); return; }",
    "    if (a == Action::Sort && view_mode_ == ViewMode::Shelves) { follow_selected(); return; }   // X: ติดตามช่อง\n"
    "    if (a == Action::Sort && !query_.empty()) { toggle_search_sort(); return; }")
t = pathlib.Path(U).read_text()
# the design colours go above every user (the TV view comes before the keyboard in the file)
ca = t.index("// DESIGN.md colours (section 2)\nnamespace dzc {")
cb = t.index("}\n", t.index("red{", ca)) + 2
block = t[ca:cb]
t = t[:ca] + t[cb:]
anchor = "// ---------- On-screen keyboard layout ----------"
assert t.count(anchor) == 1
t = t.replace(anchor, block + "\n" + anchor)
a = t.index("void App::render_shelves(gfx::Renderer& rn) {")
b = t.index("void App::render_grid(gfx::Renderer& rn) {")
t = t[:a] + pathlib.Path("/mnt/g/trimui brick pro/scripts/ytc/render_shelves.cpp.txt").read_text() + "\n" + t[b:]
pathlib.Path(U).write_text(t)
print("design_patch2 ok")
