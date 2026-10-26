// vibe_drm_listener.h — ONE LISTENER'S DRM: the receiver (decoders/drm/), the audio framer and the platform decoder
// (vibe_drm_audiodec.h) on their own thread, fed the listener's channel IQ and handing back 48 kHz stereo PCM and a
// status report. docs/v12/DRM.md.
//
// ★★★ A LISTENER MODE, NOT A WHOLE-RADIO MODE. DRM30 is ≤ 10 kHz wide and sits in an AM channel, so it is chosen like AM:
//     per listener on a range-locked radio (each listener's own VFO, their own receiver), and on a shared dial the one
//     receiver serves everybody (Stuart's shared-dial contract — the server sends full state, nobody votes). Nothing here
//     retunes anything.
// ★★ THREADING (Network > Audio > Spectrum > Decoders): the pipeline thread only COPIES its IQ into a bounded queue
//    (oldest dropped and counted after 2 s — vibehealth::BehindWatch reads droppedBlocks()); OFDM, Viterbi and the
//    codec run on a vibe-drm thread at decoder priority. The audio callback PULLS exactly the frames it is sending, from a
//    buffer held ~0.8 s deep: DRM delivers audio in 400 ms super frames, so the buffer is what makes a steady stream of
//    them. Underrun = silence (the stream never stops), overrun = the oldest audio goes (latency never grows).
#pragma once
#include "decoders/drm/drm_audio.h"
#include "decoders/drm/drm_receiver.h"
#include "vibe_drm_audiodec.h"
#include "vibe_thread.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibe {

/** JSON string escape for labels and text that came off the air. */
inline std::string drmJsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

class DrmListener {
public:
    DrmListener() { th_ = std::thread([this] { loop(); }); }
    ~DrmListener() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    DrmListener(const DrmListener&) = delete;
    DrmListener& operator=(const DrmListener&) = delete;

    /** The channel's complex baseband, centred on the dial (RxPipeline::Callbacks::iq). Never blocks. */
    void feedIq(const std::complex<float>* x, int n, double rateHz) {
        if (n <= 0) return;
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(Block{std::vector<vibedrm::cf>(x, x + n), rateHz});
        queued_ += size_t(n);
        while (queued_ > size_t(rateHz * 2.0) && q_.size() > 1) { queued_ -= q_.front().iq.size(); q_.pop_front(); ++dropped_; }
        cv_.notify_one();
    }

    /** Exactly `frames` stereo frames of 48 kHz audio (zeros while there is none). */
    void takePcm(float* stereo, int frames) {
        std::lock_guard<std::mutex> lk(pm_);
        if (!primed_ && pcm_.size() >= size_t(kPrimeFrames) * 2) primed_ = true;
        int got = 0;
        if (primed_) {
            got = std::min<int>(frames, int(pcm_.size() / 2));
            std::copy(pcm_.begin(), pcm_.begin() + long(got) * 2, stereo);
            pcm_.erase(pcm_.begin(), pcm_.begin() + long(got) * 2);
            if (got < frames) { primed_ = false; ++underruns_; }
        }
        std::fill(stereo + got * 2, stereo + frames * 2, 0.0f);
    }

    /** ★ The report, and a counter that moves when it changes (the caller sends on change, at most per super frame). */
    std::string statusJson(uint64_t* version) const {
        std::lock_guard<std::mutex> lk(sm_);
        if (version) *version = statusVer_;
        return status_;
    }
    uint64_t droppedBlocks() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }

private:
    static constexpr int kPrimeFrames = 48000 * 8 / 10;   // 0.8 s
    static constexpr int kMaxFrames = 48000 * 3;
    struct Block { std::vector<vibedrm::cf> iq; double rate; };

    void loop() {
        vibeDecoderThread("vibe-drm");
        rx_.onLogicalFrame = [this](const vibedrm::LogicalFrame& lf) { onLogical(lf); };
        rx_.onSuperFrame = [this] { publish(); };
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                b = std::move(q_.front()); q_.pop_front(); queued_ -= b.iq.size();
            }
            const bool wasLocked = rx_.status().locked;
            rx_.feed(b.iq.data(), int(b.iq.size()), b.rate);
            if (wasLocked && !rx_.status().locked) { dec_.close(); framer_ = vibedrm::AudioFramer(); publish(); }
            std::vector<float> more; dec_.drain(more); pushPcm(more);
        }
    }

    void onLogical(const vibedrm::LogicalFrame& lf) {
        const vibedrm::RxStatus& st = rx_.status();
        // the first audio service — its stream, its audio description
        const vibedrm::ServiceSdc* sv = nullptr;
        for (const auto& s : st.sdc.svc) if (s.audio.present) { sv = &s; break; }
        if (!sv || sv->audio.streamId != lf.stream) return;
        if (framer_.configure(sv->audio)) { dec_.close(); decTried_ = false; }
        if (!decTried_) { decTried_ = true; dec_.open(framer_); }
        const size_t n = lf.bytes.size() - (sv->audio.text && lf.bytes.size() >= 4 ? 4 : 0);
        std::vector<vibedrm::AccessUnit> aus;
        framer_.push(lf.bytes.data(), n, aus);
        std::vector<float> pcm;
        for (const auto& au : aus) if (au.crcOk) { dec_.decode(au, pcm); ++decodedUnits_; } else ++badUnits_;
        pushPcm(pcm);
    }

    void pushPcm(const std::vector<float>& pcm) {
        if (pcm.empty()) return;
        std::lock_guard<std::mutex> lk(pm_);
        pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
        if (pcm_.size() > size_t(kMaxFrames) * 2) pcm_.erase(pcm_.begin(), pcm_.end() - long(kPrimeFrames) * 2);
        pcmFrames_ += pcm.size() / 2;
    }

    /** ★ Words where the standard has words; the numbers a DXer reads (MER, mode, QAM, rate) as numbers. */
    void publish() {
        const vibedrm::RxStatus& s = rx_.status();
        std::string j = "{\"type\":\"drm\"";
        auto b = [&](const char* k, bool v) { j += std::string(",\"") + k + "\":" + (v ? "true" : "false"); };
        b("sync", s.timeSync); b("freq", s.freqSync); b("frame", s.frameSync); b("fac", s.facOk); b("sdc", s.sdcOk); b("msc", s.mscOk);
        b("locked", s.locked);
        char buf[512];
        if (s.mode != vibedrm::Mode::None) { j += ",\"robustness\":\""; j += vibedrm::modeLetter(s.mode); j += "\""; }
        if (s.locked) {
            static const double bwKhz[6] = { 4.5, 5, 9, 10, 18, 20 };
            std::snprintf(buf, sizeof buf, ",\"bwKhz\":%g,\"mscQam\":%d,\"sdcQam\":%d,\"interleave\":\"%s\",\"codeRate\":%.2f,"
                          "\"snr\":%.1f,\"mscMer\":%.1f,\"offsetHz\":%.1f,\"clockPpm\":%.1f",
                          s.so >= 0 && s.so < 6 ? bwKhz[s.so] : 0.0, 1 << (2 * s.mscLevels), 1 << (2 * s.sdcLevels),
                          s.longInterleave ? "long" : "short", s.codeRateB, s.facMerDb, s.mscMerDb, s.freqOffsetHz, s.sroPpm);
            j += buf;
            if (s.fac.serviceId) { std::snprintf(buf, sizeof buf, ",\"serviceId\":\"%06X\"", unsigned(s.fac.serviceId)); j += buf; }
            const vibedrm::ServiceSdc& sv = s.sdc.svc[size_t(s.fac.shortId & 3)];
            if (!sv.label.empty()) j += ",\"label\":" + drmJsonStr(sv.label);
            const char* lang = vibedrm::languageName(s.fac.language);
            if (*lang) j += ",\"language\":" + drmJsonStr(lang);
            if (!sv.lang3.empty()) j += ",\"lang3\":" + drmJsonStr(sv.lang3);
            if (!sv.country2.empty()) j += ",\"country\":" + drmJsonStr(sv.country2);
            if (!s.fac.isData) { const char* pty = vibedrm::programmeTypeName(s.fac.descriptor); if (*pty) j += ",\"programme\":" + drmJsonStr(pty); }
            if (!s.text.empty()) j += ",\"text\":" + drmJsonStr(s.text);
            if (s.sdc.haveTime) { std::snprintf(buf, sizeof buf, ",\"utc\":\"%02d:%02d\"", s.sdc.hour, s.sdc.minute); j += buf; }
            if (sv.audio.present) {
                j += ",\"codec\":" + drmJsonStr(sv.audio.codecName()) + ",\"audioMode\":" + drmJsonStr(sv.audio.modeName());
                std::snprintf(buf, sizeof buf, ",\"audioRate\":%d,\"bitrate\":%d", sv.audio.sampleRateHz(), s.bitrateBps); j += buf;
                std::string why, backend;
                const bool can = DrmAudioDecoder::canDecode(sv.audio, why, backend);
                b("playable", can && dec_.isOpen());
                if (!can) j += ",\"why\":" + drmJsonStr(why);
                else if (!dec_.isOpen() && decTried_) j += ",\"why\":" + drmJsonStr(dec_.why());
                if (dec_.isOpen()) j += ",\"decoder\":" + drmJsonStr(dec_.backend());
                std::snprintf(buf, sizeof buf, ",\"audioUnits\":%lld,\"audioBad\":%lld", (long long)decodedUnits_, (long long)badUnits_); j += buf;
            }
        }
        std::snprintf(buf, sizeof buf, ",\"facOk\":%lld,\"facBad\":%lld,\"dropped\":%llu}", (long long)s.facGood, (long long)s.facBad,
                      (unsigned long long)droppedBlocks());
        j += buf;
        std::lock_guard<std::mutex> lk(sm_);
        if (j != status_) { status_ = j; ++statusVer_; }
    }

    vibedrm::Receiver rx_;
    vibedrm::AudioFramer framer_;
    DrmAudioDecoder dec_;
    bool decTried_ = false;
    int64_t decodedUnits_ = 0, badUnits_ = 0;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> q_; size_t queued_ = 0; uint64_t dropped_ = 0; bool stop_ = false;

    std::mutex pm_;
    std::vector<float> pcm_; bool primed_ = false; int64_t underruns_ = 0; uint64_t pcmFrames_ = 0;

    mutable std::mutex sm_;
    std::string status_ = "{\"type\":\"drm\",\"sync\":false}"; uint64_t statusVer_ = 1;
    std::thread th_;
};

}  // namespace vibe
