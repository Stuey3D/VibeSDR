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
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#include "vibe_dab_channels.h"
#include "vibe_dab_resample.h"
#include "vibe_dab_mp2.h"
#include "vibe_dab_aacdec.h"
#include "vibe_dab_pad.h"
#include "vibe_dab_aac.h"
#include "vibe_dab_receiver.h"

namespace vibedab {

class DabService {
public:
        ~DabService() { stopWorker(); }

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
        rx_.setCentreHz(double(kBandIII[idx].centreHz));
        iq_.clear();
        { std::lock_guard<std::mutex> plk(pm_); pcm_.clear(); }
        /* ★★★ AND DROP ANY HELD HALF-FRAME. lsfPend_ carries the first half of a 24 kHz Layer II
         *  frame between calls; a service or multiplex change mid-pair would otherwise join it to
         *  the FIRST frame of the new subchannel and leave the pairing off by one from then on —
         *  a perfect FIC with a large fraction of the audio failing, which is precisely how it
         *  presented: 1.0% on a first entry and 16.9% on the second, same service, same signal. */
        lsfPend_.clear();
        sid_ = 0;
        mp2_.reset();
        /* ★ And the AAC decoder, for the same reason as lsfPend_ and mp2_: it holds a codec
         *  configured for the OLD service's rate and channel mode, and DAB+ services on one
         *  multiplex differ in both. Carrying it across is the chipmunk bug wearing a new hat. */
        aac_.reset();
        adts_.clear();
        pad_.reset();      // ★ the label belongs to the old programme
        // ★ A new programme starts a new clock; catching up on the old one would be a wall of silence.
        pcmOwed_ = 0; pcmPushed_ = 0;
        resampleReset();
    }
    /** ★★★ A RETUNE IS IN FLIGHT — discard everything until the radio has settled on it.
     *  Called straight after tuneHw(), which only QUEUES the frequency. See the note in feed().
     *  @param seconds how long the hardware may take; 0.25 s is generous for a USB control
     *         transfer and is paid once per multiplex change, not per frame. */
    void armRetune(double seconds = 0.25) {
        std::lock_guard<std::mutex> lk(m_);
        const double rate = rfRate_ > 0 ? rfRate_ : 2400000.0;
        settleDrop_ = size_t(seconds * rate);
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
        { std::lock_guard<std::mutex> plk(pm_); pcm_.clear(); }
        mp2_.reset();
        aac_.reset();          // ★ a new service is a new codec configuration — see setChannel
        adts_.clear();
        pad_.reset();
        pcmOwed_ = 0; pcmPushed_ = 0;
        resampleReset();
        /* ★★★ AND DROP ANY HELD HALF-FRAME. lsfPend_ carries the first half of a 24 kHz Layer II
         *  frame between calls; a service or multiplex change mid-pair would otherwise join it to
         *  the FIRST frame of the new subchannel and leave the pairing off by one from then on —
         *  a perfect FIC with a large fraction of the audio failing, which is precisely how it
         *  presented: 1.0% on a first entry and 16.9% on the second, same service, same signal. */
        lsfPend_.clear();

    }
    uint32_t service() const { return sid_; }
    /** The receiver tells us where the radio actually is, so the two can be compared. */
    void setRfCentre(double hz) { std::lock_guard<std::mutex> lk(m_); rfCentre_ = hz; }
    /** The rate the shim is actually running at — so a mismatch with 2.048 MS/s is visible rather
     *  than inferred from a receiver that simply fails to lock. */
    void setRfRate(double hz) { std::lock_guard<std::mutex> lk(m_); rfRate_ = hz; }

    /** ★★★ FEED ONLY. THE DECODING HAPPENS ON ITS OWN THREAD — see workerLoop.
     *
     *  DAB was decoded INLINE on the DSP thread: a 64-state full-frame Viterbi over every CIF,
     *  four times per 96 ms frame, inside the same callback that has to keep emptying the radio's
     *  buffers. Whenever that overran, librtlsdr DISCARDED capture buffers before we ever saw
     *  them — so `samplesIn` still averaged a healthy 2.048 MS/s and `dropped` stayed at zero,
     *  because both of those count what ARRIVED. The loss was invisible to every counter we had,
     *  which is why the measurements kept saying the input was perfect.
     *
     *  ★★★ Stuart's hypothesis, and the evidence for it is that the XCOVER — a phone, and slower —
     *  is WORSE than the Pi on an aerial with a year of flawless DAB behind it. Signal problems do
     *  not care how fast the computer is; timing problems care about nothing else.
     *
     *  ★ A gap in the IQ is a discontinuity, and ONE discontinuity spread through a 16-CIF
     *    deinterleaver is ~380 ms of damage — exactly the runs measured in the captured frames
     *    (frames 156-169, fourteen consecutive frames corrupted).
     *
     *  ★ This function is now a memcpy and a notify. Everything expensive is on the worker. */
    /** ★★★ RAW IQ TO DISK, FOR AN OFFLINE HARNESS. Off unless VIBE_IQ_DUMP names a file.
     *  Every wrong theory this week was overturned by REAL CAPTURED DATA and none by reasoning,
     *  and each hypothesis was costing a ten-minute deploy to the live Pi. With the air on disk
     *  the three stages that can still be at fault — the 16-CIF time deinterleaver, the UEP
     *  depuncturing and CU extraction — can be tested in seconds against the same samples.
     *  ★★★ BUFFERED IN RAM AND WRITTEN ONCE AT THE END. Writing 9.6 MB/s to the SD card from
     *      this thread would stall the DSP loop and punch exactly the gaps we are hunting into
     *      the recording — the capture would manufacture its own evidence.
     *  ★★ VIBE_IQ_SKIP seconds are discarded first so the AGC has settled (Stuart asked for this
     *     explicitly: a gain still climbing is not the receiver anyone listens to).
     *  ★ int16 loses nothing: the source is an 8-bit dongle. Header carries the rate and centre,
     *    because a capture whose sample rate has to be guessed is a capture that lies. */
    void dumpIq_(const float* interleaved, size_t nSamples) {
        static const char* path = std::getenv("VIBE_IQ_DUMP");
        if (!path) return;
        static const double skipSecs = std::getenv("VIBE_IQ_SKIP") ? atof(std::getenv("VIBE_IQ_SKIP")) : 30.0;
        static const double capSecs  = std::getenv("VIBE_IQ_SECS") ? atof(std::getenv("VIBE_IQ_SECS")) : 30.0;
        static bool done = false;
        if (done) return;
        const double rate = rfRate_ > 0 ? rfRate_ : 2400000.0;
        static double firstAt = 0.0;
        const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
        if (firstAt == 0.0) { firstAt = now; return; }
        if (now - firstAt < skipSecs) return;                   // let the AGC settle
        static std::vector<int16_t> buf;
        static size_t w = 0;
        if (buf.empty()) {
            buf.resize(size_t(rate * capSecs) * 2);
            fprintf(stderr, "[DAB] IQ capture started: %.0f s at %.0f MS/s -> %s (%zu MB)\n",
                    capSecs, rate, path, (buf.size() * 2) / (1024 * 1024));
        }
        const size_t want = std::min(nSamples * 2, buf.size() - w);
        for (size_t i = 0; i < want; ++i) {
            float v = interleaved[i] * 32767.0f;
            buf[w + i] = int16_t(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        w += want;
        if (w < buf.size()) return;
        done = true;
        if (FILE* fp = std::fopen(path, "wb")) {
            const char magic[8] = { 'V','I','B','E','I','Q','1','6' };
            const double rc = rfCentre_;
            std::fwrite(magic, 1, 8, fp);
            std::fwrite(&rate, sizeof rate, 1, fp);
            std::fwrite(&rc,   sizeof rc,   1, fp);
            std::fwrite(buf.data(), sizeof(int16_t), buf.size(), fp);
            std::fclose(fp);
            fprintf(stderr, "[DAB] IQ capture WRITTEN: %s\n", path);
        }
        std::vector<int16_t>().swap(buf);
    }

    void feed(const float* interleaved, size_t nSamples) {
        dumpIq_(interleaved, nSamples);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!started_) { started_ = true; stop_ = false;
                             worker_ = std::thread([this] { workerLoop(); }); }
            samplesIn_ += nSamples;
            /* ★★★ THROW AWAY THE IQ THAT ARRIVES BEFORE THE RADIO HAS ACTUALLY MOVED.
             *
             *  ★★★ THE FAULT, and it is user-visible on every platform. Stuart: "when you first
             *      activate dab mode it seems to default to 12B ... it looks like you should have
             *      a signal judging by the spectrum but nothing locks in, tune away then back and
             *      it works" — and it does the same on the XCover and on the Pi. MEASURED on the
             *      XCover, tuning 11A while the radio sat on 10D:
             *          first attempt   frames 0    locked false   rfCentre 215072000  (10D!)
             *          second attempt  frames 495  locked true    rfCentre 216928000
             *
             *  ★★★ WHY IT CANNOT RECOVER BY ITSELF. tuneHw() QUEUES the frequency onto the
             *      hardware-writer thread, so the dsp loop goes on delivering samples from the
             *      OLD multiplex for as long as that takes. The receiver has just been reset by
             *      setChannel, so it acquires on that — and acquisition is deliberately TRACKED
             *      rather than re-taken every frame (see the note in workerLoop: re-acquiring per
             *      frame costs a frame every time some other dip is deeper). Having locked onto
             *      the wrong signal, it keeps tracking it. Tuning away and back works only
             *      because by then the radio is already sitting still.
             *
             *  ★★ AND THE CENTRE CANNOT BE USED TO DETECT IT: rtlCenter is stored BEFORE tuneHw,
             *     so rfCentre_ reports the NEW multiplex while the hardware is still on the old
             *     one. The service has to be TOLD a retune is in flight — hence armRetune().
             *  ★ Counted in SAMPLES, not milliseconds: the capture rate is the clock that matters
             *    and it makes the settle identical on a fast phone and a slow Pi. */
            if (settleDrop_ > 0) {
                const size_t drop = nSamples < settleDrop_ ? nSamples : settleDrop_;
                settleDrop_ -= drop;
                preTuneDropped_ += uint32_t(drop);
                if (settleDrop_ == 0) {
                    /* ★ Everything that could hold state from the old frequency, together — a
                     *  half-assembled LSF pair or a stale AAC decoder would outlive the retune
                     *  exactly as the sync did. */
                    rx_.reset(); iq_.clear(); lsfPend_.clear();
                    mp2_.reset(); aac_.reset(); pad_.reset(); adts_.clear();
                    { std::lock_guard<std::mutex> plk(pm_); pcm_.clear(); }
                    pcmOwed_ = 0; pcmPushed_ = 0; resampleReset();
                }
                return;
            }
            /* ★★★ THE DONGLE RUNS AT 2.4 MS/s AND THE DECODER GETS 2.048. See
             *  vibe_dab_resample.h: RTL-SDRs are unreliable at 2.048 — every OpenWebRX DAB
             *  profile Stuart has is 2.4, SDRangel had the same trouble, and our own XCover
             *  delivered only 94% of the samples at 2.048 while the Pi managed 100%. 2.4 MS/s is
             *  an exact 28.8/12 division of the crystal. The 64/75 ratio is exact, so the symbol
             *  timing cannot drift. */
            const float* src = interleaved;
            size_t       n   = nSamples;
            if (std::fabs(rfRate_ - 2400000.0) < 1000.0) {
                rsOut_.clear();
                rs_.process(interleaved, nSamples, rsOut_);
                src = rsOut_.data();
                n   = rsOut_.size() / 2;
            }
            const size_t base = iq_.size();
            iq_.resize(base + n);
            for (size_t i = 0; i < n; ++i)
                iq_[base + i] = { src[2 * i], src[2 * i + 1] };
            /* ★ Bound the backlog here, where the producer is: if the worker cannot keep up, the
             *  OLDEST samples go. Audio minutes late is worse than a gap, and now the drop is
             *  ours and counted rather than the driver's and silent. */
            const size_t need = size_t(modeI().frameSamples) * 2;
            if (iq_.size() > need * 4) {
                dropped_ += uint32_t(iq_.size() - need * 2);
                iq_.erase(iq_.begin(), iq_.end() - long(need * 2));
            }
        }
        cv_.notify_one();
    }

    /** Stop the decode thread. Safe to call more than once. */
    void stopWorker() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        started_ = false;
    }

private:
    /** The decode loop. Runs on its own thread; holds m_ except where noted. */
    void workerLoop() {
        std::unique_lock<std::mutex> lk(m_);
        const size_t need = size_t(modeI().frameSamples) * 2;
        while (!stop_) {
            if (iq_.size() < need) { cv_.wait(lk); continue; }
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
            /* ★★★ LET GO OF THE LOCK BETWEEN FRAMES, OR AN UNLOCKABLE CHANNEL STARVES EVERYTHING.
             *  ★★★ THE FAULT THIS FIXES, measured. This inner loop holds m_ for as long as there
             *      is a frame's worth of IQ waiting, and json() — the station list and the whole
             *      signal block the client reads — takes the same m_. On a channel that CANNOT
             *      lock, the receiver runs its full re-acquisition scan on every frame, falls
             *      behind real time, and the loop therefore never runs out of input: m_ is held
             *      continuously and the stats stop dead. Tuning 10D produced EXACTLY ONE dab
             *      message and then nothing for 58 seconds, while 11A streamed normally — same
             *      build, same aerial, minutes apart. From outside it looks like "good signal, no
             *      multiplex", which is exactly how Stuart reported it.
             *  ★★ The yield is unconditional and cheap: unlocking and relocking a mutex nobody
             *     else wants costs almost nothing, and when somebody DOES want it they get it
             *     within one frame instead of never. Decoding is unaffected — the loop condition
             *     is re-tested afterwards, and a producer that added more IQ meanwhile is served
             *     on the next pass.
             *  ★ This is the same class of fault as the PCM buffer sharing m_ with the decoder
             *    (see takePcm): one lock covering both the slow work and the thing that reports on
             *    it. The audio path was given its own lock; the reporting path needs the door
             *    opening periodically instead. */
            lk.unlock();
            std::this_thread::yield();
            lk.lock();
            if (stop_) break;
        }   // while (iq_.size() >= need)
        }   // while (!stop_)
    }

public:

    /** Take decoded audio (interleaved stereo, 48 kHz). Returns frames written. */
    /** ★★★ THE PCM BUFFER HAS ITS OWN LOCK, AND THAT IS THE POINT.
     *  ★★★ IT USED TO SHARE m_ WITH THE DECODER, so draining the audio buffer had to wait for
     *      whatever the worker thread was doing — and workerLoop holds m_ across rx_.push(), which
     *      is the entire OFDM demodulation, de-interleave and Viterbi for a frame. Audio delivery
     *      was therefore serialised behind decoding, and no amount of clocking the reader could
     *      help: MEASURED after the 20 ms audio clock went in, the average call spacing fell from
     *      40 ms to 12 ms and there was STILL about one stall per second over 60 ms, because the
     *      clock thread was simply blocked on the mutex.
     *  ★★ Producer and consumer touch only pcm_ here, so it needs only pcm_'s lock. Order is
     *     m_ THEN pm_ (the worker already holds m_ when it fills); nothing ever takes m_ while
     *     holding pm_, so the two cannot deadlock. */
    size_t takePcm(float* out, size_t maxFrames) {
        std::lock_guard<std::mutex> lk(pm_);
        size_t n = 0;
        while (n < maxFrames && pcm_.size() >= 2) {
            out[2 * n]     = pcm_.front(); pcm_.pop_front();
            out[2 * n + 1] = pcm_.front(); pcm_.pop_front();
            ++n;
        }
        return n;
    }
    size_t pcmAvailable() { std::lock_guard<std::mutex> lk(pm_); return pcm_.size() / 2; }

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
    /** ★★★ THE CORE CHANNEL COUNT — 1 FOR PARAMETRIC STEREO, WHICH IS WHAT A DECODER MUST BE
     *  TOLD. aacChannels() is what comes OUT (PS reconstructs a second channel from a mono core);
     *  the AudioSpecificConfig and the ADTS header describe what goes IN. This file already wrote
     *  the right number into the ADTS and the frame header sent the other one, so the browser
     *  built its config from "2 channels" over a bitstream containing one — and Apple's decoder
     *  refused every frame: "InternalAudioDecoderCocoa decoding failed", 0 good frames, for as
     *  long as DAB+ has existed on Safari. Chromium is lenient about it; Apple is not. */
    int aacCoreChannels() { std::lock_guard<std::mutex> lk(m_); return aacCoreCh_; }
    /** ★★★ PARAMETRIC STEREO IN USE — HE-AAC v2. The core is mono and the second channel is
     *  reconstructed from side information. A decoder that is not told this decodes the core and
     *  stops: mono, and only the lower half of the spectrum, because SBR is not applied either.
     *  Stuart on Safari once it finally played, 2026-09-06: "the audio sounds wank, really low
     *  quality almost like hold music on a phone call", against Edge on the same service sounding
     *  "like proper musical audio and in stereo". Chromium infers both from the payload; Apple
     *  signals nothing it was not told. */
    bool aacParametricStereo() { std::lock_guard<std::mutex> lk(m_); return aacPs_; }

    /** The station list and the signal block, as the web client wants them. */
    std::string json() {
        std::lock_guard<std::mutex> lk(m_);
        const Ensemble& e = rx_.ensemble();
        const DabStats& s = rx_.stats();
        std::string j = "{\"type\":\"dab\"";
        /* ★★★ 2048, AND THE GUARD BELOW, BECAUSE 512 SILENTLY TRUNCATED THE WHOLE BLOCK.
         *
         *  ★★★ THIS WAS "DAB IS REALLY FUCKED". Every diagnostic field added during the DAB hunt
         *      — mp2HdrBad, mp2NoSync, noSyncGaps, aacRateHz, pcmPushed, pcmFilled and the rest —
         *      pushed this fragment past 512 bytes. snprintf then cut it MID-FIELD:
         *          ..."rfRateHz":2400000,"l,"locked":false,...
         *      which is not JSON, so every client discarded the message entirely and showed
         *      nothing: no station list, no lock flag, no signal analysis. From the outside that
         *      is a receiver that "never locks on even though signal and gain seem correct".
         *
         *  ★★★ AND IT IS INTERMITTENT BY CONSTRUCTION, which is why it read as flaky rather than
         *      broken. The length depends on the ensemble LABEL and on how wide the numbers have
         *      grown — so a short label fits and a long one does not, and the same multiplex
         *      works until samplesIn gains a digit. "Showed all of its stations then promptly
         *      cleared them all away" is exactly that boundary being crossed.
         *
         *  ★★ THE GUARD MATTERS MORE THAN THE SIZE. A buffer can always be outgrown again; what
         *     must never happen again is doing it SILENTLY. snprintf returns the length it WANTED,
         *     so truncation is detectable — and when it happens we now emit a short, VALID object
         *     saying so, rather than a fragment that looks like a dead receiver. */
        char b[2048];
        /* ★ `rfCentreHz` is what the RADIO is actually on, not what we asked for. They diverged on
         *  the live Pi — DAB reported 12B while the dongle sat on 96.6 MHz — and without both
         *  numbers side by side that is indistinguishable from "DAB does not decode here". */
        const int nb = snprintf(b, sizeof b,
                 ",\"channel\":\"%s\",\"centreHz\":%u,\"scf\":[%u,%u,%u,%u,%u],\"mp2Crc\":%u,\"mp2In\":%u,\"mp2Bad\":%u,\"mp2Out\":%u,\"mp2Concealed\":%u,\"scfClamped\":%u,\"mp2HdrBad\":%u,\"mp2CrcBad\":%u,\"mp2NoSync\":%u,\"mp2TooLong\":%u,\"lsfOrphans\":%u,\"noSyncGaps\":\"%s\",\"aacDecoded\":%u,\"aacServerSide\":%s,\"aacRateHz\":%d,\"aacCh\":%d,\"aacPcmPerAu\":%u,\"pcmPushed\":%llu,\"pcmAvail\":%u,\"pcmFilled\":%u,\"syncJumps\":%u,\"samplesIn\":%llu,\"pushCalls\":%u,\"pushOk\":%u,\"dropped\":%u,\"sfFrames\":%u,\"sfBadLen\":%u,\"sfTried\":%u,\"sfOk\":%u,\"aus\":%u,\"rfCentreHz\":%.0f,\"rfRateHz\":%.0f,\"label\":\"%s\",\"eid\":%u",
                 channel_ >= 0 ? kBandIII[channel_].name : "", centreHz(), scfChecked_, scfOk_[0], scfOk_[1], scfOk_[2], scfOk_[3], mp2WithCrc_, mp2In_, mp2Bad_, mp2Out_, mp2Concealed_, mp2_.scfClamped(), mp2_.hdrBad(), mp2_.crcBad(), mp2_.hdrNoSync(), mp2_.hdrTooLong(), lsfOrphans_, mp2_.noSyncGaps().c_str(), aacDecoded_, aac_.available() ? "true" : "false", aac_.rateHz(), aac_.channels(), aacPcmPerAu_, (unsigned long long)pcmPushed_, (unsigned)(pcm_.size()/2), pcmFilled_, syncJumps_, (unsigned long long)samplesIn_, pushCalls_, pushOk_, dropped_, sfFrames_, sfBadLen_, sfTried_, sfOk_, ausOut_, rfCentre_, rfRate_,
                 esc(e.label).c_str(), unsigned(e.eid));
        j += b;
        const int nb2 = snprintf(b, sizeof b,
                 ",\"locked\":%s,\"nullDepthDb\":%.1f,\"offsetHz\":%.0f,\"offsetPpm\":%.2f"
                 ",\"carrierShift\":%d,\"prs\":%.3f,\"prsRef\":%.3f,\"prsRatio\":%.3f,\"erased\":%d,\"rsFixed\":%u,\"rsLost\":%u,\"sfInvalid\":%u,\"sfFireBad\":%u,\"dls\":\"%s\",\"dlsChanges\":%u,\"dlsCrcOk\":%u,\"dlsCrcFail\":%u,\"padFrames\":%u,\"xNone\":%u,\"xShort\":%u,\"xVar\":%u,\"xApp2\":%u,\"xApp3\":%u,\"xApp1\":%u,\"xApp12\":%u,\"fibOk\":%d,\"fibTotal\":%d,\"fibRate\":%.3f"
                 ",\"frames\":%d,\"sid\":%u,\"bitrate\":%d,\"protection\":\"%s\"",
                 s.locked ? "true" : "false", s.nullDepthDb, s.freqOffsetHz, s.freqOffsetPpm,
                 s.intOffsetCarriers, s.prsCorrelation, s.prsRef, s.prsRef > 0.0f ? s.prsCorrelation / s.prsRef : 0.0f, s.erasedFrames, rsCorrected_, rsUncorrected_, sfInvalid_, sfFireBad_, esc(pad_.dls().label().text).c_str(), pad_.dls().label().changes, pad_.dls().crcOk(), pad_.dls().crcFail(), pad_.framesSeen(), pad_.xIndCount(0), pad_.xIndCount(1), pad_.xIndCount(2), pad_.appSeen(2), pad_.appSeen(3), pad_.appSeen(1), pad_.appSeen(12), s.fibsOk, s.fibsTotal, s.fibRate,
                 s.framesSeen, unsigned(sid_), rx_.serviceBitrate(),
                 rx_.uepProf().valid ? "UEP" : (rx_.profile().valid ? "EEP" : ""));
        /* ★ snprintf returns the length it WANTED, so a truncation is knowable. Emit a valid
         *  object that SAYS the block was too long rather than a fragment that parses as nothing. */
        /* ★ BOTH fragments. The second one carries the DLS text (up to 128 bytes) and was never
         *  checked — the same silent cut, one line further down. */
        if (nb < 0 || size_t(nb) >= sizeof b || nb2 < 0 || size_t(nb2) >= sizeof b) {
            char sb[192];
            snprintf(sb, sizeof sb,
                     "{\"type\":\"dab\",\"channel\":\"%s\",\"truncated\":%d,\"locked\":%s}",
                     channel_ >= 0 ? kBandIII[channel_].name : "", nb2 < 0 || size_t(nb2) >= sizeof b ? nb2 : nb,
                     s.locked ? "true" : "false");
            return std::string(sb);
        }
        j += b;
        /* ★★★ APPENDED SEPARATELY, NOT ADDED TO THE FORMAT STRING ABOVE. That snprintf carries
         *  about forty arguments, and twice tonight I added a field to it and mis-aligned the
         *  list — snprintf then produced MALFORMED JSON, every client silently discarded it, and
         *  the symptom was indistinguishable from a dead decoder. A separate, short append cannot
         *  drift out of step with its own arguments. */
        {
            char rb[64];
            snprintf(rb, sizeof rb, ",\"reacquires\":%d,\"preTuneDropped\":%u",
                     s.reacquires, preTuneDropped_);
            j += rb;
        }
        {
            /* ★ THE PLAYING SERVICE'S CODEC, AS DECODED — not as the FIC promised. For DAB+ the
             *  super frame header says the core rate, SBR and PS; for Layer II the frame header
             *  says the sample rate and channel mode. "DAB+ 32 kbit/s 32 kHz HE-AAC v2 Parametric
             *  Stereo" is what a DXer wants to read, and it is only knowable here. */
            char cb[200];
            if (sid_ && rx_.selectedType() == 63 && afmt_.outputRateHz > 0) {
                const char* prof = afmt_.sbr ? (aacPs_ ? "HE-AAC v2" : "HE-AAC v1") : "AAC-LC";
                const char* chan = aacPs_ ? "Parametric Stereo" : (aacCoreCh_ == 2 ? "Stereo" : "Mono");
                snprintf(cb, sizeof cb, ",\"codecDetail\":\"DAB+ %d kbit/s %d kHz %s %s\",\"audioRateHz\":%d,\"coreRateHz\":%d,\"sbr\":%s,\"ps\":%s,\"audioCh\":%d",
                         rx_.serviceBitrate(), afmt_.outputRateHz / 1000, prof, chan,
                         afmt_.outputRateHz, afmt_.coreRateHz, afmt_.sbr ? "true" : "false",
                         aacPs_ ? "true" : "false", aacOutCh_);
                j += cb;
            } else if (sid_ && rx_.selectedType() == 0 && mp2_.info().valid) {
                const auto& mi = mp2_.info();
                static const char* modes[4] = { "Stereo", "Joint Stereo", "Dual Channel", "Mono" };
                snprintf(cb, sizeof cb, ",\"codecDetail\":\"DAB %d kbit/s %d kHz MPEG-%s Layer II %s\",\"audioRateHz\":%d,\"audioCh\":%d",
                         mi.bitrateKbps, mi.sampleRateHz / 1000, mi.lsf ? "2 LSF" : "1", modes[mi.mode & 3],
                         mi.sampleRateHz, mi.channels);
                j += cb;
            }
        }
        {
            /* ★ What the FIC has said about the ensemble itself: ECC (with the EId, the world-unique
             *  name of this multiplex), the CIF count the TII decoder keys on, and whether the MCI
             *  is complete — the difference between "reading" and "read" (EN 300 401 6.4.2). */
            char eb[96];
            snprintf(eb, sizeof eb, ",\"ecc\":%d,\"cif\":%d,\"mci\":%s,\"nsvc\":%d",
                     e.ecc, e.cifCount, e.mciComplete() ? "true" : "false", e.serviceCount);
            j += eb;
        }
        j += ",\"services\":[";
        bool first = true;
        for (const auto& kv : e.services) {
            const Service& sv = kv.second;
            /* ★★★ TS 103 176 6.2.2: the list shows what the receiver can DECODE AND PRESENT, and
             *  6.3.3: a service with incomplete MCI is ignored. So: audio services (a data service
             *  would be a row that plays nothing), with a label and a sub-channel we know, and no
             *  CA on the component (EN 300 401 6.3.1: "shall not decode"). A row that cannot play
             *  is the AGENTS.md dead control in list form. */
            if (sv.isData || !sv.complete(e.subChannels)) continue;
            const ServiceComponent* pc = sv.primaryComponent();
            if (!pc || pc->tmid != 0 || pc->subChId < 0 || pc->ca) continue;
            if (!first) j += ',';
            first = false;
            /* ★ Stuart, 2026-09-07: "codec info needs to be more than DAB+ 32Kb/s" — the FIC knows
             *  the capacity and protection of every service before any of them is played, so the
             *  list carries bit rate, protection set/level/code rate and the CU range per row. */
            const SubChannel& sc = e.subChannels.at(pc->subChId);
            SubChannelInfo si = subChannelInfo(sc);
            int uepLevel = 0;
            if (!sc.eep && sc.protLevel >= 0 && sc.protLevel < 64) {
                si.bitrateKbps = kUepIndex[sc.protLevel].bitrateKbps;
                uepLevel       = kUepIndex[sc.protLevel].protLevel;
            }
            char prot[40];
            if (sc.eep) snprintf(prot, sizeof prot, "%s %d (%s)", si.set, si.level, si.codeRate);
            else        snprintf(prot, sizeof prot, "UEP %d", uepLevel);
            const int sizeCu = sc.eep ? sc.sizeCu : (sc.protLevel < 64 ? kUepIndex[sc.protLevel].sizeCu : 0);
            snprintf(b, sizeof b, "{\"sid\":%u,\"label\":\"%s\",\"short\":\"%s\",\"codec\":\"%s\",\"subch\":%d,\"pty\":%d,\"slides\":%s,\"kbps\":%d,\"prot\":\"%s\",\"cuStart\":%d,\"cuSize\":%d,\"scids\":%d,\"ecc\":%d}",
                     unsigned(kv.first), esc(sv.label).c_str(), esc(sv.shortLabel).c_str(),
                     pc->scType == 63 ? "DAB+" : pc->scType == 0 ? "MP2" : "?", pc->subChId,
                     sv.pty, sv.hasSlideshow() ? "true" : "false",
                     si.bitrateKbps, prot, sc.startCu, sizeCu, pc->scids, sv.ecc >= 0 ? sv.ecc : e.ecc);
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
        for (const auto& fRaw : frames) {
            /* ★★★ A 24 kHz (LSF) LAYER II FRAME SPANS TWO DAB LOGICAL FRAMES, AND WE WERE
             *  THROWING EVERY ONE OF THEM AWAY. Layer II is 1152 samples per frame however it is
             *  clocked: at 48 kHz that is 24 ms, exactly one DAB logical frame — but at 24 kHz it
             *  is 48 ms, so the audio frame arrives as TWO 192-byte halves and must be joined
             *  before it means anything.
             *  ★★★ MEASURED, on captured air: talkSPORT on 11D is `FF F4 84 CC` — MPEG-2 LSF,
             *      64 kbit/s, 24 kHz — so mp2Header computes frameBytes = 144*64000/24000 = 384
             *      against the 192 bytes in hand, and `frameBytes > n` rejected the lot. 11D and
             *      SDL decoded NOTHING while their FIC read a perfect 1.000, which is precisely
             *      the shape that says "wrong bits", not "weak signal". With the halves joined:
             *      100% failures -> 1.0%.
             *  ★ 12B never showed it: every BBC Layer II service is 48 kHz, so the first mux we
             *    tested agreed with the bug — the same trap as the mono/stereo chipmunks.
             *  ★ THE ORPHAN GUARD MATTERS. If the second half is lost (an erased frame, a dropped
             *    buffer), the held half would be joined to the NEXT service frame for ever after,
             *    turning one lost frame into permanent corruption. A chunk that carries its own
             *    valid over-length header is a first half, so the one being held was orphaned:
             *    drop it and start again. */
            std::vector<uint8_t> joined;
            const std::vector<uint8_t>* fp = &fRaw;
            if (rx_.selectedType() == 0) {
                const Mp2Info hi = mp2Header(fRaw.data(), fRaw.size());
                const bool firstHalf = hi.valid && size_t(hi.frameBytes) > fRaw.size();
                if (!lsfPend_.empty() && !firstHalf) {
                    joined = lsfPend_;
                    joined.insert(joined.end(), fRaw.begin(), fRaw.end());
                    lsfPend_.clear();
                    fp = &joined;
                } else if (firstHalf) {
                    if (!lsfPend_.empty()) ++lsfOrphans_;
                    lsfPend_.assign(fRaw.begin(), fRaw.end());
                    continue;
                }
            }
            const std::vector<uint8_t>& f = *fp;
            /* ★★ MP2 WE DECODE OURSELVES — the browser refuses Layer II, measured 2026-09-04, and
             *  MP2's patents have expired so it is the one codec we may implement. DAB+ is handed
             *  onward as ADTS instead; that path links no decoder here. */
            if (rx_.selectedType() != 0) { pumpDabPlus(f); continue; }
            /* ★ DIAGNOSTIC DUMP, off unless VIBE_DAB_DUMP names a file. Brute-forcing the
             *  ScF-CRC's position and bit selection one five-minute deploy at a time is the wrong
             *  way round; with real frames on disk the same search takes seconds and can try
             *  every hypothesis at once. Removed once the parameters are known. */
            if (const char* dp = std::getenv("VIBE_DAB_DUMP")) {
                static FILE* fp = std::fopen(dp, "wb");
                static int   left = 400;
                if (fp && left > 0) {
                    const uint32_t n32 = uint32_t(f.size());
                    std::fwrite(&n32, 4, 1, fp);
                    std::fwrite(f.data(), 1, f.size(), fp);
                    if (--left == 0) { std::fflush(fp); std::fclose(fp); fp = nullptr; }
                }
            }
            std::vector<float> out;
            ++mp2In_;
            if (mp2_.decode(f.data(), f.size(), out) <= 0) {
                ++mp2Bad_;
                /* ★ The squeal guard judges a scale factor against what this sub-band was doing a
                 *  moment ago. A refused frame ends that continuity — the next frame we accept may
                 *  be 24 ms or a second later, in a different passage — so drop the reference
                 *  rather than fight the first frames back in with it. */
                mp2_.resetScfHistory();
                continue;
            }
            ++mp2Out_;
            /* ★★★ CONCEAL THE SQUEAL. THE MPEG CRC DOES NOT COVER THE SCALEFACTORS — it spans the
             *  header, the allocation and the scfsi only (see mp2Crc16) — so a frame whose
             *  SCALEFACTORS were corrupted passes every check we make and decodes into full-scale
             *  noise. Scalefactors are logarithmic gains: one wrong index is tens of dB, which is
             *  why the failure is a SQUEAL rather than the bubbling mud of ordinary bit errors,
             *  and why Stuart has described it that way from the very first report.
             *  ★★★ MEASURED on 60 s of captured air (dab-offline): median frame RMS 0.145, p95
             *      0.218 — and a peak of 2.927, which valid Layer II output cannot produce. 15
             *      frames of 2439 arrive at full scale. Those fifteen are the squeals.
             *  ★★★ THIS IS WHAT THE REFERENCE DOES. Stuart, comparing the SAME dongle and aerial
             *      on OpenWebRX minutes earlier: "no awful squeal on OWRX, a little hiccup of
             *      silence ... now crystal clear", and "I can listen to OWRX for extended periods
             *      of time, I could not ours." A receiver that cannot decode a frame must say
             *      nothing; it must never say something loud.
             *  ★★ CONCEALMENT, NOT A CURE, and it is not pretending otherwise — the counter is
             *     published so the underlying error rate stays visible. OWRX stalls far less
             *     often than we squeal, which is the real gap and is still being worked.
             *  ★ Two tests, both from the measurement above: a peak no valid frame can reach, and
             *    a level wildly out of line with this service's own running average. The average
             *    is seeded from the first good frames and moves slowly, so a genuinely loud
             *    passage cannot be silenced by it. */
            {
                float pk = 0.0f; double sq = 0.0;
                for (float v : out) { const float a = std::fabs(v); if (a > pk) pk = a; sq += double(v) * v; }
                const double rms = out.empty() ? 0.0 : std::sqrt(sq / double(out.size()));
                const bool impossible = (pk > 1.5f);
                const bool wayOut     = (rmsRef_ > 0.0 && rms > rmsRef_ * 6.0);
                if (impossible || wayOut) {
                    ++mp2Concealed_;
                    std::fill(out.begin(), out.end(), 0.0f);       // a stall, not a squeal
                } else if (rms > 0.0) {
                    rmsRef_ = rmsRef_ > 0.0 ? rmsRef_ * 0.98 + rms * 0.02 : rms;
                }
            }
            if (mp2_.lastHadCrc()) ++mp2WithCrc_;
            {   // ★ Which ScF-CRC convention matches on air — see Mp2Decoder::tallyScfCrc.
                const auto& t = mp2_.scfCrc();
                scfChecked_ = t.checked;
                for (int v = 0; v < 4; ++v) scfOk_[v] = t.ok[v];
            }
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
            /* ★★★ THE DYNAMIC LABEL — DAB'S "NOW PLAYING" — LIVES IN THE TAIL OF THIS FRAME.
             *  EN 300 401 clause 7.4: [ ... audio ... ][ X-PAD ][ ScF-CRC ][ F-PAD (2) ]. F-PAD is
             *  the last two bytes; the scale factor CRC sits between it and the X-PAD, so it has
             *  to be lifted out before the PAD reader sees the field or the indicator list is read
             *  out of the wrong bytes.
             *  ★★ THE ScF-CRC LENGTH RULE (TS 103 466, as dablin and welle.io apply it): four
             *     bytes, except two for MPEG-1 below 56 kbit/s mono / 112 kbit/s stereo. LSF (24
             *     kHz) frames always carry four. The 2026-09-05 conclusion that "this air carries
             *     no ScF-CRC" was drawn while the X-PAD bytes were being read in the wrong order
             *     (see vibe_dab_pad.h), so it is withdrawn and the rule is applied — and the DLS
             *     CRC-16 is the oracle: a wrong length puts the indicator list in the wrong place
             *     and crcOk stays at zero, which is VISIBLE. VIBE_DAB_SCFCRC_LEN still overrides.
             *  ★ The whole tail goes to the reader, not 48 bytes: a variable X-PAD field can be
             *    four sub-fields of 48 plus its list, and the reader sizes the field itself. */
            {
                const auto& mi2 = mp2_.info();
                static const int scfCrcEnv = std::getenv("VIBE_DAB_SCFCRC_LEN")
                                           ? atoi(std::getenv("VIBE_DAB_SCFCRC_LEN")) : -1;
                size_t scfCrcLen = 4;
                if (!mi2.lsf && mi2.bitrateKbps < (mi2.channels == 1 ? 56 : 112)) scfCrcLen = 2;
                if (scfCrcEnv >= 0) scfCrcLen = size_t(scfCrcEnv);
                if (f.size() > scfCrcLen + 2) {
                    const size_t fpadAt = f.size() - 2;
                    const size_t xEnd   = fpadAt - scfCrcLen;      // X-PAD ends before the ScF-CRC
                    const size_t take   = xEnd < 200 ? xEnd : 200; // 4 x 48 + a 4-byte list
                    std::vector<uint8_t> win;
                    win.reserve(take + 2);
                    win.insert(win.end(), f.begin() + long(xEnd - take), f.begin() + long(xEnd));
                    win.push_back(f[fpadAt]); win.push_back(f[fpadAt + 1]);
                    pad_.feed(win.data(), win.size());
                }
            }
            const auto& mi = mp2_.info();
            pushPcm48Stereo(out.data(), out.size(),
                            mi.channels > 0 ? mi.channels : 2,
                            mi.sampleRateHz > 0 ? mi.sampleRateHz : int(kAudioRateHz));
        }
        while (pcm_.size() > size_t(kAudioRateHz) * 2 * 2) pcm_.pop_front();   // ~2 s of slack
    }

    /** ★★★ THE ONE PLACE ANYTHING BECOMES 48 kHz STEREO. MP2 and DAB+ both arrive at their own
     *  rate and channel count, and takePcm() reads 48 kHz stereo pairs — so BOTH need upmixing and
     *  resampling, and having written that twice is how the chipmunks got in the first time. A
     *  MONO service read as L/R pairs plays at 2x; a 24 kHz service plays at 2x again; DAB+ adds
     *  its own version of the same trap, because SBR and parametric stereo mean the decoder's
     *  output rate and channel count are not the ones in the frame header.
     *  ★ Linear interpolation on purpose: the ratios are exact small integers (48/24 = 2,
     *    48/32 = 1.5), so this is not the place to spend an FIR — and a wrong-speed stream is not
     *    a fidelity problem to be tuned, it is a bug to be removed. */
    /** ★ A new programme, or a new rate, starts a new stream — drop the carried sample and phase
     *  rather than splicing two different pieces of audio together. */
    void resampleReset() { rsPrimed_ = false; rsPhase_ = 0.0; rsPrevL_ = rsPrevR_ = 0.0f; rsRate_ = 0; }

    void pushPcm48Stereo(const float* data, size_t nSamples, int nch, int srHz) {
        if (!data || nch <= 0 || srHz <= 0) return;
        std::lock_guard<std::mutex> plk(pm_);        // ★ see takePcm — pcm_ has its own lock
        // ★ The carried state belongs to ONE rate. A service that changes it starts again.
        if (srHz != rsRate_) { resampleReset(); rsRate_ = srHz; }
        const size_t frames = nSamples / size_t(nch);
        if (!frames) return;
        const size_t before = pcm_.size();
        if (srHz == int(kAudioRateHz)) {
            for (size_t i = 0; i < frames; ++i) {
                const float l = data[i * size_t(nch)];
                const float r = nch == 1 ? l : data[i * size_t(nch) + 1];
                pcm_.push_back(l); pcm_.push_back(r);
            }
        } else {
            /* ★★★ THE PHASE MUST SURVIVE THE CALL, AND THIS IS WHY.
             *
             *  ★★★ THE FAULT IT FIXES. This used to run `for (double t = 0; t <= span; t += step)`
             *      — starting at zero every time it was called. Fine for one long buffer; wrong
             *      for a stream. DAB+ arrives one ACCESS UNIT at a time, 60 ms of 32 kHz audio per
             *      call, so the resampler restarted about seventeen times a second. At every join
             *      it (a) threw away the fractional phase it had reached and (b) never
             *      interpolated ACROSS the boundary, because the last input sample of one call and
             *      the first of the next were never seen together. Seventeen discontinuities a
             *      second is a stutter.
             *  ★★★ AND IT LOOKS LIKE NOTHING ELSE IS WRONG, WHICH IS THE TRAP. Stuart: "its odd I
             *      am getting stutters but the FIB pass rate is 100%". It was: FIB 12/12, lock
             *      solid, offset -19 Hz, every access unit decoding. The demodulator was never
             *      involved — the damage is done after the audio is already perfect, by the last
             *      arithmetic before it reaches the buffer.
             *  ★★ MP2 NEVER SHOWED IT because 48 kHz services take the fast path above and never
             *     resample at all; only a rate CHANGE reaches here, and DAB+ at 32 kHz reaches it
             *     on every single access unit.
             *  ★ prev + phase are members, reset only when the RATE changes or the programme does
             *    (see resampleReset) — a new service is a new stream and joining it to the old
             *    one's last sample would be one deliberate click instead of many accidental ones. */
            const double step = double(srHz) / double(kAudioRateHz);
            for (size_t i = 0; i < frames; ++i) {
                const float l = data[i * size_t(nch)];
                const float r = nch >= 2 ? data[i * size_t(nch) + 1] : l;
                if (!rsPrimed_) { rsPrevL_ = l; rsPrevR_ = r; rsPhase_ = 0.0; rsPrimed_ = true; }
                while (rsPhase_ < 1.0) {
                    const float ph = float(rsPhase_);
                    pcm_.push_back(rsPrevL_ + (l - rsPrevL_) * ph);
                    pcm_.push_back(rsPrevR_ + (r - rsPrevR_) * ph);
                    rsPhase_ += step;
                }
                rsPhase_ -= 1.0;
                rsPrevL_ = l; rsPrevR_ = r;
            }
        }
        /* ★★★ FRAMES ACTUALLY DELIVERED, COUNTED BEFORE THE TRIM. takePcm() reads 48 kHz stereo
         *  pairs, so this must run at exactly 48000 a second in real time. Fewer and the listener
         *  starves — audio that is perfectly CLEAR but slow, which is precisely how a rate error
         *  presents once the decoding itself is right. There is no other number that settles it:
         *  the decoder's reported rate, the AU count and the byte rate can all look correct while
         *  this one is wrong. */
        pcmPushed_ += uint64_t((pcm_.size() - before) / 2);
        while (pcm_.size() > size_t(kAudioRateHz) * 2 * 2) pcm_.pop_front();   // ~2 s of slack
    }

    /** ★★★ KEEP THE CLOCK HONEST — A LOST SUPER FRAME MUST COST SILENCE, NOT TIME.
     *
     *  ★★★ THE FAULT THIS FIXES. A super frame that fails its firecode produced NOTHING: no audio
     *      and no silence. The stream simply got shorter, so the listener starved a little more
     *      with every loss and the audio ran permanently behind. MEASURED on 11A: 248 good super
     *      frames of 298 tried delivered 43192 PCM frames a second against the 48000 takePcm()
     *      reads, with the buffer sitting at empty — audio that is perfectly CLEAR and slow.
     *      Stuart, twice, on exactly this: "still slow in safari but the audio is clear".
     *  ★★★ AND SILENCE IS THE ANSWER HE ASKED FOR: "a slight bubbling mud or very slight silence
     *      is fine but squeals is absolutely not." A gap keeps the timeline; a shortfall destroys
     *      it, and a destroyed timeline is heard on every second of the programme, not just the
     *      damaged ones.
     *
     *  ★★ THE TOLERANCE IS THE WHOLE DESIGN. Audio arrives in 120 ms bursts — five logical frames
     *     are held, then one super frame's worth is decoded at once — so pcmPushed_ legitimately
     *     lags pcmOwed_ by up to a super frame, and an AAC decoder runs a frame or two behind that
     *     again. Topping up on every frame would inject silence into a stream that was merely
     *     waiting, and then the real audio would arrive on top of it: the same total, twice the
     *     length. So nothing is filled until the deficit EXCEEDS the lag a healthy stream shows,
     *     and only the excess is filled.
     *  ★ Bounded per call: a long outage should come back as a gap, not as a wall of silence
     *    delivered in one go and then played out for seconds after the signal returned. */
    void holdTimeline() {
        std::lock_guard<std::mutex> plk(pm_);        // ★ see takePcm
        static constexpr uint64_t kLagFrames  = uint64_t(kAudioRateHz) * 240 / 1000;  // 240 ms
        static constexpr uint64_t kMaxFillPer = uint64_t(kAudioRateHz) / 2;           // 0.5 s
        if (pcmOwed_ <= pcmPushed_ + kLagFrames) return;
        uint64_t need = pcmOwed_ - pcmPushed_ - kLagFrames;
        if (need > kMaxFillPer) need = kMaxFillPer;
        for (uint64_t i = 0; i < need; ++i) { pcm_.push_back(0.0f); pcm_.push_back(0.0f); }
        pcmPushed_ += need;
        pcmFilled_ += uint32_t(need);
        while (pcm_.size() > size_t(kAudioRateHz) * 2 * 2) pcm_.pop_front();
    }

    /** One DAB+ logical frame: hold five, test the firecode, reframe the AUs it contains. */
    void pumpDabPlus(const std::vector<uint8_t>& frame) {
        /* ★ One logical frame is 24 ms of programme, whatever happens to it after this point —
         *  that is what makes the timeline knowable. 48000 x 0.024 = 1152 frames owed per call. */
        pcmOwed_ += uint64_t(kAudioRateHz) * 24 / 1000;
        struct Hold { DabService* s; ~Hold() { s->holdTimeline(); } } hold{this};
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
        /* ★★★ COUNT WHERE THE SUPER FRAME DIED, because "sfOk vs sfTried" cannot tell you whether
         *  more error correction would help or whether the signal is simply too weak. Reed-Solomon
         *  running out of correction power and a header whose firecode failed are different
         *  faults with different fixes, and both were invisible: rsCorrected and rsUncorrected
         *  were computed on every super frame and read by NOTHING. */
        rsCorrected_   += uint32_t(s.rsCorrected   < 0 ? 0 : s.rsCorrected);
        rsUncorrected_ += uint32_t(s.rsUncorrected < 0 ? 0 : s.rsUncorrected);
        if (!s.valid)       ++sfInvalid_;
        if (!s.firecodeOk)  ++sfFireBad_;
        if (!s.valid || !s.firecodeOk) { sf_.pop_front(); return; }
        sf_.clear();
        ++sfOk_;

        afmt_       = s.fmt;
        aacCoreCh_  = s.stereo ? 2 : 1;
        aacOutCh_   = (s.stereo || s.ps) ? 2 : 1;
        aacPs_      = s.ps;      // ★ mono core, stereo out — the decoder must be TOLD, see below
        /* ★★★ DECODE HERE IF THE SERVER'S OS CAN, AND ONLY PUT ADTS ON THE WIRE IF IT CANNOT.
         *  ★★★ THIS IS WHAT MAKES DAB+ EXACTLY WHAT MP2 ALREADY IS. Decoded here, it goes out as
         *      PCM through the ordinary audio path — Opus or uncompressed, whatever the listener
         *      negotiated — so format 4 never reaches a client and the whole browser codec problem
         *      stops existing. Safari playing "split seconds" of DAB+, the 960-vs-1024 framing,
         *      the mp4 timescale arithmetic, the parametric-stereo signalling: all of it is
         *      browser-side handling of a format we no longer send.
         *  ★★★ AND IT IS THE APPS THAT GAIN MOST. iOS and Android have had NO DAB+ audio work done
         *      at all; client-side decoding meant writing this twice more, with the same class of
         *      bugs each time. They now need nothing.
         *  ★ We still ship no AAC decoder — see vibe_dab_aacdec.h. This calls the platform's,
         *    which is the same posture as relying on the browser's.
         *  ★ The ADTS reframing is unchanged and still the single description of a DAB+ access
         *    unit; the decoder is fed the very same bytes the wire used to carry. */
        for (const auto& au : s.aus) {
            /* ★★★ THE PAD COMES OUT OF THE ACCESS UNIT HERE, before the decoder (which discards
             *  it) sees the bytes. Until 2026-09-07 this call did not exist and DAB+ services had
             *  no "now playing" at all, while the header of vibe_dab_pad.h said they did. */
            pad_.feedAccessUnit(au.data(), au.size());
            std::vector<uint8_t> pkt = toAdts(au.data(), au.size(), s.fmt, aacCoreCh_);
            if (pkt.empty()) continue;
            ++ausOut_;
            if (aac_.available()) {
                AacPcm dec;
                if (aac_.decode(pkt.data(), pkt.size(), dec)) {
                    if (!dec.interleaved.empty()) {
                        /* ★ Frames the decoder actually returned for this access unit. A DAB+ AU
                         *  is a fixed 1024 samples at the decoder's OUTPUT rate for HE-AAC, so
                         *  this number and the reported rate must agree — and if they do not, the
                         *  audio plays at the wrong SPEED while sounding perfectly clean. */
                        if (dec.channels > 0)
                            aacPcmPerAu_ = uint32_t(dec.interleaved.size() / size_t(dec.channels));
                        pushPcm48Stereo(dec.interleaved.data(), dec.interleaved.size(),
                                        dec.channels, dec.rateHz);
                    }
                    ++aacDecoded_;
                    continue;
                }
                /* ★ The decoder failed and said so. Fall through and hand the client the ADTS, so
                 *  a server whose decoder dies mid-programme degrades to the old behaviour rather
                 *  than to silence. */
            }
            adts_.push_back(std::move(pkt));
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
    std::deque<std::vector<uint8_t>> adts_;    ///< reframed AUs, for a server with no decoder
    AacDecoder aac_;                           ///< the PLATFORM's decoder — we ship none
    uint32_t   aacDecoded_ = 0;
    uint32_t   aacPcmPerAu_ = 0;
    uint64_t   pcmPushed_ = 0;
    uint64_t   pcmOwed_   = 0;     ///< 48 kHz frames the programme clock says we should have sent
    uint32_t   pcmFilled_ = 0;
    uint32_t   rsCorrected_ = 0, rsUncorrected_ = 0;  ///< Reed-Solomon: bytes fixed / codewords lost
    uint32_t   sfInvalid_ = 0, sfFireBad_ = 0;
    size_t     settleDrop_ = 0;      ///< IQ samples still to discard after a retune
    uint32_t   preTuneDropped_ = 0;  ///< how many were discarded, ever — published
    PadReader  pad_;               ///< dynamic label (and, later, MOT slideshow)        ///< and WHY a super frame was thrown away
    /** ★ Carried across calls so the 32 kHz -> 48 kHz conversion is ONE continuous stream rather
     *  than one restart per access unit. See pushPcm48Stereo. */
    double     rsPhase_ = 0.0;
    float      rsPrevL_ = 0.0f, rsPrevR_ = 0.0f;
    bool       rsPrimed_ = false;
    int        rsRate_ = 0;     ///< of those, how many were silence covering a lost super frame     ///< 48 kHz stereo frames delivered, ever   ///< PCM FRAMES the decoder returned for the last AU                ///< AUs turned into PCM here rather than on the client
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
    uint32_t scfChecked_ = 0, scfOk_[4] = {0,0,0,0};
    long     lastAt_ = -1;
    uint32_t syncJumps_ = 0;
    uint64_t samplesIn_ = 0;
    /** ★ Samples seen since the last check, for the capture-rate watchdog in the shim. */
public:
    uint64_t takeSamplesSeen() { std::lock_guard<std::mutex> lk(m_); const uint64_t v = samplesIn_ - seenMark_; seenMark_ = samplesIn_; return v; }
private:
    uint64_t seenMark_ = 0;
    uint32_t pushCalls_ = 0, pushOk_ = 0, dropped_ = 0;
    int  aacCoreCh_ = 2;                       ///< what the ADTS header declares
    int  aacOutCh_  = 2;                       ///< what the decoder will produce (PS -> 2)
    bool aacPs_     = false;                   ///< parametric stereo: mono core, stereo output

    /** ★ The first half of a 24 kHz Layer II frame, waiting for its second. See drainAudio(). */
    std::vector<uint8_t>    lsfPend_;
    unsigned                lsfOrphans_ = 0;
    /** Frames silenced because they decoded into something no valid audio frame can be. */
    uint32_t                mp2Concealed_ = 0;
    double                  rmsRef_ = 0.0;
    Resample24to2048        rs_;
    std::vector<float>      rsOut_;
    std::thread             worker_;
    std::condition_variable cv_;
    bool                    stop_ = false, started_ = false;

    mutable std::mutex m_;
    DabReceiver rx_{kRateHz};
    Mp2Decoder  mp2_;
    std::vector<Cplx> iq_;
    std::deque<float> pcm_;
    /** ★ Guards pcm_ ONLY — never held while taking m_. See takePcm for why it exists. */
    mutable std::mutex pm_;
    int channel_ = -1;
    double rfCentre_ = 0, rfRate_ = 0;
    uint32_t sid_ = 0, want_ = 0;
};

}  // namespace vibedab
