// Brick OS: YouTube's sound in the background ("ytc --bg <request file>"); see bgaudio.cpp.
#pragma once

// Plays the request's audio stream with no window until it ends or is told to stop.
int bgaudio_main(const char* request_file);
