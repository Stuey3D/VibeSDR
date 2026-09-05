// vibe_dab_service.h — DAB as the shim sees it: IQ in, PCM and JSON out.
//
// Everything below this file is pure DSP with no threading and no I/O. This is the one place that
// knows about locks, buffering and the shim's conventions, so the decoder stays testable on its
// own with a two-line g++ command.
//
// ★★ THE SAMPLE RATE IS NOT NEGOTIABLE. A DAB ensemble wants 2.048 MSPS — the rate the standard
//    was designed around, and the one that makes the useful symbol exactly 2048 samples. The shim
//    must put the radio there before feeding this; resampling 2.4 MSPS down would cost CPU on a Pi
//    for nothing, since the dongle can simply run at 2.048.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "vibe_dab_channels.h"
#include "vibe_dab_mp2.h"
#include "vibe_dab_aac.h"
#include "vibe_dab_receiver.h"

namespace vibedab {

class DabService {
public:
    /** The rate every listener socket is fed at, whatever the service was coded at. */
    static constexpr uint32_t kAudioRateHz = 48000;
    static constexpr uint32_t kRateHz = kCanonicalRateHz;   // 2.048 MSPS

    /** Tune to a Band III block by index into kBandIII. Clears the ensemble: a new multiplex is a
     *  new station list, and showing the old one would be a lie for as long as it took to refill. */
    void setChannel(int idx) {
        std::lock_guard<std::mutex> lk(m_);
        if (idx < 0 || idx >= int(kBandIIICount)) return;
        if (idx == channel_) return;
        channel_ = idx;
        rx_.reset();
        iq_.clear();
        pcm_.clear();
        sid_ = 0;
        mp2_.reset();
    }
    int  channel()   const { return channel_; }
    uint32_t centreHz() const { return kBandIII[channel_ < 0 ? 0 : channel_].centreHz; }

    /** Choose a service by SId. Safe to call before the ensemble has arrived — it is remembered
     *  and applied as soon as the service appears, which is what makes a bookmark work on a cold
     *  tune (recall is channel + SId; see BRIEF-dab.md). */
    void setService(uint32_t sid) {
        std::lock_guard<std::mutex> lk(m_);
        want_ = sid;
        if (rx_.ensemble().services.count(sid)) { if (rx_.selectService(sid)) sid_ = sid; }
        pcm_.clear();
        mp2_.reset();
    }
    uint32_t service() const { return sid_; }
    /** The receiver tells us where the radio actually is, so the two can be compared. */
    void setRfCentre(double hz) { std::lock_guard<std::mutex> lk(m_); rfCentre_ = hz; }
    /** The rate the shim is actually running at — so a mismatch with 2.048 MS/s is visible rather
     *  than inferred from a receiver that simply fails to lock. */
    void setRfRate(double hz) { std::lock_guard<std::mutex> lk(m_); rfRate_ = hz; }

    /** Feed interleaved complex samples at 2.048 MSPS. */
    void feed(const float* interleaved, size_t nSamples) {
        std::lock_guard<std::mutex> lk(m_);
        samplesIn_ += nSamples;
        const size_t base = iq_.size();
        iq_.resize(base + nSamples);
        for (size_t i = 0; i < nSamples; ++i)
            iq_[base + i] = { interleaved[2 * i], interleaved[2 * i + 1] };

        const size_t need = size_t(modeI().frameSamples) * 2;
        while (iq_.size() >= need) {
            ++pushCalls_;
            if (rx_.push(iq_.data(), need)) ++pushOk_;
            /* ★★★ TRACK ACROSS THE BOUNDARY — DO NOT RE-ACQUIRE EVERY FRAME.
             *  This used to call resetSync() here, throwing the lock away and acquiring afresh on
             *  every 96 ms frame. Acquisition takes the GLOBAL MINIMUM over a frame-long scan, so
             *  it only has to lose once — one noisy frame where some other dip is deeper — and
             *  that frame decodes as noise. On the live V4L that was 66 dips below 0.99 FIB in
             *  three minutes, with the gain never moving and the frequency offset rock steady at
             *  37 Hz: brief random bursts in an otherwise clean stream, which is what Stuart heard
             *  and rightly suspected was ours rather than the aerial.
             *  ★ The tell was in the numbers all along: nullDepthDb alternating between 15 and 24
             *    dB frame by frame is one detector choosing between two candidates, not a signal
             *    changing, and prs collapsing from 11.8 to 0.96 is a window in the wrong place.
             *  ★ The TRACK path searches +/-64 samples around a PREDICTION and was written for
             *    exactly this. It needs only to be told what we consumed, which is what the old
             *    comment here ("immune to the buffer edges") was working around instead. */
            {
                const long at = rx_.lastFrameStart();
                if (at >= 0) {
                    if (lastAt_ >= 0 && std::labs(at - lastAt_) > 16) ++syncJumps_;
                    lastAt_ = at;
                }
            }
            rx_.resetSync();
            /* ★★★ REVERTED (4.1.89): consuming `at + frameSamples` MEASURED WORSE on air.
             *  It raised the decoded frame rate from 20% to 50% of real time and then wrecked the
             *  thing that matters — Stuart's signal panel on 11A read frequency offset -24152 Hz,
             *  carrier shift -24 and phase reference 0.495, with FIB pass at 1.7% and most of the
             *  station list "(unnamed)". Before it, that same receiver sat at -126 Hz, shift 0 and
             *  FIB 0.99. A frame rate is not worth a receiver that cannot read the ensemble.
             *  ★ The 20% shortfall is REAL and still unexplained — see the frame-rate measurement
             *    in the 4.1.88 message. It is not this. Measure where the samples go before
             *    changing this line again. */
            iq_.erase(iq_.begin(), iq_.begin() + long(modeI().frameSamples));

            if (want_ && sid_ != want_ && rx_.ensemble().services.count(want_))
                if (rx_.selectService(want_)) sid_ = want_;

            drainAudio();
        }
        /* ★ Never let the backlog grow without bound: if the caller feeds faster than we decode,
         *  drop the OLDEST samples. Audio that is minutes late is worse than a gap. */
        if (iq_.size() > need * 4) {
            dropped_ += uint32_t(iq_.size() - need * 2);
            iq_.erase(iq_.begin(), iq_.end() - long(need * 2));
        }
    }

    /** Take decoded audio (interleaved stereo, 48 kHz). Returns frames written. */
    size_t takePcm(float* out, size_t maxFrames) {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = 0;
        while (n < maxFrames && pcm_.size() >= 2) {
            out[2 * n]     = pcm_.front(); pcm_.pop_front();
            out[2 * n + 1] = pcm_.front(); pcm_.pop_front();
            ++n;
        }
        return n;
    }
    size_t pcmAvailable() { std::lock_guard<std::mutex> lk(m_); return pcm_.size() / 2; }

    /** True while the selected service is DAB+ — its audio leaves as ADTS, not PCM. */
    bool dabPlus() { std::lock_guard<std::mutex> lk(m_); return rx_.selectedType() != 0; }
    /** Take one reframed access unit, or false when none is waiting. */
    bool takeAdts(std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (adts_.empty()) return false;
        out = std::move(adts_.front());
        adts_.pop_front();
        return true;
    }
    /** The rate the ADTS header declares — the AAC CORE rate. Under SBR the decoder doubles it
     *  itself, so this is what a decoder must be CONFIGURED with, not what it will output. */
    int aacCoreRateHz() { std::lock_guard<std::mutex> lk(m_); return afmt_.coreRateHz; }
    int aacChannels()   { std::lock_guard<std::mutex> lk(m_); return aacOutCh_; }

    /** The station list and the signal block, as the web client wants them. */
    std::string json() {
        std::lock_guard<std::mutex> lk(m_);
        const Ensemble& e = rx_.ensemble();
        const DabStats& s = rx_.stats();
        std::string j = "{\"type\":\"dab\"";
        char b[512];
        /* ★ `rfCentreHz` is what the RADIO is actually on, not what we asked for. They diverged on
         *  the live Pi — DAB reported 12B while the dongle sat on 96.6 MHz — and without both
         *  numbers side by side that is indistinguishable from "DAB does not decode here". */
        snprintf(b, sizeof b,
                 ",\"channel\":\"%s\",\"centreHz\":%u,\"mp2Crc\":%u,\"mp2In\":%u,\"mp2Bad\":%u,\"mp2Out\":%u,\"syncJumps\":%u,\"samplesIn\":%llu,\"pushCalls\":%u,\"pushOk\":%u,\"dropped\":%u,\"sfFrames\":%u,\"sfBadLen\":%u,\"sfTried\":%u,\"sfOk\":%u,\"aus\":%u,\"rfCentreHz\":%.0f,\"rfRateHz\":%.0f,\"label\":\"%s\",\"eid\":%u",
                 channel_ >= 0 ? kBandIII[channel_].name : "", centreHz(), mp2WithCrc_, mp2In_, mp2Bad_, mp2Out_, syncJumps_, (unsigned long long)samplesIn_, pushCalls_, pushOk_, dropped_, sfFrames_, sfBadLen_, sfTried_, sfOk_, ausOut_, rfCentre_, rfRate_,
                 esc(e.label).c_str(), unsigned(e.eid));
        j += b;
        snprintf(b, sizeof b,
                 ",\"locked\":%s,\"nullDepthDb\":%.1f,\"offsetHz\":%.0f,\"offsetPpm\":%.2f"
                 ",\"carrierShift\":%d,\"prs\":%.3f,\"fibOk\":%d,\"fibTotal\":%d,\"fibRate\":%.3f"
                 ",\"frames\":%d,\"sid\":%u,\"bitrate\":%d,\"protection\":\"%s\"",
                 s.locked ? "true" : "false", s.nullDepthDb, s.freqOffsetHz, s.freqOffsetPpm,
                 s.intOffsetCarriers, s.prsCorrelation, s.fibsOk, s.fibsTotal, s.fibRate,
                 s.framesSeen, unsigned(sid_), rx_.serviceBitrate(),
                 rx_.uepProf().valid ? "UEP" : (rx_.profile().valid ? "EEP" : ""));
        j += b;
        j += ",\"services\":[";
        bool first = true;
        for (const auto& kv : e.services) {
            const Service& sv = kv.second;
            int sub = -1, type = -1;
            for (const auto& c : sv.components) if (c.subChId >= 0) { sub = c.subChId; type = c.scType; if (c.primary) break; }
            if (sub < 0) continue;
            if (!first) j += ',';
            first = false;
            snprintf(b, sizeof b, "{\"sid\":%u,\"label\":\"%s\",\"codec\":\"%s\",\"subch\":%d}",
                     unsigned(kv.first), esc(sv.label).c_str(),
                     type == 63 ? "DAB+" : type == 0 ? "MP2" : "?", sub);
            j += b;
        }
        j += "]}";
        return j;
    }

private:
    /** Turn whatever logical frames arrived into PCM. */
    void drainAudio() {
        // ★ TAKE, do not index — the receiver's buffer is a bounded ring. See takeAudioFrames().
        const auto frames = rx_.takeAudioFrames();
        for (const auto& f : frames) {
            /* ★★ MP2 WE DECODE OURSELVES — the browser refuses Layer II, measured 2026-09-04, and
             *  MP2's patents have expired so it is the one codec we may implement. DAB+ is handed
             *  onward as ADTS instead; that path links no decoder here. */
            if (rx_.selectedType() != 0) { pumpDabPlus(f); continue; }
            std::vector<float> out;
            ++mp2In_;
            if (mp2_.decode(f.data(), f.size(), out) <= 0) { ++mp2Bad_; continue; }
            ++mp2Out_;
            if (mp2_.lastHadCrc()) ++mp2WithCrc_;
            /* ★★★ THE CHIPMUNKS. Mp2Decoder writes INTERLEAVED at the frame's OWN channel count
             *  and its OWN sample rate, and this pushed the result straight into a buffer that
             *  takePcm() reads as 48 kHz STEREO pairs. Two independent speed-ups, and UK DAB has
             *  both: a MONO service (talkSPORT, LBC) hands back one sample per frame slot and
             *  every pair read as L/R plays at 2x; an LSF service at 24 kHz plays at 2x again.
             *  BBC National 12B is 48 kHz stereo throughout, which is why it sounded perfect and
             *  D1 National did not — the first mux I tested agreed with the bug.
             *  ★ Upmix and resample HERE, where the frame's own header is still in hand. Linear
             *    interpolation: the ratios are exact small integers (48/24 = 2, 48/32 = 1.5) so
             *    this is not the place to spend an FIR, and a wrong-speed stream is not a
             *    fidelity problem to be tuned — it is a bug to be removed. */
            const auto& mi = mp2_.info();
            const int   nch = mi.channels > 0 ? mi.channels : 2;
            const int   sr  = mi.sampleRateHz > 0 ? mi.sampleRateHz : int(kAudioRateHz);
            const size_t frames = out.size() / size_t(nch);
            if (!frames) continue;
            if (sr == int(kAudioRateHz)) {
                for (size_t i = 0; i < frames; ++i) {
                    const float l = out[i * size_t(nch)];
                    const float r = nch == 1 ? l : out[i * size_t(nch) + 1];
                    pcm_.push_back(l); pcm_.push_back(r);
                }
            } else {
                const double step = double(sr) / double(kAudioRateHz);
                const double span = double(frames - 1);
                for (double t = 0.0; t <= span; t += step) {
                    const size_t i = size_t(t);
                    const double fr = t - double(i);
                    const size_t j = i + 1 < frames ? i + 1 : i;
                    const float l0 = out[i * size_t(nch)], l1 = out[j * size_t(nch)];
                    const float l  = float(l0 + (l1 - l0) * fr);
                    float r = l;
                    if (nch == 2) {
                        const float r0 = out[i * size_t(nch) + 1], r1 = out[j * size_t(nch) + 1];
                        r = float(r0 + (r1 - r0) * fr);
                    }
                    pcm_.push_back(l); pcm_.push_back(r);
                }
            }
        }
        while (pcm_.size() > size_t(kAudioRateHz) * 2 * 2) pcm_.pop_front();   // ~2 s of slack
    }

    /** One DAB+ logical frame: hold five, test the firecode, reframe the AUs it contains. */
    void pumpDabPlus(const std::vector<uint8_t>& frame) {
        if (frame.empty()) return;
        ++sfFrames_;
        sf_.push_back(frame);
        if (sf_.size() > 5) sf_.pop_front();
        if (sf_.size() < 5) return;

        /* ★ subchannel_index is the sub-channel size in kbit/s / 8, and clause 6.2 lays the wire
         *  out as index rows by 120 columns — so five logical frames of 24*index bytes each. The
         *  index is derived from the frame length rather than the advertised bitrate: the length
         *  is what the de-interleaver actually has to match, and deriving it cannot disagree. */
        const size_t per = sf_.front().size();
        for (const auto& x : sf_) if (x.size() != per) { ++sfBadLen_; sf_.pop_front(); return; }
        const int index = int(per / 24);
        if (index < 1 || index > 24) { ++sfBadLen_; sf_.pop_front(); return; }

        std::vector<uint8_t> wire;
        wire.reserve(per * 5);
        for (const auto& x : sf_) wire.insert(wire.end(), x.begin(), x.end());

        ++sfTried_;
        SuperFrame s = decodeSuperFrame(wire.data(), wire.size(), index);
        // ★ SLIDE BY ONE on failure. Dropping all five would re-test the same phase for ever.
        if (!s.valid || !s.firecodeOk) { sf_.pop_front(); return; }
        sf_.clear();
        ++sfOk_;

        afmt_       = s.fmt;
        aacCoreCh_  = s.stereo ? 2 : 1;
        aacOutCh_   = (s.stereo || s.ps) ? 2 : 1;
        for (const auto& au : s.aus) {
            std::vector<uint8_t> pkt = toAdts(au.data(), au.size(), s.fmt, aacCoreCh_);
            if (!pkt.empty()) { adts_.push_back(std::move(pkt)); ++ausOut_; }
        }
        // ★ Bounded like the PCM: audio minutes late is worse than a gap.
        while (adts_.size() > 250) adts_.pop_front();
    }

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (uint8_t(c) >= 0x20) o += c;
        }
        return o;
    }

    /* ── DAB+ ──────────────────────────────────────────────────────────────────────────────
     *  We never decode AAC. The super frame is de-interleaved and Reed-Solomon corrected here,
     *  the access units are reframed as ADTS, and the BROWSER's own decoder does the rest — which
     *  is the licence position this whole design was chosen for (see vibe_dab_aac.h).
     *  ★ Five 24 ms logical frames make one 120 ms super frame, and nothing tells us which of the
     *    five starts it. The firecode does: assemble five, test it, and on failure slide by ONE
     *    frame rather than dropping all five, or a stream that starts mid-super-frame never
     *    aligns at all. */
    std::deque<std::vector<uint8_t>> sf_;      ///< the five-frame window
    std::deque<std::vector<uint8_t>> adts_;    ///< reframed AUs, ready for the wire
    AudioFormat afmt_{};
    /* ★ Counters, because "no audio" has four possible causes here and guessing between them is
     *  what cost the evening: no frames arriving, frames of an unusable length, the firecode
     *  never aligning, or AUs produced and not sent. Each has its own number. */
    uint32_t sfFrames_ = 0, sfBadLen_ = 0, sfTried_ = 0, sfOk_ = 0, ausOut_ = 0;
    /* ★ The input side, because the 50% frame shortfall has exactly three possible homes and
     *  they need separating by measurement rather than argument: samples never arriving, frames
     *  arriving and being REJECTED by push(), or samples arriving and being dropped by the
     *  backlog guard. One counter each. */
    /* ★ WHERE THE NULL WAS FOUND, frame to frame. With resetSync() on every frame the buffer is
     *  re-acquired from scratch each time, and because exactly one frame is consumed the answer
     *  should be the SAME offset every time. A jump means acquisition picked a different dip —
     *  and a wrong frame feeds garbage into a 15-CIF time deinterleaver, so one bad acquisition
     *  costs ~400 ms of audio while the FIB rate, which is not interleaved, barely moves. That is
     *  the shape of Stuart's bursts on a signal reporting 98% FIB. */
    /* ★★★ THE MSC IS NOT THE FIC. They are protected separately — the FIC always heavily, the
     *  MSC by this subchannel's own UEP or EEP profile — so a 100% FIB pass rate says the CONTROL
     *  channel is clean and NOTHING about the audio. Stuart is hearing breakup at 100% FIB, which
     *  is exactly what that distinction predicts, and it is where the measurement has to go next:
     *  how many audio frames actually decode, against how many arrive. */
    uint32_t mp2In_ = 0, mp2Bad_ = 0, mp2Out_ = 0, mp2WithCrc_ = 0;
    long     lastAt_ = -1;
    uint32_t syncJumps_ = 0;
    uint64_t samplesIn_ = 0;
    uint32_t pushCalls_ = 0, pushOk_ = 0, dropped_ = 0;
    int  aacCoreCh_ = 2;                       ///< what the ADTS header declares
    int  aacOutCh_  = 2;                       ///< what the decoder will produce (PS -> 2)

    mutable std::mutex m_;
    DabReceiver rx_{kRateHz};
    Mp2Decoder  mp2_;
    std::vector<Cplx> iq_;
    std::deque<float> pcm_;
    int channel_ = -1;
    double rfCentre_ = 0, rfRate_ = 0;
    uint32_t sid_ = 0, want_ = 0;
};

}  // namespace vibedab
