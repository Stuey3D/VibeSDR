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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
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
        if (navtex_) { saveHeld(false); asm_.push(t, now); return; }
        buf_ += t;
        if (buf_.size() >= 8192 || now - lastFlush_ >= 60) flushRun(now);
    }
    void frame(const uint8_t* d, size_t n) override {
        if (!navtex_ || n < 1) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        if (d[0] == 0x03 && n >= 2) asm_.lockState(d[1], gallery::nowUtc());
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
        if (navtex_) { asm_.tick(now); if (!held_.empty() && now - heldAt_ >= 2) saveHeld(true); return; }
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
            const auto& m = held_[i];
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
            mm.title = "RTTY \xc2\xb7 " + gallery::freqText(meta_.freqHz) + " " + upper(meta_.mode);
            const std::string head = "RTTY \xc2\xb7 " + where() + "\nStarted " + gallery::utcStamp(now, "%Y-%m-%d %H:%M UTC") + "\n\n";
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
inline double greatCircleKm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}

/** ★★ STUART'S SUMMARY (2026-10-22: "FT8 in background: NOT spot files — a SUMMARY record per run/day"): spots heard,
 *  the furthest (country + km from THIS server's own position), the most frequent country, the rarest, how many
 *  countries. One .txt per run, split at UTC midnight; nothing is written for a run that heard nothing.
 *  ★ Bounded: the per-call table holds at most kMaxCalls (FT8 on a busy band at night is a few thousand calls). */
class Ft8SummaryTap : public DecoderTap {
public:
    static constexpr size_t kMaxCalls = 20000;
    Ft8SummaryTap(gallery::Store& store, gallery::Meta meta, double lat, double lon)
        : store_(store), meta_(std::move(meta)), lat_(lat), lon_(lon) { reset(gallery::nowUtc()); }
    ~Ft8SummaryTap() override { finish(true); }
    void spot(bool ft4, const std::string& call, const std::string& grid, int snr, double rfHz) override {
        (void)snr; (void)rfHz;
        std::lock_guard<std::mutex> lk(m_);
        if (done_ || call.empty()) return;
        const int64_t now = gallery::nowUtc();
        if (now / 86400 != from_ / 86400) { write(now); reset(now); }
        ++spots_; (ft4 ? ft4_ : ft8_)++;
        const std::string ctry = callsign::country(call);
        if (calls_.size() < kMaxCalls && !calls_.count(call)) {
            calls_[call] = ctry;
            if (!ctry.empty()) countryCalls_[ctry]++;
        }
        if (!ctry.empty()) countrySpots_[ctry]++;
        double la, lo;
        if (std::isfinite(lat_) && std::isfinite(lon_) && gridToLatLon(grid, la, lo)) {
            const double km = greatCircleKm(lat_, lon_, la, lo);
            if (km > farKm_) { farKm_ = km; farCall_ = call; farGrid_ = grid; farCountry_ = ctry; }
        }
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        write(gallery::nowUtc());
    }
    /** The record as text — what the gallery item holds (public for the test). */
    std::string text(int64_t to) const {
        std::string o = "FT8 / FT4 summary \xc2\xb7 " + gallery::freqText(meta_.freqHz) + " USB \xc2\xb7 "
                      + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
                      + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n"
                      + gallery::utcStamp(from_, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + gallery::utcStamp(to, "%H:%M UTC") + "\n\n";
        char b[256];
        std::snprintf(b, sizeof b, "Spots heard:       %lu (FT8 %lu, FT4 %lu) from %zu stations\n", spots_, ft8_, ft4_, calls_.size());
        o += b;
        std::snprintf(b, sizeof b, "Countries:         %zu\n", countrySpots_.size());
        o += b;
        if (farKm_ > 0) {
            std::snprintf(b, sizeof b, "Furthest:          %s (%s, %s) \xe2\x80\x94 %.0f km\n", farCall_.c_str(),
                          farCountry_.empty() ? "country unknown" : farCountry_.c_str(), farGrid_.c_str(), farKm_);
            o += b;
        } else if (!std::isfinite(lat_)) o += "Furthest:          (this server has no position set \xe2\x80\x94 add a locator in Setup)\n";
        std::string most, rare; unsigned long mostN = 0, rareN = 0;
        for (const auto& kv : countrySpots_) {
            if (kv.second > mostN) { mostN = kv.second; most = kv.first; }
            if (rare.empty() || kv.second < rareN) { rareN = kv.second; rare = kv.first; }
        }
        if (!most.empty()) { std::snprintf(b, sizeof b, "Most frequent:     %s (%lu spots)\n", most.c_str(), mostN); o += b; }
        if (!rare.empty() && rare != most) { std::snprintf(b, sizeof b, "Rarest:            %s (%lu spot%s)\n", rare.c_str(), rareN, rareN == 1 ? "" : "s"); o += b; }
        if (!countryCalls_.empty()) {
            o += "\nStations by country:\n";
            std::vector<std::pair<unsigned long, std::string>> v;
            for (const auto& kv : countryCalls_) v.push_back({ kv.second, kv.first });
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first != c.first ? a.first > c.first : a.second < c.second; });
            for (const auto& e : v) { std::snprintf(b, sizeof b, "  %-24s %lu\n", e.second.c_str(), e.first); o += b; }
        }
        return o;
    }

private:
    void reset(int64_t now) {
        from_ = now; spots_ = ft8_ = ft4_ = 0; calls_.clear(); countrySpots_.clear(); countryCalls_.clear();
        farKm_ = 0; farCall_.clear(); farGrid_.clear(); farCountry_.clear();
    }
    void write(int64_t now) {
        if (spots_ == 0) return;
        gallery::Meta m = meta_;
        m.utc = from_;
        char t[160];
        std::snprintf(t, sizeof t, "FT8 summary \xc2\xb7 %lu spots \xc2\xb7 %zu countries%s%s", spots_, countrySpots_.size(),
                      farKm_ > 0 ? " \xc2\xb7 furthest " : "", farKm_ > 0 ? (farCountry_.empty() ? farCall_ : farCountry_).c_str() : "");
        m.title = t;
        store_.add(m, "txt", text(now));
    }
    gallery::Store& store_;
    gallery::Meta meta_;
    double lat_, lon_;
    mutable std::mutex m_;
    bool done_ = false;
    int64_t from_ = 0;
    unsigned long spots_ = 0, ft8_ = 0, ft4_ = 0;
    std::map<std::string, std::string> calls_;
    std::map<std::string, unsigned long> countrySpots_, countryCalls_;
    double farKm_ = 0;
    std::string farCall_, farGrid_, farCountry_;
};

/** ★ One factory for every decoder's gallery copy — background decoding's and a listener's alike. */
inline DecoderTapPtr makeGalleryTap(gallery::Store& st, const gallery::Meta& meta, std::function<double()> dial = nullptr) {
    if (meta.kind == "wefax" || meta.kind == "sstv") return std::make_shared<GalleryImageTap>(st, meta);
    if (meta.kind == "navtex" || meta.kind == "rtty") return std::make_shared<GalleryTap>(st, meta, std::move(dial));
    return nullptr;
}

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
        std::string yieldPath;         ///< ★ box-wide "a listener needs a slot" — see requestYield()
        double      lat = NAN, lon = NAN;  ///< ★ this server's position, for the FT8 summary's distances (NaN = none)
        std::function<void(const std::string&)> log;
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
            const double need = std::max(passbandHz(c->vfo) * 2.5, 24000.0);
            int b = 64;
            while (b < p_.fftSize && p_.sampleRate * (double)b / (double)p_.fftSize < need) b <<= 1;
            c->bins = std::min(b, p_.fftSize);
            c->rate = p_.sampleRate * (double)c->bins / (double)p_.fftSize;
            c->slice.assign((size_t)c->bins, cf32{0.0f, 0.0f});
            const double binHz = p_.sampleRate / (double)p_.fftSize;
            const double off = c->vfo.hz - p_.centreHz - p_.hwOffsetHz;
            c->centreBin = (int)std::lround(off / binHz);
            const double resid = off - c->centreBin * binHz;
            c->rx.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks cb{};
            cb.ctx = c.get();
            cb.audio = &BgRunner::audioCb;
            c->rx->start(c->rate, 1024, 10.0, 48000, cb);
            c->rx->setRdsEnabled(false);
            c->rx->setTune(resid, c->vfo.mode == "lsb" ? vibedsp::RxPipeline::Mode::SSB_LSB
                                : c->vfo.mode == "nfm" ? vibedsp::RxPipeline::Mode::NFM : vibedsp::RxPipeline::Mode::SSB_USB,
                           passbandHz(c->vfo));
            DecoderHost::Env env;
            env.log = p_.log;
            env.slots = p_.slots;
            const double dialHz = c->vfo.hz;
            env.dialHz = [dialHz] { return dialHz; };
            if (p_.store) {
                gallery::Store* st = p_.store;
                const Vfo v = c->vfo; const int pi = p_.profileIdx;
                env.tap = [st, v, pi](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = galleryKindFor(ext, msg);
                    if (m.kind.empty()) return nullptr;
                    m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = v.dec;
                    m.profile = "Profile " + std::to_string(pi + 1);
                    return makeGalleryTap(*st, m);
                };
                const double la = p_.lat, lo = p_.lon;
                env.spotTap = [st, v, pi, la, lo]() -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "ft8"; m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = "ft8";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
            }
            c->host = std::make_shared<DecoderHost>(env, "bg:" + std::to_string(i));
            chans_.push_back(std::move(c));
        }
        startedAt_ = std::time(nullptr);
        yielded_.store(false);
        startDecoders();
        run_.store(true);
        { std::lock_guard<std::mutex> lk(qm_); q_.clear(); qSamples_ = 0; gap_ = false; }
        th_ = std::thread([this] { loop(); });
        running_.store(true);
        say("background decoding: profile " + std::to_string(p_.profileIdx + 1) + ", RF centre " + khz(p_.centreHz) + ", "
            + std::to_string(chans_.size()) + " VFO(s), " + std::to_string(started_) + " decoder(s) running");
    }
    /** (Re)try any decoder the slots refused — the box was full when this started. Cheap when all are running. */
    void retryRefused() {
        if (!running_.load() || yielded_.load()) return;
        for (auto& c : chans_) if (!c->decoding) startOne(*c);
    }
    /** ★★ Stop NOW: the worker joined, every decoder stopped (its tap writes what it held — partial where cut off),
     *  every slot back. Idempotent. */
    void stop(bool sayIt = true) {
        const bool was = running_.exchange(false);
        { std::lock_guard<std::mutex> lk(qm_); run_.store(false); q_.clear(); qSamples_ = 0; }
        qcv_.notify_all();
        if (th_.joinable()) th_.join();
        for (auto& c : chans_) if (c->host) c->host->shutdown();
        chans_.clear();
        if (was && sayIt) say("background decoding stopped");
    }
    bool running() const { return running_.load(); }
    /** True once another process's listener asked for a slot back (requestYield) and this runner let its decoders go. */
    bool yielded() const { return yielded_.load(); }
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

    /** ★★★ A LISTENER ON ANOTHER RADIO OF THIS MACHINE NEEDS A DECODER SLOT. Every radio is its own process and the
     *  slots are box-wide, so the request is a file every runner watches (its mtime): each lets its decoders go at
     *  once and its owner stops it (the shim's tick). A listener's decoder is never refused because the box was busy
     *  decoding for nobody. */
    static void requestYield(const std::string& path) {
        if (path.empty()) return;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return;
        const std::string t = std::to_string((long long)std::time(nullptr)) + "\n";
        (void)!::write(fd, t.data(), t.size());
        ::close(fd);
    }

private:
    struct Chan {
        BgRunner* owner = nullptr;
        Vfo vfo; int index = 0;
        int bins = 0, centreBin = 0; double rate = 0;
        std::vector<cf32> slice;
        vibedsp::Channelizer::ExtractCtx ectx;
        std::unique_ptr<vibedsp::RxPipeline> rx;
        std::shared_ptr<DecoderHost> host;
        bool decoding = false;
    };
    static void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* c = (Chan*)ctx;
        if (c && c->host && frames > 0) c->host->feed(pcm, frames, ch == 2 ? 2 : 1);
    }
    void startDecoders() { started_ = 0; for (auto& c : chans_) { startOne(*c); if (c->decoding) ++started_; } }
    void startOne(Chan& c) {
        const DecoderDef* d = decoderDef(c.vfo.dec);
        if (!d || !c.host) return;
        // ★ FT8/FT4 is the host's spotter (its own slot), every other decoder its audio-extension decoder.
        const auto r = *d->ext ? c.host->start(d->ext, attachMsg(c.vfo)) : c.host->startSpots();
        c.decoding = r == DecoderHost::Start::Ok || r == DecoderHost::Start::Joined;
        if (r == DecoderHost::Start::Refused)
            say("background decoding: VFO " + std::to_string(c.index + 1) + " (" + khz(c.vfo.hz) + ") — no decoder slot free; will retry");
    }
    void say(const std::string& s) { if (p_.log) p_.log(s); }
    /** The yield file's mtime is newer than this run's start. Checked twice a second, a stat() each. */
    bool yieldAsked() const {
        if (p_.yieldPath.empty()) return false;
        struct stat st{};
        return ::stat(p_.yieldPath.c_str(), &st) == 0 && st.st_mtime >= startedAt_;
    }
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
            if (now - lastYieldCheck > std::chrono::milliseconds(500)) {
                lastYieldCheck = now;
                if (!yielded_.load() && yieldAsked()) {
                    // ★ Let the slots go HERE, from this thread, at once — the owner's tick does the rest.
                    for (auto& c : chans_) { if (c->host) c->host->shutdown(); c->decoding = false; }
                    yielded_.store(true);
                    say("background decoding: a listener on this machine needs a decoder slot — giving them back");
                }
            }
            if (buf.empty() || yielded_.load()) continue;
            if (gap) for (auto& c : chans_) c->rx->noteInputGap();
            chan_->feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int) {
                const long long bi = chan_->blockIndex();
                for (auto& c : chans_) {
                    const int got = chan_->extract(bins, c->centreBin, c->bins, c->slice.data(), c->ectx, bi);
                    if (got > 0) c->rx->feed(c->slice.data(), got);
                }
            });
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
    std::atomic<bool> run_{false}, running_{false}, yielded_{false};
    std::atomic<uint64_t> dropped_{0};
    std::time_t startedAt_ = 0;
    int started_ = 0;
};

} // namespace bg
} // namespace vibe
