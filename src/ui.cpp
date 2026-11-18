#include "ui.h"
#include "platform.h"   // Windows timegm shim
#include "i18n.h"
#include "remux.h"
#include "hwdetect.h"
#include "http.h"
#include "../third_party/json.hpp"
#include <SDL.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace ui {

// Debug logging flag, read once (avoid getenv() on the per-frame render path).
static const bool kDbg = getenv("YTC_DEBUG") != nullptr;

// YouTube's direct media URLs do not contain the chapters shown on the watch page.
// Creators define those as timestamped description lines, so retain the timestamp
// list for shoulder-button chapter navigation. YouTube requires at least three
// chapters beginning at 0:00; applying the same rule avoids treating incidental
// timestamps in ordinary prose as chapters.
static std::vector<double> chapter_times(const std::string& description) {
    std::vector<double> out;
    size_t line_at = 0;
    while (line_at <= description.size()) {
        size_t line_end = description.find('\n', line_at);
        if (line_end == std::string::npos) line_end = description.size();
        std::string line = description.substr(line_at, line_end - line_at);
        size_t at = 0;
        while (at < line.size()) {
            while (at < line.size() && std::isspace((unsigned char)line[at])) ++at;
            size_t end = at;
            while (end < line.size() && !std::isspace((unsigned char)line[end])) ++end;
            if (end == at) break;
            std::string token = line.substr(at, end - at);
            while (!token.empty() && (token.front() == '(' || token.front() == '['))
                token.erase(token.begin());
            while (!token.empty() && (token.back() == ')' || token.back() == ']' ||
                                      token.back() == ',' || token.back() == '.'))
                token.pop_back();

            int parts[3] = {0, 0, 0}, count = 0;
            size_t p = 0;
            bool valid = !token.empty();
            while (valid && p < token.size() && count < 3) {
                size_t colon = token.find(':', p);
                size_t stop = colon == std::string::npos ? token.size() : colon;
                if (stop == p) { valid = false; break; }
                int value = 0;
                for (size_t i = p; i < stop; ++i) {
                    if (!std::isdigit((unsigned char)token[i])) { valid = false; break; }
                    value = value * 10 + (token[i] - '0');
                }
                if (!valid) break;
                parts[count++] = value;
                if (colon == std::string::npos) { p = token.size(); break; }
                p = colon + 1;
            }
            if (valid && p == token.size() && (count == 2 || count == 3)) {
                int minutes = parts[count - 2], seconds = parts[count - 1];
                if (seconds < 60 && (count == 2 || minutes < 60)) {
                    double t = count == 3 ? parts[0] * 3600.0 + minutes * 60.0 + seconds
                                          : minutes * 60.0 + seconds;
                    // A 0:00 line starts the candidate chapter block. Reset here so
                    // an unrelated timestamp earlier in the description cannot hide
                    // a valid chapter list farther down.
                    if (t == 0) { out.clear(); out.push_back(t); }
                    else if (!out.empty() && t > out.back()) out.push_back(t);
                }
                break; // at most one chapter timestamp per description line
            }
            at = end;
        }
        if (line_end == description.size()) break;
        line_at = line_end + 1;
    }
    if (out.size() < 3 || out.front() != 0) out.clear();
    return out;
}

// DESIGN.md colours (section 2)
namespace dzc {
const gfx::Color bg{0, 0, 0, 1}, s1{0x1C/255.f, 0x1C/255.f, 0x1E/255.f, 1}, s2{0x2C/255.f, 0x2C/255.f, 0x2E/255.f, 1},
    s3{0x3A/255.f, 0x3A/255.f, 0x3C/255.f, 1}, kb{0x12/255.f, 0x12/255.f, 0x14/255.f, 1},
    text{0xF2/255.f, 0xF2/255.f, 0xF7/255.f, 1}, text2{0xA1/255.f, 0xA1/255.f, 0xA6/255.f, 1},
    text3{0xC7/255.f, 0xC7/255.f, 0xCC/255.f, 1}, accent{0x0A/255.f, 0x84/255.f, 0xFF/255.f, 1},
    white{1, 1, 1, 1}, red{0xFF/255.f, 0x45/255.f, 0x3A/255.f, 1};
}

// ---------- On-screen keyboard layout ----------
namespace {
enum KType { KCHAR, KSPACE, KDEL, KSUBMIT, KSHIFT, KLEFT, KRIGHT, KLANG, KSYM };
// text: what a KCHAR key types when it isn't the one ASCII ch (Brick OS: Thai keys, UTF-8)
struct Key { const char* label; KType type; char ch; int span; const char* text = nullptr; };
// Android-TV-style QWERTY, but keeping a dedicated number row on top. `span` widens a
// key (space/search) for layout + nav. Each row totals 10 units.
const std::vector<std::vector<Key>> KB = {
    {{"1",KCHAR,'1',1},{"2",KCHAR,'2',1},{"3",KCHAR,'3',1},{"4",KCHAR,'4',1},{"5",KCHAR,'5',1},
     {"6",KCHAR,'6',1},{"7",KCHAR,'7',1},{"8",KCHAR,'8',1},{"9",KCHAR,'9',1},{"0",KCHAR,'0',1}},
    {{"q",KCHAR,'q',1},{"w",KCHAR,'w',1},{"e",KCHAR,'e',1},{"r",KCHAR,'r',1},{"t",KCHAR,'t',1},
     {"y",KCHAR,'y',1},{"u",KCHAR,'u',1},{"i",KCHAR,'i',1},{"o",KCHAR,'o',1},{"p",KCHAR,'p',1}},
    {{"a",KCHAR,'a',1},{"s",KCHAR,'s',1},{"d",KCHAR,'d',1},{"f",KCHAR,'f',1},{"g",KCHAR,'g',1},
     {"h",KCHAR,'h',1},{"j",KCHAR,'j',1},{"k",KCHAR,'k',1},{"l",KCHAR,'l',1},{".",KCHAR,'.',1}},
    {{"\xE2\x87\xA7",KSHIFT,0,1},                                             // U+21E7 shift
     {"z",KCHAR,'z',1},{"x",KCHAR,'x',1},{"c",KCHAR,'c',1},{"v",KCHAR,'v',1},{"b",KCHAR,'b',1},
     {"n",KCHAR,'n',1},{"m",KCHAR,'m',1},{",",KCHAR,',',1},
     {"\xE2\x8C\xAB",KDEL,0,1}},                                             // U+232B backspace
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},                  // U+25C0 / U+25B6
     {"space",KSPACE,' ',4},{"#+=",KSYM,0,1},{"\xE0\xB8\x81/A",KLANG,0,1},   // Brick OS: ก/A
     {"Search",KSUBMIT,0,2}},
};
// Brick OS: the shifted English page and the symbols page (#+=), as in the system keyboard (DESIGN 7.6)
#define TS(t) {t, KCHAR, 0, 1, t}
#define KB_BOTTOM4 {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1}, \
     {"space",KSPACE,' ',4},{"#+=",KSYM,0,1},{"\xE0\xB8\x81/A",KLANG,0,1},{"Search",KSUBMIT,0,2}}
const std::vector<std::vector<Key>> KB_SHIFT = {
    {TS("!"),TS("@"),TS("#"),TS("$"),TS("%"),TS("^"),TS("&"),TS("*"),TS("("),TS(")")},
    {TS("Q"),TS("W"),TS("E"),TS("R"),TS("T"),TS("Y"),TS("U"),TS("I"),TS("O"),TS("P")},
    {TS("A"),TS("S"),TS("D"),TS("F"),TS("G"),TS("H"),TS("J"),TS("K"),TS("L"),TS(":")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},TS("Z"),TS("X"),TS("C"),TS("V"),TS("B"),TS("N"),TS("M"),TS("?"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    KB_BOTTOM4,
};
const std::vector<std::vector<Key>> KB_SYM = {
    {TS("1"),TS("2"),TS("3"),TS("4"),TS("5"),TS("6"),TS("7"),TS("8"),TS("9"),TS("0")},
    {TS("-"),TS("_"),TS("="),TS("+"),TS("["),TS("]"),TS("{"),TS("}"),TS("\\"),TS("|")},
    {TS("`"),TS("~"),TS(";"),TS(":"),TS("'"),TS("\""),TS("<"),TS(">"),TS("/"),TS("@")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},TS("!"),TS("#"),TS("$"),TS("%"),TS("^"),TS("&"),TS("*"),TS("?"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    KB_BOTTOM4,
};
#undef TS
#undef KB_BOTTOM4
// Brick OS: Thai on the standard Kedmanee layout (TIS 820-2538), as on Thai PC and phone
// keyboards. A lone mark is shown on a dotted circle (label) and types the mark (text).
#define TK(t) {t, KCHAR, 0, 1, t}
#define TM(t) {t, KCHAR, 0, 1, t}
// Brick OS: every Thai row is 12 keys wide (the mockup's rows; ฃ/ฅ end the third), so the keys line up in columns
// edge to edge like the English rows; a mark key shows the mark alone.
const std::vector<std::vector<Key>> KB_TH = {
    {TK("ๅ"),TK("/"),TK("-"),TK("ภ"),TK("ถ"),TM("ุ"),TM("ึ"),TK("ค"),TK("ต"),TK("จ"),TK("ข"),TK("ช")},
    {TK("ๆ"),TK("ไ"),TK("ำ"),TK("พ"),TK("ะ"),TM("ั"),TM("ี"),TK("ร"),TK("น"),TK("ย"),TK("บ"),TK("ล")},
    {TK("ฟ"),TK("ห"),TK("ก"),TK("ด"),TK("เ"),TM("้"),TM("่"),TK("า"),TK("ส"),TK("ว"),TK("ง"),TK("ฃ")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},
     TK("ผ"),TK("ป"),TK("แ"),TK("อ"),TM("ิ"),TM("ื"),TK("ท"),TK("ม"),TK("ใ"),TK("ฝ"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',6},{"#+=",KSYM,0,1},{"\xE0\xB8\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
const std::vector<std::vector<Key>> KB_TH_SHIFT = {
    {TK("+"),TK("๑"),TK("๒"),TK("๓"),TK("๔"),TM("ู"),TK("฿"),TK("๕"),TK("๖"),TK("๗"),TK("๘"),TK("๙")},
    {TK("๐"),TK("\""),TK("ฎ"),TK("ฑ"),TK("ธ"),TM("ํ"),TM("๊"),TK("ณ"),TK("ฯ"),TK("ญ"),TK("ฐ"),TK(",")},
    {TK("ฤ"),TK("ฆ"),TK("ฏ"),TK("โ"),TK("ฌ"),TM("็"),TM("๋"),TK("ษ"),TK("ศ"),TK("ซ"),TK("."),TK("ฅ")},
    {{"\xE2\x87\xA7",KSHIFT,0,1},
     TK("("),TK(")"),TK("ฉ"),TK("ฮ"),TM("ฺ"),TM("์"),TK("?"),TK("ฒ"),TK("ฬ"),TK("ฦ"),
     {"\xE2\x8C\xAB",KDEL,0,1}},
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},
     {"space",KSPACE,' ',6},{"#+=",KSYM,0,1},{"\xE0\xB8\x81/A",KLANG,0,1},
     {"Search",KSUBMIT,0,2}},
};
#undef TK
#undef TM

// Dedicated numeric keypad for device linking (TV codes are digits only). A squared
// 4-column dialpad; each row totals 4 units, keys are drawn square.
const std::vector<std::vector<Key>> KB_NUM = {
    {{"1",KCHAR,'1',1},{"2",KCHAR,'2',1},{"3",KCHAR,'3',1},{"4",KCHAR,'4',1}},
    {{"5",KCHAR,'5',1},{"6",KCHAR,'6',1},{"7",KCHAR,'7',1},{"8",KCHAR,'8',1}},
    {{"9",KCHAR,'9',1},{"0",KCHAR,'0',1},
     {"\xE2\x8C\xAB",KDEL,0,1},{"\xE2\x9C\x93",KSUBMIT,0,1}},                    // 9 0 ⌫ ✓
    {{"space",KSPACE,' ',4}},                                                   // full-width space bar
};
} // namespace

const std::vector<std::vector<Key>>& App_kb_layout(bool num, bool sym, bool thai, bool shift) {
    if (num) return KB_NUM;
    if (sym) return KB_SYM;
    if (thai) return shift ? KB_TH_SHIFT : KB_TH;
    return shift ? KB_SHIFT : KB;
}
#define kb_layout() App_kb_layout(kb_numeric(), kb_sym_, kb_thai_, kb_shift_)

// Relative "age" for display. Search results already come as "3 months ago"
// (pass through). RSS gives ISO-8601 UTC (has 'T') -> compute relative age.
std::string humanize_age(const std::string& p) {
    if (p.empty()) return "";
    if (p.find('T') == std::string::npos) return p;   // already human (search)
    int y, mo, d, h, mi, se;
    if (std::sscanf(p.c_str(), "%d-%d-%dT%d:%d:%d", &y,&mo,&d,&h,&mi,&se) != 6) return "";
    std::tm tm{}; tm.tm_year=y-1900; tm.tm_mon=mo-1; tm.tm_mday=d;
    tm.tm_hour=h; tm.tm_min=mi; tm.tm_sec=se;
    time_t t = timegm(&tm), now = time(nullptr);
    long diff = (long)(now - t); if (diff < 0) diff = 0;
    auto fmt = [](long n, const char* u){ std::string s = std::to_string(n)+" "+u;
        if (n != 1) s += "s"; return s + " ago"; };
    if (diff < 60)          return "just now";
    if (diff < 3600)        return fmt(diff/60, "minute");
    if (diff < 86400)       return fmt(diff/3600, "hour");
    if (diff < 86400*7)     return fmt(diff/86400, "day");
    if (diff < 86400*30)    return fmt(diff/(86400*7), "week");
    if (diff < 86400*365)   return fmt(diff/(86400*30), "month");
    return fmt(diff/(86400*365), "year");
}

// ---------- ThumbCache ----------
ThumbCache::ThumbCache() { thread_ = std::thread([this]{ worker(); }); }
ThumbCache::~ThumbCache() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void ThumbCache::request(const std::string& url) {
    if (url.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (requested_.count(url)) return;
    requested_[url] = true;
    queue_.push_back(url);
}
gfx::Texture* ThumbCache::get(const std::string& url) {
    auto it = tex_.find(url);
    if (it == tex_.end()) return nullptr;
    used_[url] = ++tick_;            // mark most-recently-used (visible this frame)
    return it->second.get();
}
// Evict least-recently-used textures once over the cap. A scrolled-back tile just
// re-downloads. Called on the GL thread (pump) so freeing GL textures is safe.
void ThumbCache::evict_lru() {
    while (tex_.size() > kMaxTextures) {
        auto victim = tex_.begin(); uint64_t oldest = UINT64_MAX;
        for (auto it = tex_.begin(); it != tex_.end(); ++it) {
            uint64_t u = used_.count(it->first) ? used_[it->first] : 0;
            if (u < oldest) { oldest = u; victim = it; }
        }
        std::string url = victim->first;
        tex_.erase(victim);
        used_.erase(url);
        { std::lock_guard<std::mutex> lk(m_); requested_.erase(url); }  // allow re-request
    }
}
void ThumbCache::worker() {
    HttpClient http;
    while (!stop_) {
        std::string url;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { url = queue_.front(); queue_.pop_front(); } }
        if (url.empty()) { SDL_Delay(10); continue; }
        Pending p{url, {}, false};
        if (url.find("://") == std::string::npos) {   // local file (offline download thumb)
            std::ifstream f(url, std::ios::binary);
            if (f) { p.bytes.assign(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>()); p.ok = !p.bytes.empty(); }
        } else {
            auto r = http.get(url);
            if (getenv("YTC_DEBUG"))
                std::fprintf(stderr, "[thumb] GET %.50s -> %ld %zub\n", url.c_str(), r.status, r.body.size());
            p.ok = r.ok();
            if (r.ok()) p.bytes.assign(r.body.begin(), r.body.end());
        }
        std::lock_guard<std::mutex> lk(m_);
        done_.push_back(std::move(p));
    }
}
void ThumbCache::pump(int max_uploads) {
    std::vector<Pending> ready;
    { std::lock_guard<std::mutex> lk(m_);
      for (int i = 0; i < max_uploads && !done_.empty(); ++i) {
          ready.push_back(std::move(done_.back())); done_.pop_back(); } }
    for (auto& p : ready) {
        std::unique_ptr<gfx::Texture> t;
        if (p.ok && !p.bytes.empty())
            t = gfx::Texture::from_encoded(p.bytes.data(), p.bytes.size());
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[thumb] decode %.40s -> %s\n", p.url.c_str(), t ? "OK" : "FAIL");
        if (t) { tex_[p.url] = std::move(t); used_[p.url] = ++tick_; }
        else { std::lock_guard<std::mutex> lk(m_); requested_.erase(p.url); }  // failed -> allow retry
    }
    evict_lru();
}

// ---------- ChannelMetaCache ----------
ChannelMetaCache::ChannelMetaCache(yt::Innertube* it) : it_(it) {
    thread_ = std::thread([this]{ worker(); });
}
ChannelMetaCache::~ChannelMetaCache() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void ChannelMetaCache::request(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (requested_.count(id)) return;
    requested_[id] = true;
    queue_.push_back(id);
}
std::string ChannelMetaCache::video_count(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = vcount_.find(id);
    return it == vcount_.end() ? std::string() : it->second;
}
std::string ChannelMetaCache::avatar(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = avatar_.find(id);
    return it == avatar_.end() ? std::string() : it->second;
}
std::string ChannelMetaCache::name(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = name_.find(id);
    return it == name_.end() ? std::string() : it->second;
}
void ChannelMetaCache::worker() {
    while (!stop_) {
        std::string id;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { id = queue_.front(); queue_.pop_front(); } }
        if (id.empty()) { SDL_Delay(20); continue; }
        yt::ChannelInfo info = it_->channel_info(id);   // own HttpClient inside
        std::lock_guard<std::mutex> lk(m_);
        vcount_[id] = info.video_count_text;            // "" if not found
        avatar_[id] = info.avatar_url;                  // "" if not found
        name_[id]   = info.name;                        // "" if not found
    }
}

// ---------- RestrictedCheck ----------
RestrictedCheck::RestrictedCheck(yt::Innertube* it) : it_(it) {
    for (auto& [id, restricted] : it_->restricted_cache())   // persisted verdicts
        verdict_[id] = restricted ? 1 : 0;
    thread_ = std::thread([this]{ worker(); });
}
RestrictedCheck::~RestrictedCheck() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void RestrictedCheck::request(const std::string& channel_id, const std::string& video_id) {
    if (channel_id.empty() || video_id.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (verdict_.count(channel_id) || requested_.count(channel_id)) return;
    requested_.insert(channel_id);
    queue_.emplace_back(channel_id, video_id);
}
int RestrictedCheck::verdict(const std::string& channel_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = verdict_.find(channel_id);
    return it == verdict_.end() ? -1 : it->second;
}
void RestrictedCheck::put(const std::string& channel_id, bool restricted) {
    if (channel_id.empty()) return;
    { std::lock_guard<std::mutex> lk(m_); verdict_[channel_id] = restricted ? 1 : 0; }
    it_->set_restricted_cached(channel_id, restricted);
    dirty_ = true;
}
bool RestrictedCheck::drain_dirty() { return dirty_.exchange(false); }
void RestrictedCheck::worker() {
    while (!stop_) {
        std::pair<std::string, std::string> job;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { job = queue_.front(); queue_.pop_front(); } }
        if (job.first.empty()) { SDL_Delay(50); continue; }
        int v = it_->check_video_restricted(job.second);   // own HttpClient inside
        {
            std::lock_guard<std::mutex> lk(m_);
            if (v >= 0) verdict_[job.first] = v;
            else requested_.erase(job.first);   // unknown: allow a later retry
        }
        if (v >= 0) {
            it_->set_restricted_cached(job.first, v == 1);
            dirty_ = true;
        }
        SDL_Delay(250);   // pace the /player calls (avoid burst -> bot-wall risk)
    }
}

// ---------- App ----------
// Hardware video decode is DISABLED for release: every device decodes in
// software (even where hardware decode is detected and works), no detection
// runs, and Settings > Video hides the Video Decode row. Flip to re-enable the
// whole path (detection, per-SoC defaults, the toggle). YTC_HWDEC still
// overrides for development.
static constexpr bool kHwdecEnabled = false;

// The bundled CJK subset (data/fonts/NotoSansCJKjp-Subset.otf; see tools/make_cjk_font.sh),
// or "" when absent. Shared by the UI fonts and mpv's caption renderer.
static std::string cjk_font_path() {
    const std::string name = "NotoSansCJKjp-Subset.otf";
    for (const std::string& c : {platform::exe_dir() + "/data/fonts/" + name, "data/fonts/" + name})
        if (std::ifstream(c).good()) return c;
    return "";
}
void App::load_fonts() {
    // Portable font lookup: env override, then the port/app-local copy in
    // data/, then the Debian system path (dev machines). Handhelds don't
    // ship DejaVu — the port bundles it in data/.
    auto pick_font = []() -> std::string {
        const char* env = std::getenv("YTC_FONT");
        std::string base = platform::exe_dir();
        std::string cands[] = {
            env ? std::string(env) : std::string(),
            base + "/data/DejaVuSans.ttf",     // bundled next to the executable (desktop/port)
            "data/DejaVuSans.ttf",             // or relative to the working directory
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // Debian/Ubuntu system
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",           // Fedora/Arch system
            "/Library/Fonts/Arial Unicode.ttf",                 // macOS fallback
            "C:/Windows/Fonts/segoeui.ttf",                     // Windows fallback
        };
        for (const auto& c : cands) {
            if (c.empty()) continue;
            if (std::ifstream(c).good()) return c;
        }
        return base + "/data/DejaVuSans.ttf";
    };
    const std::string ttf_path = pick_font();
    const char* ttf = ttf_path.c_str();
    // Fallback for what DejaVu lacks: Japanese (kana, JIS X 0208 kanji, CJK
    // punctuation, full-width forms). Titles/comments in Japanese show up in every
    // UI language, not just ja. Missing file = no fallback (tofu, as before).
    const std::string cjk = cjk_font_path();
    // Font scale is NOT pure-linear with height: a floor keeps text readable on
    // small handheld screens (held close), and a cap avoids oversized text on 4K.
    float s = win_->height() / 720.f;
    if (s < 0.9f) s = 0.9f;
    if (s > 2.2f) s = 2.2f;
    // Thai (Brick OS): data/fonts/BrickOSThai.ttf, IBM Plex Sans Thai with pre-placed marks.
    std::string thai;
    for (const std::string& c : {platform::exe_dir() + "/data/fonts/BrickOSThai.ttf", std::string("data/fonts/BrickOSThai.ttf")})
        if (std::ifstream(c).good()) { thai = c; break; }
    font_title_ = gfx::Font::load(ttf, 44 * s, cjk, thai);
    font_body_  = gfx::Font::load(ttf, 32 * s, cjk, thai);
    font_small_ = gfx::Font::load(ttf, 27 * s, cjk, thai);
    {   // rail symbols (house, heart, clock, arrow, gear) from the bundled DejaVu
        std::string dv = platform::exe_dir() + "/data/DejaVuSans.ttf";
        if (!std::ifstream(dv).good()) dv = "data/DejaVuSans.ttf";
        font_icon_ = gfx::Font::load(dv, 36 * s);
    }
    fonts_baked_h_ = win_->height();
    load_design_fonts();
}


int App::compute_columns() const {
    // Aim for ~380px-wide cards; fewer columns on small/narrow screens means
    // bigger cards, thumbnails and text. Clamped to a sensible range.
    int c = (int)std::lround(win_->width() / 380.0);
    if (c < 2) c = 2;
    if (c > 6) c = 6;
    return c;
}

void App::on_resize() {
    cols_ = compute_columns();
    // Re-bake fonts only when the height changed enough to matter (atlas bakes
    // are not free, and resize events fire continuously during a drag).
    int h = win_->height();
    if (h > 0 && std::abs(h - fonts_baked_h_) >= 24) load_fonts();
}

// Directory containing the config file (where cast.json etc. live).
static std::string dir_of(const std::string& path) {
    auto s = path.find_last_of('/');
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}
App::App(const std::string& config_path, gfx::Window* win)
    : win_(win), it_(config_path), cast_(dir_of(config_path)) {
    load_fonts();
    cols_ = compute_columns();
    // YTC_VIEW / YTC_CAROUSEL (a launcher's pick) are the DEFAULT until the user picks a view in
    // Settings — not an override, or the saved choice would reset on every launch.
    {
        int def = 0;   // 0 grid / 1 carousel / 2 3D carousel / 3 coverflow / 4 shelves
        if (getenv("YTC_CAROUSEL")) def = (int)ViewMode::Carousel;
        if (const char* vm = getenv("YTC_VIEW")) def = atoi(vm);
        int v = it_.setting_int("view", def);
        view_mode_ = (ViewMode)(v >= 0 && v <= 4 ? v : def);
    }
    refresh_favorites();
    refresh_favorite_playlists();
    refresh_watch_later();
    refresh_downloads();
    hide_restricted_ = it_.setting_int("hide_restricted", 0) != 0;
    ask_resume_ = it_.setting_int("ask_resume", 1) != 0;   // default ON
    volume_ = it_.setting_int("volume", 100);              // app-local playback volume %
    if (volume_ < 0) volume_ = 0; if (volume_ > 150) volume_ = 150;
    // Hardware decode detection runs asynchronously (see start_hwdec_detect);
    // the toggle appears once it lands. Skipped entirely while kHwdecEnabled is off.
    if (kHwdecEnabled) start_hwdec_detect();
    // Video Decode default: Hardware — EXCEPT on SoCs where the v4l2m2m-copy
    // path is known to lose to software (Venus capture buffers are mapped
    // uncached; the per-frame memcpy is slower than decoding on the CPU cores).
    // Field-verified on the Mangmi Air X (qcom,sm6115) at 1080p60: dec drops 0
    // yet ~1400 presentation drops/min, and software played it better. The
    // toggle stays available; an explicit choice persists. Zero-copy (dmabuf
    // import) is the eventual fix that beats both — see docs/HWDEC_SURVEY.md.
    if (!kHwdecEnabled) {
        hwdec_mode_ = 1;   // Software, regardless of device support or a saved choice
    } else {
        int saved = it_.setting_int("hwdec", -1);
        if (saved >= 0) hwdec_mode_ = saved ? 1 : 0;
        else {
            std::string compat = hwdetect::read_file("/proc/device-tree/compatible");
            static const char* kSlowCopyPath[] = {"qcom,sm6115"};
            hwdec_mode_ = 0;
            for (const char* soc : kSlowCopyPath)
                if (compat.find(soc) != std::string::npos) hwdec_mode_ = 1;
        }
    }
    player_.set_hwdec(hwdec_mode_str());
    aspect_mode_ = it_.setting_int("aspect", 0);           // 0 fit / 1 zoom / 2 stretch
    if (aspect_mode_ < 0 || aspect_mode_ > 2) aspect_mode_ = 0;
    player_.set_aspect(aspect_mode_);
    audio_lang_pref_ = it_.setting_str("audio_lang", "app");  // dub language default
    cc_lang_pref_ = it_.setting_str("cc_lang", "app");        // default caption language
    if (cc_lang_pref_ == "off") cc_lang_pref_ = "app";        // migrate the old value
    cc_size_ = it_.setting_int("cc_size", 1);                 // caption size (medium)
    if (cc_size_ < 0 || cc_size_ > 2) cc_size_ = 1;
    cc_style_ = it_.setting_int("cc_style", 0);               // caption style (white)
    if (cc_style_ < 0 || cc_style_ > 5) cc_style_ = 0;
    player_.set_caption_style(cc_size_, cc_style_);
    {   // Captions render in mpv (libass), not our atlas: give it the CJK font too.
        std::string cjk = cjk_font_path();
        if (!cjk.empty()) player_.set_fonts_dir(cjk.substr(0, cjk.rfind('/') + 1));
    }
    sponsorblock_ = it_.setting_int("sponsorblock", 0) != 0;   // default OFF
    autoplay_ = it_.setting_int("autoplay", 0) != 0;          // default OFF
    bg_on_exit_ = it_.setting_int("bg_on_exit", 1) != 0;      // default ON
    // Home sources bitmask (1 fav / 2 history / 4 custom). Default: Favorites
    // only — History and Custom are opt-in via Browsing > Home Feed.
    home_sources_ = it_.setting_int("home_sources", 1) & 7;
    if (!home_sources_) home_sources_ = 1;

    // UI language + Innertube locale (0 = English). Drives translated labels and
    // YouTube-localized titles/metadata. Set before any feed fetch is kicked off.
    lang_ = it_.setting_int("lang", 0);
    i18n::set_language(lang_);
    it_.set_locale(i18n::language_hl(lang_), i18n::language_gl(lang_));

    // Default quality cap: 1080p (not 4K). Persisted in settings.json (Settings menu);
    // YTC_MAXHEIGHT overrides for testing (0 = uncapped).
    // YTC_MAXHEIGHT (the port launcher exports 480 for handhelds) is the DEFAULT
    // when the user hasn't chosen a Max Quality yet — not an override, or the
    // Settings choice would silently reset to 480p on every launch.
    {
        const char* mh = getenv("YTC_MAXHEIGHT");
        int def = mh ? atoi(mh) : 1080;
        play_prefs_.max_height = it_.setting_int("max_height", def);
        // A device ceiling (Brick OS: 720 on the A133P) clamps a saved choice too.
        if (const char* c = getenv("YTC_QUALITY_CEIL")) quality_ceil_ = atoi(c);
        if (quality_ceil_ > 0 &&
            (play_prefs_.max_height <= 0 || play_prefs_.max_height > quality_ceil_))
            play_prefs_.max_height = quality_ceil_;
    }
}

App::~App() {
    if (media_flag_on_) unlink(getenv("YTC_MEDIA_FLAG"));
    cast_events_run_ = false;
    if (cast_events_thread_.joinable()) cast_events_thread_.join();
    if (resolve_thread_.joinable()) resolve_thread_.join();
    if (hwdec_thread_.joinable()) hwdec_thread_.join();
    if (chinfo_thread_.joinable()) chinfo_thread_.join();
    if (more_thread_.joinable()) more_thread_.join();
    if (home_more_thread_.joinable()) home_more_thread_.join();
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    if (refresh_thread_.joinable()) refresh_thread_.join();
    if (desc_thread_.joinable()) desc_thread_.join();
    if (comments_thread_.joinable()) comments_thread_.join();
    if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
    dl_cancel_ = true;
    if (dl_thread_.joinable()) dl_thread_.join();
    if (sb_thread_.joinable()) sb_thread_.join();
    if (cc_thread_.joinable()) cc_thread_.join();
    if (cc_dl_thread_.joinable()) cc_dl_thread_.join();
    if (rel_thread_.joinable()) rel_thread_.join();
    if (sugg_thread_.joinable()) sugg_thread_.join();
}

void App::set_results(std::vector<yt::SearchResult> r) {
    filter_hidden(r);
    results_ = std::move(r);
    sel_ = 0; scroll_ = 0;
    build_tile_lines();          // precompute metadata lines once (not per frame)
    // Thumbnails are requested lazily per-viewport by the renderers (bounded memory);
    // channel metadata is small, so fetch it up front.
    for (const auto& v : results_) {
        if (v.is_channel()) chan_meta_.request(v.channel_id);   // async video count
        // Search shorts carry no uploader name; resolve it from their channel id.
        else if (v.is_short && v.author.empty() && !v.channel_id.empty())
            chan_meta_.request(v.channel_id);
    }
    queue_restricted_checks();   // judge unknown channels in the background
}
void App::search(const std::string& query) {
    query_ = query;
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    filt_type_ = filt_duration_ = filt_date_ = filt_sort_ = 0;   // fresh search resets filters
    run_search();
}

// (Re-)issue the current query_ with the active server-side filters. Called by a
// fresh search() and again whenever the Search Filters modal changes something.
void App::run_search() {
    static const int kTypePb[] = {0, 1, 2, 3};        // Off/Videos/Channels/Playlists
    static const int kDurPb[]  = {0, 4, 5, 2};        // Off/Under3/3-20/Over20
    static const int kDatePb[] = {0, 2, 3, 4, 5};     // Off/Today/Week/Month/Year
    static const int kSortPb[] = {0, 0, 3};           // Off/Relevance/Popularity (view count)
    std::string params = yt::build_search_params(kTypePb[filt_type_], kDurPb[filt_duration_],
                                                 kDatePb[filt_date_], kSortPb[filt_sort_]);
    auto f = it_.search_feed(query_, params);   // never throws (guarded in Innertube)
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    search_tab_ = 0; search_sort_ = 0;  // client-side tab/sort reset to All / Relevance
    search_base_ = results_;            // canonical relevance order of all types (tab/sort source)
    if (results_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::NoResultsConn);
        status_until_ = SDL_GetTicks() + 8000;
    }
}

// The "Search Filters" modal: four cyclable rows, each defaulting to Off. `snapshot`
// records the entry-state for change detection; false on in-place rebuilds (cycling),
// so it isn't overwritten between opening the modal and closing it.
void App::open_search_filters(bool snapshot) {
    using i18n::tr; using S = i18n::Str;
    auto row = [&](S label, const S* vals, int n, int cur, MenuAction act) {
        (void)n;
        menu_items_.push_back({tr(label) + std::string(":  ") + tr(vals[cur]), act});
    };
    static const S kType[] = {S::Off, S::TabVideos, S::FilterChannels, S::TabPlaylists};
    static const S kDur[]  = {S::Off, S::DurShort, S::DurMedium, S::DurLong};
    static const S kDate[] = {S::Off, S::DateToday, S::DateWeek, S::DateMonth, S::DateYear};
    static const S kSort[] = {S::Off, S::FilterRelevance, S::FilterPopularity};
    menu_kind_ = MenuKind::SearchFilters;
    menu_items_.clear();
    row(S::FilterType,       kType, 4, filt_type_,     MenuAction::CycleFilterType);
    row(S::FilterDuration,   kDur,  4, filt_duration_, MenuAction::CycleFilterDuration);
    row(S::FilterUploadDate, kDate, 5, filt_date_,     MenuAction::CycleFilterDate);
    row(S::FilterPrioritize, kSort, 3, filt_sort_,     MenuAction::CycleFilterSort);
    menu_sel_ = 0;
    if (snapshot) {
        filt_snapshot_[0] = filt_type_; filt_snapshot_[1] = filt_duration_;
        filt_snapshot_[2] = filt_date_; filt_snapshot_[3] = filt_sort_;
    }
    menu_open_ = true;
}

// Parse a view-count string ("1,605,755 views" or "1.6M views") into a number for
// the Popular sort. Empty/unknown (playlists, channels) sort last (returns -1).
static long long parse_view_count(const std::string& s) {
    if (s.empty()) return -1;
    double n = 0; size_t i = 0; bool any = false;
    while (i < s.size() && !isdigit((unsigned char)s[i])) ++i;
    for (; i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == ',' || s[i] == '.'); ++i) {
        if (s[i] == ',') continue;
        if (s[i] == '.') { double frac = 0, scale = 0.1;
            for (++i; i < s.size() && isdigit((unsigned char)s[i]); ++i) { frac += (s[i]-'0')*scale; scale *= 0.1; }
            n += frac; break; }
        n = n * 10 + (s[i]-'0'); any = true;
    }
    if (!any) return -1;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size()) { char u = toupper((unsigned char)s[i]);
        if (u == 'K') n *= 1e3; else if (u == 'M') n *= 1e6; else if (u == 'B') n *= 1e9; }
    return (long long)(n + 0.5);
}

// Cycle the search-results sort (Relevance -> Newest -> Popular) and pop a toast.
void App::toggle_search_sort() {
    if (query_.empty() || search_base_.empty()) return;
    search_sort_ = (search_sort_ + 1) % 3;
    apply_search_view(/*keep_selection=*/false);
    i18n::Str toast = search_sort_ == 1 ? i18n::Str::SortedByDate
                    : search_sort_ == 2 ? i18n::Str::SortedByViews
                                        : i18n::Str::SortedByRelevance;
    status_msg_ = i18n::tr(toast);
    status_until_ = SDL_GetTicks() + 2500;
}

// Switch the active search tab (All/Videos/Shorts/Playlists) and rebuild the view.
void App::load_search_tab(int tab) {
    if (tab < 0 || tab > 4 || tab == search_tab_) return;
    search_tab_ = tab;
    apply_search_view(/*keep_selection=*/false);
}

// Rebuild results_ from the canonical list: filter by the active tab, then sort.
// keep_selection preserves the highlighted item by id (used after pagination).
void App::apply_search_view(bool keep_selection) {
    std::string cur_id;
    if (keep_selection && sel_ >= 0 && sel_ < (int)results_.size())
        cur_id = results_[sel_].video_id;
    results_.clear();
    for (const auto& r : search_base_) {
        switch (search_tab_) {
            case 1: if (r.is_short || !(r.kind == yt::SearchResult::Kind::Video)) continue; break;  // Videos
            case 2: if (!r.is_short) continue; break;                                                // Shorts
            case 3: if (!r.is_channel()) continue; break;                                            // Channels
            case 4: if (!r.is_playlist()) continue; break;                                           // Playlists
            default: break;                                                                          // All
        }
        results_.push_back(r);
    }
    if (search_sort_ == 1)
        std::stable_sort(results_.begin(), results_.end(),
            [](const yt::SearchResult& a, const yt::SearchResult& b) {
                return yt::approx_age_secs(a.published_text) <
                       yt::approx_age_secs(b.published_text);   // newest (smallest age) first
            });
    else if (search_sort_ == 2)
        std::stable_sort(results_.begin(), results_.end(),
            [](const yt::SearchResult& a, const yt::SearchResult& b) {
                return parse_view_count(a.view_count_text) >
                       parse_view_count(b.view_count_text);     // most views first
            });
    build_tile_lines();
    sel_ = 0;
    if (keep_selection && !cur_id.empty())
        for (int i = 0; i < (int)results_.size(); ++i)
            if (results_[i].video_id == cur_id) { sel_ = i; break; }
    if (sel_ == 0) { scroll_ = 0; carousel_pos_ = 0; }
    ensure_visible();
}
void App::load_home() {
    query_.clear();                       // empty query + empty view_label_ => "Home"
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    cont_token_.clear();                  // merged home feed isn't paginated
    home_tab_ = 0;
    home_playlists_.clear(); home_playlists_loaded_ = false;
    home_posts_.clear(); home_posts_loaded_ = false;
    home_items_.clear();
    home_cursor_ = {}; home_pl_cursor_ = {}; home_posts_cursor_ = {};   // drop prior continuation
    results_.clear(); sel_ = 0; scroll_ = 0;
    // Fetch the merged home feed ASYNCHRONOUSLY (N favorites x 3 network calls) so
    // the window draws immediately; poll_refresh fills home_items_ + tiles when ready.
    refresh_current_view();               // RK_HOME (view_label_/query_ empty)
}

// Canonical thumbnail for a video id (our stored lists only keep id + title).
static std::string thumb_for(const std::string& video_id) {
    return "https://i.ytimg.com/vi/" + video_id + "/mqdefault.jpg";
}

void App::load_favorites() {
    query_.clear(); view_label_ = "Favorite Channels";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& [id, name] : it_.favorites()) {
        yt::SearchResult r;
        r.kind = yt::SearchResult::Kind::Channel;
        r.channel_id = id;
        r.title = name;                    // subs/video-count fill in async (chan_meta_)
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}

void App::load_watch_later() {
    query_.clear(); view_label_ = "Watch Later";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    // Entries carry their own kind + metadata (videos AND playlists).
    set_results(it_.watch_later());
}

void App::load_downloads() {
    query_.clear(); view_label_ = "Downloads";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    set_results(it_.downloads());   // tiles built from downloads/*.info (local thumbs)
}

void App::load_history() {
    query_.clear(); view_label_ = "History";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& [id, title, channel] : it_.history()) {
        yt::SearchResult r;
        r.video_id = id; r.title = title; r.author = channel;
        r.thumbnail_url = thumb_for(id);
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}
void App::refresh_favorite_playlists() {
    fav_pl_ids_.clear();
    for (auto& p : it_.favorite_playlists()) fav_pl_ids_.insert(p.id);
}

// The Favorite Playlists view: one tile per saved playlist, everything needed
// for the tile stored at favoriting time (thumbnails aren't derivable from ids).
void App::load_favorite_playlists() {
    query_.clear(); view_label_ = "Favorite Playlists";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& p : it_.favorite_playlists()) {
        yt::SearchResult r;
        r.kind = yt::SearchResult::Kind::Playlist;
        r.playlist_id = p.id;
        r.title = p.title;
        r.author = p.author;
        r.thumbnail_url = p.thumb;
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}

void App::refresh_favorites() {
    fav_ids_.clear();
    for (auto& id : it_.favorite_channel_ids()) fav_ids_.insert(id);
}
void App::refresh_watch_later() {
    wl_ids_.clear();
    for (auto& id : it_.watch_later_ids()) wl_ids_.insert(id);
}
void App::refresh_downloads() {
    dl_ids_.clear();
    for (auto& id : it_.download_ids()) dl_ids_.insert(id);
}

// Download the highlighted/playing video as a progressive .mp4 (offline), matching the
// current playback quality where a progressive stream allows.
void App::start_download(const yt::SearchResult& t) {
    if (t.video_id.empty()) return;
    if (dl_running_) {
        status_msg_ = i18n::tr(i18n::Str::DownloadBusy);
        status_until_ = SDL_GetTicks() + 2500; return;
    }
    if (it_.is_downloaded(t.video_id)) {
        status_msg_ = i18n::tr(i18n::Str::DownloadDone);
        status_until_ = SDL_GetTicks() + 2500; return;
    }
    dl_running_ = true; dl_done_ = false; dl_ok_ = false; dl_cancel_ = false;
    dl_progress_ = 0; dl_id_ = t.video_id; dl_title_ = t.title;
    int max_h = play_prefs_.max_height;   // match the quality that would have played
    yt::AudioPrefs dl_aprefs;             // ...and the audio language that would have
    dl_aprefs.lang = audio_lang_pref_ == "app"  ? i18n::language_hl(lang_)
                   : audio_lang_pref_ == "orig" ? "" : audio_lang_pref_;
    yt::SearchResult item = t;
    if (dl_thread_.joinable()) dl_thread_.join();
    dl_thread_ = std::thread([this, item, max_h, dl_aprefs]() {
        bool ok = false;
        try {
            // Use the same resolve that plays the video — it works for every video we
            // show, and its adaptive streams download via the chunked (ranged) fetch.
            yt::VideoInfo info = it_.resolve(item.video_id);
            if (getenv("YTC_DEBUG")) {
                const yt::Format* dv = nullptr; { yt::VideoPrefs vp; vp.max_height=max_h; dv=info.best_video(vp); }
                const yt::Format* da = info.best_audio(dl_aprefs);
                std::fprintf(stderr, "[dl] status=%s formats=%zu vf=%s(%d) af=%s remux=%d\n",
                    info.status.c_str(), info.formats.size(),
                    dv?(dv->url.empty()?"nourl":"ok"):"null", dv?dv->height:0,
                    da?(da->url.empty()?"nourl":"ok"):"null", (int)ytn::remux_available());
            }
            if (info.ok()) {
                const std::string id = item.video_id;
                std::string mp4 = it_.download_path(id);
                std::string finalpart = mp4 + ".part";
                HttpClient http;
                auto ua_hdrs = [](const std::string& ua) {
                    std::vector<std::string> h;
                    if (!ua.empty()) h.push_back("User-Agent: " + ua);
                    return h;
                };
                auto prog = [this](int base, int span) {
                    return [this, base, span](long long dn, long long tot) {
                        if (tot > 0) dl_progress_ = base + (int)(dn * span / tot);
                        return !dl_cancel_.load();
                    };
                };
                // Fetch to dest, verifying the byte count matches content_length; retry a
                // couple times so a dropped connection doesn't silently downgrade quality.
                auto fetch = [&](const std::string& url, const std::string& dest,
                                 long long expect, int base, int span,
                                 const std::vector<std::string>& hdrs) -> bool {
                    for (int attempt = 0; attempt < 3 && !dl_cancel_; ++attempt) {
                        if (!http.download(url, dest, hdrs, prog(base, span))) continue;
                        if (expect <= 0) return true;
                        std::ifstream f(dest, std::ios::binary | std::ios::ate);
                        if (f && (long long)f.tellg() == expect) return true;
                    }
                    return false;
                };
                // Mux adaptive video (up to the cap) + audio into one .mp4, matching the
                // quality that would have played.
                yt::VideoPrefs vp; vp.max_height = max_h;
                const yt::Format* vf = info.best_video(vp);
                const yt::Format* af = info.best_audio(dl_aprefs);
                if (ytn::remux_available() && vf && af && !vf->url.empty() && !af->url.empty()) {
                    auto h = ua_hdrs(info.user_agent);
                    std::string vtmp = mp4 + ".v", atmp = mp4 + ".a";
                    bool dv = fetch(vf->url, vtmp, (long long)vf->content_length, 0, 65, h);
                    bool da = dv && !dl_cancel_ && fetch(af->url, atmp, (long long)af->content_length, 65, 30, h);
                    if (dv && da) { dl_progress_ = 96; ok = ytn::remux_to_mp4(vtmp, atmp, finalpart); }
                    if (!getenv("YTC_DEBUG")) { std::remove(vtmp.c_str()); std::remove(atmp.c_str()); }
                }
                if (!ok && !dl_cancel_) {   // no muxer (dev build): 360p single-file
                    // The playback resolve has no progressive stream; only ANDROID_VR
                    // serves the combined itag 18, so resolve once more just for it.
                    yt::VideoInfo pinfo = it_.resolve_for_download(item.video_id);
                    const yt::Format* pf = pinfo.ok() ? pinfo.best_progressive(max_h) : nullptr;
                    if (pf && !pf->url.empty())
                        ok = fetch(pf->url, finalpart, (long long)pf->content_length, 0, 98,
                                   ua_hdrs(pinfo.user_agent));
                }
                if (ok) {
                    dl_progress_ = 100;
                    std::rename(finalpart.c_str(), mp4.c_str());
                    std::string th = !item.thumbnail_url.empty() ? item.thumbnail_url
                                   : ("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg");
                    auto tr = http.get(th);
                    if (tr.ok() && !tr.body.empty()) {
                        std::ofstream tf(it_.download_thumb_path(id), std::ios::binary);
                        tf.write(tr.body.data(), tr.body.size());
                    }
                    it_.write_download_info(id, item.title, item.author,
                                            item.channel_id, info.length_seconds, th,
                                            info.description);
                } else { std::remove(finalpart.c_str()); }
            }
        } catch (...) {}
        if (getenv("YTC_DEBUG")) std::fprintf(stderr, "[dl] finished ok=%d cancel=%d\n",
                                              (int)ok, (int)dl_cancel_.load());
        dl_ok_ = ok; dl_running_ = false; dl_done_ = true;
    });
    status_msg_ = std::string(i18n::tr(i18n::Str::Downloading)) + ": "
                + font_body_->ellipsize(dl_title_, win_->width() * 0.5f);
    status_until_ = SDL_GetTicks() + 60000;
}

void App::poll_download() {
    // Live progress line while downloading.
    if (dl_running_) {
        status_msg_ = std::string(i18n::tr(i18n::Str::Downloading)) + " "
                    + std::to_string(dl_progress_.load()) + "%: " + dl_title_;
        status_until_ = SDL_GetTicks() + 2000;
    }
    if (!dl_done_.exchange(false)) return;
    if (dl_thread_.joinable()) dl_thread_.join();
    refresh_downloads();
    status_msg_ = std::string(i18n::tr(dl_ok_ ? i18n::Str::DownloadDone
                                              : i18n::Str::DownloadFailed)) + ": " + dl_title_;
    status_until_ = SDL_GetTicks() + 3000;
    if (dl_ok_ && view_label_ == "Downloads") load_downloads();   // show the new tile
}
// Drop items per the hide settings: restricted channels (checked or learned) and/or
// Shorts. FAVORITE channels are never hidden by the restricted rule — favoriting is
// explicit user intent (their videos still play, with limited seeking) — but the
// Shorts rule applies everywhere.
void App::filter_hidden(std::vector<yt::SearchResult>& items) {
    // Live broadcasts are always hidden for now (playback is unreliable behind YouTube's
    // bot wall); is_live detection stays so this can be re-enabled as an opt-in later.
    items.erase(std::remove_if(items.begin(), items.end(),
        [&](const yt::SearchResult& r) {
            if (r.is_live) return true;
            return hide_restricted_ && !r.channel_id.empty() &&
                   fav_ids_.count(r.channel_id) == 0 &&
                   rcheck_.verdict(r.channel_id) == 1;
        }), items.end());
}
// Identity of the current view, used to discard a background refresh whose view
// the user has already navigated away from. Includes the active tab where content
// differs per tab (channel tabs; Home's async Playlists tab).
std::string App::view_sig() const {
    if (in_channel_view_) {
        std::string id = !channel_info_.channel_id.empty() ? channel_info_.channel_id
                                                           : cont_channel_id_;
        if (subview_playlist_.empty()) return "ch:" + id + ":t" + std::to_string(chan_tab_);
        return "pl:" + id;
    }
    if (!view_label_.empty()) return "label:" + view_label_;
    if (query_.empty()) return home_tab_ == 3 ? "home:posts"
                             : home_tab_ == 4 ? "home:pl" : "home";
    return "q:" + query_;
}

// What a background refresh fetched (stored in refresh_kind_ for poll_refresh).
enum { RK_CHANNEL, RK_PLAYLIST, RK_HOME, RK_HOME_PLAYLISTS, RK_HOME_POSTS, RK_SEARCH };

// Re-fetch whatever list is currently showing (hide-filter toggles, tab switches).
// Network views fetch on a BACKGROUND thread — the UI updates instantly and
// poll_refresh applies the new list when it arrives.
void App::refresh_current_view(bool is_retry) {
    if (!is_retry) { retry_pending_ = false; retry_attempt_ = 0; }  // fresh user action
    // Local (file-backed) views are instant — no thread needed. (Guarded on NOT
    // being in a subview: a lingering label must never hijack a channel refresh.)
    if (!in_channel_view_) {
        if (view_label_ == "Favorite Channels")  { load_favorites(); return; }
        if (view_label_ == "Favorite Playlists") { load_favorite_playlists(); return; }
        if (view_label_ == "Watch Later")        { load_watch_later(); return; }
        if (view_label_ == "Downloads")         { load_downloads(); return; }
        if (view_label_ == "History")           { load_history(); return; }
    }
    if (refresh_running_) return;              // one refresh at a time
    int kind = in_channel_view_
                  ? (!subview_playlist_.empty() ? RK_PLAYLIST : RK_CHANNEL)
                  : query_.empty() ? (home_tab_ == 3 ? RK_HOME_POSTS
                                       : home_tab_ == 4 ? RK_HOME_PLAYLISTS : RK_HOME)
                                   : RK_SEARCH;
    std::string id = kind == RK_PLAYLIST ? subview_playlist_
                   : !channel_info_.channel_id.empty() ? channel_info_.channel_id
                                                       : cont_channel_id_;
    if ((kind == RK_CHANNEL || kind == RK_PLAYLIST) && id.empty()) return;
    std::string query = query_;
    int tab = chan_tab_;
    refresh_sig_ = view_sig();
    refresh_kind_ = kind;
    // Pin the home source for this fetch: if the user flips Home Feed while it's
    // in flight, poll_refresh sees the mismatch and refetches instead of applying
    // a feed built from the old source.
    int hsrc = home_sources_;
    refresh_home_source_ = hsrc;
    refresh_running_ = true; refresh_done_ = false;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    refresh_thread_ = std::thread([this, kind, id, query, tab, hsrc]() {
        yt::Innertube::Feed f;
        yt::Innertube::HomeCursor hc;
        try {   // all paths use thread-local HttpClients inside Innertube
            if (kind == RK_CHANNEL) {
                if (tab == 0)      f = it_.channel_all_feed(id);
                else if (tab == 2) f = it_.channel_shorts_feed(id);
                else if (tab == 3) f = it_.channel_posts_feed(id);
                else if (tab == 4) f = it_.channel_playlists_feed(id);
                else               f = it_.channel_feed(id);
            }
            else if (kind == RK_PLAYLIST)        f = it_.playlist_feed(id);
            else if (kind == RK_HOME) {
                // Blend the enabled sources (bitmask: 1 fav / 2 history / 4 custom):
                // channel uploads via home_all (age-sorted), then the custom feed's
                // saved-search results interleaved in round-robin, deduped by id.
                bool fav = hsrc & 1, hist = (hsrc & 2) != 0, cust = (hsrc & 4) != 0;
                if (fav) f.items = it_.home_all({}, hist, &hc);
                else if (hist) {
                    std::vector<std::string> ids;
                    for (auto& [cid, nm] : it_.history_channels()) ids.push_back(cid);
                    if (!ids.empty()) f.items = it_.home_all(ids, false, &hc);
                }
                if (cust) {
                    std::vector<yt::SearchResult> cf = it_.custom_feed();
                    std::vector<yt::SearchResult> merged;
                    std::unordered_map<std::string, bool> seen;
                    auto key_of = [](const yt::SearchResult& r) {
                        return !r.video_id.empty() ? r.video_id
                             : !r.playlist_id.empty() ? r.playlist_id
                                                      : r.channel_id + r.title;
                    };
                    size_t n = std::max(f.items.size(), cf.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (i < f.items.size() && seen.emplace(key_of(f.items[i]), true).second)
                            merged.push_back(std::move(f.items[i]));
                        if (i < cf.size() && seen.emplace(key_of(cf[i]), true).second)
                            merged.push_back(std::move(cf[i]));
                    }
                    f.items = std::move(merged);
                }
                // Favorite playlists ride with the Favorites source: woven into the
                // All feed near the top at a wide spacing (they carry no upload date,
                // so an age-sort would bury them at the very end).
                if (fav) {
                    size_t pos = 4;
                    for (const auto& p : it_.favorite_playlists()) {
                        yt::SearchResult r;
                        r.kind = yt::SearchResult::Kind::Playlist;
                        r.playlist_id = p.id; r.title = p.title;
                        r.author = p.author; r.thumbnail_url = p.thumb;
                        if (pos >= f.items.size()) f.items.push_back(std::move(r));
                        else f.items.insert(f.items.begin() + pos, std::move(r));
                        pos += 9;
                    }
                }
                // ok unless a source SHOULD have produced content but the network
                // was unreachable (drives the auto-retry banner).
                bool expect = (fav && !it_.favorite_channel_ids().empty()) ||
                              (hist && !it_.history_channels().empty()) ||
                              (cust && !it_.custom_feed_sources().empty());
                f.ok = !expect || it_.has_visitor_data(); }
            else if (kind == RK_HOME_PLAYLISTS){ f.items = it_.home_playlists({}, &hc);
                // Favorited playlists lead the Playlists tab (deduped against the
                // favorites-channels' own playlists fetched above).
                std::vector<yt::SearchResult> lead;
                for (const auto& p : it_.favorite_playlists()) {
                    yt::SearchResult r;
                    r.kind = yt::SearchResult::Kind::Playlist;
                    r.playlist_id = p.id; r.title = p.title;
                    r.author = p.author; r.thumbnail_url = p.thumb;
                    lead.push_back(std::move(r));
                }
                if (!lead.empty()) {
                    f.items.erase(std::remove_if(f.items.begin(), f.items.end(),
                        [&](const yt::SearchResult& r) {
                            for (const auto& l : lead) if (l.playlist_id == r.playlist_id) return true;
                            return false;
                        }), f.items.end());
                    f.items.insert(f.items.begin(), std::make_move_iterator(lead.begin()),
                                   std::make_move_iterator(lead.end()));
                }
                f.ok = (it_.favorite_channel_ids().empty() && lead.empty()) || it_.has_visitor_data(); }
            else if (kind == RK_HOME_POSTS){ f.items = it_.home_posts({}, &hc);
                f.ok = it_.favorite_channel_ids().empty() || it_.has_visitor_data(); }
            else                                 f = it_.search_feed(query);
        } catch (...) {}   // never let an exception escape the thread
        { std::lock_guard<std::mutex> lk(refresh_m_);
          refresh_pending_ = std::move(f); refresh_home_cursor_ = std::move(hc); }
        refresh_running_ = false;
        refresh_done_ = true;
    });
}

void App::poll_refresh() {
    if (!refresh_done_.exchange(false)) return;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    yt::Innertube::Feed f;
    yt::Innertube::HomeCursor hc;
    { std::lock_guard<std::mutex> lk(refresh_m_);
      f = std::move(refresh_pending_); hc = std::move(refresh_home_cursor_); }
    bool fetch_ok = f.ok;
    if (refresh_kind_ == RK_HOME && refresh_home_source_ != home_sources_) {
        // Home Feed source changed while this fetch was in flight (its result is
        // for the old source): discard it and fetch the current source.
        refresh_current_view();
        return;
    }
    if (view_sig() != refresh_sig_) {
        // Stale (user switched tab/view while fetching). If the current view is
        // sitting empty waiting for content, kick off the right fetch now.
        if (results_.empty()) refresh_current_view();
        return;
    }
    switch (refresh_kind_) {
        case RK_HOME:                        // refresh of the Home master feed
            home_items_ = std::move(f.items);
            home_cursor_ = std::move(hc);    // per-channel tokens for "load more"
            cont_token_.clear();
            apply_home_tab();                // re-apply the active All/Videos/Shorts filter
            break;
        case RK_HOME_PLAYLISTS:
            home_playlists_ = f.items;       // cache (pre-filter copy)
            home_playlists_loaded_ = true;
            home_pl_cursor_ = std::move(hc); // per-channel Playlists-tab tokens
            cont_token_.clear();
            set_results(std::move(f.items));
            break;
        case RK_HOME_POSTS:
            home_posts_ = f.items;           // cache
            home_posts_loaded_ = true;
            home_posts_cursor_ = std::move(hc);
            cont_token_.clear();
            set_results(std::move(f.items));
            break;
        default:
            cont_token_ = f.continuation; cont_endpoint_ = f.endpoint;
            cont_channel_id_ = f.channel_id;
            // Channel tabs: rows omit the uploader; the view title (query_) is the channel
            // name. Fill it in BEFORE set_results so the composed tiles show it.
            if (refresh_kind_ == RK_CHANNEL)
                for (auto& r : f.items)
                    if (r.author.empty()) r.author = query_;
            set_results(std::move(f.items));
            break;
    }
    // Network fetch failed (e.g. opened before wifi reconnected) -> auto-retry with
    // incremental backoff (1,2,4,8,16, capped 30s) until it succeeds.
    if (!fetch_ok) {
        retry_attempt_ = std::min(retry_attempt_ + 1, 6);
        unsigned delay = 1000u << (retry_attempt_ - 1);
        if (delay > 30000) delay = 30000;
        retry_at_ = SDL_GetTicks() + delay;
        retry_pending_ = true;
    } else {
        retry_pending_ = false; retry_attempt_ = 0;
    }
}

// Queue background checks for channels we haven't judged yet (uses one of the
// channel's videos as the probe target; channel-only tiles wait for a video).
void App::queue_restricted_checks() {
    if (!hide_restricted_) return;
    for (const auto& r : results_)
        if (!r.channel_id.empty() && !r.video_id.empty())
            rcheck_.request(r.channel_id, r.video_id);
}

// ---------- Top-level menu (Start button) ----------
void App::open_main_menu() {
    if (menu_open_) return;               // don't stack on an already-open menu
    // Don't open behind another modal overlay — Start is wired straight to this from the
    // event loop (bypassing input()), so guard here for every caller.
    if (cast_picker_open_ || cast_confirm_remove_ || cast_manage_open_ ||
        resume_prompt_open_ || quit_prompt_open_ || desc_open_ || comments_open_ || casting_ ||
        mode_ == Mode::Search) return;
    menu_kind_ = MenuKind::Main;
    menu_items_.clear();
    using i18n::tr; using S = i18n::Str;
    menu_items_.push_back({tr(S::Home),             MenuAction::GoHome});
    menu_items_.push_back({tr(S::FavoriteChannels), MenuAction::GoFavorites});
    menu_items_.push_back({tr(S::FavoritePlaylists), MenuAction::GoFavoritePlaylists});
    menu_items_.push_back({tr(S::WatchLater),       MenuAction::GoWatchLater});
    menu_items_.push_back({tr(S::Downloads),        MenuAction::GoDownloads});
    menu_items_.push_back({tr(S::History),          MenuAction::GoHistory});
    menu_items_.push_back({tr(S::Settings),         MenuAction::GoSettings});
    menu_items_.push_back({tr(S::Exit),             MenuAction::Quit});
    menu_sel_ = 0;
    // Opening a menu over the player pauses it; resumed when the menu closes without
    // navigating away. Don't clear menu_paused_ if we're already paused (e.g. returning
    // from the Settings submenu), or we'd forget to resume on close.
    if (mode_ == Mode::Playing && !player_.paused()) { player_.set_pause(true); menu_paused_ = true; }
    menu_open_ = true;
}

// Human label for a max-height cap (0 = uncapped).
static std::string quality_label(int h) {
    if (h <= 0) return i18n::tr(i18n::Str::AutoHighest);
    return std::to_string(h) + "p";
}

// Languages offered by the Audio/Caption Language settings — the ones YouTube
// commonly auto-dubs or captions. English names on purpose: the bundled font
// covers Latin/Cyrillic/Greek only, so endonyms like العربية wouldn't render.
static const struct { const char* code; const char* name; } kLangChoices[] = {
    {"en","English"}, {"es","Spanish"}, {"pt","Portuguese"}, {"fr","French"},
    {"de","German"}, {"it","Italian"}, {"ru","Russian"}, {"uk","Ukrainian"},
    {"el","Greek"}, {"pl","Polish"}, {"tr","Turkish"}, {"ar","Arabic"},
    {"hi","Hindi"}, {"bn","Bengali"}, {"id","Indonesian"}, {"vi","Vietnamese"},
    {"th","Thai"}, {"ja","Japanese"}, {"ko","Korean"}, {"zh","Chinese"},
};
static const int kLangChoiceCount = (int)(sizeof(kLangChoices)/sizeof(kLangChoices[0]));

// Display label for an audio/cc language preference value ("app"/"orig"/"off"/code).
static std::string lang_pref_label(const std::string& v) {
    using S = i18n::Str;
    if (v == "app")  return i18n::tr(S::AppLanguage);
    if (v == "orig") return i18n::tr(S::OriginalTrack);
    if (v == "off")  return i18n::tr(S::Off);
    for (const auto& l : kLangChoices) if (v == l.code) return l.name;
    return v;   // unknown code from a hand-edited settings.json: show it raw
}

// Cycle a language preference through [specials..., kLangChoices...] by dir.
static std::string cycle_lang_pref(const std::string& cur, int dir,
                                   const std::vector<std::string>& specials) {
    std::vector<std::string> vals = specials;
    for (const auto& l : kLangChoices) vals.push_back(l.code);
    int n = (int)vals.size(), idx = 0;
    for (int i = 0; i < n; ++i) if (vals[i] == cur) { idx = i; break; }
    return vals[((idx + dir) % n + n) % n];
}

// ---------- Settings menu + its Audio / Video / Captions submenus ----------
void App::open_settings() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::Settings;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetAudioMenu),    MenuAction::GoSettingsAudio});
    menu_items_.push_back({tr(S::SetVideoMenu),    MenuAction::GoSettingsVideo});
    menu_items_.push_back({tr(S::SetCaptionsMenu), MenuAction::GoSettingsCaptions});
    menu_items_.push_back({tr(S::SetPlaybackMenu), MenuAction::GoSettingsPlayback});
    menu_items_.push_back({tr(S::SetBrowsingMenu), MenuAction::GoSettingsBrowsing});
    // Language stays top-level on purpose: a user stuck in the wrong language
    // must be able to find it without reading submenu labels.
    menu_items_.push_back({tr(S::SetLanguage) + std::string(":  ") + i18n::language_name(lang_),
                           MenuAction::CycleLanguage});
    menu_items_.push_back({tr(S::SetLinkedDevices), MenuAction::GoLinkedDevices});
    menu_sel_ = 0;
    // (Do not re-pause here; if opened from the player the main menu already did.)
    menu_open_ = true;
}

void App::open_settings_playback() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](bool b){ return std::string(tr(b ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsPlayback;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetAskResume) + std::string(":  ") + onoff(ask_resume_),
                           MenuAction::ToggleAskResume});
    menu_items_.push_back({tr(S::SetAutoplay) + std::string(":  ") + onoff(autoplay_),
                           MenuAction::ToggleAutoplay});
    menu_items_.push_back({tr(S::SetSponsorBlock) + std::string(":  ") + onoff(sponsorblock_),
                           MenuAction::ToggleSponsorBlock});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_browsing() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](bool b){ return std::string(tr(b ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsBrowsing;
    menu_items_.clear();
    const S vname[] = {S::ViewGrid, S::ViewCarousel, S::View3DCarousel, S::ViewCoverflow};
    menu_items_.push_back({tr(S::SetView) + std::string(":  ") +
                           (view_mode_ == ViewMode::Shelves ? std::string("TV") : std::string(tr(vname[(int)view_mode_]))),
                           MenuAction::CycleView});
    menu_items_.push_back({tr(S::SetHomeFeed), MenuAction::GoHomeFeedMenu});
    menu_items_.push_back({tr(S::SetCustomFeed), MenuAction::GoCustomFeed});
    menu_items_.push_back({tr(S::SetHidePaced) + std::string(":  ") + onoff(hide_restricted_),
                           MenuAction::ToggleHideRestricted});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Browsing > Home Feed: what flows into the Home grid — any mix of Favorites
// (channel uploads), History (channels you've watched), and the Custom feed's
// saved searches. At least one stays on (the toggle refuses to clear the last).
void App::open_settings_homefeed() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](int bit){ return std::string(tr(home_sources_ & bit ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsHomeFeed;
    menu_items_.clear();
    menu_items_.push_back({tr(S::Favorites) + std::string(":  ") + onoff(1),
                           MenuAction::ToggleFeedFavorites});
    menu_items_.push_back({tr(S::History) + std::string(":  ") + onoff(2),
                           MenuAction::ToggleFeedHistory});
    menu_items_.push_back({tr(S::HomeCustom) + std::string(":  ") + onoff(4),
                           MenuAction::ToggleFeedCustom});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Browsing > Custom Feed: one row per saved search ("query · filters"); A removes
// (with a Yes/No confirm page), B returns to Browsing. Searches are added from
// the search-results options menu ("Add Search to Custom Feed").
void App::open_feed_manage() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::FeedManage;
    menu_items_.clear();
    static const S kType[] = {S::Off, S::TabVideos, S::FilterChannels, S::TabPlaylists};
    static const S kDur[]  = {S::Off, S::DurShort, S::DurMedium, S::DurLong};
    static const S kDate[] = {S::Off, S::DateToday, S::DateWeek, S::DateMonth, S::DateYear};
    static const S kSort[] = {S::Off, S::FilterRelevance, S::FilterPopularity};
    for (const auto& s : it_.custom_feed_sources()) {
        std::string label = "\"" + s.query + "\"";
        if (s.type)        { label += " \xC2\xB7 "; label += tr(kType[s.type & 3]); }
        if (s.duration)    { label += " \xC2\xB7 "; label += tr(kDur[s.duration & 3]); }
        if (s.upload_date) { label += " \xC2\xB7 "; label += tr(kDate[s.upload_date % 5]); }
        if (s.sort)        { label += " \xC2\xB7 "; label += tr(kSort[s.sort % 3]); }
        menu_items_.push_back({label, MenuAction::FeedSourceRow});
    }
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_audio() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsAudio;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetVolume) + std::string(":  ") + std::to_string(volume_) + "%",
                           MenuAction::CycleVolume});
    menu_items_.push_back({tr(S::SetAudioLang) + std::string(":  ") + lang_pref_label(audio_lang_pref_),
                           MenuAction::CycleAudioLang});
    menu_items_.push_back({tr(S::SetBgOnExit) + std::string(":  ") + tr(bg_on_exit_ ? S::On : S::Off),
                           MenuAction::ToggleBgOnExit});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_video() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsVideo;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetMaxQuality) + std::string(":  ") + quality_label(play_prefs_.max_height),
                           MenuAction::CycleMaxQuality});
    // Video Decode: shown only when BOTH sides are real — the device has a v4l2
    // decoder (hwdetect) AND the loaded libavcodec has the v4l2m2m decoder (the
    // optional hwdec lib bundle, installed by ytc_setup). Software-only builds
    // keep the row hidden, so the toggle can never be a no-op.
    if (hwdec_capable_)
        menu_items_.push_back({tr(S::SetVideoDecode) + std::string(":  ")
                               + tr(hwdec_mode_ ? S::Software : S::Hardware),
                               MenuAction::CycleHwdec});
    const S aname[] = {S::AspectFit, S::AspectZoom, S::AspectStretch};
    menu_items_.push_back({tr(S::SetAspect) + std::string(":  ") + tr(aname[aspect_mode_]),
                           MenuAction::CycleAspect});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_captions() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsCaptions;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetCaptionLang) + std::string(":  ") + lang_pref_label(cc_lang_pref_),
                           MenuAction::CycleCaptionLang});
    const S sname[] = {S::SizeSmall, S::SizeMedium, S::SizeLarge};
    menu_items_.push_back({tr(S::SetCaptionSize) + std::string(":  ") + tr(sname[cc_size_]),
                           MenuAction::CycleCaptionSize});
    const S styname[] = {S::StyleWhite, S::StyleYellow, S::StyleWhiteOnBlack,
                         S::StyleBlackOnWhite, S::StyleYellowOnBlack, S::StyleYellowOnBlue};
    menu_items_.push_back({tr(S::SetCaptionStyle) + std::string(":  ") + tr(styname[cc_style_]),
                           MenuAction::CycleCaptionStyle});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Rebuild whichever settings page (or player menu) is current, for in-place
// label refreshes after a Left/Right value change.
void App::reopen_settings_menu() {
    switch (menu_kind_) {
        case MenuKind::SettingsAudio:    open_settings_audio(); break;
        case MenuKind::SettingsVideo:    open_settings_video(); break;
        case MenuKind::SettingsCaptions: open_settings_captions(); break;
        case MenuKind::SettingsPlayback: open_settings_playback(); break;
        case MenuKind::SettingsBrowsing: open_settings_browsing(); break;
        case MenuKind::SettingsHomeFeed: open_settings_homefeed(); break;
        case MenuKind::Settings:         open_settings(); break;
        default:                         open_menu(); break;
    }
}

// Change a setting's value by dir (-1 = Left, +1 = Right) and persist it.
void App::adjust_setting(MenuAction a, int dir) {
    // Search-filter rows cycle Off..N-1 (wrap) and rebuild the modal in place.
    auto cycle = [&](int& v, int n) { v = (v + dir % n + n) % n;
        int keep = menu_sel_; open_search_filters(/*snapshot=*/false); menu_sel_ = keep; };
    if (a == MenuAction::CycleFilterType)     { cycle(filt_type_, 4);     return; }
    if (a == MenuAction::CycleFilterDuration) { cycle(filt_duration_, 4); return; }
    if (a == MenuAction::CycleFilterDate)     { cycle(filt_date_, 5);     return; }
    if (a == MenuAction::CycleFilterSort)     { cycle(filt_sort_, 3);     return; }
    if (a == MenuAction::ToggleStats) {
        stats_for_nerds_ = !stats_for_nerds_;
        int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;   // rebuild label in place
        return;
    }
    if (a == MenuAction::ToggleHideRestricted) {
        hide_restricted_ = !hide_restricted_;
        it_.set_setting_int("hide_restricted", hide_restricted_ ? 1 : 0);
        if (hide_restricted_) {
            filter_hidden(results_);      // apply known verdicts now...
            if (sel_ >= (int)results_.size())
                sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            queue_restricted_checks();        // ...and start judging the rest
        } else if (mode_ != Mode::Playing) {
            refresh_current_view();           // bring the hidden items back
        }
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAskResume) {
        ask_resume_ = !ask_resume_;
        it_.set_setting_int("ask_resume", ask_resume_ ? 1 : 0);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleVolume) {
        int v = volume_ + dir * 5;
        if (v < 0) v = 0;
        if (v > 150) v = 150;
        volume_ = v;
        it_.set_setting_int("volume", volume_);
        if (mode_ == Mode::Playing) player_.set_volume(volume_);   // live if playing
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleBgOnExit) {
        bg_on_exit_ = !bg_on_exit_;
        it_.set_setting_int("bg_on_exit", bg_on_exit_ ? 1 : 0);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAutoplay) {
        autoplay_ = !autoplay_;
        it_.set_setting_int("autoplay", autoplay_ ? 1 : 0);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleFeedFavorites || a == MenuAction::ToggleFeedHistory ||
        a == MenuAction::ToggleFeedCustom) {
        int bit = a == MenuAction::ToggleFeedFavorites ? 1
                : a == MenuAction::ToggleFeedHistory   ? 2 : 4;
        int next = home_sources_ ^ bit;
        if (!next) return;                       // never all-off: keep the last source
        home_sources_ = next;
        it_.set_setting_int("home_sources", home_sources_);
        // If we're viewing Home, re-fetch with the new source.
        if (query_.empty() && view_label_.empty() && !in_channel_view_)
            refresh_current_view();
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleLanguage) {
        int n = i18n::language_count();
        lang_ = ((lang_ + dir) % n + n) % n;        // Left/Right wraps
        it_.set_setting_int("lang", lang_);
        i18n::set_language(lang_);
        it_.set_locale(i18n::language_hl(lang_), i18n::language_gl(lang_));
        // Re-fetch the current view so titles/metadata come back in the new language.
        // (No worker is running here — this is a synchronous menu action on the UI thread.)
        if (!in_channel_view_ && view_label_.empty()) { load_home(); }
        else refresh_current_view();
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;   // relabels the menu
        return;
    }
    if (a == MenuAction::ToggleSponsorBlock) {
        sponsorblock_ = !sponsorblock_;
        it_.set_setting_int("sponsorblock", sponsorblock_ ? 1 : 0);
        if (sponsorblock_ && mode_ == Mode::Playing)      // fetch now for the current video
            start_sponsorblock(now_playing_item_.video_id);
        else if (!sponsorblock_) { std::lock_guard<std::mutex> lk(sb_m_);
            sb_segments_.clear(); sb_skipped_.clear(); }
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptions) {
        int n = (int)cc_tracks_.size();
        if (n > 0) {
            cc_sel_ = (cc_sel_ + dir + (n + 1)) % (n + 1);   // 0=Off, 1..n tracks; wraps
            apply_caption_selection();
        }
        int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;   // rebuild label
        return;
    }
    if (a == MenuAction::CycleSpeed) {
        static const double steps[] = {0.25,0.5,0.75,1.0,1.25,1.5,1.75,2.0};
        int n = (int)(sizeof(steps)/sizeof(steps[0])), cur = 3;   // default 1.0x
        for (int i = 0; i < n; ++i) if (std::abs(steps[i]-playback_speed_) < 0.01) { cur = i; break; }
        cur = (cur + dir + n) % n;
        playback_speed_ = steps[cur];
        if (mode_ == Mode::Playing) player_.set_speed(playback_speed_);
        int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;    // rebuild label in place
        return;
    }
    if (a == MenuAction::CycleHwdec) {
        hwdec_mode_ = hwdec_mode_ ? 0 : 1;                 // toggle Hardware/Software
        it_.set_setting_int("hwdec", hwdec_mode_);
        player_.set_hwdec(hwdec_mode_str());
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        // hwdec is fixed at mpv init, so a live change needs a fresh stream: re-resolve
        // the current video at its position (applies the new decoder immediately).
        if (mode_ == Mode::Playing) { menu_open_ = false; menu_paused_ = false;
                                      replay_current(player_.position()); }
        return;
    }
    if (a == MenuAction::CycleAudioLang) {
        audio_lang_pref_ = cycle_lang_pref(audio_lang_pref_, dir, {"app", "orig"});
        it_.set_setting_str("audio_lang", audio_lang_pref_);
        // Default for the NEXT videos; the playing one keeps its (possibly
        // overridden) track — the player-menu Audio Track row changes it live.
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionLang) {
        cc_lang_pref_ = cycle_lang_pref(cc_lang_pref_, dir, {"app"});
        it_.set_setting_str("cc_lang", cc_lang_pref_);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionSize) {
        cc_size_ = ((cc_size_ + dir) % 3 + 3) % 3;
        it_.set_setting_int("cc_size", cc_size_);
        player_.set_caption_style(cc_size_, cc_style_);   // live if captions showing
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionStyle) {
        cc_style_ = ((cc_style_ + dir) % 6 + 6) % 6;
        it_.set_setting_int("cc_style", cc_style_);
        player_.set_caption_style(cc_size_, cc_style_);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleAudioTrack) {
        // Player menu only: pick a dub for THIS video (session-local, never saved).
        int n = (int)playing_audio_tracks_.size();
        if (n > 1) {
            std::string cur = audio_override_lang_.empty() ? playing_audio_lang_
                                                           : audio_override_lang_;
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (playing_audio_tracks_[i].lang == cur) { idx = i; break; }
            idx = ((idx + dir) % n + n) % n;
            audio_override_lang_ = playing_audio_tracks_[idx].lang;
            audio_dirty_ = true;             // re-resolve with the new track on menu close
        }
        int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;   // rebuild label
        return;
    }
    if (a == MenuAction::CycleAspect) {
        if (player_menu_kind(menu_kind_)) {
            // Player menu: this video only — live, never saved.
            aspect_override_ = ((effective_aspect() + dir) % 3 + 3) % 3;
            player_.set_aspect(aspect_override_);
            int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;
            return;
        }
        aspect_mode_ = ((aspect_mode_ + dir) % 3 + 3) % 3;     // Fit -> Zoom -> Stretch, wraps
        it_.set_setting_int("aspect", aspect_mode_);
        player_.set_aspect(effective_aspect());  // mpv property change: applies live
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleView) {
        int nv = ((int)view_mode_ + (dir < 0 ? 4 : 1)) % 5;
        view_mode_ = (ViewMode)nv;
        it_.set_setting_int("view", nv);
        carousel_pos_ = sel_;
        scroll_ = 0; ensure_visible();   // the focused tile in view in the new layout
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleMaxQuality) {
        // Ordered low->high so Right raises quality; Auto (highest) sits at the top end.
        static const int all_steps[] = {360, 480, 720, 1080, 1440, 2160, 0 /* Auto */};
        int steps[7], n = 0;
        for (int q : all_steps)   // the device ceiling drops what it cannot decode (and Auto)
            if (quality_ceil_ <= 0 || (q > 0 && q <= quality_ceil_)) steps[n++] = q;
        int cur = 0;
        int from = player_menu_kind(menu_kind_) ? effective_max_height() : play_prefs_.max_height;
        for (int i = 0; i < n; ++i) if (steps[i] == from) { cur = i; break; }
        cur = ((cur + dir) % n + n) % n;
        if (player_menu_kind(menu_kind_)) {
            // Player menu: this video only — the global Max Quality setting is
            // untouched. Re-resolve at the new quality when the menu closes.
            quality_override_ = steps[cur];
            if (mode_ == Mode::Playing) quality_dirty_ = true;
            int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;
            return;
        }
        play_prefs_.max_height = steps[cur];
        it_.set_setting_int("max_height", play_prefs_.max_height);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;   // relabel in place
    }
    // Future bool settings toggle here (dir flips the value).
}


// ---------- Description overlay ----------
void App::open_description(const yt::SearchResult& v) {
    desc_item_ = v;
    desc_title_ = v.title;
    desc_text_.clear();
    desc_lines_.clear();
    desc_scroll_ = 0; desc_wrap_w_ = 0;
    post_has_video_ = false;   // plain description, not the post-with-video layout
    desc_is_post_ = false; desc_post_id_.clear();
    desc_open_ = true;
    // Playing video: the description arrived with the resolve — no fetch needed.
    if (mode_ == Mode::Playing && !v.is_playlist() &&
        v.video_id == now_playing_item_.video_id) {
        desc_text_ = now_playing_desc_.empty() ? "(no description)" : now_playing_desc_;
        desc_loading_ = false;
        return;
    }
    // Downloaded video in the Downloads view: read the description saved in its .info
    // (offline, instant). From other views a downloaded video is treated as normal and
    // its description is fetched live below.
    if (!v.is_playlist() && !v.video_id.empty() && it_.is_downloaded(v.video_id) &&
        view_label_ == "Downloads") {
        std::string d = it_.download_description(v.video_id);
        desc_text_ = d.empty() ? "(no description)" : d;
        desc_loading_ = false;
        return;
    }
    // Grid tile: fetch async (one lightweight call; playlists via their VL browse).
    desc_loading_ = true;
    if (desc_running_) return;                   // a fetch is already in flight
    bool is_pl = v.is_playlist();
    std::string id = is_pl ? v.playlist_id : v.video_id;
    { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_id_ = id; }
    desc_running_ = true; desc_done_ = false;
    if (desc_thread_.joinable()) desc_thread_.join();
    desc_thread_ = std::thread([this, id, is_pl]() {
        std::string d;
        try { d = is_pl ? it_.playlist_description(id)
                        : it_.video_description(id); } catch (...) {}
        { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_ = std::move(d); }
        desc_running_ = false;
        desc_done_ = true;
    });
}
// SponsorBlock: fetch skip segments for the playing video off-thread. The result is
// applied in poll_sponsorblock() only if the signal still matches (video unchanged).
void App::start_sponsorblock(const std::string& video_id) {
    { std::lock_guard<std::mutex> lk(sb_m_); sb_segments_.clear(); sb_skipped_.clear(); }
    if (!sponsorblock_ || video_id.empty()) return;
    if (sb_running_) return;   // previous fetch still in flight; don't block the UI thread
    int sig = ++sb_sig_;
    if (sb_thread_.joinable()) sb_thread_.join();   // finished -> instant
    sb_running_ = true; sb_done_ = false;
    sb_thread_ = std::thread([this, video_id, sig]() {
        std::vector<yt::SponsorSegment> segs;
        try { segs = it_.sponsor_segments(video_id,
                  "sponsor,selfpromo,interaction,intro,outro,music_offtopic"); } catch (...) {}
        { std::lock_guard<std::mutex> lk(sb_m_);
          if (sig == sb_sig_) sb_pending_ = std::move(segs); }
        sb_running_ = false; sb_done_ = true;
    });
}
void App::poll_sponsorblock() {
    if (!sb_done_.exchange(false)) return;
    std::lock_guard<std::mutex> lk(sb_m_);
    sb_segments_ = std::move(sb_pending_);
    sb_pending_.clear();
    sb_skipped_.assign(sb_segments_.size(), false);
}

// Captions: fetch the track list off-thread when a video starts.
void App::start_captions(const std::string& video_id) {
    { std::lock_guard<std::mutex> lk(cc_m_); cc_tracks_.clear(); cc_pending_.clear(); }
    cc_sel_ = 0; cc_paths_.clear();
    cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();  // drop the
                                                       // old video's queued fetch
    cc_fail_pending_ = false;                          // stale failure: not this video's
    ++cc_sig_;                 // invalidate any in-flight fetch (its publish is dropped)
    cc_list_want_ = video_id;  // queue THIS video's list fetch; if the worker is still
                               // on the old video, poll_captions() starts it on finish —
                               // the old early-return here silently skipped the fetch,
                               // leaving the new video showing the old (often empty) list
    if (video_id.empty()) { cc_list_want_.clear(); return; }
    maybe_start_cc_list();
}
// Start the queued track-list fetch if the worker is free. Never blocks.
void App::maybe_start_cc_list() {
    if (cc_running_ || cc_list_want_.empty()) return;
    if (cc_thread_.joinable()) cc_thread_.join();   // worker idle -> instant
    std::string vid = std::move(cc_list_want_);
    cc_list_want_.clear();
    int sig = cc_sig_;
    cc_running_ = true; cc_done_ = false;
    cc_thread_ = std::thread([this, vid, sig]() {
        std::vector<yt::CaptionTrack> t;
        try { t = it_.caption_tracks(vid); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_); if (sig == cc_sig_) cc_pending_ = std::move(t); }
        cc_running_ = false; cc_done_ = true;
    });
}
void App::poll_captions() {
    if (!cc_done_.exchange(false)) return;
    { std::lock_guard<std::mutex> lk(cc_m_);
      cc_tracks_ = std::move(cc_pending_);
      cc_pending_.clear(); }
    // Caption Language setting: order the preferred language first (manual track
    // before auto-generated) so turning captions ON lands on it. It never turns
    // captions on by itself — cc_sel_ stays 0 (Off) here.
    std::string want = cc_lang_pref_ == "app" ? i18n::language_hl(lang_) : cc_lang_pref_;
    auto primary = [](const std::string& s) { return s.substr(0, s.find('-')); };
    auto rank = [&](const yt::CaptionTrack& t) {
        if (primary(t.language_code) != primary(want)) return 2;
        return t.auto_generated ? 1 : 0;
    };
    std::stable_sort(cc_tracks_.begin(), cc_tracks_.end(),
                     [&](const yt::CaptionTrack& a, const yt::CaptionTrack& b) {
                         return rank(a) < rank(b); });
    // Preferred language not offered at all? YouTube can machine-translate any
    // translatable track server-side (&tlang=, at fetch time) — synthesize a
    // "<Language> (translated)" entry as the FIRST option in the cycle. Prefer a
    // human-made source track over ASR (translation quality compounds).
    bool want_failed = std::find(cc_failed_langs_.begin(), cc_failed_langs_.end(), want)
                       != cc_failed_langs_.end();   // failed earlier this video: don't re-offer
    if (!want_failed && !cc_tracks_.empty() && rank(cc_tracks_.front()) == 2) {
        const yt::CaptionTrack* src = nullptr;
        for (const auto& t : cc_tracks_)
            if (t.translatable && (!src || (src->auto_generated && !t.auto_generated)))
                src = &t;
        if (src) {
            yt::CaptionTrack synth;
            synth.language_code = want;
            synth.name = lang_pref_label(want) + " (" + i18n::tr(i18n::Str::CcTranslated) + ")";
            synth.base_url = src->base_url;
            synth.auto_generated = true;
            synth.tlang = want;
            cc_tracks_.insert(cc_tracks_.begin(), std::move(synth));
        }
    }
    // Replay of the same video (quality/audio-track change): put the caption
    // selection back where it was. A new video always starts with captions Off.
    if (!cc_restore_key_.empty()) {
        bool failed = std::find(cc_failed_langs_.begin(), cc_failed_langs_.end(),
                                cc_restore_key_) != cc_failed_langs_.end();
        if (!failed)
            for (int i = 0; i < (int)cc_tracks_.size(); ++i)
                if (cc_tracks_[i].language_code == cc_restore_key_) {
                    cc_sel_ = i + 1; apply_caption_selection(); break;
                }
        cc_restore_key_.clear();
    }
    // The worker is free now — start the queued fetch, if a new video arrived
    // while it was still busy with the old one.
    maybe_start_cc_list();
    // The player options menu may be open showing "Captions: Loading..." — the
    // list just landed, so rebuild it in place with the real state.
    if (menu_open_ && player_menu_kind(menu_kind_) && mode_ == Mode::Playing) {
        int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;
    }
}
// Apply the current caption selection WITHOUT blocking: Off hides; an already-cached
// track is added instantly; an un-cached track is downloaded on a worker thread and
// installed later by poll_caption_download().
// Caption font for a track language: the bundled CJK font for Japanese (the
// device may have no font with kana/kanji), else mpv's default.
static std::string cc_font_for(const std::string& lang) {
    if ((lang == "ja" || lang.rfind("ja-", 0) == 0) && !cjk_font_path().empty())
        return "Noto Sans CJK JP";
    return "";
}
// The cache key for the current CC selection ("" = Off/invalid) — a track's language code.
std::string App::cc_current_key() const {
    if (cc_sel_ <= 0 || cc_sel_ > (int)cc_tracks_.size()) return "";
    return cc_tracks_[cc_sel_ - 1].language_code;
}
void App::apply_caption_selection() {
    if (cc_sel_ <= 0 || cc_sel_ > (int)cc_tracks_.size()) { player_.subtitles_off(); return; }
    std::string url = cc_tracks_[cc_sel_ - 1].base_url;
    std::string tlang = cc_tracks_[cc_sel_ - 1].tlang;
    std::string key = cc_current_key();
    auto it = cc_paths_.find(key);
    if (it != cc_paths_.end()) { player_.add_subtitle(it->second, cc_font_for(key)); return; }  // cached
    // Not cached: fetch off-thread. Hide until it arrives; show a brief status.
    // Queue the request instead of joining a busy worker — a join here would stall
    // the UI thread for a full network round-trip when cycling tracks quickly.
    player_.subtitles_off();
    status_msg_ = i18n::tr(i18n::Str::LoadingCaptions); status_until_ = SDL_GetTicks() + 4000;
    cc_dl_want_url_ = url; cc_dl_want_key_ = key;   // latest selection wins
    cc_dl_want_tlang_ = tlang;
    maybe_start_cc_download();
}
// Start the queued caption fetch if the worker is free. NEVER blocks: while a
// download runs this is a no-op — poll_caption_download() calls back here when
// it finishes, picking up whatever request is queued by then (rapid cycling
// overwrites the slot, so only the newest selection gets fetched next).
void App::maybe_start_cc_download() {
    if (cc_dl_running_) return;
    if (cc_dl_thread_.joinable()) cc_dl_thread_.join();   // worker idle -> instant
    if (cc_dl_want_key_.empty()) return;
    std::string url = std::move(cc_dl_want_url_), key = std::move(cc_dl_want_key_);
    std::string tlang = std::move(cc_dl_want_tlang_);
    cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();
    cc_dl_running_ = true; cc_dl_done_ = false;
    int sig = cc_sig_;   // which video this fetch belongs to
    cc_dl_thread_ = std::thread([this, url, key, tlang, sig]() {
        std::string vtt;
        try { vtt = it_.caption_vtt(url, tlang); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_);
          cc_dl_vtt_ = std::move(vtt); cc_dl_lang_ = key; cc_dl_res_sig_ = sig; }
        cc_dl_running_ = false; cc_dl_done_ = true;
    });
}
// Install a finished VTT — but only if that selection is still active — then
// kick off whatever request queued up while the worker was busy.
void App::poll_caption_download() {
    if (!cc_dl_done_.exchange(false)) return;
    std::string vtt, lang; int res_sig;
    { std::lock_guard<std::mutex> lk(cc_m_); vtt = std::move(cc_dl_vtt_); lang = cc_dl_lang_;
      res_sig = cc_dl_res_sig_; cc_dl_vtt_.clear(); }
    if (res_sig != cc_sig_) { maybe_start_cc_download(); return; }  // a previous video's
                                                                    // fetch: discard it
    bool still_selected = !lang.empty() && lang == cc_current_key();
    if (vtt.empty()) {
        if (still_selected && cc_dl_want_key_.empty()) {
            cc_sel_ = 0; player_.subtitles_off();
            // Remember the failure for this video, and drop any synthetic
            // (translated) entry for that language — it will never play, so it
            // must not sit in the cycle inviting the same failure again. Real
            // tracks stay listed (a transient network error shouldn't hide them).
            cc_failed_langs_.push_back(lang);
            cc_tracks_.erase(std::remove_if(cc_tracks_.begin(), cc_tracks_.end(),
                                 [&](const yt::CaptionTrack& t) {
                                     return !t.tlang.empty() && t.language_code == lang;
                                 }),
                             cc_tracks_.end());
            // Don't flash the toast now: with the options menu up the user may not
            // see it before it expires, leaving a mysterious "Captions: Off". The
            // frame loop shows it once the menu is closed.
            cc_fail_pending_ = true;
            if (menu_open_ && player_menu_kind(menu_kind_)) {   // relabel the row: Off
                int keep = menu_sel_; reopen_player_menu(); menu_sel_ = keep;
            }
        }
    } else {
        std::string path = "/tmp/ytc_cc_" + lang + ".vtt";
        { std::ofstream o(path, std::ios::binary); o << vtt; }
        cc_paths_[lang] = path;
        // A queued request for what just landed (selecting a track whose prefetch
        // was in flight) is satisfied — drop it instead of fetching it again.
        if (cc_dl_want_key_ == lang) {
            cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();
        }
        if (kDbg) std::fprintf(stderr, "[cc] %s -> %zu bytes (%s)\n", lang.c_str(), vtt.size(),
                               still_selected ? "applied" : "cached");
        if (still_selected) player_.add_subtitle(path, cc_font_for(lang));   // user may have moved on -> just cache
    }
    maybe_start_cc_download();
}

// Channel description in the same overlay (used from playlist rows, where each
// video names a different uploader).
void App::open_channel_description(const std::string& channel_id, const std::string& name) {
    desc_title_ = name;
    desc_text_.clear();
    desc_lines_.clear();
    desc_scroll_ = 0; desc_wrap_w_ = 0;
    post_has_video_ = false;   // plain description, not the post-with-video layout
    desc_is_post_ = false; desc_post_id_.clear();
    desc_open_ = true;
    desc_loading_ = true;
    if (desc_running_) return;
    std::string id = channel_id;
    desc_running_ = true; desc_done_ = false;
    if (desc_thread_.joinable()) desc_thread_.join();
    desc_thread_ = std::thread([this, id]() {
        std::string d;
        try { d = it_.channel_info(id).description; } catch (...) {}
        { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_ = std::move(d); }
        desc_running_ = false;
        desc_done_ = true;
    });
}

// Community post: the full text is already on the tile — instant overlay.
void App::open_post(const yt::SearchResult& p) {
    desc_title_ = p.published_text.empty() ? "Post"
                                           : ("Post  -  " + p.published_text
                                              + (p.view_count_text.empty() ? ""
                                                 : "  -  " + p.view_count_text));
    desc_text_ = p.post_text.empty() ? "(no text)" : p.post_text;
    desc_lines_.clear(); desc_wrap_w_ = 0; desc_scroll_ = 0;
    desc_loading_ = false;
    // Attached video: show its thumbnail (selected) on top; A plays it, Down -> text.
    post_has_video_ = !p.video_id.empty();
    desc_is_post_ = true; desc_post_id_ = p.post_id; desc_post_channel_ = p.channel_id;
    post_thumb_url_ = p.thumbnail_url;
    post_focus_ = 0;
    if (post_has_video_ && !post_thumb_url_.empty()) thumbs_.request(post_thumb_url_);
    post_item_ = p;
    post_images_ = p.post_images;
    post_img_ = 0; post_img_pos_ = 0; post_img_vel_ = 0; post_img_last_ = 0;
    post_full_ = false;
    for (const std::string& u : post_images_) thumbs_.request(u);
    desc_open_ = true;   // desc_paused_ is managed by the caller (menu path)
}

void App::poll_description() {
    if (!desc_done_.exchange(false)) return;
    if (desc_thread_.joinable()) desc_thread_.join();
    std::string d;
    { std::lock_guard<std::mutex> lk(desc_m_); d = std::move(desc_pending_); }
    if (!desc_open_ || !desc_loading_) return;   // overlay closed meanwhile — discard
    desc_text_ = d.empty() ? "(no description)" : d;
    desc_lines_.clear(); desc_wrap_w_ = 0;       // re-wrap on next render
    desc_loading_ = false;
}

// Line-break rules for CJK text, which has no spaces: a break may fall between
// any two characters when either is CJK, except before closing punctuation, small
// kana and the prolonged-sound mark, or after an opening bracket (kinsoku shori).
static bool is_cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0x20000 && c <= 0x3FFFF);
}
static bool cjk_no_break_before(uint32_t c) {
    static const char32_t kClose[] =
        U"、。，．・：；？！ー」』）］｝〕〉》】〙〗〟’”ゝゞヽヾ々〻"
        U"ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ)]},.!?:;";
    for (char32_t k : kClose) if (k == c) return true;
    return false;
}
static bool cjk_no_break_after(uint32_t c) {
    static const char32_t kOpen[] = U"「『（［｛〔〈《【〘〖〝‘“([{";
    for (char32_t k : kOpen) if (k == c) return true;
    return false;
}

// Greedy word-wrap honoring \n. Latin text breaks at spaces; CJK between
// characters (rules above). Very long unbreakable runs (URLs) hard-break at
// character boundaries so nothing overflows the panel.
std::vector<std::string> wrap_text(const gfx::Font& f, const std::string& text,
                                          float max_w) {
    std::vector<std::string> lines;
    struct Ch { size_t at; uint32_t cp; };
    std::vector<Ch> ch;
    std::vector<float> pre;   // pre[k] = width of ch[0..k)
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        size_t para_end = nl == std::string::npos ? text.size() : nl;
        ch.clear(); pre.assign(1, 0.f);
        for (size_t i = pos; i < para_end; ) {
            size_t at = i;
            uint32_t cp = gfx::utf8_next(text, i);
            if (i > para_end) i = para_end;          // truncated sequence at the edge
            const gfx::Font::Glyph* g = f.glyph(cp);
            ch.push_back({at, cp});
            pre.push_back(pre.back() + (g ? g->xadv : 0));
        }
        auto byte_at = [&](size_t k) { return k < ch.size() ? ch[k].at : para_end; };
        auto emit = [&](size_t a, size_t b) {     // ch[a..b), trailing spaces trimmed
            while (b > a && ch[b - 1].cp == ' ') --b;
            lines.push_back(text.substr(byte_at(a), byte_at(b) - byte_at(a)));
        };
        size_t line_start = 0, brk = 0;           // brk: last break opportunity (0 = none)
        for (size_t i = 0; i < ch.size(); ++i) {
            uint32_t cp = ch[i].cp;
            if (i > line_start) {
                uint32_t prev = ch[i - 1].cp;
                if (cp != ' ' && (prev == ' ' ||
                        ((is_cjk(prev) || is_cjk(cp)) &&
                         !cjk_no_break_before(cp) && !cjk_no_break_after(prev))))
                    brk = i;
            }
            if (cp == ' ' || pre[i + 1] - pre[line_start] <= max_w || i == line_start)
                continue;
            size_t cut = brk > line_start ? brk : i;   // no opportunity: hard break
            emit(line_start, cut);
            line_start = cut;
            while (line_start < i && ch[line_start].cp == ' ') ++line_start;
            brk = 0;
            // The run carried over may itself be too long (URL): loop re-checks it.
            if (pre[i + 1] - pre[line_start] > max_w && i > line_start) {
                emit(line_start, i);
                line_start = i;
            }
        }
        emit(line_start, ch.size());              // may be "" (blank paragraph line)
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}


// ---------- Comments overlay ----------
void App::open_comments(const yt::SearchResult& t) {
    comments_is_post_ = t.is_post();
    comments_target_id_ = comments_is_post_ ? t.post_id : t.video_id;
    comments_channel_id_ = t.channel_id;
    comments_title_ = i18n::tr(i18n::Str::CommentsTitle);
    comments_.clear(); comment_lines_.clear(); comment_units_.clear(); comments_wrap_w_ = 0;
    comments_scroll_ = 0; comments_cont_.clear(); comments_total_.clear();
    comments_sel_ = 0; comments_dirty_ = true; comments_reply_idx_ = -1;
    comments_sort_ = 0; comments_sort_top_.clear(); comments_sort_newest_.clear();
    comments_next_sort_token_.clear();
    comments_gen_++;                     // invalidate any in-flight fetch from a prior open
    comments_open_ = true;
    comments_loading_ = true;
    if (comments_target_id_.empty()) { comments_loading_ = false; return; }
    comments_want_fetch_ = true;         // queue the first page; started when the thread is free
    maybe_start_comment_fetch();
}

// Start the queued first-page fetch once no page fetch is in flight. The running one
// (from a previous video) finishes into a stale generation and is discarded.
void App::maybe_start_comment_fetch() {
    if (comments_running_ || !comments_want_fetch_) return;
    comments_want_fetch_ = false;
    comments_running_ = true; comments_done_ = false;
    if (comments_thread_.joinable()) comments_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string id = comments_target_id_, cid = comments_channel_id_; bool is_post = comments_is_post_;
    // A sort switch reloads page 1 with an explicit sort continuation; a fresh open
    // resolves the section token (and the count + sort tokens) from scratch.
    std::string sort_tok = comments_next_sort_token_; comments_next_sort_token_.clear();
    comments_thread_ = std::thread([this, id, cid, is_post, gen, sort_tok]() {
        yt::CommentPage pg;
        try {
            pg = is_post ? it_.post_comments(id, cid, sort_tok) : it_.video_comments(id, sort_tok);
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_pending_ = std::move(pg); comments_pending_reset_ = true; comments_pending_gen_ = gen; }
        comments_running_ = false; comments_done_ = true;
    });
}

void App::load_more_comments() {
    if (comments_running_ || comments_cont_.empty() || comments_target_id_.empty()) return;
    comments_running_ = true; comments_done_ = false;
    if (comments_thread_.joinable()) comments_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string id = comments_target_id_, cid = comments_channel_id_, cont = comments_cont_;
    bool is_post = comments_is_post_;
    comments_thread_ = std::thread([this, id, cid, cont, is_post, gen]() {
        yt::CommentPage pg;
        try { pg = is_post ? it_.post_comments(id, cid, cont) : it_.video_comments(id, cont); }
        catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_pending_ = std::move(pg); comments_pending_reset_ = false; comments_pending_gen_ = gen; }
        comments_running_ = false; comments_done_ = true;
    });
}

void App::poll_comments_page() {
    if (comments_done_.exchange(false)) {
        if (comments_thread_.joinable()) comments_thread_.join();
        yt::CommentPage pg; bool reset; unsigned gen;
        { std::lock_guard<std::mutex> lk(comments_m_);
          pg = std::move(comments_pending_); reset = comments_pending_reset_;
          gen = comments_pending_gen_; }
        // Apply only if it belongs to the current open (else it's a stale prior video).
        if (gen == comments_gen_.load() && comments_open_) {
            if (reset) {
                comments_ = std::move(pg.items);
                if (!pg.total.empty()) comments_total_ = pg.total;   // keep count across sort
                if (!pg.sort_top.empty())    comments_sort_top_    = pg.sort_top;
                if (!pg.sort_newest.empty()) comments_sort_newest_ = pg.sort_newest;
            } else comments_.insert(comments_.end(),
                                  std::make_move_iterator(pg.items.begin()),
                                  std::make_move_iterator(pg.items.end()));
            comments_cont_ = pg.continuation;
            comments_loading_ = false;
            comments_dirty_ = true;   // re-wrap / rebuild units
        }
    }
    maybe_start_comment_fetch();   // start a queued fetch once the thread is free
}

// Load replies for one comment at a time in the background: user-expanded threads
// first, then threads where the creator replied (shown inline as a preview).
void App::pump_reply_loads() {
    if (!comments_open_ || comments_reply_running_) return;
    int idx = -1;
    for (int i = 0; i < (int)comments_.size(); ++i) {
        yt::Comment& c = comments_[i];
        if (c.replies_loaded || c.replies_loading || c.reply_token.empty()) continue;
        if (c.expanded) { idx = i; break; }            // priority: user expanded
        if (c.has_creator_reply && idx < 0) idx = i;   // otherwise: creator-reply preview
    }
    if (idx < 0) return;
    comments_[idx].replies_loading = true;
    comments_reply_running_ = true; comments_reply_done_ = false;
    comments_reply_idx_ = idx;
    if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string tok = comments_[idx].reply_token; bool is_post = comments_is_post_;
    comments_reply_thread_ = std::thread([this, tok, is_post, gen]() {
        yt::CommentPage pg;
        try { pg = it_.comment_replies(tok, is_post); } catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_reply_pending_ = std::move(pg); comments_reply_pending_gen_ = gen; }
        comments_reply_running_ = false; comments_reply_done_ = true;
    });
}

// Expand or collapse the replies under the selected top-level comment.
void App::toggle_comment_replies() {
    if (comments_sel_ < 0 || comments_sel_ >= (int)comment_units_.size()) return;
    const CommentUnit& u = comment_units_[comments_sel_];
    if (!u.is_top || u.top_idx < 0 || u.top_idx >= (int)comments_.size()) return;
    yt::Comment& c = comments_[u.top_idx];
    if (c.reply_token.empty()) return;                 // no replies to show
    c.expanded = !c.expanded;                          // pump_reply_loads() fetches them
    comments_dirty_ = true;
}

// Switch the comment sort (Top <-> Newest) and reload page 1 in that order.
void App::toggle_comment_sort() {
    if (comments_sort_top_.empty() || comments_sort_newest_.empty()) return;   // no sort here
    comments_sort_ ^= 1;
    std::string tok = comments_sort_ ? comments_sort_newest_ : comments_sort_top_;
    comments_.clear(); comment_lines_.clear(); comment_units_.clear();
    comments_scroll_ = 0; comments_sel_ = 0; comments_cont_.clear();
    comments_dirty_ = true; comments_reply_idx_ = -1;
    comments_gen_++;                          // discard in-flight results from the old sort
    comments_loading_ = true;
    comments_next_sort_token_ = tok;          // page 1 in the chosen order
    comments_want_fetch_ = true;
    maybe_start_comment_fetch();
}

void App::poll_comments() {
    if (comments_reply_done_.exchange(false)) {
        if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
        yt::CommentPage pg; unsigned gen;
        { std::lock_guard<std::mutex> lk(comments_m_);
          pg = std::move(comments_reply_pending_); gen = comments_reply_pending_gen_; }
        if (gen == comments_gen_.load() && comments_open_ && comments_reply_idx_ >= 0 &&
            comments_reply_idx_ < (int)comments_.size()) {   // ignore replies from a prior open
            comments_[comments_reply_idx_].replies = std::move(pg.items);
            comments_[comments_reply_idx_].replies_loaded = true;
            comments_[comments_reply_idx_].replies_loading = false;
            comments_dirty_ = true;
        }
        comments_reply_idx_ = -1;
    }
    poll_comments_page();
    pump_reply_loads();
}

void App::close_comments() {
    comments_open_ = false;
    if (comments_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
    comments_paused_ = false;
}

// brick-os DESIGN.md 7.5 (v2-YouTubeComments / -Description): a 640 panel on the right over the dimmed page,
// radius 28, under the status bar; the hint bar below.
void App::design_side_panel(gfx::Renderer& rn, gfx::Rect& panel) {
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    rn.quad({0, 0, (float)W, (float)H}, dzc::bg.with_a(0.6f));
    design_status_bar(rn);
    panel = {W - 16*u - 640*u, 64*u, 640*u, H - 64*u - 76*u};
    rn.round_rect(panel, 28*u, gfx::Color::rgb(0x1C1C1F));
}

void App::render_comments(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(win_->width(), H);
    gfx::Rect P;
    design_side_panel(rn, P);
    const gfx::Color text2 = gfx::Color::rgb(0xB4B4BA);
    const float pad = 28*u, tx = P.x + pad;
    const gfx::Font &hf = dz(DZ_PANEL_T), &sf = dz(DZ_SHEET_M), &nf = dz(DZ_CM_NAME), &mf = dz(DZ_CM_META),
                    &bf = dz(DZ_CM_TEXT), &rf = dz(DZ_CM_REPLY);
    rn.text(hf, hf.ellipsize(comments_title_, P.w - 2*pad), tx, P.y + 20*u, dzc::white);
    std::string sub = comments_total_.empty() ? std::to_string(comments_.size()) : comments_total_;
    sub += " " + comments_title_;
    if (!comments_sort_top_.empty() && !comments_sort_newest_.empty())
        sub += std::string(" \xC2\xB7 ") + i18n::tr(comments_sort_ ? S::CommentSortNewest : S::CommentSortTop);
    float sub_y = P.y + 20*u + hf.line_height();
    rn.text(sf, sf.ellipsize(sub, P.w - 2*pad), tx, sub_y, text2);
    float body_top = sub_y + sf.line_height() + 18*u, body_bot = P.y + P.h - 20*u;
    Hints hints;
    hints.push_back({"A", i18n::tr(S::HintReplies)});
    hints.push_back({"B", i18n::tr(S::HintClose)});
    if (!comments_sort_top_.empty() && !comments_sort_newest_.empty()) hints.push_back({"X", i18n::tr(S::HintSort)});
    if (comments_.empty()) {
        rn.text(bf, i18n::tr(comments_loading_ ? S::CommentsLoading : S::CommentsEmpty), tx, body_top, text2);
        design_hint_bar(rn, {{"B", i18n::tr(S::HintClose)}});
        rn.end();
        return;
    }
    const float av = 48*u, ind = 56*u;   // avatar; a reply's indent
    const float text_x = av + 16*u;      // from the unit's left edge
    float wrap_w = P.w - 2*pad - text_x - 16*u;
    if (comments_dirty_ || comments_wrap_w_ != wrap_w) {
        comment_lines_.clear(); comment_units_.clear();
        auto emit = [&](const yt::Comment& c, int indent) {
            std::string name = c.author.empty() ? "\xE2\x80\x94" : c.author;
            comment_lines_.push_back({c.time + "\x1F" + c.likes, 0, indent, name, c.is_creator});
            for (auto& l : wrap_text(bf, c.text, wrap_w - indent*ind))
                comment_lines_.push_back({l, 1, indent, "", false});
        };
        for (int i = 0; i < (int)comments_.size(); ++i) {
            const yt::Comment& c = comments_[i];
            CommentUnit cu{(int)comment_lines_.size(), 0, true, i};
            emit(c, 0);
            if (!c.expanded && c.has_creator_reply && c.replies_loaded)
                for (const auto& r : c.replies) if (r.is_creator) emit(r, 1);
            if (!c.reply_token.empty()) {
                std::string tog = !c.expanded
                    ? std::string("\xE2\x96\xB8 ") + std::to_string(c.reply_count) + " " + i18n::tr(S::CommentReplies)
                    : (c.replies_loaded ? std::string("\xE2\x96\xBE ") + i18n::tr(S::CommentHideReplies)
                                        : std::string("\xE2\x96\xBE ") + i18n::tr(S::CommentsLoading));
                comment_lines_.push_back({tog, 3, 0});
            }
            cu.line_count = (int)comment_lines_.size() - cu.line_start;
            comment_units_.push_back(cu);
            comment_lines_.push_back({"", 2, 0});
            if (c.expanded)
                for (const auto& r : c.replies) {
                    CommentUnit ru{(int)comment_lines_.size(), 0, false, i};
                    emit(r, 1);
                    ru.line_count = (int)comment_lines_.size() - ru.line_start;
                    comment_units_.push_back(ru);
                    comment_lines_.push_back({"", 2, 1});
                }
        }
        comments_wrap_w_ = wrap_w; comments_dirty_ = false;
    }
    if (comments_sel_ >= (int)comment_units_.size()) comments_sel_ = (int)comment_units_.size() - 1;
    if (comments_sel_ < 0) comments_sel_ = 0;
    const float lh = bf.line_height() + 4*u;
    float total = comment_lines_.size() * lh, view = body_bot - body_top;
    float max_scroll = total > view ? total - view : 0;
    if (!comment_units_.empty()) {
        const CommentUnit& su = comment_units_[comments_sel_];
        float s0 = su.line_start * lh - 12*u, s1 = (su.line_start + su.line_count) * lh + 12*u;
        if (s0 < comments_scroll_) comments_scroll_ = s0;
        if (s1 > comments_scroll_ + view) comments_scroll_ = s1 - view;
    }
    comments_scroll_ = std::max(0.f, std::min(comments_scroll_, max_scroll));
    if (comments_sel_ >= (int)comment_units_.size() - 3 && !comments_cont_.empty() && !comments_running_)
        load_more_comments();
    // the focused comment: the focus colour (radius 22)
    int sel_first = -1, sel_last = -1;
    if (!comment_units_.empty()) {
        const CommentUnit& su = comment_units_[comments_sel_];
        sel_first = su.line_start; sel_last = su.line_start + su.line_count - 1;
        float hy = body_top + su.line_start * lh - comments_scroll_ - 12*u, hh = su.line_count * lh + 24*u;
        float ix = P.x + 16*u + comment_lines_[su.line_start].indent * ind;
        float top = std::max(hy, body_top - 12*u), bot = std::min(hy + hh, body_bot + 8*u);
        if (bot > top) rn.round_rect({ix, top, P.x + P.w - 16*u - ix, bot - top}, 22*u, bos_.focus);
    }
    static const uint32_t kAv[6] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF9F0A, 0xFF375F, 0x30B0C7};
    float y = body_top - comments_scroll_;
    for (int li = 0; li < (int)comment_lines_.size(); ++li) {
        const auto& cl = comment_lines_[li];
        bool foc = li >= sel_first && li <= sel_last;
        gfx::Color tc = foc ? bos_.onfocus : dzc::white, dc = foc ? bos_.onfocus : text2;
        float x0 = tx + cl.indent * ind;
        if (y >= body_top - 0.5f && y <= body_bot - lh * 0.6f) {
            if (cl.kind == 0) {
                // the letter circle: the name's first character (after @), a colour from the name
                std::string nm = cl.author;
                size_t b = nm[0] == '@' ? 1 : 0, e = b + 1;
                while (e < nm.size() && ((unsigned char)nm[e] & 0xC0) == 0x80) e++;
                unsigned hsh = 0;
                for (char ch : nm) hsh = hsh * 31 + (unsigned char)ch;
                float a = cl.indent ? 36*u : av;
                rn.round_rect({x0, y, a, a}, a / 2, gfx::Color::rgb(kAv[hsh % 6]));
                std::string letter = nm.substr(b, e - b);
                rn.text(nf, letter, x0 + (a - nf.text_width(letter)) / 2, y + (a - nf.line_height()) / 2, dzc::white);
                float x = x0 + text_x;
                rn.text(nf, nf.ellipsize(nm, wrap_w * 0.6f), x, y, cl.creator && !foc ? bos_.primary : tc);
                x += nf.text_width(nf.ellipsize(nm, wrap_w * 0.6f));
                size_t sp = cl.text.find('\x1F');
                std::string time = cl.text.substr(0, sp), likes = sp == std::string::npos ? "" : cl.text.substr(sp + 1);
                float my = y + (nf.line_height() - mf.line_height()) / 2;
                if (!time.empty()) {
                    std::string t = " \xC2\xB7 " + time;
                    rn.text(mf, t, x, my, dc);
                    x += mf.text_width(t);
                }
                if (likes.find_first_of("0123456789") != std::string::npos) {
                    rn.text(mf, " \xC2\xB7 ", x, my, dc);
                    x += mf.text_width(" \xC2\xB7 ");
                    bos_icon(rn, "thumb", x, y + (nf.line_height() - 20*u) / 2, 20*u, dc);
                    rn.text(mf, " " + likes, x + 20*u, my, dc);
                }
            } else if (cl.kind == 1) {
                rn.text(bf, cl.text, x0 + text_x, y, tc);
            } else if (cl.kind == 3) {
                rn.text(rf, cl.text, x0 + text_x, y + 4*u, foc ? bos_.onfocus : bos_.primary);
            }
        }
        y += lh;
    }
    design_hint_bar(rn, hints);
    rn.end();
}

void App::render_description(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(win_->width(), H);
    gfx::Rect P;
    design_side_panel(rn, P);
    const gfx::Color text2 = gfx::Color::rgb(0xB4B4BA);
    const float pad = 28*u, tx = P.x + pad, wrap_w = P.w - 2*pad;
    const gfx::Font &hf = dz(DZ_PANEL_T), &sf = dz(DZ_SHEET_M), &tf = dz(DZ_DESC_T), &pf = dz(DZ_CHIP_SEL),
                    &bf = dz(DZ_DESC);
    rn.text(hf, i18n::tr(desc_is_post_ ? S::TabPosts : S::DescTitle), tx, P.y + 20*u, dzc::white);
    float y = P.y + 20*u + hf.line_height();
    if (!desc_item_.author.empty()) {
        rn.text(sf, sf.ellipsize(desc_item_.author, wrap_w), tx, y, text2);
        y += sf.line_height();
    }
    y += 14*u;
    std::vector<std::string> tl = wrap_text(tf, desc_title_, wrap_w);
    for (int i = 0; i < (int)tl.size() && i < 2; ++i) {
        std::string l = tl[i];
        if (i == 1 && tl.size() > 2) l = tf.ellipsize(l + " \xE2\x80\xA6", wrap_w);
        rn.text(tf, l, tx, y, dzc::white);
        y += tf.line_height();
    }
    // pills: views, date (surface2, 24/600, height 40)
    {
        float x = tx;
        y += 10*u;
        bool any = false;
        for (const std::string& p : {desc_item_.view_count_text, desc_item_.published_text}) {
            if (p.empty()) continue;
            std::string t = pf.ellipsize(p, wrap_w / 2);
            float w = pf.text_width(t) + 32*u;
            if (x + w > tx + wrap_w) break;
            rn.round_rect({x, y, w, 40*u}, 20*u, gfx::Color::rgb(0x2C2C30));
            rn.text(pf, t, x + 16*u, y + (40*u - pf.line_height()) / 2, dzc::white);
            x += w + 10*u;
            any = true;
        }
        if (any) y += 40*u;
        y += 16*u;
    }
    float body_top = y, body_bot = P.y + P.h - 20*u;
    bool has_comments_btn = desc_is_post_ && !desc_post_id_.empty();
    int vid_idx = post_has_video_ ? 0 : -1;
    int com_idx = has_comments_btn ? (post_has_video_ ? 1 : 0) : -1;
    if (post_has_video_) {   // a post's video: a thumbnail (A plays it), the ring when focused
        float tw = std::min(wrap_w, 320*u), th = tw * 9.f / 16.f;
        gfx::Rect vr{tx, body_top + 8*u, tw, th};
        rn.quad(vr, gfx::Color::rgb(0x2C2C30));
        gfx::Texture* vt = post_thumb_url_.empty() ? nullptr : thumbs_.get(post_thumb_url_);
        if (vt) rn.textured_cover(vr, *vt, {1, 1, 1, 1});
        rn.round_corners(vr, 18*u, gfx::Color::rgb(0x1C1C1F));
        if (post_focus_ == vid_idx) design_ring(rn, vr, 18*u);
        body_top = vr.y + vr.h + 20*u;
    }
    if (has_comments_btn) {   // a tile: Comments
        bool sel = post_focus_ == com_idx;
        float bw = std::min(wrap_w, 320*u), bh = 64*u;
        gfx::Rect br{tx, body_top, bw, bh};
        rn.round_rect(br, 22*u, sel ? bos_.focus : gfx::Color::rgb(0x2C2C30));
        gfx::Color c = sel ? bos_.onfocus : dzc::white;
        bos_icon(rn, "comment", br.x + 20*u, br.y + (bh - 28*u) / 2, 28*u, sel ? c : gfx::Color::rgb(0x30D158));
        rn.text(dz(DZ_TILE), i18n::tr(S::HintComments), br.x + 60*u, br.y + (bh - dz(DZ_TILE).line_height()) / 2, c);
        body_top = br.y + bh + 20*u;
    }
    if (desc_loading_) {
        rn.text(bf, i18n::tr(S::LoadingDescription), tx, body_top, text2);
    } else {
        if (desc_lines_.empty() || desc_wrap_w_ != wrap_w) {
            desc_lines_ = wrap_text(bf, desc_text_, wrap_w);
            desc_wrap_w_ = wrap_w;
        }
        float lh = bf.line_height() + 6*u;
        float total = desc_lines_.size() * lh, view = body_bot - body_top;
        float max_scroll = total > view ? total - view : 0;
        desc_scroll_ = std::max(0.f, std::min(desc_scroll_, max_scroll));
        float ly = body_top - desc_scroll_;
        for (const auto& l : desc_lines_) {
            if (ly >= body_top - 0.5f && ly <= body_bot - lh * 0.6f) rn.text(bf, l, tx, ly, text2);
            ly += lh;
        }
        if (max_scroll > 0) {   // a thin scroll mark at the panel's right edge
            float knob = std::max(32*u, view * (view / total));
            float ky = body_top + (view - knob) * (desc_scroll_ / max_scroll);
            rn.round_rect({P.x + P.w - 12*u, ky, 4*u, knob}, 2*u, gfx::Color::rgb(0x6E6E73));
        }
    }
    Hints right;
    if (post_has_video_ || has_comments_btn) right.push_back({"A", i18n::tr(S::HintOpen)});
    right.push_back({"B", i18n::tr(S::HintClose)});
    design_hint_bar(rn, right, {{"\xE2\x86\x91\xE2\x86\x93", i18n::tr(S::HintScroll)}});
    rn.end();
}


// ---------- Context options menu ----------
void App::open_menu() {
    menu_kind_ = MenuKind::Context;
    // Target: the playing video (in player) or the highlighted grid item. On the
    // search screen with no results we still open the menu (Search Filters only) so a
    // filter that returns nothing doesn't strand the user with no way back to filters.
    bool have_item = true;
    if (mode_ == Mode::Playing) menu_target_ = now_playing_item_;
    else { const yt::SearchResult* v = selected();
           if (v) menu_target_ = *v;
           else if (search_tabs_active()) { menu_target_ = yt::SearchResult{}; have_item = false; }
           else return; }

    menu_items_.clear();
    using i18n::tr; using S = i18n::Str;
    const yt::SearchResult& t = menu_target_;
    // On the search-results screen, the options menu leads with Search Filters,
    // plus saving the current query (+ its filters) into the Custom home feed.
    if (search_tabs_active() && mode_ != Mode::Playing) {
        menu_items_.push_back({tr(S::MenuSearchFilters), MenuAction::OpenSearchFilters});
        if (!query_.empty())
            menu_items_.push_back({tr(S::MenuAddSearchFeed), MenuAction::AddSearchToFeed});
    }
    if (have_item) {
    if (t.is_post()) {
        menu_items_.push_back({tr(S::ReadPost), MenuAction::ShowDescription});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::PlayAttachedVideo), MenuAction::PlayPostVideo});
        if (!t.post_id.empty())
            menu_items_.push_back({tr(S::MenuShowComments), MenuAction::ShowComments});
    } else if (t.is_playlist()) {
        // Order everywhere: collect -> learn about it -> go/do -> heavy actions.
        bool pfav = fav_pl_ids_.count(t.playlist_id) > 0;
        menu_items_.push_back({tr(pfav ? S::RemovePlaylistFav : S::AddPlaylistFav),
                               MenuAction::FavoritePlaylistToggle});
        bool wl = wl_ids_.count(t.playlist_id) > 0;
        menu_items_.push_back({tr(wl ? S::RemoveWatchLater : S::AddWatchLater),
                               MenuAction::WatchLaterToggle});
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (!t.channel_id.empty())
            menu_items_.push_back({tr(S::ShowChannelDescription),
                                   MenuAction::ShowChannelDescription});
        if (!t.channel_id.empty())
            menu_items_.push_back({tr(S::GoToChannel), MenuAction::OpenChannel});
    } else if (t.is_channel()) {
        bool fav = fav_ids_.count(t.channel_id) > 0;
        menu_items_.push_back({tr(fav ? S::RemoveFavorite : S::AddFavorite),
                               MenuAction::FavoriteToggle});
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowChannelDescription});
    } else if (!t.video_id.empty() && dl_ids_.count(t.video_id) && view_label_ == "Downloads") {
        // Offline mode — but only in the Downloads view. Here the video plays from the
        // local file, so only options that work without the network make sense (no
        // comments, channel info, casting, quality, or captions). From other pages a
        // downloaded video streams, so it falls through to the full menu below.
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (mode_ == Mode::Playing) {   // playback speed is a local mpv setting
            char sb[24]; std::snprintf(sb, sizeof sb, ":  %gx", playback_speed_);
            menu_items_.push_back({tr(S::MenuSpeed) + std::string(sb), MenuAction::CycleSpeed});
        }
        menu_items_.push_back({tr(S::MenuRemoveDownload), MenuAction::RemoveDownload});
    } else {
        // Order everywhere: collect -> learn about it -> go/do -> heavy actions.
        // While playing, Playback (the per-video knobs) leads: it's the reason
        // the menu gets opened mid-video.
        if (mode_ == Mode::Playing)   // Brick OS 7.5c: the sound goes on after YouTube closes
            menu_items_.push_back({tr(S::ListenBackground), MenuAction::ListenBackground});
        if (mode_ == Mode::Playing)
            menu_items_.push_back({tr(S::SetPlaybackMenu), MenuAction::GoPlayback});
        if (mode_ == Mode::Playing)   // DESIGN 7.8: one switch, the same as Settings > Playback
            menu_items_.push_back({tr(S::SetAutoplay) + std::string(":  ") + tr(autoplay_ ? S::On : S::Off),
                                   MenuAction::ToggleAutoplay});
        // collect
        bool wl = wl_ids_.count(t.video_id) > 0;
        menu_items_.push_back({tr(wl ? S::RemoveWatchLater : S::AddWatchLater),
                               MenuAction::WatchLaterToggle});
        if (!t.channel_id.empty()) {
            bool fav = fav_ids_.count(t.channel_id) > 0;
            menu_items_.push_back({tr(fav ? S::RemoveFavorite : S::AddFavorite),
                                   MenuAction::FavoriteToggle});
        }
        // learn about it
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::MenuShowComments), MenuAction::ShowComments});
        if (!t.channel_id.empty())
            menu_items_.push_back({tr(S::ShowChannelDescription),
                                   MenuAction::ShowChannelDescription});
        if (!subview_playlist_.empty() && mode_ != Mode::Playing)   // playlist screens only
            menu_items_.push_back({tr(S::ShowPlaylistDescription),
                                   MenuAction::ShowPlaylistDescription});
        // go / do
        if (mode_ != Mode::Playing && !t.channel_id.empty())
            menu_items_.push_back({tr(S::GoToChannel), MenuAction::OpenChannel});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::MenuCastToDevice), MenuAction::CastToDevice});
        // heavy: download last (Remove Download in its place once downloaded)
        if (!t.video_id.empty())
            menu_items_.push_back({tr(dl_ids_.count(t.video_id) ? S::MenuRemoveDownload
                                                                : S::MenuDownload),
                                   dl_ids_.count(t.video_id) ? MenuAction::RemoveDownload
                                                             : MenuAction::DownloadVideo});
    }
    }  // end have_item
    // Clear the whole watch history — only from a tile in the History view.
    if (mode_ != Mode::Playing && view_label_ == "History")
        menu_items_.push_back({tr(S::ClearHistoryItem), MenuAction::ClearHistory});
    if (menu_items_.empty()) return;
    menu_sel_ = 0;   // adjust_setting saves/restores this across in-place rebuilds
    // Pause the player while the options menu is up (resumed on close). Don't re-pause
    // if we already paused it (rebuild during a quality change).
    if (mode_ == Mode::Playing && !player_.paused()) { player_.set_pause(true); menu_paused_ = true; }
    menu_open_ = true;
}
// Player options > Playback: the per-video value rows. Left/Right cycles them
// (overrides that never touch Settings); B returns to the options menu, and
// closing THAT still performs the deferred re-resolve for quality/audio changes.
void App::open_player_playback() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::PlayerPlayback;
    menu_items_.clear();
    menu_items_.push_back({tr(S::MenuQuality) + std::string(":  ")
                           + quality_label(effective_max_height()),
                           MenuAction::CycleMaxQuality});
    const S aname[] = {S::AspectFit, S::AspectZoom, S::AspectStretch};
    menu_items_.push_back({tr(S::SetAspect) + std::string(":  ") + tr(aname[effective_aspect()]),
                           MenuAction::CycleAspect});
    char sb[24]; std::snprintf(sb, sizeof sb, ":  %gx", playback_speed_);
    menu_items_.push_back({tr(S::MenuSpeed) + std::string(sb), MenuAction::CycleSpeed});
    std::string cc = std::string(tr(S::MenuCaptions)) + ":  ";
    if (cc_tracks_.empty()) cc += cc_running_ ? tr(S::Loading) : tr(S::CcNone);
    else if (cc_sel_ <= 0) cc += tr(S::Off);
    else cc += cc_tracks_[cc_sel_ - 1].name;   // track title, localized by YouTube (hl)
    menu_items_.push_back({cc, MenuAction::CycleCaptions});
    // Audio (dub) track — only when the video actually has alternatives.
    if (playing_audio_tracks_.size() > 1) {
        std::string cur = audio_override_lang_.empty() ? playing_audio_lang_
                                                       : audio_override_lang_;
        std::string tname;
        for (const auto& at : playing_audio_tracks_)
            if (at.lang == cur) { tname = at.name; break; }
        if (tname.empty()) tname = cur;
        menu_items_.push_back({tr(S::MenuAudioTrack) + std::string(":  ") + tname,
                               MenuAction::CycleAudioTrack});
    }
    menu_items_.push_back({tr(S::MenuStats) + std::string(":  ")
                           + tr(stats_for_nerds_ ? S::Enabled : S::Disabled),
                           MenuAction::ToggleStats});
    menu_sel_ = 0;
    menu_open_ = true;    // (player already paused by the options menu)
}
void App::reopen_player_menu() {
    if (menu_kind_ == MenuKind::PlayerPlayback) open_player_playback();
    else open_menu();
}

void App::menu_activate() {
    if (menu_sel_ < 0 || menu_sel_ >= (int)menu_items_.size()) return;
    const yt::SearchResult t = menu_target_;   // copy (open_channel may replace results)
    switch (menu_items_[menu_sel_].action) {
        case MenuAction::OpenSearchFilters:
            menu_open_ = false; open_search_filters(); return;   // swap context menu -> filters modal
        case MenuAction::FavoriteToggle: {
            std::string name = t.is_channel() ? t.title : t.author;
            if (fav_ids_.count(t.channel_id)) { it_.remove_favorite(t.channel_id);
                status_msg_ = i18n::tr(i18n::Str::RemovedFav) + std::string(": ") + name; }
            else { it_.add_favorite(t.channel_id, name);
                status_msg_ = i18n::tr(i18n::Str::AddedFav) + std::string(": ") + name; }
            status_until_ = SDL_GetTicks() + 4000; refresh_favorites();
            // Un-favoriting from the Favorite Channels view drops the tile now.
            if (view_label_ == "Favorite Channels" && mode_ != Mode::Playing) {
                menu_open_ = false; menu_paused_ = false;
                load_favorites();
                return;
            }
            // Un-favoriting a restricted channel makes it hideable again.
            if (hide_restricted_ && mode_ != Mode::Playing) {
                filter_hidden(results_);
                if (sel_ >= (int)results_.size())
                    sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            }
            break;
        }
        case MenuAction::FavoritePlaylistToggle: {
            if (fav_pl_ids_.count(t.playlist_id)) {
                it_.remove_favorite_playlist(t.playlist_id);
                status_msg_ = i18n::tr(i18n::Str::RemovedFav) + std::string(": ") + t.title;
            } else {
                it_.add_favorite_playlist({t.playlist_id, t.title, t.author, t.thumbnail_url});
                status_msg_ = i18n::tr(i18n::Str::AddedFav) + std::string(": ") + t.title;
            }
            status_until_ = SDL_GetTicks() + 4000;
            refresh_favorite_playlists();
            // Un-favoriting from the Favorite Playlists view drops the tile now.
            if (view_label_ == "Favorite Playlists" && mode_ != Mode::Playing) {
                menu_open_ = false; menu_paused_ = false;
                load_favorite_playlists();
                return;
            }
            break;
        }
        case MenuAction::WatchLaterToggle: {
            std::string wid = t.is_playlist() ? t.playlist_id : t.video_id;
            if (wl_ids_.count(wid)) { it_.remove_watch_later(wid);
                status_msg_ = i18n::tr(i18n::Str::RemovedWatchLater); }
            else { it_.add_watch_later(wid, t.title, t.is_playlist(),
                                       t.thumbnail_url, t.author, t.view_count_text);
                status_msg_ = i18n::tr(i18n::Str::AddedWatchLater); }
            status_until_ = SDL_GetTicks() + 4000; refresh_watch_later();
            break;
        }
        case MenuAction::DownloadVideo:
            menu_open_ = false; menu_paused_ = false;
            start_download(t);
            return;
        case MenuAction::RemoveDownload: {
            it_.remove_download(t.video_id); refresh_downloads();
            status_msg_ = i18n::tr(i18n::Str::DownloadRemoved);
            status_until_ = SDL_GetTicks() + 2500;
            if (view_label_ == "Downloads") load_downloads();
            break;
        }
        case MenuAction::ShowDescription: {
            menu_open_ = false;
            // If the menu auto-paused the player, keep it paused while reading —
            // the description overlay inherits the resume-on-close duty.
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            if (t.is_post()) open_post(t);
            else open_description(t);
            return;
        }
        case MenuAction::ShowComments: {
            menu_open_ = false;
            // Inherit the menu's pause (resumed when the comments overlay closes).
            comments_paused_ = menu_paused_;
            menu_paused_ = false;
            open_comments(t);
            return;
        }
        case MenuAction::PlayPostVideo:
            menu_open_ = false;
            menu_paused_ = false;
            request_playback();          // selected() is the post; its video_id plays
            return;
        case MenuAction::CastToDevice: {
            // Capture the target video (playing video, or the highlighted tile), then
            // open the device picker. If casting from the player, hand off the position.
            cast_target_id_ = t.video_id;
            cast_target_title_ = t.title;
            cast_target_is_short_ = t.is_short;
            cast_target_pos_ = (mode_ == Mode::Playing) ? (int)player_.position() : 0;
            menu_open_ = false; menu_paused_ = false;
            open_cast_picker();
            return;
        }
        case MenuAction::ShowChannelDescription: {
            menu_open_ = false;
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            open_channel_description(t.channel_id,
                                     t.is_channel() ? t.title : t.author);
            return;
        }
        case MenuAction::ShowPlaylistDescription: {
            menu_open_ = false;
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            yt::SearchResult pl;                    // the playlist being browsed
            pl.kind = yt::SearchResult::Kind::Playlist;
            pl.playlist_id = subview_playlist_;
            pl.title = query_;                      // view header = playlist title
            open_description(pl);
            return;
        }
        case MenuAction::OpenChannel: {
            menu_open_ = false;
            yt::SearchResult ch;              // synthesize a channel target from the video
            ch.kind = yt::SearchResult::Kind::Channel;
            ch.channel_id = t.channel_id;
            ch.title = t.is_channel() ? t.title : t.author;
            open_channel(ch);
            return;
        }
        // Top-level navigation (Start menu). Reachable from anywhere, so leave the
        // player and return to the grid before swapping in the new list.
        case MenuAction::GoHome:
        case MenuAction::GoFavorites:
        case MenuAction::GoFavoritePlaylists:
        case MenuAction::GoWatchLater:
        case MenuAction::GoDownloads:
        case MenuAction::GoHistory: {
            MenuAction act = menu_items_[menu_sel_].action;
            menu_open_ = false;
            menu_paused_ = false;             // navigating away; no player to resume
            if (mode_ == Mode::Playing) { save_resume_position(); player_.stop(); }
            in_channel_view_ = false;
            mode_ = Mode::Grid;
            if      (act == MenuAction::GoHome)       load_home();
            else if (act == MenuAction::GoFavorites)  load_favorites();
            else if (act == MenuAction::GoFavoritePlaylists) load_favorite_playlists();
            else if (act == MenuAction::GoWatchLater) load_watch_later();
            else if (act == MenuAction::GoDownloads)  load_downloads();
            else                                      load_history();
            return;
        }
        case MenuAction::GoSettings:
            open_settings();              // switch this overlay to the settings submenu
            return;
        case MenuAction::GoSettingsAudio:    open_settings_audio();    return;
        case MenuAction::GoSettingsVideo:    open_settings_video();    return;
        case MenuAction::GoSettingsCaptions: open_settings_captions(); return;
        case MenuAction::GoSettingsPlayback: open_settings_playback(); return;
        case MenuAction::GoSettingsBrowsing: open_settings_browsing(); return;
        case MenuAction::GoHomeFeedMenu:     open_settings_homefeed(); return;
        case MenuAction::GoPlayback:         open_player_playback();   return;
        case MenuAction::AddSearchToFeed: {
            yt::FeedSource s;
            s.query = query_;
            s.type = filt_type_; s.duration = filt_duration_;
            s.upload_date = filt_date_; s.sort = filt_sort_;
            bool added = it_.add_custom_feed_source(s);
            status_msg_ = i18n::tr(added ? i18n::Str::AddedToFeed : i18n::Str::AlreadyInFeed);
            status_until_ = SDL_GetTicks() + 3000;
            menu_open_ = false; menu_paused_ = false;
            return;
        }
        case MenuAction::GoCustomFeed:
            if (it_.custom_feed_sources().empty()) {
                status_msg_ = i18n::tr(i18n::Str::FeedEmpty);
                status_until_ = SDL_GetTicks() + 3000;
                menu_open_ = false; menu_paused_ = false;
            } else open_feed_manage();
            return;
        case MenuAction::FeedSourceRow:
            // A on a saved search: confirm before removing it.
            feed_remove_idx_ = menu_sel_;
            menu_kind_ = MenuKind::FeedRemoveConfirm;
            menu_items_.clear();
            menu_items_.push_back({i18n::tr(i18n::Str::Yes), MenuAction::FeedRemoveYes});
            menu_items_.push_back({i18n::tr(i18n::Str::No),  MenuAction::FeedRemoveNo});
            menu_sel_ = 1;   // default No
            return;
        case MenuAction::FeedRemoveYes:
            if (feed_remove_idx_ >= 0) it_.remove_custom_feed_source((size_t)feed_remove_idx_);
            feed_remove_idx_ = -1;
            if (it_.custom_feed_sources().empty()) {
                // Feed emptied: fall back off Custom, and land back on Browsing.
                if (home_sources_ & 4) {
                    home_sources_ &= ~4;
                    if (!home_sources_) home_sources_ = 1;
                    it_.set_setting_int("home_sources", home_sources_);
                }
                open_settings_browsing();
            } else open_feed_manage();
            // Viewing Home on the Custom feed? Refresh with the source gone.
            if (query_.empty() && view_label_.empty() && !in_channel_view_ && mode_ != Mode::Playing)
                refresh_current_view();
            return;
        case MenuAction::FeedRemoveNo:
            feed_remove_idx_ = -1;
            open_feed_manage();
            return;
        case MenuAction::GoLinkedDevices:
            menu_open_ = false; menu_paused_ = false;
            open_manage_devices();
            return;
        case MenuAction::ClearHistory:
            menu_open_ = false; menu_paused_ = false;
            it_.clear_history();
            load_history();               // reload the (now empty) view
            status_msg_ = i18n::tr(i18n::Str::HistoryCleared);
            status_until_ = SDL_GetTicks() + 3000;
            return;
        case MenuAction::CycleMaxQuality:
        case MenuAction::ToggleStats:
        case MenuAction::ToggleHideRestricted:
        case MenuAction::ToggleAskResume:
        case MenuAction::CycleView:
        case MenuAction::CycleVolume:
        case MenuAction::CycleHwdec:
        case MenuAction::CycleAspect:
        case MenuAction::CycleAudioLang:
        case MenuAction::CycleCaptionLang:
        case MenuAction::CycleAudioTrack:
        case MenuAction::CycleCaptionSize:
        case MenuAction::CycleCaptionStyle:
        case MenuAction::CycleSpeed:
        case MenuAction::ToggleSponsorBlock:
        case MenuAction::CycleCaptions:
        case MenuAction::ToggleAutoplay:
            if (menu_kind_ == MenuKind::Context) adjust_setting(MenuAction::ToggleAutoplay, 1);   // a tile: A flips it
            return;
        case MenuAction::ToggleFeedFavorites:
        case MenuAction::ToggleFeedHistory:
        case MenuAction::ToggleFeedCustom:
            return;   // value rows change with Left/Right only; A does nothing
        case MenuAction::Quit:
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
    }
    // Context action that keeps us on the player (favorite / watch-later toggle):
    // close the menu and resume playback if we auto-paused it.
    menu_open_ = false;
    if (menu_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
    menu_paused_ = false;
}
// Snapshot the current view onto the back stack (before entering a subview).
void App::push_view() {
    ViewState v;
    v.results = results_; v.query = query_; v.view_label = view_label_;
    v.cont_token = cont_token_; v.cont_endpoint = cont_endpoint_;
    v.cont_channel_id = cont_channel_id_;
    v.in_channel_view = in_channel_view_;
    v.subview_playlist = subview_playlist_;
    v.chan_tab = chan_tab_;
    v.channel_info = channel_info_;
    v.sel = sel_; v.scroll = scroll_;
    view_stack_.push_back(std::move(v));
}
bool App::pop_view() {
    if (view_stack_.empty()) return false;
    ViewState v = std::move(view_stack_.back());
    view_stack_.pop_back();
    results_ = std::move(v.results); query_ = v.query; view_label_ = v.view_label;
    cont_token_ = v.cont_token; cont_endpoint_ = v.cont_endpoint;
    cont_channel_id_ = v.cont_channel_id;
    in_channel_view_ = v.in_channel_view;
    subview_playlist_ = v.subview_playlist;
    chan_tab_ = v.chan_tab;
    channel_info_ = v.channel_info;
    sel_ = v.sel; scroll_ = v.scroll;
    tab_focus_ = false;
    build_tile_lines();          // recompute metadata lines for the restored list
    // Thumbnails re-requested lazily per-viewport by the renderer.
    return true;
}

void App::open_channel(const yt::SearchResult& ch) {
    if (ch.channel_id.empty()) return;
    // Copy id/name FIRST: `ch` references results_[sel_], which set_results() below
    // move-replaces — reading ch after that is a use-after-free.
    std::string id = ch.channel_id;
    std::string name = ch.title;
    push_view();                                        // Back returns here
    in_channel_view_ = true;
    view_label_.clear();   // subview owns the header; the stack restores the label
    subview_playlist_.clear();                          // this subview is a channel
    chan_tab_ = 0; tab_focus_ = false;                  // land on the "All" tab
    query_ = name;                                      // header shows the channel name now
    channel_info_ = {}; channel_info_.channel_id = id;  // subs/videos fill in async
    auto f = it_.channel_all_feed(id);                   // home/featured (mixed, paginated)
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    for (auto& r : f.items)                              // channel rows omit the uploader; fill
        if (r.author.empty()) r.author = name;           // it in BEFORE composing the tiles
    set_results(std::move(f.items));
    if (results_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::NoRecentUploads);
        status_until_ = SDL_GetTicks() + 5000;
    }
    // Fetch full channel metadata (subs, video count) in the background so it
    // never holds up the video list / UI.
    if (!chinfo_running_) {
        chinfo_running_ = true; chinfo_done_ = false;
        if (chinfo_thread_.joinable()) chinfo_thread_.join();
        chinfo_thread_ = std::thread([this, id]() {
            yt::ChannelInfo info;
            try { info = it_.channel_info(id); } catch (...) {}
            { std::lock_guard<std::mutex> lk(chinfo_m_); chinfo_pending_ = std::move(info); }
            chinfo_running_ = false; chinfo_done_ = true;
        });
    }
}
// Switch to a channel tab: the highlight moves NOW, the content fetches in the
// background (poll_refresh applies it) so the UI never blocks on the network.
void App::load_channel_tab(int tab) {
    std::string id = channel_info_.channel_id;
    if (id.empty() || tab < 0 || tab > 4) return;
    chan_tab_ = tab;
    tab_focus_ = false;                                 // first video selected on load
    results_.clear(); cont_token_.clear();
    sel_ = 0; scroll_ = 0;
    refresh_current_view();
}

// Switch a Home tab. All/Videos/Shorts are instant local filters of the fetched
// home feed; Playlists and Posts fetch (async) on first visit and are cached after.
void App::load_home_tab(int tab) {
    if (tab < 0 || tab > 4) return;
    home_tab_ = tab;
    tab_focus_ = false;                                 // first video selected on load
    cont_token_.clear();
    if (tab == 3 || tab == 4) {
        bool loaded = tab == 3 ? home_posts_loaded_ : home_playlists_loaded_;
        if (loaded) {
            auto copy = tab == 3 ? home_posts_ : home_playlists_;
            set_results(std::move(copy));
        } else {
            results_.clear(); sel_ = 0; scroll_ = 0;
            refresh_current_view();          // async home_playlists / home_posts fetch
        }
    } else {
        apply_home_tab();
    }
}

// Rebuild results_ from the Home master list for the active All/Videos/Shorts tab.
void App::apply_home_tab() {
    std::vector<yt::SearchResult> items;
    for (const auto& r : home_items_) {
        if (home_tab_ == 1 && (r.is_short || r.is_post() || r.is_playlist())) continue;  // Videos only
        if (home_tab_ == 2 && !r.is_short) continue;                  // Shorts only
        items.push_back(r);
    }
    set_results(std::move(items));
}

// Open a playlist's contents as a subview — same push/pop mechanics as a channel,
// but no async channel-info fetch (a playlist header is just its title).
void App::open_playlist(const yt::SearchResult& pl) {
    if (pl.playlist_id.empty()) return;
    // Copy FIRST (pl references results_[sel_], replaced by set_results below).
    std::string id = pl.playlist_id;
    std::string name = pl.title;
    push_view();                                        // Back returns here
    in_channel_view_ = true;
    view_label_.clear();   // subview owns the header; the stack restores the label
    subview_playlist_ = id;
    tab_focus_ = false;                                 // playlists have no tab strip
    query_ = name;                                      // header shows the playlist title
    channel_info_ = {}; channel_info_.channel_id = id;  // view identity (no async fetch)
    auto f = it_.playlist_feed(id);                      // paginated via continuation
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    if (results_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::PlaylistUnavailable);
        status_until_ = SDL_GetTicks() + 5000;
    }
}
void App::maybe_load_more() {
    if (cont_token_.empty() || more_running_) return;
    int n = (int)results_.size();
    if (n == 0 || sel_ < n - cols_ * 2) return;   // only when near the last rows
    more_running_ = true; more_done_ = false;
    if (more_thread_.joinable()) more_thread_.join();
    yt::Innertube::Feed cursor;
    cursor.continuation = cont_token_; cursor.endpoint = cont_endpoint_;
    cursor.channel_id = cont_channel_id_;
    more_thread_ = std::thread([this, cursor]() {
        yt::Innertube::Feed next;
        try { next = it_.continue_feed(cursor); } catch (...) {}
        { std::lock_guard<std::mutex> lk(more_m_); more_pending_ = std::move(next); }
        more_running_ = false; more_done_ = true;
    });
}
void App::poll_more() {
    if (!more_done_.exchange(false)) return;
    if (more_thread_.joinable()) more_thread_.join();
    yt::Innertube::Feed next;
    { std::lock_guard<std::mutex> lk(more_m_); next = std::move(more_pending_); }
    filter_hidden(next.items);
    for (auto& v : next.items) {
        thumbs_.request(v.thumbnail_url);
        if (v.is_channel()) chan_meta_.request(v.channel_id);
        else if (v.is_short && v.author.empty() && !v.channel_id.empty())
            chan_meta_.request(v.channel_id);   // resolve the short's uploader name
    }
    size_t added = next.items.size();
    bool search_ctx = !query_.empty() && !in_channel_view_ && !search_base_.empty();
    if (search_ctx) {
        // Search results: new page joins the canonical relevance list, then we rebuild
        // results_ in the active sort order (keeping the current selection by id).
        search_base_.insert(search_base_.end(), std::make_move_iterator(next.items.begin()),
                            std::make_move_iterator(next.items.end()));
        apply_search_view(/*keep_selection=*/true);
    } else {
        results_.insert(results_.end(), std::make_move_iterator(next.items.begin()),
                        std::make_move_iterator(next.items.end()));
        if (channel_tabs_active())         // channel-tab rows omit the uploader
            for (auto& r : results_)
                if (r.author.empty()) r.author = query_;
        build_tile_lines();                // refresh metadata-line cache for the grown list
    }
    cont_token_ = next.continuation;   // new token, or "" at the end of the feed
    queue_restricted_checks();
    if (getenv("YTC_DEBUG"))
        std::fprintf(stderr, "[more] +%zu -> total %zu (%s)\n", added, results_.size(),
                     cont_token_.empty() ? "END" : "more available");
}

// Home feed pages differently from search/channel: no single continuation token,
// but a per-channel cursor. When the selection nears the bottom, continue every
// channel one page on a worker, then merge the batch into home_items_ (append-only,
// so the selection doesn't jump) and re-apply the active All/Videos/Shorts filter.
// The active Home sub-tab's cursor: Playlists (3) and Posts (4) are separate
// aggregated feeds with their own per-channel cursors; All/Videos/Shorts share one.
yt::Innertube::HomeCursor& App::home_cursor_for(int tab) {
    return tab == 3 ? home_posts_cursor_ : tab == 4 ? home_pl_cursor_ : home_cursor_;
}
void App::maybe_load_more_home() {
    if (home_more_running_) return;
    if (!(query_.empty() && view_label_.empty() && !in_channel_view_)) return;  // home master only
    int tab = home_tab_;
    if (!home_cursor_for(tab).has_more()) return;
    int n = (int)results_.size();
    if (n == 0 || sel_ < n - cols_ * 2) return; // only when near the last rows
    home_more_running_ = true; home_more_done_ = false; home_more_tab_ = tab;
    if (home_more_thread_.joinable()) home_more_thread_.join();
    yt::Innertube::HomeCursor cursor = home_cursor_for(tab);   // worker owns a copy
    home_more_thread_ = std::thread([this, cursor, tab]() mutable {
        std::vector<yt::SearchResult> batch;
        try {
            batch = tab == 3 ? it_.home_posts_more(cursor)
                  : tab == 4 ? it_.home_playlists_more(cursor)
                             : it_.home_feed_more(cursor);
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(home_more_m_);
          home_more_pending_ = std::move(batch); home_more_cursor_pending_ = std::move(cursor); }
        home_more_running_ = false; home_more_done_ = true;
    });
}
void App::poll_more_home() {
    if (!home_more_done_.exchange(false)) return;
    if (home_more_thread_.joinable()) home_more_thread_.join();
    std::vector<yt::SearchResult> batch;
    yt::Innertube::HomeCursor cur;
    { std::lock_guard<std::mutex> lk(home_more_m_);
      batch = std::move(home_more_pending_); cur = std::move(home_more_cursor_pending_); }
    int tab = home_more_tab_;
    home_cursor_for(tab) = std::move(cur);   // advance the right cursor
    // Still on the home master view AND the same sub-tab the page was fetched for?
    if (!(query_.empty() && view_label_.empty() && !in_channel_view_)) return;
    if (home_tab_ != tab) return;
    if (batch.empty()) return;
    int base = (int)results_.size();
    if (tab == 3 || tab == 4) {
        filter_hidden(batch);                 // restricted only here (homogeneous feed)
        for (auto& v : batch) thumbs_.request(v.thumbnail_url);
        // Playlists / Posts: append into the tab's cache (so a round-trip keeps them)
        // and the visible list. No is_short filter — the feed is homogeneous.
        auto& cache = (tab == 3) ? home_posts_ : home_playlists_;
        for (auto& v : batch) { cache.push_back(v); results_.push_back(std::move(v)); }
    } else {
        // Append the raw page to the master (keeps Shorts so the Shorts tab still has
        // them), then add to the visible list respecting the tab + hide-shorts rule.
        size_t added = batch.size();
        home_items_.insert(home_items_.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
        std::vector<yt::SearchResult> add;
        for (size_t i = home_items_.size() - added; i < home_items_.size(); ++i) {
            const auto& r = home_items_[i];
            if (home_tab_ == 1 && (r.is_short || r.is_post())) continue;  // Videos only
            if (home_tab_ == 2 && !r.is_short) continue;                  // Shorts only
            add.push_back(r);
        }
        filter_hidden(add);                   // restricted + Shorts (only on the All tab)
        for (auto& v : add) { thumbs_.request(v.thumbnail_url); results_.push_back(std::move(v)); }
    }
    if ((int)results_.size() > base) { build_tile_lines(); queue_restricted_checks(); }
    if (getenv("YTC_DEBUG")) {
        const char* tag = tab == 3 ? ":posts" : tab == 4 ? ":pl" : "";
        std::fprintf(stderr, "[home-more%s] +%d shown -> %zu (%s)\n", tag,
                     (int)results_.size() - base, results_.size(),
                     home_cursor_for(tab).has_more() ? "more available" : "END");
    }
}

// ===================== Casting (Option B) =====================
void App::open_cast_picker() {
    cast_picker_mode_ = PickerMode::Cast;
    cast_picker_open_ = true; cast_sel_ = 0; cast_devices_.clear();
    if (cast_disc_running_) return;
    cast_disc_running_ = true; cast_disc_done_ = false;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    cast_disc_thread_ = std::thread([this]() {
        std::vector<yt::Cast::Device> devs;
        try { devs = cast_.discover(3000); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_disc_pending_ = std::move(devs); }
        cast_disc_running_ = false; cast_disc_done_ = true;
    });
}
void App::open_link_picker() {           // Linked Devices -> "Add a device": discover linkable TVs
    cast_picker_mode_ = PickerMode::Link;
    cast_picker_open_ = true; cast_sel_ = 0; cast_devices_.clear();
    if (cast_disc_running_) return;
    cast_disc_running_ = true; cast_disc_done_ = false;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    cast_disc_thread_ = std::thread([this]() {
        std::vector<yt::Cast::Device> devs;
        try { devs = cast_.discover(3000); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_disc_pending_ = std::move(devs); }
        cast_disc_running_ = false; cast_disc_done_ = true;
    });
}
void App::rebuild_picker_rows() {
    cast_devices_.clear();
    if (cast_picker_mode_ == PickerMode::Link) {
        // Only devices that can be code-linked: unpaired Cast/Android-TV devices.
        for (auto& d : cast_all_)
            if (d.kind == yt::Cast::Kind::CastDevice && d.screen_id.empty() && !d.ip.empty())
                cast_devices_.push_back(d);
        return;
    }
    for (auto& d : cast_all_) {
        if (d.kind == yt::Cast::Kind::DialYouTube) {
            cast_devices_.push_back(d);                 // smart TV: native, no code
        } else if (d.kind == yt::Cast::Kind::CastDevice) {
            // The native TV app plays a Short in its Shorts UI, which ignores the lounge
            // remote — so hide the linked (paired) row for Shorts. The web-receiver row
            // below plays it as a normal, controllable video.
            if (!d.screen_id.empty() && !cast_target_is_short_)
                cast_devices_.push_back(d);              // paired native row
            if (!d.ip.empty()) {                         // code-free "· Chromecast" row (web receiver)
                yt::Cast::Device cc = d; cc.screen_id.clear();
                cast_devices_.push_back(cc);
            }
        }
    }
}
void App::poll_cast_discovery() {
    if (!cast_disc_done_.exchange(false)) return;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_all_ = std::move(cast_disc_pending_); }
    rebuild_picker_rows();
    int maxsel = (int)cast_devices_.size() + (cast_picker_mode_ == PickerMode::Cast ? 1 : 0) - 1;
    if (cast_sel_ > maxsel) cast_sel_ = std::max(0, maxsel);
}
void App::cast_activate() {
    int n = (int)cast_devices_.size();
    if (cast_picker_mode_ == PickerMode::Link) {   // pick a device to link -> code entry for it
        if (cast_sel_ < 0 || cast_sel_ >= n) return;
        cast_link_name_ = cast_devices_[cast_sel_].name;
        open_search();
        query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;
        kb_mode_ = KbMode::LinkDevice;
        kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);
        kb_placeholder_.clear();
        return;
    }
    (void)n; return cast_activate_cast();
}
void App::cast_activate_cast() {
    int n = (int)cast_devices_.size();
    if (cast_sel_ == n) {   // "Add a device" -> on-screen keyboard in code mode
        open_search();
        query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;   // number row
        kb_mode_ = KbMode::CastCode;
        kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);   // "Enter TV Code"
        kb_placeholder_.clear();                          // no hint in the box
        return;
    }
    if (cast_sel_ >= 0 && cast_sel_ < n) start_cast(cast_devices_[cast_sel_]);
}
double App::cast_est_pos() const {
    if (cast_ev_valid_) {   // real TV position + elapsed since it was reported (if playing)
        double e = cast_ev_pos_.load();
        if (cast_ev_state_.load() == 1) e += (SDL_GetTicks() - cast_ev_ts_.load()) / 1000.0;
        double dur = cast_ev_dur_.load();
        if (dur > 0 && e > dur) e = dur;
        return e < 0 ? 0 : e;
    }
    double e = cast_base_pos_;   // fallback before the first event arrives
    if (!cast_paused_) e += (SDL_GetTicks() - cast_started_ms_) / 1000.0;
    return e < 0 ? 0 : e;
}
void App::start_cast(const yt::Cast::Device& d) {
    if (cast_play_running_) return;
    cast_play_name_ = d.name.empty() ? "TV" : d.name;
    cast_play_running_ = true; cast_play_done_ = false;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    yt::Cast::Device dev = d;
    std::string vid = cast_target_id_; int pos = cast_target_pos_;
    cast_play_thread_ = std::thread([this, dev, vid, pos]() {
        yt::Cast::Session s;
        try { s = cast_.play(dev, vid, pos); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_play_m_); cast_play_pending_ = s; }
        cast_play_running_ = false; cast_play_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 9000;
}
void App::submit_cast_code(const std::string& code) {
    if (cast_play_running_ || code.empty()) return;
    // Name the pairing after the discovered device: if exactly one unpaired Cast
    // device is on the network, the code is almost certainly for it (e.g. "SHIELD").
    std::string name; int unpaired = 0;
    for (auto& d : cast_all_) if (d.needs_code()) { name = d.name; unpaired++; }
    if (unpaired != 1) name.clear();     // ambiguous -> fall back to the generic name
    cast_play_name_ = name.empty() ? "TV" : name;
    cast_play_running_ = true; cast_play_done_ = false;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    std::string vid = cast_target_id_; int pos = cast_target_pos_; std::string nm = cast_play_name_;
    cast_play_thread_ = std::thread([this, code, vid, pos, nm]() {
        yt::Cast::Session s;
        try {
            std::string sid = cast_.pair_with_code(code, nm);
            if (!sid.empty()) {
                yt::Cast::Device d; d.screen_id = sid; d.name = nm; d.kind = yt::Cast::Kind::CastDevice;
                s = cast_.play(d, vid, pos);
            }
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_play_m_); cast_play_pending_ = s; }
        cast_play_running_ = false; cast_play_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 9000;
}
void App::poll_cast_play() {
    if (!cast_play_done_.exchange(false)) return;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    yt::Cast::Session s;
    { std::lock_guard<std::mutex> lk(cast_play_m_); s = cast_play_pending_; }
    if (s.ok) {
        cast_session_ = s; cast_name_ = cast_play_name_; casting_ = true;
        cast_paused_ = false; cast_vol_ = 100;
        cast_base_pos_ = cast_target_pos_; cast_started_ms_ = SDL_GetTicks();
        cast_picker_open_ = false;
        // Start the event-reader: long-poll the lounge backchannel for the TV's real
        // position (used for accurate seek). Runs on a copy of the session; RID=rpc so
        // it never touches the command rid counter.
        cast_ev_valid_ = false; cast_nowplaying_at_ = 0;
        if (cast_events_thread_.joinable()) cast_events_thread_.join();
        cast_events_run_ = true;
        yt::Cast::Session es = s;
        cast_events_thread_ = std::thread([this, es]() {
            int aid = -1;
            while (cast_events_run_) {
                yt::Cast::NowPlaying np;
                try { np = cast_.read_events(es, aid, 4); } catch (...) {}
                aid = np.aid;
                if (np.valid) {
                    cast_ev_pos_ = np.current_time; cast_ev_dur_ = np.duration;
                    cast_ev_state_ = np.state; cast_ev_ts_ = SDL_GetTicks();
                    cast_ev_valid_ = true;
                }
            }
        });
        // Option B: local playback stops; the handheld becomes the remote.
        if (mode_ == Mode::Playing) { save_resume_position(); player_.stop(); mode_ = Mode::Grid; }
        status_msg_.clear();
    } else {
        status_msg_ = i18n::tr(i18n::Str::CastFailed); status_until_ = SDL_GetTicks() + 4000;
    }
}
void App::cast_command(const std::string& type, double arg) {
    if (!casting_ || cast_cmd_running_) return;   // drop rapid extra presses (never block the UI)
    cast_cmd_running_ = true;
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();   // prior one already finished
    std::string ty = type; double ar = arg;
    cast_cmd_thread_ = std::thread([this, ty, ar]() {
        try { cast_.command(cast_session_, ty, ar); } catch (...) {}  // only the worker touches rid
        cast_cmd_running_ = false;
    });
}
void App::stop_casting() {
    casting_ = false;
    cast_events_run_ = false;
    if (cast_events_thread_.joinable()) cast_events_thread_.join();
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();   // don't free the session under it
    cast_session_ = {};
    cast_ev_valid_ = false;
}

// ---- Settings -> Linked Devices ----
void App::open_manage_devices() {
    cast_manage_open_ = true; cast_manage_sel_ = 0;
    cast_paired_ = cast_.paired();
}
void App::manage_activate() {
    int n = (int)cast_paired_.size();
    if (cast_manage_sel_ == n) {   // "+ Add a device" -> discovery picker of linkable TVs
        open_link_picker();
        return;
    }
    if (cast_manage_sel_ >= 0 && cast_manage_sel_ < n) {   // confirm before removing
        cast_confirm_remove_ = true; cast_confirm_sel_ = 0;   // default to "No"
    }
}
void App::confirm_remove_device() {
    int n = (int)cast_paired_.size();
    if (cast_manage_sel_ < 0 || cast_manage_sel_ >= n) return;
    std::string name = cast_paired_[cast_manage_sel_].name;
    cast_.forget(cast_paired_[cast_manage_sel_].screen_id);
    cast_paired_ = cast_.paired();
    if (cast_manage_sel_ >= (int)cast_paired_.size()) cast_manage_sel_ = std::max(0, (int)cast_paired_.size());
    status_msg_ = std::string(i18n::tr(i18n::Str::CastRemoved)) + ": " + name;
    status_until_ = SDL_GetTicks() + 2500;
}
void App::link_device(const std::string& code) {
    if (cast_link_running_ || code.empty()) return;
    cast_picker_open_ = false;              // done with the link picker -> back to the manage list
    cast_link_running_ = true; cast_link_done_ = false;
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    std::string nm = cast_link_name_;       // the device we chose to link
    cast_link_thread_ = std::thread([this, code, nm]() {
        std::string sid;
        try { sid = cast_.pair_with_code(code, nm); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_link_m_); cast_link_result_ = sid; }
        cast_link_running_ = false; cast_link_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 8000;
}
void App::poll_cast_link() {
    if (!cast_link_done_.exchange(false)) return;
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    std::string sid;
    { std::lock_guard<std::mutex> lk(cast_link_m_); sid = cast_link_result_; }
    if (sid.empty()) { status_msg_ = i18n::tr(i18n::Str::CastPairFailed); status_until_ = SDL_GetTicks() + 4000; }
    else status_msg_.clear();
    cast_paired_ = cast_.paired();   // refresh the manage list either way
}

void App::poll_channel_info() {
    if (!chinfo_done_.exchange(false)) return;
    if (chinfo_thread_.joinable()) chinfo_thread_.join();
    yt::ChannelInfo info;
    { std::lock_guard<std::mutex> lk(chinfo_m_); info = std::move(chinfo_pending_); }
    // Apply only if still viewing that channel.
    if (in_channel_view_ && info.channel_id == channel_info_.channel_id)
        channel_info_ = std::move(info);
}

const yt::SearchResult* App::selected() const {
    if (sel_ < 0 || sel_ >= (int)results_.size()) return nullptr;
    return &results_[sel_];
}
void App::input(Action a) {
    if (screen_dark_) return;   // screen off (power tap): the buttons do nothing until it wakes
    // "Leave YouTube?" (B on Home): the first row leaves, the second stays
    if (quit_prompt_open_) {
        switch (a) {
            case Action::Up: case Action::Down:
            case Action::Left: case Action::Right: quit_prompt_sel_ ^= 1; break;
            case Action::Select:
                quit_prompt_open_ = false;
                if (quit_prompt_sel_ == 0) quit_requested_ = true;
                break;
            case Action::Back: quit_prompt_open_ = false; break;
            default: break;
        }
        return;
    }
    // Resume prompt consumes input while open (a pre-playback modal).
    if (resume_prompt_open_) {
        switch (a) {
            case Action::Up: case Action::Down:
            case Action::Left: case Action::Right:
                resume_prompt_sel_ ^= 1; break;      // toggle Resume / Start over
            case Action::Select: {
                resume_prompt_open_ = false;
                yt::SearchResult it = resume_prompt_item_;
                double at = resume_prompt_sel_ == 0 ? resume_prompt_pos_ : 0;
                if (resume_prompt_sel_ == 1) it_.clear_resume_pos(it.video_id);
                start_resolve(it.video_id, it.title, at);
                break;
            }
            case Action::Back: resume_prompt_open_ = false; break;   // cancel: don't play
            default: break;
        }
        return;
    }
    // Remote mode: the handheld controls the TV (playback continues there).
    if (casting_) {
        switch (a) {
            case Action::Select: {   // A: play/pause
                double at = cast_est_pos();
                cast_paused_ = !cast_paused_;
                cast_command(cast_paused_ ? "pause" : "play");
                // Optimistic local update; the next event corrects it.
                cast_ev_pos_ = at; cast_ev_ts_ = SDL_GetTicks();
                cast_ev_state_ = cast_paused_ ? 2 : 1; cast_ev_valid_ = true;
                cast_base_pos_ = at; cast_started_ms_ = SDL_GetTicks();
                break;
            }
            case Action::Left: case Action::Right: {   // seek +/- 10s (real position)
                double t = cast_est_pos() + (a == Action::Right ? 10 : -10); if (t < 0) t = 0;
                cast_command("seekTo", t);
                cast_ev_pos_ = t; cast_ev_ts_ = SDL_GetTicks(); cast_ev_valid_ = true;
                cast_base_pos_ = t; cast_started_ms_ = SDL_GetTicks();
                break;
            }
            case Action::Up: case Action::Down:   // volume
                cast_vol_ += (a == Action::Up ? 10 : -10);
                if (cast_vol_ > 100) cast_vol_ = 100; if (cast_vol_ < 0) cast_vol_ = 0;
                cast_command("setVolume", cast_vol_); break;
            case Action::Back:
                cast_command("stopVideo");   // stop playback on the TV (esp. the web receiver)
                stop_casting();              // joins the command worker, then leaves the remote
                break;
            default: break;
        }
        return;
    }
    // Device picker overlay (but not while the code keyboard is up over it).
    if (cast_picker_open_ && mode_ != Mode::Search) {
        // Cast mode has a trailing "Add a device" row; Link mode is devices only.
        int rows = (int)cast_devices_.size() + (cast_picker_mode_ == PickerMode::Cast ? 1 : 0);
        switch (a) {
            case Action::Up:     wrap_step(cast_sel_, rows, -1); break;
            case Action::Down:   wrap_step(cast_sel_, rows, 1); break;
            case Action::Select: cast_activate(); break;
            case Action::Back:   cast_picker_open_ = false; break;   // Link mode: back to manage
            default: break;
        }
        return;
    }
    // "Remove Device?" confirmation prompt (topmost over the manage list).
    if (cast_confirm_remove_ && mode_ != Mode::Search) {
        switch (a) {
            case Action::Left: case Action::Right: cast_confirm_sel_ ^= 1; break;
            case Action::Select:
                if (cast_confirm_sel_ == 1) confirm_remove_device();
                cast_confirm_remove_ = false;
                break;
            case Action::Back:   cast_confirm_remove_ = false; break;  // cancel = No
            default: break;
        }
        return;
    }
    // Linked-devices management overlay (Settings -> Linked Devices).
    if (cast_manage_open_ && mode_ != Mode::Search) {
        int rows = (int)cast_paired_.size() + 1;    // + "Link a device"
        switch (a) {
            case Action::Up:     wrap_step(cast_manage_sel_, rows, -1); break;
            case Action::Down:   wrap_step(cast_manage_sel_, rows, 1); break;
            case Action::Select: manage_activate(); break;
            case Action::Back:   cast_manage_open_ = false; open_settings(); break;  // back to Settings
            default: break;
        }
        return;
    }
    // Comments overlay consumes input while open (topmost). Up/Down move the selected
    // comment; A expands/collapses its replies; Left/Right jump; B closes.
    if (comments_open_) {
        int n = (int)comment_units_.size();
        switch (a) {
            case Action::Up:    wrap_step(comments_sel_, n, -1); break;
            case Action::Down:  wrap_step(comments_sel_, n, 1); break;
            case Action::Left:  comments_sel_ = std::max(0, comments_sel_ - 6); break;
            case Action::Right: comments_sel_ = std::min(n - 1, comments_sel_ + 6); break;
            case Action::Select: toggle_comment_replies(); break;
            case Action::Sort:   toggle_comment_sort(); break;
            case Action::Back:
            case Action::Menu:  close_comments(); break;
            default: break;
        }
        return;
    }
    // Description overlay consumes input while open (topmost).
    if (desc_open_) {
        float step = font_small_ ? (font_small_->line_height() + 4) * 3 : 60;
        auto close_desc = [&]{
            desc_open_ = false; desc_loading_ = false;
            if (desc_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
            desc_paused_ = false;
        };
        // Brick OS post page: ←→ the pictures (a fresh press wraps), ↑↓ the text, Y a picture full screen,
        // A the focused button (เล่นคลิป / ความคิดเห็น; ←→ between them when there are no pictures), B back.
        if (desc_is_post_) {
            int nimg = (int)post_images_.size();
            bool clip = post_has_video_, com = !desc_post_id_.empty();
            int nbtn = (clip ? 1 : 0) + (com ? 1 : 0);
            if (post_focus_ >= nbtn) post_focus_ = 0;
            auto spring_go = [&] { if (!post_img_last_) post_img_vel_ = 0; };
            switch (a) {
                case Action::Left:
                    if (nimg > 1) { spring_go(); wrap_step(post_img_, nimg, -1); }
                    else if (!post_full_ && nbtn == 2) post_focus_ = 0;
                    break;
                case Action::Right:
                    if (nimg > 1) { spring_go(); wrap_step(post_img_, nimg, 1); }
                    else if (!post_full_ && nbtn == 2) post_focus_ = 1;
                    break;
                case Action::Up:   if (!post_full_) desc_scroll_ -= step; break;
                case Action::Down: if (!post_full_) desc_scroll_ += step; break;
                case Action::Search: if (nimg > 0) post_full_ = !post_full_; break;
                case Action::Select:
                    if (post_full_ || nbtn == 0) break;
                    if (clip && post_focus_ == 0) {          // play the attached clip
                        close_desc();
                        request_playback();
                    } else if (com) {                         // this post's comments
                        yt::SearchResult tmp;
                        tmp.kind = yt::SearchResult::Kind::Post; tmp.post_id = desc_post_id_;
                        tmp.channel_id = desc_post_channel_;
                        comments_paused_ = false;
                        open_comments(tmp);
                    }
                    break;
                case Action::Back: if (post_full_) post_full_ = false; else close_desc(); break;
                case Action::Menu: close_desc(); break;
                default: break;
            }
            return;
        }
        // Post overlay: selectable top buttons (video, comments) then the scrolling text.
        // Focus 0..n_top-1 = buttons in order [video, comments]; n_top = text scroll.
        bool has_comments_btn = desc_is_post_ && !desc_post_id_.empty();
        int n_top = (post_has_video_ ? 1 : 0) + (has_comments_btn ? 1 : 0);
        if (n_top > 0) {
            int vid_idx = post_has_video_ ? 0 : -1;
            int com_idx = has_comments_btn ? (post_has_video_ ? 1 : 0) : -1;
            int text_focus = n_top;
            switch (a) {
                case Action::Up:
                    if (post_focus_ == text_focus) {
                        if (desc_scroll_ > 0) desc_scroll_ -= step; else post_focus_ = n_top - 1;
                    } else if (post_focus_ > 0) post_focus_--;
                    break;
                case Action::Down:
                    if (post_focus_ < text_focus) post_focus_++; else desc_scroll_ += step;
                    break;
                case Action::Select:
                    if (post_focus_ == vid_idx) {          // play the attached video
                        close_desc();
                        request_playback();
                    } else if (post_focus_ == com_idx) {   // open this post's comments
                        yt::SearchResult tmp;
                        tmp.kind = yt::SearchResult::Kind::Post; tmp.post_id = desc_post_id_;
                        tmp.channel_id = desc_post_channel_;
                        comments_paused_ = false;          // the post overlay keeps the pause
                        open_comments(tmp);
                    }
                    break;
                case Action::Back:
                    // From the text: first B returns to the buttons; from a button: close.
                    if (post_focus_ == text_focus) { post_focus_ = n_top - 1; desc_scroll_ = 0; }
                    else close_desc();
                    break;
                case Action::Menu:  close_desc(); break;
                default: break;
            }
            return;
        }
        switch (a) {
            case Action::Up:     desc_scroll_ -= step; break;
            case Action::Down:   desc_scroll_ += step; break;
            case Action::Back:
            case Action::Menu:
            case Action::Select: close_desc(); break;
            default: break;
        }
        return;
    }
    // Options menu overlay consumes input while open.
    if (menu_open_) {
        int n = (int)menu_items_.size();
        switch (a) {
            // the options sheet is two columns of tiles (DESIGN 7.5): Up/Down by a row, Left/Right by a column
            case Action::Up:
            case Action::Down:
                if (menu_kind_ == MenuKind::Context) grid_step(menu_sel_, n, 2, a);
                else wrap_step(menu_sel_, n, a == Action::Up ? -1 : 1);
                break;
            case Action::Left:
            case Action::Right: {
                if (menu_sel_ >= n) break;
                MenuAction act = menu_items_[menu_sel_].action;
                if (is_value_action(act)) { adjust_setting(act, a == Action::Right ? +1 : -1); break; }
                if (menu_kind_ == MenuKind::Context) grid_step(menu_sel_, n, 2, a); else if (menu_kind_ == MenuKind::Settings) {   // Settings' columns: Right opens the page
                    if (a == Action::Right && is_submenu_action(act)) menu_activate();
                } else if (settings_cat() >= 0 && a == Action::Left) {   // Left goes back to the categories
                    input(Action::Back);
                }
                break;
            }
            case Action::Select:
                // The Search Filters rows are cycled with Left/Right; A does nothing
                // (without this it would fall through and close the modal).
                if (menu_kind_ != MenuKind::SearchFilters) menu_activate();
                break;
            case Action::Back:
            case Action::Menu:
                if (menu_kind_ == MenuKind::PlayerPlayback) {     // Playback -> options
                    open_menu();
                    break;
                }
                if (menu_kind_ == MenuKind::FeedRemoveConfirm) {  // cancel -> the list
                    feed_remove_idx_ = -1; open_feed_manage();
                    break;
                }
                if (menu_kind_ == MenuKind::FeedManage) {         // list -> Browsing
                    open_settings_browsing();
                    break;
                }
                if (menu_kind_ == MenuKind::SettingsHomeFeed) {   // nested under Browsing
                    open_settings_browsing();
                    break;
                }
                if (settings_kind(menu_kind_) && menu_kind_ != MenuKind::Settings) {
                    int cat = settings_cat();
                    open_settings();                              // submenu -> Settings
                    if (cat >= 0) menu_sel_ = cat;                // on the category it came from
                    break;
                }
                if (menu_kind_ == MenuKind::Settings) {   // Settings -> back to main menu
                    menu_open_ = false; open_main_menu();
                    break;
                }
                if (menu_kind_ == MenuKind::SearchFilters) {   // close filters; re-search if changed
                    menu_open_ = false;
                    bool changed = filt_type_ != filt_snapshot_[0] || filt_duration_ != filt_snapshot_[1]
                                || filt_date_ != filt_snapshot_[2] || filt_sort_ != filt_snapshot_[3];
                    if (changed) run_search();
                    break;
                }
                // Close the options menu. If the quality or audio track changed while
                // playing, re-resolve the current video and resume the position.
                menu_open_ = false;
                if ((quality_dirty_ || audio_dirty_) && mode_ == Mode::Playing) {
                    quality_dirty_ = false; audio_dirty_ = false; menu_paused_ = false;
                    replay_current(player_.position());
                } else {
                    if (menu_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
                    menu_paused_ = false;
                }
                break;
            default: break;
        }
        return;
    }
    if (mode_ == Mode::Playing) {
        // DESIGN 7.7: X captions, Y comments; the "next" row takes the keys while it is open
        if (upnext_open_) {
            int n = (int)upnext_idx_.size();
            switch (a) {
                case Action::Left:  if (n) wrap_step(upnext_sel_, n, -1); break;
                case Action::Right: if (n) wrap_step(upnext_sel_, n, 1); break;
                case Action::Select:
                    if (upnext_sel_ < n && !resolve_running_) {
                        int i = upnext_idx_[upnext_sel_];
                        upnext_open_ = false;
                        save_resume_position(); player_.stop(); has_pending_seek_ = false; pending_seek_ = 0;
                        sel_ = i;
                        play_item(results_[i], i);
                    }
                    break;
                case Action::Back: case Action::Up: case Action::Down: upnext_open_ = false; break;
                default: break;
            }
            return;
        }
        if (a == Action::Sort) { toggle_captions(); return; }
        if (a == Action::Search) {
            if (!player_.paused()) { player_.set_pause(true); comments_paused_ = true; }
            open_comments(now_playing_item_);
            return;
        }
        controls_until_ = SDL_GetTicks() + 2600;   // any input reveals the info overlay
        switch (a) {
            case Action::Select: player_.toggle_pause(); break;
            case Action::Menu:   open_menu(); break;   // options for the playing video
            case Action::Back:   save_resume_position(); player_.stop(); mode_ = Mode::Grid;
                                 shorts_mode_ = false; break;
            // Accumulate seeks; the actual seek fires from pump_async once the presses
            // settle (debounce). Rapid taps => one seek, not a burst of range requests.
            case Action::Left:   pending_seek_ -= 10; has_pending_seek_ = true;
                                 pending_seek_at_ = SDL_GetTicks(); break;
            case Action::Right:  pending_seek_ += 10; has_pending_seek_ = true;
                                 pending_seek_at_ = SDL_GetTicks(); break;
            case Action::Up:     if (shorts_mode_) play_neighbour(-1); break;   // DESIGN 7.7: shows the controls
            case Action::Down:   if (shorts_mode_) play_neighbour(+1); else open_upnext(); break;
            default: break;
        }
        return;
    }
    if (mode_ == Mode::Loading) {
        // Back cancels the pending resolve (poll_resolve discards a late result
        // once mode has left Loading); the worker still finishes in the bg.
        if (a == Action::Back) { mode_ = Mode::Grid; shorts_mode_ = false;
                                 status_msg_ = i18n::tr(i18n::Str::Cancelled); }
        return;
    }
    if (mode_ == Mode::Search) {
        const auto& kb = kb_layout();
        // Rows have different key counts/spans, so vertical moves are SPATIAL: keep the
        // horizontal center and land on whichever key in the target row spans it (a key
        // above the space bar lands on space, above Enter lands on Enter, etc.).
        int units = 0;   // Brick OS: rows of different widths are centered (Thai Kedmanee)
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
            float acc = indent(row);
            for (int j = 0; j < (int)kb[row].size(); ++j) {
                acc += kb[row][j].span;
                if (x < acc) return j;
            }
            return (int)kb[row].size() - 1;
        };
        if (sugg_focus_ && sugg_.empty()) sugg_focus_ = false;
        if (sugg_focus_) {
            int n = (int)sugg_.size();
            switch (a) {
                case Action::Left:  wrap_step(sugg_col_, n, -1); break;
                case Action::Right: wrap_step(sugg_col_, n, 1); break;
                case Action::Down:  sugg_focus_ = false; kb_row_ = 0; kb_clamp(); break;
                case Action::Up:    if (!held_) { sugg_focus_ = false; kb_row_ = (int)kb.size() - 1; kb_clamp(); } break;
                case Action::Select: query_input_ = sugg_[std::min(sugg_col_, n - 1)];
                                     kb_caret_ = (int)query_input_.size();
                                     submit_search(); break;
                case Action::Search: submit_search(); break;
                case Action::Sort:   query_input_.clear(); kb_caret_ = 0; break;
                case Action::Back:   mode_ = Mode::Grid; kb_mode_ = KbMode::Search; break;
                default: break;
            }
            return;
        }
        switch (a) {
            case Action::Left:  wrap_step(kb_col_, (int)kb[kb_row_].size(), -1); break;
            case Action::Right: wrap_step(kb_col_, (int)kb[kb_row_].size(), 1); break;
            case Action::Up:    if (kb_row_ > 0) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(--kb_row_, c); }
                                else if (!kb_numeric() && !sugg_.empty()) {
                                    sugg_focus_ = true;
                                    if (sugg_col_ >= (int)sugg_.size()) sugg_col_ = 0;
                                } else if (!held_) { float c = xcenter(kb_row_, kb_col_);
                                                     kb_row_ = (int)kb.size() - 1; kb_col_ = col_at(kb_row_, c); }
                                break;
            case Action::Down:  if (kb_row_ < (int)kb.size()-1) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(++kb_row_, c); }
                                else if (!held_) {
                                    if (!kb_numeric() && !sugg_.empty()) {
                                        sugg_focus_ = true;
                                        if (sugg_col_ >= (int)sugg_.size()) sugg_col_ = 0;
                                    } else { float c = xcenter(kb_row_, kb_col_); kb_row_ = 0; kb_col_ = col_at(0, c); }
                                }
                                break;
            case Action::Select: kb_activate(); break;
            case Action::Sort:   query_input_.clear(); kb_caret_ = 0; break;   // X clears
            case Action::Search: submit_search(); break;      // Y submits
            case Action::Back:   mode_ = Mode::Grid;           // cancel; code entry -> picker
                                 kb_mode_ = KbMode::Search; break;
            default: break;
        }
        if (kb_col_ >= (int)kb[kb_row_].size()) kb_col_ = kb[kb_row_].size()-1;
        return;
    }
    // Grid / carousel browse. Any user input here cancels a pending autoplay countdown.
    cancel_autoplay();
    if (a == Action::Search) { open_search(); return; }
    if (a == Action::Sort && !results_.empty()) { follow_selected(); return; }   // X: ติดตามช่อง
    if (a == Action::Sort && !query_.empty()) { toggle_search_sort(); return; }  // X: sort search
    if (a == Action::Menu) { open_menu(); return; }   // options for the highlighted item
    if (a == Action::Back && rail_focus_) { rail_focus_ = false; return; }
    if (a == Action::Back) {
        // Stage 1: B jumps to the top of the list (first tile selected).
        if (!results_.empty() && sel_ != 0) {
            sel_ = 0; scroll_ = 0; carousel_pos_ = 0; ensure_visible();
            return;
        }
        // Stage 2: at the top of a tabbed view, a non-All tab returns to the All tab
        // (Home and channel pages both). The All tab then does the view's normal back.
        if (home_tabs_active() && home_tab_ != 0) { load_home_tab(0); return; }
        if (channel_tabs_active() && chan_tab_ != 0) { load_channel_tab(0); return; }
        if (search_tabs_active() && search_tab_ != 0) { load_search_tab(0); return; }
        // Stage 3: normal back — unwind a subview (channel/playlist -> where it was
        // opened from), else a top-level view that isn't Home falls back to Home.
        // On Home itself, Back asks whether to leave YouTube.
        if (pop_view()) return;
        if (!query_.empty() || !view_label_.empty()) { load_home(); return; }
        quit_prompt_open_ = true; quit_prompt_sel_ = 0;
        return;
    }
    if (view_mode_ == ViewMode::Shelves) { shelves_input(a); return; }
    // Tabs (channel views AND Home) are switched only with the L/R shoulders
    // (cycle_tab); the d-pad stays entirely in the grid.
    int n = (int)results_.size();
    if (n == 0) return;
    auto activate = [&]{
        const yt::SearchResult* v = selected();
        if (v && v->is_channel()) open_channel(*v);        // open channel -> its uploads
        else if (v && v->is_playlist()) open_playlist(*v); // open playlist -> its videos
        else if (v && v->is_post()) open_post(*v);         // read the full post
        else request_playback();                            // play video
    };
    if (view_mode_ != ViewMode::Grid) {   // carousel & coverflow are 1-D strips
        switch (a) {
            case Action::Left:  wrap_step(sel_, n, -1); break;
            case Action::Right: wrap_step(sel_, n, 1); break;
            case Action::Select: activate(); break;
            default: break;
        }
        return;   // carousel_pos_ eases toward sel_ in render_carousel/coverflow
    }
    switch (a) {
        case Action::Left: case Action::Right: case Action::Up: case Action::Down:
            grid_step(sel_, n, cols_, a); break;
        case Action::Select: activate(); break;
        default: break;
    }
    ensure_visible();
}

static std::string sb_category_label(const std::string& c) {
    if (c == "sponsor")        return "sponsor";
    if (c == "selfpromo")      return "self-promo";
    if (c == "interaction")    return "interaction reminder";
    if (c == "intro")          return "intro";
    if (c == "outro")          return "outro";
    if (c == "music_offtopic") return "non-music section";
    if (c == "preview")        return "recap";
    if (c == "filler")         return "filler";
    return c.empty() ? "segment" : c;
}

void App::pump_async() {
    kb_tick();
    pump_suggestions();
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
    thumbs_.pump();
    poll_resolve();
    poll_channel_info();
    poll_more();
    poll_more_home();
    poll_cast_discovery();
    poll_cast_play();
    poll_cast_link();
    // Periodically ask the TV for its real position; the reply lands on the event
    // backchannel and refreshes cast_ev_pos_ for accurate seek.
    if (casting_ && SDL_GetTicks() - cast_nowplaying_at_ > 3000) {
        cast_command("getNowPlaying");
        cast_nowplaying_at_ = SDL_GetTicks();
    }
    poll_refresh();
    poll_description();
    poll_comments();
    poll_download();
    poll_sponsorblock();
    poll_hwdec_detect();
    // Runtime hwdec truth: once the decoder is up, mpv says what it actually
    // uses. If a hardware method is active on a device with no manifest bundle
    // (desktop VideoToolbox/VAAPI), remember it so Settings > Video shows the
    // toggle from now on — the same "only when it's real" rule, learned live.
    if (mode_ == Mode::Playing && !hwdec_probed_this_video_) {
        std::string cur = player_.hwdec_current();
        if (!cur.empty()) {
            hwdec_probed_this_video_ = true;
            if (kHwdecEnabled && cur != "no" && !hwdec_capable_) {
                hwdec_capable_ = true;
                it_.set_setting_str("hwdec_seen", cur);
                if (getenv("YTC_DEBUG"))
                    std::fprintf(stderr, "[hwdec] learned hardware decode: %s\n", cur.c_str());
            }
        }
    }
    poll_captions();
    poll_caption_download();
    // A caption fetch failed earlier: surface it now that the menu is down.
    if (cc_fail_pending_ && !menu_open_) {
        cc_fail_pending_ = false;
        status_msg_ = i18n::tr(i18n::Str::CcUnavailable);
        status_until_ = SDL_GetTicks() + 4000;
    }
    poll_related_autoplay();
    step_autoplay();
    // SponsorBlock: auto-skip when playback enters a segment. Uses an immediate
    // (non-debounced) seek to the segment end; each segment skips at most once per
    // play so a deliberate seek back in doesn't fight the user.
    if (sponsorblock_ && mode_ == Mode::Playing && !menu_open_ &&
        !sb_segments_.empty() && !player_.paused()) {
        double pos = player_.position();
        for (size_t i = 0; i < sb_segments_.size(); ++i) {
            const auto& sg = sb_segments_[i];
            if (i < sb_skipped_.size() && !sb_skipped_[i] &&
                pos >= sg.start && pos < sg.end - 0.20) {
                player_.seek_relative(sg.end - pos);
                sb_skipped_[i] = true;
                played_max_ = std::max(played_max_, sg.end);
                if (getenv("YTC_DEBUG"))
                    std::fprintf(stderr, "[sponsorblock] skipped %s [%.1f-%.1f]\n",
                                 sg.category.c_str(), sg.start, sg.end);
                status_msg_ = std::string(i18n::tr(i18n::Str::SkippedPrefix)) + " " + sb_category_label(sg.category);
                status_until_ = SDL_GetTicks() + 1600;
                break;
            }
        }
    }
    // Fire a scheduled network-retry once its backoff elapses.
    if (retry_pending_ && !refresh_running_ && mode_ != Mode::Playing &&
        SDL_GetTicks() >= retry_at_) {
        retry_pending_ = false;
        refresh_current_view(/*is_retry=*/true);
    }
    maybe_load_more();
    maybe_load_more_home();
    // Apply freshly-arrived restricted verdicts to the visible list.
    if (hide_restricted_ && rcheck_.drain_dirty() && mode_ != Mode::Playing) {
        size_t before = results_.size();
        filter_hidden(results_);
        if (results_.size() != before) {
            if (sel_ >= (int)results_.size())
                sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            ensure_visible();
        }
    }
    if (mode_ == Mode::Playing) {
        // Track the furthest position actually played (anchors the paced-seek window).
        double p = player_.position();
        if (p > played_max_) played_max_ = p;
        // Fire a debounced seek once the rapid presses settle (~350ms quiet).
        if (has_pending_seek_ && SDL_GetTicks() - pending_seek_at_ >= 350) {
            double delta = pending_seek_;
            // Restricted (paced) streams 403 any read outside the server's opaque
            // sliding window, so ONLY allow seeks inside mpv's OWN cache — those
            // never touch the network. Forward: up to the buffered end; backward:
            // bounded by the back-buffer (approximate, conservative).
            if (playing_paced_) {
                double target = p + delta;
                if (delta > 0) {
                    double limit = player_.cached_until() - 2.0;   // stay inside cache
                    if (limit < p) limit = p;
                    if (target > limit) {
                        delta = limit - p;
                        status_msg_ = i18n::tr(i18n::Str::SeekLimited);
                        status_until_ = SDL_GetTicks() + 4000;
                    }
                } else if (delta < 0) {
                    double floor_t = p - 60.0;                     // ~back-buffer span
                    if (floor_t < 0) floor_t = 0;
                    if (target < floor_t) {
                        delta = floor_t - p;
                        status_msg_ = i18n::tr(i18n::Str::SeekLimited);
                        status_until_ = SDL_GetTicks() + 4000;
                    }
                }
                if (delta > -0.5 && delta < 0.5) delta = 0;        // nothing meaningful left
            }
            if (delta != 0) player_.seek_relative(delta);
            has_pending_seek_ = false; pending_seek_ = 0;
        }
        // Made-for-kids (paced) delivery: without a PO token YouTube serves only about the first quarter of
        // each stream (measured: every itag 403s past ~25 % of its bytes, fresh URLs too, waiting does not
        // move it). Playback then freezes for good; after 6 s without progress, say why and move on.
        bool wall = false;
        if (playing_paced_ && !player_.paused() && !has_pending_seek_) {
            Uint32 now = SDL_GetTicks();
            if (p > stall_pos_ + 0.2) { stall_pos_ = p; stall_since_ = now; stall_moved_ = true; }
            else if (stall_moved_ && now - stall_since_ > 6000) wall = true;
        } else stall_since_ = SDL_GetTicks();
        if (wall) {
            save_resume_position();
            handle_playback_ended();
            status_msg_ = i18n::tr(i18n::Str::KidsWall);
            status_until_ = SDL_GetTicks() + 7000;
        } else if (!player_.pump()) {       // playback ended (EOF/error)
            handle_playback_ended();  // autoplay next / related, or back to grid
        }
    }
}

void App::toggle_view() {
    view_mode_ = (ViewMode)(((int)view_mode_ + 1) % 5);   // Grid->Carousel->3D->Coverflow->TV
    it_.set_setting_int("view", (int)view_mode_);
    carousel_pos_ = sel_; carousel_vel_ = 0;
    scroll_ = 0; ensure_visible();
}
// Critically-damped spring (Unity-style SmoothDamp) glide of carousel_pos_ toward
// the selected index — frame-rate independent, velocity-continuous, so a press
// spins smoothly into place and rapid presses flow together without restarting.
void App::update_carousel_anim() {
    unsigned now = SDL_GetTicks();
    float dt = anim_last_ms_ ? (now - anim_last_ms_) / 1000.f : 0.016f;
    anim_last_ms_ = now;
    if (dt > 0.05f) dt = 0.05f;                 // clamp after a hitch / view switch
    const float smoothTime = 0.20f;             // ~time to settle; higher = slower spin
    float target = (float)sel_;
    float omega = 2.f / smoothTime;
    float x = omega * dt;
    float ex = 1.f / (1.f + x + 0.48f*x*x + 0.235f*x*x*x);
    float change = carousel_pos_ - target;
    float temp = (carousel_vel_ + omega * change) * dt;
    carousel_vel_ = (carousel_vel_ - omega * temp) * ex;
    carousel_pos_ = target + (change + temp) * ex;
    if (std::abs(carousel_pos_ - target) < 0.001f && std::abs(carousel_vel_) < 0.02f) {
        carousel_pos_ = target; carousel_vel_ = 0;
    }
}
// L/R shoulders: switch the active tab (channel: 0..4, Home: 0..3). No-op elsewhere
// or while a menu/overlay is up.
void App::cycle_tab(int dir) {
    if (menu_open_ || desc_open_ || resume_prompt_open_ || mode_ != Mode::Grid) return;
    if (channel_tabs_active()) {
        int t = chan_tab_ + dir; if (t < 0 || t > 4) return;
        load_channel_tab(t);
    } else if (home_tabs_active()) {
        int t = home_tab_ + dir; if (t < 0 || t > 4) return;
        load_home_tab(t);
    } else if (search_tabs_active()) {
        int t = search_tab_ + dir; if (t < 0 || t > 4) return;   // All/Videos/Shorts/Channels/Playlists
        load_search_tab(t);
    } else return;
    tab_focus_ = false;   // shoulder-switch keeps focus on content, not the strip
}

void App::seek_chapter(int dir) {
    if (mode_ != Mode::Playing || menu_open_ || desc_open_ || dir == 0) return;
    if (now_playing_chapters_.empty()) {   // DESIGN 7.7: no chapters, L2/R2 jump 30 s
        pending_seek_ += 30 * dir; has_pending_seek_ = true;
        pending_seek_at_ = SDL_GetTicks(); controls_until_ = pending_seek_at_ + 2600;
        return;
    }

    // Include the pending scrub target so quick repeated shoulder presses can cross
    // several chapters before the existing seek debounce fires.
    double pos = player_.position() + (has_pending_seek_ ? pending_seek_ : 0.0);
    int current = -1;
    for (int i = 0; i < (int)now_playing_chapters_.size(); ++i) {
        if (now_playing_chapters_[i] <= pos + 0.5) current = i;
        else break;
    }

    int target = current;
    if (dir > 0) {
        target = current + 1;
        if (target >= (int)now_playing_chapters_.size()) return;
    } else {
        // As in most media players, L restarts the current chapter when more than
        // three seconds into it; another press goes to the previous chapter.
        if (current < 0) return;
        if (pos - now_playing_chapters_[current] <= 3.0) --target;
        if (target < 0) target = 0;
        if (std::abs(pos - now_playing_chapters_[target]) < 0.5) return;
    }

    pending_seek_ = now_playing_chapters_[target] - player_.position();
    has_pending_seek_ = true;
    pending_seek_at_ = SDL_GetTicks();
    controls_until_ = pending_seek_at_ + 2600;
}

// ---------- DESIGN 7.7 / 7.8 ----------
// Controller buttons the player owns (before the browse mapping). False: not handled here.
bool App::player_pad(int b) {
    bool shorts_loading = mode_ == Mode::Loading && shorts_mode_;
    if ((mode_ != Mode::Playing && !shorts_loading) || menu_open_ || desc_open_ || comments_open_ ||
        resume_prompt_open_ || quit_prompt_open_ || casting_ || cast_picker_open_)
        return false;
    if (shorts_loading) return b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ||
                               b == SDL_CONTROLLER_BUTTON_START || b == SDL_CONTROLLER_BUTTON_X;   // wait for it
    switch (b) {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  upnext_open_ = false; play_neighbour(-1); return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: upnext_open_ = false; play_neighbour(+1); return true;
        case SDL_CONTROLLER_BUTTON_START:         controls_until_ = 0; upnext_open_ = false; return true;
        default: return false;
    }
}

// L2/R2 pressed (an edge, not the axis): the player's chapters, a page of a list
void App::trigger(bool right) {
    if (menu_open_ || desc_open_ || comments_open_ || resume_prompt_open_ || quit_prompt_open_ || casting_ ||
        cast_picker_open_)
        return;
    if (mode_ == Mode::Playing) {
        if (!shorts_mode_ && !upnext_open_) seek_chapter(right ? +1 : -1);
        return;
    }
    if (mode_ != Mode::Grid) return;
    held_ = true;   // a page: three rows, stopping at the ends (no wrap)
    for (int k = 0; k < 3; ++k) input(right ? Action::Down : Action::Up);
    held_ = false;
}

// The playable clip next to the playing one in the list (Shorts mode: the next Short), or -1
int App::neighbour_index(int dir) const {
    if (now_playing_index_ < 0) return -1;
    for (int i = now_playing_index_ + dir; i >= 0 && i < (int)results_.size(); i += dir) {
        const yt::SearchResult& v = results_[i];
        if (v.video_id.empty() || v.is_channel() || v.is_playlist() || v.is_post()) continue;
        if (shorts_mode_ && !v.is_short) continue;
        return i;
    }
    return -1;
}

void App::play_neighbour(int dir) {
    if (resolve_running_ || mode_ != Mode::Playing) return;
    int i = neighbour_index(dir);
    if (i < 0) {
        if (dir > 0 && !shorts_mode_ && !now_playing_item_.video_id.empty()) {   // end of the list: related
            save_resume_position(); player_.stop(); has_pending_seek_ = false; pending_seek_ = 0;
            start_related_autoplay(now_playing_item_.video_id);
            return;
        }
        status_msg_ = i18n::tr(dir < 0 ? i18n::Str::FirstClip
                                       : (more_running_ || home_more_running_) ? i18n::Str::FindingNext
                                                                               : i18n::Str::NoMoreVideos);
        status_until_ = SDL_GetTicks() + 2000;
        return;
    }
    save_resume_position(); player_.stop(); has_pending_seek_ = false; pending_seek_ = 0;
    if (shorts_mode_) {
        shorts_anim_dir_ = dir; shorts_anim_at_ = SDL_GetTicks();
        shorts_prev_thumb_ = now_playing_item_.thumbnail_url;
    }
    sel_ = i;   // B lands on it; near the end of the list this pages more in (maybe_load_more)
    play_item(results_[i], i);
}

// X in the player: captions off, or back on (the last track, else the first)
void App::toggle_captions() {
    controls_until_ = SDL_GetTicks() + 2600;
    int n = (int)cc_tracks_.size();
    if (n == 0) {
        status_msg_ = i18n::tr(i18n::Str::CcToastNone); status_until_ = SDL_GetTicks() + 2000;
        return;
    }
    if (cc_sel_ > 0) { cc_last_ = cc_sel_; cc_sel_ = 0; }
    else cc_sel_ = cc_last_ > 0 && cc_last_ <= n ? cc_last_ : 1;
    apply_caption_selection();
    status_msg_ = cc_sel_ > 0 ? std::string(i18n::tr(i18n::Str::CcToastOn)) + cc_tracks_[cc_sel_ - 1].name
                              : std::string(i18n::tr(i18n::Str::CcToastOff));
    status_until_ = SDL_GetTicks() + 2000;
}

// Down in the player: the next clips of the list in a row from the bottom
void App::open_upnext() {
    upnext_idx_.clear();
    for (int i = now_playing_index_ < 0 ? (int)results_.size() : now_playing_index_ + 1;
         i < (int)results_.size() && upnext_idx_.size() < 10; ++i) {
        const yt::SearchResult& v = results_[i];
        if (v.video_id.empty() || v.is_channel() || v.is_playlist() || v.is_post()) continue;
        upnext_idx_.push_back(i);
    }
    if (upnext_idx_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::NoMoreVideos); status_until_ = SDL_GetTicks() + 2000;
        return;
    }
    upnext_sel_ = 0;
    upnext_open_ = true;
    controls_until_ = 0;
}

std::vector<int> App::shorts_list() const {
    std::vector<int> r;
    for (int i = 0; i < (int)results_.size(); ++i)
        if (results_[i].is_short && !results_[i].video_id.empty()) r.push_back(i);
    return r;
}

void App::render(gfx::Renderer& rn) {
    if (mode_ == Mode::Playing)      render_player(rn);
    else if (mode_ == Mode::Search)  render_search(rn);
    else {
        switch (view_mode_) {
            case ViewMode::Carousel:   render_carousel(rn); break;
            case ViewMode::Carousel3D: render_carousel3d(rn); break;
            case ViewMode::Coverflow:  render_coverflow(rn); break;
            case ViewMode::Shelves:    render_shelves(rn); break;
            default:                   render_grid(rn); break;
        }
        if (mode_ == Mode::Loading && shorts_mode_) render_shorts_loading(rn);
        else if (mode_ == Mode::Loading) render_loading(rn);
    }
    if (menu_open_) render_menu(rn);   // overlay on top of whatever view
    if (desc_open_) {   // description floats above everything (a post: its own page)
        if (desc_is_post_) render_post(rn); else render_description(rn);
    }
    if (comments_open_) render_comments(rn);  // comments float above everything
    if (resume_prompt_open_) render_resume_prompt(rn);
    if (quit_prompt_open_) render_quit_prompt(rn);
    if (casting_) render_remote(rn);
    else if (cast_picker_open_ && mode_ != Mode::Search) render_cast_picker(rn);
    else if (cast_manage_open_ && mode_ != Mode::Search) render_manage_devices(rn);
}

// Rows whose A press opens another page (submenu / modal) rather than acting.
// Rows whose value cycles with Left/Right (A does nothing). ONE list, used by
// input handling, the footer hint, and the "‹ value ›" rendering.
bool App::is_value_action(MenuAction a) {
    using A = MenuAction;
    switch (a) {
        case A::CycleMaxQuality: case A::ToggleStats: case A::ToggleHideRestricted:
        case A::ToggleAskResume: case A::CycleView: case A::CycleVolume: case A::CycleHwdec:
        case A::CycleAspect: case A::CycleAudioLang: case A::CycleCaptionLang:
        case A::CycleAudioTrack: case A::CycleCaptionSize: case A::CycleCaptionStyle:
        case A::CycleSpeed: case A::ToggleSponsorBlock: case A::CycleCaptions:
        case A::ToggleAutoplay: case A::ToggleBgOnExit: case A::ToggleFeedFavorites: case A::ToggleFeedHistory:
        case A::ToggleFeedCustom: case A::CycleLanguage: case A::CycleFilterType:
        case A::CycleFilterDuration: case A::CycleFilterDate: case A::CycleFilterSort:
            return true;
        default: return false;
    }
}

bool App::is_submenu_action(MenuAction a) {
    using A = MenuAction;
    switch (a) {
        case A::GoSettings: case A::GoSettingsAudio: case A::GoSettingsVideo:
        case A::GoSettingsCaptions: case A::GoSettingsPlayback: case A::GoSettingsBrowsing:
        case A::GoHomeFeedMenu: case A::GoCustomFeed: case A::GoLinkedDevices:
        case A::GoPlayback: case A::OpenSearchFilters:
            return true;
        default: return false;
    }
}

// render_menu: ui_bos_menu.cpp





// Compose the raw metadata lines for one result (no ellipsize — that's width-dependent
// and done at draw time). Called once per item when results_ changes, so humanize_age
// and the string building don't run every frame.
App::TileLines App::compose_lines(const yt::SearchResult& v, ChannelMetaCache& cmeta) {
    TileLines t;
    if (v.is_channel()) {
        t.l1 = v.title; t.l2 = v.subs_text;
        t.l3 = cmeta.video_count(v.channel_id); if (t.l3.empty()) t.l3 = v.author;
    } else if (v.is_post()) {
        t.l1 = v.title.substr(0, v.title.find('\n'));
        t.l2 = v.author;                             // channel name
        std::string type = v.video_id.empty() ? i18n::tr(i18n::Str::TilePost)
             : std::string(i18n::tr(i18n::Str::TilePost)) + "   -   " + i18n::tr(i18n::Str::TileVideo);
        std::string meta = v.view_count_text;        // "N likes"
        if (!v.published_text.empty()) meta += (meta.empty()?"":"   -   ") + v.published_text;
        t.l3 = meta.empty() ? type : (type + "   -   " + meta);
    } else if (v.is_playlist()) {
        t.l1 = v.title; t.l2 = v.author; t.l3 = i18n::tr(i18n::Str::TilePlaylist);
    } else {
        // Video / Short: lead the footer with the type (so it's consistent with the
        // Playlist/Post tiles and distinguishes a Short from a Video with the same
        // title/thumbnail), then the view count + age.
        t.l1 = v.title; t.l2 = v.author;
        std::string age = humanize_age(v.published_text);
        std::string meta = v.view_count_text;
        if (!age.empty()) meta += (meta.empty()?"":"   -   ") + age;
        std::string type = i18n::tr(v.is_live ? i18n::Str::TileLive
                                  : v.is_short ? i18n::Str::TileShort : i18n::Str::TileVideo);
        t.l3 = meta.empty() ? type : (type + "   -   " + meta);
    }
    return t;
}
void App::build_tile_lines() {
    shelves_src_n_ = (size_t)-1;   // TV view rows follow results_
    tile_lines_.clear();
    tile_lines_.reserve(results_.size());
    for (const auto& v : results_) tile_lines_.push_back(compose_lines(v, chan_meta_));
}


// ---- TV view (ViewMode::Shelves) ------------------------------------------------
static int shelf_kind(const yt::SearchResult& v) {
    return v.is_channel() ? 2 : v.is_playlist() ? 3 : v.is_post() ? 4 : v.is_short ? 1 : 0;
}
static constexpr int kRailItems = 6;   // Search, Home, Favorites, Watch later, History, Settings
static constexpr int kDrawerItems = 8; // + Favourite playlists, Downloads (DESIGN 7.5 drawer)

void App::rebuild_shelves() {
    using S = i18n::Str;
    static const S kNames[5] = {S::TabVideos, S::TabShorts, S::FilterChannels, S::TabPlaylists, S::TabPosts};
    static const size_t kRow = 12;
    std::vector<int> groups[5];
    for (int i = 0; i < (int)results_.size(); ++i) groups[shelf_kind(results_[i])].push_back(i);
    shelves_.clear();
    for (int k = 0; k < 5; ++k)
        for (size_t at = 0; at < groups[k].size(); at += kRow) {
            Shelf sh;
            if (at == 0) sh.title = i18n::tr(kNames[k]);
            sh.idx.assign(groups[k].begin() + at,
                          groups[k].begin() + std::min(groups[k].size(), at + kRow));
            shelves_.push_back(std::move(sh));
        }
    shelf_off_.resize(shelves_.size(), 0.f);
    shelf_col_.resize(shelves_.size(), 0);
    shelves_src_n_ = results_.size();
}

bool App::locate_shelf(int sel, int& row, int& col) const {
    for (int r = 0; r < (int)shelves_.size(); ++r)
        for (int c = 0; c < (int)shelves_[r].idx.size(); ++c)
            if (shelves_[r].idx[c] == sel) { row = r; col = c; return true; }
    return false;
}

int App::rail_page() const {
    if (in_channel_view_) return 1;
    if (view_label_ == "Favorite Channels") return 2;
    if (view_label_ == "Watch Later") return 3;
    if (view_label_ == "History") return 4;
    if (view_label_ == "Favorite Playlists") return 6;
    if (view_label_ == "Downloads") return 7;
    return query_.empty() ? 1 : 0;
}

void App::rail_activate() {
    rail_focus_ = false;
    if (rail_sel_ == 0) { open_search(); return; }
    if (rail_sel_ == 5) { rail_focus_ = true; open_main_menu(); open_settings(); return; }
    in_channel_view_ = false;
    if      (rail_sel_ == 1) load_home();
    else if (rail_sel_ == 2) load_favorites();
    else if (rail_sel_ == 3) load_watch_later();
    else if (rail_sel_ == 6) load_favorite_playlists();
    else if (rail_sel_ == 7) load_downloads();
    else                     load_history();
}

// DESIGN 5.7: a fresh press at an end goes to the other end; a held direction stops there.
// Returns 1 moved, 2 wrapped, 0 stayed.
int App::wrap_step(int& sel, int n, int dir) {
    if (n <= 0) return 0;
    int to = sel + dir;
    if (to >= 0 && to < n) { sel = to; return 1; }
    if (held_ || n < 2) return 0;
    sel = to < 0 ? n - 1 : 0;
    return 2;
}
// A grid of `cols` read row by row: Left/Right go along the reading order (past a row's end to the next row's start),
// Up/Down by a row keeping the column (the last row may be short: Down lands on its last tile); the ends wrap.
void App::grid_step(int& sel, int n, int cols, Action a) {
    if (n <= 0 || cols < 1) return;
    int rows = (n + cols - 1) / cols, r = sel / cols, c = sel % cols;
    if (a == Action::Left || a == Action::Right) { wrap_step(sel, n, a == Action::Left ? -1 : 1); return; }
    int nr = r;
    if (!wrap_step(nr, rows, a == Action::Up ? -1 : 1)) return;
    sel = std::min(nr * cols + c, n - 1);
}

void App::shelves_input(Action a) {
    if (shelves_src_n_ != results_.size()) rebuild_shelves();
    if (rail_focus_) {
        switch (a) {
            case Action::Up:     wrap_step(rail_sel_, kDrawerItems, -1); break;
            case Action::Down:   wrap_step(rail_sel_, kDrawerItems, 1); break;
            case Action::Right:  if (!shelves_.empty()) rail_focus_ = false; break;
            case Action::Select: rail_activate(); break;
            default: break;
        }
        return;
    }
    int r = 0, c = 0;
    if (!locate_shelf(sel_, r, c)) {
        if (a == Action::Left || shelves_.empty()) { rail_focus_ = true; rail_sel_ = rail_page(); }
        else sel_ = shelves_[0].idx[0];
        return;
    }
    switch (a) {
        case Action::Left:
            if (c > 0) c--;
            else { rail_focus_ = true; rail_sel_ = rail_page(); return; }
            break;
        case Action::Right: wrap_step(c, (int)shelves_[r].idx.size(), 1); break;
        case Action::Up:
        case Action::Down: {
            int nr = r;
            if (!wrap_step(nr, (int)shelves_.size(), a == Action::Up ? -1 : 1) || nr == r) return;
            shelf_col_[r] = c;   // each row remembers its tile, like a TV launcher
            r = nr;
            c = std::min(shelf_col_[r], (int)shelves_[r].idx.size() - 1);
            break;
        }
        case Action::Select: {
            const yt::SearchResult* v = selected();
            if (v && v->is_channel()) open_channel(*v);
            else if (v && v->is_playlist()) open_playlist(*v);
            else if (v && v->is_post()) open_post(*v);
            else request_playback();
            return;
        }
        default: return;
    }
    shelf_col_[r] = c;
    sel_ = shelves_[r].idx[c];
}

// Rail symbols: a drawn magnifier for Search, DejaVu glyphs for the rest.
void App::draw_rail_icon(gfx::Renderer& rn, int item, float cx, float cy, float u, gfx::Color c) {
    if (item == 0) {
        float r = 10*u, ox = cx - 3*u, oy = cy - 3*u, t = 3*u;
        rn.round_rect({ox - r, oy - r, 2*r, 2*r}, r, c);
        rn.round_rect({ox - r + t, oy - r + t, 2*(r - t), 2*(r - t)}, r - t, theme_.bg);
        float hx = ox + r*0.62f, hy = oy + r*0.62f, L = 8*u, w = 1.6f*u;
        rn.quad4(hx - w, hy + w, hx + w, hy - w, hx + L + w, hy + L - w, hx + L - w, hy + L + w, c);
        return;
    }
    static const char* kGlyph[] = {"", "\xE2\x8C\x82", "\xE2\x99\xA5", "\xE2\x97\xB7",
                                   "\xE2\x86\xBA", "\xE2\x9A\x99"};   // house heart clock arrow gear
    const gfx::Font& f = *font_icon_;
    const char* g = kGlyph[item];
    rn.text(f, g, cx - f.text_width(g) / 2, cy - f.line_height() / 2, c);
}

void App::follow_selected() {
    const yt::SearchResult* v = selected();
    if (!v || v->channel_id.empty()) return;
    std::string name = v->is_channel() ? v->title : v->author;
    if (fav_ids_.count(v->channel_id)) {
        it_.remove_favorite(v->channel_id);
        status_msg_ = i18n::tr(i18n::Str::RemovedFav) + std::string(": ") + name;
    } else {
        it_.add_favorite(v->channel_id, name);
        status_msg_ = i18n::tr(i18n::Str::AddedFav) + std::string(": ") + name;
    }
    status_until_ = SDL_GetTicks() + 4000;
    refresh_favorites();
}

int length_seconds(const std::string& s) {   // "1:02:03" / "8:24" -> seconds, 0 if none
    int total = 0, part = 0; bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') { part = part * 10 + (c - '0'); any = true; }
        else if (c == ':') { total = total * 60 + part; part = 0; }
        else if (c != ' ') return 0;
    }
    return any ? total * 60 + part : 0;
}

void App::render_shelves(gfx::Renderer& rn) {
    // brick-os DESIGN.md 7.5 (v2-YouTube): status bar, the icon rail, search pill + tab chips, rows of cards, hint bar
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(dzc::bg);
    if (shelves_src_n_ != results_.size()) rebuild_shelves();

    // a channel: its header (102 high) above the search pill and the tabs
    const float bar = 64*u, cx0 = 112*u, head_h = in_channel_view_ ? 102*u : 0.f;
    const float top_y = bar + 4*u + head_h, pill_h = 48*u;
    const float rows_top = top_y + pill_h, hint_top = H - 64*u;
    const float cardw = 300*u, gap = 22*u, thumbh = 169*u, rad = 18*u;
    const gfx::Font &rf = dz(DZ_ROW), &cf = dz(DZ_CARD), &cff = dz(DZ_CARD_FOC), &mf = dz(DZ_META);
    const float head = 16*u + rf.line_height() + 14*u;
    auto row_h = [&](const Shelf& sh) {
        return (sh.title.empty() ? 16*u : head) + thumbh + 12*u + cf.line_height() + 2*u + mf.line_height();
    };

    int fr = 0, fc = 0;
    bool have = locate_shelf(sel_, fr, fc);
    float target = 0;
    for (int r = 0; r < fr && have; ++r) target += row_h(shelves_[r]);
    shelves_scroll_ += (target - shelves_scroll_) * 0.3f;
    if (std::abs(target - shelves_scroll_) < 0.5f) shelves_scroll_ = target;

    if (results_.empty()) browse_empty_overlay(rn);
    float y = rows_top - shelves_scroll_;
    for (int r = 0; r < (int)shelves_.size(); ++r) {
        const Shelf& sh = shelves_[r];
        float rh = row_h(sh);
        if (y > hint_top) break;
        int n = (int)sh.idx.size();
        int col = (have && r == fr) ? fc : std::min(shelf_col_[r], n - 1);
        float content_w = n * (cardw + gap) - gap, view_w = W - cx0 - 32*u;
        float tx = content_w <= view_w ? 0.f : std::min(col * (cardw + gap), content_w - view_w);
        shelf_off_[r] += (tx - shelf_off_[r]) * 0.3f;
        if (std::abs(tx - shelf_off_[r]) < 0.5f) shelf_off_[r] = tx;
        if (y + rh < rows_top - 40*u) { y += rh; continue; }
        float ty = y + (sh.title.empty() ? 16*u : head);
        if (!sh.title.empty()) rn.text(rf, sh.title, cx0, y + 16*u, dzc::white);
        for (int c = 0; c < n; ++c) {
            float x = cx0 + c * (cardw + gap) - shelf_off_[r];
            int i = sh.idx[c];
            const yt::SearchResult& v = results_[i];
            if (x + cardw < -2*cardw || x > W + 2*cardw) continue;
            if (!v.thumbnail_url.empty()) thumbs_.request(v.thumbnail_url);
            else if (v.is_channel()) { std::string av = chan_meta_.avatar(v.channel_id);
                                       if (!av.empty()) thumbs_.request(av); }
            if (x + cardw < 0 || x > W) continue;
            bool foc = have && !rail_focus_ && r == fr && c == fc;
            design_card(rn, i, x, ty, cardw, thumbh, foc);
        }
        y += rh;
    }

    design_browse_top(rn, cx0);
    const gfx::Color text2 = gfx::Color::rgb(0xB4B4BA);

    // the rail: 96 wide, 56 circles from 28 under the status bar, gap 14, icons 30; the current page on surface2
    rn.quad({0, bar, cx0 - 18*u, hint_top - bar}, dzc::bg);  // cards scrolled off to the left (not the ring)
    static const char* kRailIcons[kRailItems] = {"search", "home", "heart", "clock", "history", "settings"};
    int page = rail_page();
    for (int k = 0; k < kRailItems; ++k) {
        float cy = bar + 28*u + 28*u + k * 70*u, cx = 48*u;
        if (k == page) rn.round_rect({cx - 28*u, cy - 28*u, 56*u, 56*u}, 28*u, gfx::Color::rgb(0x2C2C30));
        bos_icon(rn, kRailIcons[k], cx - 15*u, cy - 15*u, 30*u, k == page ? dzc::white : text2);
    }

    if (rail_focus_) {   // the drawer (DESIGN 7.5): the rail's places and two more
        design_hint_bar(rn, {{"A", i18n::tr(S::HintOpen)}, {"B", i18n::tr(S::HintClose)}},
                        {{"START", i18n::tr(S::HintMenu)}});
        design_drawer(rn, {{"search", i18n::tr(S::Search)}, {"home", i18n::tr(S::Home)},
                           {"heart", i18n::tr(S::FavoriteChannels)}, {"clock", i18n::tr(S::WatchLater)},
                           {"history", i18n::tr(S::History)}, {"settings", i18n::tr(S::Settings)},
                           {"list", i18n::tr(S::FavoritePlaylists)}, {"download", i18n::tr(S::Downloads)}},
                      rail_sel_, page);
    } else design_browse_hints(rn);
    design_toast(rn);
    rn.end();
}

// START's menu (MenuKind::Main) as the drawer, over whatever page is showing
void App::render_main_drawer(gfx::Renderer& rn) {
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    rn.begin(W, H);
    std::vector<std::pair<const char*, std::string>> rows;
    for (auto& it : menu_items_) {
        const char* ic = "list";
        switch (it.action) {
            case MenuAction::GoHome: ic = "home"; break;
            case MenuAction::GoFavorites: ic = "heart"; break;
            case MenuAction::GoFavoritePlaylists: ic = "list"; break;
            case MenuAction::GoWatchLater: ic = "clock"; break;
            case MenuAction::GoDownloads: ic = "download"; break;
            case MenuAction::GoHistory: ic = "history"; break;
            case MenuAction::GoSettings: ic = "settings"; break;
            case MenuAction::Quit: ic = "power"; break;
            default: break;
        }
        rows.push_back({ic, it.label});
    }
    design_status_bar(rn);
    design_hint_bar(rn, {{"A", i18n::tr(S::HintOpen)}, {"B", i18n::tr(S::HintClose)}},
                    {{"START", i18n::tr(S::HintMenu)}});
    design_drawer(rn, rows, menu_sel_, -1);
    rn.end();
}



// Transient status banner (resolve errors, "Added X to favorites", etc.). Width is
// clamped to the screen and the message ellipsized, so a long channel name never
// runs off both edges. Shared by every view's render.
void App::draw_status_banner(gfx::Renderer& rn, float, float) {   // DESIGN 5.9: the OS toast
    design_toast(rn);
}





void App::adjust_volume(int delta) {
    int v = volume_ + delta;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    volume_ = v;
    player_.set_volume(volume_);
    it_.set_setting_int("volume", volume_);       // remember for future videos
    volume_overlay_until_ = SDL_GetTicks() + 1400;
}

void App::render_player(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    // mpv paints the video frame into the framebuffer first...
    static int fc = 0;
    if (kDbg && (fc++ % 20 == 0))
        std::fprintf(stderr, "[play] frame %d pos=%.1f dur=%.1f\n", fc, player_.position(), player_.duration());
    // Full-screen video (mpv paints the frame first)...
    player_.render(W, H);
    // ...then we overlay UI on top (mpv leaves GL state; begin() resets ours).
    rn.begin(W, H);

    double pos = player_.position(), dur = player_.duration();
    // While a debounced seek is pending, show the TARGET position (scrub preview).
    if (has_pending_seek_) {
        pos += pending_seek_;
        if (pos < 0) pos = 0;
        if (dur > 0 && pos > dur) pos = dur;
    }
    float frac = dur > 0 ? (float)(pos / dur) : 0.f;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    auto tstr = [](double t){ int m=(int)t/60, sec=(int)t%60; char b[16];
        std::snprintf(b,sizeof b,"%d:%02d",m,sec); return std::string(b); };

    // Controls / details overlay: fades out shortly after playback starts, and
    // reappears on pause / start / options-menu / seek (controls_until_ deadline).
    const unsigned kRevealMs = 2600, kFadeMs = 450;
    unsigned now = SDL_GetTicks();
    bool force = menu_open_ || player_.paused();
    if (force) controls_until_ = now + kRevealMs;   // stays up; lingers then fades after resume
    float alpha;
    long remain = (long)controls_until_ - (long)now;
    if (remain <= 0) alpha = 0.f;
    else if (remain >= (long)kFadeMs) alpha = 1.f;
    else alpha = (float)remain / (float)kFadeMs;    // fade tail

    if (shorts_mode_) render_shorts(rn, pos, dur, alpha);       // DESIGN 7.8 (ui_bos_player.cpp)
    else if (upnext_open_) render_upnext(rn);                    // DESIGN 7.7
    else if (alpha > 0.001f) player_overlay(rn, pos, dur, alpha);   // DESIGN 7.5 (ui_bos_player.cpp)

    // Stats for nerds — top-left overlay, always visible while enabled.
    if (stats_for_nerds_) {
        std::vector<std::string> lines = player_.stats_lines();
        if (!lines.empty()) {
            float lh = font_small_->line_height() + 6*s;
            float pad = 14*s, pw = 0;
            for (auto& l : lines) pw = std::max(pw, font_small_->text_width(l));
            float bw = pw + pad*2, bh = lines.size()*lh + pad*2;
            rn.quad({16*s, 16*s, bw, bh}, theme_.bg.with_a(0.66f));
            float ly = 16*s + pad;
            for (auto& l : lines) {
                rn.text(*font_small_, l, 16*s + pad, ly, theme_.text);
                ly += lh;
            }
        }
    }

    // Volume change indicator — pops up on Up/Down, then fades out.
    {
        long vremain = (long)volume_overlay_until_ - (long)now;
        if (vremain > 0) {
            float va = vremain >= (long)kFadeMs ? 1.f : (float)vremain / (float)kFadeMs;
            player_volume(rn, va);
        }
    }

    // Transient status banner (e.g. resolve errors surfaced during playback).
    draw_status_banner(rn, 24*s, s);
    rn.end();
}

void App::open_search() {
    mode_ = Mode::Search;
    kb_mode_ = KbMode::Search;
    kb_title_ = i18n::tr(i18n::Str::SearchYouTube);
    kb_placeholder_.clear();
    query_input_ = query_;      // seed with the current query for quick edits
    kb_caret_ = (int)query_input_.size();
    kb_shift_ = kb_caps_ = false;
    kb_row_ = 1; kb_col_ = 0;   // start on 'q'
    status_msg_.clear();
    sugg_.clear(); sugg_focus_ = false; sugg_col_ = 0;
    sugg_seen_ = query_input_; sugg_asked_ = "\x01"; sugg_seen_ms_ = 0;
}
void App::input_text(const std::string& s) {
    for (char c : s) if (c >= 32 && c <= 126) {   // ASCII-only atlas
        query_input_.insert(query_input_.begin() + kb_caret_, c);
        kb_caret_++;
    }
}
void App::backspace() {
    if (kb_caret_ > 0) {   // one whole UTF-8 character
        int b = kb_caret_ - 1;
        while (b > 0 && ((unsigned char)query_input_[b] & 0xC0) == 0x80) b--;
        query_input_.erase((size_t)b, (size_t)(kb_caret_ - b));
        kb_caret_ = b;
    }
}
void App::test_open_numeric_kb() {
    open_search();
    query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;
    kb_mode_ = KbMode::CastCode;
    kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);
    kb_placeholder_.clear();
}
void App::kb_activate() {
    const auto& kb = kb_layout();
    const Key& k = kb[kb_row_][kb_col_];
    switch (k.type) {
        case KCHAR: {
            std::string t = k.text ? std::string(k.text) : std::string(1, k.ch);
            query_input_.insert((size_t)kb_caret_, t);
            kb_caret_ += (int)t.size();
            if (!kb_caps_) kb_shift_ = false;        // one-shot capital (Thai: one-shot SHIFT layer); caps lock stays
            break;
        }
        case KLANG:   kb_thai_ = !kb_thai_; kb_shift_ = kb_sym_ = kb_caps_ = false; break;
        case KSYM:    kb_sym_ = !kb_sym_; kb_shift_ = kb_caps_ = false; break;
        case KSPACE:  query_input_.insert(query_input_.begin() + kb_caret_, ' '); kb_caret_++; break;
        case KDEL:    backspace(); break;
        case KLEFT:   while (kb_caret_ > 0 && ((unsigned char)query_input_[--kb_caret_] & 0xC0) == 0x80) {} break;
        case KRIGHT:  if (kb_caret_ < (int)query_input_.size()) {
                          kb_caret_++;
                          while (kb_caret_ < (int)query_input_.size() &&
                                 ((unsigned char)query_input_[kb_caret_] & 0xC0) == 0x80) kb_caret_++;
                      }
                      break;
        case KSHIFT:  kb_shift_ = !kb_shift_; kb_caps_ = false; kb_sym_ = false; break;
        case KSUBMIT: submit_search(); break;
    }
}
void App::submit_search() {
    std::string q = query_input_;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (kb_mode_ == KbMode::CastCode) {   // "Add a device": pair with the code, then cast
        kb_mode_ = KbMode::Search;
        mode_ = Mode::Grid;               // picker stays open underneath
        submit_cast_code(q);
        return;
    }
    if (kb_mode_ == KbMode::LinkDevice) {   // "Link a device" (manage): pair, no cast
        kb_mode_ = KbMode::Search;
        mode_ = Mode::Grid;               // manage overlay stays open underneath
        link_device(q);
        return;
    }
    if (q.empty()) { mode_ = Mode::Grid; return; }
    mode_ = Mode::Grid;
    search(q);                 // synchronous (~1s); reuses the startup path
}

void App::draw_spinner(gfx::Renderer& rn, float cx, float cy, float r, gfx::Color c) {
    // 12 dots around a circle; a highlight chases around driven by wall time.
    const int N = 12;
    float phase = (SDL_GetTicks() / 80) % N;   // advance one dot every 80ms
    float dot = r * 0.16f;
    for (int i = 0; i < N; ++i) {
        float ang = (float)i / N * 6.2831853f - 1.5708f;
        float x = cx + std::cos(ang) * r, y = cy + std::sin(ang) * r;
        int d = (i - (int)phase + N) % N;        // 0 = head, brightest
        float a = 0.15f + 0.85f * (float)(N - d) / N;
        rn.quad({x - dot, y - dot, dot*2, dot*2}, c.with_a(a));
    }
}



void App::load_design_fonts() {
    const float u = win_->height() / 768.f;
    std::string dir = platform::exe_dir() + "/data/fonts/";
    if (!std::ifstream(dir + "Inter-Regular.ttf").good()) dir = "data/fonts/";
    std::string thai = dir + "BrickOSThai.ttf";
    if (!std::ifstream(thai).good()) thai.clear();
    const std::string cjk = cjk_font_path();
    struct { int id; const char* w; float px; } spec[] = {
        {DZ_STATUS, "SemiBold", 30}, {DZ_PCT, "SemiBold", 30}, {DZ_ROW, "Bold", 30}, {DZ_CARD, "SemiBold", 22},
        {DZ_META, "Regular", 18}, {DZ_CHIP, "Medium", 24}, {DZ_CHIP_SEL, "SemiBold", 24}, {DZ_PILL, "SemiBold", 18},
        {DZ_HINT, "Regular", 30}, {DZ_HINTKEY, "Bold", 24}, {DZ_LABEL, "Regular", 26}, {DZ_QUERY, "Medium", 36},
        {DZ_KEY, "Regular", 32}, {DZ_KBTN, "SemiBold", 24}, {DZ_KHINT, "Regular", 18}, {DZ_KCHIP, "Regular", 22},
        {DZ_BADGE, "SemiBold", 20}, {DZ_HKEYW, "Bold", 22}, {DZ_SEARCH, "Regular", 24}, {DZ_TOAST, "Medium", 26},
        {DZ_DRAWER, "SemiBold", 30}, {DZ_CARD_FOC, "Bold", 22}, {DZ_SET_LABEL, "Medium", 40},
        {DZ_SET_LABEL_F, "SemiBold", 40}, {DZ_SET_VALUE, "Regular", 30}, {DZ_SET_HEAD, "Bold", 56},
        {DZ_TILE, "SemiBold", 28}, {DZ_SHEET_T, "Bold", 34}, {DZ_SHEET_M, "Regular", 24},
        {DZ_CHIP_VAL, "Bold", 26}, {DZ_CHAPTER, "SemiBold", 22},
        {DZ_PANEL_T, "Bold", 36}, {DZ_CM_NAME, "Bold", 24}, {DZ_CM_META, "Regular", 22},
        {DZ_CM_TEXT, "Regular", 24}, {DZ_CM_REPLY, "Bold", 22}, {DZ_DESC_T, "Bold", 30},
        {DZ_DESC, "Regular", 26}, {DZ_CH_NAME, "Bold", 44}};
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

void App::render_search(gfx::Renderer& rn) {
    // brick-os DESIGN.md 7.6 (v2-KeyboardThai / -English): the status bar; the label 26, the field 80 (text 36, a
    // primary caret), suggestion chips 48; the key panel (#121214) above the hint bar: keys 62, radius 14, gap 8,
    // rows gap 10; the focused key in the focus colour x1.08; Search / OK in primary.
    using S = i18n::Str;
    const int W = win_->width(), H = win_->height();
    const float u = H / 768.f;
    bos_refresh();
    rn.begin(W, H);
    rn.clear(dzc::bg);
    const bool numeric = kb_numeric();
    const gfx::Color surf = gfx::Color::rgb(0x1C1C1F), surf2 = gfx::Color::rgb(0x2C2C30),
                     text2 = gfx::Color::rgb(0xB4B4BA);
    design_status_bar(rn);

    float x0 = 40*u, y = 64*u + 4*u;
    rn.text(dz(DZ_LABEL), kb_title_, x0, y, text2);
    y += dz(DZ_LABEL).line_height() + 8*u;
    const gfx::Font& qf = dz(DZ_QUERY);
    float bw = W - 2*x0, bh = 80*u;
    rn.round_rect({x0, y, bw, bh}, 20*u, surf);
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
    rn.text(qf, shown, tx0, ty, dzc::white);
    if (query_input_.empty() && !kb_placeholder_.empty()) rn.text(qf, kb_placeholder_, tx0, ty, text2);
    float caret_x = tx0 + qf.text_width(query_input_.substr(start, caret - start));
    if ((SDL_GetTicks() / 500) % 2 == 0) rn.quad({caret_x + 2*u, y + (bh - 44*u) / 2, 3*u, 44*u}, bos_.primary);
    y += bh + 16*u;

    if (!numeric && !sugg_.empty()) {   // chips 48, radius 24, padding 18, gap 12, 22
        const gfx::Font& f = dz(DZ_KCHIP);
        float ch = 48*u, pad = 18*u, gap = 12*u;
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
            rn.round_rect({x, y, w, ch}, 24*u, sel ? bos_.focus : surf);
            rn.text(f, sugg_[k], x + pad, y + (ch - f.line_height()) / 2, sel ? bos_.onfocus : dzc::white);
        }
    }

    const auto& kb = kb_layout();
    int nrows = (int)kb.size();
    const float keyh = 62*u, gap = 8*u, rgap = 10*u, bottom = H - 64*u;
    float panel_h = 16*u + nrows * keyh + (nrows - 1) * rgap + 16*u;
    float py = bottom - panel_h;
    rn.quad({0, py, (float)W, panel_h}, gfx::Color::rgb(0x121214));
    rn.quad({0, py, (float)W, std::max(1.f, u)}, surf);
    int units = 0;
    for (const auto& row : kb) { int n = 0; for (const auto& k : row) n += k.span; units = std::max(units, n); }
    float inner = W - 48*u;
    float unit = numeric ? keyh * 1.6f : (inner - (units - 1) * gap) / (float)units;
    float gy = py + 16*u;
    for (int pass = 0; pass < 2; ++pass)   // the focused key last, over its neighbours
    for (int r = 0; r < nrows; ++r) {
        float roww = -gap;
        for (const auto& k : kb[r]) roww += unit * k.span + gap * k.span;
        float x = (W - roww) / 2, ky = gy + r * (keyh + rgap);
        for (int c = 0; c < (int)kb[r].size(); ++c) {
            const Key& k = kb[r][c];
            float kw = unit * k.span + gap * (k.span - 1);
            bool sel = !sugg_focus_ && r == kb_row_ && c == kb_col_;
            if (sel != (pass == 1)) { x += kw + gap; continue; }
            gfx::Rect kr{x, ky, kw, keyh};
            if (sel) kr = {x - kw * 0.04f, ky - keyh * 0.04f, kw * 1.08f, keyh * 1.08f};
            bool caps_key = k.type == KSHIFT && kb_caps_;   // caps lock: the shift key filled
            gfx::Color bg = sel ? bos_.focus : (k.type == KSUBMIT || caps_key) ? bos_.primary : surf2;
            rn.round_rect(kr, 14*u, bg);
            std::string lbl = k.label; const gfx::Font* f = &dz(DZ_KEY);
            gfx::Color tc = sel ? bos_.onfocus : dzc::white;
            switch (k.type) {
                case KSUBMIT:
                    lbl = i18n::tr(kb_mode_ == KbMode::Search ? S::HintSearch : S::HintOk); f = &dz(DZ_KBTN);
                    if (!sel) tc = bos_.onprimary;
                    break;
                case KSPACE: lbl = i18n::tr(S::KbSpace); f = &dz(DZ_KBTN); break;
                case KLANG:  lbl = "\xE0\xB8\x81/A"; f = &dz(DZ_KBTN); break;
                case KSYM:   lbl = kb_sym_ ? "abc" : "#+="; f = &dz(DZ_KBTN); break;
                case KCHAR:
                    if (kb_shift_ && lbl.size() == 1 && lbl[0] >= 'a' && lbl[0] <= 'z') lbl[0] = (char)(lbl[0] - 'a' + 'A');
                    break;
                default: break;
            }
            if (k.type == KSHIFT && kb_shift_ && !sel) tc = kb_caps_ ? bos_.onprimary : bos_.primary;
            if (f->text_width(lbl) > kr.w - 8*u) f = &dz(DZ_KBTN);
            // a lone Thai mark sits where it would on a consonant: pull it to the key centre
            float my = 0;
            if (k.type == KCHAR && lbl.size() == 3 && (unsigned char)lbl[0] == 0xE0 && (unsigned char)lbl[1] == 0xB8) {
                unsigned ch = (unsigned char)lbl[2];
                if (ch == 0xB8 || ch == 0xB9 || ch == 0xBA) my = -0.28f * f->line_height();
                else if (ch == 0xB1 || (ch >= 0xB4 && ch <= 0xB7)) my = 0.30f * f->line_height();
            } else if (k.type == KCHAR && lbl.size() == 3 && (unsigned char)lbl[0] == 0xE0 && (unsigned char)lbl[1] == 0xB9) {
                unsigned ch = (unsigned char)lbl[2];
                if (ch >= 0x87 && ch <= 0x8D) my = 0.30f * f->line_height();
            }
            float lw = f->text_width(lbl);
            if (my != 0 && lw < 4*u) lw = 0;
            rn.text(*f, lbl, kr.x + (kr.w - lw) / 2, kr.y + (kr.h - f->line_height()) / 2 + my, tc);
            x += kw + gap;
        }
    }
    const char* go = i18n::tr(kb_mode_ == KbMode::Search ? S::HintSearch : S::HintOk);
    if (numeric) design_hint_bar(rn, {{"A", i18n::tr(S::HintType)}, {"B", i18n::tr(S::HintDelete)}}, {{"START", go}});
    else design_hint_bar(rn, {{"A", i18n::tr(S::HintType)}, {"B", i18n::tr(S::HintDelete)},
                              {"X", i18n::tr(S::HintSpace)}, {"Y", i18n::tr(S::HintLang)}}, {{"START", go}});
    rn.end();
}

void App::kb_clamp() {
    const auto& kb = kb_layout();
    if (kb_row_ >= (int)kb.size()) kb_row_ = (int)kb.size() - 1;
    if (kb_row_ < 0) kb_row_ = 0;
    if (kb_col_ >= (int)kb[kb_row_].size()) kb_col_ = (int)kb[kb_row_].size() - 1;
}

bool App::kb_pad(int b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_B:
            if (query_input_.empty()) { mode_ = Mode::Grid; kb_mode_ = KbMode::Search; }
            else backspace();
            return true;
        case SDL_CONTROLLER_BUTTON_X:
            query_input_.insert(query_input_.begin() + kb_caret_, ' '); kb_caret_++;
            return true;
        case SDL_CONTROLLER_BUTTON_Y:
            if (!kb_numeric()) { kb_thai_ = !kb_thai_; kb_shift_ = kb_sym_ = kb_caps_ = false; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: {   // one-shot shift; twice quickly = caps lock; again = off
            if (kb_numeric()) return true;
            unsigned now = SDL_GetTicks();
            if (kb_caps_) kb_caps_ = kb_shift_ = false;
            else if (kb_shift_ && now - kb_l1_at_ < 400) kb_caps_ = true;
            else kb_shift_ = !kb_shift_;
            kb_l1_at_ = now; kb_sym_ = false; kb_clamp();
            return true;
        }
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:   // the #+= page / back to abc
            if (!kb_numeric()) { kb_sym_ = !kb_sym_; kb_shift_ = kb_caps_ = false; kb_clamp(); }
            return true;
        case SDL_CONTROLLER_BUTTON_START:
            submit_search();
            return true;
        case SDL_CONTROLLER_BUTTON_BACK:
            mode_ = Mode::Grid; kb_mode_ = KbMode::Search;
            return true;
        default:
            return false;
    }
}

void App::kb_trigger(bool right, int v) {
    int& st = right ? trig_r_ : trig_l_;
    int now = v > 16000 ? 1 : (v < 8000 ? 0 : st);
    if (now == st) return;
    st = now;
    if (!now) { if (kb_trig_dir_ == (right ? 1 : -1)) kb_trig_dir_ = 0; return; }
    kb_trig_dir_ = right ? 1 : -1;
    kb_trig_at_ = kb_trig_last_ = SDL_GetTicks();
    kb_caret_step(kb_trig_dir_);
}
void App::kb_caret_step(int dir) {
    if (dir < 0) { while (kb_caret_ > 0 && ((unsigned char)query_input_[--kb_caret_] & 0xC0) == 0x80) {} }
    else if (kb_caret_ < (int)query_input_.size()) {
        kb_caret_++;
        while (kb_caret_ < (int)query_input_.size() && ((unsigned char)query_input_[kb_caret_] & 0xC0) == 0x80) kb_caret_++;
    }
}
// L2/R2 held: after 400 ms the caret moves every 60 ms
void App::kb_tick() {
    if (!kb_trig_dir_) return;
    if (mode_ != Mode::Search) { kb_trig_dir_ = 0; return; }
    unsigned now = SDL_GetTicks();
    if (now - kb_trig_at_ > 400 && now - kb_trig_last_ >= 60) { kb_caret_step(kb_trig_dir_); kb_trig_last_ = now; }
}

// Autocomplete: 250 ms after the query stops changing, ask YouTube's suggest endpoint
// (one request in flight; a stale answer is dropped).
void App::pump_suggestions() {
    if (sugg_done_) {
        sugg_done_ = false;
        std::lock_guard<std::mutex> lk(sugg_m_);
        if (sugg_res_q_ == query_input_) {
            sugg_ = sugg_res_;
            if (sugg_col_ >= (int)sugg_.size()) sugg_col_ = 0;
            if (sugg_.empty()) sugg_focus_ = false;
        }
    }
    if (mode_ != Mode::Search || kb_mode_ != KbMode::Search) return;
    unsigned now = SDL_GetTicks();
    if (query_input_ != sugg_seen_) { sugg_seen_ = query_input_; sugg_seen_ms_ = now; }
    if (sugg_seen_ == sugg_asked_ || sugg_running_ || now - sugg_seen_ms_ < 250) return;
    std::string q = sugg_seen_;
    sugg_asked_ = q;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) { sugg_.clear(); sugg_focus_ = false; return; }
    if (sugg_thread_.joinable()) sugg_thread_.join();
    sugg_running_ = true;
    std::string hl = i18n::language_hl(i18n::language());
    std::string full = sugg_seen_;
    sugg_thread_ = std::thread([this, q, hl, full]() {
        std::vector<std::string> out;
        try {
            std::string enc;
            static const char* hex = "0123456789ABCDEF";
            for (unsigned char c : q) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
                else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
            }
            HttpClient http;
            auto r = http.get("https://suggestqueries.google.com/complete/search?client=firefox&ds=yt&oe=utf-8&hl="
                              + hl + "&q=" + enc, {"User-Agent: Mozilla/5.0"}, 6);
            if (r.ok()) {
                auto j = nlohmann::json::parse(r.body, nullptr, false);
                if (j.is_array() && j.size() > 1 && j[1].is_array())
                    for (auto& e : j[1]) if (e.is_string() && out.size() < 10) out.push_back(e.get<std::string>());
            }
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(sugg_m_); sugg_res_q_ = full; sugg_res_ = std::move(out); }
        sugg_running_ = false;
        sugg_done_ = true;
    });
}

void App::request_playback() {
    const yt::SearchResult* v = selected();
    if (!v || v->video_id.empty()) return;   // only videos are playable
    shorts_mode_ = v->is_short; shorts_anim_at_ = 0; upnext_open_ = false;
    now_playing_item_ = *v;                      // context for the player options menu
    now_playing_index_ = sel_;                   // remember list position (for autoplay)
    stats_for_nerds_ = false;                    // per-video: reset for each new video
    playback_speed_ = 1.0;                       // per-video: speed back to normal
    audio_override_lang_.clear();                // per-video: back to the global default
    audio_dirty_ = false;
    quality_override_ = -1; quality_dirty_ = false;   // per-video Quality/Aspect overrides end
    if (aspect_override_ >= 0) { aspect_override_ = -1; player_.set_aspect(aspect_mode_); }
    cc_restore_key_.clear();                     // new video: captions start Off
    cc_failed_langs_.clear();                    // new video: failures forgotten
    // (replay_current, used for quality changes, does NOT reset these -> speed persists
    //  across a re-resolve of the same video.)
    // Ask-to-resume: if a position was saved for this video, prompt before playing.
    if (ask_resume_) {
        double rp = it_.resume_pos(v->video_id);
        if (rp > 15) {                           // ignore trivially-early stops
            resume_prompt_open_ = true;
            resume_prompt_sel_ = 0;              // default: Resume
            resume_prompt_pos_ = rp;
            resume_prompt_item_ = *v;
            return;
        }
    }
    start_resolve(v->video_id, v->title, 0);
}

void App::test_end_playback(int idx) {
    if (idx >= 0 && idx < (int)results_.size()) {
        now_playing_item_ = results_[idx]; now_playing_index_ = idx;
    }
    handle_playback_ended();
}

// Launch a specific video (autoplay path) without the resume prompt.
void App::play_item(const yt::SearchResult& v, int index) {
    shorts_mode_ = v.is_short; upnext_open_ = false;
    now_playing_item_ = v;
    now_playing_index_ = index;
    stats_for_nerds_ = false;
    playback_speed_ = 1.0;
    audio_override_lang_.clear();   // per-video track choice ends with its video
    audio_dirty_ = false;
    quality_override_ = -1; quality_dirty_ = false;
    if (aspect_override_ >= 0) { aspect_override_ = -1; player_.set_aspect(aspect_mode_); }
    cc_restore_key_.clear();        // new video: captions start Off
    cc_failed_langs_.clear();       // new video: failures forgotten
    start_resolve(v.video_id, v.title, 0);
}

// Called when playback ends naturally (EOF). If autoplay is on, pick the next video
// (next in the current list, else a related video) and ARM the staged up-next sequence
// instead of playing immediately; otherwise return to the grid.
void App::handle_playback_ended() {
    if (!now_playing_item_.video_id.empty())
        it_.clear_resume_pos(now_playing_item_.video_id);   // finished -> forget resume
    player_.stop();
    has_pending_seek_ = false; pending_seek_ = 0;
    if (shorts_mode_ && mode_ == Mode::Playing) {   // DESIGN 7.8: on -> the next Short, off -> this one again
        int i = autoplay_ ? neighbour_index(+1) : now_playing_index_;
        if (autoplay_ && i < 0) { mode_ = Mode::Grid; shorts_mode_ = false; return; }
        if (autoplay_) { shorts_anim_dir_ = 1; shorts_anim_at_ = SDL_GetTicks();
                         shorts_prev_thumb_ = now_playing_item_.thumbnail_url; sel_ = i;
                         play_item(results_[i], i); }
        else { shorts_anim_at_ = 0; play_item(now_playing_item_, now_playing_index_); }
        return;
    }
    if (!autoplay_) { mode_ = Mode::Grid; return; }
    int idx = find_next_playable(now_playing_index_);
    if (idx >= 0) { arm_upnext(results_[idx], idx); return; }  // next in the current list
    start_related_autoplay(now_playing_item_.video_id);        // end of list -> related
}

// Index of the next playable video row after from_index in results_, or -1.
int App::find_next_playable(int from_index) const {
    int start = from_index >= 0 ? from_index + 1 : (int)results_.size();
    for (int i = start; i < (int)results_.size(); ++i) {
        const yt::SearchResult& v = results_[i];
        if (!v.video_id.empty() && !v.is_channel() && !v.is_playlist() && !v.is_post())
            return i;
    }
    return -1;
}

// Stage the up-next sequence: land on the grid with the next item selected; step_autoplay()
// then waits for any current popup to clear, shows "Up next: <title>", and starts it.
void App::arm_upnext(const yt::SearchResult& v, int index) {
    auto_next_item_ = v;
    auto_next_index_ = index;
    if (index >= 0 && index < (int)results_.size()) { sel_ = index; ensure_visible(); }
    mode_ = Mode::Grid;
    auto_state_ = AutoState::WaitClear;
}

// Runs each frame (pump_async): advance the staged autoplay.
void App::step_autoplay() {
    if (auto_state_ == AutoState::None || mode_ != Mode::Grid) return;
    unsigned now = SDL_GetTicks();
    if (auto_state_ == AutoState::WaitClear) {
        if (!status_msg_.empty() && now < status_until_) return;   // let the last popup finish
        status_msg_ = std::string(i18n::tr(i18n::Str::UpNext)) + ": " + auto_next_item_.title;
        status_until_ = now + 2600;
        auto_show_until_ = now + 2600;
        auto_state_ = AutoState::ShowUpNext;
    } else if (auto_state_ == AutoState::ShowUpNext) {
        if (now < auto_show_until_) return;                        // let "Up next" show
        auto_state_ = AutoState::None;
        play_item(auto_next_item_, auto_next_index_);              // now start the next video
    }
}
void App::cancel_autoplay() {
    if (auto_state_ != AutoState::None) { auto_state_ = AutoState::None; status_msg_.clear(); }
}

// End of list: fetch related videos off-thread, then arm the up-next sequence.
void App::start_related_autoplay(const std::string& video_id) {
    if (video_id.empty()) { mode_ = Mode::Grid; return; }
    if (rel_running_) { mode_ = Mode::Grid; return; }   // a related fetch is already running
    mode_ = Mode::Loading;
    status_msg_ = i18n::tr(i18n::Str::FindingNext); status_until_ = SDL_GetTicks() + 6000;
    rel_autoplay_pending_ = true;
    if (rel_thread_.joinable()) rel_thread_.join();   // finished -> instant
    rel_running_ = true; rel_done_ = false;
    rel_thread_ = std::thread([this, video_id]() {
        std::vector<yt::SearchResult> r;
        try { r = it_.related_videos(video_id); } catch (...) {}
        { std::lock_guard<std::mutex> lk(rel_m_); rel_pending_ = std::move(r); }
        rel_running_ = false; rel_done_ = true;
    });
}
void App::poll_related_autoplay() {
    if (!rel_done_.exchange(false)) return;
    std::vector<yt::SearchResult> r;
    { std::lock_guard<std::mutex> lk(rel_m_); r = std::move(rel_pending_); rel_pending_.clear(); }
    // If the user navigated away (Back left Loading), or autoplay was cancelled, drop it.
    if (!rel_autoplay_pending_ || mode_ != Mode::Loading) { rel_autoplay_pending_ = false; return; }
    rel_autoplay_pending_ = false;
    if (r.empty()) { mode_ = Mode::Grid; status_msg_ = i18n::tr(i18n::Str::NoMoreVideos);
                     status_until_ = SDL_GetTicks() + 2500; return; }
    // Replace the list with the related set so subsequent autoplay chains through it,
    // then arm the same staged up-next sequence.
    results_ = std::move(r);
    arm_upnext(results_[0], 0);
}

// Save (or clear) the current playback position for ask-to-resume. Called when
// leaving playback; near the start or the end we forget it instead.
void App::save_resume_position() {
    if (now_playing_item_.video_id.empty()) return;
    double pos = player_.position(), dur = player_.duration();
    if (pos > 15 && (dur <= 0 || pos < dur - 15))
        it_.set_resume_pos(now_playing_item_.video_id, pos);
    else
        it_.clear_resume_pos(now_playing_item_.video_id);
}

// Re-resolve the currently-playing video (e.g. after a quality change) and resume
// at at_seconds once the new stream loads.
// The mpv "hwdec" value for the current Video Decode setting. On hwdec-capable
// devices Hardware means the matched bundle's hwdec (e.g. v4l2m2m-copy);
// elsewhere the historical auto-copy-safe default stands (software fallback).
std::string App::hwdec_mode_str() const {
    if (hwdec_mode_) return "no";
    return hwdec_hw_value_;   // the matched bundle's value, else auto-copy-safe
}

// Hardware decode detection — self-contained in the app; no data file or
// launcher state is required. Runs on a worker thread (poll_hwdec_detect applies
// it). The toggle appears only where hardware decode is REAL, in this order:
//  1. Built-in backend: a STATEFUL v4l2 decoder on the device (ioctl-verified)
//     + the v4l2m2m decoder in the libavcodec we loaded (ships in the base
//     bundle) -> hwdec=v4l2m2m-copy. Verified: Retroid Pocket 5 (Venus).
//  2. Optional downloadable backends: data/hwdec_manifest.json (dormant
//     PortMaster side; see docs/HWDEC_SETUP_FLOW.md). The app never reads
//     ytc_setup's output — it re-probes hardware + loaded libs with the
//     manifest's names. Absent manifest = nothing extra, never an error.
//  3. Desktop hw device probe: ffmpeg initializes a real hw decode device
//     (VAAPI/VDPAU/VideoToolbox/D3D11/...) for the H264 decoder's compiled
//     hw configs -> auto-copy-safe will use it. No video needed.
//  4. Learned: mpv's hwdec-current showed a hardware method here before
//     (a backend the probe can't see), persisted as "hwdec_seen".
// Everywhere else the historical auto-copy-safe default stands (mpv falls
// back to software harmlessly), with the toggle hidden.
void App::start_hwdec_detect() {
    std::string seen = it_.setting_str("hwdec_seen", "");     // file read: UI thread
    std::string mpath = platform::exe_dir() + "/data/hwdec_manifest.json";
    if (!std::ifstream(mpath).good()) mpath = "data/hwdec_manifest.json";
    hwdec_detect_done_ = false;
    hwdec_thread_ = std::thread([this, seen, mpath]() {
        bool capable = false; std::string value = "auto-copy-safe", via;
        hwdetect::Info hwi = hwdetect::detect();
        if (hwi.has_decoder() && ytn::avcodec_has_decoder("h264_v4l2m2m")) {
            // Copy mode only. Zero-copy (v4l2m2m non-copy -> DRM_PRIME dmabufs
            // -> mpv drmprime interop) was tried 2026-09-04 with LibreELEC's
            // v4l2-drmprime ffmpeg patch: on Venus (RP5, mainline 6.19) the
            // dmabuf-export path faults the video firmware / hangs the decoder
            // in-kernel. Parked with findings in docs/HWDEC_SURVEY.md. mpv
            // falls back to software per stream when hardware can't take a format.
            capable = true; value = "v4l2m2m-copy"; via = "builtin-v4l2";
        } else {
            hwdetect::Bundle hb = hwdetect::pick_bundle(hwi, mpath);
            if (hb.valid() && !hb.hwdec.empty() && !hb.files.empty() &&
                ytn::avcodec_has_decoder(hb.check)) {
                capable = true; value = hb.hwdec; via = "bundle:" + hb.key;
            }
        }
        if (!capable) {
            std::string dev = ytn::probe_hw_device();
            if (!dev.empty()) { capable = true; via = "hwdevice:" + dev; }
        }
        if (!capable && !seen.empty()) { capable = true; via = "learned:" + seen; }
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[hwdec] decoder=%s stateless=%s capable=%d hwdec=%s via=%s\n",
                         hwi.decoder.c_str(), hwi.stateless.c_str(), (int)capable,
                         value.c_str(), via.empty() ? "none" : via.c_str());
        hwdec_det_capable_ = capable; hwdec_det_value_ = value; hwdec_det_via_ = via;
        hwdec_detect_done_ = true;      // release: fields above are visible after this
    });
}
void App::poll_hwdec_detect() {
    if (!hwdec_detect_done_.exchange(false)) return;
    if (hwdec_thread_.joinable()) hwdec_thread_.join();
    hwdec_capable_ = hwdec_det_capable_;
    hwdec_hw_value_ = hwdec_det_value_;
    player_.set_hwdec(hwdec_mode_str());   // applies to the NEXT play()
    // Settings > Video already open? Show the row without a reopen.
    if (menu_open_ && menu_kind_ == MenuKind::SettingsVideo) {
        int keep = menu_sel_; open_settings_video(); menu_sel_ = keep;
    }
}

// The AudioPrefs.lang value for the next resolve: the per-video override wins,
// else the global setting ("app" -> the UI language; "orig" -> "" = original).
std::string App::effective_audio_lang() const {
    if (!audio_override_lang_.empty()) return audio_override_lang_;
    if (audio_lang_pref_ == "app")  return i18n::language_hl(lang_);
    if (audio_lang_pref_ == "orig") return "";
    return audio_lang_pref_;
}

void App::replay_current(double at_seconds) {
    if (now_playing_item_.video_id.empty()) return;
    // Same video, new stream (quality/audio-track change): carry the caption
    // selection over — start_captions resets it, poll_captions restores it.
    cc_restore_key_ = cc_current_key();
    // A restricted (paced) stream's FRESH url only serves ~the first 20MiB, so a deep
    // resume would 403 on load. Clamp into the fresh window (approx via bitrate).
    if (playing_paced_ && playing_vbitrate_ > 0) {
        double safe = (15.0 * 1024 * 1024 * 8) / (double)playing_vbitrate_;
        if (at_seconds > safe) {
            at_seconds = safe;
            status_msg_ = i18n::tr(i18n::Str::PacedResuming);
            status_until_ = SDL_GetTicks() + 5000;
        }
    }
    start_resolve(now_playing_item_.video_id, now_playing_item_.title, at_seconds);
}

// One-byte range GET against a resolved stream URL; returns the HTTP status
// (-1 on a transport error). Cheap: runs on the resolve thread behind Loading.
static long probe_stream_status(const std::string& url, const std::string& ua) {
    HttpClient http;
    std::vector<std::string> h = {"Range: bytes=0-0"};
    if (!ua.empty()) h.push_back("User-Agent: " + ua);
    return http.get(url, h, 10).status;
}

void App::start_resolve(const std::string& video_id, const std::string& title, double start_pos) {
    if (!Player::available()) {
        status_msg_ = i18n::tr(i18n::Str::NoMpv);
        return;
    }
    if (resolve_running_) return;               // one resolve at a time
    status_msg_.clear();
    loading_title_ = title;
    resume_pos_ = start_pos;                     // applied in poll_resolve after play()
    mode_ = Mode::Loading;

    std::string fallback_title = title;
    yt::VideoPrefs prefs = play_prefs_;
    prefs.max_height = effective_max_height();   // per-video Quality override, if any
    yt::AudioPrefs aprefs; aprefs.lang = effective_audio_lang();
    resolve_running_ = true;
    resolve_done_ = false;
    // "Offline mode" is scoped to the Downloads view: only there do we play the local
    // file. From any other page a downloaded video streams normally (full options).
    bool play_local = it_.is_downloaded(video_id) && view_label_ == "Downloads";
    if (resolve_thread_.joinable()) resolve_thread_.join();
    resolve_thread_ = std::thread([this, video_id, fallback_title, prefs, aprefs, play_local]() {
        bool dbg = getenv("YTC_DEBUG");
        ResolveResult r;
        r.title = fallback_title;
        // Offline download: skip the network resolve and play the local file directly.
        if (play_local) {
            r.ok = true; r.video_url = it_.download_path(video_id);
            r.audio_url.clear(); r.user_agent.clear();
            r.description = it_.download_description(video_id);   // from the .info sidecar
            { std::lock_guard<std::mutex> lk(resolve_m_); resolve_result_ = std::move(r); }
            resolve_running_ = false; resolve_done_ = true;
            return;
        }
        try {
        if (dbg) std::fprintf(stderr, "[play] resolving %s (cap=%d)...\n",
                              video_id.c_str(), prefs.max_height);
        yt::VideoInfo info = it_.resolve(video_id);   // background thread owns it_ here
        // A 403 on byte 0 of the chosen stream means the URL itself was rejected
        // (paced delivery only refuses ranges BEYOND the first window) — typically a
        // stale visitorData session. Left alone, mpv would sit "buffering" for 90s.
        // Refresh the token and resolve once more.
        if (info.ok() && !info.is_live) {
            if (const yt::Format* vf = info.best_video(prefs)) {
                long code = probe_stream_status(vf->url, info.user_agent);
                if (dbg) std::fprintf(stderr, "[play] stream probe -> HTTP %ld\n", code);
                if (code == 403) {
                    std::fprintf(stderr, "[play] stream rejected (403); refreshing session and re-resolving\n");
                    it_.refresh_visitor_data();
                    info = it_.resolve(video_id);
                }
            }
        }
        r.title = info.title.empty() ? fallback_title : info.title;
        r.description = info.description;
        r.author = info.author;
        if (!info.ok()) {
            r.ok = false;
            // Surface a human-readable reason (YouTube's own text is best).
            std::string reason = info.status_reason;
            if (info.is_upcoming)
                r.status = reason.empty() ? "Premiere hasn't started yet" : reason;
            else if (info.status == "LIVE_STREAM_OFFLINE")
                r.status = reason.empty() ? "Live stream is offline" : reason;
            else
                r.status = reason.empty() ? ("Can't play (" + info.status + ")") : reason;
            if (dbg) std::fprintf(stderr, "[play] not playable: %s / %s\n",
                                  info.status.c_str(), r.status.c_str());
        } else if (info.is_live && info.hls_manifest_url) {
            // Live: play the HLS manifest (mpv handles it live; audio is muxed).
            r.ok = true;
            r.video_url = *info.hls_manifest_url;
            r.audio_url = "";
            r.user_agent = info.user_agent;
            if (dbg) std::fprintf(stderr, "[play] resolved LIVE via HLS manifest\n");
        } else {
            const yt::Format* vf = info.best_video(prefs);
            const yt::Format* af = info.best_audio(aprefs);
            if (!vf) { r.ok = false; r.status = "No playable video format"; }
            else {
                r.ok = true;
                r.video_url = vf->url;
                r.audio_url = af ? af->url : "";
                r.user_agent = info.user_agent;
                r.video_bitrate = vf->bitrate;
                r.audio_lang = af ? af->track_lang : "";
                r.audio_tracks = info.audio_tracks();   // for the player track picker
                // The stable paced-delivery signal is YouTube's made-for-kids marker
                // in playabilityStatus. Do not infer a channel-wide restriction from
                // one deep-range 403: CDN errors and URL-specific range policies can
                // produce the same response and were misclassifying normal videos.
                r.paced = info.made_for_kids;
                if (dbg) std::fprintf(stderr, "[play] resolved: video itag %d audio itag %d paced=%d\n",
                                      vf->itag, af ? af->itag : -1, (int)r.paced);
            }
        }
        } catch (const std::exception& e) {   // never let the resolve thread terminate the app
            r.ok = false; r.status = "Error resolving (see log)";
            std::fprintf(stderr, "[play] resolve thread exception: %s\n", e.what());
        }
        { std::lock_guard<std::mutex> lk(resolve_m_); resolve_result_ = std::move(r); }
        resolve_running_ = false;
        resolve_done_ = true;
    });
}

void App::poll_resolve() {
    if (!resolve_done_.exchange(false)) return;
    if (resolve_thread_.joinable()) resolve_thread_.join();
    ResolveResult r;
    { std::lock_guard<std::mutex> lk(resolve_m_); r = std::move(resolve_result_); }

    if (mode_ != Mode::Loading) return;         // user cancelled (Back) — discard
    if (!r.ok) {
        mode_ = Mode::Grid;
        status_msg_ = r.status;
        status_until_ = SDL_GetTicks() + 6000;  // show the banner for 6s
        return;
    }
    double start_at = resume_pos_; resume_pos_ = 0;   // consume the resume point
    if (!player_.play(r.video_url, r.audio_url, r.user_agent, start_at, r.paced)) {
        mode_ = Mode::Grid; status_msg_ = i18n::tr(i18n::Str::PlayerFailed); return;
    }
    player_.set_volume(volume_);                 // apply the app-local volume level
    playing_audio_url_ = r.audio_url; playing_video_url_ = r.video_url; playing_ua_ = r.user_agent;
    playing_author_ = r.author;
    player_.set_speed(playback_speed_);          // apply speed (persists across re-resolve)
    start_sponsorblock(now_playing_item_.video_id);   // fetch skip segments off-thread
    start_captions(now_playing_item_.video_id);        // fetch caption track list off-thread
    now_playing_title_ = r.title;
    now_playing_desc_ = r.description;          // free: came with the resolve
    now_playing_chapters_ = chapter_times(r.description);
    playing_audio_tracks_ = std::move(r.audio_tracks);
    playing_audio_lang_ = r.audio_lang;
    hwdec_probed_this_video_ = false;           // re-learn hwdec-current per video
    playing_paced_ = r.paced;
    playing_vbitrate_ = r.video_bitrate;
    played_max_ = start_at;                     // seek window anchors here
    stall_pos_ = start_at; stall_since_ = SDL_GetTicks(); stall_moved_ = false;
    if (playing_paced_) {
        status_msg_ = i18n::tr(i18n::Str::KidsPartial);
        status_until_ = SDL_GetTicks() + 6000;
        // Remember the channel verdict (restriction is channel-wide in practice);
        // the "Hide Restricted" filter picks it up like any checked verdict.
        rcheck_.put(now_playing_item_.channel_id, true);
    }
    mode_ = Mode::Playing;
    controls_until_ = SDL_GetTicks() + 2600;   // show info briefly, then fade as it plays
    // Record in local watch history (most-recent first, deduped). Title prefers the
    // resolved title; fall back to the tile's title if resolve returned nothing.
    if (!now_playing_item_.video_id.empty())
        it_.add_history(now_playing_item_.video_id,
                        r.title.empty() ? now_playing_item_.title : r.title,
                        now_playing_item_.channel_id, now_playing_item_.author);
}

// Brick OS 7.5c: the request "ytc --bg" plays (bgaudio.cpp). A live clip hands over its HLS manifest and a
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

void App::ensure_visible() {
    const int H = win_->height();
    GridMetrics m = grid_metrics();       // same metrics render uses -> no clipping
    int row = sel_ / cols_;
    float y = m.top + row * m.rowstep - scroll_;   // selected row's on-screen top
    // Scroll up so the row clears the top (row 0 snaps to 0 -> header shows);
    // scroll down so the whole row + its selection ring clears the pinned footer,
    // with a pad gap so the card bottom isn't tucked under the footer.
    float bottom_limit = H - m.fh - m.pad;
    if (y < m.top) scroll_ -= (m.top - y);
    if (y + m.cardh > bottom_limit) scroll_ += (y + m.cardh) - bottom_limit;
    if (scroll_ < 0) scroll_ = 0;
}

} // namespace ui
