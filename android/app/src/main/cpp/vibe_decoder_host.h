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
#include "vibe_station_store.h"
#include "decoders/rtty_auto.h"
#include "decoders/wefax_decoder.h"
#include "decoders/sstv_decoder.h"
#include "decoders/time_decoder.h"
#include "decoders/ft8_decoder.h"
#include "decoders/digi_spotter.h"   // ★ V12: digital spots — Single (the listener's audio) or Band (a band's IQ slice)
#include "decoders/digi_bands.h"
#include "vibe_band_slice.h"   // ★ V12: Band's slice sizing and its extra slices
#include "vibe_skim_bands.h"   // ★ V12 beta91: the band decode's CW / RTTY windows follow the server's IARU region
#include "vibe_cw_run.h"
#include "vibe_psk_run.h"
#include "vibe_rtty_skim_run.h"   // ★ V12 the RTTY skimmer — docs/v12/RTTY-SKIMMER.md
#include "vibe_aprs_run.h"
#include "vibe_dec_dump.h"   // ★ the developer capture of what a decoder is fed (VIBE_DEC_AUDIO_DUMP; off when unset)
#include "vibe_fctlm.h"        // ★ V12 beta90 FUNcube BPSK 1200 telemetry (AO-73) — docs/v12/SATELLITE-BEST-SERVER.md
#include "vibe_sat_engine.h"   // ★ V12 the satellite engine's decoder hook (the packet decoder's satellite name)    // ★ V12 APRS (1200 Bd VHF, 300 Bd HF) on the listener's audio — docs/v12/AIS-APRS.md
#include "vibe_dsc_run.h"
#include "vibe_sitora_run.h"
#include "vibe_hfdl_run.h"   // ★ V12 HFDL on the USB dial — docs/v12/AVIATION.md (the aircraft go to the radio's table)
#include "decoders/same_decoder.h"   // ★ V12 EAS / SAME (47 CFR § 11.31)
#include "decoders/selcal_decoder.h" // ★ V12 SELCAL (ICAO Annex 10 Vol III, Amendment 91)
#include "decoders/textmodes.h"      // ★ V12 MFSK16/32, OLIVIA/CONTESTIA, THOR, DOMINOEX, MT63 (one object per family, AUTO)
#include "decoders/ale_decoder.h"    // ★ V12 2G HF ALE (MIL-STD-188-141 Appendix A) — docs/v12/HF-DATA.md
#include "decoders/feld_hell.h"      // ★ V12 FELD HELL, drawn: strips down the image path (0x01 lines, as WEFAX)
#if defined(VIBE_PAGING)
#include "vibe_paging_run.h"   // ★ V12 PAGING — Android local hardware only (the define is the Android library's)
#endif
#include "vibe_jtty_run.h"
#include "vibe_ncdxf_run.h"   // ★ V12 2026-11-16 the NCDXF / IARU beacon tracker
#include "vibe_balloon_run.h"  // ★ V12 balloons: UKHAS RTTY, Horus Binary, SSDV pictures — docs/v12/BALLOONS.md
#include "vibe_tones_run.h"    // ★ V12 tone decoders: DTMF and 5-tone selective calling — docs/v12/TONES.md
#include "vibe_lrpt_run.h"     // ★ V12 METEOR-M LRPT pictures on an IQ slice of the dial — docs/v12/LRPT.md
#include "vibe_orbcomm_run.h"  // ★ V12 ORBCOMM housekeeping + raw subscriber frames on an IQ slice — docs/v12/ORBCOMM.md
#include "vibe_xpdr_run.h"     // ★★★ beta94 the transponder signal finder (CW + voice over a whole linear transponder) — docs/v12/TRANSPONDER-FINDER.md
#include "vibe_sat_engine.h"    // ★ V12 the satellite engine's decoder hook (LRPT's Doppler feed-forward)
#include "vibe_nav_run.h"      // ★ V12 VOR bearing + ILS deviation on the channel IQ — docs/v12/VOR-ILS.md
#include "vibe_sarsat_run.h"   // ★ V12 COSPAS-SARSAT 406 MHz beacons on the channel IQ — docs/v12/SARSAT.md
#include "vibe_clock.h"
#include "vibe_bg_fullcpu.h"   // ★★★ beta94 nobody listening: background decoding may use the whole machine
#include "vibe_spot_report.h"   // ★ V12 beta90: the spots to PSK Reporter / WSPRnet (owner switch, off by default)
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
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
    /** ★★★ beta94 NOBODY LISTENING (vibe_bg_fullcpu.h): claim under `limit` instead of max() — BACKGROUND decoding's extra
     *  slots over the box's limit while its radio is idle. Listeners never call it; one arriving to a full box makes room
     *  (BgRegistry::makeRoomForListener). Default: no such thing, an ordinary claim. */
    virtual int  claimWithin(int limit) { (void)limit; return claim(); }
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
    int claim() override { return claimWithin(max()); }
    int claimWithin(int limit) override {
        const int m = std::max(1, std::min(limit, 64));   // ★ 64: inUseLocked's probe range
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
    /** ★ V12 (docs/v12/TONES.md): is this socket the server OWNER's (signed in to the admin page)? A line only the owner may
     *  see (textFramePriv) goes to these sockets alone. Asked per line, so signing in or out takes effect at once. */
    virtual bool privileged() const { return false; }
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
    /** ★ V12 LRPT: one finished picture of a pass (a standard PNG, full size) — `name` says which ("channel 2 near-IR",
     *  "RGB 221 false colour"); `partial` = the decoder stopped before the pass ended. */
    virtual void picture(const std::string& /*name*/, const std::string& /*png*/, int /*w*/, int /*h*/, bool /*partial*/) {}
    /** ★ V12 METEOR LRPT: the pass's map outline JSON (empty + `why` = none), handed over just before its pictures. */
    virtual void outline(const std::string& /*json*/, const std::string& /*why*/) {}
    /** ★★ V12 beta91 TELEMETRY CHARTS: one good FUNcube frame, parsed, with its time and pass (vibe_fctlm_gallery.h —
     *  the per-pass CSV and the day's log). */
    virtual void satTlm(const vibefc::Decoded& /*frame*/, const vibefc::TlmCtx& /*ctx*/) {}
    /** ★ Items this tap has written to the gallery so far (a picture tap counts its pictures; 0 = it does not count) —
     *  the background WEFAX VFO's hourly log line reads it ("charts saved"). */
    virtual size_t savedCount() const { return 0; }
};
using DecoderTapPtr = std::shared_ptr<DecoderTap>;

class DecoderHost {
public:
    struct Env {
        /** The dial this host's audio is tuned to (Hz) — a spot's RF frequency is dial + audio offset. */
        std::function<double()> dialHz;
        /** ★ V12 HFDL: the aircraft table its messages go to; null = the radio's own (vibeavi::sharedTracker). The
         *  benchmark gives its own, so a timing run never puts test aircraft on a server's map. */
        vibeavi::Tracker* aircraft = nullptr;
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
        /** ★★ …and the RTTY skimmer's (V12, 2026-11-14): the same summary of its spots. */
        std::function<DecoderTapPtr()> rttySkimSpotTap;
        /** ★ The RTTY skimmer's search workers (the owner's vibeserver.json "rttySkimWorkers", else the benchmark's / the
         *  hardware's default — rttySkimDefaultWorkers). Empty = the hardware default. */
        std::function<int()> rttySkimWorkers;
        /** ★★★ beta94 NOBODY LISTENING (vibe_bg_fullcpu.h): background decoding's budget now — the RTTY skimmer's workers,
         *  the Band governor's cap and pause rule follow it live. Null = a listener's host (today's budget, unchanged). */
        std::shared_ptr<const bg::fullcpu::Live> bgCpu;
        /** ★ A listener's AUDIO frequency → RF (their dial and mode: USB dial + a, LSB dial − a, this server's CW
         *  dial + a − 600). Empty = dial + audio. A skimmer's IQ carries its own centre. */
        std::function<double(double)> rfOfAudio;
        /** ★★ V12 BAND GOVERNOR (decoders/digi_governor.h, 2026-10-27): the OWNER's word per mode in Band — +1 Always (no
         *  activity gate, never shed), −1 Off, 0 Auto (blocked-modes tokens "spotsband+<mode>" / "spotsband-<mode>").
         *  Empty = Auto for every mode. Asked at each gate period's start (cheap: a CSV lookup). */
        std::function<int(const std::string& mode)> bandOverride;
        /** ★★ V12: a digital-spots mode the owner switched off (true = never run it, in Single or Band). Empty = none. */
        std::function<bool(const std::string& mode)> spotModeOff;
        /** ★ What the benchmark says this machine affords for Band, in FT8 units (∞ / NaN = no benchmark). Asked once per
         *  Band start. */
        std::function<double()> bandBudgetUnits;
        /** ★★ V12 tone decoders: the owner's opt-in to 5-tone digits off the amateur bands ("tones+digits"). Read when the
         *  decoder starts; empty = off. Never anything a client sends. */
        std::function<bool()> tonesDigits;
        /** ★★ V12 ALE: the owner's opt-in to AMD message TEXT off the amateur bands ("ale+amd"). Read when a line is sent;
         *  empty = off. Never anything a client sends. Addresses are always shown (not message content). */
        std::function<bool()> aleAmd;
        /** ★ The ITU region of this server's band plan (vibe_bands.h) — which bands are amateur. Empty = Region 1. */
        std::function<int()> region;
        /** ★★ V12 LRPT: the satellite engine's Doppler for this listener's pass now (Hz to remove, + = received above the
         *  published frequency) — the feed-forward for vibelrpt::Run (docs/v12/LRPT.md §Interface). Empty = none: the
         *  receiver acquires and tracks the Doppler itself, or the engine moves the dial. */
        std::function<double()> satDopplerHz;
        /** ★★ beta91 LRPT: is this listener's satellite above the horizon (1), not (0), or is there no pick (−1)? Tests;
         *  empty = the satellite engine's decoderView. With an override the rate comes from the attach only. */
        std::function<int()> satInRange;
        /** ★★ beta92 the satellite gain hold's note ("Airspy linearity preset 12 (held, 0 clip cuts)") for LRPT's per-pass
         *  summary line. Empty = the summary says the gain is not known. */
        std::function<std::string()> satGainNote;
        /** ★★ V12 LRPT's MAP OUTLINE (vibe_lrpt_overlay.h) — tests: the METEOR element sets to try, this server's place,
         *  and the clock. Empty = the satellite engine's (lrptCandidates / observer) and vibeUtcNow(). */
        std::function<std::vector<vibelrpt::geo::Candidate>()> lrptGeoCandidates;
        std::function<bool(vibe::sat::Observer&)> lrptGeoObserver;
        std::function<double()> nowUnix;
        /** ★ V12 VOR / ILS: the server's place (lat, lon; false = not given) — the nearest published station on a channel,
         *  and the owner's check of how far a radial passes from here. Never sent to a listener as such. */
        std::function<bool(double&, double&)> position;
        /** ★★ V12 beta90 THE STATION STORE (vibe_station_store.h — the DIGI MODES MAP and PSK Reporter): every spot this
         *  host makes (the digital-spots engines, CW / PSK / RTTY skimmers and decoders) goes in, tagged `stationOrigin`
         *  (vibestations::kLive / kBackground). Null = kept nowhere: the benchmark and the decoder-only sidecar, so a timing
         *  run's or a test's spots never reach the map or PSK Reporter. */
        vibestations::Store* stations = nullptr;
        uint8_t stationOrigin = vibestations::kLive;
        /** ★★ A BACKGROUND WEFAX VFO's name for its HOURLY LOG LINE ("background VFO 8 (4608.1 kHz USB)") — what the decoder
         *  heard in the hour: the audio level, lines read, the best start-tone / phasing evidence, charts begun and saved
         *  (2026-11-22: 8 h on the Pi 500 with no `wefax:` line, and "no signal" could not be told from "missed the start").
         *  Empty = no hourly line (a listener's WEFAX, the benchmark, the tests that do not ask). */
        std::string wefaxHourly;
        /** ★ Seconds in a WEFAX "hour" — 3600; a test shortens it. A run stopped after ≥ a twelfth of it says its part-hour. */
        double wefaxHourSecs = 3600.0;
        /** ★ No joiner can ever attach (a background WEFAX-stations channel): the picture in progress is not copied for one
         *  (up to kReplayMax, 4 MB, a decoder) — memory bounded to the gallery's own copy. */
        bool noReplay = false;
        /** ★ DEVELOPER CAPTURE ONLY (vibe_dec_dump.h, VIBE_DEC_AUDIO_DUMP): the server's words for what this host's audio
         *  went through — dial, mode, IF filter, squelch, gain … — asked once a second on the decode thread with no host
         *  lock held, and only while a capture runs. Empty = the capture's lines carry the queue figures alone. */
        std::function<std::string()> dumpState;
    };
    enum class Start { Ok, Joined, Refused, Unknown };

    DecoderHost(Env env, std::string session) : env_(std::move(env)), session_(std::move(session)) {}
    ~DecoderHost() { shutdown(); }
    DecoderHost(const DecoderHost&) = delete;
    DecoderHost& operator=(const DecoderHost&) = delete;

    const std::string& session() const { return session_; }
    /** ★★★ beta94 {"type":"xpdr_follow","id":…} from a listener's decoder socket — the transponder finder's VFO A follows
     *  that row ("" = stop). Nothing else running = ignored. */
    void xpdrFollow(const std::string& id) {
        std::lock_guard<std::mutex> dl(decMtx_);
        if (xpdr_) xpdr_->follow(id);
    }

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
                // ★ the developer capture (VIBE_DEC_AUDIO_DUMP): a new file set for this attach; nothing at all when unset
                if (vibedec::DecDump::enabled()) {
                    auto d = std::make_shared<vibedec::DecDump>(ext, msg, session_);
                    std::lock_guard<std::mutex> dl(dumpMtx_); dump_ = std::move(d);
                }
                // ★★ PAGING NEVER REACHES THE GALLERY (Stuart's spec): no tap for it at all
                if (env_.tap && ext != "paging") { DecoderTapPtr t = env_.tap(ext, msg); std::lock_guard<std::mutex> tl(tapMtx_); tap_ = std::move(t); }
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
        // ★ V12 beta91: the region the CW / RTTY chips' windows come from (Env::region; the shim keeps it in step anyway)
        if (env_.region) { const int rg = env_.region(); if (rg >= 1 && rg <= 3) skimbands::bandRegion().store(rg); }
        std::vector<digi::ModeDef> ms;   // ★ the modes the owner has not switched off (the Spotter keeps its own copy)
        for (const auto& m : digi::modes()) if (!(env_.spotModeOff && env_.spotModeOff(m.name))) ms.push_back(m);
        spotter_.reset(new digi::Spotter(scope, ms,
            [this](const digi::Spot& sp, double rfHz) { emitSpot(sp, rfHz); },
            [dial] { return dial ? dial() : 0.0; }, loaded));
        iqSpotter_.reset();
        if (scope == SpotScope::Band) {
            std::function<digi::Override(const std::string&)> ovf;
            if (env_.bandOverride) {
                auto ov = env_.bandOverride;
                ovf = [ov](const std::string& m) {
                    const int v = ov(m);
                    return v > 0 ? digi::Override::Always : v < 0 ? digi::Override::Off : digi::Override::Auto;
                };
                spotter_->setOverrides(ovf);
            }
            const double u = env_.bandBudgetUnits ? env_.bandBudgetUnits() : NAN;
            spotter_->setBudgetCap(std::isfinite(u) && u > 0 ? u : INFINITY);
            /* ★★★ V12 beta91 — THE CW / RTTY / WIDE-PSK CHIPS RUN IN A SPOTTER OF THEIR OWN (iqSpotter_), fed the same
             *  slices. The audio spotter above is then exactly what it was before the band-skim change — same modes, same
             *  plan, same gate, same governor. With the IQ modes inside it (even with no window, no run), test-server-spots
             *  missed its one-pipeline PSK31 / FT2 in ~1 run in 4; with them out of it, never (BAND-SKIM-CW-RTTY.md, the
             *  bisect). The chips' own registry is digi::iqModes(); the server's lists name them (digi::allModes).
             *  ★ Their pause rule reads the decode thread's queue AND the slotted modes' skipped slots (the audio spotter's). */
            std::vector<digi::ModeDef> iq;
            for (const auto& m : digi::iqModes()) if (!(env_.spotModeOff && env_.spotModeOff(m.name))) iq.push_back(m);
            if (!iq.empty()) {
                iqSpotter_.reset(new digi::Spotter(scope, iq,
                    [this](const digi::Spot& sp, double rfHz) { emitSpot(sp, rfHz); },
                    [dial] { return dial ? dial() : 0.0; }, loaded));
                if (ovf) iqSpotter_->setOverrides(ovf);
                /* ★★ beta91 (Stuart: "make sure the box is not overloaded"): the SAME budget as the audio spotter, less what
                 *  the slotted modes have committed — so on a weak box the chips (PSKW first, then RTTY, then CW: their
                 *  priorities 105 / 110 / 120) are shed by the governor before they add to a full load, as well as paused
                 *  by the pause rule once the decode falls behind. */
                iqSpotter_->setBudgetCap(std::isfinite(u) && u > 0 ? u : INFINITY);
                { digi::Spotter* audio = spotter_.get(); iqSpotter_->setBudgetOthers([audio] { return audio->committedUnits(); }); }
                /* ★★ THE PAUSE RULE (BAND-SKIM-CW-RTTY.md): behind = the slice IQ queued for this decode thread and not yet
                 *  decoded, or a slotted mode skipping a slot. CW then RTTY then PSKW pause first; the log says so. */
                iqSpotter_->setBehind([this] { return queuedIqSec(); });
                digi::Spotter* audio = spotter_.get();
                iqSpotter_->setSkipped([audio] { return audio->slotsSkipped(); });
                iqSpotter_->setLog([this](const std::string& l) { log(l); });
                iqSpotter_->setCpu([this](uint64_t ns) { cpuNs_.fetch_add(ns, std::memory_order_relaxed); });
            }
            /* ★★★ beta94 NOBODY LISTENING: background decoding's Band follows what it may use now — the cap × the cores
             *  allowed, and the looser pause rule while the machine has headroom (vibe_bg_fullcpu.h). */
            if (env_.bgCpu) {
                auto L = env_.bgCpu;
                auto view = [L] {
                    digi::Spotter::CpuView v;
                    v.scale = L->budgetScale(); v.skipCounts = L->skipCounts(); v.behindHoldSec = L->behindHoldSec();
                    return v;
                };
                spotter_->setCpuView(view);
                if (iqSpotter_) iqSpotter_->setCpuView(view);
            }
            statusSent_ = ~0ull; statusAt_ = 0;
            skimSent_.clear(); skimAt_ = 0;   // ★ beta92: the band skimmer's frame starts again with the run
        }
        if (env_.spotTap) { DecoderTapPtr t = env_.spotTap(scope == SpotScope::Band); std::lock_guard<std::mutex> tl(tapMtx_); spotTap_ = std::move(t); }
        spotsOn_ = true;
        if (scope == SpotScope::Band) {
            bandWidth_.store(bandWidthFor(dial ? dial() : 0.0), std::memory_order_relaxed);   // ★ a socket thread: no channel lock held
            bandWidthDial_.store(dial ? dial() : -1.0, std::memory_order_relaxed);   // ★ beta91: the chips name the band before the first block
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
    /** ★★ beta92 BAND SKIMMER: the {"type":"band_skim"} frame now (tests) — "" when Band is not running. */
    std::string spotsSkimJson() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotter_ || spotter_->scope() != SpotScope::Band) return "";
        return skimJsonLocked_();
    }
    /** What the spotter runs now ("FT8 14.0740, FT4 14.0800"), for the admin page. */
    std::string spotsDescribe() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotter_) return std::string();
        std::string d = spotter_->describe();
        if (iqSpotter_) { const std::string q = iqSpotter_->describe(); if (!q.empty()) d = q + (d.empty() ? "" : ", ") + d; }
        return d;
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
    /** ★★ beta92 THE SATELLITE LOCK'S AIM: the Doppler-corrected frequency this host's VFO is listening on now (the
     *  tracker sets it 25 times a second while a lock runs; 0 = no lock). The shim cuts the skimmer's slice near it
     *  (whole bins, moved only when the aim has drifted — SkimCut::aimHz) and the CW skimmer turns the rest out with a
     *  phase-continuous mix, so its window is centred on the corrected beacon frequency all pass and a signal keeps one
     *  offset from the prediction (vibe_cw_run.h feedIq). Atomic: set on the tracker thread, read on the DSP thread. */
    void setIqAim(double hz) { iqAim_.store(std::isfinite(hz) && hz > 0 ? hz : 0.0, std::memory_order_relaxed); }
    double iqAim() const { return iqAim_.load(std::memory_order_relaxed); }
    /** ★★ 2026-11-24 ORBCOMM: a FIXED slice centre the decoder asked for (the band's), 0 = none. When set, the caller cuts the
     *  slice there (clamped into its capture) and neither the dial nor the satellite lock's aim moves it. */
    double iqFixed() const { return iqFixed_.load(std::memory_order_relaxed); }
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
        IqBlock b; b.rate = rate; b.centre = centreRfHz; b.slice = slice; b.aim = iqAim_.load(std::memory_order_relaxed);
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
    /** ★★ PAGING (V12): built into this library at all? (VIBE_PAGING — the Android client library, and since 2026-11-16
     *  the desktop VibeServer for the R36S handheld; pagingProcess() says whether this process may use it.) */
    static constexpr bool pagingBuilt() {
#if defined(VIBE_PAGING)
        return true;
#else
        return false;
#endif
    }
    /** ★★★ May THIS PROCESS run paging at all? (V12, 2026-11-16.) The engine is compiled into the desktop VibeServer too
     *  now — for the R36S handheld, whose radio process is that binary — so being BUILT no longer means being allowed.
     *  True from the start only in the Android client library (VIBE_PAGING there is the phone's own app); everywhere
     *  else false until LocalSdrShim::setHandheld(true) — the handheld's radio process, which binds 127.0.0.1 only and
     *  is started by the handheld supervisor. A server never sets it, so "paging" is an UNKNOWN extension there
     *  (knownExt), whatever a client asks. vibe_paging_gate.h has the rest of the rule. */
    static std::atomic<bool>& pagingProcess() {
#if defined(VIBE_PAGING) && defined(__ANDROID__)
        static std::atomic<bool> on{true};
#else
        static std::atomic<bool> on{false};
#endif
        return on;
    }
    static bool pagingHere() { return pagingBuilt() && pagingProcess().load(std::memory_order_relaxed); }
    /** ★ How many paging receivers run in this process — the one atomic the DSP thread reads before it looks for a host. */
    static std::atomic<int>& pagingRunning() { static std::atomic<int> n{0}; return n; }
    /** ★ Paging runs here: it wants the pipeline's CHANNEL IQ (feedChannelIq). Cheap: read on every channel block. */
    bool wantsChannelIq() const { return pagingIq_.load(std::memory_order_relaxed) || packetIq_.load(std::memory_order_relaxed)
                                        || navIq_.load(std::memory_order_relaxed); }
    /** ★ V12 9600 PACKET (docs/v12/AX25-9600-FX25-IL2P.md): how many packet decoders in this process read the channel IQ
     *  (9600 / AUTO) — the one atomic the DSP thread reads before it looks for a host (a server with none pays nothing).
     *  Unlike paging this is every build and every server: amateur packet is a public broadcast. */
    static std::atomic<int>& packetIqRunning() { static std::atomic<int> n{0}; return n; }
    /** ★★ V12 VOR / ILS and 406 MHz beacons run on the CHANNEL IQ too (vibe_nav_run.h, vibe_sarsat_run.h) — on any radio,
     *  served or not (paging's local-only gate is paging's alone). How many run in this process: the DSP thread's one load. */
    static std::atomic<int>& navIqRunning() { static std::atomic<int> n{0}; return n; }
    /** ★ VOR runs here: its channel must hold the 9.96 ± 0.5 kHz subcarrier (the one pipeline widens it — navWideSync). */
    bool wantsWideChannel() { std::lock_guard<std::mutex> lk(decMtx_); return (bool)nav_; }
    /** ★★ beta92: a packet run here wants the pipeline's DECODER TAP (RxPipeline::setDecoderTap(vibeaprs::Run::kTapHz)) —
     *  the shim sets it on this host's pipeline while this holds (pktTapSync). One relaxed load. */
    bool wantsPacketTap() const { return packetIq_.load(std::memory_order_relaxed); }
    /** Hand paging `n` samples of the listener's channel IQ (post-NCO, dial at DC, cut to the passband) at `rate` Hz.
     *  Copies and returns at once, as feed() — the DSP thread never decodes. Two seconds behind and the oldest goes. */
    /* ★★ beta92 `wide`: the pipeline's DECODER TAP (RxPipeline::setDecoderTap — ±12 kHz, cut before the listener's channel
     *  filter), for packet alone; paging / VOR / 406 MHz keep the listener's channel. Packet reads the tap whenever one has
     *  come within the last second, else the channel (a path with no tap). */
    void feedChannelIq(const std::complex<float>* iq, int n, double rate, bool wide = false) {
        if (n <= 0 || !iq || !(rate > 0) || !wantsChannelIq()) return;
        if (wide && !packetIq_.load(std::memory_order_relaxed)) return;
        if (wide) wideAt_.store(nowMs_(), std::memory_order_relaxed);
        IqBlock b; b.rate = rate; b.v.assign(iq, iq + n); b.wide = wide;
        fed_.fetch_add((uint64_t)n, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(qM_);
            if (!qThread_.joinable()) { qStop_ = false; replayStop_.store(false); qThread_ = std::thread([this] { loop_(); }); }
            qcSec_ += (double)n / rate;
            qc_.push_back(std::move(b));
            while (qc_.size() > 1 && qcSec_ > 2.0) {
                qcSec_ -= (double)qc_.front().v.size() / qc_.front().rate;
                dropped_.fetch_add(qc_.front().v.size(), std::memory_order_relaxed);
                qc_.pop_front();
            }
        }
        qCv_.notify_one();
    }
    /** ★ Seconds of channel IQ waiting (the benchmark's IQ rows pace on it). */
    double queuedChannelIqSec() { std::lock_guard<std::mutex> lk(qM_); return qcSec_; }
    /** ★ What paging has locked ("POCSAG 1200"), or "" — for the admin page. */
    std::string pagingDetected() {
#if defined(VIBE_PAGING)
        std::lock_guard<std::mutex> lk(decMtx_);
        return paging_ ? paging_->detected() : std::string();
#else
        return std::string();
#endif
    }
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
    /** ★ V12 SATELLITE MODE: the running decoder's identity (ext + its parameters, "" = none) — a pick's drop detaches
     *  only the decoder the pick started, never one the listener has since replaced with their own. */
    std::string startKey() { std::lock_guard<std::mutex> lk(decMtx_); return startKey_; }
    /** The key start() would give `ext` with the attach message `msg`. */
    static std::string startKeyFor(const std::string& ext, const std::string& msg) { return ext + "|" + paramsOf(msg); }
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
    /** ★★ A PICTURE PART-WAY THROUGH (WEFAX between its start and its stop tone, SSTV between its VIS and its end) — so
     *  background decoding pauses an idle VFO for a listener before one in the middle of a chart (2026-11-22). A picture
     *  whose end never came stops counting after kPictureStaleS (a WEFAX chart is ≤ 25 min at 120 LPM: the gallery's
     *  3000-line cap). Lock-free. */
    static constexpr double kPictureStaleS = 1800.0;
    bool midPicture() const {
        const double t = pictureSince_.load(std::memory_order_relaxed);
        return t > 0 && steadySecs() - t < kPictureStaleS;
    }
    /** Frames waiting for the decode thread — a feeder that must NOT lose audio (a test, the
     *  benchmark) paces itself on this; the server never does, it drops. */
    size_t queued() { std::lock_guard<std::mutex> lk(qM_); return qFrames_; }
    /** …and the same for a skimmer's IQ, in seconds (the benchmark's skimmer rows pace on it — vibe_benchmark_more.h). */
    double queuedIq() { return queuedIqSec(); }
    /** "fsk"/"wefax"/"sstv"/"time"/"none" — WHICH decoder object actually exists (see the admin note
     *  on decoderKind: "attached" was once true while nothing had been constructed). */
    std::string kind() {
        std::lock_guard<std::mutex> lk(decMtx_);
        if (hfdl_) return "hfdl";
        return wefax_ ? "wefax" : sstv_ ? "sstv" : (fsk_ || rttyAuto_) ? "fsk" : time_ ? "time" : cw_ ? (cw_->skimmer() ? "cwskim" : "cw")
             : psk_ ? (psk_->skimmer() ? "pskskim" : "psk") : rttySkim_ ? "rttyskim" : dsc_ ? "dsc" : same_ ? "same" : selcal_ ? "selcal" : tm_ ? TextModeDecoder::extName(tmFam_) : hell_ ? "hell" : sitora_ ? "sitora" : pagingOn_() ? "paging" : aprs_ ? "aprs" : jtty_ ? "jtty" : ncdxf_ ? "ncdxf" : balloon_ ? "balloon" : tones_ ? "tones" : lrpt_ ? "lrpt" : orbcomm_ ? "orbcomm" : xpdr_ ? "xpdr" : fctlm_ ? "fctlm" : nav_ ? "vor" : sarsat_ ? "sarsat" : ale_ ? "ale" : "none";
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
        { std::lock_guard<std::mutex> ol(outMtx_);   // ★ the send and its note together — see joinPeer
          for (auto& p : snapshotPeers()) liveBin_(p, d, n);
          record(d, n); }
        if (DecoderTapPtr t = tap()) t->frame(d, n);
    }
    /** `gap`: which gap log keeps it for a listener coming back (GapDecoder / GapSpots), or -1 = not kept (a status frame
     *  that is re-sent anyway). */
    void broadcastText(const std::string& s, int gap = 0 /* GapDecoder */) {
        std::lock_guard<std::mutex> ol(outMtx_);
        for (auto& p : snapshotPeers()) liveText_(p, s);
        if (gap >= 0) gapNote((uint8_t)gap, false, s.data(), s.size());
    }

    /* ── ★★★ A JOINER SEES WHAT IS ALREADY IN THE BOX (Stuart, 2026-11-24: "when a new user joins they can see what is
     *  already in the decoder box such as a half drawn wefax image etc") ────────────────────────────────────────────────
     *  joinPeer makes `p` a peer of this host AND snapshots what the run has produced so far, in ONE step under outMtx_ —
     *  the lock every live send takes together with its note (record / gapNote). So every frame is either in the snapshot
     *  (sent before p was a peer) or sent to p live (after) — never both, never neither: no duplicate and no hole at the
     *  join. Live frames that arrive while the snapshot is still being sent are QUEUED for p (bounded, kCatchMax) and sent
     *  after it, in order.
     *   · the picture so far: WEFAX / SSTV / LRPT (replay_, the existing ≤ 4 MB copy) and FELD HELL (its last kHellLines
     *     lines, each kept at its latest — a strip re-sends a line as it fills);
     *   · the run's recent output: the gap log of this run (≤ 384 KB, already kept for a returning listener) — RTTY / NAVTEX
     *     / SITOR / CW / PSK text, APRS packets and frames, DSC / SELCAL / time lines … or, for `spots`, the spots;
     *   · APRS: the station list first (its own JSON, the state).
     *  Paced on a thread of its own at the socket's drain (as replayTo). No new copies are kept: both buffers existed. */
    size_t joinPeer(const DecoderPeerPtr& p, bool spots) {
        if (!p || !p->open()) return 0;
        if (!spots) {   // ★ the APRS station list — read BEFORE outMtx_ (decMtx_ is taken on the decode thread around sends)
            std::string l;
            { std::lock_guard<std::mutex> lk(decMtx_); if (aprs_) l = aprs_->listJson(true); }
            if (!l.empty()) p->text(l);
        }
        std::vector<std::pair<bool, std::string>> snap;
        {
            std::lock_guard<std::mutex> ol(outMtx_);
            if (catch_.count(p.get())) return 0;   // ★ already catching up (a second attach): the first one carries on
            if (!spots) {
                std::lock_guard<std::mutex> rl(replayMtx_);
                if (imageKind_.load(std::memory_order_relaxed) == 3) {
                    if (!hellLines_.empty()) { snap.push_back({ true, std::string(1, '\x02') });
                                               for (auto& kv : hellLines_) snap.push_back({ true, std::string((const char*)kv.second.data(), kv.second.size()) }); }
                } else for (auto& f : replay_) snap.push_back({ true, std::string((const char*)f.data(), f.size()) });
            }
            { std::lock_guard<std::mutex> gl(gapMtx_);
              const uint8_t k = spots ? GapSpots : GapDecoder;
              for (const auto& e : gap_) if (e.kind == k) snap.push_back({ e.bin, e.data }); }
            { std::lock_guard<std::mutex> lk(peerMtx_);
              prunePeersLocked();
              if (std::find(peers_.begin(), peers_.end(), p) == peers_.end()) peers_.push_back(p); }
            if (snap.empty()) return 0;
            catch_[p.get()];
            catchN_.fetch_add(1, std::memory_order_relaxed);
        }
        const size_t n = snap.size();
        log(std::string("decoder joined — sending ") + std::to_string(n) + (spots ? " spots" : " frames") + " already decoded in this run");
        std::lock_guard<std::mutex> tl(replayThrMtx_);
        replayThreads_.erase(std::remove_if(replayThreads_.begin(), replayThreads_.end(),
            [](std::unique_ptr<ReplayJob>& j) { if (j->done.load()) { if (j->th.joinable()) j->th.join(); return true; } return false; }),
            replayThreads_.end());
        auto job = std::make_unique<ReplayJob>();
        ReplayJob* jp = job.get();
        jp->th = std::thread([this, p, snap = std::move(snap), jp] {
            vibeDecoderThread("vibe-join");
            auto pace = [&] { for (int w = 0; w < 500 && p->open() && p->backlog() > kReplayBacklog && !replayStop_.load(); ++w)
                                  std::this_thread::sleep_for(std::chrono::milliseconds(20)); };
            for (auto& e : snap) {
                pace();
                if (!p->open() || replayStop_.load()) break;
                if (e.first) p->binary((const uint8_t*)e.second.data(), e.second.size()); else p->text(e.second);
            }
            // ★ then what arrived meanwhile, in order — the gate lifts only when the queue is empty, under outMtx_, so no
            //   live frame can overtake one still queued
            for (;;) {
                std::deque<std::pair<bool, std::string>> q; bool dropped = false;
                { std::lock_guard<std::mutex> ol(outMtx_);
                  auto it = catch_.find(p.get());
                  if (it == catch_.end()) break;
                  if (it->second.q.empty() || !p->open() || replayStop_.load()) {
                      dropped = it->second.dropped;
                      catch_.erase(it); catchN_.fetch_sub(1, std::memory_order_relaxed);
                      if (dropped) log("decoder joined — the live frames queued behind the catch-up overflowed; some were dropped");
                      break;
                  }
                  q.swap(it->second.q); it->second.bytes = 0; }
                for (auto& e : q) {
                    pace();
                    if (!p->open()) break;
                    if (e.first) p->binary((const uint8_t*)e.second.data(), e.second.size()); else p->text(e.second);
                }
            }
            jp->done.store(true);
        });
        replayThreads_.push_back(std::move(job));
        return n;
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
    /** ★★★ joinPeer's gate. outMtx_: every live send with its note (record / gapNote), and a joiner's snapshot. Order:
     *  outMtx_ → replayMtx_ / gapMtx_ / peerMtx_ (never decMtx_ under it — the decode thread holds decMtx_ around sends). */
    std::mutex outMtx_;
    struct CatchQ { std::deque<std::pair<bool, std::string>> q; size_t bytes = 0; bool dropped = false; };
    std::map<const DecoderPeer*, CatchQ> catch_;   // outMtx_
    std::atomic<int> catchN_{0};
    static constexpr size_t kCatchMax = 1024u * 1024;   // ★ live frames queued behind one joiner's catch-up, at most
    static constexpr size_t kHellLines = 400;           // ★ FELD HELL: the strip's last lines a joiner is shown
    std::map<uint32_t, std::vector<uint8_t>> hellLines_;   // replayMtx_
    /** outMtx_ held: to `p` now, or behind its catch-up. */
    bool catchQueue_(const DecoderPeer* p, bool bin, const void* d, size_t n) {
        if (!catchN_.load(std::memory_order_relaxed)) return false;
        auto it = catch_.find(p);
        if (it == catch_.end()) return false;
        if (it->second.bytes + n > kCatchMax) { it->second.dropped = true; return true; }
        it->second.q.push_back({ bin, std::string((const char*)d, n) });
        it->second.bytes += n;
        return true;
    }
    void liveBin_(const DecoderPeerPtr& p, const uint8_t* d, size_t n) { if (!catchQueue_(p.get(), true, d, n)) p->binary(d, n); }
    void liveText_(const DecoderPeerPtr& p, const std::string& s) { if (!catchQueue_(p.get(), false, s.data(), s.size())) p->text(s); }
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
        if (e == "hfdl") return true;   // ★ V12 HFDL (vibe_hfdl_run.h)
        return e == "fsk" || e == "navtex" || e == "wefax" || e == "sstv" || e == "time"
            || e == "msf" || e == "dcf77" || e == "rwm" || e == "wwv" || e == "wwvb" || e == "jjy" || e == "tdf" || e == "cw" || e == "cwskim" || e == "dsc" || e == "same" || e == "selcal" || e == "mfsk" || e == "olivia" || e == "thor" || e == "hell" || e == "sitora" || (pagingHere() && e == "paging") || e == "aprs" || e == "psk" || e == "pskskim" || e == "rttyskim" || e == "jtty" || e == "ncdxf" || e == "balloon" || e == "tones" || e == "lrpt" || e == "orbcomm" || e == "xpdr" || e == "fctlm" || e == "vor" || e == "sarsat" || e == "ale";
    }
    /** ★ "Same decoder, same settings": the parameters a decoder is built from, in a fixed order.
     *  A whitelist rather than "the message minus its envelope", so field ORDER, a session id or a
     *  future harmless field cannot make an identical request look different and restart a chart. */
    static std::string paramsOf(const std::string& msg) {
        std::string k;
        for (const char* f : { "center_frequency", "shift", "baud_rate", "lpm", "image_width", "carrier",
                               "deviation", "bandwidth", "width", "centre", "bits", "reverse", "horus_baud", "horus_spacing", "symbol_rate", "afc_range" }) {
            double v; k += num(msg, f, v) ? std::to_string(v) : std::string("-"); k += ',';
        }
        for (const char* f : { "encoding", "framing", "station", "band", "mode", "standard", "modulation", "diff" }) { k += str(msg, f); k += ','; }
        for (const char* f : { "\"inverted\":true", "\"use_phasing\":false", "\"auto_stop\":true", "\"auto_start\":true",
                               "\"auto\":true", "\"usos\":true", "\"rtty\":false", "\"horus\":false", "\"dtmf\":false", "\"five\":false", "\"interleaved\":true", "\"afc\":false", "\"invert\":true" })
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

    bool pagingOn_() const {
#if defined(VIBE_PAGING)
        return (bool)paging_;
#else
        return false;
#endif
    }
    bool running_() const { return hfdl_ || fsk_ || rttyAuto_ || wefax_ || sstv_ || time_ || cw_ || psk_ || rttySkim_ || dsc_ || same_ || selcal_ || tm_ || hell_ || sitora_ || pagingOn_() || aprs_ || jtty_ || ncdxf_ || balloon_ || tones_ || lrpt_ || orbcomm_ || xpdr_ || fctlm_ || nav_ || sarsat_ || ale_; }
    void clearLocked_() {
        // ★ LRPT hands the gallery its pass's pictures as it finishes, so it goes before the gallery's copy is closed
        if (lrpt_) lrpt_->finish(true);
        if (orbcomm_) orbcomm_->finish(true);   // ★ ORBCOMM's sky plot picture, the same way
        // ★ The gallery's copy is finished FIRST, while nothing more can arrive: what it holds is written now.
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t.swap(tap_); }
          if (t) t->finish(running_());
          if (wefax_) wefaxHourLocked_(true, t ? t->savedCount() : 0); }   // ★ the part-hour, with the chart just written
        wefaxHourAt_ = {};
        delete fsk_;   fsk_ = nullptr;
        delete rttyAuto_; rttyAuto_ = nullptr;
        fskState_.store(-1, std::memory_order_relaxed);
        delete wefax_; wefax_ = nullptr;
        delete sstv_;  sstv_ = nullptr;
        delete time_;  time_ = nullptr;
        if (cwIq_.exchange(false)) skimmersRunning().fetch_sub(1);
        cw_.reset();
        psk_.reset();
        rttySkim_.reset();   // ★ joins its search workers
        lrpt_.reset();
        orbcomm_.reset();
        xpdr_.reset();
        iqFixed_.store(0.0, std::memory_order_relaxed);
        fctlm_.reset();
        aprs_.reset();
        if (packetIq_.exchange(false)) packetIqRunning().fetch_sub(1);
        { std::lock_guard<std::mutex> dl(dumpMtx_); dump_.reset(); }   // ★ the capture closes with its decoder
        dsc_.reset();
        sitora_.reset();
        hfdl_.reset();
        same_.reset();
        selcal_.reset();
        tm_.reset();
        hell_.reset();
#if defined(VIBE_PAGING)
        if (paging_) { paging_.reset(); if (pagingIq_.exchange(false)) pagingRunning().fetch_sub(1); }
#endif
        jtty_.reset();
        ncdxf_.reset();
        balloon_.reset();
        tones_.reset();
        nav_.reset();
        sarsat_.reset();
        if (navIq_.exchange(false)) navIqRunning().fetch_sub(1);
        ale_.reset();
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t.swap(cwTap_); } if (t) t->finish(true); }
        if (!bandIq_.load()) { std::lock_guard<std::mutex> lk(qM_); qi_.clear(); qiFrames_ = 0; }   // ★ Band spots may still want it
        active_.store(false, std::memory_order_relaxed);
        imageKind_.store(0, std::memory_order_relaxed);
        pictureSince_.store(0.0, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> rl(replayMtx_); replay_.clear(); replayBytes_ = 0; replayDoneSeq_ = 0; done_.clear(); doneBytes_ = 0; hellLines_.clear(); }
        gapClear(GapDecoder);   // ★ a new run — see the gap log
    }
    void log(const std::string& s) { if (env_.log) env_.log(s); }
    /** ★★ THE HOURLY WEFAX LINE (Env::wefaxHourly) — decMtx_ held. `ending`: the run is stopping (paused for a listener,
     *  moved out of the window, the profile changed): the part-hour is said if it was at least a twelfth of an hour.
     *  `savedNow`: the gallery tap's count of charts written this run. */
    void wefaxHourLocked_(bool ending, size_t savedNow) {
        if (!wefax_ || env_.wefaxHourly.empty() || wefaxHourAt_ == std::chrono::steady_clock::time_point{}) return;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - wefaxHourAt_).count();
        const WefaxDecoder::Stats st = wefax_->takeStats();
        const size_t saved = savedNow >= wefaxSavedBase_ ? savedNow - wefaxSavedBase_ : savedNow;
        wefaxHourAt_ = now; wefaxSavedBase_ = savedNow;
        if (ending && secs < env_.wefaxHourSecs / 12.0) return;
        auto db = [](double p) { return p > 1e-12 ? 10.0 * std::log10(p) : -120.0; };
        const double rms = st.samples ? db(st.sumSq / (double)st.samples) : -120.0;
        const double pk = st.peak > 0 ? 20.0 * std::log10(st.peak / 32768.0) : -120.0;
        char b[640];
        std::snprintf(b, sizeof b, "wefax: %s on %s: audio %.1f dBFS rms (peak %.1f); %llu lines read; start tone at best %d of %d lines "
                      "(%d start a chart; strongest line %.1f, a tone line reads over 3.5), stop tone at best %d of %d; phasing at best "
                      "%d of %d lines agreeing (%d start a chart without a tone); %d chart(s) begun, %zu saved; phasing %d used, %d rejected",
                      ending ? "the part-hour before it stopped" : "the last hour", (env_.wefaxHourly + (ending ? " (" + std::to_string((int)std::lround(secs / 60.0)) + " min)" : std::string())).c_str(),
                      rms, pk, (unsigned long long)st.lines, st.bestStartHits, WefaxDecoder::kToneWindow(), WefaxDecoder::kToneHits(), st.bestStartLevel,
                      st.bestStopHits, WefaxDecoder::kToneWindow(), st.bestLookAgree, WefaxDecoder::kLookWindow(), WefaxDecoder::kLookHits(),
                      st.starts, saved, st.phasedUsed, st.phasedRejected);
        log(b);
    }
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
        if (kind == 3) {   // ★ FELD HELL: each line at its latest, the last kHellLines — what a joiner is shown (joinPeer)
            if (env_.noReplay) return;
            std::lock_guard<std::mutex> rl(replayMtx_);
            if (n == 1 && d[0] == 0x02) { hellLines_.clear(); return; }
            if (n >= 9 && d[0] == 0x01) {
                const uint32_t ln = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 8) | d[4];
                hellLines_[ln].assign(d, d + n);
                while (hellLines_.size() > kHellLines) hellLines_.erase(hellLines_.begin());
            }
            return;
        }
        const bool starts = (kind == 1 && n == 1 && d[0] == 0x02) || (kind == 2 && n >= 9 && d[0] == 0x07);
        // ★ No joiner will ever ask (a WEFAX-stations channel): only whether a picture is in progress is kept — no 4 MB copy
        if (env_.noReplay) {
            if (starts) pictureSince_.store(steadySecs(), std::memory_order_relaxed);
            if ((kind == 1 && n == 1 && d[0] == 0x03) || (kind == 2 && d[0] == 0x05)) pictureSince_.store(0.0, std::memory_order_relaxed);
            return;
        }
        std::lock_guard<std::mutex> rl(replayMtx_);
        if (starts) { archiveLocked_(kind); replay_.clear(); replayBytes_ = 0; replayDoneSeq_ = 0; }
        // ★ WEFAX 0x03 "transmission complete", SSTV 0x05 "image complete": the picture is finished — stamped now, so a
        //   listener who left AFTER it finished is not sent it again
        const bool ends = (kind == 1 && n == 1 && d[0] == 0x03) || (kind == 2 && d[0] == 0x05);
        if (ends) { std::lock_guard<std::mutex> gl(gapMtx_); replayDoneSeq_ = ++gapSeq_; }
        // ★ mid-picture (midPicture): from its start to its end
        if (starts) pictureSince_.store(steadySecs(), std::memory_order_relaxed);
        if (ends) pictureSince_.store(0.0, std::memory_order_relaxed);
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
    /** ★ V12 (TONES.md): one line, two versions — `priv` to the owner's sockets (DecoderPeer::privileged), `pub` to every other
     *  socket, the gap log and the gallery. A line the owner opted in to see is never kept where anyone else could replay it. */
    void textFramePriv(const std::string& pub, const std::string& priv) {
        auto frame = [](const std::string& text) {
            std::vector<uint8_t> m(13 + text.size());
            m[0] = 0x01;
            const uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (int i = 0; i < 8; i++) m[1 + i] = (uint8_t)(ts >> ((7 - i) * 8));
            const uint32_t len = (uint32_t)text.size();
            m[9] = (uint8_t)(len >> 24); m[10] = (uint8_t)(len >> 16); m[11] = (uint8_t)(len >> 8); m[12] = (uint8_t)len;
            std::memcpy(m.data() + 13, text.data(), text.size());
            return m;
        };
        const std::vector<uint8_t> a = frame(pub), b = frame(priv);
        { std::lock_guard<std::mutex> ol(outMtx_);   // ★ the send and its note together — see joinPeer
          for (auto& p : snapshotPeers()) { const auto& m = p->privileged() ? b : a; liveBin_(p, m.data(), m.size()); }
          gapNote(GapDecoder, true, a.data(), a.size()); }
        if (DecoderTapPtr t = tap()) t->text(pub);
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
        { std::lock_guard<std::mutex> ol(outMtx_);   // ★ the send and its note together — see joinPeer
          for (auto& p : snapshotPeers()) liveBin_(p, m.data(), m.size());
          gapNote(GapDecoder, true, m.data(), m.size()); }   // ★ the gap log — for a listener coming back
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
        } else if (ext == "time" || ext == "msf" || ext == "dcf77" || ext == "rwm" || ext == "wwv" || ext == "wwvb" || ext == "jjy" || ext == "tdf") {
            std::string which = ext == "time" ? str(msg, "station") : ext;
            if (which.empty()) which = "msf";
            buildTimeLocked_(which);
        } else if (ext == "cw" || ext == "cwskim") {
            buildCwLocked_(ext == "cwskim", msg);
            return;
        } else if (ext == "psk" || ext == "pskskim") {
            buildPskLocked_(ext == "pskskim", msg);
            return;
        } else if (ext == "rttyskim") {
            buildRttySkimLocked_(msg);
            return;
        } else if (ext == "lrpt") {
            buildLrptLocked_(msg);
            return;
        } else if (ext == "orbcomm") {
            buildOrbcommLocked_(msg);
            return;
        } else if (ext == "xpdr") {
            buildXpdrLocked_(msg);
            return;
        } else if (ext == "fctlm") {
            /* ★★ V12 beta90 FUNcube BPSK 1200 telemetry (vibe_fctlm.h): the listener's USB audio, the signal near "centre"
             *  (default 1200 Hz); "afc":false / "invert":true are the manual fallback under AUTO. sat_tlm per good frame,
             *  fctlm_status once a second, a readable line to the gallery.
             *  ★★★ beta92 a SATELLITE pick attaches it WIDE — "afc_range":2600 on a wide passband (★★ beta93: "centre":1500 on
             *  100–5000 Hz, the carrier where a hand-tuned USB signal sits; beta92 was 3800 on 100–7600)
             *  (vibe_sat_engine.h kTlmCentreHz) — and, once frames decode, it steers the pick's Doppler trim to where the
             *  signal really is (vibefc::TrimFollow → Engine::setDecoderTrim). */
            vibebpsk::RxCfg c;
            double v;
            if (num(msg, "centre", v) && v >= 300 && v <= 7000) c.centreHz = v;
            if (num(msg, "afc_range", v) && v >= 100 && v <= 3000) c.afcRangeHz = v;
            c.afc = msg.find("\"afc\":false") == std::string::npos;
            const bool inv = msg.find("\"invert\":true") != std::string::npos;
            vibefc::Run::Out o;
            o.json = [this](const std::string& j) { broadcastText(j); };
            o.line = [this](const std::string& l) { if (DecoderTapPtr t = tap()) t->text(l + "\n"); };
            o.satName = [s = session_]() {
                const vibe::sat::DecoderView dv = vibe::sat::decoderView(s);
                return dv.state.empty() ? std::string() : dv.satName.substr(0, 40);
            };
            o.unixNow = [] { return (double)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); };
            // ★★ beta91 TELEMETRY CHARTS: each frame, parsed, to the gallery's per-pass CSV (the pass from the satellite lock)
            o.record = [this](const vibefc::Decoded& d, const vibefc::TlmCtx& cx) { if (DecoderTapPtr t = tap()) t->satTlm(d, cx); };
            o.pass = [s = session_](vibefc::TlmCtx& cx) {
                const vibe::sat::DecoderView dv = vibe::sat::decoderView(s);
                if (dv.state.empty()) return;
                cx.aos = dv.aos; cx.los = dv.los;
                if (dv.on) cx.elDeg = dv.elDeg;
            };
            // ★★★ beta92 DRIFT FOLLOW: this listener's lock (else the shared dial's), and only a telemetry downlink's
            //     (Engine::decoderTrim refuses any other) — a hand-started decoder on a voice lock steers nothing
            auto lockKey = [s = session_]() {
                return !s.empty() && vibe::sat::engine().active(s) ? s : std::string(vibe::sat::kSharedKey);
            };
            o.trimNow = [lockKey](double& applied, double& target) { return vibe::sat::engine().decoderTrim(lockKey(), applied, target); };
            o.trimTo = [lockKey](double target) { vibe::sat::engine().setDecoderTrim(lockKey(), target); };
            o.log = [this](const std::string& l) { log(l); };
            fctlm_.reset(new vibefc::Run(c, inv, std::move(o)));
            active_.store(true, std::memory_order_relaxed);
            log("decoder attached: fctlm (FUNcube BPSK 1200) centre " + std::to_string((int)c.centreHz) + " Hz" + (c.afc ? " AFC Â±" + std::to_string((int)c.afcRangeHz) + " Hz" : " no AFC") + (inv ? " inverted" : ""));
            return;
        } else if (ext == "aprs") {
            /* ★★ APRS (V12): 1200 Bd Bell 202 on an NFM channel (VHF), or "baud_rate":300 — HF packet on USB/LSB. Its frames
             *  are JSON (aprs_packet / aprs_list), sent from inside the run on the decode thread; each new station line goes
             *  to the gallery's tap (DecoderTap::station). */
            double b = 1200;
            num(msg, "baud_rate", b);
            int baud = b < 600 ? 300 : 1200;
            /* ★★ V12 PACKET (docs/v12/AX25-9600-FX25-IL2P.md): "packet" = "auto" (1200 on the audio + 9600 on the channel
             *  IQ, FX.25 and IL2P on — the default a new client sends), "1200", "9600" or "300"; "fx25" / "il2p" 0/1 for a
             *  manual choice (both on when absent). `baud_rate` stays for older servers (a new client sends 1200 with
             *  "auto", so an old server runs 1200). The satellite's name comes from the satellite engine's lock on this
             *  listener (vibe::sat::decoderView), asked per frame — the channel IQ already carries its Doppler. */
            const std::string pk = str(msg, "packet");
            if (pk == "auto") baud = 0; else if (pk == "9600") baud = 9600; else if (pk == "300") baud = 300; else if (pk == "1200") baud = 1200;
            double fx = 1, il = 1;
            num(msg, "fx25", fx); num(msg, "il2p", il);
            vibeaprs::Run::Opts o;
            o.json = [this](const std::string& j) { broadcastText(j); };
            o.heard = [this](const vibestation::Heard& h) { if (DecoderTapPtr t = tap()) t->station(h); };
            o.rfHz = env_.dialHz;
            o.fx25 = fx != 0; o.il2p = il != 0;
            o.satName = [s = session_]() {
                const vibe::sat::DecoderView v = vibe::sat::decoderView(s);
                return v.on ? v.satName.substr(0, 40) : std::string();
            };
            aprs_.reset(new vibeaprs::Run(baud, std::move(o)));
            // ★ 9600 / AUTO read the listener's channel IQ (feedChannelIq): the shim hands it over while this is set
            if (aprs_->wantsIq()) { if (!packetIq_.exchange(true)) packetIqRunning().fetch_add(1); }
            active_.store(true, std::memory_order_relaxed);
            log(std::string("decoder attached: aprs ") + (baud == 0 ? "AUTO (1200 + 9600)" : std::to_string(baud) + " Bd")
                + (baud == 300 ? "" : std::string(" FX.25 ") + (fx != 0 || baud == 0 ? "on" : "off") + " IL2P " + (il != 0 || baud == 0 ? "on" : "off")));
            return;
        } else if (ext == "dsc") {
            buildDscLocked_(msg);
        } else if (ext == "sitora") {
            buildSitoraLocked_(msg);
        } else if (ext == "hfdl") {
            buildHfdlLocked_();
        } else if (ext == "same") {
            buildSameLocked_();
        } else if (ext == "selcal") {
            buildSelcalLocked_();
        } else if (ext == "mfsk" || ext == "olivia" || ext == "thor") {
            buildTextModeLocked_(ext, msg);
        } else if (ext == "hell") {
            buildHellLocked_();
#if defined(VIBE_PAGING)
        } else if (ext == "paging") {
            buildPagingLocked_(msg);
            return;   // ★ fed the channel IQ (feedChannelIq), not the audio: active_ stays false
#endif
        } else if (ext == "jtty") {
            buildJttyLocked_();
        } else if (ext == "ncdxf") {
            buildNcdxfLocked_();
        } else if (ext == "balloon") {
            buildBalloonLocked_(msg);
        } else if (ext == "tones") {
            buildTonesLocked_(msg);
        } else if (ext == "vor" || ext == "sarsat") {
            buildNavIqLocked_(ext);
            return;   // ★ fed the channel IQ (feedChannelIq), not the audio: active_ stays false
        } else if (ext == "ale") {
            buildAleLocked_();
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
            if (falseCall_("CW", sp.call, std::isfinite(sp.snrDb) ? (int)std::lround(sp.snrDb) : 0, rfHz, sp.utcMs / 1000)) return;
            DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; }
            if (t) t->cwSpot(sp.call, rfHz, sp.wpm, sp.snrDb, sp.how);
            /* ★★ V12 beta90 PSK REPORTER takes CW too (Stuart 2026-11-21: "yes cw skimmer should go") — a listener's CW
             *  decoder, the CW skimmer and background CW skimming all spot here. The reporter keeps its own rules (owner
             *  switch + callsign, plausible calls only, each station once a batch, call + band + mode once an hour). */
            if (std::isfinite(rfHz) && rfHz > 0) {
                vibe::spotreport::Spot rs;
                rs.mode = "CW"; rs.call = sp.call; rs.rfHz = rfHz;
                rs.snr = std::isfinite(sp.snrDb) ? (int)std::lround(sp.snrDb) : 0;
                rs.slotUtc = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
                vibe::spotreport::Reporter::shared().note(rs);
            }
            toStations_("CW", sp.call, std::string(), std::isfinite(sp.snrDb) ? (int)std::lround(sp.snrDb) : 0, rfHz, sp.utcMs / 1000);
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
    /** ★ One skimmer spot to PSK Reporter — the CW path's guards (a finite RF above 0, a NaN SNR as 0, the slot = now). */
    /** ★★ beta92 THE FALSE-DECODE FILTER FOR THE SKIMMERS (Stuart 11-23: "digimap should also include cw and rtty") — the
     *  same judgement as FT8's emitSpot (vibe_callsign_entity.h): a call no administration has, or one not shaped like a
     *  call (XX2324X), is a corrupted decode — the store counts it (falseToday) and nothing else hears of it (summary,
     *  PSK Reporter, the map). The skimmers send no grid, so the grid half never applies here. */
    bool falseCall_(const std::string& mode, const std::string& call, int snr, double rfHz, int64_t utc) {
        if (vibe::callentity::judge(call, std::string()) != vibe::callentity::kNoEntity) return false;
        toStations_(mode, call, std::string(), snr, rfHz, utc);
        return true;
    }
    static void noteReporter_(const std::string& mode, const std::string& call, double snrDb, double rfHz) {
        if (!(std::isfinite(rfHz) && rfHz > 0)) return;
        vibe::spotreport::Spot rs;
        rs.mode = mode; rs.call = call; rs.rfHz = rfHz;
        rs.snr = std::isfinite(snrDb) ? (int)std::lround(snrDb) : 0;
        rs.slotUtc = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
        vibe::spotreport::Reporter::shared().note(rs);
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
            if (falseCall_(sp.mode.empty() ? std::string("PSK31") : sp.mode, sp.call, (int)std::lround(sp.snrDb), rfHz, sp.utcMs / 1000)) return;
            DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; }
            if (t) t->cwSpot(sp.call, rfHz, 0.0f, sp.snrDb, sp.how);
            // ★★ V12 beta90: PSK Reporter too (its own rules: the owner's switch + callsign, plausible calls only, each station
            //    once a batch, call + band + mode once an hour) — "BPSK31"/"BPSK63" are its PSK31/PSK63 (adifMode).
            noteReporter_(sp.mode.empty() ? std::string("PSK31") : sp.mode, sp.call, sp.snrDb, rfHz);
            toStations_(sp.mode.empty() ? std::string("PSK31") : sp.mode, sp.call, std::string(), (int)std::lround(sp.snrDb), rfHz, sp.utcMs / 1000);
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
    /** ★★ THE RTTY SKIMMER (V12, 2026-11-14 — docs/v12/RTTY-SKIMMER.md): every RTTY signal across "width" Hz (24–96 kHz,
     *  as CW) of IQ centred on the dial, each with its own settings. The CW skimmer's IQ path (wantsIq / iqWidthHz /
     *  skimmersRunning). Its parameter searches run on the run's own worker threads, at decoder priority. */
    void buildRttySkimLocked_(const std::string& msg) {
        double w = 48000;
        { double v; if (num(msg, "width", v)) w = v; }
        w = std::max(12000.0, std::min(96000.0, w));
        RttySkimRun::Out o;
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        o.spot = [this](const rttysk::Spot& sp, double rfHz) {
            if (falseCall_("RTTY", sp.call, (int)std::lround(sp.snrDb), rfHz, sp.utcMs / 1000)) return;
            DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = cwTap_; }
            if (t) t->cwSpot(sp.call, rfHz, 0.0f, sp.snrDb, sp.how);
            // ★★ V12 beta90 (Stuart: the per-band skimmers feed PSK Reporter 24/7): the listener's RTTY skimmer and background
            //    decoding's, as ADIF RTTY, under the reporter's own rules.
            noteReporter_("RTTY", sp.call, sp.snrDb, rfHz);
            toStations_("RTTY", sp.call, std::string(), (int)std::lround(sp.snrDb), rfHz, sp.utcMs / 1000);
        };
        o.cpu = [this](uint64_t ns) { cpuNs_.fetch_add(ns, std::memory_order_relaxed); };
        o.log = [this](const std::string& l) { log(l); };
        const int nw = env_.rttySkimWorkers ? env_.rttySkimWorkers() : rttySkimDefaultWorkers();
        const int floorW = nw > 0 ? nw : rttySkimDefaultWorkers();
        if (env_.bgCpu) {
            // ★★★ beta94: background's skimmer spreads to the cores nobody is using, never below the owner's / default figure
            auto L = env_.bgCpu;
            o.workersNow = [L, floorW] { return std::max(floorW, L->rttyWorkers.load(std::memory_order_relaxed)); };
            o.background = true;
        }
        rttySkim_.reset(new RttySkimRun(w, floorW, std::move(o)));
        cwWidth_.store(w, std::memory_order_relaxed);
        if (env_.rttySkimSpotTap) { DecoderTapPtr t = env_.rttySkimSpotTap(); std::lock_guard<std::mutex> tl(tapMtx_); cwTap_ = std::move(t); }
        if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1);
        log("decoder attached: RTTY skimmer, " + std::to_string((int)std::lround(w / 1000)) + " kHz, "
            + std::to_string(rttySkim_->workers()) + " search worker" + (rttySkim_->workers() == 1 ? "" : "s"));
    }
    /** ★★ V12 METEOR-M LRPT (vibe_lrpt_run.h): an IQ slice of the dial (the skimmers' path), the picture as SSTV frames,
     *  the pass's PNGs to the gallery's copy. "symbol_rate" 72000 | 80000 (per satellite — the client takes it from the
     *  satellite's entry), "modulation" auto | qpsk | oqpsk, "diff" auto | on | off, "interleaved" (the 80k mode). */
    void buildLrptLocked_(const std::string& msg) {
        vibelrpt::Run::Opts o;
        /* ★ "symbol_rate" from the satellite's entry (sats.json) when the client has one; absent / 0 = AUTO over 72k and
         *  80k (the clients send nothing until the satellite engine tells them — docs/v12/LRPT.md §Interface). */
        double sr = 0; num(msg, "symbol_rate", sr);
        o.rateAuto = !(sr > 0);
        o.symbolRate = sr >= 76000 ? 80000 : 72000;
        const std::string m = str(msg, "modulation"), d = str(msg, "diff");
        o.mod = m == "qpsk" ? vibelrpt::ReceiverConfig::Qpsk : m == "oqpsk" ? vibelrpt::ReceiverConfig::Oqpsk : vibelrpt::ReceiverConfig::Auto;
        o.diff = d == "on" ? vibelrpt::ReceiverConfig::DiffOn : d == "off" ? vibelrpt::ReceiverConfig::DiffOff : vibelrpt::ReceiverConfig::DiffAuto;
        o.interleaved = msg.find("\"interleaved\":true") != std::string::npos || (o.symbolRate == 80000 && msg.find("\"interleaved\":false") == std::string::npos);
        o.json = [this](const std::string& j) { broadcastText(j, -1); };
        o.frame = [this](const uint8_t* f, size_t n) { sendSstv(std::vector<uint8_t>(f, f + n)); };
        /* ★★ THE SATELLITE ENGINE'S DOPPLER (vibe_sat_engine.h decoderView, docs/v12/SATELLITE-ENGINE.md "the decoder hook").
         *  LRPT's IQ is its OWN slice of the shared FFT cut at the listener's DIAL (feedSkimFromBins → clientCentreBin, the
         *  nominal) — NOT the channel NCO the lock moves — so it applies the model's Doppler itself (modelHz: received minus
         *  nominal, exactly Run's "shift to remove"), never inChannelHz on top of a channel that already carries it. No
         *  lock for this listener or the shared dial = 0: the receiver acquires and tracks by itself. An Env override wins. */
        if (env_.satDopplerHz) o.dopplerHz = env_.satDopplerHz;
        else {
            const std::string sess = session_;
            o.dopplerHz = [sess] { const vibe::sat::DecoderView v = vibe::sat::decoderView(sess); return v.on ? v.modelHz : 0.0; };
        }
        /* ★★ beta91 (the live Meteor-M2-4 pass, 2026-11-22): the pick also says WHICH rate (a client key attaches with
         *  none, and AUTO spent the pass's best 13 s on 80k) and whether the satellite is still up (the pass ends at its
         *  setting, never on a gap in the frames — vibe_lrpt_run.h passOver). Asked once a second; an Env override wins. */
        if (env_.satInRange) o.satInRange = env_.satInRange;
        else {
            const std::string sess = session_;
            o.satInRange = [sess] { return vibe::sat::inRangeOf(vibe::sat::decoderView(sess)); };
            o.satSymbolRate = [sess] { return vibe::sat::lrptSymbolRateOf(vibe::sat::decoderView(sess)); };
        }
        o.pictures = [this](const std::vector<vibelrpt::Picture>& ps, bool partial) {
            DecoderTapPtr t = tap();
            if (!t) return;
            if (!ps.empty() && (!ps.front().geoJson.empty() || !ps.front().geoWhy.empty())) t->outline(ps.front().geoJson, ps.front().geoWhy);
            for (const auto& p : ps) t->picture(p.name, p.png, p.w, p.h, partial);
        };
        /* ★★ V12 THE MAP OUTLINE (vibe_lrpt_overlay.h): which METEOR it may be — the pick's, if this listener's dial is the
         *  satellite engine's lock (marked "picked"), and every METEOR with an LRPT downlink and an element set — this
         *  server's place, and OUR corrected clock. An Env override wins (tests). */
        if (env_.lrptGeoCandidates) o.geoCandidates = env_.lrptGeoCandidates;
        else {
            const std::string sess = session_;
            o.geoCandidates = [sess] {
                const vibe::sat::DecoderView v = vibe::sat::decoderView(sess);
                return vibe::sat::engine().lrptCandidates(v.state.empty() ? 0 : v.norad);
            };
            o.geoObserver = [](vibe::sat::Observer& ob) { return vibe::sat::engine().observer(ob); };
        }
        if (env_.lrptGeoObserver) o.geoObserver = env_.lrptGeoObserver;
        o.nowUnix = env_.nowUnix ? env_.nowUnix : std::function<double()>([] { return vibeUtcNow(); });
        o.log = [this](const std::string& l) { log(l); };
        if (env_.satGainNote) o.gainNote = env_.satGainNote;   // ★ beta92 the per-pass summary's "gain"
        lrpt_.reset(new vibelrpt::Run(std::move(o)));
        imageKind_.store(2, std::memory_order_relaxed);   // ★ the SSTV frame shapes: a joiner is caught up on the picture
        cwWidth_.store(vibelrpt::Run::kSliceWidthHz, std::memory_order_relaxed);
        if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1);
        log("decoder attached: METEOR LRPT, " + (o.rateAuto ? std::string("72k / 80k AUTO") : std::to_string((int)o.symbolRate) + " sym/s") + ", modulation " + (m.empty() ? "auto" : m)
            + ", differential " + (d.empty() ? "auto" : d));
    }
    /** ★★★ beta94 THE TRANSPONDER SIGNAL FINDER (vibe_xpdr_run.h, docs/v12/TRANSPONDER-FINDER.md): an IQ slice FIXED on the
     *  span's satellite-frame centre ("centre" — the transponder and its beacon, vibe_sat_engine.h decoderOf), "width" Hz
     *  wide (the span + the whole Doppler swing), every CW signal skimmed and every voice found in it; xpdr_status once a
     *  second. The Doppler is turned out by the run from the satellite engine's model (this listener's lock, else the
     *  room's); VFO A's follow goes through the same lock (Engine::setFollow). */
    void buildXpdrLocked_(const std::string& msg) {
        vibexpdr::Run::Opts o;
        double w = 100000; num(msg, "width", w);
        o.widthHz = std::max(20000.0, std::min(400000.0, w));
        double v = 0;
        if (num(msg, "centre", v) && v > 1e6) o.centreHz = v;
        if (num(msg, "lo", v) && v > 1e6) o.spanLoHz = v;
        if (num(msg, "hi", v) && v > 1e6) o.spanHiHz = v;
        if (num(msg, "beacon", v) && v > 1e6) o.beaconHz = v;
        if (num(msg, "up", v) && v > 1e6) o.upHz = v;
        o.inverting = !(num(msg, "inv", v) && v == 0);
        o.usb = str(msg, "sb") != "lsb";
        if (!(o.centreHz > 0)) { o.centreHz = env_.dialHz ? env_.dialHz() : 0.0; }
        if (!(o.spanHiHz > o.spanLoHz)) { o.spanLoHz = o.centreHz - o.widthHz / 4; o.spanHiHz = o.centreHz + o.widthHz / 4; }
        iqFixed_.store(o.centreHz > 1e6 ? o.centreHz : 0.0, std::memory_order_relaxed);   // ★ the span's centre: the slice stays there
        o.json = [this](const std::string& j) { broadcastText(j, -1); };
        o.log = [this](const std::string& l) { log(l); };
        const std::string sess = session_;
        const double cen = o.centreHz;
        // ★ the model's Doppler at `hz` now: this listener's lock (else the room's), scaled from its nominal; none = 0
        if (env_.satDopplerHz) { auto f = env_.satDopplerHz; o.dopplerHz = [f, cen](double hz) { return cen > 0 ? f() * hz / cen : 0.0; }; }
        else o.dopplerHz = [sess](double hz) {
            const vibe::sat::DecoderView dv = vibe::sat::decoderView(sess);
            return dv.on && dv.downlinkHz > 0 ? dv.modelHz * hz / dv.downlinkHz : 0.0;
        };
        // ★ VFO A's follow: the same lock decoderView reads (this listener's, else the room's)
        auto keyOf = [sess]() -> std::string {
            if (!sess.empty() && vibe::sat::engine().active(sess)) return sess;
            return vibe::sat::engine().active(vibe::sat::kSharedKey) ? std::string(vibe::sat::kSharedKey) : std::string();
        };
        o.follow.state = [keyOf](double& nominal, bool& following, unsigned& seq) {
            const std::string k = keyOf();
            if (k.empty()) return false;
            const auto f = vibe::sat::engine().followState(k);
            if (!f.lock || !f.passband) return false;
            nominal = f.nominal; following = f.following; seq = f.seq;
            return true;
        };
        o.follow.set = [keyOf](double hz) { const std::string k = keyOf(); if (!k.empty()) vibe::sat::engine().setFollow(k, hz); };
        o.follow.clear = [keyOf] { const std::string k = keyOf(); if (!k.empty()) vibe::sat::engine().clearFollow(k); };
        xpdr_.reset(new vibexpdr::Run(std::move(o)));
        cwWidth_.store(std::max(20000.0, std::min(400000.0, w)), std::memory_order_relaxed);
        if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1);
        log("decoder attached: TRANSPONDER FINDER, a " + std::to_string((int)std::lround(w / 1000)) + " kHz slice centred " + std::to_string((long long)std::llround(cen)) + " Hz");
    }
    /** ★★ V12 2026-11-24 ORBCOMM (vibe_orbcomm_run.h, docs/v12/ORBCOMM.md): an IQ slice of the dial ("width": one listed
     *  channel ± Doppler = 40 kHz, a listed band up to 640 kHz — vibe_sat_engine.h decoderOf), every ORBCOMM carrier in it
     *  decoded. "max_carriers" 1–6 is the manual fallback under AUTO (4). Housekeeping as lines + orbcomm_status; subscriber
     *  messages only as raw bytes; sky dots to the machine's sky plot; the plot's PNG to the gallery at the pass's end. */
    void buildOrbcommLocked_(const std::string& msg) {
        vibeorb::Run::Opts o;
        double w = 40000; num(msg, "width", w);
        o.widthHz = std::max(20000.0, std::min(640000.0, w));
        double mc = 0; num(msg, "max_carriers", mc);
        o.maxChans = mc >= 1 ? std::min(vibeorb::kMaxChans, (int)mc) : 0;
        double fc = 0; num(msg, "centre", fc);
        iqFixed_.store(fc > 1e6 ? fc : 0.0, std::memory_order_relaxed);   // ★ the band's centre: the slice stays there
        o.json = [this](const std::string& j) { broadcastText(j, -1); };
        o.text = [this](const std::string& t) { textFrame(t); };
        o.birds = [] {
            std::vector<vibeorb::Bird> v;
            for (auto& e : vibe::sat::engine().orbcommElements()) { vibeorb::Bird b; b.norad = e.norad; b.name = e.name; b.ok = b.sgp.init(e.el); v.push_back(std::move(b)); }
            return v;
        };
        o.observer = [](vibe::sat::Observer& ob) { return vibe::sat::engine().observer(ob); };
        const std::string sess = session_;
        o.pickNorad = [sess] { const vibe::sat::DecoderView v = vibe::sat::decoderView(sess); return v.consumer == "orbcomm" ? v.norad : 0LL; };
        if (env_.satInRange) o.satInRange = env_.satInRange;
        else o.satInRange = [sess] { return vibe::sat::inRangeOf(vibe::sat::decoderView(sess)); };
        o.dots = [](const std::vector<vibe::skyplot::Dot>& d) { vibe::skyplot::shared().add(d); };
        o.skyPng = [](std::string& title) {
            vibe::skyplot::Store::Filter f; f.kind = vibe::skyplot::kOrbcomm; f.radio = vibe::logradio::mine();
            const auto dots = vibe::skyplot::shared().read(f);
            if (dots.empty()) return std::string();
            title = "ORBCOMM SKY PLOT - " + std::to_string(dots.size()) + " DOTS" + (f.radio.empty() ? std::string() : " - RADIO " + f.radio);
            return vibe::skyplot::png(dots, title);
        };
        o.picture = [this](const std::string& name, const std::string& png, int pw, int ph, bool partial) {
            if (DecoderTapPtr t = tap()) t->picture(name, png, pw, ph, partial);
        };
        o.log = [this](const std::string& l) { log(l); };
        orbcomm_.reset(new vibeorb::Run(std::move(o)));
        cwWidth_.store(std::max(20000.0, std::min(640000.0, w)), std::memory_order_relaxed);
        if (!cwIq_.exchange(true)) skimmersRunning().fetch_add(1);
        log("decoder attached: ORBCOMM, a " + std::to_string((int)std::lround(w / 1000)) + " kHz slice, " + (mc >= 1 ? std::to_string((int)mc) + " carriers at most" : std::string("carriers AUTO")));
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
    /** ★★ HFDL (V12, 2026-11-16): the HF aircraft datalink on the listener's USB dial (the published HFDL frequency; the
     *  carrier 1440 Hz above it in the audio). One line per message in the box (never a message body — vibe_hfdl_run.h),
     *  and every aircraft into the radio's table, which /addon/hfdl/aircraft and the HFDL MAP read. No parameters. */
    void buildHfdlLocked_() {
        HfdlRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        // ★ the dial asked at most once a second (it takes clientMtx): a message's frequency is the dial it was heard on
        auto dial = env_.dialHz;
        auto last = std::make_shared<std::pair<double, double>>(0.0, 0.0);
        std::function<double()> dialNow;
        if (dial) dialNow = [dial, last] {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (t - last->first >= 1.0) { last->first = t; last->second = dial(); }
            return last->second;
        };
        hfdl_.reset(new HfdlRun(env_.aircraft ? *env_.aircraft : vibeavi::sharedTracker(), std::move(o), std::move(dialNow)));
        if (!env_.aircraft) vibeavi::shareTrackerProvide();   // ★ V12 the AVIATION MAP: this radio's HFDL into the shared table
        log("decoder attached: HFDL (USB dial, carrier 1440 Hz above it)");
    }
    /** ★★ EAS / SAME (V12, 2026-10-31): the US Emergency Alert System headers (decoders/same_decoder.h, 47 CFR § 11.31)
     *  on the listener's audio — NOAA Weather Radio (162.400–162.550 MHz NFM) or any EAS participant's broadcast. Text:
     *  one line per alert; 0x03: 0 listening · 1 reading a burst; an end-of-message line once per NNNN. No parameters. */
    void buildSameLocked_() {
        same_.reset(new SameDecoder(48000));
        same_->onAlert = [this](const SameDecoder::Alert& a) { textFrame(SameDecoder::line(a)); };
        same_->onEom = [this] { textFrame("[SAME] end of message (NNNN)\n"); };
        same_->onState = [this](int st) { uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
        log("decoder attached: EAS / SAME");
    }
    /** ★★ SELCAL (V12, 2026-10-31): the aeronautical selective call (decoders/selcal_decoder.h — ICAO Annex 10 Vol III,
     *  Amendment 91's 32 tones) on the listener's audio: HF USB on the aeronautical bands, or VHF AM. One line per call. */
    /** ★★ THE MFSK KEYBOARD MODES (V12, 2026-10-31): "mfsk" = ONE key for MFSK16/32, OLIVIA, CONTESTIA and THOR (one
     *  TextModeDecoder, Family::All — every family's AUTO in one, sharing spectra; "any-<family>" a family's AUTO); the old
     *  "olivia" / "thor" names still attach their family alone (no client offers them) — decoders/textmodes.h. "mode" = one of its modes() ids, AUTO
     *  for anything else. Text as it decodes (0x01), 0x03 0 searching / 1 locked, and {"type":"textmode_status"} on
     *  change: {detected, hz} — the sub-mode AUTO found and where. */
    void buildTextModeLocked_(const std::string& ext, const std::string& msg) {
        TextModeDecoder::familyOf(ext, tmFam_);
        std::string mode = str(msg, "mode");
        if (mode.empty()) mode = "auto";
        tm_.reset(new TextModeDecoder(tmFam_, 48000, mode));
        tm_->onText = [this](const std::string& t) { textFrame(t); };
        tm_->onState = [this](int st) { uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
        tm_->onStatus = [this](const std::string& j) { broadcastText("{\"type\":\"textmode_status\",\"data\":" + j + "}", -1); };
        log("decoder attached: " + ext + " " + mode);
    }
    /** ★★ FELD HELL (V12, 2026-10-31): drawn, not read (decoders/feld_hell.h). The strip being drawn goes out as WEFAX's
     *  0x01 lines (line number, width, grey bytes) — re-sent as it fills, so a client that draws line y at y watches it
     *  grow; 0x02 once at the start (a new picture); {"type":"hell_status","data":{hz, signal}} on change. imageKind_ 3: its
     *  last kHellLines lines, each at its latest, are kept for a listener who joins part-way (joinPeer) — not replay_'s
     *  first 4 MB, which a never-ending strip would fill with its oldest lines. */
    void buildHellLocked_() {
        hell_.reset(new FeldHellDecoder(48000));
        imageKind_.store(3, std::memory_order_relaxed);   // ★★★ its last lines kept for a joiner (record, joinPeer)
        hell_->onLine = [this](uint32_t ln, uint32_t w, const uint8_t* px) {
            std::vector<uint8_t> m; m.reserve(9 + w);
            m.push_back(0x01); put32(m, ln); put32(m, w); m.insert(m.end(), px, px + w);
            broadcast(m.data(), m.size());
        };
        hell_->onStatus = [this](const std::string& j) { broadcastText("{\"type\":\"hell_status\",\"data\":" + j + "}", -1); };
        { uint8_t b = 0x02; broadcast(&b, 1); }
        log("decoder attached: hell");
    }
    void buildSelcalLocked_() {
        selcal_.reset(new SelcalDecoder(48000));
        selcal_->onCode = [this](const SelcalDecoder::Code& c) {
            char b[160];
            std::snprintf(b, sizeof b, "[SELCAL] %s \xc2\xb7 pulses %.1f s + %.1f s, gap %.2f s \xc2\xb7 %.0f dB\n",
                          c.code.c_str(), c.pulse1, c.pulse2, c.gap, c.snrDb);
            textFrame(b);
        };
        selcal_->onState = [this](int st) { uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
        log("decoder attached: SELCAL");
    }
#if defined(VIBE_PAGING)
    /** ★★ PAGING (V12, 2026-10-31): POCSAG with AUTO (vibe_paging_run.h). "mode" = a PagingDecoder::modes() id ("auto",
     *  "pocsag1200" …; unknown → auto). The shim has already checked it may run here (vsPagingAllowed). */
    void buildPagingLocked_(const std::string& msg) {
        const PagingDecoder::Mode m = PagingDecoder::modeFromId(str(msg, "mode"));
        PagingRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t b[2] = { 0x03, st }; broadcast(b, 2); };
        o.json = [this](const std::string& j) { broadcastText(j, -1); };   // ★ a status, re-sent on change: not in the gap log
        paging_.reset(new PagingRun(m, std::move(o)));
        if (!pagingIq_.exchange(true)) pagingRunning().fetch_add(1);
        log(std::string("decoder attached: paging, ") + PagingDecoder::idOf(m));
    }
#endif
    /** ★★ JTTY (V12, 2026-10-26): the WSJT-X 3.2 keyboard mode — every JTTY signal in the listener's passband at once
     *  (decoders/jtty_decoder.h); vibe_jtty_run.h shapes what is sent. No parameters: there is nothing to set. */
    void buildJttyLocked_() {
        JttyRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
        std::function<double(double)> rf = [rfOf, dial](double a) { return rfOf ? rfOf(a) : (dial ? dial() : 0.0) + a; };
        // ★ V12 beta90: JTTY's callers to the station store too (with the CQ's own grid when it came with it)
        o.spot = [this](const jtty::Spot& sp, double rfHz) { toStations_("JTTY", sp.call, sp.grid, (int)std::lround(sp.snrDb), rfHz, 0); };
        jtty_.reset(new JttyRun(std::move(rf), std::move(o)));
        active_.store(true, std::memory_order_relaxed);
        log("decoder attached: JTTY");
    }
    /** ★★ V12 2026-11-16 THE NCDXF / IARU BEACON TRACKER (decoders/ncdxf_tracker.h, vibe_ncdxf_run.h): the 18 beacons on
     *  the five bands, on the published 3-minute schedule, against the block's capture UTC. No parameters: the band is the
     *  listener's dial. */
    void buildNcdxfLocked_() {
        NcdxfRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j); };
        auto rfOf = env_.rfOfAudio; auto dial = env_.dialHz;
        std::function<double(double)> rf = [rfOf, dial](double a) { return rfOf ? rfOf(a) : (dial ? dial() : 0.0) + a; };
        ncdxf_.reset(new NcdxfRun(std::move(rf), std::move(o)));
        active_.store(true, std::memory_order_relaxed);
        log("decoder attached: NCDXF beacons");
    }
    /** ★★ V12 BALLOONS (vibe_balloon_run.h, decoders/balloon_rx.h): UKHAS RTTY telemetry, Horus Binary and SSDV pictures on
     *  the listener's USB audio, all at once. AUTO by default; any of "baud_rate", "bits" (7/8), "reverse" (0/1), "shift",
     *  "horus_baud", "horus_spacing" pins that part of the search (the manual controls under AUTO); "rtty":false /
     *  "horus":false switch a half off. */
    void buildBalloonLocked_(const std::string& msg) {
        double baud = 0, bits = 0, rev = -1, shift = 0, hb = 100, hs = 0;
        num(msg, "baud_rate", baud); num(msg, "bits", bits); num(msg, "reverse", rev); num(msg, "shift", shift);
        num(msg, "horus_baud", hb); num(msg, "horus_spacing", hs);
        const bool rtty = msg.find("\"rtty\":false") == std::string::npos, horus = msg.find("\"horus\":false") == std::string::npos;
        BalloonRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j, -1); };   // ★ a status, re-sent: not in the gap log
        o.frame = [this](const std::vector<uint8_t>& f) { broadcast(f.data(), f.size()); };
        balloon_.reset(new BalloonRun(BalloonRun::configFrom(baud, bits, rev, shift, hb, hs, rtty, horus), std::move(o)));
        active_.store(true, std::memory_order_relaxed);
        log("decoder attached: balloons");
    }
    /** ★★ V12 2026-11-19 HF ALE (decoders/ale_decoder.h, docs/v12/HF-DATA.md): 2G ALE soundings, calls and links on the
     *  listener's USB audio, one line a transmission ("[ALE] 21:04:12  call  K1ABC → W1XYZ"). ALE nets hop: whatever
     *  lands on the dial is read. No parameters. ⚖ Addresses are shown; an AMD message's TEXT (content) is shown on an
     *  amateur frequency (the band plan of this server's region, as the tone decoders decide it — HFLink's channels are
     *  amateur), elsewhere only with the owner's opt-in (Env::aleAmd) and then to the owner's own sockets only
     *  (textFramePriv) — never by a client's say-so. 0x03 1 while a transmission is being read; {"type":"ale_status"}. */
    void buildAleLocked_() {
        ale_.reset(new AleDecoder(48000));
        ale_->clock = [this]() {
            const std::time_t t = std::isfinite(decUtc_) ? (std::time_t)decUtc_ : std::time(nullptr);
            std::tm g{};
#if defined(_WIN32)
            gmtime_s(&g, &t);
#else
            gmtime_r(&t, &g);
#endif
            char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec); return std::string(b);
        };
        ale_->onLine = [this](const std::string& pub, const std::string& priv) {
            if (priv.empty()) { textFrame(pub); return; }
            const bool ham = !vibetones::amateurBand(env_.dialHz ? env_.dialHz() : 0.0, env_.region ? env_.region() : 1).empty();
            if (ham) textFrame(priv);
            else if (env_.aleAmd && env_.aleAmd()) textFramePriv(pub, priv);
            else textFrame(pub);
        };
        ale_->onState = [this](int st) { uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
        ale_->onStatus = [this](const std::string& j) { broadcastText("{\"type\":\"ale_status\",\"data\":" + j + "}", -1); };
        active_.store(true, std::memory_order_relaxed);
        log("decoder attached: ALE");
    }
    /** ★★ V12 TONE DECODERS (vibe_tones_run.h, decoders/tones_rx.h): DTMF and 5-tone selective calling on the listener's NFM / AM
     *  audio, both at once. "dtmf":false / "five":false switch one off; "standard" pins the 5-tone table (a toneTables() id;
     *  anything else AUTO). WHAT IS SHOWN is decided in TonesRun by the dial's band (amateur or not) and the owner's opt-in
     *  (Env::tonesDigits) — never by the client. */
    /** ★★ V12 VOR / ILS ("vor", vibe_nav_run.h) and 406 MHz beacons ("sarsat", vibe_sarsat_run.h): both on the listener's
     *  IQ (feedChannelIq — their slice, or the one pipeline's channel; the dial near DC), so the audio chain's filters
     *  never touch a phase; no
     *  parameters (AUTO: VOR or ILS is read from the signal, the channel says which to expect). */
    void buildNavIqLocked_(const std::string& ext) {
        if (ext == "vor") {
            NavRun::Out o;
            o.text = [this](const std::string& t) { textFrame(t); };
            o.textPriv = [this](const std::string& pub, const std::string& priv) { textFramePriv(pub, priv); };
            o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
            o.json = [this](const std::string& j) { broadcastText(j, -1); };
            nav_.reset(new NavRun(env_.dialHz, env_.position, std::move(o)));
        } else {
            SarsatRun::Out o;
            o.text = [this](const std::string& t) { textFrame(t); };
            o.textPriv = [this](const std::string& pub, const std::string& priv) { textFramePriv(pub, priv); };
            o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
            o.json = [this](const std::string& j) { broadcastText(j, -1); };
            sarsat_.reset(new SarsatRun(env_.dialHz, std::move(o)));
        }
        if (!navIq_.exchange(true)) navIqRunning().fetch_add(1);
        log(ext == "vor" ? "decoder attached: VOR / ILS" : "decoder attached: 406 MHz beacons");
    }
    void buildTonesLocked_(const std::string& msg) {
        TonesRun::Config c;
        c.dtmf = msg.find("\"dtmf\":false") == std::string::npos;
        c.five = msg.find("\"five\":false") == std::string::npos;
        c.pin = tones::tableIndex(str(msg, "standard"));
        c.digits5 = env_.tonesDigits ? env_.tonesDigits() : false;
        TonesRun::Out o;
        o.text = [this](const std::string& t) { textFrame(t); };
        o.textPriv = [this](const std::string& pub, const std::string& priv) { textFramePriv(pub, priv); };
        o.state = [this](uint8_t st) { uint8_t m[2] = { 0x03, st }; broadcast(m, 2); };
        o.json = [this](const std::string& j) { broadcastText(j, -1); };
        tones_.reset(new TonesRun(c, env_.dialHz, env_.region, std::move(o)));
        active_.store(true, std::memory_order_relaxed);
        log(std::string("decoder attached: tones (") + (c.dtmf ? "DTMF" : "") + (c.dtmf && c.five ? " + " : "") + (c.five ? "5-tone" : "")
            + (c.pin >= 0 ? std::string(", ") + tones::toneTables()[c.pin].name : std::string()) + ")");
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
            : which == "jjy"   ? TimeDecoder::Station::JJY
            : which == "tdf"   ? TimeDecoder::Station::TDF     // ★ V12 2026-11-16 ALS162's phase code (tdf_phase.h)
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
            // ★ JJY sends JST (UTC+9) — said, so a Japanese clock is not read as UTC
            std::snprintf(buf, sizeof(buf), "%s  %s %04d-%02d-%02d %02d:%02d%s  %s%s%s%s\n",
                          name.c_str(), kDay[t.weekday >= 1 && t.weekday <= 7 ? t.weekday : 0],
                          t.year, t.month, t.day, t.hour, t.minute, name == "JJY" ? " JST" : "",
                          t.dst ? "(summer time)" : "", t.leapSecondPending ? " LEAP SECOND PENDING" : "",
                          dstChange, extra);
            {
                std::lock_guard<std::mutex> bl(textMtx_);
                textBuf_ += buf;
            }
            /* ★★ V12 (R36S handheld, 2026-11-05): "SET CLOCK FROM <station>". onTime fires ON the minute boundary, and
             *  only once a minute has passed its parity AND agreed with the one before it (finishMinute's lastStamp_) —
             *  the two-consecutive-minutes rule Stuart asked for is the decoder's own. So the moment is stated here as a
             *  pair: the minute in UTC as the station says it, and this host's clock as the boundary was read. A client
             *  that wants to set a clock applies (utcMs + rxDelayMs) − hostMs to "now", whenever it likes — the host
             *  clock moves evenly, so the answer does not go stale.
             *  ★ The station's own time zone comes off first: MSF sends UK civil time (BST in summer), DCF77 CET/CEST,
             *    JJY JST; WWV and WWVB send UTC. RWM never gets here (it has no timecode).
             *  ★ rxDelayMs: the receive path between the air and this callback — the decoder reads the marker's edge a
             *    10 ms envelope frame late and the audio reaches it one DSP block (~21 ms) after capture. An estimate,
             *    stated so it can be checked, not hidden in the arithmetic. Additive JSON: older clients ignore it. */
            {
                std::tm tmv{}; tmv.tm_year = t.year - 1900; tmv.tm_mon = t.month - 1; tmv.tm_mday = t.day;
                tmv.tm_hour = t.hour; tmv.tm_min = t.minute; tmv.tm_sec = 0;
                const long long civil = (long long)timegm(&tmv);
                int offMin = 0;
                if (name == "MSF") offMin = t.dst ? 60 : 0;
                else if (name == "DCF77" || name == "TDF") offMin = t.dst ? 120 : 60;   // ★ TDF: French legal time, CET/CEST
                else if (name == "JJY") offMin = 540;
                const long long utcMs = (civil - offMin * 60LL) * 1000LL;
                const long long hostMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (t.year >= 2020 && t.year < 2100) {
                    char js[200];
                    std::snprintf(js, sizeof js, "{\"type\":\"time_sync\",\"data\":{\"station\":\"%s\",\"utcMs\":%lld,"
                                  "\"hostMs\":%lld,\"rxDelayMs\":%d}}", name.c_str(), utcMs, hostMs,
                                  // ★ TDF's phase front end opens its "dip" kDipAfterSecondMs after the top of the second
                                  31 + (name == "TDF" ? (int)vibe::TdfPhase::kDipAfterSecondMs : 0));
                    broadcastText(js, -1);
                }
            }
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
        const std::string c = safe(sp.call, 32), m = safe(sp.mode, 12);
        std::string g = safe(sp.grid, 8);
        if (c.empty() || m.empty()) return;
        /* ★★★ beta92 THE FALSE-DECODE FILTER (vibe_callsign_entity.h, docs/v12/DIGI-MAP-FILTER.md — Stuart's Pi 500 map
         *  drew "132XQA" and "7Q8FAZ" at -74.5°): a call no administration was allocated is not a spot — the store counts
         *  it (falseToday) and nothing else sees it (summary, PSK Reporter, the listener's list and live map); a grid the
         *  call's administration cannot be at is dropped here, so no list or map places it. Hashed "<…>" calls and
         *  free text never reach here (the engines spot only calls). */
        {
            const vibe::callentity::Verdict v = vibe::callentity::judge(c, g);
            if (v == vibe::callentity::kNoEntity) { toStations_(m, c, g, sp.snr, rfHz, (int64_t)(ts / 1000)); return; }
            if (v == vibe::callentity::kImplausible) g.clear();
        }
        { DecoderTapPtr t; { std::lock_guard<std::mutex> tl(tapMtx_); t = spotTap_; } if (t) t->spot(m, c, g, sp.snr, rfHz); }
        /* ★★ V12 beta90 PSK REPORTER (vibe_spot_report.h, docs/v12/PSK-REPORTER.md): every spot this server decodes — a
         *  listener's run or background decoding — goes to the process's reporter, which drops it at once unless the owner
         *  switched reporting on with a callsign. The station heard only: nothing about who was listening. Cheap (a map
         *  insert under a lock); the network is the reporter's own thread, every five minutes. */
        {
            vibe::spotreport::Spot rs;
            rs.mode = m; rs.call = c; rs.grid = g; rs.snr = sp.snr; rs.rfHz = rfHz; rs.slotUtc = (double)ts / 1000.0;
            rs.dt = std::isfinite(sp.dt) ? sp.dt : 0.f; rs.drift = std::isfinite(sp.drift) ? sp.drift : 0.f;
            rs.dbm = sp.dbm >= -30 && sp.dbm <= 70 ? sp.dbm : -1000;
            vibe::spotreport::Reporter::shared().note(rs);
        }
        toStations_(m, c, g, sp.snr, rfHz, (int64_t)(ts / 1000));
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
            "\"snr\":%d,\"frequency\":%.0f,\"band\":\"%s\",\"grid\":\"%s\",\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%llu,\"dt\":%.2f}}",
            m.c_str(), c.c_str(), sp.snr, rfHz, bandFor(rfHz), g.c_str(), how.c_str(), ctry.c_str(), (unsigned long long)ts,
            // ★ V12 (R36S handheld "Align to FT8", 2026-11-05): WSJT-X's DT, additive — a client that does not read it ignores it.
            std::isfinite(sp.dt) ? (double)sp.dt : 0.0);
        if (n > 0) broadcastText(std::string(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1), GapSpots);
    }
    /** ★ V12 beta90: one spot into the station store (Env::stations — null = nowhere). The store validates the call and
     *  the grid itself and costs a map insert; its PSK Reporter listeners run here, on the decode thread, queued by them. */
    void toStations_(const std::string& mode, const std::string& call, const std::string& grid, int snr, double rfHz, int64_t utc) {
        if (!env_.stations) return;
        vibestations::Reception r;
        r.call = call; r.grid = grid; r.mode = mode; r.band = bandFor(rfHz); r.snr = snr; r.rfHz = rfHz; r.utc = utc;
        r.origin = env_.stationOrigin;
        env_.stations->spot(r);
    }
    /** ★ Band's mode chips as JSON (spotsMtx_ held). */
    std::string statusJsonLocked_() {
        std::string j = "{\"type\":\"digital_spots_status\",\"data\":{\"scope\":\"band\",\"modes\":[";
        bool first = true;
        std::vector<digi::Spotter::ModeStatus> sts = spotter_->status();
        if (iqSpotter_) for (auto& m : iqSpotter_->status()) sts.push_back(m);   // ★ beta91: the CW / RTTY / PSKW chips
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
            const digi::Span sp = bd ? digi::spanOf(*bd, digi::allModes()) : digi::Span();   // ★ beta91: the IQ chips too
            if (sp.ok)
                for (const std::string& name : sp.allModes()) {
                    bool have = false;
                    for (const auto& m : sts) if (m.mode == name) have = true;
                    if (have) continue;
                    char b[160];
                    // ★ V12 beta91: switched off by the owner (its slice is then not cut at all) says "off", not "out"
                    const bool own = (env_.bandOverride && env_.bandOverride(name) < 0) || (env_.spotModeOff && env_.spotModeOff(name));
                    std::snprintf(b, sizeof b, "%s{\"mode\":\"%s\",\"state\":\"%s\",\"dials\":0,\"idle\":0,\"shed\":0,\"live\":0}", first ? "" : ",", name.c_str(),
                                  own ? "off" : "out");
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
        const uint64_t seq = spotter_->statusSeq() + (iqSpotter_ ? iqSpotter_->statusSeq() * 1000003ull : 0);
        if (seq == statusSent_ ? utc - statusAt_ < 15.0 : utc - statusAt_ < 1.0) return;
        statusSent_ = seq; statusAt_ = utc;
        broadcastText(statusJsonLocked_(), -1);   // ★ re-sent every 15 s — not kept for a returning listener
    }
    /** ★★★ beta92 THE BAND SKIMMER'S FRAME (Stuart, 2026-11-23, the Lenovo RSP1A): the band box shows the CW and RTTY
     *  signals the band decode follows — a CW row with its frequency and "waiting for callsign" before a call is read, and
     *  under EXPAND its decoded text "like we do in the cw skimmer, same for rtty"; the waterfall marks them, and the VFO
     *  is drawn to the windows' width. One frame, ADDITIVE (a client that does not know it ignores it; a server without it
     *  leaves those clients exactly as before):
     *    {"type":"band_skim","data":{"windows":[{"mode","lo","hi","state"}],
     *                                "signals":[{"mode","id","hz","snr","wpm","wpmLo","wpmHi","q","active","set","last",
     *                                            "call","country","settings","text"}]}}
     *  ★ BOUNDED: ≤ kSkimRowsMax rows per window and ≤ 80 in all, text ≤ 120 characters, printable ASCII only (decoded
     *    text comes off the air — the RDS lesson). Built on the decode thread from each engine's own once-a-second
     *    snapshot (SkimRow), so no decoder state is read across threads. spotsMtx_ held. */
    std::string skimJsonLocked_() {
        std::vector<digi::Spotter::SkimWindow> wins;
        std::vector<digi::SkimRow> rows;
        if (iqSpotter_) iqSpotter_->skim(wins, rows);
        auto clean = [](const std::string& in, size_t cap) {
            std::string o;
            for (char ch : in) {
                unsigned char u = (unsigned char)ch;
                if (u == '\n' || u == '\r' || u == '\t') u = ' ';
                if (u >= 0x20 && u < 0x7f && u != '"' && u != '\\') o.push_back((char)u);
            }
            if (o.size() > cap) o.erase(0, o.size() - cap);
            return o;
        };
        std::string j = "{\"type\":\"band_skim\",\"data\":{\"windows\":[";
        char b[320];
        bool first = true;
        for (const auto& w : wins) {
            if (w.mode != "CW" && w.mode != "RTTY") continue;   // ★ the readable skimmers (PSKW's wide PSK spots as before)
            std::snprintf(b, sizeof b, "%s{\"mode\":\"%s\",\"lo\":%.0f,\"hi\":%.0f,\"state\":\"%s\"}", first ? "" : ",",
                          w.mode == "CW" ? "CW" : "RTTY", w.loHz, w.hiHz, digi::stateWord(w.state));
            j += b; first = false;
        }
        j += "],\"signals\":[";
        first = true;
        int n = 0;
        for (const auto& r : rows) {
            if (r.mode != "CW" && r.mode != "RTTY") continue;
            if (n >= 80) break;
            ++n;
            const bool cwRow = r.mode == "CW";
            const char* q = cwRow ? cw::qualityWord(r.quality) : rttysk::qualityWord(r.quality);
            std::snprintf(b, sizeof b,
                "%s{\"mode\":\"%s\",\"id\":%d,\"hz\":%.0f,\"snr\":%.0f,\"wpm\":%.0f,\"wpmLo\":%d,\"wpmHi\":%d,\"q\":\"%s\","
                "\"active\":%s,\"set\":%s,\"last\":%lld,",
                first ? "" : ",", cwRow ? "CW" : "RTTY", r.id, r.hz, std::isfinite(r.snr) ? (double)r.snr : 0.0,
                std::isfinite(r.wpm) ? (double)r.wpm : 0.0, r.wpmLo, r.wpmHi, q ? q : "poor",
                r.active ? "true" : "false", r.set ? "true" : "false", (long long)r.lastMs);
            j += b; first = false;
            j += "\"call\":\"" + clean(r.call, 16) + "\",\"country\":\""
               + clean(r.call.empty() ? std::string() : callsign::country(r.call), 40)
               + "\",\"settings\":\"" + clean(r.settings, 40) + "\",\"text\":\"" + clean(r.text, digi::kSkimTextMax) + "\"}";
        }
        return j + "]}}";
    }
    /** ★ Send it when it changed, at most once a second, and every 15 s regardless (a box opened late). spotsMtx_ held. */
    void maybeSendSkimLocked_(double utc) {
        if (!iqSpotter_) return;
        if (utc - skimAt_ < 1.0 && utc >= skimAt_) return;
        std::string j = skimJsonLocked_();
        if (j == skimSent_ && utc - skimAt_ < 15.0) return;
        skimSent_.swap(j); skimAt_ = utc;
        broadcastText(skimSent_, -1);
    }
    /** Stop the run (spotsMtx_ held). `finishTap`: a real stop, not a scope change — the gallery's summary is written. */
    void stopSpotterLocked_(bool finishTap) {
        spotsOn_ = false;
        spotsActive_.store(false, std::memory_order_relaxed);
        if (bandIq_.exchange(false)) skimmersRunning().fetch_sub(1);
        if (spotter_) { spotSkipBase_ += spotter_->slotsSkipped(); spotShedBase_ += spotter_->periodsShed(); }
        iqSpotter_.reset();          // ★ beta91: the CW / RTTY / PSKW engines first (they read the audio spotter's skips)
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
        // ★★★ beta92: a BACKGROUND VFO's host (BgRunner keys them "bg:<n>") decodes for nobody here — the back seat,
        //     below every listener's decoder (vibe_thread.h vibeBackgroundThread). A listener's host keeps the decoders' rung.
        if (session_.rfind("bg:", 0) == 0) vibeBackgroundThread("vibe-bgdec");
        else vibeDecoderThread("vibe-decode");
        std::unique_lock<std::mutex> lk(qM_);
        for (;;) {
            qCv_.wait(lk, [this] { return !q_.empty() || !qi_.empty() || !qc_.empty() || qStop_; });
            if (qStop_) return;
            if (!qc_.empty()) {   // ★ PAGING's channel IQ — small, so before the skimmer's and the audio
                IqBlock b = std::move(qc_.front());
                qc_.pop_front();
                qcSec_ = std::max(0.0, qcSec_ - (double)b.v.size() / b.rate);
                lk.unlock();
                // ★ VIBE_DEC_AUDIO_DUMP only — ★ beta92: while the decoder tap runs, ITS blocks (one rate in the file)
                { const int64_t w = wideAt_.load(std::memory_order_relaxed);
                  if (b.wide || !(w > 0 && nowMs_() - w < 2000))
                      if (std::shared_ptr<vibedec::DecDump> d = dumpNow_()) d->iq(b.v.data(), (int)b.v.size(), b.rate); }
                const uint64_t c0 = vibeThreadCpuNs();
                {
                    std::lock_guard<std::mutex> dl(decMtx_);
#if defined(VIBE_PAGING)
                    if (paging_ && !b.wide) paging_->feedIq(b.v.data(), (int)b.v.size(), b.rate);
#endif
                    // ★ V12 packet (1200 + 9600): ★★ beta92 the DECODER TAP (b.wide), or — only where no tap has come for
                    //   two seconds — the listener's channel
                    if (aprs_ && aprs_->wantsIq()) {
                        const int64_t w = wideAt_.load(std::memory_order_relaxed);
                        const bool tapLive = w > 0 && nowMs_() - w < 2000;
                        if (b.wide || !tapLive) aprs_->feedIq(b.v.data(), (int)b.v.size(), b.rate);
                    }
                }
                if (!b.wide) {   // ★ V12 VOR / ILS and 406 MHz beacons: their lines and state are sent from inside
                    std::lock_guard<std::mutex> dl(decMtx_);
                    if (nav_) nav_->feedIq(b.v.data(), (int)b.v.size(), b.rate);
                    if (sarsat_) sarsat_->feedIq(b.v.data(), (int)b.v.size(), b.rate);
                }
                cpuNs_.fetch_add(vibeThreadCpuNs() - c0, std::memory_order_relaxed);
                lk.lock();
                continue;
            }
            if (!qi_.empty()) {
                IqBlock b = std::move(qi_.front());
                qi_.pop_front();
                qiFrames_ -= b.v.size();
                if (b.slice == 0) qiMainSec_ = std::max(0.0, qiMainSec_ - (double)(b.v.size() + (size_t)b.gapN) / b.rate);
                lk.unlock();
                const uint64_t c0 = vibeThreadCpuNs();
                {
                    std::lock_guard<std::mutex> dl(decMtx_);
                    if (b.gapN == 0 && b.slice == 0 && cw_ && cw_->skimmer()) cw_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre, b.aim);
                    if (b.gapN == 0 && b.slice == 0 && psk_ && psk_->skimmer()) psk_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                    if (b.gapN == 0 && b.slice == 0 && rttySkim_) rttySkim_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                    if (b.gapN == 0 && b.slice == 0 && lrpt_) lrpt_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                    if (b.gapN == 0 && b.slice == 0 && orbcomm_) orbcomm_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                    if (b.gapN == 0 && b.slice == 0 && xpdr_) xpdr_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre);
                }
                {
                    std::lock_guard<std::mutex> sl(spotsMtx_);
                    if (spotter_ && spotter_->scope() == SpotScope::Band) {
                        if (b.gapN > 0) { spotter_->feedGap(b.gapN, b.rate, b.centre, b.utc); }
                        else {
                        spotter_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre, b.utc,
                                         b.pw.empty() ? nullptr : b.pw.data(), (int)b.pw.size(), b.binHz, b.slice);
                        /* ★ beta91: the CW / RTTY / PSKW spotter gets NO bins — no activity gate of its own (a 40–100 kHz CW or
                         *  RTTY window is never empty on an open band, and its skimmer is the detector), so the decode
                         *  thread does not measure every slice a second time; the governor and the pause rule still apply. */
                        if (iqSpotter_) iqSpotter_->feedIq(b.v.data(), (int)b.v.size(), b.rate, b.centre, b.utc, nullptr, 0, b.binHz, b.slice);
                        maybeSendStatusLocked_(b.utc);
                        maybeSendSkimLocked_(b.utc);   // ★ beta92 the band skimmer's readable rows, windows and marks
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
            decUtc_ = utc;   // ★ V12 the block's capture UTC, for a decoder on a UTC schedule (NCDXF)
            if (std::shared_ptr<vibedec::DecDump> d = dumpNow_()) dumpAudio_(*d, v.data(), (int)v.size());   // ★ VIBE_DEC_AUDIO_DUMP only
            decode_(v.data(), (int)v.size());
            spots_(v.data(), (int)v.size(), utc);
            cpuNs_.fetch_add(vibeThreadCpuNs() - c0, std::memory_order_relaxed);
            lk.lock();
        }
    }
    /** ★ The running capture, or null (one relaxed check when VIBE_DEC_AUDIO_DUMP is unset: the static string). */
    std::shared_ptr<vibedec::DecDump> dumpNow_() {
        if (!vibedec::DecDump::enabled()) return nullptr;
        std::lock_guard<std::mutex> dl(dumpMtx_);
        return dump_;
    }
    /** The audio block into the capture, and once a second its line: the queue's figures and the server's state words.
     *  Decode thread, no host lock held (Env::dumpState may take the shim's client lock). */
    void dumpAudio_(vibedec::DecDump& d, const float* x, int n) {
        d.audio(x, n);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (t - dumpTickAt_ < 1.0) return;
        dumpTickAt_ = t;
        double qs = 0; { std::lock_guard<std::mutex> ql(qM_); qs = (double)qFrames_ / 48000.0; }
        d.tick(t, qs, dropped_.load(std::memory_order_relaxed), env_.dumpState ? env_.dumpState() : std::string());
    }
    void stopQueue_() {
        replayStop_.store(true);
        { std::lock_guard<std::mutex> tl(replayThrMtx_);
          for (auto& j : replayThreads_) if (j->th.joinable()) j->th.join();
          replayThreads_.clear(); }
        { std::lock_guard<std::mutex> lk(qM_); qStop_ = true; q_.clear(); qUtc_.clear(); qFrames_ = 0; qi_.clear(); qiFrames_ = 0; qc_.clear(); qcSec_ = 0; }
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
            if (tones_) {
                // ★ tones: float audio straight in; lines and state are sent from inside (TonesRun)
                tones_->feedAudio(data, count);
                return;
            }
            if (balloon_) {
                // ★ balloons: float audio straight in; reports, pictures and state are sent from inside (BalloonRun)
                balloon_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (ncdxf_) {
                // ★ NCDXF: float audio + the block's capture UTC (the slot is cut by when it was heard, as FT8's)
                ncdxf_->feedAudio(data, count, decUtc_);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (jtty_) {
                // ★ JTTY: float audio straight in; its lines, lists, spots and state are sent from inside (JttyRun).
                jtty_->feedAudio(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (ale_) {
                ale_->feed(data, count);   // ★ ALE: its lines, state and status are sent from inside (buildAleLocked_)
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (tm_) {
                // ★ MFSK / OLIVIA / THOR / DOMINOEX / MT63: float audio straight in; text, state and status sent from inside
                tm_->feed(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (hell_) {
                hell_->feed(data, count);   // ★ FELD HELL: its lines and status are sent from inside
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (selcal_) {
                selcal_->feed(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (same_) {
                // ★ EAS / SAME: float audio straight in; its lines and state are sent from inside (buildSameLocked_)
                same_->feed(data, count);
                if (DecoderTapPtr t = tap()) t->tick();
                return;
            }
            if (hfdl_) {
                // ★ HFDL: float audio straight in; its lines and status are sent from inside (HfdlRun).
                hfdl_->feedAudio(data, count);
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
            if (fctlm_) {
                fctlm_->feedAudio(data, count);   // ★ FUNcube telemetry: its frames and status are sent from inside
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
            if (wefax_) {
                wefax_->process(mono.data(), count);
                if (!env_.wefaxHourly.empty()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (wefaxHourAt_ == std::chrono::steady_clock::time_point{}) { wefaxHourAt_ = now; wefaxSavedBase_ = 0; }
                    else if (std::chrono::duration<double>(now - wefaxHourAt_).count() >= env_.wefaxHourSecs) {
                        DecoderTapPtr t = tap();
                        wefaxHourLocked_(false, t ? t->savedCount() : 0);
                    }
                }
                return;
            }
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
    std::chrono::steady_clock::time_point wefaxHourAt_{};   // ★ the hourly WEFAX line: this hour began (decMtx_)
    size_t wefaxSavedBase_ = 0;                             // …and the tap's saved count then
    SstvDecoder*  sstv_  = nullptr;
    TimeDecoder*  time_  = nullptr;
    std::unique_ptr<CwRun> cw_;               // ★ CW: a listener's decoder or a skimmer (buildCwLocked_)
    std::unique_ptr<vibeaprs::Run> aprs_;     // ★ APRS (V12): the modem + the station list (vibe_aprs_run.h)
    std::unique_ptr<vibefc::Run> fctlm_;      // ★ V12 beta90 FUNcube BPSK 1200 telemetry (vibe_fctlm.h)
    std::unique_ptr<SitorARun> sitora_;       // ★ SITOR-A: the ARQ listener (buildSitoraLocked_)
    std::unique_ptr<HfdlRun> hfdl_;           // ★ HFDL: the aircraft datalink on the USB dial (buildHfdlLocked_)
    std::unique_ptr<SameDecoder> same_;       // ★ EAS / SAME (buildSameLocked_)
    std::unique_ptr<SelcalDecoder> selcal_;   // ★ SELCAL (buildSelcalLocked_)
    std::unique_ptr<TextModeDecoder> tm_;     // ★ MFSK / OLIVIA / THOR (buildTextModeLocked_)
    std::unique_ptr<FeldHellDecoder> hell_;   // ★ FELD HELL (buildHellLocked_)
    TextModeDecoder::Family tmFam_ = TextModeDecoder::Family::Mfsk;
#if defined(VIBE_PAGING)
    std::unique_ptr<PagingRun> paging_;       // ★ PAGING: fed the channel IQ (feedChannelIq) — Android local hardware only
#endif
    std::atomic<bool> packetIq_{false};       // ★ V12 9600 / AUTO packet runs: the shim hands this host its channel IQ too
    std::atomic<int64_t> wideAt_{0};           // ★ beta92 when the last decoder-tap block came (nowMs_; 0 = never)
    static int64_t nowMs_() { return 1 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::atomic<bool> pagingIq_{false};       // ★ paging runs: the shim hands this host its channel IQ (wantsChannelIq)
    std::unique_ptr<DscRun> dsc_;             // ★ DSC: MF/HF or VHF channel 70 (buildDscLocked_)
    std::unique_ptr<NcdxfRun> ncdxf_;         // ★ V12 NCDXF beacons (buildNcdxfLocked_)
    std::unique_ptr<BalloonRun> balloon_;     // ★ V12 balloons (buildBalloonLocked_)
    std::unique_ptr<TonesRun> tones_;
    std::unique_ptr<AleDecoder> ale_;         // ★ V12 2G HF ALE (buildAleLocked_)
    std::unique_ptr<NavRun> nav_;             // ★ V12 VOR / ILS (buildNavIqLocked_) — fed the channel IQ
    std::unique_ptr<SarsatRun> sarsat_;       // ★ V12 406 MHz beacons (buildNavIqLocked_) — fed the channel IQ
    std::atomic<bool> navIq_{false};          // ★ one of them runs: the shim hands this host its channel IQ
    double decUtc_ = NAN;                     ///< ★ the capture UTC of the block decode_ is handed (NaN = now)
    std::unique_ptr<JttyRun> jtty_;           // ★ JTTY: every JTTY signal in the passband (buildJttyLocked_)
    std::atomic<bool>   cwIq_{false};         // ★ the skimmer wants IQ (wantsIq)
    std::atomic<double> cwWidth_{0.0};
    std::atomic<double> iqFixed_{0.0};        // ★★ ORBCOMM's fixed slice centre (buildOrbcommLocked_) — 0 = none
    std::atomic<double> iqAim_{0.0};          // ★★ beta92 the satellite lock's corrected frequency (setIqAim) — 0 = none
    DecoderTapPtr cwTap_;                    // ★ the gallery's CW (or PSK) summary (Env::cwSpotTap / pskSpotTap), under tapMtx_
    std::unique_ptr<PskRun> psk_;             // ★ PSK31/63: a listener's decoder or a skimmer (buildPskLocked_) — shares cwIq_/cwWidth_
    std::unique_ptr<RttySkimRun> rttySkim_;   // ★ the RTTY skimmer (buildRttySkimLocked_) — shares cwIq_/cwWidth_, owns its workers
    std::unique_ptr<vibexpdr::Run> xpdr_;      // ★★★ beta94 the transponder finder (buildXpdrLocked_) — shares cwIq_/cwWidth_/iqFixed_
    std::unique_ptr<vibeorb::Run> orbcomm_;    // ★ V12 ORBCOMM (buildOrbcommLocked_) — shares cwIq_/cwWidth_
    std::unique_ptr<vibelrpt::Run> lrpt_;      // ★ V12 METEOR-M LRPT (buildLrptLocked_) — shares cwIq_/cwWidth_
    int   sstvDecim_ = 0; float sstvAcc_ = 0.0f;
    int   textSlot_ = -1;
    std::atomic<bool> active_{false};
    std::atomic<int>  imageKind_{0};          // 0 none · 1 WEFAX · 2 SSTV — what record() keeps
    std::atomic<double> pictureSince_{0.0};   // ★ steadySecs() the picture in progress began; 0 = none (midPicture)
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
    std::unique_ptr<digi::Spotter> iqSpotter_; // ★ V12 beta91: Band's CW / RTTY / wide-PSK chips, a spotter of their own
    bool  spotsOn_ = false;
    std::atomic<bool> spotsActive_{false};      // ★ Single: wants the listener's audio
    std::atomic<bool> bandIq_{false};           // ★ Band: wants the band's IQ slice (wantsIq)
    std::atomic<double> bandWidth_{24000.0}, bandWidthDial_{-1.0};   // ★ the slice width (set at start and by the decode thread)
    std::atomic<int> nExtras_{0};                // ★ Band's extra slices (iqExtraSlices) — set with bandWidth_
    std::atomic<double> exC_[kMaxExtraSlices], exW_[kMaxExtraSlices];
    void setExtras_(double dialHz) {
        // ★ V12 beta91: an extra slice whose every mode the owner switched off for Band (or off altogether) is not cut
        auto ov = env_.bandOverride; auto off = env_.spotModeOff;
        const std::vector<ExtraSlice> e = bandExtraSlices(dialHz, [ov, off](const std::string& m) {
            return (ov && ov(m) < 0) || (off && off(m));
        });
        /* ★★ V12 beta91 — NEVER REWRITTEN WHEN NOTHING CHANGED. This runs on the decode thread whenever the main slice's
         *  centre moves — and the DSP thread reads the extras every block (iqExtraSlices) to aim the slices. Rewriting them
         *  published a moment of NO extras (nExtras_ 0 while the values were refilled); a one-pipeline radio's tap then
         *  re-planned its front end without them (another decimation, another centre), the main slice's centre moved, this
         *  ran again — a loop that remade the slices and restarted the activity gate's floor every few blocks. It needs an
         *  extra slice to show, so it began with the CW / RTTY / wide-PSK slice on 20 m (test-server-spots: FT2 and PSK31
         *  missed their first loops, 3 of 7 runs). Now: unchanged extras are left alone; changed ones are written values
         *  first, count last (a reader sees the old set or the new, never none). */
        const int n = (int)std::min<size_t>(e.size(), kMaxExtraSlices);
        bool same = n == nExtras_.load(std::memory_order_acquire);
        for (int k = 0; same && k < n; ++k)
            same = exC_[k].load(std::memory_order_relaxed) == e[(size_t)k].centreHz && exW_[k].load(std::memory_order_relaxed) == e[(size_t)k].widthHz;
        if (same) return;
        if (n < nExtras_.load(std::memory_order_relaxed)) nExtras_.store(n, std::memory_order_release);   // ★ shrink first
        for (int k = 0; k < n; ++k) { exC_[k].store(e[(size_t)k].centreHz, std::memory_order_relaxed); exW_[k].store(e[(size_t)k].widthHz, std::memory_order_relaxed); }
        nExtras_.store(n, std::memory_order_release);
    }
    uint64_t spotSkipBase_ = 0;
    uint64_t spotShedBase_ = 0;                  // ★ Band periods shed by runs already stopped (spotPeriodsShed is monotonic)
    uint64_t statusSent_ = ~0ull;                // ★ the spotter's statusSeq last sent (spotsMtx_)
    std::string skimSent_;                       // ★ beta92 the band_skim frame last sent (spotsMtx_)
    double   skimAt_ = 0;                        //   and when (capture UTC)
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
                     int gapN = 0; double aim = 0; bool wide = false; };   // ★ beta92 wide: the decoder tap (feedChannelIq)   // ★ aim: setIqAim's frequency when the block was cut (0 = none)   // ★ gapN > 0: no samples, a gap of that many to bridge
    /* ★★ BAND'S MINUTE LINE (V12 2026-10-27, for the Pi 2's "every chip light, nothing decoded"): what the decode thread was
     *  fed (seconds of each slice per minute of capture — 60 means it kept up), how far behind the capture it ran (now −
     *  the block's capture UTC: FT8 cuts its slots by that stamp), what was dropped, slots decoded / skipped, spots per mode
     *  and the chips. One line a minute, decode thread, spotsMtx_ held. */
    std::mutex diagMtx_;
    std::map<std::string, int> diagSpots_;
    std::map<std::string, uint64_t> diagCpu_;   // ★ V12 beta91: each IQ chip's engine CPU at the last minute line
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
        std::vector<digi::Spotter::ModeStatus> stAll = spotter_->status();
        if (iqSpotter_) for (auto& m : iqSpotter_->status()) stAll.push_back(m);
        for (const auto& m : stAll) {
            chips += " " + m.mode + ":" + digi::stateWord(m.state);
            // ★ V12 beta91: the CW / RTTY chips' own threads, % of one core over the minute (what they cost on this box)
            if (m.iq) {
                const uint64_t was = diagCpu_[m.mode]; diagCpu_[m.mode] = m.cpuNs;
                const double span = b.utc - diagAt_;
                if (span > 0 && m.cpuNs >= was) { char c[32]; std::snprintf(c, sizeof c, "(%.1f%%)", 100.0 * (double)(m.cpuNs - was) / 1e9 / span); chips += c; }
            }
        }
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
    std::deque<IqBlock> qc_;             // ★ PAGING's channel IQ (feedChannelIq), the same thread; bounded by qcSec_
    double qcSec_ = 0;
    size_t qiFrames_ = 0;
    size_t qFrames_ = 0;
    bool   qStop_ = false;
    std::thread qThread_;
    std::atomic<uint64_t> fed_{0}, dropped_{0};
    std::atomic<uint64_t> cpuNs_{0};
    // ★ the developer capture (vibe_dec_dump.h): its own leaf lock — taken under decMtx_ (start, clear) or alone (loop_)
    std::mutex dumpMtx_;
    std::shared_ptr<vibedec::DecDump> dump_;
    double dumpTickAt_ = 0;   // decode thread only
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
    /** ★★ PAGING (V12): a listener's channel IQ (per-VFO). One relaxed load when nothing pages anywhere. */
    void feedChannelIqSession(const std::string& session, const std::complex<float>* iq, int n, double rate) {
        if ((DecoderHost::pagingRunning().load(std::memory_order_relaxed) <= 0 && DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0)
            || !anyPer_.load(std::memory_order_relaxed)) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it != bySession_.end()) h = it->second; }
        if (h) h->feedChannelIq(iq, n, rate);
    }
    /** ★★ …and the one pipeline's (a shared dial, or one listener). */
    void feedChannelIqShared(const std::complex<float>* iq, int n, double rate) {
        if (DecoderHost::pagingRunning().load(std::memory_order_relaxed) <= 0 && DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_); h = shared_; }
        if (h) h->feedChannelIq(iq, n, rate);
    }
    /** ★★ V12 VOR / ILS, 406 MHz beacons: a listener's IQ (their slice, per-VFO) — the same queue as paging's. */
    void feedNavIqSession(const std::string& session, const std::complex<float>* iq, int n, double rate) {
        if (DecoderHost::navIqRunning().load(std::memory_order_relaxed) <= 0 || !anyPer_.load(std::memory_order_relaxed)) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it != bySession_.end()) h = it->second; }
        if (h) h->feedChannelIq(iq, n, rate);
    }
    /** ★★ …and the one pipeline's channel. */
    void feedNavIqShared(const std::complex<float>* iq, int n, double rate) {
        if (DecoderHost::navIqRunning().load(std::memory_order_relaxed) <= 0) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_); h = shared_; }
        if (h) h->feedChannelIq(iq, n, rate);
    }
    /** ★★ beta92 THE DECODER TAP (RxPipeline::setDecoderTap → Callbacks::decIq): a listener's, and the one pipeline's.
     *  One relaxed load when no packet decoder reads IQ anywhere in this process. */
    void feedDecTapSession(const std::string& session, const std::complex<float>* iq, int n, double rate) {
        if (DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0 || !anyPer_.load(std::memory_order_relaxed)) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it != bySession_.end()) h = it->second; }
        if (h) h->feedChannelIq(iq, n, rate, true);
    }
    void feedDecTapShared(const std::complex<float>* iq, int n, double rate) {
        if (DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_); h = shared_; }
        if (h) h->feedChannelIq(iq, n, rate, true);
    }
    /** ★★ beta92: does this listener's own host (or the one pipeline's) run packet on IQ — so its pipeline wants the tap? */
    bool sessionWantsPacketTap(const std::string& session) {
        if (DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0) return false;
        std::lock_guard<std::mutex> lk(m_);
        auto it = bySession_.find(session);
        return it != bySession_.end() && it->second && it->second->wantsPacketTap();
    }
    bool sharedWantsPacketTap() {
        if (DecoderHost::packetIqRunning().load(std::memory_order_relaxed) <= 0) return false;
        std::lock_guard<std::mutex> lk(m_);
        return shared_ && shared_->wantsPacketTap();
    }
    /** ★ Does the one pipeline's host run VOR (which needs its channel 25 kHz wide)? */
    bool sharedWantsWide() {
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_); h = shared_; }
        return h && h->wantsWideChannel();
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
        return shared_ && (shared_->wantsAudio() || shared_->wantsChannelIq());   // ★ paging runs on the channel IQ
    }
    /** ★ …and is it a CW skimmer, wanting IQ (V12)? The same leaf lock. */
    bool sharedWantsIq() const {
        std::lock_guard<std::mutex> lk(m_);
        return shared_ && shared_->wantsIq();
    }
    /** Is anything decoding anywhere on this radio? */
    bool anyRunning() {
        for (auto& kv : all()) if (kv.second->wantsAudio() || kv.second->wantsIq() || kv.second->wantsChannelIq()) return true;
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
