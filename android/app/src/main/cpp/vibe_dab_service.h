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
        const size_t base = iq_.size();
        iq_.resize(base + nSamples);
        for (size_t i = 0; i < nSamples; ++i)
            iq_[base + i] = { interleaved[2 * i], interleaved[2 * i + 1] };

        const size_t need = size_t(modeI().frameSamples) * 2;
        while (iq_.size() >= need) {
            rx_.push(iq_.data(), need);
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
            rx_.resetSync();
            /* ★★★ CONSUME THROUGH THE FRAME WE JUST DECODED, not a fixed amount off the front.
             *  The null is found at `at`, anywhere in the search window — but this erased exactly
             *  frameSamples from position 0 regardless, so the buffer stayed misaligned by `at`
             *  for ever. push() then hit its own guard:
             *      if (start + symLen * symbolsPerFrame > n) return false;
             *  because a frame starting at `at` does not FIT in two frames' worth of buffer once
             *  `at` exceeds one frame. It only got through on the occasional oversized buffer.
             *  ★★★ MEASURED: 2.09 DAB frames decoded per second against a real-time rate of 10.42
             *    — 20% — with the box 64% idle and the radio at 22% CPU, so nothing was starved
             *    for time. Four of every five frames were being thrown away by an off-by-`at`
             *    buffer, and the audio arrived at 11 packets/s instead of 50: the client ran dry
             *    and concealed, which is Stuart's "random 1 second burst of errors" and why it
             *    sounded like reception on a signal whose FIB CRC was 0.99.
             *  ★ Aligning here also makes the next acquisition start ON the null, which is why
             *    the depth reading stops flickering between two candidates. */
            const long at = rx_.lastFrameStart();
            const size_t drop = at >= 0 ? size_t(at) + size_t(modeI().frameSamples)
                                        : size_t(modeI().frameSamples);
            iq_.erase(iq_.begin(), iq_.begin() + long(drop < iq_.size() ? drop : iq_.size()));

            if (want_ && sid_ != want_ && rx_.ensemble().services.count(want_))
                if (rx_.selectService(want_)) sid_ = want_;

            drainAudio();
        }
        /* ★ Never let the backlog grow without bound: if the caller feeds faster than we decode,
         *  drop the OLDEST samples. Audio that is minutes late is worse than a gap. */
        if (iq_.size() > need * 4) iq_.erase(iq_.begin(), iq_.end() - long(need * 2));
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
                 ",\"channel\":\"%s\",\"centreHz\":%u,\"rfCentreHz\":%.0f,\"rfRateHz\":%.0f,\"label\":\"%s\",\"eid\":%u",
                 channel_ >= 0 ? kBandIII[channel_].name : "", centreHz(), rfCentre_, rfRate_,
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
            if (rx_.selectedType() != 0) continue;
            std::vector<float> out;
            if (mp2_.decode(f.data(), f.size(), out) <= 0) continue;
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

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (uint8_t(c) >= 0x20) o += c;
        }
        return o;
    }

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
