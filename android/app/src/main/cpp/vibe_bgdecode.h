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
// ★★★ beta92 — BELOW THE DECODERS, AND NOT ONLY "NOBODY LISTENING" (Pi 500 V4L, 2026-11-23). "Keep decoding in range"
//     runs these VFOs WITH a listener on, and the rung above was not what they had: each VFO's demodulator is an
//     RxPipeline whose worker (VIBE_DSP_THREADS=1, every multi-core server) took workerInit's AUDIO rung, -19 — level
//     with the listener's own DSP, demod and raw IQ out. A listener turning raw IQ out on at 250 kHz on a satellite pick
//     got broken audio, "IQ overrun — the DSP thread was blocked" and the IQ stream at 11 % of its rate. Now every
//     thread here is the BACKGROUND rung (vibe_thread.h vibeBackgroundThread: vibe-bgdsp, vibe-bgdemod, vibe-bgdec,
//     vibe-bgndb), below listener decoders, and feedIq never waits even for the queue's lock.
#pragma once
#include "vibe_bgdecode_config.h"   // ★ the profiles, validate(), attachMsg()
#include "vibe_bgdecode_band.h"     // ★ V12: a Band digital-spots VFO's span (installBandSpan)
#include "vibe_bg_ease.h"           // ★ V12 2026-11-24: easing off for listeners, on purpose, before it hurts
#include "vibe_bg_fullcpu.h"        // ★★★ beta94: nobody listening — background may use the whole machine
#include "vibe_band_slice.h"        // ★ …and its slices: sized, tapered and placed as a listener's Band (one rule)
#include "vibe_clock.h"             // ★ the capture time a Band slice's slots are cut by
#include "vibe_decoder_host.h"
#include "vibe_gallery.h"
#include "vibe_gallery_shared.h"   // ★ beta92 the last-10 card, one per server
#include "vibe_station_log.h"      // ★ V12 APRS: the day's packets + the station card (StationTap)
#include "vibe_dsc_gallery.h"
#include "vibe_fctlm_gallery.h"   // ★ V12 beta91 FUNcube telemetry: a CSV per pass + the day's log (TELEMETRY CHARTS)
#include "vibe_wefax_finish.h"     // ★ beta92 a WEFAX chart is saved auto-aligned and on clean paper, as the client draws it
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
                // ★ beta96: the longer form adds both-copies-agreed and copies-disagreed (display only)
                if (n >= 21) { held_.back().haveSplit = true; held_.back().fecAgreed = u32(13); held_.back().fecDisagreed = u32(17); }
            }
            saveHeld(true);
        }
        else if (d[0] == 0x08 && n >= 5) {
            // ★★ beta96: a second's SNR slice — i16 BE signal, i16 BE noise, dBFS × 10 — for the message arriving
            auto i16 = [&](size_t o) { return (int16_t)(uint16_t)(d[o] << 8 | d[o + 1]); };
            asm_.slice(i16(1) / 10.0, i16(3) / 10.0);
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
            // ★ 2026-11-01: with the start time and the header — a corrupted header letter (started outside its slots, the
            //   message naming another station) reads "Suspected <station>" on that station's letter; a start-lost message
            //   is named only from its own text. The why (plain English, this message's evidence) goes in the file.
            if (m.id.size() == 4) {
                const char b1 = m.id[0];
                const navtex::Resolution r = navtex::resolveAll(b1, meta_.freqHz, meta_.lat, meta_.lon, m.text, (double)m.startedAt, m.id);
                if (r.stations.size() == 1 || !r.textName.empty()) { m.station = navtex::labelOf(b1, r); m.why = r.why; }
            } else if (m.startLost) {
                const navtex::Resolution r = navtex::startLostResolve(meta_.freqHz, meta_.lat, meta_.lon, m.text, (double)m.startedAt);
                if (r.stations.size() == 1 || !r.textName.empty()) { m.station = navtex::startLostLabel(meta_.freqHz, meta_.lat, meta_.lon, m.text, (double)m.startedAt); m.why = r.why; }
            }
            gallery::Meta mm = meta_;
            mm.utc = m.startedAt;
            mm.title = gallery::NavtexAsm::title(m);
            mm.qualJson = gallery::navtexQualityJson(m);   // ★ beta96: the FEC split + SNR timeline, when measured
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
            const std::string what = meta_.kind == "cw" ? "CW" : meta_.kind == "psk" ? "PSK" : meta_.kind == "sitora" ? "SITOR-A"
                                   : meta_.kind == "balloon" ? "Balloon telemetry" : meta_.kind == "fctlm" ? "FUNcube telemetry" : "RTTY";
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
    if (ext == "aprs") return "aprs";      // ★ V12: the day's packets + a summary card of stations (vibe_station_log.h)
    if (ext == "dsc") return "dsc";        // ★ DSC: the day's log per frequency + an item per distress/urgency call
    if (ext == "sitora") return "sitora";  // ★ V12 SITOR-A: the link's text, as an RTTY run (both stations, labelled)
    if (ext == "balloon") return "balloon"; // ★ V12 balloons: the reports as a run, and each SSDV picture as a .jpg
    if (ext == "lrpt") return "lrpt";      // ★ V12 METEOR-M LRPT: each pass's channels and composite as PNGs
    if (ext == "orbcomm") return "orbcomm"; // ★ V12 ORBCOMM: this radio's sky plot as a PNG at each pass's end
    if (ext == "fctlm") return "fctlm";    // ★ V12 beta90 FUNcube telemetry — beta91: a CSV per pass + the day's log (vibe_fctlm_gallery.h)
    // ★★★ beta94 a transponder finder whose span holds the bird's FUNcube telemetry (JO-97, AO-73) decodes it too: the same
    //   per-pass CSV and day's log as a telemetry pick (the finder's CW and voice rows are not kept)
    if (ext == "xpdr") return msg.find("\"tlm\":") != std::string::npos ? "fctlm" : "";
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
 *    to be a picture (noise that tripped a start tone) are not kept.
 *  ★★ A LISTENER'S WEFAX IS KEPT ONLY IF IT WAS A CHART (Stuart 2026-11-22: "lots of dead wefax false starts"). A
 *    listener's decoder free-runs (no auto_start — it draws whatever is on the dial), so tuning round with WEFAX open
 *    left a 58–100-line strip of noise in the gallery at every retune. Now a listener's strip is kept only if a START
 *    TONE or PHASING was seen for it (0x02 start, or 0x04 phase 1 start tone / 2 phasing), or it ran kChartLines (600
 *    lines = 5 minutes at 120 LPM — a chart joined part-way, with no start of its own) — otherwise it is dropped
 *    silently.
 *  ★★ …AND A BACKGROUND CHART BEGUN WITHOUT EITHER (2026-11-22): waiting in the background, the decoder now also begins a
 *    chart that comes OUT OF THE NOISE (Northwood faded in 2½ minutes into a chart — wefax_decoder.cpp), or after a stop
 *    tone with no start tone. Such a chart is missing its top, so it is held to the same 600 lines and, kept, is marked
 *    PARTIAL. A background 0x02 counts as a start when the decoder's last phase report was a start tone or phasing (1/2;
 *    the start tone is reported before its START fires) — or when none has come (an older decoder).
 *  ★★★ A WEFAX CHART IS SAVED AS THE LISTENER SAW IT (Stuart 2026-11-23: "background wefax is not applying our auto align
 *    and margin or contrast tweaks like they do for users"). The gallery shows this PNG — it was the raw lines: grey
 *    speckle, rolled. It is now finished first (vibe_wefax_finish.h, a port of the clients' wefaxAlign + wefaxCrisp,
 *    held to them pixel for pixel by test-wefax-finish): auto-aligned for the station on this dial (meta.freqHz — the dial,
 *    background and listener alike), lost lines filled, the chart's own paper → white and ink → black. Background charts
 *    and listeners' alike; SSTV untouched. The keep / drop rules above are decided first, unchanged. */
class GalleryImageTap : public DecoderTap {
public:
    static constexpr uint32_t kMaxLines = 3000, kMaxWidth = 4096, kChartLines = 600;
    GalleryImageTap(gallery::Store& store, gallery::Meta meta) : store_(store), meta_(std::move(meta)) {
        sstv_ = meta_.kind == "sstv"; listener_ = meta_.origin != "background";
    }
    ~GalleryImageTap() override { finish(true); }
    void frame(const uint8_t* d, size_t n) override {
        if (n < 1) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        auto u32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
        const uint8_t op = d[0];
        if (!sstv_) {
            if (op == 0x02 && n == 1) { save(true); begin(0, 0); marked_ = listener_ || lastPhase_ < 0 || lastPhase_ == 1 || lastPhase_ == 2; return; }
            if (op == 0x03 && n == 1) { save(false); return; }
            // ★ 0x04 <phase>: a start tone (1) or phasing (2) heard — whatever is drawn next belongs to a real chart
            if (op == 0x04 && n == 2) { lastPhase_ = d[1]; if (d[1] == 1 || d[1] == 2) { if (open_) marked_ = true; else pendingMark_ = true; } return; }
            if (op == 0x01 && n >= 9) {
                const uint32_t ln = u32(1), w = u32(5);
                if (w == 0 || w > kMaxWidth || n < 9 + (size_t)w) return;
                if (!open_) begin(0, 0);
                if (w_ == 0) w_ = w;
                if (w != w_) return;
                if (ln >= kMaxLines) { save(true); begin(0, 0); w_ = w; marked_ = true; }   // ★ the rest of a long chart
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
    size_t savedCount() const override { return saved(); }
    /** ★ Listener strips dropped (no start tone, no phasing, under kChartLines) — for the tests. */
    size_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }

private:
    void begin(uint32_t w, uint32_t h) {
        open_ = true; px_.clear(); got_.clear(); lines_ = 0; maxRow_ = 0; started_ = gallery::nowUtc();
        marked_ = pendingMark_; pendingMark_ = false;
        w_ = w > kMaxWidth ? 0 : w; h_ = h > 2000 ? 0 : h;
        if (sstv_ && w_ && h_) px_.assign((size_t)w_ * h_ * 3, 0);
    }
    void put(uint32_t row, const uint8_t* p, size_t bytes) {
        const size_t off = (size_t)row * bytes;
        if (px_.size() < off + bytes) px_.resize(off + bytes, 255);
        std::memcpy(px_.data() + off, p, bytes);
        if (!sstv_) { if (got_.size() < (size_t)row + 1) got_.resize((size_t)row + 1, 0); got_[row] = 1; }   // ★ which lines came
        lines_++;
        if (row + 1 > maxRow_) maxRow_ = row + 1;
    }
    /** Write what is held, once. `partial`: it did not reach its end. */
    void save(bool partial) {
        if (!open_) return;
        open_ = false;
        const uint32_t minLines = sstv_ ? 16 : 50;
        if (w_ == 0 || lines_ < minLines || maxRow_ == 0) { px_.clear(); return; }
        // ★★ a WEFAX strip with no start tone, no phasing and under 5 minutes: noise from tuning round (a listener's), or a
        //    background chart out of the noise too short to be worth keeping — dropped. A background one kept is PARTIAL.
        if (!sstv_ && !marked_ && lines_ < kChartLines) { px_.clear(); ++dropped_; return; }
        if (!sstv_ && !listener_ && !marked_) partial = true;
        const int ch = sstv_ ? 3 : 1;
        const uint32_t h = sstv_ ? (partial ? maxRow_ : h_) : maxRow_;
        px_.resize((size_t)w_ * h * ch, 255);
        if (!sstv_) {   // ★ beta92: aligned + crisp, as the client draws it (see the class note)
            got_.resize(h, 0);
            wefaxfinish::finishChart(px_.data(), w_, h, got_.data(), meta_.freqHz);
            got_.clear(); got_.shrink_to_fit();
        }
        const std::string png = png::encode((int)w_, (int)h, ch, px_.data());
        px_.clear(); px_.shrink_to_fit();
        if (png.empty()) return;
        gallery::Meta m = meta_;
        m.utc = started_;
        m.partial = partial;
        m.title = (sstv_ ? "SSTV" + (mode_.empty() ? std::string() : " \xc2\xb7 " + mode_)
                         : meta_.station.empty() ? std::string("WEFAX chart") : "WEFAX \xc2\xb7 " + meta_.station)
                + " \xc2\xb7 " + std::to_string(w_) + "\xc3\x97" + std::to_string(h);
        if (!store_.add(m, "png", png).empty()) ++saved_;
    }
    gallery::Store& store_;
    gallery::Meta meta_;
    mutable std::mutex m_;
    bool sstv_ = false, open_ = false, done_ = false;
    bool listener_ = false, marked_ = false, pendingMark_ = false;   // ★ a listener's tap; this chart had a start tone / phasing
    int lastPhase_ = -1;                                              // ★ the decoder's last 0x04 phase report (-1 none yet)
    size_t dropped_ = 0;
    uint32_t w_ = 0, h_ = 0, lines_ = 0, maxRow_ = 0;
    std::vector<uint8_t> px_;
    std::vector<uint8_t> got_;   // ★ WEFAX: line y was received (a line never sent is filled or left, as the client does)
    std::string mode_;
    int64_t started_ = 0;
    size_t saved_ = 0;
};

// ── METEOR-M LRPT: the pass's pictures, as the decoder renders them ─────────────────────────────────────────────
/** ★★ V12 (docs/v12/LRPT.md): the decoder hands over finished full-size PNGs (vibelrpt::Run → DecoderTap::picture) when
 *  the pass ends or it is stopped — this files each one. The preview frames it also broadcasts (SSTV-shaped, half size)
 *  are NOT what is kept: they are a live view. ★ Standard PNGs only (Stuart: standard formats). */
class LrptGalleryTap : public DecoderTap {
public:
    LrptGalleryTap(gallery::Store& store, gallery::Meta meta, std::string what = "METEOR LRPT")
        : store_(store), meta_(std::move(meta)), what_(std::move(what)), started_(gallery::nowUtc()) {}
    void picture(const std::string& name, const std::string& png, int w, int h, bool partial) override {
        if (png.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        gallery::Meta m = meta_;
        m.utc = started_;
        m.partial = partial;
        m.title = what_ + " \xc2\xb7 " + gallery::printable(name, 40) + " \xc2\xb7 " + std::to_string(w) + "\xc3\x97" + std::to_string(h);
        m.geo = geoName_; m.geoWhy = geoWhy_;
        if (!store_.add(m, "png", png).empty()) ++saved_;
    }
    /* ★★ V12 2026-11-24 THE MAP OUTLINE (vibe_lrpt_overlay.h, docs/v12/LRPT-MAP-OVERLAY.md). Called once per pass, BEFORE
     *  its pictures: the outline is filed as its own small item (kind "lrptgeo", JSON — listed by no client, served like
     *  any item) and every picture of the pass names it ("geo"); with none, the pictures carry the reason ("gw"). The
     *  PNGs themselves stay the plain picture. */
    void outline(const std::string& json, const std::string& why) override {
        std::lock_guard<std::mutex> lk(m_);
        geoName_.clear(); geoWhy_ = why;
        if (json.empty()) return;
        gallery::Meta m = meta_;
        m.kind = "lrptgeo";
        m.utc = started_;
        m.title = "METEOR LRPT \xc2\xb7 map outline";
        geoName_ = store_.add(m, "json", json);
        if (geoName_.empty()) geoWhy_ = "the map outline could not be saved";
    }
    size_t saved() const { std::lock_guard<std::mutex> lk(m_); return saved_; }
private:
    gallery::Store& store_;
    gallery::Meta meta_;
    std::string what_;   // ★ the title's first word ("METEOR LRPT", "ORBCOMM" — the same picture tap for the sky plot)
    mutable std::mutex m_;
    int64_t started_ = 0;
    size_t saved_ = 0;
    std::string geoName_, geoWhy_;
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
    /** ★★ V12 2026-10-26: spots per MODE ("FT8", "FT4", "WSPR", "JS8", "FT2" …) — Band mode reads several at once, and the
     *  cards name them (Stuart: the summary cards carry the mode). ft8/ft4 above are kept as the two always shown. */
    std::map<std::string, unsigned long> byMode;
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
    void add(const std::string& mode, const std::string& call, const std::string& grid, const std::string& ctry, double km,
             double rfHz = NAN, double wpm = 0) {
        ++spots;
        if (mode == "FT4") ++ft4; else if (mode == "FT8") ++ft8;
        if (!cw && byMode.size() < 32) byMode[mode]++;
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
    /** The modes heard, most spots first (ties by name). */
    std::vector<std::pair<unsigned long, std::string>> modesByCount() const {
        std::vector<std::pair<unsigned long, std::string>> v;
        for (const auto& kv : byMode) v.push_back({ kv.second, kv.first });
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first != c.first ? a.first > c.first : a.second < c.second; });
        return v;
    }
    /** "FT8+FT4" — the card's title names the modes it counts. "" when none. */
    std::string modesLabel() const {
        std::string o;
        for (const auto& e : modesByCount()) { if (!o.empty()) o += "+"; o += e.second; }
        return o;
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
        // ★ FT8 and FT4 always (the Single-mode pair, as the card always read); any other mode heard after them.
        std::string per = "FT8 " + std::to_string(ft8) + ", FT4 " + std::to_string(ft4);
        for (const auto& kv : modesByCount()) if (kv.second != "FT8" && kv.second != "FT4") per += ", " + kv.second + " " + std::to_string(kv.first);
        std::snprintf(b, sizeof b, "Spots heard:       %lu (%s) from %zu stations\n", spots, per.c_str(), calls.size());
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
 *     the file tmp + rename, its new index line appended) and moved to the top of the list. When the run ends it is
 *     written once more, "to 18:37 UTC", and kept — a listener whose arrival stopped the run finds what it was hearing.
 *     It is the instant look; the hours are the record.
 *     ★ COST, stated: each refresh writes the card and APPENDS one index line (~250 bytes) — the index is never
 *       rewritten for it (2026-11-01; compaction tidies the old lines away) — at most once a minute per FT8 VFO, and
 *       not at all on a quiet band.
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
        // ★ …and the RTTY skimmer's (V12 2026-11-14, meta.kind "rttyskim"): the PSK shape, labelled RTTY.
        cw_ = meta_.kind == "cw" || meta_.kind == "psk" || meta_.kind == "rttyskim";
        psk_ = meta_.kind == "psk" || meta_.kind == "rttyskim";
        label_ = meta_.kind == "rttyskim" ? "RTTY" : psk_ ? "PSK" : cw_ ? "CW" : "FT8";
        hour_.cw = cw_; hour_.speed = !psk_;
        const int64_t now = this->now();
        start_ = hourFrom_ = rollWrite_ = now;
        if (meta_.liveSpots) meta_.liveSpots->store(0, std::memory_order_relaxed);
        if (meta_.liveCountries) meta_.liveCountries->store(0, std::memory_order_relaxed);
    }
    ~Ft8SummaryTap() override { finish(true); }
    void spot(const std::string& mode, const std::string& call, const std::string& gridIn, int snr, double rfHz) override {
        (void)snr;
        if (cw_) return;
        // ★★ beta 4: only a real CALLSIGN is a spot, and only a real grid is its position (ft8IsCallsign / ft8IsGrid).
        if (!ft8IsCallsign(call)) return;
        // ★★ beta92 the false-decode filter (vibe_callsign_entity.h): no administration = no spot; an impossible grid = none
        const std::string g = ft8IsGrid(gridIn) ? gridIn : std::string();
        const vibe::callentity::Verdict v = vibe::callentity::judge(call, g);
        if (v == vibe::callentity::kNoEntity) return;
        add(mode.empty() ? std::string("FT8") : mode, call, v == vibe::callentity::kImplausible ? std::string() : g, rfHz, 0);
    }
    /** The pre-V12 form (FT8 or FT4) — the tests and spotText. */
    void spot(bool ft4, const std::string& call, const std::string& gridIn, int snr, double rfHz) {
        spot(std::string(ft4 ? "FT4" : "FT8"), call, gridIn, snr, rfHz);
    }
    /** ★ A CW spot — the call was already checked by the skimmer (cw::isCallsign); CW carries no grid. */
    void cwSpot(const std::string& call, double rfHz, float wpm, float snrDb, const std::string& how) override {
        (void)snrDb; (void)how;
        if (!cw_ || !ft8IsCallsign(call) || vibe::callentity::judge(call, "") == vibe::callentity::kNoEntity) return;
        add("CW", call, std::string(), rfHz, wpm);
    }
    void add(const std::string& mode, const std::string& call, const std::string& grid, double rfHz, double wpm) {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        closeHourIfDue(t);
        const std::string ctry = callsign::country(call);
        double la, lo, km = NAN;
        if (!grid.empty() && std::isfinite(lat_) && std::isfinite(lon_) && gridToLatLon(grid, la, lo)) km = greatCircleKm(lat_, lon_, la, lo);
        hour_.add(mode, call, grid, ctry, km, rfHz, wpm);
        recent_.push_back({ t, mode, call, grid, ctry, km, rfHz, wpm, ++seq_ });
        if (!cw_ && runModes_.size() < 32) runModes_[mode]++;
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
    struct Recent { int64_t t; std::string mode, call, grid, ctry; double km, rf, wpm; uint64_t seq = 0; };
    int64_t now() const { return clock_ ? clock_() : gallery::nowUtc(); }
    /** ★★ beta92 the last-10 card is the SERVER's: "every radio on this server" in place of who this run is. */
    std::string whereShared() const {
        std::string w = where();
        for (const char* who : { "background decoding", "a listener's decoder" }) {
            const size_t p = w.find(who);
            if (p != std::string::npos) { w = w.substr(0, p) + "every radio on this server\n"; break; }
        }
        return w;
    }
    std::string where() const {
        if (cw_) {
            std::string m = meta_.mode; for (auto& c : m) c = (char)std::toupper((unsigned char)c);
            const bool skim = meta_.decoder == "cwskim" || meta_.decoder == "pskskim" || meta_.decoder == "rttyskim";
            return label_ + " spots summary \xc2\xb7 " + gallery::freqText(meta_.freqHz)
                 + (skim ? " (the skimmer's centre)" : " " + m) + " \xc2\xb7 "
                 + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
                 + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n";
        }
        // ★★ V12: Band mode's card says so — one card for the whole band, every mode on it.
        if (meta_.decoder == "ft8band") {
            const digi::Band* b = digi::bandAt(meta_.freqHz);
            return std::string("Digital spots summary \xc2\xb7 Band ") + (b ? b->name : gallery::freqText(meta_.freqHz).c_str()) + " \xc2\xb7 "
                 + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
                 + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n";
        }
        return "FT8 / FT4 summary \xc2\xb7 " + gallery::freqText(meta_.freqHz) + " USB \xc2\xb7 "
             + (meta_.origin == "background" ? "background decoding" : "a listener's decoder")
             + (meta_.profile.empty() ? std::string() : " (" + meta_.profile + ")") + "\n";
    }
    bool havePos() const { return std::isfinite(lat_) && std::isfinite(lon_); }
    /** ★★ THE CARD'S TITLE NAMES ITS MODES (V12, 2026-10-26): "FT8", "FT8+FT4", "WSPR+FT8" — what this stretch heard, most
     *  first. A stretch that heard nothing is named by the run's modes so far, else "FT8" (what it always said). CW: "CW". */
    std::string labelFor(const Ft8Tally& t) const {
        if (cw_) return label_;
        std::string l = t.modesLabel();
        if (l.empty()) {
            std::vector<std::pair<unsigned long, std::string>> v;
            for (const auto& kv : runModes_) v.push_back({ kv.second, kv.first });
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.first != c.first ? a.first > c.first : a.second < c.second; });
            for (const auto& e : v) { if (!l.empty()) l += "+"; l += e.second; }
        }
        return l.empty() ? label_ : l;
    }
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
        m.title = labelFor(hour_) + " " + gallery::utcStamp(hourFrom_, "%H:%M") + "\xe2\x80\x93" + gallery::utcStamp(to, "%H:%M UTC") + " \xc2\xb7 " + hour_.headline();
        store_.add(m, "txt", hourText(to));
    }
    void trim(int64_t t) {
        while (!recent_.empty() && recent_.front().t <= t - kWindowSec) { recent_.pop_front(); rollDirty_ = true; }
    }
    /** The rolling card, rewritten whole (or created, or adopted from an earlier run). `last`: the run is over. */
    /* ★★★ V12 beta92 THE LAST-10 CARD IS ONE PER SERVER (Stuart 11-22: "we dont separate the digital spots"): one card per
     *  kind and DIAL for the whole machine (slot "last10-<kind>-<kHz>", found without the radio, profile or origin), drawn
     *  from a SHARED window — <gallery>/.shared/<slot>.state, every radio's spots of the last 10 minutes — merged under the
     *  shared Lock at each refresh: this run's new spots go in unless ANOTHER radio's process already has the same call in
     *  the same mode within kSameSec (both radios decoded one transmission); older than the window drop out. So the card
     *  counts each transmission once, whichever radio heard it. Bounded: the window's spots (kMaxRecent at most). The hourly
     *  cards stay this run's own (an hour is written once, when it closes). */
    static constexpr int64_t kSameSec = 10;
    std::string rollSlot() const {
        char b[64]; std::snprintf(b, sizeof b, "last10-%s-%.0f", meta_.kind.c_str(), meta_.freqHz / 1e3);
        return b;
    }
    /** The shared window with this run's new spots merged in (each line: t, writer, mode, call, grid, ctry, km, rf, wpm). */
    std::vector<Recent> mergeShared(int64_t t) {
        namespace sh = gallery::sharedday;
        const std::string slot = rollSlot(), me = std::to_string((long long)::getpid());
        std::vector<Recent> all; std::vector<std::string> who;
        const std::string text = sh::readState(store_, slot);
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
            const std::vector<std::string> f = sh::splitTabs(text.substr(p, e - p)); p = e + 1;
            if (f.size() != 9) continue;
            Recent r; r.t = std::atoll(f[0].c_str());
            if (r.t <= t - kWindowSec || r.t > t + 60) continue;
            r.mode = f[2]; r.call = f[3]; r.grid = f[4]; r.ctry = f[5];
            r.km = f[6].empty() ? NAN : std::atof(f[6].c_str()); r.rf = std::atof(f[7].c_str()); r.wpm = std::atof(f[8].c_str());
            all.push_back(r); who.push_back(f[1]);
        }
        for (const auto& r : recent_) {
            if (r.seq <= sharedSeq_) continue;
            bool dup = false;
            for (size_t k = 0; k < all.size() && !dup; k++)
                dup = who[k] != me && all[k].call == r.call && all[k].mode == r.mode && std::llabs(all[k].t - r.t) <= kSameSec;
            if (!dup) { all.push_back(r); who.push_back(me); }
        }
        if (!recent_.empty()) sharedSeq_ = recent_.back().seq;
        while (all.size() > kMaxRecent) { all.erase(all.begin()); who.erase(who.begin()); }
        std::string o;
        char b[64];
        for (size_t k = 0; k < all.size(); k++) {
            const Recent& r = all[k];
            o += std::to_string((long long)r.t) + "\t" + who[k] + "\t" + sh::field(r.mode) + "\t" + sh::field(r.call) + "\t" + sh::field(r.grid) + "\t" + sh::field(r.ctry) + "\t";
            if (std::isfinite(r.km)) { std::snprintf(b, sizeof b, "%.1f", r.km); o += b; }
            std::snprintf(b, sizeof b, "\t%.0f\t%.1f\n", r.rf, r.wpm); o += b;
        }
        sh::writeState(store_, slot, o);
        return all;
    }
    /** The rolling card, rewritten whole (or created, or adopted from an earlier run). `last`: the run is over. */
    void writeRolling(int64_t t, bool last) {
        rollWrite_ = t; rollDirty_ = false;
        if (recent_.empty() && rollName_.empty() && lastSpot_ == 0) return;   // ★ never heard anything: no card
        std::unique_ptr<gallery::sharedday::Lock> fl;
        std::vector<Recent> win;
        const bool shared = !store_.dir().empty() && store_.enabled();
        if (shared) { fl.reset(new gallery::sharedday::Lock(store_.dir())); win = mergeShared(t); }
        else win.assign(recent_.begin(), recent_.end());
        int64_t lastAny = lastSpot_;
        Ft8Tally w; w.cw = cw_; w.speed = !psk_;
        for (const auto& r : win) { w.add(r.mode, r.call, r.grid, r.ctry, r.km, r.rf, r.wpm); lastAny = std::max(lastAny, r.t); }
        const int64_t from = shared ? t - kWindowSec : std::max(start_, t - kWindowSec);
        gallery::Meta m = meta_;
        m.utc = from;
        m.slot = shared ? rollSlot() : "last10";
        const std::string to = gallery::utcStamp(t, "%H:%M UTC");
        std::string body = shared ? whereShared() : where();
        if (last) body += "The last 10 minutes of the run, to " + to + " (" + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + ")\n\n";
        else body += "The last 10 minutes: " + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + " (refreshed every minute; the hourly cards are the record)\n\n";
        if (w.spots) {
            body += w.figures(havePos());
            m.title = labelFor(w) + " last 10 min" + (last ? " to " + to : std::string()) + " \xc2\xb7 " + w.headline();
        } else {
            const std::string ls = lastAny ? gallery::utcStamp(lastAny, "%H:%M UTC") : std::string();
            body += "Nothing heard in the last 10 minutes" + (ls.empty() ? std::string() : " (the last spot was at " + ls + ")") + ".\n";
            m.title = labelFor(w) + " last 10 min" + (last ? " to " + to : std::string()) + " \xc2\xb7 nothing heard"
                    + (ls.empty() ? std::string() : " since " + gallery::utcStamp(lastAny, "%H:%M"));
        }
        if (rollName_.empty()) rollName_ = shared ? gallery::sharedday::adopt(store_, m) : store_.adoptSlot(m);
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
    uint64_t seq_ = 0, sharedSeq_ = 0;   ///< ★ beta92 this run's spots, and how many of them are in the shared window
    bool rollDirty_ = false;
    std::string rollName_;
    uint64_t runSpots_ = 0;
    std::set<std::string> runCountries_;
    std::map<std::string, unsigned long> runModes_;   // ★ spots per mode this run (the title of a quiet stretch)
};

/** ★★ V12 APRS in the gallery: the decoder host's station lines into a day log + summary card (vibe_station_log.h). One
 *  DayLog per decoder run (a VFO or a listener's session), finished when the decoder stops. */
class StationTap : public DecoderTap {
public:
    StationTap(gallery::Store& st, const gallery::Meta& m, std::function<double()> dial = nullptr)
        : st_(st), meta_(m), dial_(std::move(dial)) { last_ = m.lastItemAt; }
    void station(const vibestation::Heard& h) override {
        if (!log_) open();
        log_->heard(h);
        if (last_) last_->store(int64_t(::time(nullptr)));
    }
    void tick() override { if (log_) log_->tick(); }
    void finish(bool) override { if (log_) log_->finish(); }
private:
    /* ★★ OPENED AT THE FIRST STATION, ON THE DIAL IT WAS HEARD ON (Stuart's R36S card, 2026-11-11: the day's APRS logs were
     *  "0000 - 97200.000 kHz APRS" and "128816.667 kHz" — the FM and airband frequencies he was on when he opened APRS,
     *  which then tuned 144.800). The meta's frequency is the dial at ATTACH, before the decoder's own retune; by the
     *  first packet the dial is where APRS listens. Background decoding's meta is already its channel (no dial given). */
    void open() {
        vibestation::DayLog::Options o;
        const double hz = dial_ ? dial_() : 0;
        o.kind = meta_.kind; o.label = "APRS"; o.what = "stations"; o.freqHz = hz > 0 ? hz : meta_.freqHz; o.mode = meta_.mode;
        o.origin = meta_.origin; o.profile = meta_.profile;
        o.kindWord = [](const std::string& k, int n) -> std::string {
            if (k == "position" || k == "mice" || k == "nmea") return n == 1 ? "station on the move or fixed" : "stations with positions";
            if (k == "weather") return n == 1 ? "weather station" : "weather stations";
            if (k == "object" || k == "item") return n == 1 ? "object" : "objects";
            if (k == "message" || k == "ack" || k == "bulletin") return n == 1 ? "messaging station" : "messaging stations";
            if (k == "status") return n == 1 ? "status" : "statuses";
            return "other";
        };
        log_ = std::make_shared<vibestation::DayLog>(st_, o);
    }
    gallery::Store& st_;
    gallery::Meta meta_;
    std::function<double()> dial_;
    std::shared_ptr<vibestation::DayLog> log_;
    std::shared_ptr<std::atomic<int64_t>> last_;
};

/** ★★ V12 BALLOONS (vibe_balloon_run.h): the reports go into a text run exactly as RTTY's do (GalleryTap), and each SSDV
 *  picture is kept as a JPEG — when it COMPLETES (the 0x0B frame says so), or, cut off (the decoder stopped, the listener
 *  left), the last state it reached, marked partial. Bounded: one JPEG per picture key held (the newest draw), ≤ 4 keys. */
class BalloonGalleryTap : public DecoderTap {
public:
    BalloonGalleryTap(gallery::Store& store, gallery::Meta meta, std::function<double()> dial)
        : store_(store), meta_(meta), text_(store, std::move(meta), std::move(dial)) {}
    ~BalloonGalleryTap() override { finish(true); }
    void text(const std::string& t) override { text_.text(t); }
    void tick() override { text_.tick(); }
    void frame(const uint8_t* d, size_t n) override {
        if (n < 12 || d[0] != 0x0B) return;
        const size_t kl = d[1];
        if (n < 2 + kl + 9) return;
        const std::string key((const char*)d + 2, kl);
        const uint8_t* p = d + 2 + kl;
        const int have = p[0] << 8 | p[1], expected = p[2] << 8 | p[3];
        const bool complete = p[4] != 0;
        const int w = p[5] << 8 | p[6], h = p[7] << 8 | p[8];
        std::string jpg((const char*)p + 9, n - (2 + kl + 9));
        std::lock_guard<std::mutex> lk(m_);
        if (done_ || jpg.size() < 4) return;
        Held& e = held_[key];
        e.jpg.swap(jpg); e.have = have; e.expected = expected; e.w = w; e.h = h;
        if (!e.started) e.started = gallery::nowUtc();
        if (complete && !e.saved) { save_(key, e, false); e.saved = true; e.jpg.clear(); e.jpg.shrink_to_fit(); }
        if (held_.size() > 4) {   // the oldest unfinished goes (saved as far as it got)
            auto old = held_.begin();
            for (auto it = held_.begin(); it != held_.end(); ++it) if (it->second.started < old->second.started) old = it;
            if (!old->second.saved && !old->second.jpg.empty()) save_(old->first, old->second, true);
            held_.erase(old);
        }
    }
    void finish(bool interrupted) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (done_) return;
            done_ = true;
            for (auto& kv : held_) if (!kv.second.saved && !kv.second.jpg.empty()) save_(kv.first, kv.second, true);
            held_.clear();
        }
        text_.finish(interrupted);
    }
private:
    struct Held { std::string jpg; int have = 0, expected = 0, w = 0, h = 0; int64_t started = 0; bool saved = false; };
    void save_(const std::string& key, const Held& e, bool partial) {
        gallery::Meta m = meta_;
        m.utc = e.started; m.partial = partial;
        const std::string call = gallery::printable(key.substr(0, key.find('#')), 12);
        m.title = "SSDV \xc2\xb7 " + call + " \xc2\xb7 " + std::to_string(e.w) + "\xc3\x97" + std::to_string(e.h)
                + (partial ? " \xc2\xb7 " + std::to_string(e.have) + (e.expected ? " of " + std::to_string(e.expected) : std::string()) + " packets" : std::string());
        store_.add(m, "jpg", e.jpg);
    }
    gallery::Store& store_;
    gallery::Meta meta_;
    GalleryTap text_;
    std::mutex m_;
    bool done_ = false;
    std::map<std::string, Held> held_;
};

/** ★ One factory for every decoder's gallery copy — background decoding's and a listener's alike. */
inline DecoderTapPtr makeGalleryTap(gallery::Store& st, const gallery::Meta& meta, std::function<double()> dial = nullptr,
                                    std::function<void(const std::string&)> log = nullptr, const std::string& tag = "") {
    if (meta.kind == "wefax" || meta.kind == "sstv") return std::make_shared<GalleryImageTap>(st, meta);
    if (meta.kind == "lrpt") return std::make_shared<LrptGalleryTap>(st, meta);
    if (meta.kind == "orbcomm") return std::make_shared<LrptGalleryTap>(st, meta, "ORBCOMM");   // ★ the sky plot's PNG
    if (meta.kind == "aprs") return std::make_shared<StationTap>(st, meta, std::move(dial));
    if (meta.kind == "dsc") return std::make_shared<DscGalleryTap>(st, meta, std::move(dial));
    if (meta.kind == "balloon") return std::make_shared<BalloonGalleryTap>(st, meta, std::move(dial));
    if (meta.kind == "fctlm") return std::make_shared<FcTlmGalleryTap>(st, meta);   // ★ beta91: per-pass CSV + the day's log
    if (meta.kind == "navtex" || meta.kind == "rtty" || meta.kind == "cw" || meta.kind == "psk" || meta.kind == "sitora") {
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
    /** ★ `busy` (2026-11-22): a picture decoder part-way through a chart — paused for a listener only when nothing idle is
     *  running (the state line's optional third field; an older process writes two, read as not busy). */
    struct Entry { std::string id; int rank = 0; bool running = false; bool busy = false; };
    struct Pub { int rank = 0; bool running = false; bool busy = false; };
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
        std::vector<Pub> p; for (auto& e : v) p.push_back({ e.first, e.second, false });
        publishState(p);
    }
    /** …and whether each is part-way through a picture ("<rank> <running> <busy>"). */
    void publishState(const std::vector<Pub>& v) {
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ < 0) return;
        std::string b;
        for (auto& e : v) b += std::to_string(e.rank) + (e.running ? " 1" : " 0") + (e.busy ? " 1\n" : "\n");
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
            // ★ a line a VFO: "<rank> <running>" or "<rank> <running> <busy>"
            for (const char* q = b; *q; ) {
                const char* e = std::strchr(q, '\n');
                const std::string line(q, e ? (size_t)(e - q) : std::strlen(q));
                int rank = 0, run = 0, busy = 0;
                const int k = std::sscanf(line.c_str(), "%d %d %d", &rank, &run, &busy);
                if (k < 2) break;
                out.push_back({ id, rank, run == 1, k >= 3 && busy == 1 });
                if (!e) break;
                q = e + 1;
            }
        }
        ::closedir(d);
        return out;
    }
    /** ★ The radio whose background gives way: the lowest-ranked RUNNING VFO box-wide; a tie, the radio running the most.
     *  "" = no background decoder is running anywhere. */
    /** ★★ (2026-11-22) A VFO part-way through a picture (busy) is passed over while an idle one is running anywhere: a
     *  chart is 10–25 minutes and is lost from where it was cut; an idle VFO loses nothing. */
    static std::string victim(const std::string& dir, int* rankOut = nullptr) {
        const auto v = all(dir);
        std::map<std::string, int> running;
        bool idle = false;
        for (auto& e : v) if (e.running) { running[e.id]++; if (!e.busy) idle = true; }
        std::string id; int rank = 0;
        for (auto& e : v) {
            if (!e.running || (idle && e.busy)) continue;
            if (id.empty() || e.rank > rank || (e.rank == rank && running[e.id] > running[id])) { id = e.id; rank = e.rank; }
        }
        if (rankOut) *rankOut = rank;
        return id;
    }
    /** ★ beta94: the radios whose background gives way for `need` slots — one entry per VFO to pause (a radio may appear
     *  more than once), lowest-ranked running first, idle before mid-picture (victim()'s rule, repeated). */
    static std::vector<std::string> victims(const std::string& dir, int need) {
        auto v = all(dir);
        std::vector<std::string> out;
        while ((int)out.size() < need) {
            std::map<std::string, int> running;
            bool idle = false;
            for (auto& e : v) if (e.running) { running[e.id]++; if (!e.busy) idle = true; }
            int at = -1;
            for (int i = 0; i < (int)v.size(); ++i) {
                const auto& e = v[(size_t)i];
                if (!e.running || (idle && e.busy)) continue;
                if (at < 0) { at = i; continue; }
                const auto& b = v[(size_t)at];
                if (e.rank > b.rank || (e.rank == b.rank && running[e.id] > running[b.id])) at = i;
            }
            if (at < 0) break;
            out.push_back(v[(size_t)at].id);
            v[(size_t)at].running = false;
        }
        return out;
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
        /* ★★★ beta94: an idle radio's background may hold slots OVER the box's limit (vibe_bg_fullcpu.h) — so the listener
         *  may need more than one given back. As many as it takes, each from the lowest-ranked running VFO left. */
        const int need = std::max(1, slots.inUse() - slots.max() + 1);
        const auto picks = victims(dir, need);
        if (picks.empty()) return false;
        for (const auto& id : picks) requestDrop(dir, id);
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

// ── An NDB skimmer VFO's decoder (V12 2026-10-27) ───────────────────────────────────────────────────────────────
/** ★★ What the runner hands an NDB skimmer VFO's slice to. The decoder itself (vibendb::Service in its fed-slice mode, the
 *  listener's own skimmer, beacon list and gallery cards — vibe_bgdecode_ndb.h) is MADE by the host through
 *  Params::ndbMake, so this header (and every test of the runner) does not carry the beacon list; a host without it simply
 *  shows the VFO "not available on this server". Made when the VFO starts (its slot claimed), stopped when it pauses or
 *  the run ends — the stop writes the gallery's cards (the last-10-minute card "to HH:MM", the hour so far, partial). */
class NdbFeed {
public:
    virtual ~NdbFeed() = default;
    /** From the runner's worker: copy and return — never decode on its thread. */
    virtual void feedSlice(const vibedsp::cf32* x, int n, double rate, double centreRfHz) = 0;
    /** The radio moved: what the slice covers now, and where the hardware's DC is. */
    virtual void aim(double hwCentreHz, double loHz, double hiHz) = 0;
    virtual void stop() = 0;
    virtual uint64_t cpuNs() const = 0;          ///< its decode thread's own CPU clock
    virtual uint64_t droppedBlocks() const = 0;  ///< slices its queue dropped (it fell behind)
    virtual std::string brief() const = 0;       ///< "12 beacons identified of 41 carriers"
};
struct NdbStart { int profileIdx = 0; double rate = 0, hwCentreHz = 0, loHz = 0, hiHz = 0, lat = NAN, lon = NAN; };

// ── ★★★ beta94 background decoding's view of the box's decoder slots ──────────────────────────────────────────────
/** ★★ NOBODY LISTENING (vibe_bg_fullcpu.h): while this radio is idle its background may hold `extraSlots` decoder slots
 *  OVER the box's limit (a WEFAX VFO that waited "for a decoder slot" runs). An ordinary claim first; only when that is
 *  refused, one under the raised limit — counted, so the runner knows to give them back when a listener comes. A listener
 *  is never refused for them: BgRegistry::makeRoomForListener asks for as many as it needs. */
class BgSlots : public DecoderSlots {
public:
    BgSlots(DecoderSlots* inner, std::shared_ptr<const fullcpu::Live> live) : in_(inner), live_(std::move(live)) {}
    int extra() const { return live_ && live_->full.load(std::memory_order_relaxed) ? std::max(0, live_->extraSlots.load(std::memory_order_relaxed)) : 0; }
    int claim() override {
        const int t = in_->claim();
        if (t >= 0) return t;
        const int x = extra();
        if (x <= 0) return -1;
        const int u = in_->claimWithin(in_->max() + x);
        if (u >= 0) { std::lock_guard<std::mutex> lk(m_); extraTok_.push_back(u); }
        return u;
    }
    void release(int token) override {
        { std::lock_guard<std::mutex> lk(m_);
          auto it = std::find(extraTok_.begin(), extraTok_.end(), token);
          if (it != extraTok_.end()) extraTok_.erase(it); }
        in_->release(token);
    }
    int max() const override { return in_->max() + extra(); }
    int inUse() const override { return in_->inUse(); }
    int claimWithin(int limit) override { return in_->claimWithin(limit); }
    /** Slots this runner took over the box's limit, still held. */
    int extraHeld() const { std::lock_guard<std::mutex> lk(m_); return (int)extraTok_.size(); }
    /** The box holds more than its limit (somebody's extras). */
    bool boxOver() const { return in_->inUse() > in_->max(); }
private:
    DecoderSlots* in_;
    std::shared_ptr<const fullcpu::Live> live_;
    mutable std::mutex m_;
    std::vector<int> extraTok_;
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
        /** ★ V12 beta90: the station store every spot goes to (the DIGI MODES MAP, PSK Reporter); null = none (tests). */
        vibestations::Store* stations = nullptr;
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
        /** ★★ V12 2026-10-27 — a BAND digital-spots VFO runs the listener's own Band governor (decoders/digi_governor.h):
         *  the owner's word per mode (Setup → Modes and decoders), the benchmark's budget in FT8 units (∞ = none) — SHARED
         *  between the profile's Band VFOs, never each given the whole box — and the server's load (engines go light).
         *  `bandWhyNot` non-empty = Band is switched off on this receiver (the owner, or the benchmark's default on a box
         *  that measured it red — Stuart's rule: the benchmark sets the default, the owner can tick it back): the VFO is
         *  shown paused with the reason, never run behind the owner's back. All may be empty (the tests). */
        std::function<int(const std::string& mode)> bandOverride;
        std::function<double()> bandBudgetUnits;
        std::function<bool()> loaded;
        std::function<std::string()> bandWhyNot;
        /** ★★ V12 2026-10-27 — AN NDB SKIMMER VFO: `ndbMake` makes its decoder (empty = this host has none: the VFO is
         *  shown not available, never run); `ndbReach` the part of the LF band this radio reaches (empty = all of it);
         *  `ndbWhyNot` non-empty = it may not run now, said on the VFO (the owner switched NDB off here; a listener's own
         *  NDB mode has the band — their skimmer keeps the gallery's NDB cards meanwhile, never two at once). */
        /** ★★ V12 beta90 — a SKIMMER VFO (kind "cw" / "rtty" / "psk", skimKindOf): non-empty = it may not run here (the
         *  owner switched that mode off, or the radio has no IQ), said on the VFO. `rttySkimWorkers` = the RTTY skimmer's
         *  search workers (the listener's setting). Both may be empty (the tests). */
        std::function<std::string(const std::string& kind)> skimWhyNot;
        std::function<int()> rttySkimWorkers;
        std::function<std::shared_ptr<NdbFeed>(const NdbStart&)> ndbMake;
        std::function<bool(double& lo, double& hi)> ndbReach;
        std::function<std::string()> ndbWhyNot;
        /** ★ The WEFAX VFOs' hourly log line's "hour" (DecoderHost::Env::wefaxHourSecs) — a test shortens it. */
        double wefaxHourSecs = 3600.0;
        /** ★★★ beta94 NOBODY LISTENING (vibe_bg_fullcpu.h): what background decoding may use now — the shim's health tick
         *  writes it; every VFO's host reads it (RTTY workers, Band cap and pause rule) and the slots take extras by it.
         *  Null = today's budget throughout (the tests, a host without the policy). */
        std::shared_ptr<fullcpu::Live> cpu;
    };
    BgRunner() = default;
    /** ★★★ beta94 THE SHUTDOWN ABORT (beta93 too; the Mac 11-25 15:27, every clean stop once background decoding had run —
     *  an apt upgrade, a restart, a settings save): the shim stops the runner itself, then `delete impl` ran this a second
     *  time, and stop() published to p_.registry — the shim's BgRegistry, ALREADY destroyed (declared after the runner).
     *  Its dead mutex threw "mutex lock failed: Invalid argument" out of a noexcept destructor: std::terminate, SIGABRT.
     *  Now: the registry is told only by a stop that stopped something; the destructor never reaches it (whoever owns
     *  the runner stops it while the registry lives); and nothing thrown leaves here. test-server-bg-stop.mjs. */
    ~BgRunner() {
        p_.registry = nullptr;
        try { stop(false); } catch (...) {}
    }

    /** Build every VFO's channel, start every decoder, start the worker. */
    void start(const Params& p) {
        stop(false);
        p_ = p;
        // ★★★ beta94: background's own view of the slots — extras over the box's limit while nobody listens
        bgSlots_.reset();
        ladder_ = std::make_shared<digi::LoadLadder>();
        { auto lg = p_.log; ladder_->setLog([lg](const std::string& l) { if (lg) lg("background decoding: " + l); }); }
        if (p_.cpu && p_.slots) { bgSlots_.reset(new BgSlots(p_.slots, p_.cpu)); p_.slots = bgSlots_.get(); }
        if (!chan_ || chan_->fftSize() != p_.fftSize) { chan_.reset(new vibedsp::Channelizer(p_.fftSize)); fedIn_ = 0; }
        chans_.clear();
        installBandSpan();
        // ★ The benchmark's Band budget is the BOX's: shared between this profile's Band VFOs, never each given the lot.
        int nBand = 0;
        for (const auto& v : p_.profile.vfos) if (isBandSpots(v)) ++nBand;
        for (size_t i = 0; i < p_.profile.vfos.size(); i++) {
            auto c = std::make_unique<Chan>();
            c->owner = this; c->vfo = p_.profile.vfos[i]; c->index = (int)i;
            // ★ A CW SKIMMER takes the channel's IQ itself — its slice just wide enough for the segment (the outer 3 % is
            //   the channel filter's), and no demodulator at all.
            c->skim = isSkimmer(c->vfo);
            /* ★★ A BAND VFO (V12 2026-10-27) is fed IQ too: the band's main slice, centred where its modes are (the span's
             *  centre, whatever frequency in the band was typed), sized and tapered exactly as a listener's Band slice
             *  (vibe_band_slice.h — flat over 7/8 of its rate), and its extra slices cut off the same forward FFT. */
            c->band = isBandSpots(c->vfo);
            BandSpan span;
            if (c->band) {
                if (bandSpan(c->vfo.hz, span)) {
                    c->bandCentre = span.centreHz; c->bandName = span.name;
                    c->bandHalf = std::max(span.centreHz - span.loHz, span.hiHz - span.centreHz);
                }
                else { c->bandCentre = c->vfo.hz; c->blockWhy = "not in a band the digital modes use"; }
                if (p_.bandWhyNot) { const std::string w = p_.bandWhyNot(); if (!w.empty()) c->blockWhy = w; }
            }
            /* ★★ AN NDB SKIMMER VFO (V12 2026-10-27) is fed IQ too: ONE slice over as much of the NDB band as the capture
             *  holds (place() works it out, and again whenever the radio moves), cut and tapered exactly as the listener's
             *  NDB tap cuts it (vibe_wide_tap.h), to the host's skimmer (Params::ndbMake). */
            c->ndb = isNdbSkim(c->vfo);
            if (c->ndb && !p_.ndbMake) c->blockWhy = "the NDB skimmer is not available on this server";
            /* ★★ A WEFAX STATIONS VFO (V12 2026-11-22): a channel per ticked frequency — its own slice, USB demodulator and
             *  WEFAX decoder (vibe_wefax_stations.h) — and ONE decoder slot for the lot (claimed by startFax). */
            c->fax = isFaxStations(c->vfo);
            if (c->fax) {
                buildFax(*c, (int)i);
                place(*c);
                chans_.push_back(std::move(c));
                continue;
            }
            // ★★ beta92 an APRS VFO's decoder reads its pipeline's DECODER TAP (±12 kHz, before the VFO's NFM filter —
            //    vibe_aprs_run.h iq1200_): its slice holds ±16 kHz flat, as a listener's does (clientRetune)
            const bool pktTap = c->vfo.dec == "aprs" && c->vfo.mode == "nfm";
            double need = c->skim ? passbandHz(c->vfo) / 0.92 : std::max(passbandHz(c->vfo) * 2.5, 24000.0);
            if (pktTap) need = std::max(need, 40000.0);
            int b = 64;
            while (b < p_.fftSize && p_.sampleRate * (double)b / (double)p_.fftSize < need) b <<= 1;
            c->bins = std::min(b, p_.fftSize);
            if (c->band) c->bins = bandSliceBins(p_.sampleRate, p_.fftSize, std::min(DecoderHostBandWidth(c->bandCentre), p_.sampleRate * 0.8));
            c->rate = p_.sampleRate * (double)c->bins / (double)p_.fftSize;
            c->slice.assign((size_t)c->bins, cf32{0.0f, 0.0f});
            if (!c->skim && !c->band && !c->ndb) {
                c->rx.reset(new vibedsp::RxPipeline());
                vibedsp::RxPipeline::Callbacks cb{};
                cb.ctx = c.get();
                cb.audio = &BgRunner::audioCb;
                if (pktTap) cb.decIq = &BgRunner::decIqCb;
                // ★ its name in its own log lines ("[pilot] …"), fixed for the VFO's life
                { const std::string lb = "background VFO " + std::to_string(i + 1) + " (" + khz(c->vfo.hz) + " " + c->vfo.mode + ")";
                  c->rx->setLogLabel([lb] { return lb; }); }
                c->rx->setBackground(true);   // ★★★ beta92: its demodulator thread takes the background rung (vibe-bgdemod)
                c->rx->start(c->rate, 1024, 10.0, 48000, cb);
                c->rx->setRdsEnabled(false);
                if (pktTap) c->rx->setDecoderTap(vibeaprs::Run::kTapHz);
            }
            place(*c);
            DecoderHost::Env env;
            env.log = p_.log;
            env.slots = p_.slots;
            env.bgCpu = p_.cpu;   // ★★★ beta94 nobody listening: RTTY workers, Band cap and pause rule follow it
            env.bandLadder = ladder_;   // ★★★ beta94 one load ladder for every band: depth drops everywhere before a mode
            // ★★ a WEFAX VFO says what it heard every hour (the decoder's evidence — "no signal" vs "missed the start")
            if (c->vfo.dec == "wefax") { env.wefaxHourly = "background VFO " + std::to_string(i + 1) + " (" + khz(c->vfo.hz) + " " + upperMode(c->vfo.mode) + ")"; env.wefaxHourSecs = p_.wefaxHourSecs; }
            // ★ A Band VFO's "dial" is its span's centre: the host sizes the slice and finds the band's extra slices by it.
            const double dialHz = c->band ? c->bandCentre : c->vfo.hz;
            env.dialHz = [dialHz] { return dialHz; };
            if (c->band) {
                /* ★★ V12 2026-11-24 EASING FOR LISTENERS (vibe_bg_ease.h): step 0 turns this band's EXTRA chips (CW / RTTY /
                 *  wide PSK — digi::iqModes) off while a listener needs the room; FT8/FT4/WSPR carry on. The owner's own
                 *  word wins: a chip he set to Always is never eased, one he set Off stays off. Read at each gate period. */
                auto ov = p_.bandOverride; auto off = c->chipsOff;
                env.bandOverride = [ov, off](const std::string& m) -> int {
                    const int own = ov ? ov(m) : 0;
                    if (own == 0 && off->load(std::memory_order_relaxed))
                        for (const auto& d : digi::iqModes()) if (m == d.name) return -1;
                    return own;
                };
                c->hasChips = !digi::iqModes().empty();
                env.loaded = p_.loaded;
                auto bu = p_.bandBudgetUnits; const int share = std::max(1, nBand);
                env.bandBudgetUnits = [bu, share] { const double u = bu ? bu() : NAN; return std::isfinite(u) && u > 0 ? u / share : u; };
            }
            // ★ An audio tone → RF by the VFO's sideband (DSC names the frequency it heard: 2185.8 USB + 1700 = 2187.5).
            const bool lsbV = c->vfo.mode == "lsb";
            env.rfOfAudio = [dialHz, lsbV](double a) { return lsbV ? dialHz - a : dialHz + a; };
            env.stations = p_.stations; env.stationOrigin = vibestations::kBackground;
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
                // ★★ …and an RTTY skimmer's (V12 beta90), labelled RTTY: the same summary a listener's RTTY skimmer keeps
                //    (meta.kind "rttyskim" — Ft8SummaryTap's keyboard-mode shape).
                env.rttySkimSpotTap = [st, v, pi, la, lo, lo2, last2, ls, lc]() -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "rttyskim"; m.origin = "background"; m.freqHz = v.hz; m.mode = v.mode; m.decoder = "rttyskim";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo2;
                    m.lastItemAt = last2; m.liveSpots = ls; m.liveCountries = lc;
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
                // ★ FT8/FT4 on its dial — or, on a Band VFO, ONE card for the whole band (decoder "ft8band", the band's
                //   centre as its frequency: the card's first line names the band, as a listener's Band card does).
                const bool bandV = c->band; const double bandC = c->bandCentre;
                env.spotTap = [st, v, pi, la, lo, lo2, last2, ls, lc, bandV, bandC](bool) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = "ft8"; m.origin = "background"; m.freqHz = bandV ? bandC : v.hz; m.mode = v.mode; m.decoder = bandV ? "ft8band" : "ft8";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo2;
                    m.lastItemAt = last2; m.liveSpots = ls; m.liveCountries = lc;
                    return std::make_shared<Ft8SummaryTap>(*st, m, la, lo);
                };
            }
            env.rttySkimWorkers = p_.rttySkimWorkers;
            /* ★★ V12 beta90: a skimmer the owner has switched off here (CW / RTTY / PSK, Setup → Modes and decoders), or
             *  that this radio cannot run (an Icom: one audio channel, no IQ), is shown paused with the reason — never
             *  run behind the owner's back (AGENTS.md: no dead control, no hidden one either). */
            if (c->skim && p_.skimWhyNot) { const std::string w = p_.skimWhyNot(skimKindOf(c->vfo.dec)); if (!w.empty()) c->blockWhy = w; }
            c->host = std::make_shared<DecoderHost>(env, "bg:" + std::to_string(i));
            chans_.push_back(std::move(c));
        }
        startedAt_ = std::time(nullptr);
        { std::lock_guard<std::mutex> ck(cpuMtx_); cpu_.clear(); }
        easeTaken_.clear(); easeCpu_.clear(); gov_.reset(); easeExcused_.store(false); easePaused_.store(0); easeChips_.store(0); easeDepth_.store(false);
        easeExhaustedSaid_ = false; vfoCount_.store((int)chans_.size());   // ★ new channels, new counters: the CPU window starts again
        listenerOnFlag_->store(p_.listenerOn);
        startDecoders();
        publish();
        run_.store(true);
        { std::lock_guard<std::mutex> hl(heldM_); held_.clear(); heldSamples_ = 0; }
        { std::lock_guard<std::mutex> lk(qm_); q_.clear(); qSamples_ = 0; gap_ = false; gapSamples_ = 0; missed_.store(0); }
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
                if (c->fax && c->decoding) syncFax(*c);   // ★ its frequencies that left the window stop, those that came in start
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
        { std::lock_guard<std::mutex> hl(heldM_); held_.clear(); heldSamples_ = 0; }
        qcv_.notify_all();
        if (th_.joinable()) th_.join();
        for (auto& c : chans_) { if (c->host) c->host->shutdown(); stopNdb(*c); if (c->fax) { stopFax(*c); for (auto& sb : c->subs) sb->host->shutdown(); } }
        chans_.clear();
        decodingNow_.store(0); degrading_.store(false);
        easeTaken_.clear(); easePaused_.store(0); easeChips_.store(0); easeDepth_.store(false);
        if (ladder_) ladder_->forceMin(false);
        if (was && p_.registry) p_.registry->publish({});   // ★ beta94 only a stop that stopped something (see ~BgRunner)
        if (was && sayIt) say("background decoding stopped");
    }
    bool running() const { return running_.load(); }
    /** The VFOs paused for listeners (or waiting for a first slot) — rank order. */
    int  decodersPaused() const { int n = 0; for (auto& c : chans_) if (!c->decoding) ++n; return n; }
    /** ★ Test seam: run the worker's slot housekeeping now (pause requests, resumes) rather than within its 0.5 s. */
    void slotTickForTest() { slotTick(true); }
    /** ★ Test seam: capture samples waiting for the worker (a test paces its feed by it rather than by sleeps). */
    size_t queuedForTest() { std::lock_guard<std::mutex> lk(qm_); return qSamples_; }
    int  decodersRunning() const { int n = 0; for (auto& c : chans_) if (c->decoding) ++n; return n; }
    uint64_t droppedSamples() const { return dropped_.load(); }
    /** ★ beta94: VFOs decoding as of the last publish (≤ 0.5 s old) — LOCK-FREE, for the health tick on another thread
     *  (decodersRunning() walks chans_, which stop() clears). 0 when stopped. */
    int decodingNow() const { return running_.load() ? decodingNow_.load() : 0; }
    uint64_t fedSamples() const { uint64_t n = 0; for (auto& c : chans_) if (c->host) n += c->host->fedSamples(); return n; }

    /** ★ THE DSP THREAD'S WHOLE COST: a copy into a bounded queue. Never waits; past half a second the oldest goes. */
    /** ★ V12 2026-10-27: `captureUtc` — when the chunk's LAST sample was captured, stamped HERE on the DSP thread (NaN =
     *  now): a Band VFO's FT8/WSPR slots are cut by it, never by when this thread got round to the block (the queue may
     *  hold half a second). A test passes its own clock. */
    /** ★★★ beta92 — AND NEVER WAITS FOR THE LOCK EITHER. The worker runs at the background rung (vibe-bgdsp, nice 19):
     *  on a box its own VFOs keep busy it can be preempted in the instant it holds qm_ to pop a chunk, and a lock_guard
     *  here then parked the DSP thread behind the lowest-priority thread in the process — priority inversion, the radio
     *  waiting on background decoding. So the DSP thread TRY-locks: busy = this chunk is not taken (counted in dropped_
     *  and busyDrops_, and bridged as a hole before the next chunk that is, exactly like an oldest-dropped one). Background
     *  decoding loses 32 ms; the listener loses nothing. */
    /* ★★★ b94 (11-25) — …BUT NEVER DROPS FOR IT EITHER. A busy queue used to cost the chunk, and on the Pi 500's RSP1B
     *  (8 MS/s, a USB packet of ~1–2 k samples per DSP-thread chunk) the worker pops so often that the try-lock missed
     *  ~0.3–0.4 s of IQ a minute as HUNDREDS of tiny holes. Each hole is a phase step in every Band slice (the channeliser
     *  is fed the two sides back to back) and its bridge lands up to a hop early; a few a second smear a tone by ~10 Hz —
     *  FT8 / JS8 (6.25 Hz tones) and WSPR (1.46 Hz) decoded nothing on 40 m and 30 m from the beta92 install on, FT4 (21 Hz)
     *  and FT2 kept going (replayed: the same 40 m IQ, 1 ms chunks, 1.5 s/min of busy misses: FT8 148 spots → 0).
     *  Now a chunk the DSP thread cannot hand over is HELD on its side (heldM_ is the DSP thread's own — nobody else takes
     *  it but start/stop) and goes in, in order, ahead of the next chunk that finds the queue free. Still never waits on
     *  the worker. Bounded: past kHeldSec the oldest held chunk goes (counted in busyDrops_ and dropped_, bridged as
     *  before) — a worker preempted for that long is falling behind anyway, and the queue's own cap then decides. */
    static constexpr double kHeldSec = 0.25;
    void feedIq(const cf32* iq, int n, double captureUtc = NAN) {
        if (!running_.load(std::memory_order_relaxed) || n <= 0) return;
        QItem it; it.v.assign(iq, iq + n);
        it.utc = std::isfinite(captureUtc) ? captureUtc : vibeUtcNow();
        std::lock_guard<std::mutex> hl(heldM_);   // ★ the DSP thread's own (uncontended but for start/stop)
        {
            std::unique_lock<std::mutex> lk(qm_, std::try_to_lock);
            if (!lk.owns_lock()) {
                busyHeld_.fetch_add(1, std::memory_order_relaxed);
                heldSamples_ += (size_t)n;
                held_.push_back(std::move(it));
                const size_t hcap = (size_t)std::max(16384.0, p_.sampleRate * kHeldSec);
                while (heldSamples_ > hcap && held_.size() > 1) {   // ★ the oldest held goes: a hole, bridged
                    const size_t k = held_.front().v.size();
                    heldSamples_ -= k;
                    missed_.fetch_add(k + held_.front().missBefore, std::memory_order_relaxed);
                    dropped_.fetch_add((uint64_t)k, std::memory_order_relaxed);
                    busyDrops_.fetch_add(1, std::memory_order_relaxed);
                    held_.pop_front();
                }
                return;
            }
            held_.push_back(std::move(it));
            for (QItem& h : held_) {
                h.missBefore += missed_.exchange(0, std::memory_order_relaxed);   // ★ the hole sits just before THIS chunk
                qSamples_ += h.v.size();
                q_.push_back(std::move(h));
            }
            held_.clear(); heldSamples_ = 0;
            const size_t cap = (size_t)std::max(65536.0, p_.sampleRate * 0.5);
            while (qSamples_ > cap && q_.size() > 1) {
                qSamples_ -= q_.front().v.size();
                dropped_.fetch_add(q_.front().v.size(), std::memory_order_relaxed);
                gapSamples_ += q_.front().v.size() + q_.front().missBefore;   // ★ the oldest go — so the hole is just before the new front
                q_.pop_front();
                gap_ = true;
            }
        }
        qcv_.notify_one();
    }
    /** ★ Test seam (beta92): hold the worker's queue lock, as the worker does for an instant when it pops a chunk. */
    std::unique_lock<std::mutex> holdQueueForTest() { return std::unique_lock<std::mutex>(qm_); }
    /** ★ beta92: chunks the DSP thread did not hand over because the worker held the queue at that instant (test seam). */
    uint64_t busyDrops() const { return busyDrops_.load(std::memory_order_relaxed); }
    /** ★ b94: chunks the DSP thread found the queue busy for and HELD (handed over with the next one) — test seam, admin. */
    uint64_t busyHeld() const { return busyHeld_.load(std::memory_order_relaxed); }
    /** ★ b94 test seam: chunks held on the DSP thread's side now. */
    size_t heldForTest() { std::lock_guard<std::mutex> hl(heldM_); return held_.size(); }

    // ── ★★★ V12 2026-11-24 EASING OFF FOR LISTENERS (vibe_bg_ease.h, docs/v12/BG-EASE.md) ─────────────────────────────
    /** One step in effect: which VFO, what was done, and what it cost when taken (% of one core, -1 unknown). */
    struct EaseStep { int vfo = 0; ease::Kind kind = ease::Kind::Pause; double costPct = -1; };
    /** What the shim measured, handed in every ~2 s (the health tick). */
    struct EaseInputs {
        int64_t  nowMs = 0;              ///< a steady clock, ms (a test passes its own)
        uint64_t listenerIqDrops = 0;    ///< monotonic: the listener's IQ buffers dropped (the DSP thread fell behind)
        double   listenerLag = -1;       ///< the listener's IQ queue at its fullest since the last tick, 0..1 (-1 unknown)
        double   machineBusyPct = -1;    ///< the whole machine 0..100 (-1 unknown — Android)
        int      cores = 0;
        double   listenerHz = NAN;       ///< the listener's dial — chips and skimmers furthest from it go first (NaN = centre)
    };
    /** ★★ ONE TICK OF THE GOVERNOR: at most one step taken or given back, said in the log in plain words. Never touches a
     *  listener's own decoders (it only ever reaches this runner's VFOs). Takes the slot lock and the channel lock, as
     *  retarget() does — the worker is between blocks when it runs. */
    void easeTick(const EaseInputs& in) {
        if (!running_.load()) return;
        std::lock_guard<std::mutex> sl(slotMtx_);
        std::lock_guard<std::mutex> cl(chanMtx_);
        const auto now = std::chrono::steady_clock::now();
        easeCpu_.push_back({ now, vfoCpuNsLocked() });
        while (easeCpu_.size() > 2 && now - easeCpu_[1].t >= std::chrono::seconds(20)) easeCpu_.pop_front();
        while (easeCpu_.size() > 16) easeCpu_.pop_front();
        // ★ what a VFO costs now, measured (its threads' own CPU clocks over the last ~20 s), % of one core; -1 unknown
        auto costOf = [&](int i) -> double {
            if (easeCpu_.size() < 2) return -1;
            const auto& a = easeCpu_.front(); const auto& b = easeCpu_.back();
            if (a.ns.size() != b.ns.size() || i < 0 || (size_t)i >= b.ns.size()) return -1;
            const double w = std::chrono::duration<double>(b.t - a.t).count();
            if (w < 1.0) return -1;
            return 100.0 * (double)(b.ns[(size_t)i] >= a.ns[(size_t)i] ? b.ns[(size_t)i] - a.ns[(size_t)i] : 0) / (w * 1e9);
        };
        const double ref = std::isfinite(in.listenerHz) ? in.listenerHz : p_.centreHz;
        std::vector<ease::Candidate> cands;
        for (auto& c : chans_) {
            if (c->eased || !c->inWindow || !c->blockWhy.empty() || !c->decoding) continue;
            const double at = c->band ? c->bandCentre
                            : c->fax ? (c->subs.empty() ? p_.centreHz : c->subs.front()->centreHz)
                            : c->ndb ? 0.5 * (c->ndbLo + c->ndbHi) : c->vfo.hz;
            ease::Candidate k;
            k.vfo = c->index; k.distHz = std::fabs(at - ref); k.rank = c->index + 1; k.busy = busy(*c);
            if (c->band && c->hasChips && !c->chipsOff->load()) { k.kind = ease::Kind::Chips; k.tier = ease::kTierChips; cands.push_back(k); }
            k.kind = ease::Kind::Pause; k.tier = ease::tierOf(c->vfo.dec);
            cands.push_back(k);
        }
        /* ★★★ beta94: the FIRST step for a listener — decode depth to the minimum on every band (the load ladder), before
         *  any band loses its extra modes. Offered while a Band VFO decodes and the depth is not already forced. */
        if (!ladder_->forced())
            for (auto& c : chans_)
                if (c->band && c->decoding && !c->eased && c->inWindow) {
                    ease::Candidate k; k.vfo = c->index; k.kind = ease::Kind::Depth; k.tier = ease::kTierDepth; k.rank = 0;
                    cands.push_back(k);
                    break;
                }
        bool canShed = false, canSafety = false;
        for (auto& k : cands) { if (k.tier == ease::kTierSafety) canSafety = true; else canShed = true; }
        ease::Signals sg;
        sg.nowMs = in.nowMs; sg.listenerOn = p_.listenerOn;
        sg.bgDroppedSec = p_.sampleRate > 0 ? (double)dropped_.load(std::memory_order_relaxed) / p_.sampleRate : 0.0;
        sg.listenerIqDrops = in.listenerIqDrops; sg.listenerLag = in.listenerLag;
        sg.machineBusyPct = in.machineBusyPct; sg.cores = in.cores;
        const ease::Act a = gov_.tick(sg, (int)easeTaken_.size(), canShed, canSafety,
                                      easeTaken_.empty() ? -1.0 : easeTaken_.back().costPct);
        const std::string n = std::to_string(easeTaken_.size() + 1);
        if (a == ease::Act::Shed || a == ease::Act::ShedSafety) {
            const int k = ease::pickNext(cands, a == ease::Act::ShedSafety);
            if (k >= 0) {
                Chan& c = *chans_[(size_t)cands[(size_t)k].vfo];
                EaseStep st{ c.index, cands[(size_t)k].kind, costOf(c.index) };
                char cost[96] = "";
                if (st.costPct >= 0) std::snprintf(cost, sizeof cost, " (it was costing %.1f %% of one core)", st.costPct);
                if (st.kind == ease::Kind::Depth) {
                    ladder_->forceMin(true);
                    st.costPct = -1;
                    say("background decoding: eased for listeners, step " + n + " \xe2\x80\x94 decode depth to the minimum on every band"
                        " (one pass, fewer candidates, stricter sync; all bands keep decoding) \xe2\x80\x94 " + gov_.why());
                } else if (st.kind == ease::Kind::Chips) {
                    c.chipsOff->store(true);
                    say("background decoding: eased for listeners, step " + n + " \xe2\x80\x94 VFO " + name(c)
                        + ": its extra modes (CW, RTTY, wide PSK) off, FT8/FT4/WSPR carry on" + cost + " \xe2\x80\x94 " + gov_.why());
                } else {
                    pauseOne(c, "eased for listeners", false);
                    c.eased = true;
                    say("background decoding: eased for listeners, step " + n + " \xe2\x80\x94 VFO " + name(c) + " paused" + cost
                        + (cands[(size_t)k].tier == ease::kTierSafety ? " \xe2\x80\x94 a distress watch, paused LAST" : "")
                        + " \xe2\x80\x94 " + gov_.why());
                }
                easeTaken_.push_back(st);
            }
        } else if (a == ease::Act::Restore && !easeTaken_.empty()) {
            const EaseStep st = easeTaken_.back();
            easeTaken_.pop_back();
            if (st.kind == ease::Kind::Depth)
                say("background decoding: eased step " + std::to_string(easeTaken_.size() + 1) + " given back \xe2\x80\x94 decode depth "
                    "back to the ladder's own (" + std::string(digi::LoadLadder::name(ladder_->learnt())) + ") \xe2\x80\x94 " + gov_.why());
            else
            say("background decoding: eased step " + std::to_string(easeTaken_.size() + 1) + " given back \xe2\x80\x94 VFO "
                + name(*chans_[(size_t)st.vfo]) + (st.kind == ease::Kind::Chips ? ": its extra modes back on" : ": resuming") + " \xe2\x80\x94 " + gov_.why());
            undoEase(st);
        } else if (a == ease::Act::RestoreAll && !easeTaken_.empty()) {
            const size_t k = easeTaken_.size();
            while (!easeTaken_.empty()) { undoEase(easeTaken_.back()); easeTaken_.pop_back(); }
            say("background decoding: " + gov_.why() + " \xe2\x80\x94 all " + std::to_string(k) + " eased step(s) given back");
        }
        int paused = 0, chips = 0;
        for (auto& c : chans_) { if (c->eased) ++paused; if (c->chipsOff->load()) ++chips; }
        easePaused_.store(paused); easeChips_.store(chips); easeDepth_.store(ladder_->forced());
        const bool exh = p_.listenerOn && gov_.exhausted();
        if (exh && !easeExhaustedSaid_)
            say("background decoding: TOO HEAVY even eased \xe2\x80\x94 " + gov_.why() + "; nothing left to ease "
                + (canSafety ? "but the distress watches (DSC / NAVTEX), which are never paused for background decoding's own sake"
                             : "(every background VFO is paused or eased)"));
        easeExhaustedSaid_ = exh;
        easeExcused_.store(p_.listenerOn && !exh);
        degrading_.store(p_.listenerOn && gov_.listenerHarm());   // ★ beta94: the pill's only case (ease::PillWatch)
        publish();
    }
    /** ★ For the health card: a listener is on and background decoding's drops are the planned give-way (not a fault). */
    bool dropsExcused() const { return easeExcused_.load(); }
    /** ★★ beta94: a listener on and THEIR receiver falling behind (their IQ dropped, or their queue past 60 % twice in 10 s)
     *  while background decoding runs — its load is what they feel (sluggish, audio breaking up) until the easing has
     *  shed enough. The health pill's only reason to mention it (vibe_bg_ease.h PillWatch). */
    bool degradingListener() const { return running_.load() && degrading_.load(); }

    /** ★ The health card's line while eased, "" when not: "eased for listeners (3 of 9 paused)". Fixed words, no user text. */
    std::string easeNote() const {
        const int pz = easePaused_.load(), ch = easeChips_.load();
        const bool dp = easeDepth_.load();
        if (pz <= 0 && ch <= 0 && !dp) return std::string();
        std::string t = "eased for listeners (";
        if (dp) t += std::string("decode depth at the minimum") + (pz > 0 || ch > 0 ? ", " : "");
        if (pz > 0) t += std::to_string(pz) + " of " + std::to_string(vfoCount_.load()) + " paused";
        if (ch > 0) t += std::string(pz > 0 ? ", " : "") + "extra modes off on " + std::to_string(ch) + " band" + (ch == 1 ? "" : "s");
        return t + ")";
    }
    /** ★ beta94 test seam: the profile's load ladder (every Band VFO's). */
    std::shared_ptr<digi::LoadLadder> ladderForTest() const { return ladder_; }
    int easeStepsForTest() { std::lock_guard<std::mutex> sl(slotMtx_); return (int)easeTaken_.size(); }
    bool vfoEasedForTest(int i) { std::lock_guard<std::mutex> sl(slotMtx_); return i >= 0 && (size_t)i < chans_.size() && chans_[(size_t)i]->eased; }
    bool chipsOffForTest(int i) { std::lock_guard<std::mutex> sl(slotMtx_); return i >= 0 && (size_t)i < chans_.size() && chans_[(size_t)i]->chipsOff->load(); }
    bool vfoDecodingForTest(int i) { std::lock_guard<std::mutex> sl(slotMtx_); return i >= 0 && (size_t)i < chans_.size() && chans_[(size_t)i]->decoding; }

    /** ★★ V12 beta 4 — THE ADMIN PAGE'S LINE (admin only; LocalSdrShim::adminStatusJson). Per VFO: running / paused (and
     *  why) / out of the radio's window, the decoder's lock, the last item written, and its CPU as % of ONE core,
     *  averaged over ~10 s. ★ Measured, never estimated: every figure is a thread reading its own CPU clock around its own
     *  work (vibeThreadCpuNs) — the worker around the shared FFT and each VFO's cut + demodulator, each decode thread
     *  around its decoder — so the counters cost two clock reads per block and nothing samples anything until the page
     *  asks (it polls every 2 s while open). The channeliser's FFT serves every VFO at once: it is reported as
     *  "channels", never shared out by guesswork. "cpuPct": null until two samples ≥ 1 s apart exist. */
    /** ★ V12 2026-11-24: `extra` — more fields for the runner's object, from the caller (the shim's per-thread figures). */
    std::string statusJson(const std::string& extra = std::string()) {
        std::lock_guard<std::mutex> cl(chanMtx_);
        std::lock_guard<std::mutex> ck(cpuMtx_);
        const auto now = std::chrono::steady_clock::now();
        CpuSample cur{ now, sharedNs_.load(std::memory_order_relaxed), vfoCpuNsLocked() };
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
            auto& c = *chans_[i];
            const DecoderDef* d = decoderDef(c.vfo.dec);
            const auto nf = c.ndb ? c.feed() : nullptr;
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
               // ★ V12 2026-11-24: paused or its extra modes off FOR LISTENERS (easing — not a fault)
               + (c.eased ? ",\"eased\":true" : "") + (c.chipsOff->load() ? ",\"chipsOff\":true" : "")
               // ★ V12: a Band VFO names its band and what its governor runs now ("FT8 7.074, FT4 7.0475 …")
               + (c.band ? ",\"band\":\"" + gallery::jsonEsc(c.bandName) + "\",\"modes\":\"" + gallery::jsonEsc(c.decoding && c.host ? c.host->spotsDescribe() : std::string()) + "\"" : std::string())
               // ★ V12: an NDB skimmer names the part of the band it covers and how it is doing ("12 beacons identified of 41 carriers")
               + (c.ndb ? ",\"ndbLo\":" + std::to_string((long long)std::llround(c.ndbLo)) + ",\"ndbHi\":" + std::to_string((long long)std::llround(c.ndbHi))
                          + ",\"ndb\":\"" + gallery::jsonEsc(nf ? nf->brief() : std::string()) + "\"" : std::string())
               // ★ V12 2026-11-22: a WEFAX stations VFO lists its frequencies — decoding, waiting (out of the window) or mid-chart
               + (c.fax ? ",\"fax\":" + faxJson(c) : std::string())
               // ★ FT8/FT4: the summary's running count, so a quiet card is not read as "no decodes"
               + (d && !c.ndb && (!*d->ext || d->widthHz > 0) ?",\"spots\":" + std::to_string((unsigned long long)c.liveSpots->load(std::memory_order_relaxed))
                                + ",\"countries\":" + std::to_string((unsigned long long)c.liveCountries->load(std::memory_order_relaxed)) : std::string())
               + "}";
        }
        j += "],\"cpuPct\":" + pct(sumNow, sumBase);
        j += ",\"ease\":{\"paused\":" + std::to_string(easePaused_.load()) + ",\"chips\":" + std::to_string(easeChips_.load())
           + ",\"excused\":" + (easeExcused_.load() ? "true" : "false") + ",\"note\":\"" + gallery::jsonEsc(easeNote()) + "\"}";
        // ★★★ beta94 the load ladder: how deep the Band VFOs decode now (0 full … 5 the minimum)
        if (ladder_) j += ",\"depth\":" + std::to_string(ladder_->depth()) + ",\"depthName\":\"" + digi::LoadLadder::name(ladder_->depth()) + "\"";
        // ★★★ beta94 nobody listening: what background decoding may use now (absent = today's budget)
        if (p_.cpu && p_.cpu->full.load()) {
            const int c = p_.cpu->cores.load();
            j += ",\"fullCpu\":{\"cores\":" + std::to_string(c) + ",\"rttyWorkers\":" + std::to_string(p_.cpu->rttyWorkers.load())
               + ",\"extraSlots\":" + std::to_string(p_.cpu->extraSlots.load()) + "}";
        }
        return j + extra + "}";
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
        // ★ V12 2026-10-27: a Band digital-spots VFO — its span's centre (where the main slice is cut), the band's name,
        //   why it cannot run here ("" = it can), the slice's bins' power (Band's activity gate) and its extra slices.
        bool band = false;
        double bandCentre = 0, bandHalf = 0;   // ★ the span's centre and half its width (its modes' edges)
        std::string bandName, blockWhy;
        std::vector<float> pw;
        struct Extra { std::vector<cf32> slice; std::vector<float> pw; vibedsp::Channelizer::ExtractCtx ctx; double gapCarry = 0; };
        Extra bx[kMaxExtraSlices];
        double gapCarry = 0;
        // ★ V12 2026-10-27: an NDB skimmer VFO — the part of the band its slice covers now, its decoder (made on start,
        //   stopped on pause — under ndbM, as the admin page reads it from another thread), and the decoder slot it holds.
        bool ndb = false;
        double ndbLo = 0, ndbHi = 0;
        std::mutex ndbM;
        std::shared_ptr<NdbFeed> ndbFeed;
        int ndbSlot = -1;
        uint64_t ndbCpuDone = 0, ndbDropDone = 0, ndbLastDrop = 0;   // ★ stopped feeds' CPU and drops, so the counters only rise
        std::shared_ptr<NdbFeed> feed() { std::lock_guard<std::mutex> lk(ndbM); return ndbFeed; }
        // ★★ V12 2026-11-22: a WEFAX stations VFO — a channel per ticked frequency, and the one decoder slot it holds.
        struct FaxSub {
            double centreHz = 0, dialHz = 0;     // ★ the station's assigned (centre) frequency, and its USB dial 1.9 kHz below
            std::string label;                   // "Northwood GYA 4610.0 kHz"
            int bins = 0, centreBin = 0; double rate = 0;
            std::vector<cf32> slice;
            vibedsp::Channelizer::ExtractCtx ectx;
            std::unique_ptr<vibedsp::RxPipeline> rx;
            std::shared_ptr<DecoderHost> host;
            bool inWindow = false, running = false;
        };
        bool fax = false;
        std::vector<std::unique_ptr<FaxSub>> subs;
        int faxSlot = -1, faxRunning = -1;
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
        // ★★ V12 2026-11-24 EASING (vibe_bg_ease.h): paused for listeners (comes back only by easeTick), and a Band VFO's
        //    extra chips off (read by its bandOverride wrapper on the decode thread — hence shared and atomic).
        bool eased = false;
        bool hasChips = false;
        std::shared_ptr<std::atomic<bool>> chipsOff = std::make_shared<std::atomic<bool>>(false);
    };
    /** Cut this VFO's channel against the current centre, and say whether it is inside the window at all. */
    void place(Chan& c) {
        const double binHz = p_.sampleRate / (double)p_.fftSize;
        if (c.ndb) { placeNdb(c, binHz); return; }
        if (c.fax) { placeFax(c, binHz); return; }
        const double at = c.band ? c.bandCentre : c.vfo.hz;
        const double off = at - p_.centreHz - p_.hwOffsetHz;
        c.inWindow = std::fabs(at - p_.centreHz) <= halfWindowHz(p_.sampleRate);
        // ★ A Band VFO is in the window only when its whole main slice is (the same guard the save checks)
        if (c.band) c.inWindow = std::fabs(at - p_.centreHz) + c.bandHalf <= halfWindowHz(p_.sampleRate) + 3000.0;
        c.centreBin = (int)std::lround(off / binHz);
        const double resid = off - c.centreBin * binHz;
        // ★ A skimmer's slice is centred where its centre bin is: the VFO less the residual (its spots are RF anyway).
        c.sliceRf = p_.centreHz + p_.hwOffsetHz + c.centreBin * binHz;
        if (!c.rx) return;
        c.rx->setTune(resid, c.vfo.mode == "lsb" ? vibedsp::RxPipeline::Mode::SSB_LSB
                           : c.vfo.mode == "nfm" ? vibedsp::RxPipeline::Mode::NFM : vibedsp::RxPipeline::Mode::SSB_USB,
                      passbandHz(c.vfo));
    }
    /** ★★ An NDB skimmer VFO against the current centre: the part of 190–535 kHz the capture holds (ndbSegment — the same
     *  rule the save checks, clipped to what the radio reaches), and a slice over it sized and centred as the listener's
     *  NDB tap cuts one (the segment + 2.6 kHz, over 0.92 of the slice's rate, a power-of-two of bins). Under 100 kHz of
     *  the band in the window = out of the window, paused like any VFO a listener's dial left behind. */
    void placeNdb(Chan& c, double binHz) {
        double rl = 0, rh = 0;
        if (p_.ndbReach && !p_.ndbReach(rl, rh)) { rl = 1e12; rh = 0; }   // ★ reaches none of it
        double lo = 0, hi = 0;
        const bool ok = ndbSegment(p_.centreHz, p_.sampleRate, lo, hi, rl, rh);
        c.inWindow = ok;
        if (!ok) return;
        const double width = hi - lo + 2600.0;
        int b = 64;
        while (b < p_.fftSize && p_.sampleRate * (double)b / (double)p_.fftSize < width / 0.92) b <<= 1;
        b = std::min(b, p_.fftSize);
        if (b != c.bins) { c.bins = b; c.slice.assign((size_t)b, cf32{0.0f, 0.0f}); c.ectx = vibedsp::Channelizer::ExtractCtx(); }
        c.rate = p_.sampleRate * (double)c.bins / (double)p_.fftSize;
        c.centreBin = (int)std::lround((0.5 * (lo + hi) - p_.centreHz - p_.hwOffsetHz) / binHz);
        c.sliceRf = p_.centreHz + p_.hwOffsetHz + c.centreBin * binHz;
        const bool moved = std::fabs(lo - c.ndbLo) > 1 || std::fabs(hi - c.ndbHi) > 1;
        c.ndbLo = lo; c.ndbHi = hi;
        if (moved) if (auto f = c.feed()) f->aim(p_.centreHz + p_.hwOffsetHz, lo, hi);
    }
    /** ★★ A WEFAX stations VFO's channels (V12 2026-11-22): one per ticked frequency, each exactly what a single WEFAX VFO
     *  is — its slice off the shared FFT, a USB demodulator at 48 kHz, a DecoderHost running "wefax" with the background
     *  options — but holding NO slot of its own (the VFO holds one for all) and keeping no joiner's replay (noReplay). Each
     *  chart goes to the gallery as its own item, titled by station and frequency, filed at that frequency's dial. */
    void buildFax(Chan& c, int i) {
        for (double f : c.vfo.fax) {
            if (c.subs.size() >= kMaxFaxFreqs) break;
            auto sb = std::make_unique<Chan::FaxSub>();
            sb->centreHz = f; sb->dialHz = wefaxst::dialOf(f); sb->label = wefaxst::label(f);
            const double need = std::max(2700.0 * 2.5, 24000.0);
            int b = 64;
            while (b < p_.fftSize && p_.sampleRate * (double)b / (double)p_.fftSize < need) b <<= 1;
            sb->bins = std::min(b, p_.fftSize);
            sb->rate = p_.sampleRate * (double)sb->bins / (double)p_.fftSize;
            sb->slice.assign((size_t)sb->bins, cf32{0.0f, 0.0f});
            sb->rx.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks cb{};
            cb.ctx = sb.get();
            cb.audio = &BgRunner::faxAudioCb;
            { const std::string lb = "background VFO " + std::to_string(i + 1) + " (" + sb->label + ")";
              sb->rx->setLogLabel([lb] { return lb; }); }
            sb->rx->setBackground(true);     // ★★★ beta92: the background rung, as every background VFO's
            sb->rx->start(sb->rate, 1024, 10.0, 48000, cb);
            sb->rx->setRdsEnabled(false);
            DecoderHost::Env env;
            env.log = p_.log;
            env.slots = nullptr;                 // ★ the VFO's one slot covers every channel
            env.noReplay = true;
            const double dial = sb->dialHz;
            env.dialHz = [dial] { return dial; };
            env.rfOfAudio = [dial](double a) { return dial + a; };
            env.wefaxHourly = "background VFO " + std::to_string(i + 1) + " (" + sb->label + ")";
            env.wefaxHourSecs = p_.wefaxHourSecs;
            if (p_.store) {
                gallery::Store* st = p_.store;
                const int pi = p_.profileIdx;
                auto lo_ = listenerOnFlag_;
                const double la0 = p_.lat, lo0 = p_.lon;
                auto last = c.lastItem;
                const std::string station = sb->label;
                env.tap = [st, dial, pi, lo_, la0, lo0, last, station](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
                    gallery::Meta m;
                    m.kind = galleryKindFor(ext, msg);
                    if (m.kind.empty()) return nullptr;
                    m.origin = "background"; m.freqHz = dial; m.mode = "usb"; m.decoder = "wefax";
                    m.profile = "Profile " + std::to_string(pi + 1);
                    m.listenerOn = lo_; m.lat = la0; m.lon = lo0; m.lastItemAt = last;
                    m.station = station;
                    return makeGalleryTap(*st, m);
                };
            }
            sb->host = std::make_shared<DecoderHost>(env, "bg:" + std::to_string(i) + ":" + wefaxst::khzText(f));
            c.subs.push_back(std::move(sb));
        }
        if (c.subs.empty()) c.blockWhy = "no WEFAX station frequency is ticked";
    }
    static void faxAudioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* sb = (Chan::FaxSub*)ctx;
        if (sb && sb->host && frames > 0) sb->host->feed(pcm, frames, ch == 2 ? 2 : 1);
    }
    /** Cut each channel against the current centre; the VFO is in the window when any of its frequencies is. */
    void placeFax(Chan& c, double binHz) {
        bool any = false;
        for (auto& sb : c.subs) {
            sb->inWindow = faxInWindow(sb->centreHz, p_.centreHz, p_.sampleRate);
            const double off = sb->dialHz - p_.centreHz - p_.hwOffsetHz;
            sb->centreBin = (int)std::lround(off / binHz);
            const double resid = off - sb->centreBin * binHz;
            sb->rx->setTune(resid, vibedsp::RxPipeline::Mode::SSB_USB, 2700.0);
            any = any || sb->inWindow;
        }
        c.inWindow = any;
    }
    /** ★ Start a WEFAX stations VFO: ONE decoder slot, then a decoder on every ticked frequency inside the window. */
    void startFax(Chan& c) {
        c.decoding = false;
        const int tok = p_.slots ? p_.slots->claim() : 0;
        if (tok < 0) { c.pauseWhy = "waiting for a decoder slot (listeners come first)"; return; }
        c.faxSlot = p_.slots ? tok : -1;
        c.decoding = true;
        c.pauseWhy.clear();
        syncFax(c);
    }
    /** Each channel running exactly when the VFO runs and its frequency is inside the window. */
    void syncFax(Chan& c) {
        int n = 0;
        for (auto& sb : c.subs) {
            const bool want = c.decoding && sb->inWindow;
            if (want && !sb->running) {
                sb->rx->noteInputGap();
                const auto r = sb->host->start("wefax", attachMsg(c.vfo));
                sb->running = r == DecoderHost::Start::Ok || r == DecoderHost::Start::Joined;
            } else if (!want && sb->running) { sb->host->stop(); sb->running = false; }
            if (sb->running) ++n;
        }
        if (c.decoding && n != c.faxRunning) {
            c.faxRunning = n;
            say("background decoding: VFO " + name(c) + " \xe2\x80\x94 decoding " + std::to_string(n) + " of " + std::to_string(c.subs.size())
                + " WEFAX frequencies (the rest are outside the radio's window)");
        }
    }
    /** Stop every channel (each tap writes its chart so far, partial) and give the slot back. */
    void stopFax(Chan& c) {
        for (auto& sb : c.subs) if (sb->running) { sb->host->stop(); sb->running = false; }
        c.faxRunning = -1;
        if (c.faxSlot >= 0) { if (p_.slots) p_.slots->release(c.faxSlot); c.faxSlot = -1; }
    }
    std::string faxJson(const Chan& c) const {
        std::string j = "[";
        for (size_t k = 0; k < c.subs.size(); k++) {
            const auto& sb = *c.subs[k];
            char hz[32]; std::snprintf(hz, sizeof hz, "%.0f", sb.centreHz);
            j += std::string(k ? "," : "") + "{\"hz\":" + hz + ",\"label\":\"" + gallery::jsonEsc(sb.label) + "\",\"state\":\""
               + (sb.running ? (sb.host->midPicture() ? "chart" : "running") : sb.inWindow ? "paused" : "out") + "\"}";
        }
        return j + "]";
    }
    /** ★ Start an NDB skimmer VFO: a decoder slot, then its decoder (Params::ndbMake). */
    void startNdb(Chan& c) {
        c.decoding = false;
        const std::string w = p_.ndbWhyNot ? p_.ndbWhyNot() : std::string();
        if (!w.empty()) { c.pauseWhy = w; return; }
        const int tok = p_.slots ? p_.slots->claim() : 0;
        if (tok < 0) { c.pauseWhy = "waiting for a decoder slot (listeners come first)"; return; }
        NdbStart s;
        s.profileIdx = p_.profileIdx; s.rate = p_.sampleRate; s.hwCentreHz = p_.centreHz + p_.hwOffsetHz;
        s.loHz = c.ndbLo; s.hiHz = c.ndbHi; s.lat = p_.lat; s.lon = p_.lon;
        std::shared_ptr<NdbFeed> f;
        try { f = p_.ndbMake(s); } catch (const std::exception& ex) { say(std::string("background decoding: the NDB skimmer could not start: ") + ex.what()); }
        if (!f) { if (p_.slots) p_.slots->release(tok); c.pauseWhy = "the NDB skimmer could not start"; return; }
        { std::lock_guard<std::mutex> lk(c.ndbM); c.ndbFeed = std::move(f); }
        c.ndbSlot = tok;
        c.decoding = true;
        c.pauseWhy.clear();
    }
    /** ★ Stop it: its decoder writes the gallery's cards (the last 10 minutes "to HH:MM", the hour so far), the slot back. */
    void stopNdb(Chan& c) {
        std::shared_ptr<NdbFeed> f;
        { std::lock_guard<std::mutex> lk(c.ndbM); f = std::move(c.ndbFeed); c.ndbFeed.reset(); }
        if (f) {
            f->stop();
            std::lock_guard<std::mutex> lk(c.ndbM);
            c.ndbCpuDone += f->cpuNs(); c.ndbDropDone += f->droppedBlocks();
        }
        if (c.ndbSlot >= 0) { if (p_.slots) p_.slots->release(c.ndbSlot); c.ndbSlot = -1; }
    }
    /** ★ The channels the budget allows now (a locked radio's free user slots); a large number when there is none. */
    int budget() const { return p_.channelBudget ? std::max(0, p_.channelBudget()) : 1 << 20; }
    int runningCount() const { int n = 0; for (auto& c : chans_) if (c->decoding) ++n; return n; }
    /** ★★ beta92 an APRS VFO's decoder tap → its host's packet decoder (DecoderHost::feedChannelIq, wide). */
    static void decIqCb(void* ctx, const cf32* x, int n, double rate) {
        auto* c = (Chan*)ctx;
        if (c && c->host) c->host->feedChannelIq(reinterpret_cast<const std::complex<float>*>(x), n, rate, true);
    }
    static void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* c = (Chan*)ctx;
        testBurn(frames);
        if (c && c->host && frames > 0) c->host->feed(pcm, frames, ch == 2 ? 2 : 1);
    }
    /** ★ TEST-ONLY (scripts/test-server-iq-priority.mjs, beta92): VIBESERVER_BG_TEST_BURN=k makes every background VFO's
     *  demodulator thread (vibe-bgdemod — this callback's thread) spin k × each audio block's duration, so a profile
     *  SATURATES the cores the way a busy one does on a Pi — the condition under which the listener's chain and raw IQ
     *  out must still keep real time. Unset (every real server): one relaxed branch. */
    static void testBurn(int frames) {
        static const double k = [] { const char* e = std::getenv("VIBESERVER_BG_TEST_BURN"); return e ? std::atof(e) : 0.0; }();
        if (!(k > 0.0) || frames <= 0) return;
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(k * frames / 48000.0);
        volatile double x = 0.0;
        while (std::chrono::steady_clock::now() < until) x = x + 1.0;
    }
    /** ★ From the top of the list down: background takes any free slots, in its own priority order. */
    void startDecoders() {
        started_ = 0;
        for (auto& c : chans_) {
            if (!c->inWindow) continue;
            // ★ A Band VFO the owner (or the benchmark's default) has switched off here: said once, never started
            if (!c->blockWhy.empty()) { c->pauseWhy = c->blockWhy; say("background decoding: VFO " + name(*c) + " not started \xe2\x80\x94 " + c->blockWhy); continue; }
            if (runningCount() >= budget()) { c->pauseWhy = "waiting \xe2\x80\x94 every user slot of this radio is taken (listeners come first)"; say("background decoding: VFO " + name(*c) + " waiting \xe2\x80\x94 every user slot of this radio is taken (listeners come first)"); continue; }
            startOne(*c);
            if (c->decoding) ++started_;
            else if (c->ndb && !c->pauseWhy.empty() && c->pauseWhy.find("decoder slot") == std::string::npos)
                say("background decoding: VFO " + name(*c) + " waiting \xe2\x80\x94 " + c->pauseWhy);
            else say("background decoding: VFO " + name(*c) + " waiting \xe2\x80\x94 no decoder slot free (listeners come first)");
        }
    }
    void startOne(Chan& c) {
        const DecoderDef* d = decoderDef(c.vfo.dec);
        if (c.ndb) { if (c.blockWhy.empty()) startNdb(c); return; }
        if (c.fax) { if (c.blockWhy.empty()) startFax(c); return; }
        if (!d || !c.host || !c.blockWhy.empty()) return;
        // ★ FT8/FT4 is the host's spotter (its own slot), every other decoder its audio-extension decoder — and a Band
        //   VFO the same spotter in Band scope (every mode on the band, behind the governor).
        const auto r = *d->ext ? c.host->start(d->ext, attachMsg(c.vfo))
                     : c.host->startSpots(c.band ? DecoderHost::SpotScope::Band : DecoderHost::SpotScope::Single);
        c.decoding = r == DecoderHost::Start::Ok || r == DecoderHost::Start::Joined;
        c.pauseWhy = c.decoding ? "" : "waiting for a decoder slot (listeners come first)";
    }
    /** ★ Pause one VFO for a listener: its decoder stops — the tap WRITES what it held (an RTTY buffer; a chart or
     *  picture in progress as PARTIAL) — and its slot is free. The channel keeps running, decoder-less (the host takes
     *  no audio while nothing runs), which is cheaper than tearing down and rebuilding the pipeline when it comes back. */
    void pauseOne(Chan& c, const std::string& why = "a listener needed a decoder slot", bool sayIt = true) {
        if (!c.decoding) return;
        if (c.ndb) stopNdb(c);
        else if (c.fax) stopFax(c);
        else if (!c.host) return;
        else { c.host->stop(); c.host->stopSpots(); }
        c.decoding = false;
        c.pauseWhy = why;
        if (sayIt) say("background decoding: VFO " + name(c) + " paused \xe2\x80\x94 " + why);
    }
    static std::string upperMode(std::string m) { for (auto& ch : m) ch = (char)std::toupper((unsigned char)ch); return m; }
    std::string name(const Chan& c) const {
        const DecoderDef* d = decoderDef(c.vfo.dec);
        // ★ An NDB skimmer has no frequency of its own: it is named by the part of the band it covers
        if (c.ndb) {
            char b[96]; std::snprintf(b, sizeof b, "%.0f\xe2\x80\x93%.0f kHz", c.ndbLo / 1e3, c.ndbHi / 1e3);
            return std::to_string(c.index + 1) + " (" + (c.ndbHi > c.ndbLo ? std::string(b) : std::string("the NDB band")) + " " + (d ? d->label : "?") + ")";
        }
        if (c.fax) return std::to_string(c.index + 1) + " (WEFAX stations: " + std::to_string(c.subs.size()) + " frequenc" + (c.subs.size() == 1 ? "y" : "ies") + ")";
        return std::to_string(c.index + 1) + " (" + khz(c.vfo.hz) + " " + (d ? d->label : "?") + ")";
    }
    void publish() {
        { int n = 0; for (auto& c : chans_) if (c->decoding) ++n; decodingNow_.store(n); }   // ★ the health tick's lock-free read
        if (!p_.registry) return;
        std::vector<BgRegistry::Pub> v;
        // ★ out of the window (or switched off here) = not waiting; nor is a VFO EASED for a listener (vibe_bg_ease.h) — it
        //   comes back by easeTick alone, and would otherwise hold every lower-ranked VFO on the box back behind it
        for (auto& c : chans_) if (c->inWindow && c->blockWhy.empty() && !c->eased) v.push_back({ c->index + 1, c->decoding, c->decoding && busy(*c) });
        p_.registry->publishState(v);
    }
    /** ★★ Part-way through a picture (a WEFAX chart, an SSTV frame) — see DecoderHost::midPicture. */
    bool busy(const Chan& c) const {
        if (c.fax) { for (auto& sb : c.subs) if (sb->running && sb->host->midPicture()) return true; return false; }
        return c.host && c.host->midPicture();
    }
    /** ★★ WHICH VFO GIVES WAY (2026-11-22): rows are in rank order, so the last running is the lowest — but a picture
     *  decoder part-way through a chart is passed over while an idle VFO is running: the chart would be cut (saved
     *  partial) where an idle VFO loses nothing. Only when every running VFO is mid-picture does the lowest of them go. */
    Chan* pausePick() {
        Chan* low = nullptr; Chan* idle = nullptr;
        for (auto& c : chans_) if (c->decoding) { low = c.get(); if (!busy(*c)) idle = c.get(); }
        return idle ? idle : low;
    }
    /** ★★ The slot housekeeping, on the worker thread twice a second: listeners' pause requests first (lowest rank
     *  first), then — every 3 s, `force` for a test — the highest-ranked paused VFO back when a slot is free and nobody
     *  on the box waits ahead of it. Never a tight loop: a stat() and a directory read at most. */
    void slotTick(bool force = false) {
        std::lock_guard<std::mutex> sl(slotMtx_);
        bool changed = false;
        // ★★ The channel budget (a locked radio's free user slots): over it, the lowest running VFO gives up its channel.
        while (runningCount() > budget()) {
            Chan* low = pausePick();
            if (!low) break;
            pauseOne(*low, "a listener took this radio's last user slot"); changed = true;
        }
        /* ★★★ beta94: a listener came (or the machine got hot) — slots this runner took OVER the box's limit while nobody
         *  listened go back, the lowest VFO first, one a tick, until the box is within its limit again. */
        if (bgSlots_ && !(p_.cpu && p_.cpu->full.load(std::memory_order_relaxed)) && bgSlots_->extraHeld() > 0 && bgSlots_->boxOver()) {
            if (Chan* low = pausePick()) { pauseOne(*low, "background back to the normal budget"); changed = true; }
        }
        // ★★ An NDB skimmer that may not run now (a listener's own NDB mode has the band; the owner switched NDB off): paused,
        //    said why — it resumes by the ordinary path once the reason is gone.
        std::string ndbWhy;
        bool ndbWhyRead = false;
        for (auto& c : chans_) {
            if (!c->ndb || !c->decoding || !p_.ndbWhyNot) continue;
            if (!ndbWhyRead) { ndbWhy = p_.ndbWhyNot(); ndbWhyRead = true; }
            if (!ndbWhy.empty()) { pauseOne(*c, ndbWhy); changed = true; }
        }
        for (int n = p_.registry ? p_.registry->takeDrops() : 0; n > 0; --n) {
            Chan* low = pausePick();                                      // ★ the lowest-ranked idle one (see pausePick)
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
                if (c->decoding || !c->inWindow || !c->blockWhy.empty() || c->eased) continue;   // ★ eased: back only by easeTick
                if (c->ndb && p_.ndbWhyNot) {
                    if (!ndbWhyRead) { ndbWhy = p_.ndbWhyNot(); ndbWhyRead = true; }
                    if (!ndbWhy.empty()) { c->pauseWhy = ndbWhy; continue; }   // ★ not its turn — never blocks the VFOs below
                }
                if (!p_.slots || p_.slots->inUse() >= p_.slots->max() || runningCount() >= budget()) break;
                const int best = p_.registry ? BgRegistry::bestPaused(p_.registry->dir()) : 0;
                if (best && c->index + 1 > best) break;             // ★ a higher-ranked VFO on another radio is waiting
                if (c->rx) c->rx->noteInputGap();                   // ★ its channel was not fed while paused
                for (auto& sb : c->subs) sb->rx->noteInputGap();
                startOne(*c);
                if (!c->decoding) break;
                say("background decoding: VFO " + name(*c) + " resumed");
                changed = true;
                break;                                               // ★ one at a time — the box is re-read next round
            }
        }
        publish();   // ★ every tick: a picture starting or ending changes who gives way (a no-op when nothing changed)
    }
    /** ★ Give one eased step back. A paused VFO resumes by the ordinary path (slotTick, a slot permitting), promptly. */
    void undoEase(const EaseStep& st) {
        if (st.kind == ease::Kind::Depth) { ladder_->forceMin(false); return; }
        if (st.vfo < 0 || (size_t)st.vfo >= chans_.size()) return;
        Chan& c = *chans_[(size_t)st.vfo];
        if (st.kind == ease::Kind::Chips) { c.chipsOff->store(false); return; }
        c.eased = false;
        if (!c.decoding) c.pauseWhy = "back from easing \xe2\x80\x94 resuming";
        lastResume_ = {};
    }
    /** Each VFO's CPU so far (ns, monotonic) — its demodulator's cut + feed, its decoder(s). chanMtx_ held. */
    std::vector<uint64_t> vfoCpuNsLocked() {
        std::vector<uint64_t> v;
        v.reserve(chans_.size());
        for (auto& c : chans_) {
            uint64_t ndbNs = 0;
            if (c->ndb) { std::lock_guard<std::mutex> nl(c->ndbM); ndbNs = c->ndbCpuDone + (c->ndbFeed ? c->ndbFeed->cpuNs() : 0); }
            uint64_t faxNs = 0;
            for (auto& sb : c->subs) faxNs += sb->host->cpuNs();
            v.push_back(c->demodNs.load(std::memory_order_relaxed) + (c->host ? c->host->cpuNs() : 0) + ndbNs + faxNs);
        }
        return v;
    }
    void say(const std::string& s) { if (p_.log) p_.log(s); }
    void loop() {
        vibeBackgroundThread("vibe-bgdsp");   // ★★★ beta92: the back seat — below listener decoders (vibe_thread.h)
        auto lastYieldCheck = std::chrono::steady_clock::now();
        for (;;) {
            std::vector<cf32> buf; bool gap = false; double bufUtc = 0; size_t gapN = 0;
            {
                std::unique_lock<std::mutex> lk(qm_);
                qcv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !q_.empty() || !run_.load(); });
                if (!run_.load()) return;
                if (!q_.empty()) {
                    const size_t miss = q_.front().missBefore;   // ★ beta92: chunks feedIq could not hand over (busy)
                    buf = std::move(q_.front().v); bufUtc = q_.front().utc; q_.pop_front(); qSamples_ -= buf.size();
                    gap = std::exchange(gap_, false) || miss > 0; gapN = std::exchange(gapSamples_, (size_t)0) + miss;
                }
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
                    if (c->ndb) {
                        uint64_t nd;
                        { std::lock_guard<std::mutex> nl(c->ndbM); nd = c->ndbDropDone + (c->ndbFeed ? c->ndbFeed->droppedBlocks() : 0); }
                        if (nd > c->ndbLastDrop) say("background decoding: VFO " + name(*c) + " \xe2\x80\x94 " + std::to_string((unsigned long long)(nd - c->ndbLastDrop))
                                                     + " NDB slice(s) dropped in the last minute (its skimmer fell behind)");
                        c->ndbLastDrop = nd;
                    }
                }
            }
            if (now - lastYieldCheck > std::chrono::milliseconds(500)) { lastYieldCheck = now; slotTick(); }
            if (buf.empty()) continue;
            if (gap) for (auto& c : chans_) { if (c->rx) c->rx->noteInputGap(); for (auto& sb : c->subs) sb->rx->noteInputGap(); }
            std::lock_guard<std::mutex> cl(chanMtx_);
            // ★ V12: a hole this queue dropped is BRIDGED in every Band VFO's engines (zeros of exactly its length, as the
            //   listener's tap does — DecoderHost::feedIq with no samples), so a slot with a hole in it still completes.
            if (gapN > 0) for (auto& c : chans_) if (c->band && c->decoding) bridgeBand(*c, gapN, bufUtc - (double)buf.size() / p_.sampleRate);
            // ★ V12 beta 4: what this thread costs, on its OWN clock — the shared FFT, and each VFO's cut + demodulator.
            const uint64_t t0 = vibeThreadCpuNs();
            uint64_t perVfo = 0;
            /* ★ THE CAPTURE TIME OF EACH BLOCK (vibe_wide_tap.h's rule): the channeliser completes block j when its
             *  (j+1)·hop-th input sample is in, and this chunk's last sample was captured at bufUtc — so a block ending k
             *  samples before the chunk's end was captured k / rate earlier. Only a Band VFO reads it. */
            const long long inStart = fedIn_, inEnd = fedIn_ + (long long)buf.size();
            fedIn_ = inEnd;
            const long long hop = chan_->hop();
            chan_->feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int) {
                const long long bi = chan_->blockIndex();
                const long long endIdx = std::min(inEnd, std::max(inStart, (bi + 1) * hop));
                const double utc = bufUtc - (double)(inEnd - endIdx) / p_.sampleRate;
                for (auto& c : chans_) {
                    if (!c->decoding) continue;          // ★ a paused or out-of-window channel costs nothing
                    const uint64_t a = vibeThreadCpuNs();
                    if (c->band) feedBand(*c, bins, bi, utc);
                    else if (c->fax) {
                        // ★ V12: each ticked WEFAX frequency inside the window — its own cut and demodulator, as a VFO's
                        for (auto& sb : c->subs) {
                            if (!sb->running) continue;
                            const int got = chan_->extract(bins, sb->centreBin, sb->bins, sb->slice.data(), sb->ectx, bi);
                            if (got > 0) sb->rx->feed(sb->slice.data(), got);
                        }
                    }
                    else if (c->ndb) {
                        // ★ V12: the NDB skimmer's slice — the same cut and taper as the listener's NDB tap, to its own queue
                        const int got = chan_->extract(bins, c->centreBin, c->bins, c->slice.data(), c->ectx, bi);
                        if (got > 0) if (auto f = c->feed()) f->feedSlice(c->slice.data(), got, c->rate, c->sliceRf);
                    } else {
                    const int got = chan_->extract(bins, c->centreBin, c->bins, c->slice.data(), c->ectx, bi);
                    if (got > 0) {
                        if (c->rx) c->rx->feed(c->slice.data(), got);
                        else if (c->host) c->host->feedIq(c->slice.data(), got, c->rate, c->sliceRf);
                    }
                    }
                    const uint64_t d = vibeThreadCpuNs() - a;
                    c->demodNs.fetch_add(d, std::memory_order_relaxed); perVfo += d;
                }
            });
            const uint64_t all = vibeThreadCpuNs() - t0;
            sharedNs_.fetch_add(all > perVfo ? all - perVfo : 0, std::memory_order_relaxed);
        }
    }

    /** ★★ A BAND VFO's block (V12 2026-10-27) — exactly what a listener's Band gets (local_sdr_shim.cpp feedOneClient):
     *  the main slice off this SAME forward FFT with a sixteenth's roll-off, its bins' power for the activity gate (one
     *  multiply-add a bin, no transform), then each extra slice the capture holds — one small inverse FFT each. */
    void feedBand(Chan& c, const cf32* bins, long long bi, double utc) {
        if (!c.host) return;
        const double binHz = p_.sampleRate / (double)p_.fftSize;
        const int got = chan_->extract(bins, c.centreBin, c.bins, c.slice.data(), c.ectx, bi, kBandTaperDiv);
        if (got <= 0) return;
        const float* pw = nullptr;
        if (c.host->wantsBins()) {
            c.pw.resize((size_t)c.bins);
            digi::slicePower(bins, p_.fftSize, c.centreBin, c.bins, c.pw.data());
            pw = c.pw.data();
        }
        c.host->feedIq(c.slice.data(), got, c.rate, c.sliceRf, pw, pw ? c.bins : 0, binHz, utc, 0);
        const double hwC = p_.centreHz + p_.hwOffsetHz;
        ExtraSlice xs[kMaxExtraSlices];
        const int nx = c.host->iqExtraSlices(xs);
        for (int k = 0; k < nx; ++k) {
            const int eb = bandSliceBins(p_.sampleRate, p_.fftSize, xs[k].widthHz);
            const int ecb = (int)std::lround((xs[k].centreHz - hwC) / binHz);
            const double erate = p_.sampleRate * (double)eb / (double)p_.fftSize, ecentre = hwC + ecb * binHz;
            if (!captureHolds(p_.sampleRate, hwC, ecentre, erate)) continue;   // ★ beyond this capture: not decoded
            Chan::Extra& x = c.bx[k];
            if ((int)x.slice.size() != eb) x.slice.assign((size_t)eb, cf32{0.0f, 0.0f});
            const int eg = chan_->extract(bins, ecb, eb, x.slice.data(), x.ctx, bi, kBandTaperDiv);
            if (eg <= 0) continue;
            const float* epw = nullptr;
            if (pw) { x.pw.resize((size_t)eb); digi::slicePower(bins, p_.fftSize, ecb, eb, x.pw.data()); epw = x.pw.data(); }
            c.host->feedIq(x.slice.data(), eg, erate, ecentre, epw, epw ? eb : 0, binHz, utc, k + 1);
        }
    }
    /** ★ `n` capture samples this queue dropped, ending at `endUtc`: each slice told how many of ITS samples are missing. */
    void bridgeBand(Chan& c, size_t n, double endUtc) {
        if (!c.host) return;
        const double binHz = p_.sampleRate / (double)p_.fftSize, hwC = p_.centreHz + p_.hwOffsetHz;
        auto gapTo = [&](double& carry, double rate, double centre, int slice) {
            const double want = (double)n * rate / p_.sampleRate + carry;
            const int k = (int)std::floor(want);
            carry = want - k;
            if (k > 0) c.host->feedIq(nullptr, k, rate, centre, nullptr, 0, binHz, endUtc, slice);
        };
        gapTo(c.gapCarry, c.rate, c.sliceRf, 0);
        ExtraSlice xs[kMaxExtraSlices];
        const int nx = c.host->iqExtraSlices(xs);
        for (int k = 0; k < nx; ++k) {
            const int eb = bandSliceBins(p_.sampleRate, p_.fftSize, xs[k].widthHz);
            const int ecb = (int)std::lround((xs[k].centreHz - hwC) / binHz);
            const double erate = p_.sampleRate * (double)eb / (double)p_.fftSize, ecentre = hwC + ecb * binHz;
            if (captureHolds(p_.sampleRate, hwC, ecentre, erate)) gapTo(c.bx[k].gapCarry, erate, ecentre, k + 1);
        }
    }

    Params p_;
    std::unique_ptr<vibedsp::Channelizer> chan_;
    long long fedIn_ = 0;                 // ★ samples fed to chan_ since it was made (each block's capture time)
    std::vector<std::unique_ptr<Chan>> chans_;
    std::thread th_;
    std::mutex qm_;
    std::condition_variable qcv_;
    struct QItem { std::vector<cf32> v; double utc = 0; size_t missBefore = 0; };   // ★ missBefore: see feedIq (beta92)
    std::atomic<size_t> missed_{0};          // ★ beta92: samples feedIq could not hand over since the last one it did
    std::atomic<uint64_t> busyDrops_{0};
    std::atomic<uint64_t> busyHeld_{0};      // ★ b94: chunks held for a busy queue (handed over late, not lost)
    std::mutex heldM_;                       // ★ b94: the DSP thread's side of the hand-over (feedIq; start/stop clear it)
    std::deque<QItem> held_;
    size_t heldSamples_ = 0;
    std::atomic<int> decodingNow_{0};
    std::atomic<bool> degrading_{false};
    std::deque<QItem> q_;
    size_t qSamples_ = 0;
    size_t gapSamples_ = 0;               // ★ dropped since the worker last took a chunk (bridged for Band VFOs)
    bool gap_ = false;
    std::atomic<bool> run_{false}, running_{false};
    std::chrono::steady_clock::time_point lastResume_{};
    std::mutex slotMtx_;
    std::unique_ptr<BgSlots> bgSlots_;   // ★ beta94: p_.slots points here while p_.cpu is set
    std::shared_ptr<digi::LoadLadder> ladder_ = std::make_shared<digi::LoadLadder>();   // ★ beta94: every Band VFO's
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
    // ★★ V12 2026-11-24 easing for listeners — see easeTick. The steps in effect (undone last-first), the governor, a
    //    20 s window of each VFO's CPU (what a step is worth), and what the health card reads (lock-free).
    std::vector<EaseStep> easeTaken_;
    ease::Governor gov_;
    struct EaseCpu { std::chrono::steady_clock::time_point t; std::vector<uint64_t> ns; };
    std::deque<EaseCpu> easeCpu_;         // ★ ≤ 12 samples (2 s apart, 20 s)
    std::atomic<bool> easeExcused_{false};
    std::atomic<int>  easePaused_{0}, easeChips_{0}, vfoCount_{0};
    std::atomic<bool> easeDepth_{false};   // ★ beta94: the ladder forced to minimum depth for a listener
    bool easeExhaustedSaid_ = false;
};

} // namespace bg
} // namespace vibe
