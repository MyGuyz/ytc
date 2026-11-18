// Brick OS: YouTube's sound in the background ("ytc --bg <request file>").
//
// YTC writes a request (key=value lines) and exits; its pak's launch.sh then starts
// this mode, detached, so the launcher comes back while the sound plays on. No window,
// no GL: libmpv plays the audio stream alone through the same ytc:// range fetcher.
//
// Files in BG_DIR (tmpfs), the contract with the music service (musicplayerd), which
// shows this as its "YouTube" source to every client (status bar, HOME panel, Music):
//   pid    this process
//   state  rewritten twice a second: state=playing|paused, pos_ms, dur_ms, id, title,
//          channel, thumb
//   cmd    a FIFO, one command a line: play, pause, toggle, seek <+-sec>, volume <0-150>, stop
//   last   on exit: id and pos_ms, so YouTube can reopen the video where the sound was
#include "bgaudio.h"
#include "stream.h"
#include "http.h"
#include <mpv/client.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char* BG_DIR = "/tmp/ytbg";

std::string path(const char* name) { return std::string(BG_DIR) + "/" + name; }

std::map<std::string, std::string> read_kv(const std::string& file) {
    std::map<std::string, std::string> kv;
    FILE* f = std::fopen(file.c_str(), "r");
    if (!f) return kv;
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        kv[line] = eq + 1;
    }
    std::fclose(f);
    return kv;
}

void write_atomic(const std::string& file, const std::string& text) {
    std::string tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);   // binary-safe: the picture too
    std::fclose(f);
    std::rename(tmp.c_str(), file.c_str());
}

volatile sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

double get_double(mpv_handle* mpv, const char* prop) {
    double v = 0;
    if (mpv_get_property(mpv, prop, MPV_FORMAT_DOUBLE, &v) < 0) return 0;
    return std::isfinite(v) ? v : 0;
}
bool get_flag(mpv_handle* mpv, const char* prop) {
    int v = 0;
    mpv_get_property(mpv, prop, MPV_FORMAT_FLAG, &v);
    return v != 0;
}

}  // namespace

int bgaudio_main(const char* request_file) {
    auto req = read_kv(request_file);
    unlink(request_file);
    const std::string audio = req["audio"], ua = req["ua"];
    if (audio.empty()) {
        std::fprintf(stderr, "[bg] no audio url in %s\n", request_file);
        return 1;
    }
    mkdir(BG_DIR, 0755);
    write_atomic(path("pid"), std::to_string(getpid()) + "\n");
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGHUP, SIG_IGN);   // outlives the launcher's shell
    std::signal(SIGPIPE, SIG_IGN);

    // The command FIFO, opened read-write so it never sees EOF when a writer leaves.
    std::string cmd_path = path("cmd");
    unlink(cmd_path.c_str());
    mkfifo(cmd_path.c_str(), 0666);
    int cmd_fd = open(cmd_path.c_str(), O_RDWR | O_NONBLOCK);

    mpv_handle* mpv = mpv_create();
    if (!mpv) return 1;
    mpv_set_option_string(mpv, "ytdl", "no");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "vid", "no");
    mpv_set_option_string(mpv, "video", "no");
    mpv_set_option_string(mpv, "audio-display", "no");
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "volume-max", "150");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "16MiB");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "16MiB");
    mpv_set_option_string(mpv, "user-agent", ua.c_str());
    if (req["paced"] == "1") {   // restricted delivery: stay inside the server's window
        mpv_set_option_string(mpv, "demuxer-readahead-secs", "25");
        mpv_set_option_string(mpv, "cache-secs", "25");
    }
    if (!req["volume"].empty()) mpv_set_option_string(mpv, "volume", req["volume"].c_str());
    if (mpv_initialize(mpv) < 0) return 1;
    mpv_request_log_messages(mpv, "error");
    ytn::register_stream(mpv);

    // raw: a live HLS manifest (mpv reads it itself); no scheme: a downloaded file
    bool raw = req["raw"] == "1" || audio.find("://") == std::string::npos;
    std::string url = raw ? audio : ytn::wrap_url(audio, ua);
    std::string start = "start=" + (req["start"].empty() ? std::string("0") : req["start"]);
    const char* load[] = {"loadfile", url.c_str(), "replace", start.c_str(), nullptr};
    if (mpv_command(mpv, load) < 0) {
        mpv_terminate_destroy(mpv);
        return 1;
    }

    // The clip's picture for the HOME panel and Music (best effort: the sound is already on)
    std::string thumb;
    if (!req["thumb_url"].empty()) {
        HttpClient http;
        auto r = http.get(req["thumb_url"], {}, 8);
        if (r.ok() && !r.body.empty()) {
            thumb = path("thumb.jpg");
            write_atomic(thumb, r.body);
        }
    }
    std::string head = "id=" + req["id"] + "\ntitle=" + req["title"] + "\nchannel=" + req["channel"] +
                       "\nthumb=" + thumb + "\n";
    // What YouTube needs to reopen the clip (written to "last" on exit)
    std::string again = "id=" + req["id"] + "\ntitle=" + req["title"] + "\nchannel=" + req["channel"] +
                        "\nchannel_id=" + req["channel_id"] + "\nthumb_url=" + req["thumb_url"] + "\n";
    double pos = std::atof(req["start"].c_str()), dur = std::atof(req["duration"].c_str());
    std::string pending;
    bool ended = false;
    while (!g_quit && !ended) {
        mpv_event* e = mpv_wait_event(mpv, 0.5);
        for (; e->event_id != MPV_EVENT_NONE; e = mpv_wait_event(mpv, 0)) {
            if (e->event_id == MPV_EVENT_SHUTDOWN) ended = true;
            if (e->event_id == MPV_EVENT_END_FILE) {
                auto* ef = (mpv_event_end_file*)e->data;
                if (ef->reason == MPV_END_FILE_REASON_EOF || ef->reason == MPV_END_FILE_REASON_ERROR) ended = true;
            }
        }
        // Commands, one a line.
        char buf[256];
        ssize_t n;
        while (cmd_fd >= 0 && (n = read(cmd_fd, buf, sizeof buf)) > 0) pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string c = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (c == "stop") g_quit = 1;
            else if (c == "play") mpv_set_property_string(mpv, "pause", "no");
            else if (c == "pause") mpv_set_property_string(mpv, "pause", "yes");
            else if (c == "toggle") {
                const char* cy[] = {"cycle", "pause", nullptr};
                mpv_command(mpv, cy);
            } else if (c.rfind("seek ", 0) == 0) {
                std::string s = c.substr(5);
                const char* sk[] = {"seek", s.c_str(), "relative", nullptr};
                mpv_command(mpv, sk);
            } else if (c.rfind("volume ", 0) == 0) {
                std::string v = c.substr(7);
                mpv_set_property_string(mpv, "volume", v.c_str());
            }
        }
        double p = get_double(mpv, "time-pos"), d = get_double(mpv, "duration");
        if (p > 0) pos = p;
        if (d > 0) dur = d;
        char st[96];
        std::snprintf(st, sizeof st, "state=%s\npos_ms=%d\ndur_ms=%d\n",
                      get_flag(mpv, "pause") ? "paused" : "playing", (int)(pos * 1000), (int)(dur * 1000));
        write_atomic(path("state"), st + head);
    }
    // Where the sound got to, for YouTube to reopen the video there.
    char last[64];
    std::snprintf(last, sizeof last, "pos_ms=%d\nended=%d\n", (int)(pos * 1000), ended ? 1 : 0);
    write_atomic(path("last"), again + last);
    unlink(path("state").c_str());
    unlink(path("pid").c_str());
    if (cmd_fd >= 0) close(cmd_fd);
    unlink(cmd_path.c_str());
    mpv_terminate_destroy(mpv);
    return 0;
}
