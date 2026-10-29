// vibe_decoder_host.h — one listener's decoders (RTTY, NAVTEX, WEFAX, SSTV, time signals, FT8/FT4),
// and the box-wide decoder SLOTS that bound how many of them run at once.
//
// ★★★ WHY THIS IS ITS OWN THING (B6, 2026-09-30). Every decoder used to be a member of the RADIO —
//     one FskDecoder, one WefaxDecoder, one Ft8Decoder per Impl — and "Only the decoder OWNER is
//     actually decoding". That is right on a SHARED DIAL, where everybody hears one VFO and so shares
//     its decoder (Stuart, 2026-08-20: "the decoder box needs to be shared too, just show a mirror of
//     it to the others"). On a RANGE-LOCKED radio it is wrong: every listener has their OWN VFO, so
//     one decoder cannot be everybody's. Measured on the Pi 500's RSP that night: listener B starting
//     WEFAX replaced listener A's RTTY, every decoder socket on the radio got WEFAX, and FT8 made
//     0 decodes in 9 minutes on three VFOs because it was fed the WEFAX listener's audio. Stuart:
//     "that is not how it is designed … that needs fixing".
//  ★★ So the decoders are a HOST, and the shim holds one per LISTENER on a per-VFO radio (each fed
//     that listener's own audio) and exactly one on a shared dial or a one-pipeline radio (fed the
//     one pipeline, mirrored to every decoder socket — the old behaviour, unchanged).
//  ★★ Nobody's decoder is ever replaced by someone else's: a host is only ever driven by the
//     sockets of the session that owns it (per-VFO), or it IS the shared dial's (shared).
//
// ★★★ THE LIMIT IS PER BOX, NOT PER RADIO, because that is where the CPU is. On Linux every radio is
//     its own PROCESS (main.cpp: "the front door forks one process per radio"), so a counter in the
//     shim would be a per-radio limit wearing a box-wide name — three radios would each let N run.
//     FileSlots below is shared by every process on the machine through flock()ed slot files in the
//     runtime directory. flock is released by the KERNEL when a process dies, so a crashed radio
//     cannot leak a slot — the property that rules out a counter in a shared file.
//  ★ One slot per running decoder: the RTTY/NAVTEX/WEFAX/SSTV/time decoder is one, the FT8+FT4
//    spotter is another (they are independent, and a listener may run both). Advanced RDS is not a
//    slot: it is not an audio decoder, it has its own gate (the benchmark's rdsx row and the owner's
//    "Adv RDS" switch), and it costs nothing until somebody opens it.
//  ★ On a shared dial the one decoder serves everybody, so it is ONE slot however many watch it.
//
// ★★★ PRIORITY: DECODERS ARE LAST (Stuart's order, vibe_thread.h: Network > Audio > Spectrum >
//     Decoders). Each host runs its own vibe-decode thread at decoder priority and is handed audio
//     through a bounded queue: a host that falls 4 s behind LOSES AUDIO (counted), the listener never
//     waits for it. One thread per host, not one for the box, so one listener's FT8 burst cannot
//     starve another listener's RTTY either.
#pragma once
#include "decoders/fsk_decoder.h"
#include "decoders/rtty_auto.h"
#include "decoders/wefax_decoder.h"
#include "decoders/sstv_decoder.h"
#include "decoders/time_decoder.h"
#include "decoders/ft8_decoder.h"
#include "decoders/digi_spotter.h"   // ★ V12: digital spots — Single (the listener's audio) or Band (a band's IQ slice)
#include "decoders/digi_bands.h"
#include "vibe_band_slice.h"   // ★ V12: Band's slice sizing and its extra slices
#include "vibe_cw_run.h"
#include "vibe_psk_run.h"
#include "vibe_aprs_run.h"    // ★ V12 APRS (1200 Bd VHF, 300 Bd HF) on the listener's audio — docs/v12/AIS-APRS.md
#include "vibe_dsc_run.h"
#include "vibe_sitora_run.h"
#include "vibe_jtty_run.h"
#include "vibe_clock.h"
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

namespace vibe {

// ── The slots ───────────────────────────────────────────────────────────────────────────────────

/** How many decoders may run at once, and who holds them. A token is what claim() returned; -1 is
 *  "no slot" (refused). */
class DecoderSlots {
public:
    virtual ~DecoderSlots() = default;
    virtual int  claim() = 0;
    virtual void release(int token) = 0;
    /** The limit in force (≥ 1). */
    virtual int  max() const = 0;
    /** Slots held right now — box-wide where the implementation can see the box. */
    virtual int  inUse() const = 0;
};

/** ★★ THE BOX-WIDE SLOTS: slot i is the file <dir>/decoder-slot-<i>.lock, held by an exclusive
 *  non-blocking flock for as long as the decoder runs. Claiming walks 0..max-1 and takes the first
 *  free one; the fd is the token.
 *  ★ flock locks belong to the open file DESCRIPTION, so two opens of the same slot in one process
 *    conflict exactly as two processes do — which is also what makes this testable in one process.
 *  ★ The limit is read through `maxFn` on every claim, so an owner's change applies to the next
 *    decoder started without a restart. A lowered limit never stops a running decoder: slots at or
 *    above the new max are simply never handed out again, and they free as their decoders end.
 *  ★ No directory (Android, a test, a host with no runtime dir): falls back to an in-process count,
 *    which on those hosts IS the box — they run one radio per process and one process. */
class FileSlots : public DecoderSlots {
public:
    FileSlots(std::string dir, std::function<int()> maxFn) : dir_(std::move(dir)), maxFn_(std::move(maxFn)) {}
    ~FileSlots() override {
        std::lock_guard<std::mutex> lk(m_);
        for (int fd : held_) if (fd >= 0) ::close(fd);
    }
    int max() const override { const int m = maxFn_ ? maxFn_() : 1; return m < 1 ? 1 : m; }
    /** ★ Name the shared directory after construction (the host learns it at startup). Ignored while
     *  this process holds a slot, so a claim is always released through the store that granted it. */
    void setDir(const std::string& d) {
        std::lock_guard<std::mutex> lk(m_);
        if (held_.empty() && local_ == 0) dir_ = d;
    }
    int claim() override {
        const int m = max();
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) {
            if (local_ >= m) return -1;
            ++local_;
            return kLocalToken;
        }
        // ★ A LOWERED limit leaves slots at or above it held until their decoders end. The index
        //   bound below would not see those, so the total is checked too — otherwise the first slot
        //   to free under the new limit would be handed straight back out, over it.
        if (inUseLocked() >= m) return -1;
        for (int i = 0; i < m; ++i) {
            const std::string p = path(i);
            const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd < 0) {
                // ★ The directory went away or is not ours: say so once and fall back to counting
                //   here, rather than refusing every decoder on the box for a filesystem reason.
                if (!warned_) { warned_ = true; std::fprintf(stderr, "[decoders] slot file %s: %s — counting in this process only\n", p.c_str(), std::strerror(errno)); }
                dir_.clear();
                if (local_ >= m) return -1;
                ++local_;
                return kLocalToken;
            }
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { held_.push_back(fd); return fd; }
            ::close(fd);
        }
        return -1;
    }
    void release(int token) override {
        if (token < 0) return;
        std::lock_guard<std::mutex> lk(m_);
        if (token == kLocalToken) { if (local_ > 0) --local_; return; }
        auto it = std::find(held_.begin(), held_.end(), token);
        if (it == held_.end()) return;
        held_.erase(it);
        ::flock(token, LOCK_UN);
        ::close(token);
    }
    int inUse() const override {
        std::lock_guard<std::mutex> lk(m_);
        return inUseLocked();
    }
private:
    int inUseLocked() const {
        if (dir_.empty()) return local_;
        int n = 0;
        // ★ Probe each slot with a fresh descriptor: held by anyone — this process included — the
        //   probe fails. Past max() too, so slots held under a limit since lowered still count.
        const int m = std::max(max(), 64);
        for (int i = 0; i < m; ++i) {
            const int fd = ::open(path(i).c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) { if (i >= max()) break; else continue; }
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) ::flock(fd, LOCK_UN); else ++n;
            ::close(fd);
        }
        return n + local_;
    }
    static constexpr int kLocalToken = 1 << 30;
    std::string path(int i) const { return dir_ + "/decoder-slot-" + std::to_string(i) + ".lock"; }
    mutable std::mutex m_;
    mutable std::string dir_;
    std::function<int()> maxFn_;
    std::vector<int> held_;
    int  local_ = 0;
    bool warned_ = false;
};

/** The words a listener sees when the box is full. ONE definition: the server sends it and every
 *  client shows it verbatim, so the web client, the app and Jr cannot drift apart on it. */
/** ★ V12 beta 4: CPU time THIS thread has used, ns (CLOCK_THREAD_CPUTIME_ID — Linux, Android, macOS ≥ 10.12, iOS). Read
 *  by the thread itself around its own work, so nothing samples another thread's clock: the admin page's "% of one
 *  core" for background decoding (BgRunner::statusJson). 0 where the clock is missing. */
inline uint64_t vibeThreadCpuNs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

inline std::string decoderLimitMessage(int max) {
    return "All " + std::to_string(max) + (max == 1 ? " decoder slot" : " decoder slots")
         + " on this server " + (max == 1 ? "is" : "are") + " in use \xe2\x80\x94 try again shortly.";
}

// ── The host ────────────────────────────────────────────────────────────────────────────────────

/** Where a host's output goes: one decoder socket. The shim wraps its WebSocket; a test records. */
struct DecoderPeer {
    virtual ~DecoderPeer() = default;
    virtual bool open() const = 0;
    virtual void binary(const uint8_t* d, size_t n) = 0;
    virtual void text(const std::string& s) = 0;
    /** Bytes queued and not yet on the wire — paces a replay. 0 when unknown. */
    virtual size_t backlog() const { return 0; }
};
using DecoderPeerPtr = std::shared_ptr<DecoderPeer>;

/** ★★★ A SECOND READER OF EVERYTHING A HOST SENDS — the GALLERY's (vibe_gallery.h, V12 2026-10-22). Not a peer: a peer
 *  is a SOCKET, counted to decide "mirror or owner" (addPeer) and pruned when it closes; a tap is the server keeping a
 *  copy, and must change neither. One tap per running decoder, made by Env::tap when the decoder starts and FINISHED
 *  when it stops (a replace, a detach, a listener leaving, a shutdown) — which is the moment a half-received NAVTEX
 *  message or an RTTY buffer is written.
 *  ★ Called on the DECODE thread (text, frame, tick) and on whichever thread stops the decoder (finish) — the tap has
 *    its own lock. It must never call back into the host. */
struct DecoderTap {
    virtual ~DecoderTap() = default;
    /** Decoded text, as the 0x01 text frame carries it. */
    virtual void text(const std::string&) {}
    /** Every binary frame the host broadcasts (0x03 state, 0x07 NAVTEX counts, image lines …). */
    virtual void frame(const uint8_t*, size_t) {}
    /** Once per block of audio decoded — the clock a periodic write or a 75-s "end lost" runs on. */
    virtual void tick() {}
    /** The decoder stopped. Write what is held; `interrupted` = it was cut off, not finished. */
    virtual void finish(bool /*interrupted*/) {}
    /** One digital spot (the spots tap only — see Env::spotTap): the MODE's name ("FT8", "FT4", "WSPR" … — V12
     *  2026-10-26, it was a bool for FT4), the sender's call and grid as received, its SNR, and the RF it was heard on. */
    virtual void spot(const std::string& /*mode*/, const std::string& /*call*/, const std::string& /*grid*/, int /*snr*/, double /*rfHz*/) {}
    /** ★ One CW spot (the CW summary tap only — see Env::cwSpotTap): the call, where, how fast, how strong, and what it
     *  was doing ("CQ", "TEST", "DE"). */
    virtual void cwSpot(const std::string& /*call*/, double /*rfHz*/, float /*wpm*/, float /*snrDb*/, const std::string& /*how*/) {}
    /** ★ V12 APRS: one new station line (vibe_station_log.h — the day's log and summary card). */
    virtual void station(const vibestation::Heard&) {}
    /** ★ One DSC call (V12, 2026-10-26): the call, its readable line and JSON, and whether it repeats one just given. */
    virtual void dscCall(const dsc::Call& /*call*/, const std::string& /*line*/, const std::string& /*json*/, bool /*repeat*/) {}
};
using DecoderTapPtr = std::shared_ptr<DecoderTap>;

class DecoderHost {
public:
    struct Env {
        /** The dial this host's audio is tuned to (Hz) — a spot's RF frequency is dial + audio offset. */
        std::function<double()> dialHz;
        std::function<void(const std::string&)> log;
        /** Box-wide slots; null = unlimited (a decoder-only sidecar serving one app). */
        DecoderSlots* slots = nullptr;
        /** ★ Is the server loaded right now? (vibe_tune_pace.h's rungs: dspCpu, a full demod queue, the CPU
         *  rung, the snail.) FT8/FT4 read it before every EXTRA decode pass — loaded = one pass, today's cost.
         *  Empty = never loaded. */
        std::function<bool()> loaded;
        /** ★★ THE GALLERY'S COPY (2026-10-22): asked once per decoder start with what was started; null = nothing kept.
         *  Empty = no gallery (Android, a test, the decoder-only sidecar). */
        std::function<DecoderTapPtr(const std::string& ext, const std::string& msg)> tap;
        /** ★★ …and FT8/FT4's: asked once when the spotter starts, finished when it stops (the gallery keeps a SUMMARY of
         *  a run, not the spots — Stuart, 2026-10-22). */
        std::function<DecoderTapPtr(bool band)> spotTap;   // ★ V12: `band` = Band mode (one card for the whole band)
        /** ★★ …and CW's (V12, 2026-10-25): asked when a CW decoder or skimmer starts, finished when it stops — a SUMMARY
         *  of the run's spots, as FT8's. `skimmer` says which. */
        std::function<DecoderTapPtr(bool skimmer)> cwSpotTap;
        /** ★★ …and PSK's (V12, 2026-10-26): the same summary of a PSK decoder's or skimmer's spots. */
        std::function<DecoderTapPtr(bool skimmer)> pskSpotTap;
        /** ★ A listener's AUDIO frequency → RF (their dial and mode: USB dial + a, LSB dial − a, this server's CW
         *  dial + a − 600). Empty = dial + audio. A skimmer's IQ carries its own centre. */
        std::function<double(double)> rfOfAudio;
        /** ★★ V12 BAND GOVERNOR (decoders/digi_governor.h, 2026-10-27): the OWNER's word per mode in Band — +1 Always (no
         *  activity gate, never shed), −1 Off, 0 Auto (blocked-modes tokens "spotsband+<mode>" / "spotsband-<mode>").
         *  Empty = Auto for every mode. Asked at each gate period's start (cheap: a CSV lookup). */
        std::function<int(const std::string& mode)> bandOverride;
        /** ★ What the benchmark says this machine affords for Band, in FT8 units (∞ / NaN = no benchmark). Asked once per
         *  Band start. */
        std::function<double()> bandBudgetUnits;
    };
    enum class Start { Ok, Joined, Refused, Unknown };

    DecoderHost(Env env, std::string session) : env_(std::move(env)), session_(std::move(session)) {}
    ~DecoderHost() { shutdown(); }
    DecoderHost(const DecoderHost&) = delete;
    DecoderHost& operator=(const DecoderHost&) = delete;

    const std::string& session() const { return session_; }

    // ── the sockets ────────────────────────────────────────────────────────────────────────────
    /** Attach a decoder socket. @return true when it joined as a MIRROR (another socket already
     *  holds the host) — on a shared dial that is someone else watching the same decoder. */
    bool addPeer(const DecoderPeerPtr& p) {
        std::lock_guard<std::mutex> lk(peerMtx_);
        prunePeersLocked();
        for (auto& q : peers_) if (q == p) return peers_.size() > 1;
        peers_.push_back(p);
        return peers_.size() > 1;
    }
    /** Detach one. @return how many OPEN sockets remain. */
    size_t removePeer(const DecoderPeer* p) {
        std::lock_guard<std::mutex> lk(peerMtx_);
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
            [&](const DecoderPeerPtr& q) { return !q || q.get() == p || !q->open(); }), peers_.end());
        return peers_.size();
    }
    size_t peerCount() {
        std::lock_guard<std::mutex> lk(peerMtx_);
        prunePeersLocked();
        return peers_.size();
    }

    // ── control ────────────────────────────────────────────────────────────────────────────────
    /** Start the audio-extension decoder `ext` (fsk, navtex, wefax, sstv, time or a station name)
     *  with the attach message's parameters. Replaces whatever THIS host was running and keeps its
     *  slot; a host with no slot must claim one first.
     *  ★★ Joined: the SAME decoder with the SAME parameters is already running — the caller is a
     *     second window onto it (a mirror on a shared dial, or a reconnect). It is left running and
     *     `joiner` is caught up on the picture so far, rather than restarting a ten-minute WEFAX
     *     chart because somebody else opened the panel. */
    static constexpr uint64_t kNoCatchUp = ~0ull;
    /** `catchUpSince`: the gap mark taken when THIS joiner's decoder socket went (a held session coming back) — the
     *  pictures finished since then are replayed before the one in progress (replayTo). kNoCatchUp = a plain join. */
    Start start(const std::string& ext, const std::string& msg, const DecoderPeerPtr& joiner = nullptr,
                uint64_t catchUpSince = kNoCatchUp) {
        if (!knownExt(ext)) return Start::Unknown;
        const std::string key = ext + "|" + paramsOf(msg);
        {
            std::lock_guard<std::mutex> lk(decMtx_);
            if (running_() && key == startKey_) {
                // fall through to the replay below, outside the lock
            } else {
                if (textSlot_ < 0) {
                    textSlot_ = env_.slots ? env_.slots->claim() : 0;
                    if (textSlot_ < 0) return Start::Refused;
                }
                clearLocked_();
                startKey_ = key;
                name_ = ext;
                /* ★ NO THROW MAY ESCAPE (audit 2026-10-03): this runs on a socket thread, where an
                 *  uncaught bad_alloc from a decoder's buffers is std::terminate — the whole server,
                 *  for every listener. The parameters are clamped now, so this is the backstop. */
                try { buildLocked_(ext, msg); }
                catch (const std::exception& ex) {
                    clearLocked_(); startKey_.clear(); name_.clear();
                    log(std::string("decoder attach failed: ") + ex.what());
                    return Start::Refused;
                }
                if (env_.tap) { DecoderTapPtr t = env_.tap(ext, msg); std::lock_guard<std::mutex> tl(tapMtx_); tap_ = std::move(t); }
                return Start::Ok;
            }
        }
        if (joiner) replayTo(joiner, catchUpSince);
        // ★ APRS: a second window (a mirror, a reconnect) gets the whole station list with its tracks at once
        if (joiner) { std::string l; { std::lock_guard<std::mutex> lk(decMtx_); if (aprs_) l = aprs_->listJson(true); } if (!l.empty()) joiner->text(l); }
        return Start::Joined;
    }
    /** Stop the audio-extension decoder and give its slot back. */
    void stop() {
        std::lock_guard<std::mutex> lk(decMtx_);
        clearLocked_();
        startKey_.clear(); name_.clear();
        if (textSlot_ >= 0) { if (env_.slots) env_.slots->release(textSlot_); textSlot_ = -1; }
        { std::lock_guard<std::mutex> bl(textMtx_); textBuf_.clear(); morse_.clear(); nvCounts_.clear(); }
    }
    /** ★★ DIGITAL SPOTS — a second, independent slot. SINGLE: every registered mode on the listener's own audio (FT8 +
     *  FT4, the button there always was). BAND (V12, 2026-10-26): the band's IQ slice, every mode on its own dial — see
     *  decoders/digi_spotter.h. Idempotent for the same scope; the other scope replaces the run and keeps the slot. */
    using SpotScope = digi::Spotter::Scope;
    Start startSpots(SpotScope scope = SpotScope::Single) {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (spotsOn_ && spotter_ && spotter_->scope() == scope) return Start::Joined;
        if (spotsSlot_ < 0) {
            spotsSlot_ = env_.slots ? env_.slots->claim() : 0;
            if (spotsSlot_ < 0) return Start::Refused;
        }
        stopSpotterLocked_(false);
        gapClear(GapSpots);   // ★ a new run: the last one's spots are not "missed" by it (the gap log)
        diagAt_ = 0;   // ★ Band's minute line starts again with the new run
        /* ★★ EXTRA FT8 PASSES ONLY ON SPARE CPU (2026-10-05): the server says it is loaded, or this host's own
         *  queue is a second behind (the decode thread is not keeping up) — either way, one pass. */
        auto loaded = [this] {
            if (env_.loaded && env_.loaded()) return true;
            return queued() > 48000 || queuedIqSec() > 1.0;
        };
        auto dial = env_.dialHz;
        spotter_.reset(new digi::Spotter(scope, digi::modes(),
            [this](const digi::Spot& sp, double rfHz) { emitSpot(sp, rfHz); },
            [dial] { return dial ? dial() : 0.0; }, loaded));
        if (scope == SpotScope::Band) {
            if (env_.bandOverride) {
                auto ov = env_.bandOverride;
                spotter_->setOverrides([ov](const std::string& m) {
                    const int v = ov(m);
                    return v > 0 ? digi::Override::Always : v < 0 ? digi::Override::Off : digi::Override::Auto;
                });
            }
            const double u = env_.bandBudgetUnits ? env_.bandBudgetUnits() : NAN;
            spotter_->setBudgetCap(std::isfinite(u) && u > 0 ? u : INFINITY);
            statusSent_ = ~0ull; statusAt_ = 0;
        }
        if (env_.spotTap) { DecoderTapPtr t = env_.spotTap(scope == SpotScope::Band); std::lock_guard<std::mutex> tl(tapMtx_); spotTap_ = std::move(t); }
        spotsOn_ = true;
        if (scope == SpotScope::Band) {
            bandWidth_.store(bandWidthFor(dial ? dial() : 0.0), std::memory_order_relaxed);   // ★ a socket thread: no channel lock held
            setExtras_(dial ? dial() : 0.0);
            if (!bandIq_.exchange(true)) skimmersRunning().fetch_add(1);
        } else {
            spotsActive_.store(true, std::memory_order_relaxed);
        }
        log(scope == SpotScope::Band ? "digital spots: Band started" : "digital spots: Single mode started");
        return Start::Ok;
    }
    void stopSpots() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        stopSpotterLocked_(true);
        if (spotsSlot_ >= 0) { if (env_.slots) env_.slots->release(spotsSlot_); spotsSlot_ = -1; }
    }
    /** "single" / "band" / "" — what the admin page and the clients are told. */
    std::string spotsScope() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotsOn_ || !spotter_) return "";
        return spotter_->scope() == SpotScope::Band ? "band" : "single";
    }
    /** Slots every spots engine has skipped (monotonic, survives a band change) — the health pill's "TOO HEAVY". */
    uint64_t spotSlotsSkipped() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        return spotSkipBase_ + (spotter_ ? spotter_->slotsSkipped() : 0);
    }
    /** ★ Band's gate periods SHED over budget (monotonic, survives a band change) — the health pill's line for it. */
    uint64_t spotPeriodsShed() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        return spotShedBase_ + (spotter_ ? spotter_->periodsShed() : 0);
    }
    /** ★ Band's per-mode state as JSON (the {"type":"digital_spots_status"} frame's data) — "" when Band is not running. */
    std::string spotsStatusJson() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotter_ || spotter_->scope() != SpotScope::Band) return "";
        return statusJsonLocked_();
    }
    /** What the spotter runs now ("FT8 14.0740, FT4 14.0800"), for the admin page. */
    std::string spotsDescribe() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        return spotter_ ? spotter_->describe() : std::string();
    }
    /** ★ The IQ width Band mode wants for a dial: the registered modes' span of the band it is on, wide enough either side
     *  of the dial to hold it all (a dial typed off-centre still decodes the whole band). Off any band: 24 kHz. */
    static double bandWidthFor(double dialHz) { return DecoderHostBandWidth(dialHz); }   // ★ vibe_band_slice.h (one rule)
    /** ★★ V12 (2026-10-27): Band's EXTRA slices for the band the main slice is on — its clusters beyond the main one (40 m's
     *  JTTY, 6 m's JTTY, 15/10 m's FT4 …: digi_bands.h Span::extras). Atomic reads only (the DSP thread asks, with the
     *  listener's channel lock held, as for iqWidthHz). Returns how many were written to `out` (≤ kMaxExtraSlices). */
    int iqExtraSlices(ExtraSlice* out) const {
        if (!bandIq_.load(std::memory_order_relaxed)) return 0;
        const int n = std::min(kMaxExtraSlices, nExtras_.load(std::memory_order_acquire));
        for (int k = 0; k < n; ++k) { out[k].centreHz = exC_[k].load(std::memory_order_relaxed); out[k].widthHz = exW_[k].load(std::memory_order_relaxed); }
        return n;
    }
    /** Everything off, the queue drained and its thread joined. Idempotent. */
    void shutdown() {
        stopQueue_();
        stop();
        stopSpots();
    }

    // ── audio in ───────────────────────────────────────────────────────────────────────────────
    /** Anything running that wants audio? Cheap: read on every block of a listener's audio. */
    bool wantsAudio() const { return active_.load(std::memory_order_relaxed) || spotsActive_.load(std::memory_order_relaxed); }
    /** ★ How many CW skimmers run in this process — the one atomic every listener's channel block reads before it looks
     *  for its host (so a radio with none pays nothing). */
    static std::atomic<int>& skimmersRunning() { static std::atomic<int> n{0}; return n; }
    /** ★ A SKIMMER (CW or PSK) is running — it wants complex IQ (feedIq), not audio. Cheap: read on every channel block. */
    bool wantsIq() const { return cwIq_.load(std::memory_order_relaxed) || bandIq_.load(std::memory_order_relaxed); }
    /** ★ The skimmer's segment width asked for (Hz) — what the caller cuts its slice to. 0 = none running. */
    double iqWidthHz() const {
        // ★★ The CW skimmer and Band spots share ONE slice (both centred on the dial): the wider of the two. The skimmer
        //    decodes only its own width of a wider slice (CwRun clamps), and Band mode's tuners take only their dials.
        // ★★★ AN ATOMIC READ ONLY: this is asked on the DSP thread with the listener's channel lock held
        //     (feedOneClient), where asking for the dial (env_.dialHz takes clientMtx) would be a lock-order inversion.
        //     The width follows the dial from the decode thread instead (loop_: bandWidthFor(the slice's own centre)).
        double w = cwIq_.load(std::memory_order_relaxed) ? cwWidth_.load(std::memory_order_relaxed) : 0.0;
        if (bandIq_.load(std::memory_order_relaxed)) w = std::max(w, bandWidth_.load(std::memory_order_relaxed));
        return w;
    }
    /** ★ Band spots wants the slice's FFT bins' power with each block (its activity gate — digi_activity.h). Cheap. */
    bool wantsBins() const { return bandIq_.load(std::memory_order_relaxed); }
    /** Hand the skimmer `n` IQ samples at `rate` Hz centred on `centreRfHz`. Copies and returns at once, as feed().
     *  ★ `binPow`/`nb`/`binHz`: the slice's bins' power for this block (digi::slicePower), for Band's gate; null = none. */
    /** ★ `captureUtc`: the capture time of the block's LAST sample (the one-pipeline tap works it out from the DSP thread's
     *  stamp — vibe_wide_tap.h); NaN = now (the per-listener extract runs on the DSP thread itself). `slice`: 0 = Band's
     *  main slice (and the CW/PSK skimmers'), 1… = an extra slice (iqExtraSlices), for Band only. */
    void feedIq(const std::complex<float>* iq, int n, double rate, double centreRfHz,
                const float* binPow = nullptr, int nb = 0, double binHz = 0, double captureUtc = NAN, int slice = 0) {
        if (n <= 0 || !wantsIq() || !(rate > 0)) return;
        if (slice > 0 && !bandIq_.load(std::memory_order_relaxed)) return;
        IqBlock b; b.rate = rate; b.centre = centreRfHz; b.slice = slice;
        // ★ V12 2026-10-27: a NULL block is a GAP the tap dropped — `n` samples to bridge with zeros (Spotter::feedGap)
        if (!iq) { if (!bandIq_.load(std::memory_order_relaxed)) return; b.gapN = n; binPow = nullptr; }
        else b.v.assign(iq, iq + n);
        if (binPow && nb > 0 && binHz > 0 && wantsBins()) { b.pw.assign(binPow, binPow + nb); b.binHz = binHz; }
        // ★ Band spots cut UTC slots by it — the CAPTURE time, never when the block reached here or was decoded
        b.utc = std::isfinite(captureUtc) ? captureUtc : vibeUtcNow();
        fed_.fetch_add((uint64_t)n, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(qM_);
            if (!qThread_.joinable()) { qStop_ = false; replayStop_.store(false); qThread_ = std::thread([this] { loop_(); }); }
            qiFrames_ += b.v.size();
            if (slice == 0) qiMainSec_ += (double)n / rate;   // ★ a gap counts: bridging it is work too
            qi_.push_back(std::move(b));
            // ★ DROP, NEVER WAIT — two seconds of the main slice behind and the oldest goes (extra slices with it).
            while (qi_.size() > 1 && qiMainSec_ > 2.0) {
                const IqBlock& f = qi_.front();
                qiFrames_ -= f.v.size();
                if (f.slice == 0) qiMainSec_ -= (double)(f.v.size() + (size_t)f.gapN) / f.rate;
                dropped_.fetch_add(f.v.size(), std::memory_order_relaxed);
                iqDropped_.fetch_add(1, std::memory_order_relaxed);
                qi_.pop_front();
            }
        }
        qCv_.notify_one();
    }
    /** ★ Band's IQ blocks dropped behind the decode thread (monotonic) — the health pill's TOO HEAVY line reads it. */
    uint64_t iqBlocksDropped() const { return iqDropped_.load(std::memory_order_relaxed); }
    /** Hand the decoders `count` frames of 48 kHz audio, `stride` floats apart (1 = mono, 2 = take the
     *  left of interleaved stereo). Copies and returns at once — NEVER decodes on the caller's thread.
     *  ★ `captureUtc`: the corrected UTC of the block's LAST frame, stamped HERE, on the audio thread — the
     *    decode thread may run seconds behind, and FT8's slot is cut by when the audio was heard, not decoded
     *    (2026-10-05). NaN = now (every server caller); a test feeding faster than real time passes its own. */
    void feed(const float* pcm, int count, int stride = 1, double captureUtc = NAN) {
        if (count <= 0 || !wantsAudio()) return;
        if (!std::isfinite(captureUtc)) captureUtc = vibeUtcNow();
        std::vector<float> v((size_t)count);
        for (int i = 0; i < count; ++i) v[(size_t)i] = pcm[(size_t)i * (size_t)stride];
        fed_.fetch_add((uint64_t)count, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(qM_);
            if (!qThread_.joinable()) { qStop_ = false; replayStop_.store(false); qThread_ = std::thread([this] { loop_(); }); }
            qFrames_ += v.size();
            q_.push_back(std::move(v));
            qUtc_.push_back(captureUtc);
            // ★ DROP, NEVER WAIT — see the file note. The oldest goes: a decoder resumes on fresh
            //   audio, and 4 s is a whole WEFAX line and more than an FT8 symbol run.
            while (qFrames_ > kMaxFrames && q_.size() > 1) {
                qFrames_ -= q_.front().size();
                dropped_.fetch_add(q_.front().size(), std::memory_order_relaxed);
                q_.pop_front();
                qUtc_.pop_front();
            }
        }
        qCv_.notify_one();
    }

    // ── what the admin page reads ──────────────────────────────────────────────────────────────
    std::string name() { std::lock_guard<std::mutex> lk(decMtx_); return name_; }
    bool spotsOn() { std::lock_guard<std::mutex> lk(spotsMtx_); return spotsOn_; }
    /** The slots this host holds (0, 1 or 2). */
    int slotsHeld() {
        int n = 0;
        { std::lock_guard<std::mutex> lk(decMtx_); if (textSlot_ >= 0) ++n; }
        { std::lock_guard<std::mutex> lk(spotsMtx_); if (spotsSlot_ >= 0) ++n; }
        return n;
    }
    uint64_t fedSamples() const { return fed_.load(std::memory_order_relaxed); }
    uint64_t droppedSamples() const { return dropped_.load(std::memory_order_relaxed); }
    /** ★ V12 beta 4: the decode thread's CPU, ns, since this host was made (its own clock, read around each block). */
    uint64_t cpuNs() const { return cpuNs_.load(std::memory_order_relaxed); }
    /** ★ The FSK decoder's state as it last said (0 no signal, 1–2 hunting, 3 locked — the 0x03 op); -1 = none running. */
    int fskState() const { return fskState_.load(std::memory_order_relaxed); }
    /** Frames waiting for the decode thread — a feeder that must NOT lose audio (a test, the
     *  benchmark) paces itself on this; the server never does, it drops. */
    size_t queued() { std::lock_guard<std::mutex> lk(qM_); return qFrames_; }
    /** "fsk"/"wefax"/"sstv"/"time"/"none" — WHICH decoder object actually exists (see the admin note
     *  on decoderKind: "attached" was once true while nothing had been constructed). */
    std::string kind() {
        std::lock_guard<std::mutex> lk(decMtx_);
        return wefax_ ? "wefax" : sstv_ ? "sstv" : (fsk_ || rttyAuto_) ? "fsk" : time_ ? "time" : cw_ ? (cw_->skimmer() ? "cwskim" : "cw")
             : psk_ ? (psk_->skimmer() ? "pskskim" : "psk") : dsc_ ? "dsc" : sitora_ ? "sitora" : aprs_ ? "aprs" : jtty_ ? "jtty" : "none";
    }
    /** RTTY health for the admin page, or false when no FSK decoder runs. */
    bool fskHealth(unsigned long& resyncs, double& level, double& threshold, int& state) {
        std::lock_guard<std::mutex> lk(decMtx_);
        if (rttyAuto_) {
            resyncs = rttyAuto_->resyncs(); level = rttyAuto_->audioLevel(); threshold = rttyAuto_->audioThreshold();
            state = rttyAuto_->stateNow(); return true;
        }
        if (!fsk_) return false;
        resyncs = fsk_->resyncs(); level = fsk_->audioLevel(); threshold = fsk_->audioThreshold(); state = fsk_->stateNow();
        return true;
    }

    /** Send every attached socket a frame. */
    void broadcast(const uint8_t* d, size_t n) {
        for (auto& p : snapshotPeers()) p->binary(d, n);
        record(d, n);
        if (DecoderTapPtr t = tap()) t->frame(d, n);
    }
    /** `gap`: which gap log keeps it for a listener coming back (GapDecoder / GapSpots), or -1 = not kept (a status frame
     *  that is re-sent anyway). */
    void broadcastText(const std::string& s, int gap = 0 /* GapDecoder */) {
        for (auto& p : snapshotPeers()) p->text(s);
        if (gap >= 0) gapNote((uint8_t)gap, false, s.data(), s.size());
    }

    /* ── ★★★ THE GAP LOG — WHAT A LISTENER WHO WAS AWAY MISSED (V12 held session, 2026-10-27) ──────────────────────────
     *  Stuart: "when the OS suspends the app … your session on the server is held for 3 minutes all decoders are still
     *  running for this period and no data is missed". The decoders DO keep running through the hold (the shim keeps the
     *  host) — but every line, spot and call they produced went to sockets that were not there, and nothing kept them:
     *  the image stream has always had `replay_`, text never did. So the last few minutes of output are kept here, each
     *  stamped with a sequence number; the shim notes the number when a listener's decoder socket goes (gapMark) and,
     *  once it is back and has re-attached, sends what came after it (replayGap), paced like the image replay.
     *  ★ Bounded (kGapMaxBytes / kGapMaxEntries, oldest out first) — three minutes of a busy Band FT8 run fits.
     *  ★ A NEW run of a decoder clears its own entries (clearLocked_ / a new spotter): the lines of the RTTY you closed
     *    are not "missed" by the NAVTEX you opened. Status frames (state bytes, chips) are not kept — they are re-sent. */
    enum : uint8_t { GapDecoder = 0, GapSpots = 1 };
    /** The sequence number of the newest entry — everything up to it was sent while the socket was there. */
    uint64_t gapMark() {
        // ★★ somebody's decoder socket has gone while this decoder runs: from now the pictures it finishes are kept for
        //    them (see THE PICTURES FINISHED WHILE AWAY), for as long as a gap mark is honoured (the shim's 15 minutes)
        { std::lock_guard<std::mutex> rl(replayMtx_); awayUntil_ = steadySecs() + kAwayKeepS; }
        std::lock_guard<std::mutex> gl(gapMtx_); return gapSeq_;
    }
    /** Send `p` every entry of `kind` after `since`, paced on a thread of its own. @return how many. */
    size_t replayGap(const DecoderPeerPtr& p, uint64_t since, uint8_t kind) {
        std::vector<GapEntry> out;
        { std::lock_guard<std::mutex> gl(gapMtx_);
          for (const auto& e : gap_) if (e.seq > since && e.kind == kind) out.push_back(e); }
        if (out.empty() || !p || !p->open()) return 0;
        const size_t n = out.size();
        log(std::string("decoder rejoined — replaying ") + std::to_string(n) + (kind == GapSpots ? " spots" : " decoder messages")
            + " sent while the listener was away");
        std::lock_guard<std::mutex> tl(replayThrMtx_);
        replayThreads_.erase(std::remove_if(replayThreads_.begin(), replayThreads_.end(),
            [](std::unique_ptr<ReplayJob>& j) { if (j->done.load()) { if (j->th.joinable()) j->th.join(); return true; } return false; }),
            replayThreads_.end());
        auto job = std::make_unique<ReplayJob>();
        ReplayJob* jp = job.get();
        jp->th = std::thread([this, p, out = std::move(out), jp] {
            vibeDecoderThread("vibe-gap");
            for (auto& e : out) {
                for (int w = 0; w < 500 && p->open() && p->backlog() > kReplayBacklog && !replayStop_.load(); ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (!p->open() || replayStop_.load()) break;
                if (e.bin) p->binary((const uint8_t*)e.data.data(), e.data.size()); else p->text(e.data);
            }
            jp->done.store(true);
        });
        replayThreads_.push_back(std::move(job));
        return n;
    }

    /** ★★★ CATCH A LATE ARRIVAL UP ON THE PICTURE SO FAR — PACED. A full WEFAX chart is ~2 MB of
     *  lines, four times a listener's whole outbox; sending it in one go dropped the joiner for
     *  backlog (1006) before its own attach was read, and the client's 3-s reconnect then did it
     *  again, for ever (Pi 500, 2026-09-30: 1042 frames replayed, closed, repeated). So it goes at
     *  the pace the socket drains, on a thread of its own, and gives up if the socket closes. */
    void replayTo(const DecoderPeerPtr& p, uint64_t since = kNoCatchUp) {
        std::vector<std::vector<uint8_t>> frames;
        size_t pics = 0;
        { std::lock_guard<std::mutex> rl(replayMtx_);
          // ★★ the pictures FINISHED while this listener was away, oldest first, each ending in its own "complete" frame —
          //    so every client files it exactly as it files one finished live — then the one in progress
          if (since != kNoCatchUp)
              for (const DonePic& d : done_)
                  if (d.seq > since) { frames.insert(frames.end(), d.frames.begin(), d.frames.end()); ++pics; }
          frames.insert(frames.end(), replay_.begin(), replay_.end()); }
        if (frames.empty() || !p || !p->open()) return;
        if (pics) log("decoder rejoined — replaying " + std::to_string(pics) + (pics == 1 ? " picture" : " pictures")
                      + " finished while the listener was away, then the one in progress");
        log("decoder joined — replaying " + std::to_string(frames.size()) + " frames of the image so far");
        std::lock_guard<std::mutex> tl(replayThrMtx_);
        replayThreads_.erase(std::remove_if(replayThreads_.begin(), replayThreads_.end(),
            [](std::unique_ptr<ReplayJob>& j) { if (j->done.load()) { if (j->th.joinable()) j->th.join(); return true; } return false; }),
            replayThreads_.end());
        auto job = std::make_unique<ReplayJob>();
        ReplayJob* jp = job.get();
        jp->th = std::thread([this, p, frames = std::move(frames), jp] {
            vibeDecoderThread("vibe-replay");
            for (auto& f : frames) {
                for (int w = 0; w < 500 && p->open() && p->backlog() > kReplayBacklog && !replayStop_.load(); ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (!p->open() || replayStop_.load()) break;
                p->binary(f.data(), f.size());
            }
            jp->done.store(true);
        });
        replayThreads_.push_back(std::move(job));
    }

private:
    struct ReplayJob { std::thread th; std::atomic<bool> done{false}; };
    struct GapEntry { uint64_t seq; uint8_t kind; bool bin; std::string data; };
    static constexpr size_t kGapMaxBytes = 384u * 1024, kGapMaxEntries = 4000;
    std::mutex gapMtx_;
    std::deque<GapEntry> gap_;
    size_t gapBytes_ = 0;
    uint64_t gapSeq_ = 0;
    void gapNote(uint8_t kind, bool bin, const void* d, size_t n) {
        if (!n || n > 64u * 1024) return;
        std::lock_guard<std::mutex> gl(gapMtx_);
        gap_.push_back(GapEntry{ ++gapSeq_, kind, bin, std::string((const char*)d, n) });
        gapBytes_ += n;
        while (!gap_.empty() && (gapBytes_ > kGapMaxBytes || gap_.size() > kGapMaxEntries)) {
            gapBytes_ -= gap_.front().data.size(); gap_.pop_front();
        }
    }
    /** A new run of `kind`: its predecessor's output is not "missed" by it. */
    void gapClear(uint8_t kind) {
        std::lock_guard<std::mutex> gl(gapMtx_);
        for (auto it = gap_.begin(); it != gap_.end(); ) {
            if (it->kind == kind) { gapBytes_ -= it->data.size(); it = gap_.erase(it); } else ++it;
        }
    }
    static constexpr size_t kMaxFrames = 48000 * 4;          // 4 s of audio behind = drop
    static constexpr size_t kReplayMax = 4u * 1024 * 1024;
    static constexpr size_t kReplayBacklog = 128u * 1024;    // pace a replay below this backlog

    static bool knownExt(const std::string& e) {
        return e == "fsk" || e == "navtex" || e == "wefax" || e == "sstv" || e == "time"
            || e == "msf" || e == "dcf77" || e == "rwm" || e == "wwv" || e == "wwvb" || e == "cw" || e == "cwskim" || e == "dsc" || e == "sitora" || e == "aprs" || e == "psk" || e == "pskskim" || e == "jtty";
    }
    /** ★ "Same decoder, same settings": the parameters a decoder is built from, in a fixed order.
     *  A whitelist rather than "the message minus its envelope", so field ORDER, a session id or a
     *  future harmless field cannot make an identical request look different and restart a chart. */
    static std::string paramsOf(const std::string& msg) {
        std::string k;
        for (const char* f : { "center_frequency", "shift", "baud_rate", "lpm", "image_width", "carrier",
                               "deviation", "bandwidth", "width", "centre" }) {
            double v; k += num(msg, f, v) ? std::to_string(v) : std::string("-"); k += ',';
        }
        for (const char* f : { "encoding", "framing", "station", "band" }) { k += str(msg, f); k += ','; }
        for (const char* f : { "\"inverted\":true", "\"use_phasing\":false", "\"auto_stop\":true", "\"auto_start\":true",
                               "\"auto\":true", "\"usos\":true" })
            k += msg.find(f) != std::string::npos ? '1' : '0';
        return k;
    }
    // ── tiny JSON readers (the same shapes the shim's jsonNum/jsonStr accept) ──
    static bool num(const std::string& j, const char* k, double& out) {
        const std::string key = std::string("\"") + k + "\"";
        size_t p = j.find(key); if (p == std::string::npos) return false;
        p = j.find(':', p + key.size()); if (p == std::string::npos) return false;
        ++p; while (p < j.size() && (j[p] == ' ' || j[p] == '"')) ++p;
        char* e = nullptr; const double v = std::strtod(j.c_str() + p, &e);
        if (e == j.c_str() + p) return false;
        /* ★★ strtod reads "nan" and "inf" (audit 2026-10-03), and every number here sizes a buffer
         *  or divides something in a decoder. A non-finite value is treated as absent: the default. */
        if (!std::isfinite(v)) return false;
        out = v; return true;
    }
    static std::string str(const std::string& j, const char* k) {
        const std::string key = std::string("\"") + k + "\"";
        size_t p = j.find(key); if (p == std::string::npos) return "";
        p = j.find(':', p + key.size()); if (p == std::string::npos) return "";
        p = j.find('"', p); if (p == std::string::npos) return "";
        const size_t e = j.find('"', p + 1); if (e == std::string::npos) return "";
        return j.substr(p + 1, e - p - 1);
    }

    bool running_() const { return fsk_ || rttyAuto_ || wefax_ || sstv_ || time_ || cw_ || psk_ || dsc_ || sitora_ || aprs_ || jtty_; }
    void clearLocked_() {
        // ★ The gallery's copy is finished FIRST, while nothing more can arrive: what it holds is written now.
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t.swap(tap_); }
          if (t) t->finish(running_()); }
        delete fsk_;   fsk_ = nullptr;
        delete rttyAuto_; rttyAuto_ = nullptr;
        fskState_.store(-1, std::memory_order_relaxed);
        delete wefax_; wefax_ = nullptr;
        delete sstv_;  sstv_ = nullptr;
        delete time_;  time_ = nullptr;
        if (cwIq_.exchange(false)) skimmersRunning().fetch_sub(1);
        cw_.reset();
        psk_.reset();
        aprs_.reset();
        dsc_.reset();
        sitora_.reset();
        jtty_.reset();
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t.swap(cwTap_); } if (t) t->finish(true); }
        if (!bandIq_.load()) { std::lock_guard<std::mutex> lk(qM_); qi_.clear(); qiFrames_ = 0; }   // ★ Band spots may still want it
        active_.store(false, std::memory_order_relaxed);
        imageKind_.store(0, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> rl(replayMtx_); replay_.clear(); replayBytes_ = 0; replayDoneSeq_ = 0; done_.clear(); doneBytes_ = 0; }
        gapClear(GapDecoder);   // ★ a new run — see the gap log
    }
    void log(const std::string& s) { if (env_.log) env_.log(s); }
    std::vector<DecoderPeerPtr> snapshotPeers() {
        std::lock_guard<std::mutex> lk(peerMtx_);
        prunePeersLocked();
        return peers_;
    }
    void prunePeersLocked() {
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
            [](const DecoderPeerPtr& q) { return !q || !q->open(); }), peers_.end());
    }
    /** ★ Only the image stream is replayable; 0x02 alone is a new image and restarts the log. */
    void record(const uint8_t* d, size_t n) {
        if (n == 0) return;
        // ★ Read from an atomic, not name_: SSTV calls this from its own video thread.
        const int kind = imageKind_.load(std::memory_order_relaxed);
        if (kind == 0) return;
        std::lock_guard<std::mutex> rl(replayMtx_);
        const bool starts = (kind == 1 && n == 1 && d[0] == 0x02) || (kind == 2 && n >= 9 && d[0] == 0x07);
        if (starts) { archiveLocked_(kind); replay_.clear(); replayBytes_ = 0; replayDoneSeq_ = 0; }
        // ★ WEFAX 0x03 "transmission complete", SSTV 0x05 "image complete": the picture is finished — stamped now, so a
        //   listener who left AFTER it finished is not sent it again
        const bool ends = (kind == 1 && n == 1 && d[0] == 0x03) || (kind == 2 && d[0] == 0x05);
        if (ends) { std::lock_guard<std::mutex> gl(gapMtx_); replayDoneSeq_ = ++gapSeq_; }
        if (replayBytes_ + n > kReplayMax && !ends) return;   // ★ the end marker always goes in (it is a byte or two)
        replay_.emplace_back(d, d + n);
        replayBytes_ += n;
    }
    /* ── ★★★ THE PICTURES FINISHED WHILE AWAY (V12 beta24, 2026-10-27) ─────────────────────────────────────────────────
     *  The held session promises "no data is missed" (d93ad592d). The gap log carries text and spots, and replay_ the
     *  picture in progress — but a picture FINISHED during the hold, with the next one already started, was cleared by
     *  that start and lost: a returning listener got the new chart and never the one before it. So while somebody is
     *  away (a gap mark taken in the last kAwayKeepS — gapMark), the picture a new start would clear is MOVED (no copy)
     *  into done_, stamped with the gap sequence at which it finished; a joiner coming back with its mark (start's
     *  catchUpSince) is sent the ones finished after it, oldest first, then the one in progress.
     *  ★ THE CAP: the last kDoneMax (2) pictures and at most kReplayMax (4 MB) between them, forgotten after kAwayKeepS.
     *    A 3-minute hold finishes at most one WEFAX chart (they run 10–20 minutes; the one finishing may have started long
     *    before) or a few SSTV pictures (36 s–4 min, ~250 KB each), so two covers it. Only while somebody is away: a host
     *    everybody is watching keeps nothing extra. Against a 1 GB Pi 2 (or a 32-bit 1 GB phone serving its own app) that
     *    is 4 MB beside the 4 MB the picture in progress may already hold — per image decoder that has an absent listener,
     *    and a single-listener radio has at most one. Memory goes back the moment the run stops (clearLocked_). */
    struct DonePic { uint64_t seq = 0; double at = 0; size_t bytes = 0; std::vector<std::vector<uint8_t>> frames; };
    static constexpr size_t kDoneMax = 2;
    static constexpr double kAwayKeepS = 900.0;   // ★ the shim forgets a gap mark after 15 minutes (dxGapNote)
    static double steadySecs() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    /** replayMtx_ held: the picture in replay_ is about to be cleared by a new start — keep it if somebody is away. */
    void archiveLocked_(int kind) {
        const double now = steadySecs();
        while (!done_.empty() && now - done_.front().at > kAwayKeepS) { doneBytes_ -= done_.front().bytes; done_.pop_front(); }
        if (replay_.empty() || now >= awayUntil_) return;
        DonePic p;
        if (replayDoneSeq_) p.seq = replayDoneSeq_;
        else {
            // ★ ended by the next start, not by its own end marker: one is added, so the client files it as finished
            std::lock_guard<std::mutex> gl(gapMtx_); p.seq = ++gapSeq_;
            replay_.push_back({ (uint8_t)(kind == 1 ? 0x03 : 0x05) });
            replayBytes_ += 1;
        }
        p.at = now; p.bytes = replayBytes_; p.frames = std::move(replay_);
        replay_.clear();
        doneBytes_ += p.bytes;
        done_.push_back(std::move(p));
        while (!done_.empty() && (done_.size() > kDoneMax || doneBytes_ > kReplayMax)) {
            doneBytes_ -= done_.front().bytes; done_.pop_front();
        }
    }
    void textFrame(const std::string& text) {
        std::vector<uint8_t> m(13 + text.size());
        m[0] = 0x01;
        const uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int i = 0; i < 8; i++) m[1 + i] = (uint8_t)(ts >> ((7 - i) * 8));   // big-endian
        const uint32_t len = (uint32_t)text.size();
        m[9] = (uint8_t)(len >> 24); m[10] = (uint8_t)(len >> 16); m[11] = (uint8_t)(len >> 8); m[12] = (uint8_t)len;
        std::memcpy(m.data() + 13, text.data(), text.size());
        for (auto& p : snapshotPeers()) p->binary(m.data(), m.size());
        gapNote(GapDecoder, true, m.data(), m.size());   // ★ the gap log — for a listener coming back
        if (DecoderTapPtr t = tap()) t->text(text);
    }
    static void put32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
        v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
    }

    /** Build the decoder named by `ext`. Call with decMtx_ held. The bodies are the shim's own
     *  startDecoder/startWefax/startTime/startSstv, moved here unchanged in behaviour. */
    void buildLocked_(const std::string& ext, const std::string& msg) {
        if (ext == "wefax") {
            WefaxDecoder::Config cfg;
            double v;
            /* ★ Clamped BEFORE the int cast — (int)1e300 is undefined behaviour, not a big number.
             *  The decoder then whitelists lpm and clamps the width itself (audit 2026-10-03). */
            auto toInt = [](double x) { return (int)std::max(-1.0e6, std::min(1.0e6, x)); };
            if (num(msg, "lpm", v))         cfg.lpm        = toInt(v);
            if (num(msg, "image_width", v)) cfg.imageWidth = toInt(v);
            if (num(msg, "carrier", v))     cfg.carrier    = v;
            if (num(msg, "deviation", v))   cfg.deviation  = v;
            if (num(msg, "bandwidth", v))   cfg.bandwidth  = toInt(v);
            cfg.usePhasing = msg.find("\"use_phasing\":false") == std::string::npos;
            cfg.autoStop   = msg.find("\"auto_stop\":true")    != std::string::npos;
            cfg.autoStart  = msg.find("\"auto_start\":true")   != std::string::npos;
            wefax_ = new WefaxDecoder(48000, cfg);
            imageKind_.store(1, std::memory_order_relaxed);
            wefax_->onLine = [this](uint32_t ln, uint32_t w, const uint8_t* px) {
                std::vector<uint8_t> m(9 + w);
                m[0] = 0x01;
                m[1] = (uint8_t)(ln >> 24); m[2] = (uint8_t)(ln >> 16); m[3] = (uint8_t)(ln >> 8); m[4] = (uint8_t)ln;
                m[5] = (uint8_t)(w >> 24);  m[6] = (uint8_t)(w >> 16);  m[7] = (uint8_t)(w >> 8);  m[8] = (uint8_t)w;
                std::memcpy(m.data() + 9, px, w);
                broadcast(m.data(), m.size());
            };
            wefax_->onStart = [this]() { uint8_t b = 0x02; broadcast(&b, 1); };
            wefax_->onStop  = [this]() { uint8_t b = 0x03; broadcast(&b, 1); };
            // ★ 0x04 <phase> — start tone / phasing / image / stop tone, on change (older clients ignore it).
            wefax_->onPhase = [this](int ph) { uint8_t b[2] = { 0x04, (uint8_t)ph }; broadcast(b, 2); };
            // ★★ WHY a chart was or was not phased (2026-10-07) — into the log, so Export logs answers it after a miss.
            /* ★★ …and to the CLIENT as 0x08 <utf-8 text>: on the app's Kiwi path this host runs on the phone, where the
             *  native log is not in Export logs — the client files it with its decisions, which are. Rare (a few lines a
             *  chart). Older clients ignore an opcode they do not know. */
            wefax_->onDiag = [this](const std::string& m) {
                log("wefax: " + m);
                std::vector<uint8_t> b(1 + m.size()); b[0] = 0x08; std::memcpy(b.data() + 1, m.data(), m.size());
                broadcast(b.data(), b.size());
            };
            log("decoder attached: wefax lpm=" + std::to_string(cfg.lpm) + " width=" + std::to_string(cfg.imageWidth));
        } else if (ext == "sstv") {
            // ★ autoSync ON — see the long note that lived in the shim's startSstv (redrawFromLuminance
            //   measures HOW FAR a correction overruns; proven on the four Essex Ham recordings).
            sstv_ = new SstvDecoder(12000, /*autoSync=*/true);
            imageKind_.store(2, std::memory_order_relaxed);
            sstvDecim_ = 0; sstvAcc_ = 0.0f;
            sstv_->onImageStart = [this](int w, int h) {
                std::vector<uint8_t> m; m.push_back(0x07); put32(m, (uint32_t)w); put32(m, (uint32_t)h);
                sendSstv(m);
            };
            sstv_->onLine = [this](int y, int w, const uint8_t* rgb) {
                std::vector<uint8_t> m; m.reserve(9 + (size_t)w * 3);
                m.push_back(0x01); put32(m, (uint32_t)y); put32(m, (uint32_t)w);
                m.insert(m.end(), rgb, rgb + (size_t)w * 3);
                sendSstv(m);
            };
            sstv_->onMode = [this](uint8_t, const std::string& name) {
                std::vector<uint8_t> m; m.push_back(0x02);
                m.push_back((uint8_t)(name.size() >> 8)); m.push_back((uint8_t)name.size());
                m.insert(m.end(), name.begin(), name.end());
                sendSstv(m);
            };
            sstv_->onStatus = [this](const std::string& s) {
                std::vector<uint8_t> m; m.push_back(0x03); m.push_back(0x00);
                m.push_back((uint8_t)(s.size() >> 8)); m.push_back((uint8_t)s.size());
                m.insert(m.end(), s.begin(), s.end());
                sendSstv(m);
            };
            sstv_->onSync = [this]() { std::vector<uint8_t> m{ 0x04 }; sendSstv(m); };
            sstv_->onComplete = [this]() { std::vector<uint8_t> m; m.push_back(0x05); put32(m, 0); sendSstv(m); };
            sstv_->onRedrawStart = [this]() { std::vector<uint8_t> m{ 0x08 }; sendSstv(m); };
            log("decoder attached: sstv");
        } else if (ext == "time" || ext == "msf" || ext == "dcf77" || ext == "rwm" || ext == "wwv" || ext == "wwvb") {
            std::string which = ext == "time" ? str(msg, "station") : ext;
            if (which.empty()) which = "msf";
            buildTimeLocked_(which);
        } else if (ext == "cw" || ext == "cwskim") {
            buildCwLocked_(ext == "cwskim", msg);
            return;
        } else if (ext == "psk" || ext == "pskskim") {
            buildPskLocked_(ext == "pskskim", msg);
            return;
        } else if (ext == "aprs") {
            /* ★★ APRS (V12): 1200 Bd Bell 202 on an NFM channel (VHF), or "baud_rate":300 — HF packet on USB/LSB. Its frames
             *  are JSON (aprs_packet / aprs_list), sent from inside the run on the decode thread; each new station line goes
             *  to the gallery's tap (DecoderTap::station). */
            double b = 1200;
            num(msg, "baud_rate", b);
            vibeaprs::Run::Opts o;
            o.json = [this](const std::string& j) { broadcastText(j); };
            o.heard = [this](const vibestation::Heard& h) { if (DecoderTapPtr t = tap()) t->station(h); };
            o.rfHz = env_.dialHz;
            aprs_.reset(new vibeaprs::Run(b < 600 ? 300 : 1200, std::move(o)));
            active_.store(true, std::memory_order_relaxed);
            log(std::string("decoder attached: aprs ") + (b < 600 ? "300" : "1200") + " Bd");
            return;
        } else if (ext == "dsc") {
            buildDscLocked_(msg);
        } else if (ext == "sitora") {
            buildSitoraLocked_(msg);
        } else if (ext == "jtty") {
            buildJttyLocked_();
        } else if (ext != "navtex" && msg.find("\"auto\":true") != std::string::npos) {
            /* ★★ RTTY, ONE CLICK (Stuart, 2026-10-04: "auto config the baud and shift etc so again its a one click use"). The
             *  shift, centre, baud and polarity are found from the signal (decoders/rtty_auto.h); what it chose is printed
             *  into the text. A client that sends "auto" also sends a preset's values, which an older server simply uses. */
            rttyAuto_ = new RttyAuto(48000);
            fskState_.store(0, std::memory_order_relaxed);
            rttyAuto_->onChar = [this](char32_t ch) {
                std::lock_guard<std::mutex> bl(textMtx_);
                if (ch < 0x80) textBuf_.push_back((char)ch);
                else if (ch < 0x800) { textBuf_.push_back((char)(0xC0 | (ch >> 6))); textBuf_.push_back((char)(0x80 | (ch & 0x3F))); }
            };
            rttyAuto_->onState = [this](int st) { fskState_.store(st, std::memory_order_relaxed); uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
            // ★ 0x06 | i16 BE: the tuning guide, in AUDIO Hz (+ = the tones should go up in pitch); 0 = clear.
            rttyAuto_->onTuneHint = [this](int hz) {
                const int16_t v = (int16_t)std::max(-32000, std::min(32000, hz));
                uint8_t m[3] = { 0x06, (uint8_t)((uint16_t)v >> 8), (uint8_t)((uint16_t)v & 0xff) };
                broadcast(m, 3);
            };
            log("decoder attached: fsk auto");
        } else {
            const bool navtex = ext == "navtex";
            double cf, sh, baud;
            const bool inv = msg.find("\"inverted\":true") != std::string::npos;
            if (!num(msg, "center_frequency", cf)) cf = navtex ? 500.0 : 1000.0;
            if (!num(msg, "shift", sh)) sh = 170.0;
            if (!num(msg, "baud_rate", baud)) baud = navtex ? 100.0 : 45.45;
            std::string enc = str(msg, "encoding"); if (enc.empty()) enc = navtex ? "CCIR476" : "ITA2";
            std::string framing = str(msg, "framing"); if (framing.empty()) framing = navtex ? "4/7" : "5N1.5";
            fsk_ = new FskDecoder(48000, cf, sh, baud, framing, enc, inv);
            fskState_.store(0, std::memory_order_relaxed);
            if (msg.find("\"usos\":true") != std::string::npos) fsk_->setUsos(true);   // ★ unshift on space (manual RTTY)
            fsk_->onChar = [this](char32_t ch) {
                std::lock_guard<std::mutex> bl(textMtx_);
                if (ch < 0x80) textBuf_.push_back((char)ch);
                else if (ch < 0x800) { textBuf_.push_back((char)(0xC0 | (ch >> 6))); textBuf_.push_back((char)(0x80 | (ch & 0x3F))); }
            };
            fsk_->onState = [this](int st) { fskState_.store(st, std::memory_order_relaxed); uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
            /* ★★ NAVTEX: THE FEC'S OWN COUNT OF EACH MESSAGE (2026-10-05) — clean / repaired / lost, sent as op 0x07
             *  BEHIND the text that carries its NNNN (queued here, sent after the text frame in decode_), so the box
             *  attaches it to the message it just closed. Clients older than this ignore an op they do not know
             *  (DecoderClient / decoders.ts only act on 0x01/0x03/0x06) and keep deriving "% lost" from the '_'s —
             *  which cannot see a REPAIRED character at all: it prints like a clean one. */
            if (NavtexRx* nr = fsk_->navtex()) {
                nr->onMessageEnd = [this](const NavtexRx::Counts& c) {
                    std::lock_guard<std::mutex> bl(textMtx_);
                    if (nvCounts_.size() < 8) nvCounts_.push_back(c);
                };
            }
            char b[160];
            std::snprintf(b, sizeof b, "decoder attached: fsk cf=%.0f shift=%.0f baud=%.2f enc=%s", cf, sh, baud, enc.c_str());
            log(b);
        }
        active_.store(true, std::memory_order_relaxed);
    }
    /** ★★ CW (V12, 2026-10-25): the listener's decoder ("cw", on their audio) or a SKIMMER ("cwskim", on IQ the caller
     *  cuts — "width" Hz of it, 24–96 kHz). One engine, decoders/cw_skimmer.h; vibe_cw_run.h shapes what is sent. */
    void buildCwLocked_(bool skimmer, const std::string& msg) {
        double w = 48000;
        if (skimmer) { double v; if (num(msg, "width", v)) w = v; w = std::max(12000.0, std::min(192000.0, w)); }
        CwRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        o.spot = [this](const cw::Spot& sp, double rfHz) {
            DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; }
            if (t) t->cwSpot(sp.call, rfHz, sp.wpm, sp.snrDb, sp.how);
        };
        std::function<double(double)> rf;
        if (!skimmer) {
            auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
            rf = [rfOf, dial](double a) { return rfOf ? rfOf(a) : (dial ? dial() : 0.0) + a; };
        }
        cw_.reset(new CwRun(skimmer, w, std::move(rf), std::move(o)));
        cwWidth_.store(w, std::memory_order_relaxed);
        if (env_.cwSpotTap) { DecoderTapPtr t = env_.cwSpotTap(skimmer); std::lock_guard<std::mutex> tl(tapMtx_); cwTap_ = std::move(t); }
        if (skimmer) { if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1); }
        else active_.store(true, std::memory_order_relaxed);
        log(skimmer ? "decoder attached: CW skimmer, " + std::to_string((int)std::lround(w / 1000)) + " kHz" : std::string("decoder attached: CW"));
    }
    /** ★★ PSK31/63 (V12, 2026-10-26): the listener's decoder ("psk", on their audio — the signal nearest "centre", the
     *  middle of their passband, is ONE SIGNAL's) or a SKIMMER ("pskskim", on IQ the caller cuts — "width" Hz, 4–48 kHz).
     *  It shares the CW skimmer's IQ path (wantsIq / iqWidthHz / skimmersRunning): one host runs one decoder at a time.
     *  One engine, decoders/psk_skimmer.h; vibe_psk_run.h shapes what is sent. */
    void buildPskLocked_(bool skimmer, const std::string& msg) {
        double w = 12000, centre = 1500;
        if (skimmer) { double v; if (num(msg, "width", v)) w = v; w = std::max(4000.0, std::min(48000.0, w)); }
        { double v; if (num(msg, "centre", v) && v > 100 && v < 4000) centre = v; }
        PskRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        o.spot = [this](const psk::Spot& sp, double rfHz) {
            DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; }
            if (t) t->cwSpot(sp.call, rfHz, 0.0f, sp.snrDb, sp.how);
        };
        std::function<double(double)> rf;
        if (!skimmer) {
            auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
            rf = [rfOf, dial](double a) { return rfOf ? rfOf(a) : (dial ? dial() : 0.0) + a; };
        }
        psk_.reset(new PskRun(skimmer, w, centre, std::move(rf), std::move(o)));
        cwWidth_.store(w, std::memory_order_relaxed);
        // ★ the gallery's summary: the same tap CW's runs use (cwTap_ — one decoder runs at a time), labelled PSK
        if (env_.pskSpotTap) { DecoderTapPtr t = env_.pskSpotTap(skimmer); std::lock_guard<std::mutex> tl(tapMtx_); cwTap_ = std::move(t); }
        if (skimmer) { if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1); }
        else active_.store(true, std::memory_order_relaxed);
        log(skimmer ? "decoder attached: PSK skimmer, " + std::to_string((int)std::lround(w / 1000)) + " kHz" : std::string("decoder attached: PSK"));
    }
    /** ★★ DSC (V12, 2026-10-26): "band":"vhf" = channel 70 on an FM demodulator's audio; anything else MF/HF, its tones
     *  centred on "center_frequency" (1700 Hz: the dial 1.7 kHz below the DSC frequency on USB, above it on LSB). */
    void buildDscLocked_(const std::string& msg) {
        const bool vhf = str(msg, "band") == "vhf";
        double cf = 1700;
        if (!vhf) { double v; if (num(msg, "center_frequency", v)) cf = std::max(400.0, std::min(3000.0, v)); }
        DscRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        o.call = [this](const dsc::Call& c, const std::string& line, const std::string& j, bool rep) {
            if (DecoderTapPtr t = tap()) t->dscCall(c, line, j, rep);
        };
        auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
        std::function<double()> rf;
        if (vhf) rf = [dial] { return dial ? dial() : 0.0; };
        else rf = [rfOf, dial, cf] { return rfOf ? rfOf(cf) : (dial ? dial() : 0.0) + cf; };
        dsc_.reset(new DscRun(vhf, cf, std::move(rf), std::move(o)));
        log(vhf ? std::string("decoder attached: DSC (VHF channel 70)") : "decoder attached: DSC (MF/HF), tones around " + std::to_string((int)cf) + " Hz");
    }
    /** ★★ SITOR-A (V12, 2026-10-29): the maritime ARQ teleprinter, both stations of a link as a third station hears
     *  them (decoders/sitor_a.h, vibe_sitora_run.h). "center_frequency" is where the two tones sit in the audio
     *  (default 1000 Hz, like RTTY); the polarity, each station's timing and frequency error are found from the signal. */
    void buildSitoraLocked_(const std::string& msg) {
        double cf = 1000; { double v; if (num(msg, "center_frequency", v)) cf = std::max(300.0, std::min(3000.0, v)); }
        SitorARun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        sitora_.reset(new SitorARun(cf, std::move(o)));
        log("decoder attached: SITOR-A, tones around " + std::to_string((int)cf) + " Hz");
    }
    /** ★★ JTTY (V12, 2026-10-26): the WSJT-X 3.2 keyboard mode — every JTTY signal in the listener's passband at once
     *  (decoders/jtty_decoder.h); vibe_jtty_run.h shapes what is sent. No parameters: there is nothing to set. */
    void buildJttyLocked_() {
        JttyRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
        std::function<double(double)> rf = [rfOf, dial](double a) { return rfOf ? rfOf(a) : (dial ? dial() : 0.0) + a; };
        jtty_.reset(new JttyRun(std::move(rf), std::move(o)));
        active_.store(true, std::memory_order_relaxed);
        log("decoder attached: JTTY");
    }
    void sendSstv(const std::vector<uint8_t>& m) {
        std::lock_guard<std::mutex> sl(sstvSendMtx_);   // ★ SSTV has a video-decode thread of its own
        broadcast(m.data(), m.size());
    }
    void buildTimeLocked_(const std::string& which) {
        const TimeDecoder::Station st =
              which == "dcf77" ? TimeDecoder::Station::DCF77
            : which == "rwm"   ? TimeDecoder::Station::RWM
            : which == "wwvb"  ? TimeDecoder::Station::WWVB
            : which == "wwv"   ? TimeDecoder::Station::WWV
                               : TimeDecoder::Station::MSF;
        time_ = new TimeDecoder(48000, st);
        std::string name = which; for (auto& c : name) c = (char)std::toupper((unsigned char)c);
        TimeDecoder* td = time_;
        // ★★ WWV's line names the transmitter actually heard — "WWV" (Fort Collins) or "WWVH"
        //    (Kauai), from the seconds tick — and says "WWV/WWVH" until one is clearly stronger
        //    (2026-10-05). Every other station's tag is its own name.
        time_->onTime = [this, td](const TimeDecoder::TimeStamp& t) {
            const std::string name = td->stationTag();
            static const char* kDay[8] = { "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
            // ★ Display-only extras on the end of the same line (2026-10-05): DUT1 where the station
            //   states it, and WWV/WWVB's "DST changes today" — their second DST bit differs from
            //   the first on exactly that day, and on no other.
            char extra[48] = "";
            if (t.dut1Known) std::snprintf(extra, sizeof(extra), "  DUT1 %+.1f s", t.dut1Tenths / 10.0);
            const char* dstChange = (t.hasDst2 && t.dst2 != t.dst)
                                  ? (t.dst2 ? " DST BEGINS TODAY" : " DST ENDS TODAY") : "";
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s  %s %04d-%02d-%02d %02d:%02d  %s%s%s%s\n",
                          name.c_str(), kDay[t.weekday >= 1 && t.weekday <= 7 ? t.weekday : 0],
                          t.year, t.month, t.day, t.hour, t.minute,
                          t.dst ? "(summer time)" : "", t.leapSecondPending ? " LEAP SECOND PENDING" : "",
                          dstChange, extra);
            std::lock_guard<std::mutex> bl(textMtx_);
            textBuf_ += buf;
        };
        // ★★★ FILL THE FIELDS AS THEY ARRIVE — a replace-in-place line (see the shim's old note).
        time_->onPartial = [this, td](const TimeDecoder::Partial& p) {
            const std::string name = td->stationTag();
            { std::lock_guard<std::mutex> bl(textMtx_);
              if (morse_.size() >= 3) { textBuf_ += "RWM ID: " + morse_ + "\n"; morse_.clear(); } }
            char buf[200], yy[8], mo[4], dd[4], hh[4], mi[4];
            // ★★★ "??" FOR A DOUBTED FIELD, NEVER ITS NUMBER (2026-10-05). A field is shown before
            //     the parity that checks it arrives, and MSF's "2014" read as a confident date for
            //     half a minute (Stuart). See TimeDecoder::Partial — Bad = impossible, or not what
            //     this frame must say after a lock. Width-matched, so the line does not jump.
            std::snprintf(yy, sizeof(yy), !p.year ? "----" : p.yearBad   ? "????" : "%04d", p.t.year);
            std::snprintf(mo, sizeof(mo), !p.month ? "--"  : p.monthBad  ? "??"   : "%02d", p.t.month);
            std::snprintf(dd, sizeof(dd), !p.day   ? "--"  : p.dayBad    ? "??"   : "%02d", p.t.day);
            std::snprintf(hh, sizeof(hh), !p.hour  ? "--"  : p.hourBad   ? "??"   : "%02d", p.t.hour);
            std::snprintf(mi, sizeof(mi), !p.minute? "--"  : p.minuteBad ? "??"   : "%02d", p.t.minute);
            std::snprintf(buf, sizeof(buf), "\r%s  %s-%s-%s %s:%s   second %02d/59", name.c_str(), yy, mo, dd, hh, mi, p.second);
            std::lock_guard<std::mutex> bl(textMtx_);
            textBuf_ += buf;
        };
        time_->onState = [this, name, td](TimeDecoder::State s) {
            const char* w = s == TimeDecoder::State::NoSignal ? "no carrier"
                          : s == TimeDecoder::State::Searching ? "searching for the minute"
                          : s == TimeDecoder::State::Reading   ? "reading the minute" : "locked";
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[%s] %s \xe2\x80\x94 carrier %+.0f dB\n", name.c_str(), w, td->snrDb());
            std::lock_guard<std::mutex> bl(textMtx_);
            textBuf_ += buf;
        };
        time_->onMorse = [this](char c) {
            std::lock_guard<std::mutex> bl(textMtx_);
            morse_ += c;
            if (morse_.size() >= 24) { textBuf_ += "RWM ID: " + morse_ + "\n"; morse_.clear(); }
        };
        // ★★ SAY UP FRONT WHEN THERE IS NOTHING TO WAIT FOR (RWM carries no time code).
        if (!time_->carriesTimeCode()) {
            std::lock_guard<std::mutex> bl(textMtx_);
            textBuf_ += "RWM sends second and minute markers and a Morse callsign \xe2\x80\x94 it carries "
                        "NO date or time code, so none can be shown. Use it for propagation and calibration.\n";
        }
        log("time decoder: " + name + " (tune it in CW)");
    }

    /** ★ One spot out: to the gallery's summary and, as {"type":"digital_spot"}, to every decoder socket. `mode` is the
     *  engine's name (V12: any registered mode, not FT8-or-FT4). */
    void emitSpot(const digi::Spot& sp, double rfHz) {
        { std::lock_guard<std::mutex> dl(diagMtx_); diagSpots_[sp.mode]++; }   // ★ Band's minute line (bandDiagLocked_)
        /* ★★ STAMPED WITH ITS SLOT'S START (2026-10-26, M7EIW via Stuart): the time WSJT-X shows, so the clients' HH:MM:SS
         *  names the transmission block it was heard in (UberSDR stamps its spots the same way). An engine that does not
         *  know its slot (NaN): the moment it was decoded, as before. */
        const uint64_t ts = std::isfinite(sp.slotUtc) && sp.slotUtc > 0
            ? (uint64_t)std::llround(sp.slotUtc * 1000.0)
            : (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        /* ★★★ THESE FIELDS COME OFF THE AIR, AND ANYONE MAY TRANSMIT ANYTHING — a whitelist, so
         *  none can carry a quote, a backslash or a byte that is not UTF-8 (the RDS fault on a
         *  different decoder; Stuart, 2026-09-27: "sandbox everything"). And snprintf returns the
         *  length it WANTED: clamped below to what was written. The mode name is ours, but cleaned the same way. */
        auto safe = [](const std::string& in, size_t cap) {
            std::string o;
            for (char ch : in) {
                if (o.size() >= cap) break;
                const unsigned char u = (unsigned char)ch;
                if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '/' || u == '-') o.push_back(ch);
            }
            return o;
        };
        const std::string c = safe(sp.call, 32), g = safe(sp.grid, 8), m = safe(sp.mode, 12);
        if (c.empty() || m.empty()) return;
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = spotTap_; } if (t) t->spot(m, c, g, sp.snr, rfHz); }
        /* ★ V12 (2026-10-27): JTTY and PSK say what the station was doing ("CQ", "QSO", "DE", "TEST"), and every spot
         *  carries the call's country (our own prefix table — vibe_callsign_country.h), printable ASCII only. */
        const std::string how = safe(sp.how, 8);
        std::string ctry;
        for (char ch : callsign::country(c)) {
            const unsigned char u = (unsigned char)ch;
            if (u >= 0x20 && u < 0x7f && u != '"' && u != '\\' && ctry.size() < 40) ctry.push_back(ch);
        }
        char buf[512];
        const int n = std::snprintf(buf, sizeof(buf),
            "{\"type\":\"digital_spot\",\"data\":{\"mode\":\"%s\",\"callsign\":\"%s\","
            "\"snr\":%d,\"frequency\":%.0f,\"band\":\"%s\",\"grid\":\"%s\",\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%llu}}",
            m.c_str(), c.c_str(), sp.snr, rfHz, bandFor(rfHz), g.c_str(), how.c_str(), ctry.c_str(), (unsigned long long)ts);
        if (n > 0) broadcastText(std::string(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1), GapSpots);
    }
    /** ★ Band's mode chips as JSON (spotsMtx_ held). */
    std::string statusJsonLocked_() {
        std::string j = "{\"type\":\"digital_spots_status\",\"data\":{\"scope\":\"band\",\"modes\":[";
        bool first = true;
        const std::vector<digi::Spotter::ModeStatus> sts = spotter_->status();
        for (const auto& m : sts) {
            char b[200];
            std::snprintf(b, sizeof b, "%s{\"mode\":\"%s\",\"state\":\"%s\",\"dials\":%d,\"idle\":%llu,\"shed\":%llu,\"live\":%llu}",
                          first ? "" : ",", m.mode.c_str(), digi::stateWord(m.state), m.dials, (unsigned long long)m.idle,
                          (unsigned long long)m.shed, (unsigned long long)m.live);
            j += b; first = false;
        }
        /* ★★ A mode this band carries that is not running at all — its slice lies beyond what this radio's capture holds
         *  (a narrow capture: an extra slice 153 kHz off on 6 m) — is a chip too, "out", never silently missing. */
        {
            const digi::Band* bd = digi::bandAt(bandWidthDial_.load(std::memory_order_relaxed));
            const digi::Span sp = bd ? digi::spanOf(*bd, digi::modes()) : digi::Span();
            if (sp.ok)
                for (const std::string& name : sp.allModes()) {
                    bool have = false;
                    for (const auto& m : sts) if (m.mode == name) have = true;
                    if (have) continue;
                    char b[160];
                    std::snprintf(b, sizeof b, "%s{\"mode\":\"%s\",\"state\":\"out\",\"dials\":0,\"idle\":0,\"shed\":0,\"live\":0}", first ? "" : ",", name.c_str());
                    j += b; first = false;
                }
        }
        const double u = spotter_->budgetUnits();
        char t[64];
        if (std::isfinite(u)) std::snprintf(t, sizeof t, "],\"budget\":%.1f}}", u); else std::snprintf(t, sizeof t, "],\"budget\":null}}");
        return j + t;
    }
    /** ★ Send the chips when a mode's state changed (at most once a second) and every 15 s regardless (a window opened
     *  late sees them). spotsMtx_ held, on the decode thread. */
    void maybeSendStatusLocked_(double utc) {
        const uint64_t seq = spotter_->statusSeq();
        if (seq == statusSent_ ? utc - statusAt_ < 15.0 : utc - statusAt_ < 1.0) return;
        statusSent_ = seq; statusAt_ = utc;
        broadcastText(statusJsonLocked_(), -1);   // ★ re-sent every 15 s — not kept for a returning listener
    }
    /** Stop the run (spotsMtx_ held). `finishTap`: a real stop, not a scope change — the gallery's summary is written. */
    void stopSpotterLocked_(bool finishTap) {
        spotsOn_ = false;
        spotsActive_.store(false, std::memory_order_relaxed);
        if (bandIq_.exchange(false)) skimmersRunning().fetch_sub(1);
        if (spotter_) { spotSkipBase_ += spotter_->slotsSkipped(); spotShedBase_ += spotter_->periodsShed(); }
        spotter_.reset();            // ★ joins the engines' workers (an FT8 decode in progress is aborted, not waited for)
        if (finishTap) { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t.swap(spotTap_); } if (t) t->finish(true); }
    }
public:
    static const char* bandFor(double hz) {
        // ★ The digital-spots band table first (2200 m … 70 cm), then the older HF names it never had.
        if (const digi::Band* b = digi::bandAt(hz)) return b->id;
        const double m = hz / 1e6;
        if (m >= 1.8  && m < 2.0)   return "160m";
        if (m >= 3.5  && m < 4.0)   return "80m";
        if (m >= 5.3  && m < 5.5)   return "60m";
        if (m >= 7.0  && m < 7.3)   return "40m";
        if (m >= 10.1 && m < 10.15) return "30m";
        if (m >= 14.0 && m < 14.35) return "20m";
        if (m >= 18.0 && m < 18.2)  return "17m";
        if (m >= 21.0 && m < 21.45) return "15m";
        if (m >= 24.8 && m < 25.0)  return "12m";
        if (m >= 28.0 && m < 29.7)  return "10m";
        if (m >= 50.0 && m < 54.0)  return "6m";
        return "";
    }
private:

    // ── the decode thread ──
    void loop_() {
        vibeDecoderThread("vibe-decode");
        std::unique_lock<std::mutex> lk(qM_);
        for (;;) {
            qCv_.wait(lk, [this] { return !q_.empty() || !qi_.empty() || qStop_; });
            if (qStop_) return;
            if (!qi_.empty()) {
                IqBlock b = std::move(qi_.front());
                qi_.pop_front();
                qiFrames_ -= b.v.size();
                if (b.slice == 0) qiMainSec_ = std::max(0.0, qiMainSec_ - (double)(b.v.size() + (size_t)b.gapN) / b.rate);
                lk.unlock();
                const uint64_t c0 = vibeThreadCpuNs();
                {
                    std::lock_guard<std::mutex> dl(decMtx_);
                    if (b.gapN == 0 && b.slice == 0 && cw_ && cw_->skimmer()) cw_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                    if (b.gapN == 0 && b.slice == 0 && psk_ && psk_->skimmer()) psk_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                }
                {
                    std::lock_guard<std::mutex> sl(spotsMtx_);
                    if (spotter_ && spotter_->scope() == SpotScope::Band) {
                        if (b.gapN > 0) { spotter_->feedGap(b.gapN, b.rate, b.centre, b.utc); }
                        else {
                        spotter_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre, b.utc,
                                         b.pw.empty() ? nullptr : b.pw.data(), (int)b.pw.size(), b.binHz);
                        maybeSendStatusLocked_(b.utc);
                        bandDiagLocked_(b);
                        }
                        // ★ The slice is centred on the dial: the next blocks are cut to the span of the band it is on now
                        //   (a band step is followed within a block or two). Cached per centre — the table walk is cheap but not free.
                        if (b.slice == 0 && std::fabs(b.centre - bandWidthDial_.load(std::memory_order_relaxed)) > 0.5) {
                            bandWidth_.store(bandWidthFor(b.centre), std::memory_order_relaxed);
                            bandWidthDial_.store(b.centre, std::memory_order_relaxed);
                            setExtras_(b.centre);   // ★ and the band's extra slices follow it
                        }
                    }
                }
                { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = spotTap_; } if (t && bandIq_.load()) t->tick(); }
                { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; } if (t) t->tick(); }
                cpuNs_.fetch_add(vibeThreadCpuNs() - c0, std::memory_order_relaxed);
                lk.lock();
                continue;
            }
            std::vector<float> v = std::move(q_.front());
            const double utc = qUtc_.front();
            q_.pop_front();
            qUtc_.pop_front();
            qFrames_ -= v.size();
            lk.unlock();
            const uint64_t c0 = vibeThreadCpuNs();
            decode_(v.data(), (int)v.size());
            spots_(v.data(), (int)v.size(), utc);
            cpuNs_.fetch_add(vibeThreadCpuNs() - c0, std::memory_order_relaxed);
            lk.lock();
        }
    }
    void stopQueue_() {
        replayStop_.store(true);
        { std::lock_guard<std::mutex> tl(replayThrMtx_);
          for (auto& j : replayThreads_) if (j->th.joinable()) j->th.join();
          replayThreads_.clear(); }
        { std::lock_guard<std::mutex> lk(qM_); qStop_ = true; q_.clear(); qUtc_.clear(); qFrames_ = 0; qi_.clear(); qiFrames_ = 0; }
        qCv_.notify_all();
        if (qThread_.joinable()) qThread_.join();
    }
    void decode_(const float* data, int count) {
        std::string text;
        std::vector<NavtexRx::Counts> counts;
        {
            std::lock_guard<std::mutex> lk(decMtx_);
            if (!running_()) return;
            if (sstv_) {
                // SSTV runs at 12 kHz — box-average 4 and feed.
                std::vector<int16_t> dec; dec.reserve((size_t)count / 4 + 1);
                for (int i = 0; i < count; i++) {
                    sstvAcc_ += data[i];
                    if (++sstvDecim_ >= 4) {
                        const int s = (int)std::lround(sstvAcc_ / 4.0f * 32767.0f);
                        dec.push_back((int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s)));
                        sstvDecim_ = 0; sstvAcc_ = 0.0f;
                    }
                }
                if (!dec.empty()) sstv_->process(dec.data(), (int)dec.size());
                return;
            }
            if (jtty_) {
                // ★ JTTY: float audio straight in; its lines, lists, spots and state are sent from inside (JttyRun).
                jtty_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (sitora_) {
                // ★ SITOR-A: float audio straight in; its text, status and state are sent from inside (SitorARun).
                sitora_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (dsc_) {
                // ★ DSC: float audio straight in; its lines, JSON and state are sent from inside (DscRun).
                dsc_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (cw_) {
                // ★ CW: the engine takes the float audio itself (no int16 round trip); its frames are sent from inside.
                cw_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (psk_) {
                psk_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (aprs_) {
                // ★ APRS likewise: float audio in, JSON out from inside the run
                aprs_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            std::vector<int16_t> mono((size_t)count);
            for (int i = 0; i < count; i++) {
                const int s = (int)std::lround(data[i] * 32767.0f);
                mono[(size_t)i] = (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
            }
            if (wefax_) { wefax_->process(mono.data(), count); return; }
            // ★★★ FALL THROUGH TO THE TEXT FLUSH — the time decoder's minutes ride the RTTY text
            //     channel, drained below (an early return once decoded perfectly and sent nothing).
            if (time_) time_->process(mono.data(), count);
            else if (fsk_) fsk_->process(mono.data(), count);
            else if (rttyAuto_) rttyAuto_->process(mono.data(), count);
            { std::lock_guard<std::mutex> bl(textMtx_); text.swap(textBuf_); counts.swap(nvCounts_); }
        }
        if (!text.empty()) textFrame(text);
        // ★ 0x07 | u32 BE clean | u32 BE repaired | u32 BE lost — after the text, never before it (see fsk attach).
        for (const auto& c : counts) {
            uint8_t m[13]; m[0] = 0x07;
            const unsigned long v[3] = { c.clean, c.repaired, c.failed };
            for (int k = 0; k < 3; k++) {
                const uint32_t x = (uint32_t)std::min<unsigned long>(v[k], 0xffffffffUL);
                m[1 + 4 * k] = (uint8_t)(x >> 24); m[2 + 4 * k] = (uint8_t)(x >> 16);
                m[3 + 4 * k] = (uint8_t)(x >> 8);  m[4 + 4 * k] = (uint8_t)x;
            }
            broadcast(m, sizeof m);
        }
        if (DecoderTapPtr t = tap()) t->tick();
    }
    void spots_(const float* data, int count, double captureUtc) {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotsOn_ || !spotter_ || spotter_->scope() != SpotScope::Single) return;
        spotter_->feedAudio48k(data, count, captureUtc);
        // ★ The spots tap's clock too — the FT8 summary's last-10-minutes card rolls and its hourly card closes on it.
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = spotTap_; } if (t) t->tick(); }
    }
    double queuedIqSec() {
        std::lock_guard<std::mutex> lk(qM_);
        double s = 0;
        for (auto& b : qi_) if (b.rate > 0) s += (double)(b.v.size() + (size_t)b.gapN) / b.rate;
        return s;
    }

    Env env_;
    std::string session_;

    std::mutex peerMtx_;
    std::vector<DecoderPeerPtr> peers_;

    std::mutex decMtx_;
    std::string name_, startKey_;
    FskDecoder*   fsk_   = nullptr;
    RttyAuto*     rttyAuto_ = nullptr;   // ★ RTTY with "auto":true — finds shift, baud and polarity itself
    WefaxDecoder* wefax_ = nullptr;
    SstvDecoder*  sstv_  = nullptr;
    TimeDecoder*  time_  = nullptr;
    std::unique_ptr<CwRun> cw_;               // ★ CW: a listener's decoder or a skimmer (buildCwLocked_)
    std::unique_ptr<vibeaprs::Run> aprs_;     // ★ APRS (V12): the modem + the station list (vibe_aprs_run.h)
    std::unique_ptr<SitorARun> sitora_;       // ★ SITOR-A: the ARQ listener (buildSitoraLocked_)
    std::unique_ptr<DscRun> dsc_;             // ★ DSC: MF/HF or VHF channel 70 (buildDscLocked_)
    std::unique_ptr<JttyRun> jtty_;           // ★ JTTY: every JTTY signal in the passband (buildJttyLocked_)
    std::atomic<bool>   cwIq_{false};         // ★ the skimmer wants IQ (wantsIq)
    std::atomic<double> cwWidth_{0.0};
    DecoderTapPtr cwTap_;                     // ★ the gallery's CW (or PSK) summary (Env::cwSpotTap / pskSpotTap), under tapMtx_
    std::unique_ptr<PskRun> psk_;             // ★ PSK31/63: a listener's decoder or a skimmer (buildPskLocked_) — shares cwIq_/cwWidth_
    int   sstvDecim_ = 0; float sstvAcc_ = 0.0f;
    int   textSlot_ = -1;
    std::atomic<bool> active_{false};
    std::atomic<int>  imageKind_{0};          // 0 none · 1 WEFAX · 2 SSTV — what record() keeps
    std::mutex textMtx_;
    std::string textBuf_, morse_;
    std::vector<NavtexRx::Counts> nvCounts_;   // ★ NAVTEX per-message FEC counts, sent after the text (0x07)
    std::mutex sstvSendMtx_;
    std::mutex tapMtx_;
    DecoderTapPtr tap_;                       // ★ the gallery's copy of the running decoder — see DecoderTap
    DecoderTapPtr spotTap_;                   // ★ …and of the FT8/FT4 spotter (a summary — see Env::spotTap)
    DecoderTapPtr tap() { std::lock_guard<std::mutex> tl(tapMtx_); return tap_; }

    std::mutex spotsMtx_;
    std::unique_ptr<digi::Spotter> spotter_;   // ★ the digital-spots run (Single or Band) — see startSpots
    bool  spotsOn_ = false;
    std::atomic<bool> spotsActive_{false};      // ★ Single: wants the listener's audio
    std::atomic<bool> bandIq_{false};           // ★ Band: wants the band's IQ slice (wantsIq)
    std::atomic<double> bandWidth_{24000.0}, bandWidthDial_{-1.0};   // ★ the slice width (set at start and by the decode thread)
    std::atomic<int> nExtras_{0};                // ★ Band's extra slices (iqExtraSlices) — set with bandWidth_
    std::atomic<double> exC_[kMaxExtraSlices], exW_[kMaxExtraSlices];
    void setExtras_(double dialHz) {
        const std::vector<ExtraSlice> e = bandExtraSlices(dialHz);
        nExtras_.store(0, std::memory_order_release);
        for (size_t k = 0; k < e.size() && (int)k < kMaxExtraSlices; ++k) { exC_[k].store(e[k].centreHz); exW_[k].store(e[k].widthHz); }
        nExtras_.store((int)std::min<size_t>(e.size(), kMaxExtraSlices), std::memory_order_release);
    }
    uint64_t spotSkipBase_ = 0;
    uint64_t spotShedBase_ = 0;                  // ★ Band periods shed by runs already stopped (spotPeriodsShed is monotonic)
    uint64_t statusSent_ = ~0ull;                // ★ the spotter's statusSeq last sent (spotsMtx_)
    double   statusAt_ = 0;                      //   and when (capture UTC)                 // ★ skipped slots of runs already stopped (spotSlotsSkipped is monotonic)
    int   spotsSlot_ = -1;

    std::mutex replayMtx_;
    std::vector<std::vector<uint8_t>> replay_;
    size_t replayBytes_ = 0;
    uint64_t replayDoneSeq_ = 0;                 // ★ the gap sequence at which replay_'s picture finished (0 = not yet)
    std::deque<DonePic> done_;                   // ★ pictures finished while somebody was away (replayMtx_)
    size_t doneBytes_ = 0;
    double awayUntil_ = 0;                       // ★ steadySecs() until which a finished picture is kept (replayMtx_)
    std::mutex replayThrMtx_;
    std::vector<std::unique_ptr<ReplayJob>> replayThreads_;
    std::atomic<bool> replayStop_{false};

    std::mutex qM_;
    std::condition_variable qCv_;
    std::deque<std::vector<float>> q_;
    std::deque<double> qUtc_;            // ★ each block's capture time (feed), in step with q_
    struct IqBlock { std::vector<std::complex<float>> v; double rate = 0, centre = 0, utc = 0; std::vector<float> pw; double binHz = 0; int slice = 0;
                     int gapN = 0; };   // ★ gapN > 0: no samples, a gap of that many to bridge
    /* ★★ BAND'S MINUTE LINE (V12 2026-10-27, for the Pi 2's "every chip light, nothing decoded"): what the decode thread was
     *  fed (seconds of each slice per minute of capture — 60 means it kept up), how far behind the capture it ran (now −
     *  the block's capture UTC: FT8 cuts its slots by that stamp), what was dropped, slots decoded / skipped, spots per mode
     *  and the chips. One line a minute, decode thread, spotsMtx_ held. */
    std::mutex diagMtx_;
    std::map<std::string, int> diagSpots_;
    double diagAt_ = 0, diagMain_ = 0, diagExtra_ = 0, diagLagMin_ = 1e9, diagLagMax_ = -1e9;
    double diagCentre_ = 0, diagRate_ = 0;
    uint64_t diagDrop0_ = 0, diagSlots0_ = 0, diagSkip0_ = 0, diagBridge0_ = 0;
    void bandDiagLocked_(const IqBlock& b) {
        const double lag = vibeUtcNow() - b.utc;
        if (b.slice == 0) { diagMain_ += (double)b.v.size() / b.rate; diagCentre_ = b.centre; diagRate_ = b.rate; }
        else diagExtra_ += (double)b.v.size() / b.rate;
        diagLagMin_ = std::min(diagLagMin_, lag); diagLagMax_ = std::max(diagLagMax_, lag);
        if (diagAt_ <= 0) { diagAt_ = b.utc; diagDrop0_ = iqBlocksDropped(); diagSlots0_ = spotter_->slotsDecoded(); diagSkip0_ = spotter_->slotsSkipped(); diagBridge0_ = spotter_->usBridged(); return; }
        if (b.utc - diagAt_ < 60.0) return;
        std::string spots, chips;
        { std::lock_guard<std::mutex> dl(diagMtx_); for (auto& kv : diagSpots_) spots += " " + kv.first + " " + std::to_string(kv.second); diagSpots_.clear(); }
        for (const auto& m : spotter_->status()) chips += " " + m.mode + ":" + digi::stateWord(m.state);
        char line[640];
        std::snprintf(line, sizeof line,
                      "Band decode, last %.0f s of capture: main slice %.1f s fed (%.0f S/s at %.4f MHz), extra slices %.1f s; "
                      "decode %.2f-%.2f s behind the capture; IQ blocks dropped %llu; gaps bridged %.2f s (all slices); slots decoded %llu, skipped %llu; spots:%s; chips:%s",
                      b.utc - diagAt_, diagMain_, diagRate_, diagCentre_ / 1e6, diagExtra_, diagLagMin_, diagLagMax_,
                      (unsigned long long)(iqBlocksDropped() - diagDrop0_), (double)(spotter_->usBridged() - diagBridge0_) / 1e6,
                      (unsigned long long)(spotter_->slotsDecoded() - diagSlots0_),
                      (unsigned long long)(spotter_->slotsSkipped() - diagSkip0_), spots.empty() ? " none" : spots.c_str(), chips.c_str());
        log(line);
        diagAt_ = b.utc; diagMain_ = diagExtra_ = 0; diagLagMin_ = 1e9; diagLagMax_ = -1e9;
        diagDrop0_ = iqBlocksDropped(); diagSlots0_ = spotter_->slotsDecoded(); diagSkip0_ = spotter_->slotsSkipped(); diagBridge0_ = spotter_->usBridged();
    }

    double qiMainSec_ = 0;               // ★ seconds of the MAIN slice queued (qM_)
    std::atomic<uint64_t> iqDropped_{0};
    std::deque<IqBlock> qi_;             // ★ a CW skimmer's IQ (feedIq), served before audio by the same thread
    size_t qiFrames_ = 0;
    size_t qFrames_ = 0;
    bool   qStop_ = false;
    std::thread qThread_;
    std::atomic<uint64_t> fed_{0}, dropped_{0};
    std::atomic<uint64_t> cpuNs_{0};
    std::atomic<int>      fskState_{-1};
};

// ── The router: which host a socket, and a block of audio, belongs to ───────────────────────────

/** ★★★ ONE HOST PER LISTENER ON A PER-VFO RADIO, ONE FOR THE RADIO OTHERWISE.
 *  `perVfo` is the shim's perClientDsp(): true when every listener has their own channel and so
 *  their own audio. Then a decoder socket is routed by its SESSION to that listener's host, and only
 *  that listener's audio is fed to it. Otherwise there is ONE pipeline — a shared dial, or a radio
 *  with one listener — and one host, fed from it and mirrored to every decoder socket, exactly as
 *  before (the note on dspFor: null is NORMAL, it means one pipeline).
 *  ★ Guarded by its own leaf mutex; it never calls out while holding it. */
class DecoderRouter {
public:
    using EnvFor = std::function<DecoderHost::Env(const std::string& session)>;
    explicit DecoderRouter(EnvFor envFor) : envFor_(std::move(envFor)) {}
    ~DecoderRouter() { shutdownAll(); }

    /** The host a decoder socket of `session` belongs to — created on first use. */
    std::shared_ptr<DecoderHost> hostFor(const std::string& session, bool perVfo) {
        std::lock_guard<std::mutex> lk(m_);
        if (!perVfo) {
            if (!shared_) shared_ = std::make_shared<DecoderHost>(envFor_(std::string()), std::string());
            return shared_;
        }
        auto& h = bySession_[session];
        if (!h) h = std::make_shared<DecoderHost>(envFor_(session), session);
        anyPer_.store(true, std::memory_order_relaxed);
        return h;
    }
    /** The one-pipeline host, or null if none was ever made. */
    std::shared_ptr<DecoderHost> shared() { std::lock_guard<std::mutex> lk(m_); return shared_; }
    /** This listener's own host, or null. */
    std::shared_ptr<DecoderHost> find(const std::string& session) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = bySession_.find(session);
        return it == bySession_.end() ? nullptr : it->second;
    }
    /** A listener's audio (per-VFO). Costs an atomic read when no listener has a host. */
    void feedSession(const std::string& session, const float* pcm, int count, int stride, double captureUtc = NAN) {
        if (!anyPer_.load(std::memory_order_relaxed)) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it != bySession_.end()) h = it->second; }
        if (h) h->feed(pcm, count, stride, captureUtc);
    }
    /** The one pipeline's audio. */
    void feedShared(const float* pcm, int count, int stride) {
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_); h = shared_; }
        if (h) h->feed(pcm, count, stride);
    }
    /** A listener's host is finished with — every decoder stopped, every slot released, NOW. */
    void drop(const std::string& session) {
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it == bySession_.end()) return;
          h = it->second; bySession_.erase(it);
          anyPer_.store(!bySession_.empty(), std::memory_order_relaxed); }
        h->shutdown();            // ★ outside the lock: it joins the decode thread
    }
    /** Every per-listener host whose session `alive` no longer recognises, stopped after `graceSec`
     *  of absence — a listener kicked or timed out keeps no decoder slot, while a blip of a few
     *  seconds (a reconnecting spectrum socket) does not throw away a ten-minute chart. */
    void reapOrphans(const std::function<bool(const std::string&)>& alive, double now, double graceSec) {
        // ★ `alive` is the shim asking its own tables (it takes clientMtx), so it is called with NO
        //   lock of ours held: the shim also reaches this router while holding clientMtx, and two
        //   locks taken in both orders is the deadlock this server has already had twice.
        std::vector<std::string> sessions;
        { std::lock_guard<std::mutex> lk(m_); for (auto& kv : bySession_) sessions.push_back(kv.first); }
        std::vector<std::pair<std::string, bool>> verdict;
        for (auto& s : sessions) verdict.push_back({ s, alive(s) });
        std::vector<std::string> gone;
        { std::lock_guard<std::mutex> lk(m_);
          for (auto& v : verdict) {
              if (v.second) { orphanSince_.erase(v.first); continue; }
              auto it = orphanSince_.find(v.first);
              if (it == orphanSince_.end()) { orphanSince_[v.first] = now; continue; }
              if (now - it->second >= graceSec) gone.push_back(v.first);
          } }
        for (auto& s : gone) {
            { std::lock_guard<std::mutex> lk(m_); orphanSince_.erase(s); }
            std::shared_ptr<DecoderHost> h = find(s);
            if (h) { h->stop(); h->stopSpots(); }
        }
    }
    /** Every host, for the admin table: (session, host). The shared one has an empty session. */
    std::vector<std::pair<std::string, std::shared_ptr<DecoderHost>>> all() {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::pair<std::string, std::shared_ptr<DecoderHost>>> v;
        if (shared_) v.push_back({ std::string(), shared_ });
        for (auto& kv : bySession_) v.push_back(kv);
        return v;
    }
    /** ★★★ Is the ONE-PIPELINE host decoding — i.e. does a decoder need the shared audio chain to
     *  keep running? (2026-10-07) The shim's idle gate and its park decision count this as a
     *  consumer, so a decoder whose listener's audio and waterfall sockets have gone is not starved.
     *  Leaf lock, atomic reads: safe under the shim's clientMtx and on its DSP thread. */
    bool sharedWantsAudio() const {
        std::lock_guard<std::mutex> lk(m_);
        return shared_ && shared_->wantsAudio();
    }
    /** ★ …and is it a CW skimmer, wanting IQ (V12)? The same leaf lock. */
    bool sharedWantsIq() const {
        std::lock_guard<std::mutex> lk(m_);
        return shared_ && shared_->wantsIq();
    }
    /** Is anything decoding anywhere on this radio? */
    bool anyRunning() {
        for (auto& kv : all()) if (kv.second->wantsAudio() || kv.second->wantsIq()) return true;
        return false;
    }
    void shutdownAll() {
        std::vector<std::shared_ptr<DecoderHost>> hs;
        { std::lock_guard<std::mutex> lk(m_);
          if (shared_) hs.push_back(shared_);
          for (auto& kv : bySession_) hs.push_back(kv.second);
          shared_.reset(); bySession_.clear(); orphanSince_.clear();
          anyPer_.store(false, std::memory_order_relaxed); }
        for (auto& h : hs) h->shutdown();
    }
private:
    EnvFor envFor_;
    mutable std::mutex m_;
    std::shared_ptr<DecoderHost> shared_;
    std::map<std::string, std::shared_ptr<DecoderHost>> bySession_;
    std::map<std::string, double> orphanSince_;
    std::atomic<bool> anyPer_{false};
};

} // namespace vibe
