// vibe_bgdecode.h — BACKGROUND DECODING: when nobody is listening, the radio decodes for the owner (V12, 2026-10-22).
//
// ★★★ THE OWNER'S SPEC (Stuart, 2026-10-22, for NickB's NAVTEX the night Storm Austen came in):
//     "Background Decoding: When the last user leaves the server after 5 minutes the SDR will move to your selected RF
//      Centre. Set an RF centre here and choose 1 or multiple VFO's and decoders for background decoding."
//     Nick's case, which must configure and must work: centre 500 kHz; VFO 1 489.5 kHz USB NAVTEX; VFO 2 517.5 kHz USB
//     NAVTEX; all day.
//
// ── The pieces ────────────────────────────────────────────────────────────────────────────────────────────────
//  • Config — PROFILES, each an RF centre, its VFOs (frequency → demodulator → decoder, in the order a listener chooses
//    them) and a UTC time window (00:00–00:00 = all day). Stored as ONE JSON string in the radio's config entry
//    (RadioConfig::bgDecode), because vibeserver_config's reader is a flat key scanner and a nested array of objects
//    would let one radio's "freq" be read out of a profile — the same reason antennaSwitch is a string.
//  • validate() — what the setup page is refused at SAVE: overlapping profiles (named), a VFO the radio cannot see
//    around its centre at its sample rate (saying why), a decoder that makes no sense on its demodulator.
//  • GalleryTap — a DecoderTap (vibe_decoder_host.h) that writes NAVTEX messages and RTTY runs into the gallery
//    (vibe_gallery.h). The SAME tap serves a listener's own decoder, labelled "live".
//  • BgRunner — the engine: one channel per VFO cut from ONE forward FFT of the capture (vibedsp::Channelizer, the
//    per-listener architecture), each with its own demodulator (RxPipeline) and decoder host — "one internal channel
//    per VFO, fed like a listener's". The shim owns WHEN it runs; this owns HOW.
//
// ★★★ "NO LISTENERS" IS ONE TRIGGER, NOT THE DESIGN. A later build adds radios that are hidden admin-only or DEDICATED
//     to background decoding full-time (docs/v12/RADIO-ROLES.md). So nothing here knows about listeners: the runner is
//     started and stopped by its owner (the shim's bgTick), and a dedicated radio will simply start it and never stop.
//
// ★★ PRIORITY: DECODERS ARE LAST (vibe_thread.h). The DSP thread only COPIES the capture into a bounded queue (half a
//    second; the oldest is dropped, never waited for); the channels run on a thread of their own at decoder priority,
//    and each decoder host on its own decode thread as for a listener. A Pi 2 that cannot keep up loses samples here,
//    never audio anywhere else — and while this runs there is, by construction, nobody listening.
#pragma once
#include "vibe_bgdecode_config.h"   // ★ the profiles, validate(), attachMsg()
#include "vibe_decoder_host.h"
#include "vibe_gallery.h"
#include "vibe_png.h"
#include "vibe_callsign_country.h"
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibe {
namespace bg {

// ── The gallery's copy of a text decoder ─────────────────────────────────────────────────────────────────────────
/** ★★ NAVTEX: one item per message, written as it completes. RTTY: buffered, appended every minute or 8 KB, and at once
 *  on finish(); a new file each UTC day and past Store::kRunMaxBytes. `dial` is read on every chunk — a LISTENER may
 *  retune under a running decoder, and what follows is another transmission (as the client's NAVTEX box clears). */
class GalleryTap : public DecoderTap {
public:
    GalleryTap(gallery::Store& store, gallery::Meta meta, std::function<double()> dial = nullptr)
        : store_(store), meta_(std::move(meta)), dial_(std::move(dial)) {
        navtex_ = meta_.kind == "navtex";
        lastFlush_ = gallery::nowUtc();
        if (navtex_) asm_.onMessage = [this](const gallery::NavtexMsg& m) { held_.push_back(m); heldAt_ = gallery::nowUtc(); };
    }
    ~GalleryTap() override { finish(true); }
    void text(const std::string& t) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        followDial();
        const int64_t now = gallery::nowUtc();
        if (navtex_) { saveHeld(false); asm_.push(t, gallery::nowUtcMs()); return; }
        buf_ += t;
        if (buf_.size() >= 8192 || now - lastFlush_ >= 60) flushRun(now);
    }
    void frame(const uint8_t* d, size_t n) override {
        if (!navtex_ || n < 1) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        if (d[0] == 0x03 && n >= 2) {
            // ★ Lock transitions, into the log (2026-10-22: the next real run must SHOW what split a message, if anything).
            const bool lk = d[1] >= 3;
            if (log_ && lk != lockedNow_) log_(std::string("background decoding: ") + tag_ + (lk ? " NAVTEX lock" : " NAVTEX lock lost"));
            lockedNow_ = lk;
            asm_.lockState(d[1], gallery::nowUtcMs());
        }
        else if (d[0] == 0x07 && n >= 13) {
            // ★ The FEC's own count, sent BEHIND the text carrying its NNNN — so it belongs to the message just held.
            auto u32 = [&](size_t o) { return (unsigned long)d[o] << 24 | (unsigned long)d[o + 1] << 16 | (unsigned long)d[o + 2] << 8 | d[o + 3]; };
            if (!held_.empty() && !held_.back().endLost && !held_.back().startLost && !held_.back().haveFec) {
                held_.back().haveFec = true;
                held_.back().fecClean = u32(1); held_.back().fecRepaired = u32(5); held_.back().fecLost = u32(9);
            }
            saveHeld(true);
        }
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t now = gallery::nowUtc();
        if (navtex_) { asm_.tick(gallery::nowUtcMs()); if (!held_.empty() && now - heldAt_ >= 2) saveHeld(true); return; }
        if (!buf_.empty() && now - lastFlush_ >= 60) flushRun(now);
    }
    void finish(bool interrupted) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        if (navtex_) { asm_.flush(); saveHeld(true, interrupted); return; }
        flushRun(gallery::nowUtc());
        if (!run_.empty()) store_.close(run_);
    }
    size_t saved() const { std::lock_guard<std::mutex> lk(m_); return saved_; }
    std::string lastName() const { std::lock_guard<std::mutex> lk(m_); return lastName_; }
    /** Background decoding's taps say what they see (lock transitions) under the VFO's name. */
    void setLog(std::function<void(const std::string&)> f, std::string tag) { std::lock_guard<std::mutex> lk(m_); log_ = std::move(f); tag_ = std::move(tag); }

private:
    void followDial() {
        if (!dial_) return;
        const double d = dial_();
        if (std::fabs(d - meta_.freqHz) < 100.0) return;
        if (navtex_) { saveHeld(true); asm_.clearLive(); }
        else { flushRun(gallery::nowUtc()); if (!run_.empty()) store_.close(run_); run_.clear(); }
        meta_.freqHz = d;
    }
    std::string where() const {
        std::string w = gallery::freqText(meta_.freqHz) + " " + upper(meta_.mode) + " \xc2\xb7 "
                      + (meta_.origin == "background" ? "background decoding" : "a listener's decoder");
        if (!meta_.profile.empty()) w += " (" + meta_.profile + ")";
        return w;
    }
    static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
    /** Write the finished NAVTEX messages. `partialTail`: the decoder was cut off — the last one is marked partial. */
    void saveHeld(bool all, bool partialTail = false) {
        if (held_.empty() || (!all && gallery::nowUtc() - heldAt_ < 2)) return;
        for (size_t i = 0; i < held_.size(); i++) {
            auto m = held_[i];
            // ★ V12 beta 4: name the station from its letter, the dial and this server's position — the rule the listener's
            //   box uses (navtexStations.generated.ts), so the gallery and the box say the same thing. ★ Also a station the
            //   table lacks, named by its own message ("E · Mondolfo Radio, Italy 🇮🇹?"); never the "or" pair.
            if (m.id.size() == 4) {
                const char b1 = m.id[0];
                if (navtex::nameable(b1, meta_.freqHz, meta_.lat, meta_.lon, m.text)) m.station = navtex::label(b1, meta_.freqHz, meta_.lat, meta_.lon, m.text);
            }
            gallery::Meta mm = meta_;
            mm.utc = m.startedAt;
            mm.title = gallery::NavtexAsm::title(m);
            mm.partial = m.startLost || m.endLost || (partialTail && i + 1 == held_.size());
            const std::string n = store_.add(mm, "txt", gallery::NavtexAsm::fileText(m, where()));
            if (!n.empty()) { ++saved_; lastName_ = n; }
        }
        held_.clear();
    }
    void flushRun(int64_t now) {
        if (buf_.empty()) { lastFlush_ = now; return; }
        // ★ A new file at UTC midnight, and past the run cap — none grows for ever.
        const int64_t day = now / 86400;
        if (!run_.empty() && (day != runDay_ || store_.sizeOf(run_) + buf_.size() > gallery::Store::kRunMaxBytes)) {
            store_.close(run_); run_.clear();
        }
        if (run_.empty() || !store_.append(run_, buf_)) {
            if (!run_.empty()) store_.close(run_);
            gallery::Meta mm = meta_;
            mm.utc = now;
            // ★ A CW run is written exactly as an RTTY run is (V12 2026-10-25) — only its name differs.
            const std::string what = meta_.kind == "cw" ? "CW" : meta_.kind == "psk" ? "PSK" : "RTTY";
            mm.title = what + " \xc2\xb7 " + gallery::freqText(meta_.freqHz) + " " + upper(meta_.mode);
            const std::string head = what + " \xc2\xb7 " + where() + "\nStarted " + gallery::utcStamp(now, "%Y-%m-%d %H:%M UTC") + "\n\n";
            run_ = store_.create(mm, "txt", head + buf_);
            runDay_ = day;
            if (!run_.empty()) { ++saved_; lastName_ = run_; }
        }
        buf_.clear();
        lastFlush_ = now;
    }

    gallery::Store& store_;
    gallery::Meta meta_;
    std::function<double()> dial_;
    mutable std::mutex m_;
    bool navtex_ = false, done_ = false;
    gallery::NavtexAsm asm_;
    std::function<void(const std::string&)> log_;
    std::string tag_;
    bool lockedNow_ = false;
    std::vector<gallery::NavtexMsg> held_;
    int64_t heldAt_ = 0, lastFlush_ = 0, runDay_ = 0;
    std::string buf_, run_, lastName_;
    size_t saved_ = 0;
};

/** The kind a decoder extension is filed under — "" = not kept (the time signals: a clock is not a keepsake). */
inline std::string galleryKindFor(const std::string& ext, const std::string& msg) {
    if (ext == "navtex") return "navtex";
    if (ext == "fsk") return msg.find("CCIR476") != std::string::npos ? "navtex" : "rtty";
    if (ext == "wefax" || ext == "sstv") return ext;
    if (ext == "cw") return "cw";          // ★ a listener's CW decoder: its text, as an RTTY run (a skimmer keeps a summary)
    if (ext == "psk") return "psk";        // ★ …and a listener's PSK decoder (V12 2026-10-26)
    return "";
}

// ── The gallery's copy of a picture decoder ──────────────────────────────────────────────────────────────────────
/** ★★ WEFAX and SSTV: one PNG per picture, written when it COMPLETES (WEFAX's stop tone, SSTV's end of frame). A picture
 *  cut off — a listener arriving, the profile changing, a new start tone before the stop — is saved ONCE, marked
 *  PARTIAL, as far as it got (Stuart: "a chart/picture cut off … is saved once, marked PARTIAL").
 *  ★ Reads the host's own frames (the ones the client draws): WEFAX 0x02 start · 0x01 line(u32 ln, u32 w, px) · 0x03
 *    stop; SSTV 0x07 start(u32 w, u32 h) · 0x01 line(u32 y, u32 w, rgb) · 0x02 mode name · 0x05 complete. A redraw
 *    re-sends lines by index, so they simply replace.
 *  ★ Bounded: a chart is at most kMaxLines lines (~5 MB grey); past that it is saved and a new one begun. Too few lines
 *    to be a picture (noise that tripped a start tone) are not kept. */
class GalleryImageTap : public DecoderTap {
public:
    static constexpr uint32_t kMaxLines = 3000, kMaxWidth = 4096;
    GalleryImageTap(gallery::Store& store, gallery::Meta meta) : store_(store), meta_(std::move(meta)) { sstv_ = meta_.kind == "sstv"; }
    ~GalleryImageTap() override { finish(true); }
    void frame(const uint8_t* d, size_t n) override {
        if (n < 1) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        auto u32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
        const uint8_t op = d[0];
        if (!sstv_) {
            if (op == 0x02 && n == 1) { save(true); begin(0, 0); return; }
            if (op == 0x03 && n == 1) { save(false); return; }
            if (op == 0x01 && n >= 9) {
                const uint32_t ln = u32(1), w = u32(5);
                if (w == 0 || w > kMaxWidth || n < 9 + (size_t)w) return;
                if (!open_) begin(0, 0);
                if (w_ == 0) w_ = w;
                if (w != w_) return;
                if (ln >= kMaxLines) { save(true); begin(0, 0); w_ = w; }
                const uint32_t row = ln >= kMaxLines ? ln % kMaxLines : ln;
                put(row, d + 9, w);
            }
            return;
        }
        if (op == 0x07 && n >= 9) { save(true); begin(u32(1), u32(5)); return; }
        if (op == 0x02 && n >= 3) { const size_t l = ((size_t)d[1] << 8) | d[2]; if (n >= 3 + l) mode_ = gallery::printable(std::string((const char*)d + 3, l), 40); return; }
        if (op == 0x05) { save(false); return; }
        if (op == 0x01 && n >= 9 && open_) {
            const uint32_t y = u32(1), w = u32(5);
            if (w == 0 || w != w_ || y >= h_ || n < 9 + (size_t)w * 3) return;
            put(y, d + 9, w * 3);
        }
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        save(true);
    }
    size_t saved() const { std::lock_guard<std::mutex> lk(m_); return saved_; }

private:
    void begin(uint32_t w, uint32_t h) {
        open_ = true; px_.clear(); lines_ = 0; maxRow_ = 0; started_ = gallery::nowUtc();
        w_ = w > kMaxWidth ? 0 : w; h_ = h > 2000 ? 0 : h;
        if (sstv_ && w_ && h_) px_.assign((size_t)w_ * h_ * 3, 0);
    }
    void put(uint32_t row, const uint8_t* p, size_t bytes) {
        const size_t off = (size_t)row * bytes;
        if (px_.size() < off + bytes) px_.resize(off + bytes, 255);
        std::memcpy(px_.data() + off, p, bytes);
        lines_++;
        if (row + 1 > maxRow_) maxRow_ = row + 1;
    }
    /** Write what is held, once. `partial`: it did not reach its end. */
    void save(bool partial) {
        if (!open_) return;
        open_ = false;
        const uint32_t minLines = sstv_ ? 16 : 50;
        if (w_ == 0 || lines_ < minLines || maxRow_ == 0) { px_.clear(); return; }
        const int ch = sstv_ ? 3 : 1;
        const uint32_t h = sstv_ ? (partial ? maxRow_ : h_) : maxRow_;
        px_.resize((size_t)w_ * h * ch, 255);
        const std::string png = png::encode((int)w_, (int)h, ch, px_.data());
        px_.clear(); px_.shrink_to_fit();
        if (png.empty()) return;
        gallery::Meta m = meta_;
        m.utc = started_;
        m.partial = partial;
        m.title = (sstv_ ? "SSTV" + (mode_.empty() ? std::string() : " \xc2\xb7 " + mode_) : std::string("WEFAX chart"))
                + " \xc2\xb7 " + std::to_string(w_) + "\xc3\x97" + std::to_string(h);
        if (!store_.add(m, "png", png).empty()) ++saved_;
    }
    gallery::Store& store_;
    gallery::Meta meta_;
    mutable std::mutex m_;
    bool sstv_ = false, open_ = false, done_ = false;
    uint32_t w_ = 0, h_ = 0, lines_ = 0, maxRow_ = 0;
    std::vector<uint8_t> px_;
    std::string mode_;
    int64_t started_ = 0;
    size_t saved_ = 0;
};

// ── FT8 / FT4: a SUMMARY, not the spots ─────────────────────────────────────────────────────────────────────────
/** Maidenhead (4 or 6 characters) → the square's centre. False for anything that is not a grid (FT8's "RR73" is not). */
inline bool gridToLatLon(const std::string& g, double& lat, double& lon) {
    if (g.size() < 4) return false;
    const char A = (char)std::toupper((unsigned char)g[0]), B = (char)std::toupper((unsigned char)g[1]);
    if (A < 'A' || A > 'R' || B < 'A' || B > 'R' || !std::isdigit((unsigned char)g[2]) || !std::isdigit((unsigned char)g[3])) return false;
    if (g == "RR73") return false;
    lon = (A - 'A') * 20.0 - 180.0 + (g[2] - '0') * 2.0;
    lat = (B - 'A') * 10.0 - 90.0 + (g[3] - '0') * 1.0;
    if (g.size() >= 6 && std::isalpha((unsigned char)g[4]) && std::isalpha((unsigned char)g[5])) {
        lon += (std::tolower((unsigned char)g[4]) - 'a') * (2.0 / 24.0) + 1.0 / 24.0;
        lat += (std::tolower((unsigned char)g[5]) - 'a') * (1.0 / 24.0) + 0.5 / 24.0;
    } else { lon += 1.0; lat += 0.5; }
    return true;
}
/** ★★ beta 4 (the Lenovo, 1840 kHz: "Furthest: DE (country unknown, AA00) — 15880 km"): what the SUMMARY counts.
 *  A CALLSIGN: its longest '/'-part (the call itself — "G4ABC" of "EA8/G4ABC/P") is 3–10 letters and digits with at
 *  least one digit and one letter, ending in a letter; never an FT8 word (DE CQ QRZ TO TU TNX RR73 RRR 73) and never a
 *  grid. "DE", "73" and "RR73" all fail. */
inline bool ft8IsCallsign(const std::string& in) {
    std::string c;
    for (char ch : in) c += (char)std::toupper((unsigned char)ch);
    static const char* const words[] = { "DE", "CQ", "QRZ", "TO", "TU", "TNX", "RR73", "RRR", "73", "DX" };
    for (const char* w : words) if (c == w) return false;
    std::string best, part;
    auto take = [&] { if (part.size() > best.size()) best = part; part.clear(); };
    for (char ch : c) { if (ch == '/') take(); else part += ch; }
    take();
    if (best.size() < 3 || best.size() > 10) return false;
    bool digit = false, letter = false;
    for (char ch : best) {
        if (ch >= '0' && ch <= '9') digit = true;
        else if (ch >= 'A' && ch <= 'Z') letter = true;
        else return false;
    }
    if (!digit || !letter || !(best.back() >= 'A' && best.back() <= 'Z')) return false;
    for (const char* w : words) if (best == w) return false;
    return true;
}
/** A GRID the summary trusts: a Maidenhead square AA–RR + 00–99, never RR73 (a sign-off) nor AA00 (the corner of the
 *  map — a placeholder, never a real station). */
inline bool ft8IsGrid(const std::string& g) {
    if (g.size() != 4) return false;
    if (!(g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9')) return false;
    return g != "RR73" && g != "AA00";
}
/** ★ A decoded MESSAGE → the transmitting station and ITS grid: "CQ [modifier] CALL GRID" → CALL's; "CALL1 CALL2 GRID"
 *  → CALL2's (CALL1 is who it is sent to). A grid is never paired with anything but a callsign. False = no station. */
inline bool ft8StationFromText(const std::string& text, std::string& call, std::string& grid) {
    call.clear(); grid.clear();
    std::vector<std::string> t; std::string w;
    for (char ch : text) { if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { if (!w.empty()) t.push_back(w); w.clear(); } else w += (char)std::toupper((unsigned char)ch); }
    if (!w.empty()) t.push_back(w);
    size_t i = 0;
    if (!t.empty() && (t[0] == "CQ" || t[0] == "QRZ")) {
        i = 1;
        if (i < t.size() && !ft8IsCallsign(t[i])) ++i;            // ★ the modifier: DX, NA, DE, 123 …
    } else if (t.size() >= 2 && ft8IsCallsign(t[0])) i = 1;       // ★ CALL1 CALL2 …: the sender is the second
    else return false;
    if (i >= t.size() || !ft8IsCallsign(t[i])) return false;
    call = t[i];
    if (i + 1 < t.size() && ft8IsGrid(t[i + 1])) grid = t[i + 1];
    return true;
}
inline double greatCircleKm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}

/** ★ One stretch of FT8/FT4 spots, counted: what both summary cards report (the hour's and the last 10 minutes').
 *  ★ Bounded: at most kMaxCalls calls (FT8 on a busy band at night is a few thousand). */
struct Ft8Tally {
    static constexpr size_t kMaxCalls = 20000;
    unsigned long spots = 0, ft8 = 0, ft4 = 0;
    /** ★ V12 2026-10-25: the same tally serves the CW summary — no FT8/FT4 split, no grids, and two lines of its own
     *  (the most-heard call, the busiest 1 kHz of the band). */
    bool cw = false;
    bool speed = true;      // ★ …with an average speed line (CW's WPM); PSK's speed is its mode, so none
    std::map<std::string, unsigned long> callSpots;
    std::map<long, unsigned long> khzSpots;
    double wpmSum = 0;
    std::map<std::string, std::string> calls;
    std::map<std::string, unsigned long> countrySpots, countryCalls;
    double farKm = 0;
    std::string farCall, farGrid, farCountry;
    /** `km`: from this server to the call's own grid, NaN when either is unknown. */
    void add(bool isFt4, const std::string& call, const std::string& grid, const std::string& ctry, double km,
             double rfHz = NAN, double wpm = 0) {
        ++spots; (isFt4 ? ft4 : ft8)++;
        if (cw) {
            if (callSpots.size() < kMaxCalls || callSpots.count(call)) callSpots[call]++;
            if (std::isfinite(rfHz) && rfHz > 0) khzSpots[(long)std::floor(rfHz / 1000.0)]++;
            wpmSum += wpm;
        }
        if (calls.size() < kMaxCalls && !calls.count(call)) {
            calls[call] = ctry;
            if (!ctry.empty()) countryCalls[ctry]++;
        }
        if (!ctry.empty()) countrySpots[ctry]++;
        // ★ The furthest is a station with a valid call, ITS valid grid, and a country we can name — never "country unknown".
        if (!ctry.empty() && !grid.empty() && std::isfinite(km) && km > farKm) { farKm = km; farCall = call; farGrid = grid; farCountry = ctry; }
    }
    std::string mostHeard(unsigned long* n = nullptr) const {
        std::string best; unsigned long bn = 0;
        for (const auto& kv : callSpots) if (kv.second > bn) { bn = kv.second; best = kv.first; }
        if (n) *n = bn;
        return bn > 1 ? best : std::string();     // ★ only when one call stands out — not a tie of ones
    }
    long busiestKhz(unsigned long* n = nullptr) const {
        long best = 0; unsigned long bn = 0;
        for (const auto& kv : khzSpots) if (kv.second > bn) { bn = kv.second; best = kv.first; }
        if (n) *n = bn;
        return best;
    }
    /** "9 spots · 3 countries · furthest Australia" — CW: "312 spots · 148 calls · 23 countries · most heard DL1ABC" */
    std::string headline() const {
        char t[200];
        if (cw) {
            const std::string mh = mostHeard();
            std::snprintf(t, sizeof t, "%lu spot%s \xc2\xb7 %zu call%s \xc2\xb7 %zu countr%s%s%s", spots, spots == 1 ? "" : "s",
                          calls.size(), calls.size() == 1 ? "" : "s", countrySpots.size(), countrySpots.size() == 1 ? "y" : "ies",
                          mh.empty() ? "" : " \xc2\xb7 most heard ", mh.c_str());
            return t;
        }
        std::snprintf(t, sizeof t, "%lu spot%s \xc2\xb7 %zu countr%s%s%s", spots, spots == 1 ? "" : "s", countrySpots.size(),
                      countrySpots.size() == 1 ? "y" : "ies", farKm > 0 ? " \xc2\xb7 furthest " : "", farKm > 0 ? farCountry.c_str() : "");
        return t;
    }
    /** The figures, one per line (the beta 4 card's lines, unchanged). */
    std::string figures(bool havePosition) const {
        std::string o; char b[256];
        if (cw) {
            std::snprintf(b, sizeof b, "Spots heard:       %lu from %zu stations (calls heard identifying themselves twice)\n", spots, calls.size());
            o += b;
            if (spots && speed) { std::snprintf(b, sizeof b, "Average speed:     %.0f WPM\n", wpmSum / (double)spots); o += b; }
            unsigned long n = 0;
            const std::string mh = mostHeard(&n);
            if (!mh.empty()) { std::snprintf(b, sizeof b, "Most heard:        %s (%lu spots)\n", mh.c_str(), n); o += b; }
            const long k = busiestKhz(&n);
            if (n) { std::snprintf(b, sizeof b, "Busiest:           %ld\xe2\x80\x93%ld kHz (%lu spots)\n", k, k + 1, n); o += b; }
        } else {
        std::snprintf(b, sizeof b, "Spots heard:       %lu (FT8 %lu, FT4 %lu) from %zu stations\n", spots, ft8, ft4, calls.size());
        o += b;
        }
        std::snprintf(b, sizeof b, "Countries:         %zu\n", countrySpots.size());
        o += b;
        if (farKm > 0) {
            std::snprintf(b, sizeof b, "Furthest:          %s (%s, %s) \xe2\x80\x94 %.0f km\n", farCall.c_str(), farCountry.c_str(), farGrid.c_str(), farKm);
            o += b;
        } else if (!havePosition) o += "Furthest:          (this server has no position set \xe2\x80\x94 add a locator in Setup)\n";
        std::string most, rare; unsigned long mostN = 0, rareN = 0;
        for (const auto& kv : countrySpots) {
            if (kv.second > mostN) { mostN = kv.second; most = kv.first; }
            if (rare.empty() || kv.second < rareN) { rareN = kv.second; rare = kv.first; }
        }
        if (!most.empty()) { std::snprintf(b, sizeof b, "Most frequent:     %s (%lu spots)\n", most.c_str(), mostN); o += b; }
        if (!rare.empty() && rare != most) { std::snprintf(b, sizeof b, "Rarest:            %s (%lu spot%s)\n", rare.c_str(), rareN, rareN == 1 ? "" : "s"); o += b; }
        if (!countryCalls.empty()) {
            o += "\nStations by country:\n";
            std::vector<std::pair<unsigned long, std::string>> v;
            for (const auto& kv : countryCalls) v.push_back({ kv.second, kv.first });
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first != c.first ? a.first > c.first : a.second < c.second; });
            for (const auto& e : v) { std::snprintf(b, sizeof b, "  %-24s %lu\n", e.second.c_str(), e.first); o += b; }
        }
        return o;
    }
};

/** ★★ STUART'S SUMMARY (2026-10-22: "FT8 in background: NOT spot files — a SUMMARY record"): spots heard, the furthest
 *  (country + km from THIS server's own position), the most frequent country, the rarest, how many countries.
 *  ★★★ TWO CARDS (2026-10-23), replacing beta 4's one card per run rewritten every 10 min:
 *   • LAST 10 MINUTES — ONE standing card per VFO (Meta::slot "last10": the same radio, origin, profile and dial find
 *     it again after a restart or a crash, Store::adoptSlot), always the rolling last 10 minutes. Rewritten at most once
 *     a minute (kRefreshSec), only when the window changed (a spot arrived or one aged out), atomically (Store::replace:
 *     tmp + rename, the index the same way) and moved to the top of the list. When the run ends it is written once more,
 *     "to 18:37 UTC", and kept — a listener whose arrival stopped the run finds what it was hearing. It is the instant
 *     look; the hours are the record.
 *     ★ COST, stated: each refresh rewrites index.jsonl whole (~250 bytes an item — 125 KB for 500 items, 1.2 MB at the
 *       5000 cap) — at most once a minute per FT8 VFO, and not at all on a quiet band.
 *   • HOURLY — one card per UTC clock hour, written when the hour closes (store add, never rewritten, evicted oldest
 *     first like everything else): for judging an antenna over days. A run that started or ended inside the hour
 *     writes the part it heard, marked partial, with its minutes. Midnight is just an hour boundary.
 *  Nothing is written for a stretch that heard nothing. Only real callsigns and their OWN grids count (ft8IsCallsign /
 *  ft8IsGrid, beta 4). The admin line's live count is the RUN's, as before ("142 spots, 23 countries so far"). */
class Ft8SummaryTap : public DecoderTap {
public:
    static constexpr int64_t kWindowSec = 10 * 60, kRefreshSec = 60, kHourSec = 3600;
    static constexpr size_t kMaxRecent = 20000;   // ★ spots held for the window (a busy band: ~1500 in 10 min)
    Ft8SummaryTap(gallery::Store& store, gallery::Meta meta, double lat, double lon, std::function<int64_t()> clock = nullptr)
        : store_(store), meta_(std::move(meta)), lat_(lat), lon_(lon), clock_(std::move(clock)) {
        // ★★ V12 2026-10-25: the SAME summary for CW (meta.kind "cw") — the listener's CW decoder and the skimmer: spots,
        //    calls, countries, the most-heard call and the busiest kHz, per hour and for the last 10 minutes.
        //    And PSK's (V12 2026-10-26): the same keyboard-mode summary, without a speed.
        cw_ = meta_.kind == "cw" || meta_.kind == "psk";
        psk_ = meta_.kind == "psk";
        label_ = psk_ ? "PSK" : cw_ ? "CW" : "FT8";
        hour_.cw = cw_; hour_.speed = !psk_;
        const int64_t now = this->now();
        start_ = hourFrom_ = rollWrite_ = now;
        if (meta_.liveSpots) meta_.liveSpots->store(0, std::memory_order_relaxed);
        if (meta_.liveCountries) meta_.liveCountries->store(0, std::memory_order_relaxed);
    }
    ~Ft8SummaryTap() override { finish(true); }
    void spot(bool ft4, const std::string& call, const std::string& gridIn, int snr, double rfHz) override {
        (void)snr;
        if (cw_) return;
        // ★★ beta 4: only a real CALLSIGN is a spot, and only a real grid is its position (ft8IsCallsign / ft8IsGrid).
        if (!ft8IsCallsign(call)) return;
        add(ft4, call, ft8IsGrid(gridIn) ? gridIn : std::string(), rfHz, 0);
    }
    /** ★ A CW spot — the call was already checked by the skimmer (cw::isCallsign); CW carries no grid. */
    void cwSpot(const std::string& call, double rfHz, float wpm, float snrDb, const std::string& how) override {
        (void)snrDb; (void)how;
        if (!cw_ || !ft8IsCallsign(call)) return;
        add(false, call, std::string(), rfHz, wpm);
    }
    void add(bool ft4, const std::string& call, const std::string& grid, double rfHz, double wpm) {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        closeHourIfDue(t);
        const std::string ctry = callsign::country(call);
        double la, lo, km = NAN;
        if (!grid.empty() && std::isfinite(lat_) && std::isfinite(lon_) && gridToLatLon(grid, la, lo)) km = greatCircleKm(lat_, lon_, la, lo);
        hour_.add(ft4, call, grid, ctry, km, rfHz, wpm);
        recent_.push_back({ t, ft4, call, grid, ctry, km, rfHz, wpm });
        if (recent_.size() > kMaxRecent) recent_.pop_front();
        lastSpot_ = t; rollDirty_ = true;
        ++runSpots_;
        if (!ctry.empty() && runCountries_.size() < 1000) runCountries_.insert(ctry);
        if (meta_.liveSpots) meta_.liveSpots->store(runSpots_, std::memory_order_relaxed);
        if (meta_.liveCountries) meta_.liveCountries->store(runCountries_.size(), std::memory_order_relaxed);
    }
    /** ★ The decode thread's clock: the hour closes on a quiet band too, and the window ages spots out. */
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        closeHourIfDue(t);
        trim(t);
        if (rollDirty_ && t - rollWrite_ >= kRefreshSec) writeRolling(t, false);
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        const int64_t t = now();
        closeHourIfDue(t);
        writeHour(t);
        trim(t);
        writeRolling(t, true);
        if (!rollName_.empty()) store_.close(rollName_);
    }
    /** A decoded message as TEXT ("CQ DX K1ABC FN42") → spot() for its transmitting station, if it has one. */
    void spotText(bool ft4, const std::string& text) {
        std::string c, g;
        if (ft8StationFromText(text, c, g)) spot(ft4, c, g, 0, meta_.freqHz);
    }
    /** The rolling card's item name ("" before its first write). */
    std::string rollingName() const { std::lock_guard<std::mutex> lk(m_); return rollName_; }
    /** The hour so far as its card's text (public for the tests). */
    std::string text(int64_t to) const { std::lock_guard<std::mutex> lk(m_); return hourText(to); }

private:
    struct Recent { int64_t t; bool ft4; std::string call, grid, ctry; double km, rf, wpm; };
    int64_t now() const { return clock_ ? clock_() : gallery::nowUtc(); }
    std::string where() const {
        if (cw_) {
            std::string m = meta_.mode; for (auto& c : m) c = (char)std::toupper((unsigned char)c);
            const bool skim = meta_.decoder == "cwskim" || meta_.decoder == "pskskim";
            return label_ + " spots summary \xc2\xb7 " + gallery::freqText(meta_.freqHz)
                 + (skim ? " (the skimmer's centre)" : " " + m) + " \xc2\xb7 "
                 + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
                 + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n";
        }
        return "FT8 / FT4 summary \xc2\xb7 " + gallery::freqText(meta_.freqHz) + " USB \xc2\xb7 "
             + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
             + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n";
    }
    bool havePos() const { return std::isfinite(lat_) && std::isfinite(lon_); }
    std::string hourText(int64_t to) const {
        const int64_t mins = (to - hourFrom_ + 30) / 60;
        const bool whole = hourFrom_ % kHourSec == 0 && to - hourFrom_ >= kHourSec;
        std::string o = where() + gallery::utcStamp(hourFrom_, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + gallery::utcStamp(to, "%H:%M UTC");
        o += whole ? std::string(" \xc2\xb7 the hour\n\n") : " \xc2\xb7 " + std::to_string((long long)mins) + " of the hour's 60 minutes\n\n";
        return o + hour_.figures(havePos());
    }
    /** The clock hour closed: its card, and a fresh tally for the next (from the hour's start). */
    void closeHourIfDue(int64_t t) {
        if (t / kHourSec == hourFrom_ / kHourSec) return;
        writeHour((hourFrom_ / kHourSec + 1) * kHourSec);
        hour_ = Ft8Tally(); hour_.cw = cw_; hour_.speed = !psk_;
        hourFrom_ = t / kHourSec * kHourSec;
    }
    void writeHour(int64_t to) {
        if (hour_.spots == 0) return;
        gallery::Meta m = meta_;
        m.utc = hourFrom_;
        m.partial = !(hourFrom_ % kHourSec == 0 && to - hourFrom_ >= kHourSec);
        m.title = label_ + " " + gallery::utcStamp(hourFrom_, "%H:%M") + "\xe2\x80\x93" + gallery::utcStamp(to, "%H:%M UTC") + " \xc2\xb7 " + hour_.headline();
        store_.add(m, "txt", hourText(to));
    }
    void trim(int64_t t) {
        while (!recent_.empty() && recent_.front().t <= t - kWindowSec) { recent_.pop_front(); rollDirty_ = true; }
    }
    /** The rolling card, rewritten whole (or created, or adopted from an earlier run). `last`: the run is over. */
    void writeRolling(int64_t t, bool last) {
        rollWrite_ = t; rollDirty_ = false;
        if (recent_.empty() && rollName_.empty() && lastSpot_ == 0) return;   // ★ never heard anything: no card
        Ft8Tally w; w.cw = cw_; w.speed = !psk_;
        for (const auto& r : recent_) w.add(r.ft4, r.call, r.grid, r.ctry, r.km, r.rf, r.wpm);
        const int64_t from = std::max(start_, t - kWindowSec);
        gallery::Meta m = meta_;
        m.utc = from;
        m.slot = "last10";
        const std::string to = gallery::utcStamp(t, "%H:%M UTC");
        std::string body = where();
        if (last) body += "The last 10 minutes of the run, to " + to + " (" + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + ")\n\n";
        else body += "The last 10 minutes: " + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + " (refreshed every minute; the hourly cards are the record)\n\n";
        if (w.spots) {
            body += w.figures(havePos());
            m.title = label_ + " last 10 min" + (last ? " to " + to : std::string()) + " \xc2\xb7 " + w.headline();
        } else {
            const std::string ls = lastSpot_ ? gallery::utcStamp(lastSpot_, "%H:%M UTC") : std::string();
            body += "Nothing heard in the last 10 minutes" + (ls.empty() ? std::string() : " (the last spot was at " + ls + ")") + ".\n";
            m.title = label_ + " last 10 min" + (last ? " to " + to : std::string()) + " \xc2\xb7 nothing heard"
                    + (ls.empty() ? std::string() : " since " + gallery::utcStamp(lastSpot_, "%H:%M"));
        }
        if (rollName_.empty()) rollName_ = store_.adoptSlot(m);
        if (rollName_.empty() || !store_.replace(rollName_, m, body, /*toNewest=*/true)) {
            if (!rollName_.empty()) store_.close(rollName_);
            rollName_ = store_.create(m, "txt", body);
        }
    }
    gallery::Store& store_;
    gallery::Meta meta_;
    bool cw_ = false;
    std::string label_ = "FT8";
    bool psk_ = false;
    double lat_, lon_;
    std::function<int64_t()> clock_;
    mutable std::mutex m_;
    bool done_ = false;
    int64_t start_ = 0, hourFrom_ = 0, rollWrite_ = 0, lastSpot_ = 0;
    Ft8Tally hour_;
    std::deque<Recent> recent_;
    bool rollDirty_ = false;
    std::string rollName_;
    uint64_t runSpots_ = 0;
    std::set<std::string> runCountries_;
};

/** ★ One factory for every decoder's gallery copy — background decoding's and a listener's alike. */
inline DecoderTapPtr makeGalleryTap(gallery::Store& st, const gallery::Meta& meta, std::function<double()> dial = nullptr,
                                    std::function<void(const std::string&)> log = nullptr, const std::string& tag = "") {
    if (meta.kind == "wefax" || meta.kind == "sstv") return std::make_shared<GalleryImageTap>(st, meta);
    if (meta.kind == "navtex" || meta.kind == "rtty" || meta.kind == "cw" || meta.kind == "psk") {
        auto t = std::make_shared<GalleryTap>(st, meta, std::move(dial));
        if (log) t->setLog(std::move(log), tag);
        return t;
    }
    return nullptr;
}

// ── Priority across the box: which background VFO gives way to a listener ───────────────────────────────────────
/** ★★★ LISTENERS FIRST, BACKGROUND IN ITS OWN ORDER (Stuart, 2026-10-22: "background decoding gets a tier list drag the
 *  VFO's into order of priority, then the decoder slots are for real listeners as highest priority so a background decode
 *  will have to drop off until its free again").
 *  ★ A profile's VFO order IS its priority: row 1 is rank 1, the highest. Background decoding takes any free slots from
 *    the top of its list; when a LISTENER's decoder finds none, the LOWEST-ranked running background VFO on the whole box
 *    pauses — that one only (a tie on rank: the radio running the most background decoders) — and when a slot frees,
 *    the highest-ranked PAUSED one gets it back.
 *  ★ The box is several processes (one per radio), so the state is files beside the decoder slots:
 *    `bg-<id>.state` — this radio's background VFOs, "<rank> <1 running | 0 paused>" a line — held under an exclusive
 *    flock for as long as the process lives (a dead one's file is unlocked, so it counts for nothing); and
 *    `bg-<id>.drop` — one byte appended per listener asking that radio to pause one more VFO, taken by its runner within
 *    half a second. Appends, so two listeners at once pause two VFOs, not one twice. */
class BgRegistry {
public:
    struct Entry { std::string id; int rank = 0; bool running = false; };
    BgRegistry() = default;
    ~BgRegistry() { close(); }
    BgRegistry(const BgRegistry&) = delete;
    BgRegistry& operator=(const BgRegistry&) = delete;
    void open(const std::string& dir, const std::string& id) {
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ >= 0 || dir.empty()) return;
        dir_ = dir; id_ = id;
        ::unlink(dropPath(dir_, id_).c_str());              // ★ requests meant for an earlier run of this radio are stale
        fd_ = ::open(statePath(dir_, id_).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX | LOCK_NB) != 0) { ::close(fd_); fd_ = -1; }
    }
    void close() {
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ < 0) return;
        ::unlink(statePath(dir_, id_).c_str());
        ::unlink(dropPath(dir_, id_).c_str());
        ::flock(fd_, LOCK_UN); ::close(fd_); fd_ = -1;
    }
    bool isOpen() const { std::lock_guard<std::mutex> lk(m_); return fd_ >= 0; }
    const std::string& dir() const { return dir_; }
    const std::string& id() const { return id_; }
    /** This radio's VFOs, rank and running/paused. */
    void publish(const std::vector<std::pair<int, bool>>& v) {
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ < 0) return;
        std::string b;
        for (auto& e : v) b += std::to_string(e.first) + (e.second ? " 1\n" : " 0\n");
        if (b == last_) return;
        last_ = b;
        if (::ftruncate(fd_, 0) == 0 && !b.empty()) (void)!::pwrite(fd_, b.data(), b.size(), 0);
    }
    /** Pause requests waiting for THIS radio (and clear them). */
    int takeDrops() {
        std::string p; { std::lock_guard<std::mutex> lk(m_); if (fd_ < 0) return 0; p = dropPath(dir_, id_); }
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0 || st.st_size <= 0) return 0;
        ::unlink(p.c_str());
        return (int)st.st_size;
    }
    /** Every live radio's background VFOs. */
    static std::vector<Entry> all(const std::string& dir) {
        std::vector<Entry> out;
        if (dir.empty()) return out;
        DIR* d = ::opendir(dir.c_str());
        if (!d) return out;
        while (dirent* e = ::readdir(d)) {
            const std::string f = e->d_name;
            if (f.rfind("bg-", 0) != 0 || f.size() < 10 || f.compare(f.size() - 6, 6, ".state") != 0) continue;
            const std::string id = f.substr(3, f.size() - 9);
            const int fd = ::open((dir + "/" + f).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            const bool alive = ::flock(fd, LOCK_SH | LOCK_NB) != 0;   // ★ held by its process = alive
            if (!alive) ::flock(fd, LOCK_UN);
            char b[1024] = {0};
            const ssize_t got = alive ? ::pread(fd, b, sizeof b - 1, 0) : 0;
            ::close(fd);
            if (got <= 0) continue;
            const char* q = b;
            int rank = 0, run = 0, used = 0;
            while (std::sscanf(q, "%d %d\n%n", &rank, &run, &used) == 2 && used > 0) { out.push_back({ id, rank, run == 1 }); q += used; }
        }
        ::closedir(d);
        return out;
    }
    /** ★ The radio whose background gives way: the lowest-ranked RUNNING VFO box-wide; a tie, the radio running the most.
     *  "" = no background decoder is running anywhere. */
    static std::string victim(const std::string& dir, int* rankOut = nullptr) {
        const auto v = all(dir);
        std::map<std::string, int> running;
        for (auto& e : v) if (e.running) running[e.id]++;
        std::string id; int rank = 0;
        for (auto& e : v) {
            if (!e.running) continue;
            if (id.empty() || e.rank > rank || (e.rank == rank && running[e.id] > running[id])) { id = e.id; rank = e.rank; }
        }
        if (rankOut) *rankOut = rank;
        return id;
    }
    /** The best rank PAUSED anywhere on the box (0 = none) — a radio resumes its own only if nobody waits ahead of it. */
    static int bestPaused(const std::string& dir) {
        int best = 0;
        for (auto& e : all(dir)) if (!e.running && (best == 0 || e.rank < best)) best = e.rank;
        return best;
    }
    static void requestDrop(const std::string& dir, const std::string& id) {
        const int fd = ::open(dropPath(dir, id).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return;
        (void)!::write(fd, "x", 1);
        ::close(fd);
    }
    /** ★★★ A LISTENER'S DECODER FOUND NO FREE SLOT: pause the lowest-ranked background VFO on the box and wait (briefly)
     *  for its slot. @return true when a slot is free — the caller claims it at once. False: no background decoder was
     *  running anywhere (the box is full of LISTENERS' decoders), or none freed in time. */
    static bool makeRoomForListener(DecoderSlots& slots, const std::string& dir, int timeoutMs = 3000) {
        if (slots.inUse() < slots.max()) return true;
        const std::string id = victim(dir);
        if (id.empty()) return false;
        requestDrop(dir, id);
        for (int t = 0; t < timeoutMs; t += 50) {
            if (slots.inUse() < slots.max()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return slots.inUse() < slots.max();
    }

private:
    static std::string statePath(const std::string& d, const std::string& id) { return d + "/bg-" + id + ".state"; }
    static std::string dropPath(const std::string& d, const std::string& id) { return d + "/bg-" + id + ".drop"; }
    mutable std::mutex m_;
    std::string dir_, id_, last_;
    int fd_ = -1;
};

// ── The engine ──────────────────────────────────────────────────────────────────────────────────────────────────
class BgRunner {
public:
    using cf32 = vibedsp::cf32;
    struct Params {
        int         profileIdx = 0;
        Profile     profile;
        double      sampleRate = 0;
        double      centreHz = 0;      ///< where the capture really is (logical), read once at start
        double      hwOffsetHz = 0;    ///< the dongle's offset tuning — the shim's hwOffsetHz()
        int         fftSize = 16384;
        DecoderSlots* slots = nullptr;
        gallery::Store* store = nullptr;
        BgRegistry* registry = nullptr;   ///< ★ the box's priority list (pause the lowest, resume the highest) — see BgRegistry
        double      lat = NAN, lon = NAN;  ///< ★ this server's position, for the FT8 summary's distances (NaN = none)
        std::function<void(const std::string&)> log;
        /** ★★ How many VFO CHANNELS may run at once besides the slots (a LOCKED radio: its free user slots — Stuart,
         *  "10 user slots … treat each slot as an idle radio"). Empty = no limit but the slots. Read lock-free. */
        std::function<int()> channelBudget;
        /** ★ A listener is on (the items say so: "decoded while a listener was on"). */
        bool listenerOn = false;
        /** ★ Started at a centre the profile did not choose (a listener's dial): the VFOs inside it carry on, and the log
         *  says so rather than "profile N, RF centre <someone else's>". */
        bool inRangeOnly = false;
    };
    BgRunner() = default;
    ~BgRunner() { stop(false); }

    /** Build every VFO's channel, start every decoder, start the worker. */
    void start(const Params& p) {
        stop(false);
        p_ = p;
        if (!chan_ || chan_->fftSize() != p_.fftSize) chan_.reset(new vibedsp::Channelizer(p_.fftSize));
        chans_.clear();
        for (size_t i = 0; i < p_.profile.vfos.size(); i++) {
            auto c = std::make_unique<Chan>();
            c->owner = this; c->vfo = p_.profile.vfos[i]; c->index = (int)i;
            // ★ A CW SKIMMER takes the channel's IQ itself — its slice just wide enough for the segment (the outer 3 % is
            //   the channel filter's), and no demodulator at all.
            c->skim = isSkimmer(c->vfo);
            const double need = c->skim ? passbandHz(c->vfo) / 0.92 : std::max(passbandHz(c->vfo) * 2.5, 24000.0);
            int b = 64;
            while (b < p_.fftSize && p_.sampleRate * (double)b / (double)p_.fftSize < need) b <<= 1;
            c->bins = std::min(b, p_.fftSize);
            c->rate = p_.sampleRate * (double)c->bins / (double)p_.fftSize;
            c->slice.assign((size_t)c->bins, cf32{0.0f, 0.0f});
            if (!c->skim) {
                c->rx.reset(new vibedsp::RxPipeline());
                vibedsp::RxPipeline::Callbacks cb{};
                cb.ctx = c.get();
                cb.audio = &BgRunner::audioCb;
                c->rx->start(c->rate, 1024, 10.0, 48000, cb);
                c->rx->setRdsEnabled(false);
            }
            place(*c);
            DecoderHost::Env env;
            env.log = p_.log;
            env.slots = p_.slots;
            const double dialHz = c->vfo.hz;
            env.dialHz = [dialHz] { return dialHz; };
            if (p_.store) {
                gallery::Store* st = p_.store;
                const Vfo v = c->vfo; const int pi = p_.profileIdx;
                auto lg = p_.log;
                const std::string tag = "VFO " + std::to_string(i + 1) + " (" + khz(v.hz) + ")";
                auto lo_ = listenerOnFlag_;
                const double la0 = p_.lat, lo0 = p_.lon;
                auto last = c->lastItem;
                env.tap = [st, v, pi, lg, tag, lo_, la0, lo0, last](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = galleryKindFor(ext, msg);
                    if (m.kind.empty()) return nullptr;
                    m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = v.dec;
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo_;
                    m.lat = la0; m.lon = lo0;
                    m.lastItemAt = last;
                    return makeGalleryTap(*st, m, nullptr, lg, tag);
                };
                const double la = p_.lat, lo = p_.lon;
                auto lo2 = listenerOnFlag_;
                auto last2 = c->lastItem; auto ls = c->liveSpots, lc = c->liveCountries;
                // ★ …and a CW skimmer's: the same summary, labelled CW (Ft8SummaryTap with kind "cw").
                env.cwSpotTap = [st, v, pi, la, lo, lo2, last2, ls, lc](bool) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "cw"; m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = "cwskim";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo2;
                    m.lastItemAt = last2; m.liveSpots = ls; m.liveCountries = lc;
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
                // ★ …and a PSK skimmer's (V12 2026-10-26), labelled PSK.
                env.pskSpotTap = [st, v, pi, la, lo, lo2, last2, ls, lc](bool) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "psk"; m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = "pskskim";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo2;
                    m.lastItemAt = last2; m.liveSpots = ls; m.liveCountries = lc;
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
                env.spotTap = [st, v, pi, la, lo, lo2, last2, ls, lc]() -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "ft8"; m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = "ft8";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo2;
                    m.lastItemAt = last2; m.liveSpots = ls; m.liveCountries = lc;
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
            }
            c->host = std::make_shared<DecoderHost>(env, "bg:" + std::to_string(i));
            chans_.push_back(std::move(c));
        }
        startedAt_ = std::time(nullptr);
        { std::lock_guard<std::mutex> ck(cpuMtx_); cpu_.clear(); }   // ★ new channels, new counters: the CPU window starts again
        listenerOnFlag_->store(p_.listenerOn);
        startDecoders();
        publish();
        run_.store(true);
        { std::lock_guard<std::mutex> lk(qm_); q_.clear(); qSamples_ = 0; gap_ = false; }
        th_ = std::thread([this] { loop(); });
        running_.store(true);
        int inWin = 0; for (auto& c : chans_) if (c->inWindow) ++inWin;
        if (p_.inRangeOnly)
            say("background decoding: carrying on in range \xe2\x80\x94 profile " + std::to_string(p_.profileIdx + 1) + "'s VFOs inside the radio's window at "
                + khz(p_.centreHz) + ": " + std::to_string(started_) + " of " + std::to_string(chans_.size()) + " decoding"
                + (p_.listenerOn ? " \xe2\x80\x94 with a listener on" : ""));
        else
        say("background decoding: profile " + std::to_string(p_.profileIdx + 1) + ", RF centre " + khz(p_.centreHz) + ", "
            + std::to_string(chans_.size()) + " VFO(s), " + std::to_string(started_) + " decoder(s) running"
            + (inWin < (int)chans_.size() ? " (" + std::to_string(inWin) + " inside the radio's window)" : std::string())
            + (p_.listenerOn ? " \xe2\x80\x94 with a listener on" : ""));
    }
    /** ★★★ THE RADIO MOVED UNDER US (a listener retuned it, or background decoding moved it to the profile centre, or a
     *  listener arrived and it went back to their dial): every channel is re-cut against the new centre — WITHOUT
     *  rebuilding it, so a message in progress on a VFO that stays in the window is not split. VFOs that leave the
     *  window pause (their output written, partial where cut off); VFOs that come back resume (slots and budget
     *  permitting). A different sample rate needs a rebuild: the caller restarts instead. */
    void retarget(double centreHz, double hwOffsetHz, bool listenerOn) {
        if (!running_.load()) return;
        std::lock_guard<std::mutex> sl(slotMtx_);
        listenerOnFlag_->store(listenerOn);
        int left = 0, back = 0;
        {
            std::lock_guard<std::mutex> cl(chanMtx_);
            p_.centreHz = centreHz; p_.hwOffsetHz = hwOffsetHz; p_.listenerOn = listenerOn;
            for (auto& c : chans_) {
                const bool was = c->inWindow;
                place(*c);
                if (was && !c->inWindow) { if (c->decoding) { pauseOne(*c, "left the radio's window"); } ++left; }
                if (!was && c->inWindow) ++back;
            }
        }
        if (back) lastResume_ = {};    // ★ come back promptly (the next slotTick)
        publish();
        if (left || back) say("background decoding: the radio moved to " + khz(centreHz) + " \xe2\x80\x94 "
                              + std::to_string(left) + " VFO(s) left its window, " + std::to_string(back) + " came back in");
    }
    double centreHz() const { return p_.centreHz; }
    double sampleRate() const { return p_.sampleRate; }
    int  inWindowCount() const { int n = 0; for (auto& c : chans_) if (c->inWindow) ++n; return n; }
    /** ★★ Stop NOW: the worker joined, every decoder stopped (its tap writes what it held — partial where cut off),
     *  every slot back. Idempotent. */
    void stop(bool sayIt = true) {
        const bool was = running_.exchange(false);
        { std::lock_guard<std::mutex> lk(qm_); run_.store(false); q_.clear(); qSamples_ = 0; }
        qcv_.notify_all();
        if (th_.joinable()) th_.join();
        for (auto& c : chans_) if (c->host) c->host->shutdown();
        chans_.clear();
        if (p_.registry) p_.registry->publish({});
        if (was && sayIt) say("background decoding stopped");
    }
    bool running() const { return running_.load(); }
    /** The VFOs paused for listeners (or waiting for a first slot) — rank order. */
    int  decodersPaused() const { int n = 0; for (auto& c : chans_) if (!c->decoding) ++n; return n; }
    /** ★ Test seam: run the worker's slot housekeeping now (pause requests, resumes) rather than within its 0.5 s. */
    void slotTickForTest() { slotTick(true); }
    int  decodersRunning() const { int n = 0; for (auto& c : chans_) if (c->decoding) ++n; return n; }
    uint64_t droppedSamples() const { return dropped_.load(); }
    uint64_t fedSamples() const { uint64_t n = 0; for (auto& c : chans_) if (c->host) n += c->host->fedSamples(); return n; }

    /** ★ THE DSP THREAD'S WHOLE COST: a copy into a bounded queue. Never waits; past half a second the oldest goes. */
    void feedIq(const cf32* iq, int n) {
        if (!running_.load(std::memory_order_relaxed) || n <= 0) return;
        std::vector<cf32> v(iq, iq + n);
        {
            std::lock_guard<std::mutex> lk(qm_);
            q_.push_back(std::move(v));
            qSamples_ += (size_t)n;
            const size_t cap = (size_t)std::max(65536.0, p_.sampleRate * 0.5);
            while (qSamples_ > cap && q_.size() > 1) {
                qSamples_ -= q_.front().size();
                dropped_.fetch_add(q_.front().size(), std::memory_order_relaxed);
                q_.pop_front();
                gap_ = true;
            }
        }
        qcv_.notify_one();
    }

    /** ★★ V12 beta 4 — THE ADMIN PAGE'S LINE (admin only; LocalSdrShim::adminStatusJson). Per VFO: running / paused (and
     *  why) / out of the radio's window, the decoder's lock, the last item written, and its CPU as % of ONE core,
     *  averaged over ~10 s. ★ Measured, never estimated: every figure is a thread reading its own CPU clock around its own
     *  work (vibeThreadCpuNs) — the worker around the shared FFT and each VFO's cut + demodulator, each decode thread
     *  around its decoder — so the counters cost two clock reads per block and nothing samples anything until the page
     *  asks (it polls every 2 s while open). The channeliser's FFT serves every VFO at once: it is reported as
     *  "channels", never shared out by guesswork. "cpuPct": null until two samples ≥ 1 s apart exist. */
    std::string statusJson() {
        std::lock_guard<std::mutex> cl(chanMtx_);
        std::lock_guard<std::mutex> ck(cpuMtx_);
        const auto now = std::chrono::steady_clock::now();
        CpuSample cur{ now, sharedNs_.load(std::memory_order_relaxed), {} };
        for (auto& c : chans_) cur.vfo.push_back(c->demodNs.load(std::memory_order_relaxed) + (c->host ? c->host->cpuNs() : 0));
        if (!cpu_.empty() && cpu_.back().vfo.size() != cur.vfo.size()) cpu_.clear();   // ★ a restart: new VFOs
        if (cpu_.empty() || now - cpu_.back().t >= std::chrono::milliseconds(1500)) cpu_.push_back(cur);
        while (cpu_.size() > 2 && now - cpu_[1].t >= std::chrono::seconds(10)) cpu_.pop_front();
        const CpuSample* base = nullptr;
        if (!cpu_.empty() && now - cpu_.front().t >= std::chrono::seconds(1)) base = &cpu_.front();
        const double wallNs = base ? (double)std::chrono::duration_cast<std::chrono::nanoseconds>(now - base->t).count() : 0;
        auto pct = [&](uint64_t a, uint64_t b) -> std::string {
            if (!base || wallNs <= 0) return "null";
            char t[32]; std::snprintf(t, sizeof t, "%.2f", 100.0 * (double)(a >= b ? a - b : 0) / wallNs); return t;
        };
        std::string j = "{\"running\":" + std::string(running_.load() ? "true" : "false")
            + ",\"profile\":" + std::to_string(p_.profileIdx + 1)
            + ",\"centreHz\":" + std::to_string((long long)std::llround(p_.centreHz))
            + ",\"inRangeOnly\":" + (p_.inRangeOnly ? "true" : "false")
            + ",\"listenerOn\":" + (p_.listenerOn ? "true" : "false");
        uint64_t sumNow = cur.shared, sumBase = base ? base->shared : 0;
        j += ",\"channelsCpuPct\":" + pct(cur.shared, sumBase) + ",\"vfos\":[";
        for (size_t i = 0; i < chans_.size(); i++) {
            const auto& c = *chans_[i];
            const DecoderDef* d = decoderDef(c.vfo.dec);
            const std::string state = !c.inWindow ? "out" : c.decoding ? "running" : "paused";
            const int fs = c.decoding && c.host ? c.host->fskState() : -1;
            const char* lock = fs < 0 ? "" : fs == 3 ? "locked" : fs == 0 ? "no signal" : "hunting";
            sumNow += cur.vfo[i]; if (base) sumBase += base->vfo[i];
            char hz[32]; std::snprintf(hz, sizeof hz, "%.0f", c.vfo.hz);
            if (i) j += ",";
            j += std::string("{\"hz\":") + hz + ",\"dec\":\"" + gallery::jsonEsc(c.vfo.dec) + "\",\"label\":\"" + gallery::jsonEsc(d ? d->label : c.vfo.dec)
               + "\",\"state\":\"" + state + "\",\"why\":\"" + gallery::jsonEsc(state == "paused" ? c.pauseWhy : std::string())
               + "\",\"lock\":\"" + lock + "\",\"last\":" + std::to_string((long long)c.lastItem->load(std::memory_order_relaxed))
               + ",\"cpuPct\":" + pct(cur.vfo[i], base ? base->vfo[i] : 0)
               // ★ FT8/FT4: the summary's running count, so a quiet card is not read as "no decodes"
               + (d && (!*d->ext || d->widthHz > 0) ? ",\"spots\":" + std::to_string((unsigned long long)c.liveSpots->load(std::memory_order_relaxed))
                                + ",\"countries\":" + std::to_string((unsigned long long)c.liveCountries->load(std::memory_order_relaxed)) : std::string())
               + "}";
        }
        j += "],\"cpuPct\":" + pct(sumNow, sumBase) + "}";
        return j;
    }


private:
    struct Chan {
        BgRunner* owner = nullptr;
        Vfo vfo; int index = 0;
        int bins = 0, centreBin = 0; double rate = 0;
        std::vector<cf32> slice;
        vibedsp::Channelizer::ExtractCtx ectx;
        std::unique_ptr<vibedsp::RxPipeline> rx;     // ★ null for a CW skimmer (fed IQ)
        std::shared_ptr<DecoderHost> host;
        bool skim = false;
        double sliceRf = 0;                          // ★ the RF frequency at the centre of this VFO's slice
        bool decoding = false;
        bool inWindow = true;    // ★ inside what the radio captures now (a listener's dial may have moved it away)
        uint64_t lastDrop = 0;
        // ★ V12 beta 4, the admin page: this VFO's demodulator CPU (the worker's own clock around its cut and feed),
        //   when its decoder last wrote an item, and why it is paused if it is.
        std::atomic<uint64_t> demodNs{0};
        std::shared_ptr<std::atomic<int64_t>> lastItem = std::make_shared<std::atomic<int64_t>>(0);
        std::shared_ptr<std::atomic<uint64_t>> liveSpots = std::make_shared<std::atomic<uint64_t>>(0),
                                               liveCountries = std::make_shared<std::atomic<uint64_t>>(0);
        std::string pauseWhy;
    };
    /** Cut this VFO's channel against the current centre, and say whether it is inside the window at all. */
    void place(Chan& c) {
        const double binHz = p_.sampleRate / (double)p_.fftSize;
        const double off = c.vfo.hz - p_.centreHz - p_.hwOffsetHz;
        c.inWindow = std::fabs(c.vfo.hz - p_.centreHz) <= halfWindowHz(p_.sampleRate);
        c.centreBin = (int)std::lround(off / binHz);
        const double resid = off - c.centreBin * binHz;
        // ★ A skimmer's slice is centred where its centre bin is: the VFO less the residual (its spots are RF anyway).
        c.sliceRf = p_.centreHz + p_.hwOffsetHz + c.centreBin * binHz;
        if (!c.rx) return;
        c.rx->setTune(resid, c.vfo.mode == "lsb" ? vibedsp::RxPipeline::Mode::SSB_LSB
                           : c.vfo.mode == "nfm" ? vibedsp::RxPipeline::Mode::NFM : vibedsp::RxPipeline::Mode::SSB_USB,
                      passbandHz(c.vfo));
    }
    /** ★ The channels the budget allows now (a locked radio's free user slots); a large number when there is none. */
    int budget() const { return p_.channelBudget ? std::max(0, p_.channelBudget()) : 1 << 20; }
    int runningCount() const { int n = 0; for (auto& c : chans_) if (c->decoding) ++n; return n; }
    static void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* c = (Chan*)ctx;
        if (c && c->host && frames > 0) c->host->feed(pcm, frames, ch == 2 ? 2 : 1);
    }
    /** ★ From the top of the list down: background takes any free slots, in its own priority order. */
    void startDecoders() {
        started_ = 0;
        for (auto& c : chans_) {
            if (!c->inWindow) continue;
            if (runningCount() >= budget()) { c->pauseWhy = "waiting \xe2\x80\x94 every user slot of this radio is taken (listeners come first)"; say("background decoding: VFO " + name(*c) + " waiting \xe2\x80\x94 every user slot of this radio is taken (listeners come first)"); continue; }
            startOne(*c);
            if (c->decoding) ++started_;
            else say("background decoding: VFO " + name(*c) + " waiting \xe2\x80\x94 no decoder slot free (listeners come first)");
        }
    }
    void startOne(Chan& c) {
        const DecoderDef* d = decoderDef(c.vfo.dec);
        if (!d || !c.host) return;
        // ★ FT8/FT4 is the host's spotter (its own slot), every other decoder its audio-extension decoder.
        const auto r = *d->ext ? c.host->start(d->ext, attachMsg(c.vfo)) : c.host->startSpots();
        c.decoding = r == DecoderHost::Start::Ok || r == DecoderHost::Start::Joined;
        c.pauseWhy = c.decoding ? "" : "waiting for a decoder slot (listeners come first)";
    }
    /** ★ Pause one VFO for a listener: its decoder stops — the tap WRITES what it held (an RTTY buffer; a chart or
     *  picture in progress as PARTIAL) — and its slot is free. The channel keeps running, decoder-less (the host takes
     *  no audio while nothing runs), which is cheaper than tearing down and rebuilding the pipeline when it comes back. */
    void pauseOne(Chan& c, const char* why = "a listener needed a decoder slot") {
        if (!c.host || !c.decoding) return;
        c.host->stop(); c.host->stopSpots();
        c.decoding = false;
        c.pauseWhy = why;
        say("background decoding: VFO " + name(c) + " paused \xe2\x80\x94 " + why);
    }
    std::string name(const Chan& c) const {
        const DecoderDef* d = decoderDef(c.vfo.dec);
        return std::to_string(c.index + 1) + " (" + khz(c.vfo.hz) + " " + (d ? d->label : "?") + ")";
    }
    void publish() {
        if (!p_.registry) return;
        std::vector<std::pair<int, bool>> v;
        for (auto& c : chans_) if (c->inWindow) v.push_back({ c->index + 1, c->decoding });   // ★ out of the window = not waiting
        p_.registry->publish(v);
    }
    /** ★★ The slot housekeeping, on the worker thread twice a second: listeners' pause requests first (lowest rank
     *  first), then — every 3 s, `force` for a test — the highest-ranked paused VFO back when a slot is free and nobody
     *  on the box waits ahead of it. Never a tight loop: a stat() and a directory read at most. */
    void slotTick(bool force = false) {
        std::lock_guard<std::mutex> sl(slotMtx_);
        bool changed = false;
        // ★★ The channel budget (a locked radio's free user slots): over it, the lowest running VFO gives up its channel.
        while (runningCount() > budget()) {
            Chan* low = nullptr;
            for (auto& c : chans_) if (c->decoding) low = c.get();
            if (!low) break;
            pauseOne(*low, "a listener took this radio's last user slot"); changed = true;
        }
        for (int n = p_.registry ? p_.registry->takeDrops() : 0; n > 0; --n) {
            Chan* low = nullptr;
            for (auto& c : chans_) if (c->decoding) low = c.get();       // ★ rows are in rank order: the last running is the lowest
            if (!low) break;
            pauseOne(*low); changed = true;
        }
        const auto now = std::chrono::steady_clock::now();
        // ★ Never straight back into a slot just given up: the listener it was for claims it within moments, and a
        //   resume in the same breath would take it first. Resumes wait 3 s after any pause.
        if (changed) lastResume_ = now;
        else if (force || now - lastResume_ > std::chrono::seconds(3)) {
            lastResume_ = now;
            for (auto& c : chans_) {
                if (c->decoding || !c->inWindow) continue;
                if (!p_.slots || p_.slots->inUse() >= p_.slots->max() || runningCount() >= budget()) break;
                const int best = p_.registry ? BgRegistry::bestPaused(p_.registry->dir()) : 0;
                if (best && c->index + 1 > best) break;             // ★ a higher-ranked VFO on another radio is waiting
                if (c->rx) c->rx->noteInputGap();                   // ★ its channel was not fed while paused
                startOne(*c);
                if (!c->decoding) break;
                say("background decoding: VFO " + name(*c) + " resumed");
                changed = true;
                break;                                               // ★ one at a time — the box is re-read next round
            }
        }
        if (changed) publish();
    }
    void say(const std::string& s) { if (p_.log) p_.log(s); }
    void loop() {
        vibeDecoderThread("vibe-bgdsp");
        auto lastYieldCheck = std::chrono::steady_clock::now();
        for (;;) {
            std::vector<cf32> buf; bool gap = false;
            {
                std::unique_lock<std::mutex> lk(qm_);
                qcv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !q_.empty() || !run_.load(); });
                if (!run_.load()) return;
                if (!q_.empty()) { buf = std::move(q_.front()); q_.pop_front(); qSamples_ -= buf.size(); gap = std::exchange(gap_, false); }
            }
            const auto now = std::chrono::steady_clock::now();
            /* ★★ DROPS, INTO THE LOG, once a minute when there were any (2026-10-22): the IQ queue (this thread fell behind
             *  the radio) and each VFO's decoder queue (its decoder fell behind). A message split on a quiet box must be
             *  explainable from the log the next morning. */
            if (now - lastDropLog_ > std::chrono::seconds(60)) {
                lastDropLog_ = now;
                const uint64_t iq = dropped_.load();
                if (iq > lastIqDrop_ && p_.sampleRate > 0) {
                    char b[160]; std::snprintf(b, sizeof b, "background decoding: %.1f s of IQ dropped in the last minute (the channels fell behind the radio)",
                                               (double)(iq - lastIqDrop_) / p_.sampleRate);
                    say(b);
                }
                lastIqDrop_ = iq;
                for (auto& c : chans_) {
                    const uint64_t d = c->host ? c->host->droppedSamples() : 0;
                    if (d > c->lastDrop) {
                        char b[200]; std::snprintf(b, sizeof b, "background decoding: VFO %d (%s) — %.1f s of audio dropped in the last minute (its decoder fell behind)",
                                                   c->index + 1, khz(c->vfo.hz).c_str(), (double)(d - c->lastDrop) / 48000.0);
                        say(b);
                    }
                    c->lastDrop = d;
                }
            }
            if (now - lastYieldCheck > std::chrono::milliseconds(500)) { lastYieldCheck = now; slotTick(); }
            if (buf.empty()) continue;
            if (gap) for (auto& c : chans_) if (c->rx) c->rx->noteInputGap();
            std::lock_guard<std::mutex> cl(chanMtx_);
            // ★ V12 beta 4: what this thread costs, on its OWN clock — the shared FFT, and each VFO's cut + demodulator.
            const uint64_t t0 = vibeThreadCpuNs();
            uint64_t perVfo = 0;
            chan_->feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int) {
                const long long bi = chan_->blockIndex();
                for (auto& c : chans_) {
                    if (!c->decoding) continue;          // ★ a paused or out-of-window channel costs nothing
                    const uint64_t a = vibeThreadCpuNs();
                    const int got = chan_->extract(bins, c->centreBin, c->bins, c->slice.data(), c->ectx, bi);
                    if (got > 0) {
                        if (c->rx) c->rx->feed(c->slice.data(), got);
                        else if (c->host) c->host->feedIq(c->slice.data(), got, c->rate, c->sliceRf);
                    }
                    const uint64_t d = vibeThreadCpuNs() - a;
                    c->demodNs.fetch_add(d, std::memory_order_relaxed); perVfo += d;
                }
            });
            const uint64_t all = vibeThreadCpuNs() - t0;
            sharedNs_.fetch_add(all > perVfo ? all - perVfo : 0, std::memory_order_relaxed);
        }
    }

    Params p_;
    std::unique_ptr<vibedsp::Channelizer> chan_;
    std::vector<std::unique_ptr<Chan>> chans_;
    std::thread th_;
    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<std::vector<cf32>> q_;
    size_t qSamples_ = 0;
    bool gap_ = false;
    std::atomic<bool> run_{false}, running_{false};
    std::chrono::steady_clock::time_point lastResume_{};
    std::mutex slotMtx_;
    std::mutex chanMtx_;   // ★ the worker's channel cut vs retarget()
    std::shared_ptr<std::atomic<bool>> listenerOnFlag_ = std::make_shared<std::atomic<bool>>(false);   // ★ the worker's slotTick and a test's slotTickForTest
    std::atomic<uint64_t> dropped_{0};
    std::chrono::steady_clock::time_point lastDropLog_{};
    uint64_t lastIqDrop_ = 0;
    std::time_t startedAt_ = 0;
    int started_ = 0;
    std::atomic<uint64_t> sharedNs_{0};   // ★ the worker's channeliser (one FFT for every VFO) — not any one VFO's
    struct CpuSample { std::chrono::steady_clock::time_point t; uint64_t shared; std::vector<uint64_t> vfo; };
    std::deque<CpuSample> cpu_;           // ★ statusJson's ~10 s window: at most 7 samples, taken only when asked
    std::mutex cpuMtx_;
};

} // namespace bg
} // namespace vibe
