// vibe_local_speaker.h — ★★★ V12 R36S: THE ENGINE PLAYS THE RECEIVER ITSELF (Linux, ALSA by its kernel interface).
//
// WHY (2026-11-06, Stuart's R36S, handheld6's log): the page's audio path — the decode Worker, the AudioWorklet and
// GStreamer's queues inside WPEWebProcess — cost ~1.1 of the four A35 cores, and the browser would not start sound
// without a button press. On the handheld the radio, the speaker and the engine are the same box, so the PCM the engine
// already makes goes straight to the codec: no WebSocket audio, no decode, no WebAudio, no gesture. The native UI that
// follows uses the same output.
//
// ★ Fed from the DSP thread (push): never blocks. A writer thread owns the device and blocks in WRITEI.
// ★ Stereo, S16_LE, 48 kHz — the engine's rate (AUDIO_SR); mono is doubled on the way in.
// ★ Jitter: ~60 ms target, at most ~250 ms queued (the oldest is dropped, so the speaker never lags the dial). An
//   underrun is filled with silence — a glitch, never a stall.
// ★ The device is opened on the first audio and closed after kIdleCloseMs with none (a stopped radio, a mode with no
//   audio): the codec and amplifier then go idle and draw nothing.
// ★ The card: VIBE_SPEAKER_CARD (an ALSA card number or its id, e.g. "rk817ext"), else card 0.
#pragma once

#if defined(__linux__) && !defined(__ANDROID__)
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

namespace vibe {

class LocalSpeaker {
public:
    static constexpr unsigned kRate = 48000;
    static constexpr unsigned kPeriod = 480;            // 10 ms
    static constexpr unsigned kPeriods = 8;             // 80 ms in the device
    static constexpr size_t   kTargetFrames = 2880;     // ~60 ms queued before playing (absorbs DSP block jitter)
    static constexpr size_t   kMaxFrames = 12000;       // ~250 ms: past this the oldest goes
    static constexpr int      kIdleCloseMs = 3000;

    using LogFn = void (*)(const char*);
    explicit LocalSpeaker(LogFn log = nullptr) : log_(log) {}
    ~LocalSpeaker() { stop(); }

    /** From the DSP thread. ch 1 or 2, interleaved. */
    void push(const int16_t* pcm, int frames, int ch) {
        if (frames <= 0 || (ch != 1 && ch != 2)) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (int i = 0; i < frames; i++) {
                const int16_t l = pcm[(size_t)i * ch], r = ch == 2 ? pcm[(size_t)i * 2 + 1] : l;
                q_.push_back(l); q_.push_back(r);
            }
            while (q_.size() > kMaxFrames * 2) { q_.pop_front(); q_.pop_front(); dropped_++; }
            lastPush_ = std::chrono::steady_clock::now();
        }
        if (!run_.load(std::memory_order_acquire)) start();
        cv_.notify_one();
    }

    bool playing() const { return fd_.load() >= 0; }
    std::string problem() const { std::lock_guard<std::mutex> lk(m_); return problem_; }

    void stop() {
        run_.store(false);
        cv_.notify_all();
        const int fd = fd_.load();
        if (fd >= 0) ioctl(fd, SNDRV_PCM_IOCTL_DROP);       // wakes a blocked write
        if (th_.joinable()) th_.join();
    }

private:
    LogFn log_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<int16_t> q_;
    std::chrono::steady_clock::time_point lastPush_{};
    std::string problem_;
    uint64_t dropped_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> fd_{-1};
    std::thread th_;
    std::mutex startMtx_;

    void say(const std::string& s) { if (log_) log_(s.c_str()); }

    void start() {
        std::lock_guard<std::mutex> lk(startMtx_);
        if (run_.load()) return;
        if (th_.joinable()) th_.join();
        run_.store(true, std::memory_order_release);
        th_ = std::thread([this] { loop(); });
    }

    static int cardNumber() {
        const char* e = std::getenv("VIBE_SPEAKER_CARD");
        if (!e || !*e) return 0;
        char* end = nullptr;
        const long n = std::strtol(e, &end, 10);
        if (end && *end == 0) return (int)n;
        for (int c = 0; c < 16; c++) {                       // an id: /proc/asound/cardN/id
            char p[64]; std::snprintf(p, sizeof p, "/proc/asound/card%d/id", c);
            FILE* f = std::fopen(p, "r");
            if (!f) continue;
            char id[64] = {0};
            const bool got = std::fgets(id, sizeof id, f) != nullptr;
            std::fclose(f);
            if (!got) continue;
            std::string s(id);
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (s == e) return c;
        }
        return 0;
    }

    static void hwInit(snd_pcm_hw_params& p) {
        std::memset(&p, 0, sizeof p);
        for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
            std::memset(p.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xFF, sizeof p.masks[0].bits);
        for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
            snd_interval& i = p.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
            i.min = 0; i.max = ~0u;
        }
        p.rmask = ~0u; p.cmask = 0; p.info = ~0u;
    }
    static void hwMask(snd_pcm_hw_params& p, int n, unsigned bit) {
        snd_mask& m = p.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        std::memset(m.bits, 0, sizeof m.bits);
        m.bits[bit >> 5] |= 1u << (bit & 31);
    }
    static void hwInt(snd_pcm_hw_params& p, int n, unsigned v) {
        snd_interval& i = p.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        i.min = v; i.max = v; i.integer = 1;
    }

    int openDevice(std::string& err) {
        const int card = cardNumber();
        char path[64]; std::snprintf(path, sizeof path, "/dev/snd/pcmC%dD0p", card);
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            err = std::string("could not open the speaker (") + path + "): " + std::strerror(errno)
                + (errno == EBUSY ? " — another program has the sound card"
                   : errno == EACCES ? " — the vibeserver user must be in the 'audio' group" : "");
            return -1;
        }
        snd_pcm_hw_params hp; hwInit(hp);
        hwMask(hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        hwMask(hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
        hwMask(hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        hwInt(hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
        hwInt(hp, SNDRV_PCM_HW_PARAM_RATE, kRate);
        hwInt(hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPeriod);
        hwInt(hp, SNDRV_PCM_HW_PARAM_PERIODS, kPeriods);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) != 0) {
            err = std::string("the speaker would not run at 48 kHz stereo 16-bit: ") + std::strerror(errno);
            ::close(fd); return -1;
        }
        snd_pcm_sw_params sp; std::memset(&sp, 0, sizeof sp);
        sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
        sp.period_step = 1;
        sp.avail_min = kPeriod;
        sp.start_threshold = kPeriod * 2;                 // starts once two periods are in
        sp.stop_threshold = kPeriod * kPeriods;           // an underrun stops it; the loop re-prepares
        sp.boundary = (snd_pcm_uframes_t)kPeriod * kPeriods;
        while (sp.boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - kPeriod * kPeriods) sp.boundary *= 2;
        if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) != 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) != 0) {
            err = std::string("the speaker refused its settings: ") + std::strerror(errno);
            ::close(fd); return -1;
        }
        return fd;
    }

    void loop() {
        std::vector<int16_t> buf((size_t)kPeriod * 2);
        bool primed = false;
        auto lastFail = std::chrono::steady_clock::time_point{};
        while (run_.load()) {
            // ── wait for audio (and close the device when it has stopped coming) ──
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(primed ? 5 : 50), [&] {
                    return !run_.load() || q_.size() >= (primed ? kPeriod * 2 : kTargetFrames * 2);
                });
                if (!run_.load()) break;
                const auto idle = std::chrono::steady_clock::now() - lastPush_;
                if (idle > std::chrono::milliseconds(kIdleCloseMs)) {
                    lk.unlock();
                    closeDevice("no audio for 3 s");
                    primed = false;
                    std::unique_lock<std::mutex> lk2(m_);
                    cv_.wait(lk2, [&] { return !run_.load() || !q_.empty(); });
                    continue;
                }
                if (!primed && q_.size() < kTargetFrames * 2) continue;
                primed = true;
                // one period: what is there, then silence (an underrun of the queue is a glitch, never a stall)
                const size_t have = std::min(q_.size(), buf.size());
                for (size_t i = 0; i < have; i++) { buf[i] = q_.front(); q_.pop_front(); }
                for (size_t i = have; i < buf.size(); i++) buf[i] = 0;
                if (have < buf.size()) primed = false;           // refill to the target before the next
            }
            // ── the device ──
            if (fd_.load() < 0) {
                if (std::chrono::steady_clock::now() - lastFail < std::chrono::seconds(2)) continue;
                std::string err;
                const int fd = openDevice(err);
                if (fd < 0) {
                    lastFail = std::chrono::steady_clock::now();
                    bool changed;
                    { std::lock_guard<std::mutex> lk(m_); changed = problem_ != err; problem_ = err; }
                    if (changed) say("speaker: " + err);
                    continue;
                }
                fd_.store(fd);
                { std::lock_guard<std::mutex> lk(m_); problem_.clear(); }
                say("speaker: playing (48 kHz stereo, the engine's own audio — no browser audio)");
            }
            snd_xferi x{}; x.buf = buf.data(); x.frames = kPeriod;
            if (ioctl(fd_.load(), SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) continue;
            if (!run_.load()) break;
            if (errno == EPIPE) { ioctl(fd_.load(), SNDRV_PCM_IOCTL_PREPARE); continue; }   // underrun: carry on
            if (errno == EINTR || errno == EAGAIN) continue;
            closeDevice(std::string("write failed: ") + std::strerror(errno));
            lastFail = std::chrono::steady_clock::now();
        }
        closeDevice("");
    }

    void closeDevice(const std::string& why) {
        const int fd = fd_.exchange(-1);
        if (fd < 0) return;
        ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
        ::close(fd);
        if (!why.empty()) say("speaker: closed (" + why + ")");
    }
};

}  // namespace vibe
#endif
