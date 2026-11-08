#!/usr/bin/env python3
# YTC_SEARCHSHOT: also shoot the autocomplete chips, a focused chip, and the Thai layout.
import pathlib, sys
p = pathlib.Path("src/main_ui.cpp"); t = p.read_text()
old = """            app.render(rn);
            win->screenshot(std::string(shot) + "_kb_typed.png");
"""
new = old + """            for (int w = 0; w < 3000; w += 50) { app.pump_async(); SDL_Delay(50); }
            app.render(rn); win->screenshot(std::string(shot) + "_kb_sugg.png");
            for (int i = 0; i < 6; ++i) app.input(ui::App::Action::Up);
            app.input(ui::App::Action::Right);
            app.render(rn); win->screenshot(std::string(shot) + "_kb_suggfocus.png");
            app.input(ui::App::Action::Down);
            app.kb_pad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            app.render(rn); win->screenshot(std::string(shot) + "_kb_thai.png");
"""
if t.count(old) != 1: sys.exit("anchor")
p.write_text(t.replace(old, new)); print("ok")
