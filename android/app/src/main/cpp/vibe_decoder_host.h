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
#include "decoders/wefax_decoder.h"
#include "decoders/sstv_decoder.h"
#include "decoders/time_decoder.h"
#include "decoders/ft8_decoder.h"
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

class DecoderHost {
public:
    struct Env {
        /** The dial this host's audio is tuned to (Hz) — a spot's RF frequency is dial + audio offset. */
        std::function<double()> dialHz;
        std::function<void(const std::string&)> log;
        /** Box-wide slots; null = unlimited (a decoder-only sidecar serving one app). */
        DecoderSlots* slots = nullptr;
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
    Start start(const std::string& ext, const std::string& msg, const DecoderPeerPtr& joiner = nullptr) {
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
                return Start::Ok;
            }
        }
        if (joiner) replayTo(joiner);
        return Start::Joined;
    }
    /** Stop the audio-extension decoder and give its slot back. */
    void stop() {
        std::lock_guard<std::mutex> lk(decMtx_);
        clearLocked_();
        startKey_.clear(); name_.clear();
        if (textSlot_ >= 0) { if (env_.slots) env_.slots->release(textSlot_); textSlot_ = -1; }
        { std::lock_guard<std::mutex> bl(textMtx_); textBuf_.clear(); morse_.clear(); }
    }
    /** FT8 + FT4 spots — a second, independent slot. Idempotent. */
    Start startSpots() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (spotsOn_) return Start::Joined;
        if (spotsSlot_ < 0) {
            spotsSlot_ = env_.slots ? env_.slots->claim() : 0;
            if (spotsSlot_ < 0) return Start::Refused;
        }
        delete ft8_; delete ft4_;
        ft8_ = new Ft8Decoder(12000, false);
        ft4_ = new Ft8Decoder(12000, true);
        ft8_->onSpot = [this](const std::string& to, const std::string& de, const std::string& g, int s, float f) { emitSpot(false, to, de, g, s, f); };
        ft4_->onSpot = [this](const std::string& to, const std::string& de, const std::string& g, int s, float f) { emitSpot(true,  to, de, g, s, f); };
        spotDecim_ = 0; spotAcc_ = 0.0f;
        spotsOn_ = true;
        spotsActive_.store(true, std::memory_order_relaxed);
        log("digital spots (FT8/FT4) started");
        return Start::Ok;
    }
    void stopSpots() {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        spotsOn_ = false;
        spotsActive_.store(false, std::memory_order_relaxed);
        delete ft8_; ft8_ = nullptr;
        delete ft4_; ft4_ = nullptr;
        if (spotsSlot_ >= 0) { if (env_.slots) env_.slots->release(spotsSlot_); spotsSlot_ = -1; }
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
    /** Hand the decoders `count` frames of 48 kHz audio, `stride` floats apart (1 = mono, 2 = take the
     *  left of interleaved stereo). Copies and returns at once — NEVER decodes on the caller's thread. */
    void feed(const float* pcm, int count, int stride = 1) {
        if (count <= 0 || !wantsAudio()) return;
        std::vector<float> v((size_t)count);
        for (int i = 0; i < count; ++i) v[(size_t)i] = pcm[(size_t)i * (size_t)stride];
        fed_.fetch_add((uint64_t)count, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(qM_);
            if (!qThread_.joinable()) { qStop_ = false; replayStop_.store(false); qThread_ = std::thread([this] { loop_(); }); }
            qFrames_ += v.size();
            q_.push_back(std::move(v));
            // ★ DROP, NEVER WAIT — see the file note. The oldest goes: a decoder resumes on fresh
            //   audio, and 4 s is a whole WEFAX line and more than an FT8 symbol run.
            while (qFrames_ > kMaxFrames && q_.size() > 1) {
                qFrames_ -= q_.front().size();
                dropped_.fetch_add(q_.front().size(), std::memory_order_relaxed);
                q_.pop_front();
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
    /** Frames waiting for the decode thread — a feeder that must NOT lose audio (a test, the
     *  benchmark) paces itself on this; the server never does, it drops. */
    size_t queued() { std::lock_guard<std::mutex> lk(qM_); return qFrames_; }
    /** "fsk"/"wefax"/"sstv"/"time"/"none" — WHICH decoder object actually exists (see the admin note
     *  on decoderKind: "attached" was once true while nothing had been constructed). */
    std::string kind() {
        std::lock_guard<std::mutex> lk(decMtx_);
        return wefax_ ? "wefax" : sstv_ ? "sstv" : fsk_ ? "fsk" : time_ ? "time" : "none";
    }
    /** RTTY health for the admin page, or false when no FSK decoder runs. */
    bool fskHealth(unsigned long& resyncs, double& level, double& threshold, int& state) {
        std::lock_guard<std::mutex> lk(decMtx_);
        if (!fsk_) return false;
        resyncs = fsk_->resyncs(); level = fsk_->audioLevel(); threshold = fsk_->audioThreshold(); state = fsk_->stateNow();
        return true;
    }

    /** Send every attached socket a frame. */
    void broadcast(const uint8_t* d, size_t n) {
        for (auto& p : snapshotPeers()) p->binary(d, n);
        record(d, n);
    }
    void broadcastText(const std::string& s) { for (auto& p : snapshotPeers()) p->text(s); }

    /** ★★★ CATCH A LATE ARRIVAL UP ON THE PICTURE SO FAR — PACED. A full WEFAX chart is ~2 MB of
     *  lines, four times a listener's whole outbox; sending it in one go dropped the joiner for
     *  backlog (1006) before its own attach was read, and the client's 3-s reconnect then did it
     *  again, for ever (Pi 500, 2026-09-30: 1042 frames replayed, closed, repeated). So it goes at
     *  the pace the socket drains, on a thread of its own, and gives up if the socket closes. */
    void replayTo(const DecoderPeerPtr& p) {
        std::vector<std::vector<uint8_t>> frames;
        { std::lock_guard<std::mutex> rl(replayMtx_); frames = replay_; }
        if (frames.empty() || !p || !p->open()) return;
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
    static constexpr size_t kMaxFrames = 48000 * 4;          // 4 s of audio behind = drop
    static constexpr size_t kReplayMax = 4u * 1024 * 1024;
    static constexpr size_t kReplayBacklog = 128u * 1024;    // pace a replay below this backlog

    static bool knownExt(const std::string& e) {
        return e == "fsk" || e == "navtex" || e == "wefax" || e == "sstv" || e == "time"
            || e == "msf" || e == "dcf77" || e == "rwm" || e == "wwv" || e == "wwvb";
    }
    /** ★ "Same decoder, same settings": the parameters a decoder is built from, in a fixed order.
     *  A whitelist rather than "the message minus its envelope", so field ORDER, a session id or a
     *  future harmless field cannot make an identical request look different and restart a chart. */
    static std::string paramsOf(const std::string& msg) {
        std::string k;
        for (const char* f : { "center_frequency", "shift", "baud_rate", "lpm", "image_width", "carrier",
                               "deviation", "bandwidth" }) {
            double v; k += num(msg, f, v) ? std::to_string(v) : std::string("-"); k += ',';
        }
        for (const char* f : { "encoding", "framing", "station" }) { k += str(msg, f); k += ','; }
        for (const char* f : { "\"inverted\":true", "\"use_phasing\":false", "\"auto_stop\":true", "\"auto_start\":true" })
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

    bool running_() const { return fsk_ || wefax_ || sstv_ || time_; }
    void clearLocked_() {
        delete fsk_;   fsk_ = nullptr;
        delete wefax_; wefax_ = nullptr;
        delete sstv_;  sstv_ = nullptr;
        delete time_;  time_ = nullptr;
        active_.store(false, std::memory_order_relaxed);
        imageKind_.store(0, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> rl(replayMtx_); replay_.clear(); replayBytes_ = 0; }
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
        if (kind == 1 && n == 1 && d[0] == 0x02) { replay_.clear(); replayBytes_ = 0; }
        if (kind == 2 && n >= 9 && d[0] == 0x07) { replay_.clear(); replayBytes_ = 0; }
        if (replayBytes_ + n > kReplayMax) return;
        replay_.emplace_back(d, d + n);
        replayBytes_ += n;
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
            fsk_->onChar = [this](char32_t ch) {
                std::lock_guard<std::mutex> bl(textMtx_);
                if (ch < 0x80) textBuf_.push_back((char)ch);
                else if (ch < 0x800) { textBuf_.push_back((char)(0xC0 | (ch >> 6))); textBuf_.push_back((char)(0x80 | (ch & 0x3F))); }
            };
            fsk_->onState = [this](int st) { uint8_t m[2] = { 0x03, (uint8_t)st }; broadcast(m, 2); };
            char b[160];
            std::snprintf(b, sizeof b, "decoder attached: fsk cf=%.0f shift=%.0f baud=%.2f enc=%s", cf, sh, baud, enc.c_str());
            log(b);
        }
        active_.store(true, std::memory_order_relaxed);
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
        time_->onTime = [this, name](const TimeDecoder::TimeStamp& t) {
            static const char* kDay[8] = { "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s  %s %04d-%02d-%02d %02d:%02d  %s%s\n",
                          name.c_str(), kDay[t.weekday >= 1 && t.weekday <= 7 ? t.weekday : 0],
                          t.year, t.month, t.day, t.hour, t.minute,
                          t.dst ? "(summer time)" : "", t.leapSecondPending ? " LEAP SECOND PENDING" : "");
            std::lock_guard<std::mutex> bl(textMtx_);
            textBuf_ += buf;
        };
        // ★★★ FILL THE FIELDS AS THEY ARRIVE — a replace-in-place line (see the shim's old note).
        time_->onPartial = [this, name](const TimeDecoder::Partial& p) {
            { std::lock_guard<std::mutex> bl(textMtx_);
              if (morse_.size() >= 3) { textBuf_ += "RWM ID: " + morse_ + "\n"; morse_.clear(); } }
            char buf[200], yy[8], mo[4], dd[4], hh[4], mi[4];
            std::snprintf(yy, sizeof(yy), p.year  ? "%04d" : "----", p.t.year);
            std::snprintf(mo, sizeof(mo), p.month ? "%02d" : "--",   p.t.month);
            std::snprintf(dd, sizeof(dd), p.day   ? "%02d" : "--",   p.t.day);
            std::snprintf(hh, sizeof(hh), p.hour  ? "%02d" : "--",   p.t.hour);
            std::snprintf(mi, sizeof(mi), p.minute? "%02d" : "--",   p.t.minute);
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

    void emitSpot(bool isFt4, const std::string& callTo, const std::string& callDe,
                  const std::string& grid, int snr, float audioHz) {
        (void)callTo;
        const double rfHz = (env_.dialHz ? env_.dialHz() : 0.0) + audioHz;   // dial (USB) + audio offset
        const uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        /* ★★★ THESE TWO FIELDS COME OFF THE AIR, AND ANYONE MAY TRANSMIT ANYTHING — a whitelist, so
         *  neither can carry a quote, a backslash or a byte that is not UTF-8 (the RDS fault on a
         *  different decoder; Stuart, 2026-09-27: "sandbox everything"). And snprintf returns the
         *  length it WANTED: clamped below to what was written. */
        auto safe = [](const std::string& in, size_t cap) {
            std::string o;
            for (char ch : in) {
                if (o.size() >= cap) break;
                const unsigned char u = (unsigned char)ch;
                if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '/' || u == '-') o.push_back(ch);
            }
            return o;
        };
        const std::string c = safe(callDe, 32), g = safe(grid, 8);
        char buf[384];
        const int n = std::snprintf(buf, sizeof(buf),
            "{\"type\":\"digital_spot\",\"data\":{\"mode\":\"%s\",\"callsign\":\"%s\","
            "\"snr\":%d,\"frequency\":%.0f,\"band\":\"%s\",\"grid\":\"%s\",\"timestamp\":%llu}}",
            isFt4 ? "FT4" : "FT8", c.c_str(), snr, rfHz, bandFor(rfHz), g.c_str(), (unsigned long long)ts);
        if (n > 0) broadcastText(std::string(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1));
    }
public:
    static const char* bandFor(double hz) {
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
            qCv_.wait(lk, [this] { return !q_.empty() || qStop_; });
            if (qStop_) return;
            std::vector<float> v = std::move(q_.front());
            q_.pop_front();
            qFrames_ -= v.size();
            lk.unlock();
            decode_(v.data(), (int)v.size());
            spots_(v.data(), (int)v.size());
            lk.lock();
        }
    }
    void stopQueue_() {
        replayStop_.store(true);
        { std::lock_guard<std::mutex> tl(replayThrMtx_);
          for (auto& j : replayThreads_) if (j->th.joinable()) j->th.join();
          replayThreads_.clear(); }
        { std::lock_guard<std::mutex> lk(qM_); qStop_ = true; q_.clear(); qFrames_ = 0; }
        qCv_.notify_all();
        if (qThread_.joinable()) qThread_.join();
    }
    void decode_(const float* data, int count) {
        std::string text;
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
            { std::lock_guard<std::mutex> bl(textMtx_); text.swap(textBuf_); }
        }
        if (!text.empty()) textFrame(text);
    }
    void spots_(const float* data, int count) {
        std::lock_guard<std::mutex> lk(spotsMtx_);
        if (!spotsOn_) return;
        std::vector<int16_t> dec; dec.reserve((size_t)count / 4 + 1);
        for (int i = 0; i < count; i++) {
            spotAcc_ += data[i];
            if (++spotDecim_ >= 4) {
                const int s = (int)std::lround(spotAcc_ / 4.0f * 32767.0f);
                dec.push_back((int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s)));
                spotDecim_ = 0; spotAcc_ = 0.0f;
            }
        }
        if (dec.empty()) return;
        if (ft8_) ft8_->process(dec.data(), (int)dec.size());
        if (ft4_) ft4_->process(dec.data(), (int)dec.size());
    }

    Env env_;
    std::string session_;

    std::mutex peerMtx_;
    std::vector<DecoderPeerPtr> peers_;

    std::mutex decMtx_;
    std::string name_, startKey_;
    FskDecoder*   fsk_   = nullptr;
    WefaxDecoder* wefax_ = nullptr;
    SstvDecoder*  sstv_  = nullptr;
    TimeDecoder*  time_  = nullptr;
    int   sstvDecim_ = 0; float sstvAcc_ = 0.0f;
    int   textSlot_ = -1;
    std::atomic<bool> active_{false};
    std::atomic<int>  imageKind_{0};          // 0 none · 1 WEFAX · 2 SSTV — what record() keeps
    std::mutex textMtx_;
    std::string textBuf_, morse_;
    std::mutex sstvSendMtx_;

    std::mutex spotsMtx_;
    Ft8Decoder* ft8_ = nullptr;
    Ft8Decoder* ft4_ = nullptr;
    bool  spotsOn_ = false;
    std::atomic<bool> spotsActive_{false};
    int   spotDecim_ = 0; float spotAcc_ = 0.0f;
    int   spotsSlot_ = -1;

    std::mutex replayMtx_;
    std::vector<std::vector<uint8_t>> replay_;
    size_t replayBytes_ = 0;
    std::mutex replayThrMtx_;
    std::vector<std::unique_ptr<ReplayJob>> replayThreads_;
    std::atomic<bool> replayStop_{false};

    std::mutex qM_;
    std::condition_variable qCv_;
    std::deque<std::vector<float>> q_;
    size_t qFrames_ = 0;
    bool   qStop_ = false;
    std::thread qThread_;
    std::atomic<uint64_t> fed_{0}, dropped_{0};
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
    void feedSession(const std::string& session, const float* pcm, int count, int stride) {
        if (!anyPer_.load(std::memory_order_relaxed)) return;
        std::shared_ptr<DecoderHost> h;
        { std::lock_guard<std::mutex> lk(m_);
          auto it = bySession_.find(session);
          if (it != bySession_.end()) h = it->second; }
        if (h) h->feed(pcm, count, stride);
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
    /** Is anything decoding anywhere on this radio? */
    bool anyRunning() {
        for (auto& kv : all()) if (kv.second->wantsAudio()) return true;
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
    std::mutex m_;
    std::shared_ptr<DecoderHost> shared_;
    std::map<std::string, std::shared_ptr<DecoderHost>> bySession_;
    std::map<std::string, double> orphanSince_;
    std::atomic<bool> anyPer_{false};
};

} // namespace vibe
