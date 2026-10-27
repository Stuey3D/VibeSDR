// vibe_watch.h — V12 VFO A/B: a listener's WATCH CHANNELS (GitHub discussion #57, Gesiima: "listen to two or more
// frequencies at the same time … monitoring a calling frequency and a second channel without having to switch back
// and forth"). Stuart, 2026-10-27: dual frequencies per radio, a simple VFO A/B toggle in the tuning menu.
//
// ★★ WHAT A WATCH CHANNEL IS. One more demodulator for ONE listener, inside the capture window the radio already has:
//    its own frequency, mode, passband and squelch, its own AGC (its own RxPipeline), and an open/closed squelch state
//    with the time it last changed — reported to the client on every change. VFO A is the listener's ordinary channel;
//    VFO B is watch channel id 1. The channel COUNT is a parameter (kMaxPerListener), not an A/B pair baked into the
//    DSP, because the scanner (coming, brief not yet written — Stuart: "this all fits into the scanner work too") will
//    drive the same objects: a PRIORITY WATCH samples a channel and switches the audio to it when its squelch opens
//    (`SquelchGate` already carries open + changedAt for exactly that), and CHANNEL STEPPING is a re-aim of one
//    channel (`WatchChannel::want`). Nothing here knows how many there are.
//
// ★★ ONE AUDIO STREAM, NO SECOND SOCKET. The watch channels' audio is mixed into the listener's own stream ON THE
//    SERVER (`PanMixer`), so the client needs no second audio socket, no second Opus decoder and no new code path to
//    play it: an old client simply hears it, a new one also draws the second needle.
//
// ★★★ WHERE EACH VFO SITS IN THE HEAD (Stuart, 2026-10-27): "at a glance the lower is the left vfo and the higher is
//     the right and it will always be that way" — by FREQUENCY, not by A/B, so the waterfall and the ears agree.
//     · STEREO (the default, the listener can turn it off at any time beside binaural): sources ranked by frequency,
//       lowest hard LEFT, highest hard RIGHT (more than two spread evenly between). Within kSameHz of each other they
//       are one place — the middle, both ears.
//     · STEREO OFF, or a MONO output (the client asked for mono: a phone speaker, a mono headset): every source centre,
//       so the server's mono fold hears both at full level and nothing is lost.
//     · EVERY MOVE IS A RAMP — adding B pans A from the middle to its ear while B fades in on the other side, removing B
//       fades it out and pans A back to the middle, tuning one across the other swaps the ears — all over kRampSec on a
//       raised-cosine curve (no corner, so no click). kSwapHz of hysteresis keeps a needle wobbling over the other from
//       fluttering the ears.
//     · THE PAN LAW IS EQUAL-POWER, NORMALISED TO TODAY'S MONO: centre = 1.0 in each ear (exactly what a listener with
//       one VFO hears now, so the hand-back on removal has no step), hard side = √2 in one ear (+3 dB, the same acoustic
//       power). The AGC's output setpoint is 0.25 (vibedsp AGC kTarget), so the +3 dB has 9 dB of headroom.
//     ★ Why not BinauralSpatialiser: it is a LEAN — the far ear drops at most 10 dB and is "never silent" — built to
//       say which way to tune. Two different stations need the opposite: each wholly in its own ear.
//
// ★ COST, stated (Stuart: "VibeSDR and VibeServer is heavily optimised to run on super low end hardware"): a watch
//   channel is ONE MORE LISTENER'S DSP. On a range-locked radio it is one more slice of the forward FFT every listener
//   already shares (an extract + a small channel-rate pipeline, on that listener's own thread); on a one-pipeline radio
//   it is a full-rate tune-and-decimate on a thread of its own at audio priority (`FullRateFeeder`), because there is no
//   shared transform to slice. Nothing at all runs for a listener with VFO B off. Measured in test-watch-cost.
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibe_vfo_dc.h"   // ★ fitCentre keeps the PHYSICAL DC out of both passbands (vfoDcZone, kVfoDcHoldMinHz)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibewatch {

using cf32 = vibedsp::cf32;
using Mode = vibedsp::RxPipeline::Mode;

constexpr int    kMaxPerListener = 1;      ///< watch channels a listener may hold today (VFO B). The scanner raises it.
constexpr int    kOutRate = 48000;
constexpr double kRampSec = 0.40;          ///< every pan and fade — Stuart: "a nice transition", ~300-500 ms
constexpr double kSwapHz = 100.0;          ///< the ears swap only once a needle is this far past the other
constexpr double kSameHz = 5.0;            ///< "both on exactly the same frequency (within a few Hz)" → both centre
constexpr double kSquelchHystDb = 2.0;     ///< a gate that opened closes only this far below its threshold…
constexpr double kSquelchTailSec = 0.25;   ///< …and only after this long there (a rig's squelch tail)
constexpr double kGateFadeSec = 0.010;     ///< the gate itself opens and shuts over 10 ms — no click on a carrier

// ── The squelch, with its open/closed state and WHEN it changed ─────────────────────────────────────────────────────
/** ★★ The state a scanner reads. `open` follows the level with hysteresis and a tail so it does not chatter at the
 *  frame rate (a priority watch that flickered would yank the audio about); `changedAt` is when it last changed, in
 *  the caller's clock (the shim passes wall-clock ms so the client can show "open 12 s"). Squelch OFF = always open. */
struct SquelchGate {
    bool   on = false;
    float  db = -100.0f;
    bool   open = true;
    double changedAt = 0;      // caller's clock
    double belowSince = -1;    // caller's clock, -1 = not below
    /** Fold in one level reading. Returns true when `open` changed. `now` and `tailUnits` share the caller's units. */
    bool update(float levelDb, double now, double tail = kSquelchTailSec * 1000.0) {
        bool want = open;
        if (!on) want = true;
        else if (!open) want = levelDb >= db;
        else {
            if (levelDb < db - (float)kSquelchHystDb) { if (belowSince < 0) belowSince = now; if (now - belowSince >= tail) want = false; }
            else belowSince = -1;
        }
        if (want == open) return false;
        open = want; changedAt = now; if (!open) belowSince = -1;
        return true;
    }
};

// ── A mono FIFO between a channel's demodulator and the mixer ───────────────────────────────────────────────────────
/** ★ The watch audio and the main audio come out of two pipelines that are fed the same samples but cut them into
 *  different block sizes — and on a one-pipeline radio on two different threads. The FIFO absorbs that: it PRIMES to
 *  `primeFrames` before releasing anything (so a thread hand-off cannot underrun it every block), never holds more than
 *  `maxFrames` (the oldest goes — latency is bounded, never grows), and an underrun is filled with silence and re-primes.
 *  Thread-safe; one producer, one consumer. */
class PcmFifo {
public:
    explicit PcmFifo(size_t primeFrames = 480, size_t maxFrames = 9600) : prime_(primeFrames), max_(maxFrames) {}
    void setPrime(size_t p) { std::lock_guard<std::mutex> lk(m_); prime_ = p; }
    void push(const float* x, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        q_.insert(q_.end(), x, x + n);
        if (q_.size() > max_) { dropped_ += q_.size() - max_; q_.erase(q_.begin(), q_.begin() + (q_.size() - max_)); }
    }
    /** Fill `out` with n frames; returns how many were real audio (the rest are zeros). */
    size_t pop(float* out, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        if (!primed_ && q_.size() >= prime_) primed_ = true;
        size_t got = 0;
        if (primed_) {
            got = std::min(n, q_.size());
            std::copy(q_.begin(), q_.begin() + got, out);
            q_.erase(q_.begin(), q_.begin() + got);
            if (got < n) { primed_ = false; underruns_++; }
        }
        std::fill(out + got, out + n, 0.0f);
        return got;
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); primed_ = false; }
    size_t depth() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    uint64_t underruns() const { std::lock_guard<std::mutex> lk(m_); return underruns_; }
    uint64_t droppedFrames() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
private:
    mutable std::mutex m_;
    std::deque<float> q_;
    size_t prime_, max_;
    bool primed_ = false;
    uint64_t underruns_ = 0, dropped_ = 0;
};

/** ★★★ THE WATCH CHANNEL'S CUSHION: 50 ms (V12 beta24, 2026-10-27). It was 480 frames — 10 ms, LESS than one block of
 *  the mix (≈491 frames a 10 ms block at 48 kHz, the count varying by a frame as the resampler goes). Priming at 480 left
 *  no cushion at all: a block of 491 in and 492 asked for was an underrun, silence and a re-prime, and the next block the
 *  same. Worse, every RxPipeline on a multi-core server demodulates on its own worker thread (VIBE_DSP_THREADS, the
 *  2026-10-16 split), so B's audio reaches the FIFO a hand-off LATER than A's is mixed: on a loaded box (an Xcode and a
 *  Gradle build beside the server) B lost over half of a second — test-server-vfo-ab's "A and B at matching levels"
 *  read B at 0.08 against A's 0.18, and the listener would have heard B stutter. 50 ms is five blocks: the worker's
 *  hand-off and a scheduler's hiccup on a Pi 2 both fit, and VFO B a twentieth of a second behind VFO A is not
 *  something an ear can place (they are different stations in different ears). Decoders on B are fed BEFORE the FIFO
 *  (the audio tap), so they never wait for it. */
constexpr size_t kWatchPrimeFrames = 2400;

// ── One watch channel ───────────────────────────────────────────────────────────────────────────────────────────────
/** What the listener asked for. Written by the control thread through WatchChannel::want(), applied by the feeding
 *  thread at its next block — so a retune never races a feed. */
struct Wanted {
    double hz = 0;
    std::string mode = "am";   // the wire name, for reporting back
    Mode   rxMode = Mode::AM;
    double bwHz = 10000;       // already clamped by the caller (RxPipeline::clampBwHz + the owner's cap)
    bool   squelchOn = false;
    float  squelchDb = -100.0f;
};

class WatchChannel {
public:
    explicit WatchChannel(int id) : id_(id), fifo_(kWatchPrimeFrames, 9600) {}
    ~WatchChannel() { std::lock_guard<std::mutex> lk(feedMtx_); if (rx_) rx_->stop(); }
    int id() const { return id_; }

    /** Control thread: re-aim. Cheap; takes effect at the next feed. */
    void want(const Wanted& w) {
        std::lock_guard<std::mutex> lk(wantMtx_);
        const bool tuneChange = w.hz != want_.hz || w.rxMode != want_.rxMode || w.bwHz != want_.bwHz;
        want_ = w; dirty_ = dirty_ || tuneChange;
        gate_.on = w.squelchOn; gate_.db = w.squelchDb;
        if (!gate_.on && !gate_.open) { gate_.open = true; }
    }
    Wanted wanted() const { std::lock_guard<std::mutex> lk(wantMtx_); return want_; }

    /** Feeding thread: `n` samples at `rate`, centred `offsetHz` BELOW this channel's frequency (i.e. the channel sits at
     *  +offsetHz in this stream). The pipeline is rebuilt only when the input rate changes (a mode/width that needs a
     *  wider slice); a retune inside it moves the oscillator, exactly as a listener's own channel does. */
    void feed(const cf32* x, int n, double rate, double offsetHz) {
        if (n <= 0 || !(rate > 0)) return;
        std::lock_guard<std::mutex> lk(feedMtx_);
        Wanted w; bool dirty;
        { std::lock_guard<std::mutex> wl(wantMtx_); w = want_; dirty = dirty_; dirty_ = false; }
        if (!rx_ || rate != rate_) {
            if (rx_) rx_->stop();
            rx_.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks cb{};
            cb.ctx = this;
            cb.audio = &WatchChannel::audioCb;   // ★ no spectrum, no zoom, no RDS — this pipeline exists to be HEARD
            rx_->start(rate, 1024, 1.0, kOutRate, cb);
            rx_->setStereoEnabled(false);
            rate_ = rate; lastOff_ = 1e300; dirty = true;
        }
        if (dirty || offsetHz != lastOff_) { rx_->setTune(offsetHz, w.rxMode, w.bwHz); lastOff_ = offsetHz; }
        rx_->feed(x, n);
    }
    /** Forget the audio queued so far (the channel was parked outside the window, or re-armed). */
    void flush() { fifo_.clear(); }

    /** The level the shim measured in this channel's passband (dB on the wide row's scale — the squelch's units).
     *  Returns true when the squelch changed state. `nowMs` is the caller's clock (wall ms). */
    bool level(float db, double nowMs) {
        levelDb_.store(db, std::memory_order_relaxed);
        std::lock_guard<std::mutex> wl(wantMtx_);
        const bool ch = gate_.update(db, nowMs);
        open_.store(gate_.open, std::memory_order_relaxed);
        return ch;
    }
    SquelchGate gate() const { std::lock_guard<std::mutex> wl(wantMtx_); return gate_; }
    bool  open() const { return open_.load(std::memory_order_relaxed); }
    float levelDb() const { return levelDb_.load(std::memory_order_relaxed); }

    /** ★★ Round 3: muted in the listener's mix (PanMixer::Src::muted). Everything else carries on — the demodulator, the
     *  level, the squelch — so a VFO decoding while muted loses nothing. */
    void setMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

    /** ★★ DECODE ON EITHER VFO (Stuart, 2026-10-27: "surely it doesnt matter which VFO is listen and which is decode?").
     *  This channel's demodulated audio — mono, 48 kHz, BEFORE the mix (so neither its mute nor its squelch reaches it),
     *  exactly where a listener's own decoder is fed VFO A's — handed to whatever decodes VFO B. Called on the feeding
     *  thread inside the demodulator's callback; the tap must copy and return (DecoderHost::feed queues, never decodes).
     *  Unset = one relaxed atomic read a block. */
    using AudioTap = std::function<void(const float* pcm, int frames)>;
    void setAudioTap(AudioTap t) {
        std::lock_guard<std::mutex> lk(tapMtx_);
        tap_ = std::move(t);
        hasTap_.store((bool)tap_, std::memory_order_release);
    }

    /** ★ Outside the capture window: the channel is kept (the listener's choice survives a radio move) but silent. */
    void setParked(bool p) { if (p && !parked_.load()) flush(); parked_.store(p); }
    bool parked() const { return parked_.load(); }

    PcmFifo& fifo() { return fifo_; }

private:
    static void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* self = static_cast<WatchChannel*>(ctx);
        if (self->parked_.load(std::memory_order_relaxed) || frames <= 0) return;
        const float* mono = pcm;
        std::vector<float> m;
        if (ch != 1) {
            // ★ Stereo cannot happen (setStereoEnabled(false)) — folded anyway, so a future WFM change cannot swap ears.
            m.resize((size_t)frames);
            for (int i = 0; i < frames; i++) m[i] = 0.5f * (pcm[2 * i] + pcm[2 * i + 1]);
            mono = m.data();
        }
        self->fifo_.push(mono, (size_t)frames);
        if (self->hasTap_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(self->tapMtx_);
            if (self->tap_) self->tap_(mono, frames);
        }
    }
    const int id_;
    mutable std::mutex wantMtx_;
    std::mutex feedMtx_;
    Wanted want_;
    bool dirty_ = true;
    SquelchGate gate_;
    std::atomic<bool> open_{true}, parked_{false}, muted_{false}, hasTap_{false};
    std::mutex tapMtx_;
    AudioTap tap_;
    std::atomic<float> levelDb_{-200.0f};
    std::unique_ptr<vibedsp::RxPipeline> rx_;
    double rate_ = 0, lastOff_ = 1e300;
    PcmFifo fifo_;
};

// ── The pan mixer ───────────────────────────────────────────────────────────────────────────────────────────────────
/** A smooth move from one value to another over kRampSec, raised-cosine eased (zero slope at both ends — no corner in
 *  the gain curve, so no click). Re-aiming mid-move starts a new move FROM WHERE IT IS, so nothing ever jumps. */
struct Ramp {
    float from = 0, to = 0, cur = 0;
    int   t = 0, dur = 1;
    void set(float v) { from = to = cur = v; t = dur = 1; }
    void aim(float v, int durFrames) {
        if (v == to) return;
        from = cur; to = v; t = 0; dur = std::max(1, durFrames);
    }
    bool moving() const { return t < dur; }
    float step() {
        if (t < dur) {
            ++t;
            const float u = (float)t / (float)dur;
            const float e = 0.5f - 0.5f * std::cos(3.14159265f * u);
            cur = from + (to - from) * e;
        }
        return cur;
    }
};

/** Mixes N mono sources into one L/R stream. Source 0 is the listener's own channel (VFO A); the rest are watch
 *  channels, identified by a stable key (the channel id) so a source keeps its ramps from call to call. */
class PanMixer {
public:
    struct Src {
        int    key = 0;            // 0 = the listener's own channel; else the watch channel id
        double hz = 0;
        bool   present = true;     // false = fading out (being removed)
        bool   gateOpen = true;    // its squelch (the main channel's is applied before the mixer)
        /** ★★ MUTED (round 3 — Stuart, 2026-10-27: "the ability to mute a VFO that is decoding so for WEFAX no need to
         *  hear the WEFAX noises whilst listening to SSB … when the 2nd VFO is muted the audio from the first one pans
         *  back to mono so both ears are in play"). A muted source FADES OUT over kRampSec and gives up its place in the
         *  head: the ranking below sees only the sources that are heard, so one left alone goes to the middle — both
         *  ears — on the same raised-cosine, equal-power move as everything else. Unmuting is the reverse. Only the
         *  MIX forgets it: its demodulator, its level, its squelch and anything decoding from it carry on untouched. */
        bool   muted = false;
        const float* pcm = nullptr;
    };
    explicit PanMixer(int rate = kOutRate) : rate_(rate) {}
    /** Stereo routing on (the default) or both-in-both-ears. Switchable live — the move ramps like any other. */
    void setStereo(bool on) { stereo_ = on; }
    bool stereo() const { return stereo_; }
    /** ★ A mono output folds L+R into one: every source centre, at unity, so nothing is lost or halved. */
    void setMonoOut(bool on) { mono_ = on; }

    /** One block. `out` gets `frames` interleaved L/R floats. Sources not seen in this call are dropped (their fade-out
     *  has to be driven by passing them with present=false until faded()). */
    void mix(const std::vector<Src>& srcs, int frames, float* out) {
        const int dur = (int)std::lround(kRampSec * rate_);
        const int gdur = std::max(1, (int)std::lround(kGateFadeSec * rate_));
        aimPositions(srcs, dur);
        // keep only the states we were given, in the order given
        std::vector<State*> st; st.reserve(srcs.size());
        for (auto& s : srcs) {
            State& x = stateFor(s.key);
            x.seen = true;
            x.gain.aim(s.present && !s.muted ? 1.0f : 0.0f, dur);
            x.gate.aim(s.gateOpen ? 1.0f : 0.0f, gdur);
            st.push_back(&x);
        }
        states_.erase(std::remove_if(states_.begin(), states_.end(), [](const State& x) { return !x.seen; }), states_.end());
        for (auto& x : states_) x.seen = false;
        // st pointers may have been invalidated by the erase — re-resolve
        for (size_t k = 0; k < srcs.size(); k++) st[k] = &stateFor(srcs[k].key);
        const float kHalfPi = 1.57079633f, kRoot2 = 1.41421356f;
        for (int i = 0; i < frames; i++) {
            float l = 0, r = 0;
            for (size_t k = 0; k < srcs.size(); k++) {
                State& x = *st[k];
                const float pos = x.pos.step(), g = x.gain.step() * x.gate.step();
                const float v = srcs[k].pcm ? srcs[k].pcm[i] * g : 0.0f;
                if (v == 0.0f) continue;
                const float th = pos * kHalfPi;
                l += v * kRoot2 * std::cos(th);
                r += v * kRoot2 * std::sin(th);
            }
            out[2 * i] = l; out[2 * i + 1] = r;
        }
    }
    /** Has the source with this key finished fading out (or never existed)? */
    bool faded(int key) const {
        for (auto& x : states_) if (x.key == key) return !x.gain.moving() && x.gain.cur <= 0.0f;
        return true;
    }
    /** Is everything at rest with only source 0 left, centre and at unity — i.e. the mixer would output exactly
     *  today's mono-in-both-ears? Then the caller can hand back to the ordinary path with no step.
     *  ★ AT UNITY too: a VFO A that was muted is still fading back in when B goes (the shim unmutes A as B is removed),
     *    and handing back mid-fade would jump it to full level. */
    bool restingMono() const {
        for (auto& x : states_) {
            if (x.key == 0) {
                if (x.pos.moving() || std::fabs(x.pos.cur - 0.5f) > 1e-6f) return false;
                if (x.gain.moving() || x.gain.cur < 1.0f) return false;
            }
            else if (x.gain.moving() || x.gain.cur > 0.0f) return false;
        }
        return true;
    }
    /** Where each source is headed (0 = hard left, 0.5 = centre, 1 = hard right) — for the client's L/R chips. */
    float targetPos(int key) const { for (auto& x : states_) if (x.key == key) return x.pos.to; return 0.5f; }
    void reset() { states_.clear(); order_.clear(); }

private:
    struct State { int key = 0; Ramp pos, gain, gate; bool seen = false; bool fresh = true; };
    State& stateFor(int key) {
        for (auto& x : states_) if (x.key == key) return x;
        states_.push_back(State{});
        State& x = states_.back();
        x.key = key; x.pos.set(0.5f);
        x.gain.set(key == 0 ? 1.0f : 0.0f);   // ★ a new watch channel FADES IN; the listener's own channel is already there
        x.gate.set(1.0f);
        return x;
    }
    /** Rank the PRESENT sources by frequency — with kSwapHz of hysteresis against the previous order — and aim each at
     *  its place. Sources fading out keep the place they had. */
    void aimPositions(const std::vector<Src>& srcs, int dur) {
        std::vector<const Src*> live;
        // ★ A muted source is not heard, so it takes no place: the one left alone goes to the middle (see Src::muted).
        for (auto& s : srcs) if (s.present && !s.muted) live.push_back(&s);
        // previous order first, newcomers appended
        std::vector<const Src*> ord;
        for (int key : order_) for (auto* s : live) if (s->key == key) ord.push_back(s);
        std::vector<const Src*> fresh;
        for (auto* s : live) if (std::find(ord.begin(), ord.end(), s) == ord.end()) fresh.push_back(s);
        // a newcomer goes in by plain frequency order
        for (auto* s : fresh) {
            auto at = std::find_if(ord.begin(), ord.end(), [&](const Src* o) { return o->hz > s->hz; });
            ord.insert(at, s);
        }
        // hysteresis: swap neighbours only when out of order by more than kSwapHz
        for (bool moved = true; moved;) {
            moved = false;
            for (size_t i = 0; i + 1 < ord.size(); i++)
                if (ord[i]->hz > ord[i + 1]->hz + kSwapHz) { std::swap(ord[i], ord[i + 1]); moved = true; }
        }
        order_.clear();
        for (auto* s : ord) order_.push_back(s->key);
        // groups within kSameHz share one place
        std::vector<int> group(ord.size(), 0);
        int g = 0;
        for (size_t i = 0; i < ord.size(); i++) {
            if (i > 0 && std::fabs(ord[i]->hz - ord[i - 1]->hz) > kSameHz) g++;
            group[i] = g;
        }
        const int groups = ord.empty() ? 0 : g + 1;
        const bool spread = stereo_ && !mono_ && groups > 1;
        for (size_t i = 0; i < ord.size(); i++) {
            const float p = spread ? (float)group[i] / (float)(groups - 1) : 0.5f;
            stateFor(ord[i]->key).pos.aim(p, dur);
        }
    }
    int rate_;
    bool stereo_ = true, mono_ = false;
    std::vector<State> states_;
    std::vector<int> order_;
};

// ── Where the radio sits for TWO VFOs ───────────────────────────────────────────────────────────────────────────────
/** ★★★ THE RF CENTRE FOLLOWS BOTH VFOs (round 3 — Stuart, 2026-10-27: "in Dual VFO mode the RF centre should move
 *  accordingly so that both VFO's can be tuned as if they have the full 2.4MSPS window"). On a radio whose dial is THIS
 *  listener's own (a personal receiver), each VFO allows the centre anywhere within `lim` of itself — `lim` is the
 *  shim's own figure for a VFO (dongleForView: the usable half-span, less the edge margin, less half its passband), so
 *  neither passband ever sits in the roll-off. Both fit while those two ranges overlap.
 *  · Already inside both: the centre stays (no retune, no PLL relock — the commonest case by far).
 *  · Otherwise: DECISIVELY to the middle of the room both allow — the same reason dongleForView recentres rather than
 *    shuffling (the next nudge would need another retune) — but never with the capture's centre (the DC spike a
 *    dongle carries there) inside `clearHz` of either VFO: the nearest point outside both clearances is taken instead.
 *  · No overlap: `ok` false — the VFOs are further apart than one capture can hold, and the caller refuses.
 *  ★★★ THE DC IS PHYSICAL (V12 beta24, 2026-10-27). A zero-IF radio's spike sits at the capture's PHYSICAL centre, the
 *  logical centre + `dcOffHz` (hwOffsetHz(): 15 kHz on a dongle) — not at `cur`. This rule used to keep the LOGICAL
 *  centre clear, so a B tune that moved the radio could put the spike exactly on A or B (centre 15 kHz below a VFO).
 *  Now, given `dcOffHz` > 0 and each VFO's zone (vibe::vfoDcZone — its passband + the 2.5 kHz guard, never stricter
 *  than the locked VFO), every answer keeps centre + `dcOffHz` out of BOTH zones:
 *    · inside the room with the DC in a zone → the smallest move that clears it, the DC `hold` beyond the zone's edge
 *      (vibe_vfo_dc.h's hysteresis, so a VFO hovering at the edge does not ask again a step later);
 *    · a decisive move → `clearHz` is measured from the physical DC, and a zone is never landed in.
 *  With `dcOffHz` 0 (an HF+, a SpyServer, the rule switched off) it is exactly the old placement.
 *  Pure, so test-watch checks it. */
struct CentreFit { bool ok = false; bool moved = false; double centre = 0; double lo = 0, hi = 0; };   // lo..hi: the room both allow
inline CentreFit fitCentre(double cur, double aHz, double aLim, double bHz, double bLim, double clearHz,
                           double dcOffHz = 0.0, vibe::VfoDcZone aZone = {}, vibe::VfoDcZone bZone = {}) {
    CentreFit f; f.centre = cur;
    const double lo = std::max(aHz - aLim, bHz - bLim), hi = std::min(aHz + aLim, bHz + bLim);
    f.lo = lo; f.hi = hi;
    if (lo > hi) return f;
    f.ok = true;
    const double off = dcOffHz > 0.0 ? dcOffHz : 0.0;
    auto inZone = [&](const vibe::VfoDcZone& z, double c) { const double d = c + off; return off > 0.0 && z.lo < z.hi && d > z.lo && d < z.hi; };
    auto onDc = [&](double c) { return inZone(aZone, c) || inZone(bZone, c); };
    auto inRoom = [&](double c) { return c >= lo && c <= hi; };
    // ★ the ways out of the zones: the DC `hold` beyond either edge of each (vibe_vfo_dc.h's figure)
    std::vector<double> edges;
    for (const vibe::VfoDcZone* z : { &aZone, &bZone }) {
        if (off <= 0.0 || !(z->lo < z->hi)) continue;
        const double hold = std::max(vibe::kVfoDcHoldMinHz, 0.5 * (z->hi - z->lo));
        edges.push_back(z->lo - hold - off);
        edges.push_back(z->hi + hold - off);
    }
    auto nearest = [&](const std::vector<double>& xs, double to, const std::function<bool(double)>& good, double& out) {
        bool found = false; double bestD = 1e300;
        for (double x : xs) {
            if (!inRoom(x) || !good(x)) continue;
            const double d = std::fabs(x - to);
            if (d < bestD) { bestD = d; out = x; found = true; }
        }
        return found;
    };
    if (inRoom(cur)) {
        if (!onDc(cur)) return f;                       // ★ the commonest case by far: no retune, no PLL relock
        double c = cur;
        if (nearest(edges, cur, [&](double x) { return !onDc(x); }, c)) { f.moved = true; f.centre = c; return f; }
        // nowhere close clears it — fall through to the decisive placement
    }
    // ★ the DC (PHYSICAL) kept `clearHz` from both VFOs when the centre has to move anyway
    auto clear = [&](double x) { return std::fabs(x + off - aHz) >= clearHz && std::fabs(x + off - bHz) >= clearHz; };
    double c = 0.5 * (lo + hi);
    if (!clear(c) || onDc(c)) {
        std::vector<double> xs = { aHz - clearHz - off, aHz + clearHz - off, bHz - clearHz - off, bHz + clearHz - off };
        xs.insert(xs.end(), edges.begin(), edges.end());
        double best = c;
        if (nearest(xs, c, [&](double x) { return clear(x) && !onDc(x); }, best)
            || nearest(xs, c, [&](double x) { return !onDc(x); }, best))
            c = best;
    }
    f.moved = true; f.centre = c;
    return f;
}

// ── A one-pipeline radio's feeder ───────────────────────────────────────────────────────────────────────────────────
/** ★★ On a radio with no channeliser running (one pipeline — a personal receiver, or a shared dial) there is no
 *  forward FFT to slice, so a watch channel tunes and decimates the full-rate capture itself. That work runs HERE, on
 *  a thread of its own at AUDIO priority (it is somebody's audio — Network > Audio > Spectrum > Decoders), and the DSP
 *  thread's whole cost is a copy into a bounded queue: half a second, the oldest dropped (counted), never waited for. */
class FullRateFeeder {
public:
    using Work = std::function<void(const cf32* x, int n)>;
    ~FullRateFeeder() { stop(); }
    void start(Work w, std::function<void()> threadInit = {}) {
        if (run_.load()) return;
        work_ = std::move(w);
        run_.store(true);
        th_ = std::thread([this, threadInit] { if (threadInit) threadInit(); loop(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(m_); run_.store(false); q_.clear(); qn_ = 0; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    void feed(const cf32* x, int n, double rate) {
        if (!run_.load(std::memory_order_relaxed) || n <= 0) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.emplace_back(x, x + n); qn_ += (size_t)n;
            const size_t cap = (size_t)std::max(65536.0, rate * 0.5);
            while (qn_ > cap && q_.size() > 1) { qn_ -= q_.front().size(); q_.pop_front(); dropped_.fetch_add(1); }
        }
        cv_.notify_one();
    }
    uint64_t droppedBlocks() const { return dropped_.load(); }
private:
    void loop() {
        for (;;) {
            std::vector<cf32> b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !q_.empty() || !run_.load(); });
                if (!run_.load()) return;
                if (q_.empty()) continue;
                b = std::move(q_.front()); q_.pop_front(); qn_ -= b.size();
            }
            if (work_) work_(b.data(), (int)b.size());
        }
    }
    Work work_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<cf32>> q_;
    size_t qn_ = 0;
    std::atomic<uint64_t> dropped_{0};
};

} // namespace vibewatch
