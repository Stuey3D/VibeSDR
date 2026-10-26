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
#include "vibe_drm_report.h"
#include "vibe_thread.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibe {

class DrmListener {
public:
    DrmListener() { th_ = std::thread([this] { loop(); }); }
    ~DrmListener() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        if (dumpF_) std::fclose(dumpF_);
    }
    DrmListener(const DrmListener&) = delete;
    DrmListener& operator=(const DrmListener&) = delete;

    /** The channel's complex baseband, centred on the dial (RxPipeline::Callbacks::iq). Never blocks. */
    void feedIq(const std::complex<float>* x, int n, double rateHz) {
        if (n <= 0) return;
        std::lock_guard<std::mutex> lk(m_);
        /* ★ DIAGNOSTIC (test benches only): VIBE_DRM_DUMP=<dir> writes every block this listener is fed — [f64 rate][i32 n]
         *  [n × cf32] — so a run that lost sync can be replayed offline through the same receiver (the flaky
         *  test-server-drm, 2026-10-26). Off unless the variable is set; never in a release's environment. */
        if (dumpF_ == nullptr && !dumpTried_) {
            dumpTried_ = true;
            if (const char* d = std::getenv("VIBE_DRM_DUMP")) {
                char path[512]; std::snprintf(path, sizeof path, "%s/drm-iq-%p.bin", d, static_cast<void*>(this));
                dumpF_ = std::fopen(path, "wb");
            }
        }
        if (dumpF_) { const int32_t nn = n; std::fwrite(&rateHz, 8, 1, dumpF_); std::fwrite(&nn, 4, 1, dumpF_); std::fwrite(x, 8, size_t(n), dumpF_); }
        lastN_ = n; lastRate_ = rateHz;
        q_.push_back(Block{std::vector<vibedrm::cf>(x, x + n), rateHz});
        queued_ += size_t(n);
        while (queued_ > size_t(rateHz * 2.0) && q_.size() > 1) { queued_ -= q_.front().iq.size(); q_.pop_front(); ++dropped_; }
        cv_.notify_one();
    }

    /** ★★★ BLOCKS THAT NEVER CAME (2026-10-26 — the flaky test-server-drm). A range-locked listener's DSP thread is fed
     *  through a queue with ~40 ms of slack (handBlockToListeners); when that thread is descheduled the oldest blocks
     *  are DROPPED, and the thread sees it as a jump in the channelizer's block numbering. Audio shrugs a 10 ms hole off.
     *  OFDM does not: the samples after the hole arrive a fraction of a symbol early, the frame alignment is gone, 16
     *  FACs fail and the lock is lost — 5 to 10 s of silence for one scheduling hiccup. So the hole is FILLED with as
     *  many zeros as the blocks carried (every block of a channel is the same length — the last one is the measure):
     *  the timeline keeps its alignment, the missing cells equalise to zero weight — erasures, which the interleaver
     *  and the Viterbi absorb — and the lock holds. Counted for the Signal pane ("IQ gaps"). */
    void noteGap(int blocks) {
        if (blocks <= 0) return;
        std::lock_guard<std::mutex> lk(m_);
        ++gaps_;
        if (lastN_ <= 0 || !(lastRate_ > 0) || blocks > 64) return;   // nothing to measure by, or far too long to bridge
        const size_t n = size_t(blocks) * size_t(lastN_);
        q_.push_back(Block{std::vector<vibedrm::cf>(n, vibedrm::cf(0, 0)), lastRate_});
        queued_ += n;
        if (dumpF_) { const int32_t nn = int32_t(n); std::fwrite(&lastRate_, 8, 1, dumpF_); std::fwrite(&nn, 4, 1, dumpF_);
                      std::fwrite(q_.back().iq.data(), 8, n, dumpF_); }
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
    uint64_t gaps() const { std::lock_guard<std::mutex> lk(m_); return gaps_; }

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

    /** ★ The report (vibe_drm_report.h — its own header so the wire format is tested on its own). */
    void publish() {
        const vibedrm::RxStatus& s = rx_.status();
        DrmReportExtras x;
        const vibedrm::ServiceSdc& sv = s.sdc.svc[size_t(s.fac.shortId & 3)];
        if (s.locked && sv.audio.present) {
            std::string why, backend;
            x.canDecode = DrmAudioDecoder::canDecode(sv.audio, why, backend);
            x.decoderOpen = dec_.isOpen(); x.decoderTried = decTried_;
            x.why = x.canDecode ? (x.decoderOpen ? std::string() : dec_.why()) : why;
            if (x.decoderOpen) x.backend = dec_.backend();
            x.concealed = dec_.unitsConcealed();
        }
        x.audioUnits = decodedUnits_; x.audioBad = badUnits_;
        { std::lock_guard<std::mutex> lk(pm_); x.underruns = underruns_; }
        x.dropped = droppedBlocks();
        x.gaps = gaps();
        x.blanked = rx_.recentBlankedFraction();
        std::string j = drmReportJson(s, x);
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
    std::FILE* dumpF_ = nullptr; bool dumpTried_ = false;   // VIBE_DRM_DUMP (diagnostic)
    int lastN_ = 0; double lastRate_ = 0; uint64_t gaps_ = 0;   // noteGap: the block length to fill a hole with

    std::mutex pm_;
    std::vector<float> pcm_; bool primed_ = false; int64_t underruns_ = 0; uint64_t pcmFrames_ = 0;

    mutable std::mutex sm_;
    std::string status_ = "{\"type\":\"drm\",\"sync\":false}"; uint64_t statusVer_ = 1;
    std::thread th_;
};

}  // namespace vibe
