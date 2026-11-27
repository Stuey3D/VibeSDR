// vibe_lrpt_run.h — METEOR-M LRPT as a listener's decoder (V12, 2026-11-19): the DecoderHost's "lrpt" extension.
//
// ★★★ WHERE IT SITS. A listener tunes the satellite's downlink (137.100 / 137.900 MHz — per satellite, sats.json) and
//     starts LRPT; the host asks the DSP thread for an IQ SLICE of kSliceWidthHz centred on the dial (the CW / RTTY
//     skimmers' path, vibe_band_slice.h) and hands each block to this on its own decode thread (decoder priority:
//     Network > Audio > Spectrum > Decoders). Nothing here touches the radio.
//
// ★★★ THE DOPPLER CONTRACT with the satellite engine (branch v12-sat, docs/v12/SATELLITE-ENGINE.md): EITHER the engine
//     moves the listener's dial with the pass (the slice follows the dial, nothing to do here), OR it leaves the dial on
//     the published frequency and supplies Env::satDopplerHz — the shift to remove now, Hz, positive = received above
//     nominal. This reads it once per IQ block (≈ 20 times a second) and feeds it forward to the carrier loop; the loop
//     tracks the remainder. With neither, the receiver's own x⁴ acquisition and Costas loop follow ±3.1 kHz / 65 Hz/s
//     unaided (test-lrpt: −2.8 kHz with a +65 Hz/s slope decodes every block) — the engine only makes it faster.
//
// ★★ WHAT CLIENTS SEE: the SSTV picture frames (vibe_decoder_host.h: 0x07 start w h · 0x01 line y w rgb · 0x05 complete),
//    a QUARTER-SIZE colour preview (392 px wide; the composite when channels 1 and 2 are live, else the first live channel
//    as grey) growing a strip at a time — ★★★ beta96 with each row's OTHER live channels beside it (0x09, kChannelRowOp:
//    each viewer picks which channel to see) — plus {"type":"lrpt_status","data":{…}} once a second. WHAT THE GALLERY KEEPS: the
//    full-size PNGs — every live channel and the composite — when the pass ends (the satellite engine says it has set;
//    with no engine, 30 s with neither a frame nor a signal — passOver()) or the decoder stops (then marked partial).
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>      // ★ the developer IQ capture (Dump)
#include <cstdlib>     // getenv, atol
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vibe_lrpt.h"
#include "vibe_lrpt_overlay.h"   // ★ V12 the MAP OUTLINE (docs/v12/LRPT-MAP-OVERLAY.md)

namespace vibelrpt {

/* ★★★ V12 beta95 THE GAP IS DRAWN AS A GAP (Stuart, 2026-11-25, a Meteor pass on the Sony: "it did draw but scrolled to
 *  nothing" — the frames lost in the overhead fade were placed by time, geometry right, as SOLID BLACK, and the panel sat
 *  on the black). Rows with no picture are a dim diagonal HATCH, never plain black, and a gap tall enough is LABELLED in
 *  the picture itself — "NO SIGNAL 20:24-20:26" — so both clients and anyone saving the preview see what it is. The
 *  glyphs are our own 5 × 7 drawings, only the characters that label needs. */
namespace gapart {
inline const uint8_t* glyph(char c) {
    static const uint8_t D[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2},
        {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12} };
    static const uint8_t N[7] = {17,25,21,19,17,17,17}, O[7] = {14,17,17,17,17,17,14}, S[7] = {15,16,16,14,1,1,30},
        I[7] = {14,4,4,4,4,4,14}, G[7] = {14,17,16,23,17,17,15}, A[7] = {14,17,17,31,17,17,17}, L[7] = {16,16,16,16,16,16,31},
        C[7] = {0,12,12,0,12,12,0}, H[7] = {0,0,0,31,0,0,0};
    if (c >= '0' && c <= '9') return D[c - '0'];
    switch (c) { case 'N': return N; case 'O': return O; case 'S': return S; case 'I': return I; case 'G': return G;
                 case 'A': return A; case 'L': return L; case ':': return C; case '-': return H; default: return nullptr; }
}
/** The hatch for a pixel with no picture: dim diagonal stripes (a gap that reads as one on any screen). */
inline void hatch(uint8_t* px, int x, int y) {
    const uint8_t v = ((x + y) % 6) < 2 ? 54 : 26;
    px[0] = v; px[1] = v; px[2] = uint8_t(v + 10);
}
/** "20:24" from a satellite stamp (ms of day); "" for none. */
inline std::string hhmm(int32_t ms) {
    if (ms < 0) return std::string();
    char b[8]; const int m = int(ms / 60000) % 1440; std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60); return b;
}
/** Is text pixel (x, y) of `t` drawn at `scale` with its top-left at (x0, y0) lit? */
inline bool lit(const std::string& t, int scale, int x0, int y0, int x, int y) {
    if (scale <= 0 || x < x0 || y < y0) return false;
    const int gx = (x - x0) / scale, gy = (y - y0) / scale;
    if (gy >= 7) return false;
    const int ci = gx / 6, col = gx % 6;
    if (ci >= int(t.size()) || col >= 5) return false;
    const uint8_t* g = glyph(t[size_t(ci)]);
    return g && ((g[gy] >> (4 - col)) & 1);
}
/* ★★★ beta96 THE LIVE PREVIEW SQUASHES ITS GAPS (Stuart, 2026-11-26, Meteor-M2-4: 616 rows of no signal took as much
 *  room as the decoded picture, and near LOS a fade of thin strips between short gaps pushed the clean cloud out of
 *  view). The server keeps drawing the preview in TRUE geometry (the rows above — an older client shows those, the
 *  live outline is in those coordinates); it ALSO lists the gaps in lrpt_status ("gaps"), merged here, and the clients
 *  (src/utils/lrptGaps.ts) draw each listed gap as one short hatched band with its label. The gallery's pictures and
 *  the outline are untouched.
 *  MERGING: two gaps with only a short run of decoded strips between them (≤ kMergeStrips strips, or under kMergeMs of
 *  satellite time) are ONE gap, "patchy" — so a fading tail is one band and one label, never "NO SIGNAL 14:42" ×6. */
struct Gap {
    int s0 = 0, s1 = 0;          ///< strips [s0, s1), the decoded slivers of a patchy gap included
    int32_t msA = -1, msB = -1;  ///< the satellite time of the decoded strip before / after (−1 none)
    int lines = 0;               ///< lines of no signal in it (8 a strip; a patchy gap's slivers not counted)
    bool patchy = false;
};
constexpr int kMergeStrips = 1;        ///< a run of decoded strips this short (8 lines) never separates two gaps
constexpr int32_t kMergeMs = 5000;     ///< …nor one shorter than this (≈ 4 strips at 1.232 s)
constexpr size_t kMaxGaps = 64;        ///< the status carries at most this many (the closest pairs merge first)
/** `raw`: each gap's strips [s0, s1) in order; `ms`: a strip's satellite time (−1 unknown). */
inline std::vector<Gap> mergeGaps(const std::vector<std::pair<int, int>>& raw, const std::function<int32_t(int)>& ms) {
    std::vector<Gap> out;
    auto span = [&](int a, int b) -> int32_t {   // satellite time from strip a to strip b (−1 unknown); midnight wraps
        const int32_t x = ms(a), y = ms(b);
        if (x < 0 || y < 0) return -1;
        return y >= x ? y - x : y + 86400000 - x;
    };
    for (const auto& r : raw) {
        if (r.second <= r.first) continue;
        if (!out.empty()) {
            Gap& p = out.back();
            const int run = r.first - p.s1;   // decoded strips between
            // the run's own length in satellite time: first to last strip, plus one strip's period (span / (run − 1))
            const int32_t sp = run > 1 ? span(p.s1, r.first - 1) : -1;
            const int32_t t = sp >= 0 ? int32_t(int64_t(sp) * run / (run - 1)) : -1;
            if (run <= kMergeStrips || (t >= 0 && t < kMergeMs)) {
                p.s1 = r.second; p.lines += (r.second - r.first) * 8; p.patchy = true; p.msB = ms(r.second);
                continue;
            }
        }
        Gap g; g.s0 = r.first; g.s1 = r.second; g.lines = (r.second - r.first) * 8;
        g.msA = r.first > 0 ? ms(r.first - 1) : -1; g.msB = ms(r.second);
        out.push_back(g);
    }
    while (out.size() > kMaxGaps) {   // bounded: the two closest gaps become one (patchy)
        size_t best = 1; int bestRun = 1 << 30;
        for (size_t i = 1; i < out.size(); i++) { const int run = out[i].s0 - out[i - 1].s1; if (run < bestRun) { bestRun = run; best = i; } }
        Gap& p = out[best - 1]; const Gap& q = out[best];
        p.s1 = q.s1; p.lines += q.lines; p.patchy = true; p.msB = q.msB;
        out.erase(out.begin() + long(best));
    }
    return out;
}
}   // namespace gapart

class Run {
public:
    static constexpr double kSliceWidthHz = 140000;    ///< flat width asked for: 72k × 1.6 = 115 kHz occupied, + Doppler
    static constexpr double kPassEndSec = 30;          ///< no frame AND no signal this long (nobody knows the pass) = over
    static constexpr double kSetSec = 5;               ///< the engine says the satellite has set: frames stopped this long = over
    /** ★ The signal reading's floor for "the satellite is still there": (S+N)/N in dB (noise reads ≈ 0). */
    static constexpr double kQuietDb = 1.5;
    /** ★★ What the reading must reach to decode: (S+N)/N ≈ +4 dB (2026-11-24, docs/v12/LRPT-SENSITIVITY.md — was +5 before
     *  the soft Costas detector and the narrower loops). Synthetic OQPSK + differential: 50 % of frames at Es/N0 1.6 dB
     *  ((S+N)/N 3.9 dB). On the real Pi 500 passes, of the frames the new decoder got, 89 of 97 (24 Nov 10:55), 43 of 63
     *  (09:20) and 60 of 148 (23 Nov 13:59) came while the reading said 3.5–4.5 dB. */
    static constexpr double kNeedDb = 4.0;
    static constexpr int kPreviewW = kLineWidth / 4;   ///< 392
    static constexpr int kPreviewRowsPerStrip = 2;
    static constexpr int kPreviewMaxH = kMaxStrips * kPreviewRowsPerStrip;   ///< 1600
    /** ★★★ beta96 THE VIEWER'S CHANNEL PICKER (Stuart 11-26: "can we have the option to change the meteor image being
     *  viewed in the preview as one of the channels is this grain but the other channels have images"). Beside each 0x01
     *  preview row, one frame:  0x09 | u32 y | u32 w | u8 rApid | u8 bApid | u8 n | n × (u8 apid | w grey bytes)
     *  — rApid / bApid say what the 0x01 row of y carries (R = G = rApid, B = bApid; the same APID when it is one grey
     *  channel), the n channels are the live ones it does NOT carry. So the composite's two channels come free from the
     *  0x01 row and only the rest is extra: on a 1-2-4 pass ONE channel, 404 bytes beside the 0x01's 1185 (+34 %, about
     *  0.6 kB/s at the preview's ≈ 1.5 rows a second). Every viewer gets the same bytes (the frames are the host's
     *  broadcast and the joiner's replay); which channel is SHOWN is each client's own choice — the server's decode, the
     *  0x01 rows and the gallery are unchanged. An older client ignores an opcode it does not know (both clients do). */
    static constexpr uint8_t kChannelRowOp = 0x09;

    struct Opts {
        double symbolRate = 72000;
        ReceiverConfig::Mod mod = ReceiverConfig::Auto;
        ReceiverConfig::Diff diff = ReceiverConfig::DiffAuto;
        bool interleaved = false;
        bool rateAuto = false;                                           ///< no satellite entry named the rate: 72k and 80k tried
        std::function<void(const std::string&)> json;                    ///< text frames (lrpt_status)
        std::function<void(const uint8_t*, size_t)> frame;               ///< binary picture frames
        std::function<double()> dopplerHz;                               ///< the satellite engine's feed-forward; null = none
        /** ★ The satellite engine's pick: its LRPT symbol rate (72000 / 80000), 0 = not known. Asked once a second while
         *  AUTO is still walking the rates (Receiver::preferRate). */
        std::function<double()> satSymbolRate;
        /** ★★ Is the satellite above the horizon? 1 = in range (the engine tracks it), 0 = not (below the horizon / the pass
         *  ended), −1 = nobody knows (no pick: a hand-tuned dial). Asked once a second. See passOver(). */
        std::function<int()> satInRange;
        /** The pass's pictures (full size) — the gallery's copy. `partial` = cut off by a stop, not the pass ending. */
        std::function<void(const std::vector<Picture>&, bool partial)> pictures;
        std::function<void(const std::string&)> log;
        std::function<void(Receiver&)> made;                             ///< ★ test seam: each new receiver (diagnostics)
        /** ★★ beta92 what the radio's gain was over the pass (the satellite gain hold's note: "Airspy linearity preset 12
         *  (held, 0 clip cuts)", "RTL-SDR 32.8 dB (held …)", or why a radio has none) — for the per-pass summary. */
        std::function<std::string()> gainNote;
        /** ★★ V12 THE MAP OUTLINE (vibe_lrpt_overlay.h, docs/v12/LRPT-MAP-OVERLAY.md). `geoCandidates`: the METEORs it may
         *  be (the satellite engine's pick first, marked; else every METEOR with LRPT and elements) — null = no outline is
         *  attempted (tests, a developer replay without orbits). `geoObserver`: this server's place (false = not set).
         *  `nowUnix`: OUR UTC clock (the server's corrected clock); null = the system clock. */
        std::function<std::vector<geo::Candidate>()> geoCandidates;
        std::function<bool(vibe::sat::Observer&)> geoObserver;
        std::function<double()> nowUnix;
    };
    /** ★ The live preview's outline is recomputed at most this often while the picture grows. */
    static constexpr double kLiveGeoSec = 20.0;
    explicit Run(Opts o) : o_(std::move(o)) {}

    /* ★★ A DEVELOPER'S CAPTURE, NOT A FEATURE (Stuart 2026-11-21: "no point adding a recorder for users but for you to use
     *  as a debug/test then it is fine"). With VIBE_LRPT_IQ_DUMP=<directory> in the server's environment, each pass's IQ
     *  slice — exactly what this receiver is fed — goes to <dir>/lrpt-<unix>-<rate>.cf32 (interleaved float32 I,Q) with a
     *  .txt beside it: one line a second "t_s doppler_Hz rate lock carrier sync frames bad" so a replay feeds the same
     *  Doppler (tool-lrpt-replay.cpp). Capped (VIBE_LRPT_IQ_DUMP_MB, default 1536 MB) and written on the decode thread
     *  (never the audio thread); unset = not even a file handle. Why it exists: a live Meteor-M2-3 pass at ~4–5 dB over the
     *  floor stayed "listening" while the synthetic test decodes at Es/N0 4 dB — the live path needs replaying. */
    struct Dump {
        FILE* iq = nullptr; FILE* log = nullptr; bool tried = false; uint64_t bytes = 0, cap = 0; double lastLog = -1;
        ~Dump() { if (iq) std::fclose(iq); if (log) std::fclose(log); }
        template <class Rx>
        void write(const std::complex<float>* x, int n, double rate, double dop, double t, const Rx& rx) {
            if (!tried) {
                tried = true;
                const char* dir = std::getenv("VIBE_LRPT_IQ_DUMP");
                if (!dir || !*dir) return;
                const char* mb = std::getenv("VIBE_LRPT_IQ_DUMP_MB");
                cap = uint64_t(mb && std::atol(mb) > 0 ? std::atol(mb) : 1536) * 1024 * 1024;
                const std::string base = std::string(dir) + "/lrpt-" + std::to_string((long long)std::time(nullptr)) + "-" + std::to_string(int(rate));
                iq = std::fopen((base + ".cf32").c_str(), "wb");
                log = std::fopen((base + ".txt").c_str(), "w");
                if (log) std::fprintf(log, "# lrpt IQ capture: rate %.1f S/s, float32 I,Q interleaved\n# t_s doppler_Hz rate lock carrier sync frames bad\n", rate);
            }
            if (!iq || bytes >= cap) return;
            const size_t want = size_t(n) * sizeof(std::complex<float>);
            bytes += std::fwrite(x, 1, want, iq);
            if (log && t - lastLog >= 1.0) {
                lastLog = t;
                const auto st = rx.stats();
                std::fprintf(log, "%.1f %.1f %.1f %d %d %d %llu %llu\n", t, dop, rate, int(std::lround(rx.lockMetric() * 100)),
                             rx.demodLocked() ? 1 : 0, rx.syncLocked() ? 1 : 0, (unsigned long long)st.rsOk, (unsigned long long)st.rsFail);
                std::fflush(log);
            }
            if (bytes >= cap) { std::fclose(iq); iq = nullptr; if (log) { std::fprintf(log, "# cap reached\n"); std::fflush(log); } }
        }
    };
    ~Run() { finish(true); }

    /** One IQ block from the host's queue (decode thread). `captureUtc`: OUR clock when the block's LAST sample was
     *  captured (DecoderHost's IqBlock::utc — stamped where the DSP thread cut it, before the decode queue); NaN = not
     *  known (a direct caller with no queue: the replay tool, tests) — then "now" when the stamp lands. */
    void feedIq(const std::complex<float>* iq, int n, double rate, double /*centreRfHz*/, double captureUtc = NAN) {
        if (n <= 0 || !(rate > 0)) return;
        if (!rx_ || std::fabs(rate - rate_) > 0.5) {
            const double sMax = o_.rateAuto ? 80000 : o_.symbolRate, sMin = o_.rateAuto ? 72000 : o_.symbolRate;
            if (rate < sMax * 1.8 || rate > sMin * 8.5) {
                if (!rateWarned_) { rateWarned_ = true; status("the IQ slice runs at " + std::to_string(int(rate)) + " S/s \xe2\x80\x94 LRPT needs 1.8 to 8 times the symbol rate"); }
                return;
            }
            if (rx_) { savePass(true); }
            rate_ = rate; newReceiver();
        }
        const double dop = o_.dopplerHz ? o_.dopplerHz() : 0.0;
        if (o_.dopplerHz) rx_->setDopplerHz(dop);
        dump_.write(iq, n, rate, dop, fedSec_, *rx_);   // ★ developer capture only (VIBE_LRPT_IQ_DUMP); nothing when unset
        rx_->feedIq(reinterpret_cast<const float*>(iq), size_t(n));
        fedSec_ += n / rate;
        // ── progressive preview: every new strip that has landed ──
        if (rx_->version() != seenVersion_) {
            seenVersion_ = rx_->version(); lastFrameSec_ = fedSec_;
            /* ★ the overlay's clock check: OUR time when the newest stamp arrived (vibe_lrpt_overlay.h checkClock).
             *  ★★★ b96-satgrace (Mac Ha, a phone server: "the satellite's clock reads 5.5 s behind ours") — the time the
             *  block was CAPTURED, not when the decode thread got round to it: a busy decode queue (a pass + VDL2 + the
             *  ORBCOMM sky plot on one phone) lags the air by seconds, and that lag always read as "satellite behind", so a
             *  correct on-board clock was refused. The stamp's frame ends somewhere inside this block, so the error left is
             *  at most the block's length (tens of ms). */
            wallStamp_ = rx_->lastStampMs();
            wallAt_ = std::isfinite(captureUtc) && captureUtc > 0 ? captureUtc : now();
        }
        // ★ at most once a second: the version moves with every packet, and each preview resends its last strips
        if (seenVersion_ != previewVersion_ && fedSec_ - lastPreview_ >= 1.0) { previewVersion_ = seenVersion_; lastPreview_ = fedSec_; preview(); }
        if (fedSec_ - lastStatus_ >= 1.0) { lastStatus_ = fedSec_; status(""); }
        // ── the signal reading: the demodulator's Es/N0, averaged over the second (see status) ──
        snrAcc_ += rx_->esn0(); snrN_++;
        if (fedSec_ - lastAsk_ >= 1.0) {
            lastAsk_ = fedSec_;
            if (o_.satInRange) inRange_ = o_.satInRange();
            if (o_.rateAuto && o_.satSymbolRate && rx_->rateGroups() > 1) rx_->preferRate(o_.satSymbolRate());
            if (snrDb_ > kQuietDb) lastSignalSec_ = fedSec_;
            sumSecond();
            // ★ beta92 THE PASS SUMMARY at LOS when there is no picture to save (the "nearly" pass — passOver below only
            //   runs for a pass that has pictures); with pictures it comes with them, just below
            if (prevInRange_ == 1 && inRange_ == 0 && !havePictures_) summary("LOS \xe2\x80\x94 the satellite has set");
            else if (inRange_ == -1 && !havePictures_ && sum_.secs > 0 && (sum_.heard || sum_.frames) && passOver())
                summary("no frame and no signal for 30 s");
            prevInRange_ = inRange_;
        }
        if (havePictures_ && passOver()) {
            if (o_.log) o_.log(inRange_ == 0 ? "lrpt: the satellite has set \xe2\x80\x94 the pass's pictures to the gallery"
                                             : "lrpt: pass over (no frame and no signal for 30 s) \xe2\x80\x94 pictures to the gallery");
            summary(inRange_ == 0 ? "LOS \xe2\x80\x94 the satellite has set" : "pass over (no frame and no signal for 30 s)");
            savePass(false);
            saved_++;
            newReceiver();
        }
    }
    /* ★★★ WHEN A PASS IS OVER (2026-11-22, Stuart's Meteor-M2-4 pass: "pass over (no frame for 30 s)" at 517 s with
     *  eight minutes of it to come, then a fresh receiver and a second picture). A gap in the frames is a gap in the
     *  PICTURE — a fade, a gain step, the aerial's null overhead — not the end of the pass:
     *   · the satellite engine says it is in range (TRACK / COAST)  → never over; keep the picture open, keep hunting;
     *   · the engine says it has set (ENDED, or the pass is not on) → over once frames have stopped for kSetSec;
     *   · nobody knows (a hand-tuned dial)                          → over after kPassEndSec with no frame AND no
     *     signal: while the receiver still measures the satellite (kQuietDb over the noise) the pass goes on. */
    bool passOver() const {
        const double sinceFrame = fedSec_ - lastFrameSec_;
        if (inRange_ == 1) return false;
        if (inRange_ == 0) return sinceFrame > kSetSec;
        return sinceFrame > kPassEndSec && fedSec_ - lastSignalSec_ > kPassEndSec;
    }
    /** The decoder stops (a replace, the listener leaving, a shutdown): keep what the pass got, marked partial. */
    void finish(bool interrupted) {
        if (done_) return;
        done_ = true;
        // ★ beta92 the decoder stopping ends the pass's summary too — unless the pass was already summed up at LOS and
        //   nothing has been heard since (the minutes after LOS are not a second pass)
        if (sum_.secs > 0 && (sum_.heard || sum_.frames || !summarised_)) summary(interrupted ? "decoder stopped" : "decoder finished");
        if (rx_) savePass(interrupted);
    }
    const Receiver* receiver() const { return rx_.get(); }
    /** The last per-pass summary line (tests). */
    const std::string& lastSummary() const { return lastSummary_; }

    /* ── ★★★ beta92 THE PER-PASS SUMMARY (the XCover's METEOR-M2-3 pass, 2026-11-23: "kept looking like it was going to
     *  work but nothing", and the log could not say how near it came). ONE line at LOS / the pass's end / the decoder
     *  stopping: how long, the best signal reading and how long it stood at or above what a decode needs (kNeedDb), the
     *  symbol rate (or the rates AUTO tried), how many seconds the carrier loop and the frame sync held, the frames (good /
     *  failed) and picture lines, and what the radio's gain was (the satellite hold's note). Counted once a second across
     *  receivers (a rate change or a saved picture starts a new one; the pass goes on). */
    struct PassSum {
        int secs = 0, aboveSec = 0, carrierSec = 0, syncSec = 0;
        double peakSnr = 0;
        bool heard = false;
        unsigned rates = 0;            // bit 0 = 72k, bit 1 = 80k
        uint64_t frames = 0, bad = 0;
        int lines = 0;                 // lines of finished receivers this pass
    };
    const PassSum& passSum() const { return sum_; }

private:
    Dump dump_;
    PassSum sum_;
    uint64_t sumOk_ = 0, sumBad_ = 0;   // the current receiver's counts already summed
    int prevInRange_ = -1;
    bool summarised_ = false;
    std::string lastSummary_;
    int curLines() const { if (!rx_) return 0; int s0, s1; rx_->stripRange(s0, s1); return std::max(0, s1 - s0) * 8; }
    void sumSecond() {
        if (!rx_) return;
        ++sum_.secs;
        sum_.peakSnr = std::max(sum_.peakSnr, snrDb_);
        if (snrDb_ >= kNeedDb) ++sum_.aboveSec;
        if (snrDb_ > kQuietDb) sum_.heard = true;
        if (rx_->demodLocked()) ++sum_.carrierSec;
        if (rx_->syncLocked()) ++sum_.syncSec;
        const double sr = rx_->symbolRate();
        sum_.rates |= sr >= 76000 ? 2u : 1u;
        const Stats st = rx_->stats();
        if (st.rsOk >= sumOk_) { sum_.frames += st.rsOk - sumOk_; sumOk_ = st.rsOk; }
        if (st.rsFail >= sumBad_) { sum_.bad += st.rsFail - sumBad_; sumBad_ = st.rsFail; }
    }
    void summary(const std::string& why) {
        if (sum_.secs <= 0) return;
        const int lines = sum_.lines + curLines();
        auto rateName = [](unsigned b) { return b == 3u ? std::string("72k and 80k") : b == 2u ? std::string("80k") : std::string("72k"); };
        std::string rate = o_.rateAuto ? "AUTO, tried " + rateName(sum_.rates)
                                       : std::to_string(int(o_.symbolRate / 1000)) + "k" + (o_.interleaved ? " interleaved" : "");
        const char* verdict = sum_.frames > 0 ? (lines > 0 ? "decoded" : "frames but no picture lines")
                            : sum_.syncSec > 0 ? "synced, no frame passed the error check"
                            : sum_.carrierSec > 0 ? "carrier locked, never synced"
                            : sum_.heard ? "heard, never locked" : "nothing heard";
        std::string gain = o_.gainNote ? o_.gainNote() : std::string();
        if (gain.empty()) gain = "not known (no satellite gain hold ran)";
        char b[640];
        std::snprintf(b, sizeof b, "lrpt pass summary (%s): %d min %02d s \xc2\xb7 peak SNR %.1f dB (a decode needs %.1f), %d s at or above it "
                      "\xc2\xb7 symbol rate %s \xc2\xb7 carrier %d s, sync %d s \xc2\xb7 %llu frame%s (%llu failed), %d line%s \xe2\x80\x94 %s "
                      "\xc2\xb7 gain: %s", why.c_str(), sum_.secs / 60, sum_.secs % 60, sum_.peakSnr, kNeedDb, sum_.aboveSec, rate.c_str(),
                      sum_.carrierSec, sum_.syncSec, (unsigned long long)sum_.frames, sum_.frames == 1 ? "" : "s",
                      (unsigned long long)sum_.bad, lines, lines == 1 ? "" : "s", verdict, gain.c_str());
        lastSummary_ = b;
        if (o_.log) o_.log(lastSummary_);
        summarised_ = true;
        sum_ = PassSum{};
        // ★ the current receiver's lines and frames are this pass's: the next pass counts only what comes after
        sum_.lines = -curLines();
    }
    void newReceiver() {
        sum_.lines += curLines();   // ★ a receiver replaced mid-pass: its lines stay counted
        sumOk_ = sumBad_ = 0;
        ReceiverConfig c; c.sampleRate = rate_; c.symbolRate = o_.symbolRate; c.mod = o_.mod; c.diff = o_.diff; c.interleaved = o_.interleaved; c.rateAuto = o_.rateAuto;
        rx_ = std::make_unique<Receiver>(c);
        if (o_.made) o_.made(*rx_);
        seenVersion_ = 0; previewVersion_ = 0; havePictures_ = false; sentRows_ = 0; previewOpen_ = false; lastFrameSec_ = fedSec_;
        wallAt_ = 0; wallStamp_ = -1; lastGeoSec_ = -1; geoStrips_ = 0; heading_ = -1; headingAsked_ = -1; dark_.clear();
        lastSignalSec_ = fedSec_; snrAcc_ = 0; snrN_ = 0;
        if (o_.log) o_.log("lrpt: receiver at " + std::to_string(int(rate_)) + " S/s, "
                           + (o_.rateAuto ? std::string("72k / 80k AUTO") : std::to_string(int(o_.symbolRate)) + " sym/s" + (o_.interleaved ? " (80k interleaved)" : "")));
    }
    void savePass(bool partial) {
        if (!rx_ || !havePictures_) return;
        preview(true);
        if (sentRows_ > 0 && o_.frame) { std::vector<uint8_t> m{ 0x05, 0, 0, 0, 0 }; o_.frame(m.data(), m.size()); }
        decideHeading(true);
        const bool rot = heading_ == 1;   // ★★★ beta96 a northbound pass: north up, turned ONCE, here
        auto pics = renderPictures(*rx_, rot);
        if (rot && o_.log) o_.log("lrpt: northbound pass \xe2\x80\x94 the pictures are turned round, north up");
        if (!pics.empty() && o_.geoCandidates) {
            int s0, s1; rx_->stripRange(s0, s1);
            const geo::Overlay ov = overlay(s0, s1, 1.0, rot);
            for (auto& p : pics) { if (ov.ok) p.geoJson = ov.json; else p.geoWhy = ov.why; }
            if (o_.log) o_.log(ov.ok ? "lrpt: map outline made (" + std::to_string(ov.points) + " points)" : "lrpt: no map outline \xe2\x80\x94 " + ov.why);
        }
        if (!pics.empty() && o_.pictures) o_.pictures(pics, partial);
        havePictures_ = false;
    }
    double now() const {
        if (o_.nowUnix) return o_.nowUnix();
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    /** The outline for strips [s0, s1) — the full picture (scale 1) or the live preview (a quarter). */
    geo::Overlay overlay(int s0, int s1, double scale, bool rot180 = false) const {
        std::vector<int32_t> ms;
        for (int s = s0; s < s1; s++) ms.push_back(rx_->stripTimeMs(s));
        std::vector<geo::Candidate> cs = o_.geoCandidates ? o_.geoCandidates() : std::vector<geo::Candidate>{};
        vibe::sat::Observer ob; const bool haveObs = o_.geoObserver && o_.geoObserver(ob) && ob.valid;
        return geo::makeOverlay(ms, (s1 - s0) * 8, wallAt_, wallStamp_, cs, haveObs ? &ob : nullptr, scale, geo::Instrument{}, rot180);
    }
    /** ★★★ beta96 NORTH UP: which way this pass goes (geo::northbound), decided once a picture exists and then kept for the
     *  pass — the clients turn the live preview by it (lrpt_status "rot"), the gallery's pictures are turned by it. Asked
     *  at most every 5 s until known (an SGP4 call); `force` = the pass is ending, ask now. */
    void decideHeading(bool force = false) {
        if (heading_ >= 0 || !o_.geoCandidates || !rx_) return;
        if (!force && headingAsked_ >= 0 && fedSec_ - headingAsked_ < 5.0) return;
        headingAsked_ = fedSec_;
        const std::vector<geo::Candidate> cs = o_.geoCandidates();
        vibe::sat::Observer ob; const bool haveObs = o_.geoObserver && o_.geoObserver(ob) && ob.valid;
        heading_ = geo::northbound(cs, haveObs ? &ob : nullptr, wallAt_ > 0 ? wallAt_ : now());
    }
    /** ★★ THE LIVE OUTLINE: {"type":"lrpt_geo","data":{…}} in the PREVIEW's coordinates (a quarter size, from the
     *  preview's first strip), every kLiveGeoSec while the picture grows — the clients draw it over the growing picture
     *  when the viewer's MAP OUTLINE toggle is on. A pass whose checks fail sends {"why":"…"} once. */
    void liveGeo() {
        if (!o_.geoCandidates || !o_.json || !previewOpen_ || sentRows_ <= 0) return;
        if (fedSec_ - lastGeoSec_ < kLiveGeoSec && lastGeoSec_ >= 0) return;
        const int strips = (sentRows_ + kPreviewRowsPerStrip - 1) / kPreviewRowsPerStrip;
        if (strips == geoStrips_) return;
        lastGeoSec_ = fedSec_; geoStrips_ = strips;
        const geo::Overlay ov = overlay(previewS0_, previewS0_ + strips, double(kPreviewW) / kLineWidth);
        std::string esc; for (char c : ov.why) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
        o_.json(std::string("{\"type\":\"lrpt_geo\",\"data\":") + (ov.ok ? ov.json : "{\"why\":\"" + esc + "\"}") + "}");
    }
    static void put32(std::vector<uint8_t>& m, uint32_t v) { m.push_back(uint8_t(v >> 24)); m.push_back(uint8_t(v >> 16)); m.push_back(uint8_t(v >> 8)); m.push_back(uint8_t(v)); }
    /** ★★ THE PREVIEW: a QUARTER-SIZE picture (392 px, two rows a strip), each row sent ONCE, in order, a strip only once
     *  the next has begun (by then its period's packets — every channel's — are in). Never a row re-sent: both clients
     *  read a line number going back as a NEW picture (the WEFAX rule), and a re-send every second was 28 kB/s anyway.
     *  Quarter size because the app pre-sizes an SSTV canvas from 0x07 (392 × 1600 RGBA = 2.5 MB on a 1 GB phone,
     *  not 10 MB at half size); the full pictures are the gallery's. `flush`: the pass is over — the last strip too. */
    void preview(bool flush = false) {
        int s0, s1; rx_->stripRange(s0, s1);
        if (s1 <= s0) return;
        const auto live = rx_->liveApids();
        if (live.empty()) return;
        havePictures_ = true;
        if (!o_.frame) return;
        if (!previewOpen_) {
            previewOpen_ = true; previewS0_ = s0;
            std::vector<uint8_t> m{ 0x07 }; put32(m, kPreviewW); put32(m, kPreviewMaxH);
            o_.frame(m.data(), m.size());
            const std::string nm = "METEOR LRPT";
            std::vector<uint8_t> n{ 0x02, uint8_t(nm.size() >> 8), uint8_t(nm.size()) }; n.insert(n.end(), nm.begin(), nm.end());
            o_.frame(n.data(), n.size());
        }
        const bool rgb = std::find(live.begin(), live.end(), 64) != live.end() && std::find(live.begin(), live.end(), 65) != live.end();
        const int rA = rgb ? 65 : live.front(), bA = rgb ? 64 : live.front();   // ★ what the 0x01 row carries (R = G, B)
        // ★★★ beta96 each live channel's levels ONCE per preview (the composite's two were already taken here every
        //   second): they stretch the rows, and say which visible channels are dark (a night pass — kDarkHi)
        struct Lv { int apid, lo, hi; bool inv; const Channel* c; };
        std::vector<Lv> lvs; dark_.clear();
        for (int ap : live) {
            Lv l{ ap, 0, 255, ap >= 67, &rx_->channels().at(ap) }; l.c->levels(l.lo, l.hi);
            if (visibleApid(ap) && l.hi < kDarkHi) dark_.push_back(ap);
            lvs.push_back(l);
        }
        auto lvOf = [&](int ap) -> const Lv& { for (const Lv& l : lvs) if (l.apid == ap) return l; return lvs.front(); };
        const Channel* a = lvOf(rA).c;
        const Channel* b = lvOf(bA).c;
        const int lo1 = lvOf(rA).lo, hi1 = lvOf(rA).hi, lo2 = lvOf(bA).lo, hi2 = lvOf(bA).hi;
        const bool inv = !rgb && live.front() >= 67;
        // ★★★ beta96 the channels the 0x01 row does NOT carry — sent beside it as 0x09 (see kChannelRowOp)
        std::vector<const Lv*> extra;
        for (const Lv& l : lvs) if (l.apid != rA && l.apid != bA) extra.push_back(&l);
        std::vector<uint8_t> m9;
        const int upto = flush ? s1 : s1 - 1;
        std::vector<uint8_t> line(size_t(kPreviewW) * 3);
        auto lv = [](int v, int lo, int hi) { const int o = (v - lo) * 255 / std::max(1, hi - lo); return uint8_t(o < 0 ? 0 : o > 255 ? 255 : o); };
        // ★★ beta95 the gap this row is in (gapart): its strips [g0, g1), its label and where the label sits — found once a gap
        int g0 = -1, g1 = -1, ty0 = 0, tx0 = 0, tScale = 0; std::string label;
        for (int y = sentRows_; ; y++) {
            const int s = previewS0_ + y / kPreviewRowsPerStrip, r = (y % kPreviewRowsPerStrip) * (8 / kPreviewRowsPerStrip);
            if (s >= upto || y >= kPreviewMaxH) break;
            const bool gap = !stripDecoded(s);
            if (gap && !(s >= g0 && s < g1)) {
                g0 = s; while (g0 > previewS0_ && !stripDecoded(g0 - 1)) g0--;
                g1 = s + 1; while (g1 < s1 && !stripDecoded(g1)) g1++;
                const int r0 = (g0 - previewS0_) * kPreviewRowsPerStrip, h = (g1 - g0) * kPreviewRowsPerStrip;
                const std::string a0 = gapart::hhmm(g0 > 0 ? rx_->stripTimeMs(g0 - 1) : -1), a1 = gapart::hhmm(g1 < s1 ? rx_->stripTimeMs(g1) : -1);
                label = "NO SIGNAL" + (a0.empty() ? std::string() : " " + a0 + (a1.empty() || a1 == a0 ? std::string() : "-" + a1));
                tScale = h >= 7 * 2 + 6 ? 2 : h >= 9 ? 1 : 0;
                if (tScale && int(label.size()) * 6 * tScale > kPreviewW - 8) tScale = 1;
                ty0 = r0 + (h - 7 * tScale) / 2;
                tx0 = (kPreviewW - (int(label.size()) * 6 - 1) * tScale) / 2;
            }
            for (int x = 0; x < kPreviewW; x++) {
                const int px = x * (kLineWidth / kPreviewW), col = px / 8;
                if (!a->has(s, col) || !b->has(s, col)) {
                    uint8_t* q = &line[size_t(x) * 3];
                    if (gap && gapart::lit(label, tScale, tx0, ty0, x, y)) { q[0] = 214; q[1] = 214; q[2] = 224; }
                    else gapart::hatch(q, x, y);
                    continue;
                }
                const size_t pi = (size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(px);
                uint8_t R = lv(a->pixels()[pi], lo1, hi1), B = lv(b->pixels()[pi], lo2, hi2);
                if (inv) R = B = uint8_t(255 - R);
                line[size_t(x) * 3] = R; line[size_t(x) * 3 + 1] = R; line[size_t(x) * 3 + 2] = rgb ? B : R;
            }
            std::vector<uint8_t> m; m.reserve(9 + line.size()); m.push_back(0x01); put32(m, uint32_t(y)); put32(m, kPreviewW);
            m.insert(m.end(), line.begin(), line.end());
            o_.frame(m.data(), m.size());
            // ★★★ beta96 the same row of every other live channel, stretched by its own levels, its missing blocks
            //   hatched (and a gap's label lit) exactly as the 0x01 row draws them — so any channel the viewer picks
            //   reads as the composite does. Right after its 0x01: a client holding the pair can show either.
            m9.clear(); m9.reserve(12 + extra.size() * (1 + size_t(kPreviewW)));
            m9.push_back(kChannelRowOp); put32(m9, uint32_t(y)); put32(m9, kPreviewW);
            m9.push_back(uint8_t(rA)); m9.push_back(uint8_t(bA)); m9.push_back(uint8_t(extra.size()));
            for (const Lv* l : extra) {
                m9.push_back(uint8_t(l->apid));
                for (int x = 0; x < kPreviewW; x++) {
                    const int px = x * (kLineWidth / kPreviewW), col = px / 8;
                    if (!l->c->has(s, col)) {
                        uint8_t q[3];
                        if (gap && gapart::lit(label, tScale, tx0, ty0, x, y)) q[0] = 214; else gapart::hatch(q, x, y);
                        m9.push_back(q[0]); continue;
                    }
                    const size_t pi = (size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(px);
                    uint8_t v = lv(l->c->pixels()[pi], l->lo, l->hi);
                    m9.push_back(l->inv ? uint8_t(255 - v) : v);
                }
            }
            o_.frame(m9.data(), m9.size());
            sentRows_ = y + 1;
        }
        decideHeading();
        liveGeo();
    }
    /** ★★★ beta96 the preview's gaps (gapart::mergeGaps) as ,"gaps":[[r0,r1,msA,msB,lines,patchy],…] in PREVIEW rows
     *  [r0, r1) — only gaps whose rows have all been sent (a gap is closed by the decoded strip after it); "" for none. */
    std::string previewGapsJson() const {
        if (!rx_ || !previewOpen_ || sentRows_ <= 0) return std::string();
        const int sEnd = previewS0_ + sentRows_ / kPreviewRowsPerStrip;
        std::vector<std::pair<int, int>> raw;
        for (int s = previewS0_; s < sEnd; s++) {
            if (stripDecoded(s)) continue;
            int e = s; while (e < sEnd && !stripDecoded(e)) e++;
            if (e >= sEnd) break;            // not yet closed by a decoded strip that has been sent
            raw.emplace_back(s, e); s = e;
        }
        if (raw.empty()) return std::string();
        const auto gs = gapart::mergeGaps(raw, [this](int s) { return rx_->stripTimeMs(s); });
        std::string j = ",\"gaps\":[";
        for (size_t i = 0; i < gs.size(); i++) {
            char b[96];
            std::snprintf(b, sizeof b, "%s[%d,%d,%d,%d,%d,%d]", i ? "," : "", (gs[i].s0 - previewS0_) * kPreviewRowsPerStrip,
                          (gs[i].s1 - previewS0_) * kPreviewRowsPerStrip, int(gs[i].msA), int(gs[i].msB), gs[i].lines, gs[i].patchy ? 1 : 0);
            j += b;
        }
        return j + "]";
    }
    /** ★ beta95 strip `s` carries any picture in a live channel (else it is a gap: gapart). */
    bool stripDecoded(int s) const {
        if (!rx_) return false;
        for (int a : rx_->liveApids()) { const Channel& c = rx_->channels().at(a); for (int k = 0; k < kMcuPerLine; k++) if (c.has(s, k)) return true; }
        return false;
    }
    void status(const std::string& why) {
        // ★ the second's average of the demodulator's Es/N0 → (S+N)/N in dB: what the waterfall shows over its floor at
        //   the top of the signal's hump, and ≈ 0 on noise (the estimate is signed, so the average sits at 0 there)
        if (snrN_ > 0) { const double es = snrAcc_ / snrN_; snrDb_ = 10 * std::log10(std::max(1.0, 1.0 + es)); snrAcc_ = 0; snrN_ = 0; }
        if (!o_.json) return;
        std::string j = "{\"type\":\"lrpt_status\",\"data\":{\"on\":true";
        { char b[96]; std::snprintf(b, sizeof b, ",\"snrDb\":%.1f,\"needDb\":%.1f,\"saved\":%d,\"inRange\":%d", snrDb_, kNeedDb, saved_, inRange_); j += b; }
        if (rx_) {
            const Stats s = rx_->stats();
            int s0, s1; rx_->stripRange(s0, s1);
            j += ",\"carrier\":" + std::string(rx_->demodLocked() ? "true" : "false") + ",\"sync\":" + (rx_->syncLocked() ? "true" : "false")
               + ",\"mode\":\"" + rx_->modeName() + "\",\"diff\":" + std::to_string(rx_->diffWinner())
               + ",\"symbolRate\":" + std::to_string(int(rx_->symbolRate())) + ",\"lock\":" + std::to_string(int(std::lround(rx_->lockMetric() * 100)))
               + ",\"offsetHz\":" + std::to_string(int(std::lround(rx_->carrierHz())))
               + ",\"frames\":" + std::to_string((unsigned long long)s.rsOk) + ",\"bad\":" + std::to_string((unsigned long long)s.rsFail)
               + ",\"packets\":" + std::to_string((unsigned long long)s.packets) + ",\"lines\":" + std::to_string((s1 - s0) * 8);
            // ★★ beta95 decoded lines apart from the gap's (the "656 lines" of 11-25 held ~450 of no signal)
            { int dec = 0; for (int k = s0; k < s1; k++) if (stripDecoded(k)) dec++;
              j += ",\"decoded\":" + std::to_string(dec * 8) + ",\"gapLines\":" + std::to_string((s1 - s0 - dec) * 8); }
            j += previewGapsJson();
            if (heading_ >= 0) j += heading_ == 1 ? ",\"rot\":180" : ",\"rot\":0";   // ★★★ beta96 north up: turn the preview
            j += ",\"channels\":[";
            bool first = true;
            const auto live = rx_->liveApids();
            for (int a : live) { j += std::string(first ? "" : ",") + "\"" + channelName(a) + "\""; first = false; }
            j += "]";
            // ★★★ beta96 the same channels as APIDs (the viewer's channel picker), and the visible ones that are dark
            //   (a night pass: the clients' AUTO shows a thermal channel instead of a speckled composite — kDarkHi).
            //   "dark" is measured as the preview is drawn (once a second, with its levels).
            j += ",\"apids\":[";
            for (size_t i = 0; i < live.size(); i++) j += (i ? "," : "") + std::to_string(live[i]);
            j += "],\"dark\":[";
            first = true;
            for (int a : dark_) if (std::find(live.begin(), live.end(), a) != live.end()) { j += std::string(first ? "" : ",") + std::to_string(a); first = false; }
            j += "]";
        }
        if (!why.empty()) j += ",\"why\":\"" + why + "\"";
        j += "}}";
        o_.json(j);
    }

    Opts o_;
    std::unique_ptr<Receiver> rx_;
    double rate_ = 0, fedSec_ = 0, lastStatus_ = -1, lastFrameSec_ = 0;
    double snrAcc_ = 0, snrDb_ = 0, lastAsk_ = -1, lastSignalSec_ = 0; int snrN_ = 0, inRange_ = -1, saved_ = 0;
    uint64_t seenVersion_ = 0, previewVersion_ = 0;
    double lastPreview_ = -1;
    bool havePictures_ = false, previewOpen_ = false, done_ = false, rateWarned_ = false;
    int sentRows_ = 0, previewS0_ = 0;
    double wallAt_ = 0, lastGeoSec_ = -1; int32_t wallStamp_ = -1; int geoStrips_ = 0;   // ★ the map outline
    int heading_ = -1; double headingAsked_ = -1;   // ★★★ beta96 1 northbound (north up = turned), 0 southbound, −1 not known
    std::vector<int> dark_;   // ★★★ beta96 the live visible channels with no sunlight in them (preview, kDarkHi)
};

} // namespace vibelrpt
