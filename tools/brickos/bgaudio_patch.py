#!/usr/bin/env python3
"""Brick OS screen-off listening for YTC. With YTC_MEDIA_FLAG set, YTC keeps that file
present while a video plays (not paused), so the system power-button handler (sleepmon)
turns the screen off instead of sleeping. With YTC_SCREEN_OFF_FLAG set, YTC watches that
file: while it exists the screen is off, so YTC ignores input and stops decoding video
(audio keeps playing). Run from the ytc checkout root."""
import sys
from pathlib import Path


def edit(path, pairs):
    p = Path(path)
    s = p.read_text()
    for old, new in pairs:
        n = s.count(old)
        if n != 1:
            sys.exit(f"{path}: expected 1 of {old[:70]!r}, found {n}")
        s = s.replace(old, new)
    p.write_text(s)


if "set_video_enabled" in Path("src/player.h").read_text():
    sys.exit("already patched")

edit("src/player.h", [
    ("""    void set_speed(double mult);""",
     """    void set_speed(double mult);
    // Decode and show video (true) or play audio only (false), live.
    void set_video_enabled(bool on);"""),
])
edit("src/player.cpp", [
    ("""void Player::add_subtitle(const std::string& path, const std::string& font) {""",
     """void Player::set_video_enabled(bool on) {
    if (!impl_->mpv) return;
    mpv_set_property_string(impl_->mpv, "vid", on ? "auto" : "no");
}
void Player::add_subtitle(const std::string& path, const std::string& font) {"""),
])
edit("src/ui.h", [
    ("""    bool hwdec_probed_this_video_ = false;""",
     """    bool media_flag_on_ = false;    // YTC_MEDIA_FLAG file present (a video is playing)
    bool screen_dark_ = false;      // YTC_SCREEN_OFF_FLAG file present (screen is off)
    bool hwdec_probed_this_video_ = false;"""),
])
edit("src/ui.cpp", [
    ("""#include <SDL.h>
#include <algorithm>""", """#include <SDL.h>
#include <unistd.h>
#include <algorithm>"""),
    ("""App::~App() {
    cast_events_run_ = false;""", """App::~App() {
    if (media_flag_on_) unlink(getenv("YTC_MEDIA_FLAG"));
    cast_events_run_ = false;"""),
    ("""void App::input(Action a) {
    // Resume prompt consumes input while open (a pre-playback modal).""",
     """void App::input(Action a) {
    if (screen_dark_) return;   // screen off (power tap): the buttons do nothing until it wakes
    // Resume prompt consumes input while open (a pre-playback modal)."""),
    ("""void App::pump_async() {
    thumbs_.pump();""", """void App::pump_async() {
    // Screen-off listening (Brick OS): flag "a video is playing" for the power button
    // handler, and play audio only while it has the screen off.
    if (const char* mf = getenv("YTC_MEDIA_FLAG")) {
        bool on = mode_ == Mode::Playing && player_.active() && !player_.paused();
        if (on != media_flag_on_) {
            media_flag_on_ = on;
            if (on) { if (FILE* f = std::fopen(mf, "w")) std::fclose(f); }
            else unlink(mf);
        }
    }
    if (const char* sf = getenv("YTC_SCREEN_OFF_FLAG")) {
        bool dark = access(sf, F_OK) == 0;
        if (dark != screen_dark_) { screen_dark_ = dark; player_.set_video_enabled(!dark); }
    }
    thumbs_.pump();"""),
])
print("patched")
