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
#include "vibedsp/vibedsp.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
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

/** The kind a decoder extension is filed under — "" = not kept (phase 1 keeps the text decoders). */
inline std::string galleryKindFor(const std::string& ext, const std::string& msg) {
    if (ext == "navtex") return "navtex";
    if (ext == "fsk") return msg.find("CCIR476") != std::string::npos ? "navtex" : "rtty";
    return "";
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
            const double need = std::max(2700.0 * 2.5, 24000.0);
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
            c->rx->setTune(resid, c->vfo.mode == "lsb" ? vibedsp::RxPipeline::Mode::SSB_LSB : vibedsp::RxPipeline::Mode::SSB_USB, 2700.0);
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
                    return std::make_shared<GalleryTap>(*st, m);
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
        const auto r = c.host->start(d->ext, attachMsg(c.vfo));
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
