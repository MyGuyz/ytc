#!/usr/bin/env python3
"""Brick OS: YouTube's sound in the background (DESIGN.md 7.5c). Adds "ytc --bg <request>" (src/bgaudio.cpp, copied
from this folder), the player menu's "Listen in background" tile, Settings > Audio > "Keep playing after exit", the
request YTC writes when it hands the clip over, and YTC_RESUME_ID (launch.sh) to reopen the clip where the sound got
to. Run from the ytc checkout root."""
import shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def edit(path, pairs):
    p = Path(path)
    s = p.read_text(encoding="utf-8")
    for old, new in pairs:
        n = s.count(old)
        if n != 1:
            sys.exit(f"{path}: expected 1 of {old[:70]!r}, found {n}")
        s = s.replace(old, new)
    p.write_text(s, encoding="utf-8")


if "exit_to_background" in Path("src/ui.h").read_text(encoding="utf-8"):
    sys.exit("already patched")

for f in ("bgaudio.cpp", "bgaudio.h"):
    shutil.copy(HERE / f, Path("src") / f)

edit("CMakeLists.txt", [
    ("src/player.cpp src/stream.cpp", "src/player.cpp src/bgaudio.cpp src/stream.cpp"),
])

edit("src/main_ui.cpp", [
    ('#include "remux.h"\n', '#include "remux.h"\n#include "bgaudio.h"\n'),
    ("    SDL_SetMainReady();   // pairs with SDL_MAIN_HANDLED\n",
     "    SDL_SetMainReady();   // pairs with SDL_MAIN_HANDLED\n"
     "    if (argc > 2 && std::string(argv[1]) == \"--bg\") return bgaudio_main(argv[2]);   // Brick OS: sound only\n"),
    ("        app.load_home();          // always start on Home (favorite-channel RSS; may be empty)\n    }\n",
     "        app.load_home();          // always start on Home (favorite-channel RSS; may be empty)\n    }\n"
     "    // Brick OS: the sound was playing in the background; reopen its clip where it got to\n"
     "    if (const char* rid = std::getenv(\"YTC_RESUME_ID\")) app.resume_background(rid);\n"),
    ("    SDL_StopTextInput();\n    if (pad) SDL_GameControllerClose(pad);\n    return 0;\n}",
     "    app.exit_to_background(false);   // Brick OS: \"Keep playing after exit\"\n"
     "    SDL_StopTextInput();\n    if (pad) SDL_GameControllerClose(pad);\n    return 0;\n}"),
])

edit("src/ui.h", [
    ("        std::string status, video_url, audio_url, user_agent, title, description;\n",
     "        std::string status, video_url, audio_url, user_agent, title, description, author;\n"),
    ("ToggleSponsorBlock, CycleCaptions, ToggleAutoplay,",
     "ToggleSponsorBlock, CycleCaptions, ToggleAutoplay, ListenBackground, ToggleBgOnExit,"),
    ('    bool autoplay_ = false;                // setting "autoplay" (default off)\n',
     '    bool autoplay_ = false;                // setting "autoplay" (default off)\n'
     '    bool bg_on_exit_ = true;               // setting "bg_on_exit": quitting during a clip keeps its sound\n'
     '    std::string playing_audio_url_, playing_video_url_, playing_ua_, playing_author_;   // for the hand-over\n'
     '    bool bg_handed_ = false;               // the sound went to "ytc --bg"\n'),
    ("    bool wants_quit() const { return quit_requested_; }     // set by the menu's Exit item\n",
     "    bool wants_quit() const { return quit_requested_; }     // set by the menu's Exit item\n"
     "    // Brick OS: hand the clip's sound to \"ytc --bg\" (a request in /tmp/ytbg) and stop the player. forced = the\n"
     "    // Listen-in-background tile; otherwise only while a clip plays and the setting is on.\n"
     "    bool exit_to_background(bool forced);\n"
     "    void resume_background(const std::string& video_id);   // reopen the clip the sound came from\n"),
])

edit("src/ui.cpp", [
    ("#include <unistd.h>\n", "#include <unistd.h>\n#include <sys/stat.h>\n"),
    ('    autoplay_ = it_.setting_int("autoplay", 0) != 0;          // default OFF\n',
     '    autoplay_ = it_.setting_int("autoplay", 0) != 0;          // default OFF\n'
     '    bg_on_exit_ = it_.setting_int("bg_on_exit", 1) != 0;      // default ON\n'),
    ("    player_.set_volume(volume_);                 // apply the app-local volume level\n",
     "    player_.set_volume(volume_);                 // apply the app-local volume level\n"
     "    playing_audio_url_ = r.audio_url; playing_video_url_ = r.video_url; playing_ua_ = r.user_agent;\n"
     "    playing_author_ = r.author;\n"),
    ("        r.description = info.description;\n", "        r.description = info.description;\n        r.author = info.author;\n"),
    ("""    menu_items_.push_back({tr(S::SetAudioLang) + std::string(":  ") + lang_pref_label(audio_lang_pref_),
                           MenuAction::CycleAudioLang});
""", """    menu_items_.push_back({tr(S::SetAudioLang) + std::string(":  ") + lang_pref_label(audio_lang_pref_),
                           MenuAction::CycleAudioLang});
    menu_items_.push_back({tr(S::SetBgOnExit) + std::string(":  ") + tr(bg_on_exit_ ? S::On : S::Off),
                           MenuAction::ToggleBgOnExit});
"""),
    ("    if (a == MenuAction::ToggleAutoplay) {\n",
     "    if (a == MenuAction::ToggleBgOnExit) {\n"
     "        bg_on_exit_ = !bg_on_exit_;\n"
     "        it_.set_setting_int(\"bg_on_exit\", bg_on_exit_ ? 1 : 0);\n"
     "        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;\n"
     "        return;\n"
     "    }\n"
     "    if (a == MenuAction::ToggleAutoplay) {\n"),
    ("case A::ToggleAutoplay: case A::ToggleFeedFavorites:",
     "case A::ToggleAutoplay: case A::ToggleBgOnExit: case A::ToggleFeedFavorites:"),
    ("""        if (mode_ == Mode::Playing)
            menu_items_.push_back({tr(S::SetPlaybackMenu), MenuAction::GoPlayback});
""", """        if (mode_ == Mode::Playing)   // Brick OS 7.5c: the sound goes on after YouTube closes
            menu_items_.push_back({tr(S::ListenBackground), MenuAction::ListenBackground});
        if (mode_ == Mode::Playing)
            menu_items_.push_back({tr(S::SetPlaybackMenu), MenuAction::GoPlayback});
"""),
    ("""        case MenuAction::Quit:
            menu_open_ = false;
            menu_paused_ = false;
            quit_requested_ = true;
            return;
""", """        case MenuAction::Quit:
            menu_open_ = false;
            menu_paused_ = false;
            quit_requested_ = true;
            return;
        case MenuAction::ListenBackground:
            menu_open_ = false;
            menu_paused_ = false;
            if (exit_to_background(true)) quit_requested_ = true;
            return;
        case MenuAction::ToggleBgOnExit:
            adjust_setting(MenuAction::ToggleBgOnExit, 1);
            return;
"""),
    ("""void App::ensure_visible() {""", r"""// Brick OS 7.5c: the request "ytc --bg" plays (bgaudio.cpp). A live clip hands over its HLS manifest and a
// downloaded one its file, both played as they are (raw); otherwise the audio stream through ytc://.
bool App::exit_to_background(bool forced) {
    if (bg_handed_) return true;
    if (!forced && (!bg_on_exit_ || mode_ != Mode::Playing || player_.paused())) return false;
    if (!player_.active() || now_playing_item_.video_id.empty()) return false;
    const std::string& v = playing_video_url_;
    bool raw = v.find(".m3u8") != std::string::npos || v.find("/manifest/hls") != std::string::npos ||
               (!v.empty() && v.find("://") == std::string::npos);
    std::string src = (raw || playing_audio_url_.empty()) ? v : playing_audio_url_;
    if (src.empty()) return false;
    auto one_line = [](std::string s) { for (char& c : s) if (c == '\n' || c == '\r') c = ' '; return s; };
    std::string title = now_playing_title_.empty() ? now_playing_item_.title : now_playing_title_;
    mkdir("/tmp/ytbg", 0755);
    FILE* f = std::fopen("/tmp/ytbg/req", "w");
    if (!f) return false;
    std::fprintf(f, "audio=%s\nraw=%d\nua=%s\nstart=%.3f\nduration=%.3f\npaced=%d\nvolume=%d\n", src.c_str(),
                 raw ? 1 : 0, playing_ua_.c_str(), player_.position(), player_.duration(), playing_paced_ ? 1 : 0,
                 volume_);
    std::fprintf(f, "id=%s\ntitle=%s\nchannel=%s\nchannel_id=%s\nthumb_url=%s\n", now_playing_item_.video_id.c_str(),
                 one_line(title).c_str(), one_line(playing_author_.empty() ? now_playing_item_.author : playing_author_).c_str(),
                 now_playing_item_.channel_id.c_str(), now_playing_item_.thumbnail_url.c_str());
    std::fclose(f);
    if (!now_playing_item_.video_id.empty() && player_.position() > 5)
        it_.set_resume_pos(now_playing_item_.video_id, player_.position());
    player_.stop();
    bg_handed_ = true;
    return true;
}

void App::resume_background(const std::string& video_id) {
    std::ifstream in("/tmp/ytbg/last");
    std::string line;
    yt::SearchResult v;
    double pos = 0;
    bool ended = false;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), val = line.substr(eq + 1);
        if (k == "id") v.video_id = val;
        else if (k == "title") v.title = val;
        else if (k == "channel") v.author = val;
        else if (k == "channel_id") v.channel_id = val;
        else if (k == "thumb_url") v.thumbnail_url = val;
        else if (k == "pos_ms") pos = std::atof(val.c_str()) / 1000.0;
        else if (k == "ended") ended = val == "1";
    }
    if (v.video_id != video_id || ended) return;
    play_item(v, -1);
    resume_pos_ = pos;   // play_item started the resolve at 0; poll_resolve applies this
}

void App::ensure_visible() {"""),
])

edit("src/ui_bos_menu.cpp", [
    ("            case MenuAction::ToggleAutoplay: ic = \"repeat\"; col = 0x30D158; break;\n",
     "            case MenuAction::ToggleAutoplay: ic = \"repeat\"; col = 0x30D158; break;\n"
     "            case MenuAction::ListenBackground: ic = \"music\"; col = 0xFF375F; break;\n"),
])

edit("src/i18n.h", [
    ("SetAutoplay, SetSponsorBlock,", "SetAutoplay, ListenBackground, SetBgOnExit, SetSponsorBlock,"),
])
edit("src/i18n.cpp", [
    ('"SetAutoplay","SetSponsorBlock",', '"SetAutoplay","ListenBackground","SetBgOnExit","SetSponsorBlock",'),
    ('"Ask to Resume","Home Feed","Autoplay","SponsorBlock",',
     '"Ask to Resume","Home Feed","Autoplay","Listen in background","Keep playing after exit","SponsorBlock",'),
])
for lang, a, b in (("en", "Listen in background", "Keep playing after exit"),
                   ("th", "ฟังเบื้องหลัง", "ออกแล้วเล่นเสียงต่อ")):
    edit(f"data/lang/{lang}.json", [
        ('  "SetAutoplay": ', f'  "ListenBackground": "{a}",\n  "SetBgOnExit": "{b}",\n  "SetAutoplay": '),
    ])
print("ok")
