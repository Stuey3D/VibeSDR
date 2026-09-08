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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <chrono>
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
#include "vibe_dab_txdb.h"
#include "vibe_dab_padtap.h"
#include "vibe_dab_packet.h"
#include "vibe_dab_spi.h"
#include "vibe_thread.h"
#include "vibe_dab_epg.h"
#include <dirent.h>
#include <sys/stat.h>

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
        dlsAll_.clear(); scanCursor_ = 0; scanRotatedAt_ = 0;
        for (auto& t : taps_) t.reset();
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
        aacPcmAcc_ = 0.0; aacAuAcc_ = 0; aacAuTotal_ = 0; aacEffRateHz_ = 0; aacRateWarned_ = false; aacPrimed_ = false;
        aacStartedKnown_ = knownRatio_ > 0.0;   // ★ known from the start — no "setting the clock" while the pipe primes
        adts_.clear();
        pad_.reset();      // ★ the label belongs to the old programme
        slide_ = Slide{}; // ★ and so does the picture
        cats_.clear(); slideAlert_ = 0; slideClickUrl_.clear();   // ★ and the gallery it belonged to
        spiSid_ = 0; packets_.reset(); carousel_.reset(); spiLogoRefs_.clear(); spiSeen_ = 0; cacheLoadedEid_ = 0;   // ★ a new multiplex, a new carousel
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
    /** ★ The block's NAME ("11A"), for anything that has to describe the tuning to a person —
     *  the admin page's listener table, which showed "216.928 MHz WFM" for a receiver that was
     *  decoding a multiplex (Stuart, 2026-09-08: "the connection logs dont recognise DAB they
     *  just show WF"). A frequency in Band III with a demod name attached is not a description of
     *  DAB; the block is. */
    const char* channelName() const { return channel_ >= 0 ? kBandIII[channel_].name : ""; }
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
        aacPcmAcc_ = 0.0; aacAuAcc_ = 0; aacPrimed_ = false;   // ★ a restarted pipe primes again — see the count
        aacStartedKnown_ = knownRatio_ > 0.0;
        adts_.clear();
        pad_.reset();
        slide_ = Slide{}; cats_.clear(); slideAlert_ = 0; slideClickUrl_.clear();
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
            if (settleDrop_ > 0) {
                const size_t drop = nSamples < settleDrop_ ? nSamples : settleDrop_;
                settleDrop_ -= drop;
                preTuneDropped_ += uint32_t(drop);
                if (settleDrop_ == 0) {
                    rx_.reset(); iq_.clear(); lsfPend_.clear();
                    mp2_.reset(); aac_.reset(); pad_.reset(); adts_.clear();
                    aacPcmAcc_ = 0.0; aacAuAcc_ = 0; aacPrimed_ = false;
                    { std::lock_guard<std::mutex> plk(pm_); pcm_.clear(); }
                    pcmOwed_ = 0; pcmPushed_ = 0; resampleReset();
                }
                return;
            }
        }
        /* ★★★ THE RATE CONVERTER RUNS OUTSIDE THE DECODER'S MUTEX. It was inside it — and it was
         *  70 % of the receiver's CPU (see vibe_dab_resample.h), so for most of every block the
         *  worker thread could not take IQ and the DSP thread could not hand it over: two
         *  real-time threads serialised on the one lock, on the phone, for nothing. rs_ and
         *  rsOut_ are only ever touched by the thread that calls feed() (the DSP loop; dab-offline's
         *  main), so they need no lock at all. Only the append to iq_ does. */
        const float* src = interleaved;
        size_t       n   = nSamples;
        if (std::fabs(rfRate_ - 2400000.0) < 1000.0) {
            rsOut_.clear();
            rs_.process(interleaved, nSamples, rsOut_);
            src = rsOut_.data();
            n   = rsOut_.size() / 2;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            const size_t base = iq_.size();
            iq_.resize(base + n);
            for (size_t i = 0; i < n; ++i)
                iq_[base + i] = { src[2 * i], src[2 * i + 1] };
            const size_t need = size_t(modeI().frameSamples) * 2;
            if (iq_.size() > need * 4) {
                dropped_ += uint32_t(iq_.size() - need * 2);
                iq_.erase(iq_.begin(), iq_.end() - long(need * 2));
            }
        }
        cv_.notify_one();
    }
    void stopWorker() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        started_ = false;
    }

private:
    /** The decode loop. Runs on its own thread; holds m_ except where noted. */
    void workerLoop() {
        /* ★★★ THE HEAVIEST CONSUMER IN THE PRODUCT, AND IT WAS NAMELESS AND UNPRIORITISED. A
         *  64-state full-frame Viterbi over every CIF, four times per 96 ms frame — the work that
         *  was moved OFF the DSP thread precisely because overrunning there made librtlsdr discard
         *  capture buffers. Moving it to its own thread solved that; leaving that thread at the
         *  default priority just moved the starvation somewhere the counters could not see it.
         *  ★ local_sdr_shim.cpp asserted this already ran at -19 "via vibeAudioThread". It did
         *    not: the helper was defined inside that .cpp and unreachable from this header. */
        vibeAudioThread("vibe-dab");
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
            /* ★★★ TOLD WHAT WAS CONSUMED, NOT RESET. resetSync() here threw the prediction away after
             *  every frame, so FrameSync's tracking branch never ran and each frame was a cold
             *  acquisition against a 6 dB null — the whole reason 9A could not lock (see the note
             *  on FrameSync). Consumption below is unchanged; only the detector keeps its memory. */
            rx_.syncConsumed(modeI().frameSamples);
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
            pumpScan();
            pumpSpi();
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

    /** ★ The transmitter directory (by country) and where THIS receiver is, so a TII code can be
     *  printed as a place and a distance. Either may be absent: then the codes stand alone. */
    void setTxDb(const DabTxDb* db) { std::lock_guard<std::mutex> lk(m_); txdb_ = db; }
    void setReceiverPosition(double lat, double lon) { std::lock_guard<std::mutex> lk(m_); rxLat_ = lat; rxLon_ = lon; }

    /** ★ The receiver's own judgement of the signal, for the gain loop. A DAB gain step is right
     *  when the FIC reads better and the null symbol stands deeper — the figures the demodulator
     *  lives by — not when a narrow carrier stands further above its neighbours. */
    /** ★ The services worth remembering: complete MCI (TS 103 176 6.3.3), audio, labelled. */
    struct LearnRow { uint32_t sid; std::string label; int ecc; int eid; };
    std::vector<LearnRow> learnable() {
        std::vector<LearnRow> out;
        std::lock_guard<std::mutex> lk(m_);
        const Ensemble& e = rx_.ensemble();
        if (!e.mciComplete() || e.eid == 0) return out;
        for (const auto& kv : e.services) {
            const Service& sv = kv.second;
            if (sv.isData || !sv.complete(e.subChannels)) continue;
            out.push_back({ sv.sid, sv.label, sv.ecc >= 0 ? sv.ecc : e.ecc, int(e.eid) });
        }
        return out;
    }
    /** ★★★ THE LEARNT RATIO SURVIVES A RESTART. Stuart, on Saber's Linux box (2026-09-07): "the
     *  station was normal speed then there was a little blip then it went slow again and had to
     *  retrain itself". Inside one process the ratio is applied the moment any decoder restarts,
     *  so a retrain after a blip means the PROCESS restarted (systemd brings it straight back)
     *  and the ratio — a property of that box's ffmpeg, which never changes — went with it. It is
     *  now written next to the bookmarks when it converges and read back at start-up, so a
     *  restart resumes at speed. An exact-frame decoder (Android) never needs the file. */
    /** ★ Where the carousel's files are kept between visits: <dir>/<ecc>-<eid>/<content name>. A
     *  carousel cycles in minutes; a logo seen once should never be waited for again. */
    void setCacheDir(const std::string& dir) { std::lock_guard<std::mutex> lk(m_); cacheDir_ = dir; }
    void setRatioFile(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        ratioFile_ = path;
        if (AacDecoder::kExactFrames || path.empty()) return;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            double r = 0.0;
            if (fscanf(f, "%lf", &r) == 1 && r > 0.5 && r < 2.0) knownRatio_ = r;
            fclose(f);
        }
    }
    /** ★ The newest slideshow image off the air for the playing service — a station logo or
     *  now-playing artwork (TS 101 499), served by the shim at /vibeserver/dabslide. */
    struct Slide { std::vector<uint8_t> bytes; std::string mime, name; uint32_t seq = 0; uint32_t sid = 0; };
    bool slide(Slide& out) { std::lock_guard<std::mutex> lk(m_); pollSlide(); if (!slide_.seq) return false; out = slide_; return true; }

    /** ★★★ THE OFF-AIR PICTURE, KEPT. Where the carousel's logos are cached per ensemble, this is
     *  the per-SERVICE slideshow store — and it exists because of a station with no logo anywhere
     *  else. Stuart, 2026-09-08, on Embrace (7B): "Embrace on the NNDAB multiplex has no RadioDNS
     *  or other logo fallback but when I tuned to it in the advanced signal window the image drew
     *  in and worked, but then wasnt saved so when i switched back to the list it wasnt in the
     *  list nor the VTS and when I tuned away and back again it had to be read again from the air."
     *  All three follow from one fact: the slide lived ONLY in `slide_`, which setService and
     *  setChannel both clear — correctly, because the pane must never show the last station's
     *  artwork under this one's name. The live slide keeps that behaviour; this is a second,
     *  durable copy that survives the retune and answers by SId. */
    void setSlideDir(const std::string& dir) { std::lock_guard<std::mutex> lk(m_); slideDir_ = dir; }

    /** The kept slideshow image for one service, from memory or from the disk store. */
    bool serviceSlide(uint32_t sid, Slide& out) {
        std::lock_guard<std::mutex> lk(m_);
        pollSlide();
        auto it = slideBySid_.find(sid);
        if (it != slideBySid_.end() && !it->second.bytes.empty()) { out = it->second; return true; }
        if (slideDir_.empty() || !sid) return false;
        for (const char* ext : { "png", "jpg", "gif", "webp" }) {
            const std::string p = slidePath(sid, ext);
            FILE* f = fopen(p.c_str(), "rb");
            if (!f) continue;
            Slide s; uint8_t buf[4096]; size_t r;
            while ((r = fread(buf, 1, sizeof buf, f)) > 0) s.bytes.insert(s.bytes.end(), buf, buf + r);
            fclose(f);
            if (s.bytes.empty()) continue;
            s.mime = std::string("image/") + (strcmp(ext, "jpg") == 0 ? "jpeg" : ext);
            s.sid = sid; s.seq = 1;
            slideRemember(sid, s);                // ★ read once, then answered from memory
            out = s; return true;
        }
        return false;
    }
    /** Do we hold a picture for this service? Cheap enough for the station list's every block. */
    bool haveServiceSlide(uint32_t sid) {
        std::lock_guard<std::mutex> lk(m_);
        return haveSlideNoLock(sid);
    }
    struct Quality { bool locked; float fibRate; float nullDepthDb; double mscBer; };
    Quality quality() {
        std::lock_guard<std::mutex> lk(m_);
        const DabStats& s = rx_.stats();
        return { s.locked, float(s.fibRate), s.nullDepthDb, s.mscBer };
    }

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
                snprintf(cb, sizeof cb, ",\"codecDetail\":\"DAB+ %d kbit/s %d kHz %s %s\",\"audioRateHz\":%d,\"coreRateHz\":%d,\"sbr\":%s,\"ps\":%s,\"audioCh\":%d,\"aacEffRateHz\":%d",
                         rx_.serviceBitrate(), afmt_.outputRateHz / 1000, prof, chan,
                         afmt_.outputRateHz, afmt_.coreRateHz, afmt_.sbr ? "true" : "false",
                         aacPs_ ? "true" : "false", aacOutCh_, aacEffRateHz_);
                /* ★ Stuart, 2026-09-07, on the start-up glide of a DAB+ service on the Pi: "I
                 *  don't mind the glide, it's quite fun, just make a notification to show buffer
                 *  building". The measured-rate estimate is still moving for the first ~3 s of
                 *  access units; the client says so while it is. */
                /* Settling = the running estimate has not converged yet (~160 access units, the
                 *  moving window's first full settle; Stuart heard the glide outlast a 48-AU
                 *  notice) and no remembered ratio started this service at the right speed. */
                j += (aacAuTotal_ < 160 && !aacStartedKnown_) ? ",\"aacSettling\":true" : ",\"aacSettling\":false";
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
            /* ★ The RDS equivalents the ensemble broadcasts about ITSELF (Stuart, 2026-09-07: "the
             *  real full RDS info from DAB"): the clock (FIG 0/10, CT) with the local offset (0/9),
             *  and the other blocks this ensemble is on (0/21, AF). */
            pollSlide();
            /* ★★★ THE CATEGORISED SLIDESHOW'S GALLERY (TS 101 499). Names and titles only; the
             *  pictures are fetched by name from the carousel endpoint as they are needed. */
            if (!cats_.empty()) {
                std::string g;
                for (const auto& kv : cats_) {
                    // ★ 5.3.5.3: a category with no title "shall not be shown to the user".
                    if (kv.second.title.empty() || kv.second.slides.empty()) continue;
                    if (!g.empty()) g += ',';
                    g += "{\"id\":" + std::to_string(int(kv.first))
                       + ",\"title\":\"" + esc(kv.second.title) + "\",\"slides\":[";
                    bool fs = true;
                    for (const auto& sl : kv.second.slides) {
                        if (!fs) g += ','; fs = false;
                        g += "{\"n\":\"" + esc(sl.name) + "\",\"i\":" + std::to_string(sl.slideId) + "}";
                    }
                    g += "]}";
                }
                if (!g.empty()) j += ",\"slideCats\":[" + g + "]";
            }
            if (slideAlert_) j += ",\"slideAlert\":" + std::to_string(slideAlert_);
            if (!slideClickUrl_.empty()) j += ",\"slideClick\":\"" + esc(slideClickUrl_) + "\"";
            if (slide_.seq) {
                char sb[160];
                snprintf(sb, sizeof sb, ",\"slide\":{\"seq\":%u,\"mime\":\"%s\",\"bytes\":%zu,\"name\":\"", slide_.seq, slide_.mime.c_str(), slide_.bytes.size());
                j += sb; j += esc(slide_.name) + "\"}";
            }
            {
                char mb[96];
                snprintf(mb, sizeof mb, ",\"motGroups\":%u,\"motCrcFail\":%u,\"motObjects\":%u", pad_.mot().groups(), pad_.mot().crcFails(), pad_.mot().objects());
                j += mb;
                char pb[160];
                snprintf(pb, sizeof pb, ",\"spi\":{\"sid\":%u,\"packets\":%u,\"groups\":%u,\"crcFail\":%u,\"lost\":%u,\"dir\":%s,\"named\":%zu,\"complete\":%u,\"logoSvcs\":%zu}",
                         spiSid_, packets_.packets(), packets_.groups(), packets_.crcFails(), packets_.lost(), carousel_.haveDirectory() ? "true" : "false", carousel_.named(), carousel_.completeCount(), spiLogoRefs_.size());
                j += pb;
                char qb[200]; snprintf(qb, sizeof qb, ",\"spiParse\":{\"runs\":%u,\"siDocs\":%u,\"svcs\":%u,\"eid\":%u},\"spiCrc\":{\"byLen\":[%u,%u,%u,%u],\"last\":\"%02x %02x %02x\",\"lastAddr\":%d}",
                                         spiParseRuns_, spiSiDocs_, spiParsedSvcs_, unsigned(e.eid), packets_.failByLen(0), packets_.failByLen(1), packets_.failByLen(2), packets_.failByLen(3),
                                         packets_.lastFail()[0], packets_.lastFail()[1], packets_.lastFail()[2], packets_.lastFailAddr()); j += qb;
                if (carousel_.named()) {   // ★ what the carousel holds — names, types, sizes, done — for the DX pane and for debugging
                    j += ",\"spiObjs\":[";
                    bool f1 = true; size_t k = 0;
                    for (const auto& kv : carousel_.objects()) {
                        if (k++ >= 40) break;
                        if (!f1) j += ','; f1 = false;
                        char ob[64]; snprintf(ob, sizeof ob, "\",\"ct\":%d,\"st\":%d,\"size\":%u,\"done\":%s}", kv.second.contentType, kv.second.subType, kv.second.bodySize, kv.second.complete ? "true" : "false");
                        j += "{\"name\":\"" + esc(kv.first) + ob;
                    }
                    j += "]";
                }
            }
            {   /* ★ The DATA services and every user application signalled (FIG 0/13), so a DXer
                 *  can see what else the multiplex carries — and whether an SPI/EPG data service
                 *  (0x007) exists to take logos from when the audio carries no slideshow. */
                std::string ds = ",\"dataSvcs\":[";
                bool f1 = true;
                for (const auto& kv : e.services) {
                    const Service& sv = kv.second;
                    std::string apps;
                    for (const auto& c : sv.components) for (const auto& a : c.apps) { char ab[16]; snprintf(ab, sizeof ab, "%s%u", apps.empty() ? "" : ",", unsigned(a.type)); apps += ab; }
                    if (!sv.isData && apps.empty()) continue;
                    if (!f1) ds += ','; f1 = false;
                    ds += "{\"sid\":" + std::to_string(kv.first) + ",\"label\":\"" + esc(sv.label) + "\",\"data\":" + (sv.isData ? "true" : "false") + ",\"apps\":[" + apps + "]}";
                }
                j += ds + "]";
            }
            {   // ★ How much linking/frequency signalling this ensemble carries at all — the DXer's
                //   answer to "why is the FM row empty": the mux sends none, or we missed it.
                char cb[48];
                snprintf(cb, sizeof cb, ",\"nLinks\":%zu,\"nFi\":%zu", e.links.size(), e.freqInfo.size());
                j += cb;
                const auto& td = rx_.tiiDiag();
                char db[96];
                snprintf(db, sizeof db, ",\"tiiDiag\":{\"comb\":%d,\"f4s\":%.1f,\"f45\":%.2f,\"frames\":%d}", td.comb, td.fourthOverSigma, td.fourthOverFifth, td.frames);
                j += db;
                /* ★ The raw linkage table (FIG 0/6) and frequency table (FIG 0/21), for the DX pane and
                 *  for seeing what a multiplex actually signals. Small: a few sets of a few ids. */
                if (!e.links.empty()) {
                    j += ",\"links\":[";
                    bool f1 = true;
                    for (const auto& lk : e.links) {
                        const LinkSet& ls = lk.second;
                        if (!f1) j += ','; f1 = false;
                        char lb[80];
                        snprintf(lb, sizeof lb, "{\"lsn\":%u,\"idlq\":%d,\"hard\":%s,\"active\":%s,\"ils\":%s,\"ids\":[",
                                 unsigned(ls.lsn), ls.idlq, ls.hard ? "true" : "false", ls.active ? "true" : "false", ls.ils ? "true" : "false");
                        j += lb;
                        bool f2 = true;
                        for (uint32_t id : ls.ids) { if (!f2) j += ','; f2 = false; j += std::to_string(id); }
                        j += "]}";
                    }
                    j += "]";
                }
                if (!e.freqInfo.empty()) {
                    j += ",\"fi\":[";
                    bool f1 = true;
                    for (const auto& fk : e.freqInfo) {
                        const FreqInfo& fi = fk.second;
                        if (!f1) j += ','; f1 = false;
                        j += "{\"id\":" + std::to_string(fi.id) + ",\"rm\":" + std::to_string(fi.rm) + ",\"hz\":[";
                        bool f2 = true;
                        for (uint32_t hz : fi.hz) { if (!f2) j += ','; f2 = false; j += std::to_string(hz); }
                        j += "]}";
                    }
                    j += "]";
                }
            }
            /* ★★★ NOW AND NEXT, FROM THE MULTIPLEX'S OWN SCHEDULE (TS 102 371 Programme
             *  Information). `epgPi` is published even when it is zero, and that is the point:
             *  nothing in the UK transmits PI, so the row must be able to say "none transmitted"
             *  rather than looking like a decoder that failed. Same posture as the TII pane on
             *  11D, which says so rather than showing an empty box. */
            {
                const EpgProgramme *nowP = nullptr, *nextP = nullptr;
                epgNowNext(sid_, e, nowP, nextP);
                j += ",\"epgPi\":" + std::to_string(spiPiDocs_);
                auto one = [&](const char* key, const EpgProgramme* p) {
                    if (!p) return;
                    char tb[48];
                    snprintf(tb, sizeof tb, "\",\"at\":\"%02d:%02d\",\"mins\":%d,\"lto\":%d}",
                             (p->start.utcMinutes() + p->start.ltoHalfHours * 30 + 1440) / 60 % 24,
                             (p->start.utcMinutes() + p->start.ltoHalfHours * 30 + 1440) % 60,
                             (p->durationSec + 59) / 60, p->start.ltoHalfHours);
                    j += std::string(",\"") + key + "\":{\"name\":\"" + esc(p->name)
                       + "\",\"desc\":\"" + esc(p->description) + tb;
                };
                one("epgNow",  nowP);
                one("epgNext", nextP);
            }
            if (e.mjd >= 0) {
                char tb[64];
                snprintf(tb, sizeof tb, ",\"mjd\":%d,\"utc\":\"%02d:%02d:%02d\",\"lto\":%d", e.mjd, e.utcHour, e.utcMin, e.utcSec, e.ltoHalfHours);
                j += tb;
            }
            /* ★★★ ANNOUNCEMENTS ON AIR NOW (FIG 0/19, EN 300 401 8.1.6). Reported, never acted on:
             *  the correct receiver behaviour is to switch the audio to the announcing sub-channel,
             *  and on a SHARED VFO that is a hijack — one listener's traffic flash would drag every
             *  other listener off the station they chose, which is the same fault as the `dab off`
             *  that put the whole radio back on 96.1 under a listener who was never told
             *  (2026-09-07). So the pane says an announcement is running and on which service, and
             *  the listener decides.
             *  ★★ AN ANNOUNCEMENT IS ONLY TRUE NOW. 0/19 repeats while it runs and simply stops
             *     when it ends — a transmitter need not send the clearing flags — so an entry is
             *     aged out here rather than latched. Ten seconds: the FIG repeats about once a
             *     second, so this survives a burst of FIB losses without outliving the event.
             *  ★ Only announcements whose CLUSTER the tuned service belongs to are relevant to
             *    this listener, except the alarm, which is relevant to everyone (8.1.6.1). */
            {
                const double now = ficNowSec();
                auto sup = e.announceSupport.find(sid_);
                std::string items;
                for (const auto& kv : e.announceActive) {
                    if (now - kv.second.at > 10.0) continue;
                    const bool alarm = (kv.second.aswFlags & 0x8000) != 0;
                    bool mine = alarm;
                    if (!mine && sup != e.announceSupport.end())
                        for (uint8_t c : sup->second.clusters) if (c == kv.first) { mine = true; break; }
                    if (!mine) continue;
                    std::string types;
                    for (int b = 0; b < 11; ++b) {
                        if (!(kv.second.aswFlags & (0x8000 >> b))) continue;
                        const char* nm = dabAnnouncementName(b);
                        if (!nm) continue;
                        if (!types.empty()) types += ',';
                        types += "\"" + std::string(nm) + "\"";
                    }
                    if (types.empty()) continue;
                    /* ★ Which service is carrying it: the sub-channel is the only pointer 0/19
                     *  gives, so it is matched back to a service the listener has a name for. */
                    std::string on;
                    for (const auto& sv : e.services) {
                        const auto* pc = sv.second.primaryComponent();
                        if (pc && pc->subChId == kv.second.subChId) { on = sv.second.label; break; }
                    }
                    if (!items.empty()) items += ',';
                    items += "{\"cluster\":" + std::to_string(int(kv.first))
                           + ",\"types\":[" + types + "]"
                           + ",\"subChId\":" + std::to_string(kv.second.subChId)
                           + ",\"on\":\"" + esc(on) + "\""
                           + ",\"alarm\":" + (alarm ? "true" : "false") + "}";
                }
                if (!items.empty()) j += ",\"announce\":[" + items + "]";
                /* ★ And what this service CAN carry (0/18) — a station that supports the traffic
                 *  flash but is not running one now is a different thing from one that never will,
                 *  and only the standing capability can tell them apart. */
                if (sup != e.announceSupport.end() && sup->second.asuFlags) {
                    std::string types;
                    for (int b = 0; b < 11; ++b) {
                        if (!(sup->second.asuFlags & (0x8000 >> b))) continue;
                        const char* nm = dabAnnouncementName(b);
                        if (!nm) continue;
                        if (!types.empty()) types += ',';
                        types += "\"" + std::string(nm) + "\"";
                    }
                    if (!types.empty()) j += ",\"announceSupport\":[" + types + "]";
                }
            }
            {
                auto fi = e.freqInfo.find((0u << 16) | e.eid);
                if (fi != e.freqInfo.end() && !fi->second.hz.empty()) {
                    j += ",\"altHz\":[";
                    bool f1 = true;
                    for (uint32_t hz : fi->second.hz) { if (!f1) j += ','; f1 = false; j += std::to_string(hz); }
                    j += "]";
                }
            }
        }
        {
            /* ★ Signal analysis for the DX pane: MER, raw MSC bit error rate, the PRS impulse
             *  response and a constellation snapshot. Built by append, never through the big
             *  printf (see the argument-misalignment note above). */
            char ab[96];
            snprintf(ab, sizeof ab, ",\"mer\":%.1f,\"mscBer\":%.5f,\"irPeak\":%d,\"aacDec\":\"%s\"",
                     s.merDb, s.mscBer, s.irPeakSamples, aac_.backend());   // ★ which decoder DAB+ goes through — never guessed from the sound
            j += ab;
            const auto& ir = rx_.impulseResponse();
            if (!ir.empty()) {
                j += ",\"ir\":[";
                for (size_t i = 0; i < ir.size(); ++i) { if (i) j += ','; j += std::to_string(int(ir[i])); }
                j += "]";
            }
            const auto& cs = rx_.constellation();
            if (!cs.empty()) {
                j += ",\"iq\":[";
                for (size_t i = 0; i < cs.size(); ++i) { if (i) j += ','; j += std::to_string(int(cs[i])); }
                j += "]";
            }
        }
        {
            /* ★ TII: which transmitter(s) of the SFN we are hearing — main id, sub id, dB over
             *  the null's noise. The figure a DX-er wants beside the ensemble name. */
            j += ",\"tii\":[";
            bool f1 = true;
            for (const auto& h : rx_.tii()) {
                char tb[96];
                snprintf(tb, sizeof tb, "%s{\"main\":%d,\"sub\":%d,\"db\":%.1f", f1 ? "" : ",", h.mainId, h.subId, h.strength);
                j += tb; f1 = false;
                /* ★ Named from the country's directory when there is one for this ensemble's ECC;
                 *  nearest site when the code is reused; distance only when we know where we are. */
                if (txdb_ && e.ecc >= 0) {
                    const DabTxMatch m = txdb_->lookup(e.ecc, e.eid, h.mainId, h.subId, rxLat_, rxLon_);
                    if (m.found) {
                        j += ",\"site\":\"" + esc(m.site) + "\",\"area\":\"" + esc(m.area) + "\"";
                        char kb[64];
                        snprintf(kb, sizeof kb, ",\"lat\":%.4f,\"lon\":%.4f", m.lat, m.lon);
                        j += kb;
                        if (m.km >= 0) { snprintf(kb, sizeof kb, ",\"km\":%.1f", m.km); j += kb; }
                        if (m.ambiguous) j += ",\"ambiguous\":true";
                    }
                }
                j += "}";
            }
            j += "]";
            if (txdb_ && e.eid) {
                const auto sites = txdb_->sitesFor(e.ecc, e.eid, rxLat_, rxLon_);
                if (!sites.empty()) {
                    j += ",\"licensed\":[";
                    bool f1 = true;
                    for (const auto& st : sites) {
                        if (!f1) j += ','; f1 = false;
                        char sb[64];
                        snprintf(sb, sizeof sb, "\",\"code\":\"%02X/%02X\",\"km\":%.1f}", st.mainId, st.subId, st.km);
                        j += "{\"site\":\"" + esc(st.site) + "\",\"area\":\"" + esc(st.area) + sb;
                    }
                    j += "]";
                }
            }
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
            /* ★ `logoSlide` says we hold a picture this service sent over the air. It is the LAST
             *  resort behind RadioDNS and the SPI carousel, because a slideshow is programme
             *  artwork, not a logo — but for a small station that publishes neither it IS the
             *  station's picture, and Embrace on 7D is exactly that case. */
            snprintf(b, sizeof b, "{\"sid\":%u,\"logoAir\":%s,\"logoSlide\":%s,\"label\":\"%s\",\"short\":\"%s\",\"codec\":\"%s\",\"subch\":%d,\"pty\":%d,\"ptyDyn\":%s,\"slides\":%s,\"kbps\":%d,\"prot\":\"%s\",\"cuStart\":%d,\"cuSize\":%d,\"scids\":%d,\"ecc\":%d",
                     unsigned(kv.first), hasAirLogo(kv.first) ? "true" : "false",
                     haveSlideNoLock(kv.first) ? "true" : "false", esc(sv.label).c_str(), esc(sv.shortLabel).c_str(),
                     pc->scType == 63 ? "DAB+" : pc->scType == 0 ? "MP2" : "?", pc->subChId,
                     sv.pty, sv.ptyDynamic ? "true" : "false", sv.hasSlideshow() ? "true" : "false",
                     si.bitrateKbps, prot, sc.startCu, sizeCu, pc->scids, sv.ecc >= 0 ? sv.ecc : e.ecc);
            j += b;
            /* ★ THE RDS SIDE OF THIS SERVICE, from the ensemble's own signalling: the FM stations
             *  that ARE this programme (FIG 0/6 links to RDS PI codes, FIG 0/21 their frequencies)
             *  and the other DAB services carrying it. Stuart, 2026-09-07: "I don't want invented
             *  RDS, I want the real full RDS info from DAB". */
            {
                std::vector<uint16_t> lsns;
                bool hard = false, active = false;
                /* ★ A set names this service by its SId (IdLQ 0) — or, on 11D as measured, by its PI
                 *  in a PI-only set (IdLQ 1), which is the implicit SId = PI rule again. */
                for (const auto& lk : e.links) {
                    const LinkSet& ls = lk.second;
                    if (ls.idlq != 0 && ls.idlq != 1) continue;
                    for (uint32_t id : ls.ids)
                        if (id == kv.first || (id & 0xFFFFu) == (kv.first & 0xFFFFu)) { lsns.push_back(ls.lsn); hard = ls.hard; active = ls.active; break; }
                }
                std::vector<uint32_t> pis, sids;
                for (const auto& lk : e.links) {
                    const LinkSet& ls = lk.second;
                    bool in = false; for (uint16_t l : lsns) if (l == ls.lsn) { in = true; break; }
                    if (!in) continue;
                    for (uint32_t id : ls.ids) {
                        if (ls.idlq == 1) { const uint32_t pi = id & 0xFFFFu; if (pi == (kv.first & 0xFFFFu)) continue; bool h = false; for (uint32_t x : pis) if (x == pi) h = true; if (!h) pis.push_back(pi); }
                        else if (ls.idlq == 0 && id != kv.first && (id & 0xFFFFu) != (kv.first & 0xFFFFu)) { bool h = false; for (uint32_t x : sids) if (x == id) h = true; if (!h) sids.push_back(id); }
                    }
                }
                /* ★ IMPLICIT LINKING (TS 103 176 6.4.2 / EN 300 401 6.3.1): a programme service's
                 *  16-bit SId has the RDS PI code's structure — country id + reference — and in the
                 *  UK it IS the station's PI, which is how a car radio follows Flex FM between 7D
                 *  and FM with no explicit link set on air (Stuart, 2026-09-07). With no explicit
                 *  link the SId stands as the PI, marked implicit, and FIG 0/21 keyed by it gives
                 *  the FM frequencies. */
                const bool implicit = pis.empty() && !sv.isData;
                if (!sv.isData) pis.insert(pis.begin(), kv.first & 0xFFFFu);   // own PI first, then the linked ones
                if (!pis.empty() || !sids.empty()) {
                    j += std::string(",\"piImplicit\":") + (implicit ? "true" : "false");
                    j += ",\"pi\":[";
                    bool f1 = true; for (uint32_t pi : pis) { if (!f1) j += ','; f1 = false; j += std::to_string(pi); }
                    j += "],\"fm\":[";
                    f1 = true;
                    for (uint32_t pi : pis) {
                        auto fi = e.freqInfo.find((8u << 16) | pi);
                        if (fi == e.freqInfo.end()) continue;
                        for (uint32_t hz : fi->second.hz) { if (!f1) j += ','; f1 = false; j += std::to_string(hz); }
                    }
                    j += "],\"linkSids\":[";
                    f1 = true; for (uint32_t s2 : sids) { if (!f1) j += ','; f1 = false; j += std::to_string(s2); }
                    j += std::string("],\"linkHard\":") + (hard ? "true" : "false") + ",\"linkActive\":" + (active ? "true" : "false");
                }
            }
            /* ★ Every row's "now playing": the playing service's live label, the others' from the
             *  scanner, with how old it is. */
            if (kv.first == sid_ && pad_.dls().label().valid && !pad_.dls().label().text.empty()) {
                j += ",\"dls\":\"" + esc(pad_.dls().label().text) + "\",\"dlsAge\":0";
                /* ★★★ DL PLUS — THE STATION SAYING WHICH PART IS THE ARTIST (TS 102 980). Only
                 *  for the tuned service: the PAD scanner reads other services' labels without
                 *  their audio, and a tag is a span of a label, so a scanned row's tags would go
                 *  stale the moment its label was refreshed by another slot.
                 *  ★ itemRunning false means the ITEM has ended — an ad break or the news under a
                 *    label the station has not bothered to clear. A receiver that keeps showing
                 *    the last track through the news is the thing this flag exists to prevent. */
                const auto& dl = pad_.dls().label();
                if (dl.hasDlPlus) {
                    j += std::string(",\"dlpRunning\":") + (dl.itemRunning ? "true" : "false");
                    j += ",\"dlp\":{";
                    /* ★ Keyed by NAME, and the table is deliberately many-to-one: 9 and 12 are
                     *  both "news", 27 and 28 both "programme". A duplicate key is not an object
                     *  — JSON.parse keeps only the last — so the first tag of a name wins and the
                     *  rest are dropped here, where it is visible, rather than in the browser. */
                    bool ft = true;
                    std::vector<std::string> used;
                    for (const auto& t : dl.tags) {
                        const char* nm = dlPlusTypeName(t.type);
                        if (!nm || t.text.empty()) continue;
                        if (std::find(used.begin(), used.end(), nm) != used.end()) continue;
                        used.emplace_back(nm);
                        if (!ft) j += ','; ft = false;
                        j += "\"" + std::string(nm) + "\":\"" + esc(t.text) + "\"";
                    }
                    j += "}";
                }
            } else {
                auto dr = dlsAll_.find(kv.first);
                if (dr != dlsAll_.end() && !dr->second.text.empty()) {
                    char ab[40]; snprintf(ab, sizeof ab, "\",\"dlsAge\":%.0f", nowSec() - dr->second.at);
                    j += ",\"dls\":\"" + esc(dr->second.text) + ab;
                }
            }
            j += "}";
        }
        j += "]}";
        return j;
    }

private:
    /* ── ★★★ THE PAD SCANNER — every station's "now playing", not just the one you hear ──
     *  Stuart's idea, 2026-09-07. Four extra sub-channels are decoded for their PAD only (see
     *  vibe_dab_padtap.h) and rotated through the ensemble every few seconds, so each service's
     *  label is refreshed roughly every half minute on a 30-service multiplex; the playing
     *  service keeps its own live label. Costs four small Viterbis — the Pi's DSP thread had
     *  three quarters of its time spare. */
    static constexpr size_t kScanSlots   = 4;
    static constexpr double kScanDwellS  = 4.0;    ///< the deinterleaver needs 0.4 s, a label ~2 s
    /* ★★★ LOGOS FOR THE WHOLE MULTIPLEX, OFF THE AIR. A data service with user application 0x007
     *  (SPI: "BBC Guide" on 12B, measured 2026-09-07) carries the SI document and every logo
     *  file in one MOT carousel on a packet-mode sub-channel. A dedicated slot past the PAD
     *  scanner's decodes that sub-channel; packets → data groups → directory-mode MOT → the SI
     *  document names each service's logo files → logos by SId. Stuart: "don't forget worldwide
     *  multiplexes may carry this info". */
    static constexpr size_t kSpiSlot = kScanSlots;
    void pumpSpi() {
        const Ensemble& e = rx_.ensemble();
        if (spiSid_ == 0 || !e.services.count(spiSid_)) {
            spiSid_ = 0;
            for (const auto& kv : e.services) {
                const Service& sv = kv.second;
                if (!sv.isData) continue;
                for (const auto& c : sv.components) {
                    bool spi = false; for (const auto& a : c.apps) if (a.type == 0x007) spi = true;
                    if (spi && c.tmid == 3 && c.subChId >= 0 && c.packetAddr >= 0 && e.subChannels.count(c.subChId)) {
                        if (rx_.scanSelect(kSpiSlot, kv.first)) { spiSid_ = kv.first; spiAddr_ = c.packetAddr; packets_.setAddress(spiAddr_); packets_.setSink([this](const uint8_t* g, size_t n) { carousel_.feedDataGroup(g, n); }); }
                        break;
                    }
                }
                if (spiSid_) break;
            }
            if (!spiSid_) return;
        }
        cacheLoad(e);
        for (const auto& f : rx_.takeScanFrames(kSpiSlot)) packets_.feedFrame(f.data(), f.size());
        cacheSave(e);
        if (carousel_.version() != spiSeen_) {
            spiSeen_ = carousel_.version();
            /* Re-read every complete SI object and rebuild the logo map: service SId → the best
             *  logo file names. Cheap — a few objects, a few services. */
            std::map<uint32_t, std::vector<SpiLogoRef>> refs;
            std::map<uint32_t, std::vector<EpgProgramme>> sched;
            ++spiParseRuns_; spiParsedSvcs_ = 0; spiSiDocs_ = 0; spiPiDocs_ = 0;
            for (const auto& kv : carousel_.objects()) {
                const MotCarousel::Object& o = kv.second;
                if (!o.complete || o.contentType != 7) continue;         // SPI application objects
                /* ★★★ SUBTYPE TELLS THE THREE SPI DOCUMENTS APART (TS 102 371 table 11):
                 *      7/0 Service Information — the services and their logos
                 *      7/1 Programme Information — the schedule
                 *      7/2 Group Information — series groupings, not read
                 *  Only 7/0 was ever looked at; 7/1 was dropped by this filter, which is why the
                 *  EPG "was not implemented" — the objects were arriving and being discarded. */
                if (o.subType == 1) { readEpgObject(o, e, sched); continue; }
                if (o.subType != 0) continue;
                ++spiSiDocs_;
                for (const auto& sv : SpiDocument::parse(o.body.data(), o.body.size())) {
                    ++spiParsedSvcs_;
                    if (sv.sid && (sv.eid == 0 || sv.eid == e.eid)) refs[sv.sid] = sv.logos;
                }
            }
            spiLogoRefs_.swap(refs);
            if (!sched.empty() || !epg_.empty()) epg_.swap(sched);
        }
    }
public:
    std::map<uint32_t, std::vector<EpgProgramme>> epg_;   ///< the schedule, by SId — see readEpgObject
    uint32_t spiPiDocs_ = 0;      ///< Programme Information objects seen (0 everywhere in the UK)
    std::string cacheDir_; uint16_t cacheLoadedEid_ = 0;
    static std::string cacheName(int ecc, uint16_t eid) { char b[24]; snprintf(b, sizeof b, "%02x-%04x", unsigned(ecc & 0xFF), unsigned(eid)); return b; }
    static bool safeName(const std::string& n) { if (n.empty() || n.size() > 120) return false; for (char c : n) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false; return n[0] != '.'; }
    static void typeFor(const std::string& n, int& ct, int& st) {
        const size_t d = n.rfind('.'); const std::string ext = d == std::string::npos ? "" : n.substr(d + 1);
        if (ext == "png" || ext == "PNG") { ct = 2; st = 3; } else if (ext == "jpg" || ext == "jpeg" || ext == "JPG") { ct = 2; st = 1; } else { ct = 7; st = 0; }
    }
    void cacheLoad(const Ensemble& e) {
        if (cacheDir_.empty() || !e.eid || cacheLoadedEid_ == e.eid) return;
        cacheLoadedEid_ = e.eid;
        const std::string dir = cacheDir_ + "/" + cacheName(e.ecc, e.eid);
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        size_t loaded = 0;
        while (dirent* en = readdir(d)) {
            const std::string n = en->d_name;
            if (!safeName(n)) continue;
            FILE* f = fopen((dir + "/" + n).c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> body; uint8_t buf[4096]; size_t r;
            while ((r = fread(buf, 1, sizeof buf, f)) > 0) body.insert(body.end(), buf, buf + r);
            fclose(f);
            int ct, st; typeFor(n, ct, st);
            if (!body.empty()) { carousel_.inject(n, ct, st, std::move(body)); ++loaded; }
        }
        closedir(d);
        if (loaded) fprintf(stderr, "[DAB] %zu carousel files for %s from the cache\n", loaded, cacheName(e.ecc, e.eid).c_str());
    }
    /** ★★★ ONE PROGRAMME INFORMATION OBJECT (MOT 7/1) INTO THE SCHEDULE.
     *
     *  ★ A carousel carries one PI object per service per day, so the programmes for one service
     *    arrive across several objects and must be MERGED, not replaced — the last object read
     *    would otherwise be the only day anyone ever saw.
     *  ★ Sorted by start time and de-duplicated on it: a carousel repeats its objects for ever, so
     *    without this the same evening accumulates on every cycle until the pane is unreadable.
     *  ★ NOT VERIFIABLE HERE. Nothing within reach of Northampton transmits PI (measured
     *    2026-09-08). This is written against TS 102 371 and tested against synthesised documents;
     *    it costs nothing when no object arrives, which is the UK case. */
    void readEpgObject(const MotCarousel::Object& o, const Ensemble& e,
                       std::map<uint32_t, std::vector<EpgProgramme>>& sched) {
        ++spiPiDocs_;
        for (const auto& s : EpgDocument::parse(o.body.data(), o.body.size())) {
            if (!s.sid) continue;
            if (s.eid && e.eid && s.eid != e.eid) continue;      // another ensemble's schedule
            auto& v = sched[s.sid];
            for (const auto& pr : s.programmes) {
                if (!pr.start.valid() || pr.name.empty()) continue;
                bool dup = false;
                for (const auto& x : v)
                    if (x.start.mjd == pr.start.mjd && x.start.utcMinutes() == pr.start.utcMinutes()
                        && x.name == pr.name) { dup = true; break; }
                if (!dup) v.push_back(pr);
            }
            std::sort(v.begin(), v.end(), [](const EpgProgramme& a, const EpgProgramme& b) {
                if (a.start.mjd != b.start.mjd) return a.start.mjd < b.start.mjd;
                return a.start.utcMinutes() < b.start.utcMinutes();
            });
            if (v.size() > 400) v.resize(400);                   // a week of a busy service
        }
    }

    /** What is on now and next for a service, judged against the ensemble's own clock (FIG 0/10).
     *  ★ The ensemble clock rather than ours: a receiver whose host clock is wrong would otherwise
     *    show the wrong programme with no way for the listener to tell. */
    bool epgNowNext(uint32_t sid, const Ensemble& e,
                    const EpgProgramme*& now, const EpgProgramme*& next) const {
        now = next = nullptr;
        auto it = epg_.find(sid);
        if (it == epg_.end() || it->second.empty() || e.mjd < 0) return false;
        const int nowMin = e.utcHour * 60 + e.utcMin;
        for (const auto& pr : it->second) {
            const long tp = long(pr.start.mjd) * 1440 + pr.start.utcMinutes();
            const long tn = long(e.mjd) * 1440 + nowMin;
            if (tp <= tn && tn < tp + (pr.durationSec + 59) / 60) now = &pr;
            else if (tp > tn && !next) next = &pr;
        }
        return now || next;
    }

    void cacheSave(const Ensemble& e) {
        if (cacheDir_.empty() || !e.eid) return;
        const std::vector<std::string> done = carousel_.takeCompleted();
        if (done.empty()) return;
        const std::string dir = cacheDir_ + "/" + cacheName(e.ecc, e.eid);
        mkdir(cacheDir_.c_str(), 0755); mkdir(dir.c_str(), 0755);
        for (const std::string& n : done) {
            if (!safeName(n)) continue;
            const MotCarousel::Object* o = carousel_.find(n);
            if (!o) continue;
            const std::string tmp = dir + "/" + n + ".tmp";
            if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(o->body.data(), 1, o->body.size(), f); fclose(f); rename(tmp.c_str(), (dir + "/" + n).c_str()); }
        }
    }
public:
    /** The best off-air logo for a service: the largest square/unrestricted PNG or JPEG we hold. */
    bool airLogo(uint32_t sid, std::vector<uint8_t>& bytes, std::string& mime, int& w, int& h) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = spiLogoRefs_.find(sid);
        if (it == spiLogoRefs_.end()) return false;
        const MotCarousel::Object* best = nullptr; int bestPx = -1; const SpiLogoRef* bestRef = nullptr;
        for (const auto& r : it->second) {
            const MotCarousel::Object* o = carousel_.find(r.url);
            if (!o) continue;
            const int px = r.width * r.height;
            if (px > bestPx && px <= 320 * 240) { best = o; bestPx = px; bestRef = &r; }
        }
        if (!best) return false;
        bytes = best->body; w = bestRef->width; h = bestRef->height;
        mime = !bestRef->mime.empty() ? bestRef->mime : (best->subType == 1 ? "image/jpeg" : "image/png");
        return true;
    }
    void ensembleIds(int& ecc, uint16_t& eid) { std::lock_guard<std::mutex> lk(m_); const Ensemble& e = rx_.ensemble(); ecc = e.ecc; eid = e.eid; }
    /** Any complete carousel object by its content name — the SI document, a logo, anything the
     *  multiplex carries (for the DX pane and for inspection). */
    bool carouselObject(const std::string& name, std::vector<uint8_t>& bytes, int& ct, int& st) {
        std::lock_guard<std::mutex> lk(m_);
        const MotCarousel::Object* o = carousel_.find(name);
        if (!o) return false;
        bytes = o->body; ct = o->contentType; st = o->subType;
        return true;
    }
    /** Does an off-air logo exist for this service (any complete file)? */
    bool hasAirLogo(uint32_t sid) const {
        auto it = spiLogoRefs_.find(sid);
        if (it == spiLogoRefs_.end()) return false;
        for (const auto& r : it->second) if (carousel_.find(r.url)) return true;
        return false;
    }
private:
    void pumpScan() {
        const Ensemble& e = rx_.ensemble();
        if (e.services.empty()) return;
        const double now = nowSec();
        // harvest what the slots have decoded
        for (size_t i = 0; i < rx_.scanSlots() && i < taps_.size(); ++i) {
            const uint32_t sid = rx_.scanSid(i);
            if (!sid) continue;
            const int type = rx_.scanType(i);
            for (const auto& f : rx_.takeScanFrames(i)) taps_[i].feed(f, type);
            const DynamicLabel& l = taps_[i].label();
            if (l.valid && !l.text.empty()) {
                auto& rec = dlsAll_[sid];
                if (rec.text != l.text) { rec.text = l.text; rec.changes++; }
                rec.at = now;
            }
        }
        if (now - scanRotatedAt_ < kScanDwellS) return;
        scanRotatedAt_ = now;
        // the next K audio services in SId order, skipping the one being played
        std::vector<uint32_t> order;
        for (const auto& kv : e.services) {
            const Service& sv = kv.second;
            if (sv.isData || !sv.complete(e.subChannels)) continue;
            const ServiceComponent* pc = sv.primaryComponent();
            if (!pc || pc->tmid != 0 || pc->subChId < 0 || pc->ca) continue;
            if (kv.first == sid_) continue;
            order.push_back(kv.first);
        }
        if (order.empty()) return;
        if (taps_.size() < kScanSlots) taps_.resize(kScanSlots);
        for (size_t i = 0; i < kScanSlots; ++i) {
            const uint32_t sid = order[(scanCursor_ + i) % order.size()];
            taps_[i].reset();
            if (!rx_.scanSelect(i, sid)) rx_.scanSelect(i, 0);
        }
        scanCursor_ = (scanCursor_ + kScanSlots) % order.size();
    }
    static double nowSec() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct DlsRec { std::string text; double at = 0; uint32_t changes = 0; };

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
        /* ★★★ A RATE CHANGE IS NOT A RESTART. This reset the interpolator whenever the input rate
         *  moved — and during the DAB+ start-up glide the measured rate moves on every access
         *  unit, so every unit began with a dropped phase and a re-primed sample: a click per
         *  unit, fading as the estimate converged, gone once the ratio was remembered. Saber, in
         *  the Netherlands where every station is DAB+, heard "weird clicking noises that do
         *  appear to go away" (2026-09-07). The phase and the held samples carry across; only the
         *  step changes. A reset is still right when the STREAM restarts (service change), and
         *  that path calls resampleReset() itself. */
        rsRate_ = srHz;
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
            /* ★ VIBE_DAB_ADTS_DUMP=<path>: the first 500 access units as an ADTS file, so the
             *  platform decoder can be run on the SAME bytes by hand (ffprobe / ffmpeg) when its
             *  output rate is in doubt — the 2026-09-07 "slow on the Pi" measurement. */
            {
                static const char* dumpPath = std::getenv("VIBE_DAB_ADTS_DUMP");
                static FILE* dumpFp = dumpPath ? std::fopen(dumpPath, "wb") : nullptr;
                static int dumped = 0;
                if (dumpFp && dumped < 500) { std::fwrite(pkt.data(), 1, pkt.size(), dumpFp); if (++dumped == 500) { std::fclose(dumpFp); dumpFp = nullptr; fprintf(stderr, "[DAB] ADTS dump written: %s\n", dumpPath); } }
            }
            if (aac_.available()) {
                AacPcm dec;
                /* ★ Count the access unit HERE, whether or not this call returns samples: the
                 *  ffmpeg pipe hands back its output in bursts, so charging two units' samples to
                 *  the one call that received them read 102 kHz where the truth was 51.2 (measured
                 *  both ways, 2026-09-07). */
                /* ★★★ COUNT ONLY ONCE THE DECODER HAS ANSWERED. ffmpeg buffers several access
                 *  units before it returns the first sample, and units written into that buffer
                 *  were counted as programme time with nothing against them — a permanent deficit
                 *  in the window, so the measured rate read low by the pipe's latency. On the UK's
                 *  2-3 units per super frame it stayed under the 1 % override; on a Dutch HE-AAC v1
                 *  service at 48 kHz (6 units per super frame) the same latency is three times the
                 *  fraction, the window "converged" below the remembered ratio, overrode it, and
                 *  every station "started at normal speed then went slow and had to relearn"
                 *  (Stuart, from Saber's box, 2026-09-07). From the first output onwards each
                 *  write yields one unit's worth, so the count starts there. */
                if (s.fmt.accessUnits > 0 && aacPrimed_) aacAuAcc_ += 1;
                if (aac_.decode(pkt.data(), pkt.size(), dec)) {
                    if (!dec.interleaved.empty()) {
                        /* ★★★ THE FIRST BURST IS EXCLUDED, NOT COUNTED. It pays out the units ffmpeg
                         *  buffered while priming — samples with no counted unit against them — and
                         *  counting it biased the rate HIGH (1.0987 against 1.0669, measured on the
                         *  Pi's V4L on a 96 kbit/s service, 2026-09-07), which paced every unit on the
                         *  box fast and gap-filled 9 % of the output: the "vinyl popping". From here
                         *  on each unit written yields one unit's worth out, so the count is unbiased
                         *  whatever the latency. */
                        if (!aacPrimed_) { aacPrimed_ = true; aacAuAcc_ = 0; aacPcmAcc_ = 0.0; aacPrimeBurst_ = true; }
                        /* ★ Frames the decoder actually returned for this access unit. A DAB+ AU
                         *  is a fixed 1024 samples at the decoder's OUTPUT rate for HE-AAC, so
                         *  this number and the reported rate must agree — and if they do not, the
                         *  audio plays at the wrong SPEED while sounding perfectly clean. */
                        if (dec.channels > 0)
                            aacPcmPerAu_ = uint32_t(dec.interleaved.size() / size_t(dec.channels));
                        /* ★★★ THE RATE IS COMPUTED, NEVER BELIEVED — on the server exactly as in
                         *  the browser. ffmpeg is asked for 48 kHz and says 48 kHz, but a decoder
                         *  that cannot do the 960-sample DAB+ transform decodes each access unit
                         *  as 1024 and hands back 6.67 % more samples than 60 ms holds; played at
                         *  the rate it CLAIMS they run slow, which is what Stuart heard on the Pi
                         *  (2026-09-07) while the Xcover's AMediaCodec, which does 960, was
                         *  perfect. The access unit's duration is fixed by the super frame (120 ms
                         *  over 2/3/4/6 AUs), so samples-per-AU over the last few seconds IS the
                         *  true rate. Averaged over AUs because a resampler inside the decoder does
                         *  not return exactly the same count every call. */
                        if (dec.channels > 0 && s.fmt.accessUnits > 0 && !aacPrimeBurst_)
                            aacPcmAcc_ += double(dec.interleaved.size() / size_t(dec.channels));
                        aacPrimeBurst_ = false;
                        /* ★ REMEMBERED, PROCESS-WIDE. Stuart likes the glide but not on every
                         *  return to a station: once the decoder's ratio (samples returned over
                         *  samples due) has been measured, the next service starts from it. */
                        /* ★★★ ONLY FOR A DECODER THAT CANNOT FRAME EXACTLY. AMediaCodec on the
                         *  Xcover returns 960 per unit and its claimed rate is right; it is also
                         *  asynchronous, so measuring its early, bursty output read a false rate
                         *  and paced the audio at it — a start-up ramp on a platform that had
                         *  played DAB+ perfectly that morning (Stuart, 2026-09-07). */
                        double& s_knownRatio = knownRatio_;   // ★ a member now, so it can be kept on disk (see setRatioFile)
                        ++aacAuTotal_;                                   // lifetime, never halved
                        int rate = s_knownRatio > 0.0 ? int(std::lround(double(dec.rateHz) * s_knownRatio)) : dec.rateHz;
                        if (aacAuTotal_ <= 1) aacStartedKnown_ = s_knownRatio > 0.0;
                        /* ★ THE REMEMBERED RATIO WINS UNTIL A CONVERGED MEASUREMENT DISAGREES.
                         *  Letting the fresh measurement override it from the eighth unit re-ran
                         *  the whole glide on every return to a station, because the pipe primes
                         *  again for every service (Stuart, 2026-09-07). And the "converged" test
                         *  read the moving window's counter, which is halved and never reaches
                         *  its own threshold — so the notice never cleared. Lifetime counter now. */
                        const bool converged = aacAuTotal_ >= 160;
                        if (!AacDecoder::kExactFrames && aacAuAcc_ >= 8) {
                            const double auSec = 0.120 / double(s.fmt.accessUnits);
                            const double eff   = aacPcmAcc_ / (double(aacAuAcc_) * auSec);
                            const bool plausible = eff > 8000.0 && eff < 200000.0;
                            const bool deviant   = plausible && std::fabs(eff - double(rate)) > double(rate) * 0.01;
                            if (s_knownRatio <= 0.0) {
                                // Learning: pace at the measurement and adopt it once it has converged.
                                if (deviant) {
                                    if (!aacRateWarned_) { aacRateWarned_ = true;
                                        fprintf(stderr, "[DAB] AAC decoder claims %d Hz but returns %.0f samples/s of programme — pacing at the measured rate\n", dec.rateHz, eff); }
                                    rate = int(std::lround(eff));
                                }
                                if (converged && dec.rateHz > 0) { s_knownRatio = double(rate) / double(dec.rateHz); saveRatio(); }
                            } else {
                                /* ★★★ KNOWN: THE RATIO IS A PROPERTY OF THE DECODER AND DOES NOT MOVE. The
                                 *  first version kept chasing the moving window after convergence, so a
                                 *  burst of lost access units — a sub-second blip — skewed the window by more
                                 *  than 1 %, the rate followed it down, and the audio ran slow until the
                                 *  window had refilled: "normal speed then there was a little blip then it
                                 *  went slow again and had to retrain itself" (Stuart, on Saber's box,
                                 *  2026-09-07; the Pi's journal shows the same: 44706 against 51200). Now the
                                 *  window is only WATCHED: a disagreement must persist for 400 units (a
                                 *  changed ffmpeg, not a blip) before the ratio is relearnt. */
                                aacDeviantRun_ = deviant ? aacDeviantRun_ + 1 : 0;
                                if (aacDeviantRun_ >= 400 && dec.rateHz > 0) {
                                    fprintf(stderr, "[DAB] AAC decoder's output rate has changed: %.0f samples/s against %d expected — relearning\n", eff, rate);
                                    s_knownRatio = eff / double(dec.rateHz); rate = int(std::lround(eff)); saveRatio(); aacDeviantRun_ = 0;
                                }
                            }
                            if (aacAuAcc_ >= 100) { aacPcmAcc_ *= 0.5; aacAuAcc_ /= 2; }   // a moving window
                        }
                        aacEffRateHz_ = rate;
                        pushPcm48Stereo(dec.interleaved.data(), dec.interleaved.size(),
                                        dec.channels, rate);
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
    PadReader  pad_;               ///< dynamic label (and, later, MOT slideshow)
    std::vector<PadTap> taps_;     ///< the PAD scanner's slots — see pumpScan
    uint32_t spiSid_ = 0; int spiAddr_ = -1; uint32_t spiSeen_ = 0;
    uint32_t spiParseRuns_ = 0, spiParsedSvcs_ = 0, spiSiDocs_ = 0;
    PacketAssembler packets_;
    MotCarousel     carousel_;
    std::map<uint32_t, std::vector<SpiLogoRef>> spiLogoRefs_;
    std::map<uint32_t, DlsRec> dlsAll_;
    double scanRotatedAt_ = 0; size_t scanCursor_ = 0;        ///< and WHY a super frame was thrown away
    /** ★ Carried across calls so the 32 kHz -> 48 kHz conversion is ONE continuous stream rather
     *  than one restart per access unit. See pushPcm48Stereo. */
    double     rsPhase_ = 0.0;
    float      rsPrevL_ = 0.0f, rsPrevR_ = 0.0f;
    bool       rsPrimed_ = false;
    int        rsRate_ = 0;     ///< of those, how many were silence covering a lost super frame     ///< 48 kHz stereo frames delivered, ever   ///< PCM FRAMES the decoder returned for the last AU                ///< AUs turned into PCM here rather than on the client
    AudioFormat afmt_{};
    double aacPcmAcc_ = 0.0; int aacAuAcc_ = 0; int aacEffRateHz_ = 0; bool aacRateWarned_ = false;
    bool aacPrimed_ = false;   // the decoder has returned its first sample — counting starts AFTER it
    bool aacPrimeBurst_ = false;   // this output is the priming burst: excluded from the count
    bool aacStartedKnown_ = AacDecoder::kExactFrames; int aacAuTotal_ = 0;
    Slide slide_;
    uint32_t slideSeq_ = 0;
    std::string slideDir_;
    /** ★ The categorised slideshow's gallery (TS 101 499 5.3.5), by CategoryID. Names only — the
     *  pictures themselves stay in the carousel and are fetched by name, so a 64-slide gallery
     *  costs a few hundred bytes rather than a few megabytes. */
    struct CatSlide { std::string name; int slideId = 0; uint32_t seq = 0; };
    struct SlideCategory { std::string title; std::vector<CatSlide> slides; };
    std::map<uint8_t, SlideCategory> cats_;
    int slideAlert_ = 0;
    std::string slideClickUrl_;
    /** ★★★ A CACHE, AND CACHES MUST BE BOUNDED. This holds whole JPEGs — one per service ever
     *  visited, across every multiplex — and nothing evicted them: a server left running while
     *  people tune around grew by ~10 kB a station for ever. The DISK store is the durable copy
     *  (serviceSlide reloads from it in microseconds), so memory here is pure convenience and
     *  can be dropped at any time. Caught auditing my own change from an hour earlier. */
    static constexpr size_t kMaxSlidesHeld = 24;
    std::map<uint32_t, Slide> slideBySid_;   ///< the last picture from each service visited
    std::deque<uint32_t> slideOrder_;        ///< insertion order, for the eviction above
    /* ★ Bounded by construction: ONE pass over the queue, evicting the oldest entries that are
     *  not the playing service. A loop that can put an element back is a loop that can, under
     *  some input nobody has thought of yet, not finish — and this runs on the DSP thread. */
    void slideRemember(uint32_t sid, const Slide& s) {   // caller holds m_
        if (!slideBySid_.count(sid)) slideOrder_.push_back(sid);
        slideBySid_[sid] = s;
        size_t excess = slideOrder_.size() > kMaxSlidesHeld ? slideOrder_.size() - kMaxSlidesHeld : 0;
        for (auto it = slideOrder_.begin(); excess && it != slideOrder_.end(); ) {
            if (*it == sid_ || *it == sid) { ++it; continue; }   // never evict what is playing
            slideBySid_.erase(*it);
            it = slideOrder_.erase(it);
            --excess;
        }
    }
    /** ★ Keyed by the ensemble as well as the SId: an SId is only unique within its ensemble
     *  (that is what the ECC and EId are for), and two multiplexes reusing one SId would
     *  otherwise show each other's artwork. */
    /** ★ The station list asks this for every service on every stats block, and the JSON builder
     *  already holds m_ — so the lock lives in the public wrapper, not here. A `stat` per service
     *  is cheaper than it looks and the answers land in the page cache after the first pass; the
     *  in-memory map answers for everything visited this session without touching the disk. */
    bool haveSlideNoLock(uint32_t sid) const {   // caller holds m_
        if (slideBySid_.count(sid)) return true;
        if (slideDir_.empty() || !sid || !rx_.ensemble().eid) return false;
        for (const char* ext : { "png", "jpg", "gif", "webp" }) {
            struct stat st{};
            if (::stat(slidePath(sid, ext).c_str(), &st) == 0 && st.st_size > 0) return true;
        }
        return false;
    }
    std::string slidePath(uint32_t sid, const char* ext) const {   // caller holds m_
        char b[64];
        snprintf(b, sizeof b, "/%02x-%04x-%08x.%s", unsigned(rx_.ensemble().ecc & 0xFF),
                 unsigned(rx_.ensemble().eid), unsigned(sid), ext);
        return slideDir_ + b;
    }
    static const char* slideExt(const std::string& mime) {
        if (mime == "image/png")  return "png";
        if (mime == "image/jpeg") return "jpg";
        if (mime == "image/gif")  return "gif";
        if (mime == "image/webp") return "webp";
        return nullptr;                       // ★ an unknown type is not written, so it cannot be read back
    }
    void pollSlide() {   // caller holds m_
        MotObject o;
        if (!pad_.mot().take(o)) return;
        /* ★★★ CATEGORISED SLIDESHOW (TS 101 499 clause 5.3.5). A slide that names a category joins
         *  a browsable gallery instead of merely replacing the last picture. Kept BEFORE the body
         *  is moved out of the object, because it is the body we are about to std::move.
         *  ★ CategoryID 0 decategorizes (5.3.5.1) and a category with a null title "shall not be
         *    shown to the user" (5.3.5.3) — both are the transmitter withdrawing a slide, so both
         *    remove rather than add. */
        if (o.categoryId > 0 && o.slideId >= 0) {
            CatSlide cs; cs.name = o.name; cs.slideId = o.slideId; cs.seq = slideSeq_ + 1;
            auto& cat = cats_[uint8_t(o.categoryId)];
            if (!o.categoryTitle.empty()) cat.title = o.categoryTitle;
            /* ★ "When a slide is received containing a CategoryID/SlideID which matches that of
             *  any slide already in the Holding Buffer, the other slides shall be decategorized"
             *  (5.3.5.1) — so a repeated SlideID REPLACES, and a carousel cannot grow for ever. */
            bool replaced = false;
            for (auto& x : cat.slides) if (x.slideId == cs.slideId) { x = cs; replaced = true; break; }
            if (!replaced) cat.slides.push_back(cs);
            std::sort(cat.slides.begin(), cat.slides.end(),
                      [](const CatSlide& a, const CatSlide& b) { return a.slideId < b.slideId; });
            if (cat.slides.size() > 64) cat.slides.resize(64);
        } else if (o.categoryId == 0) {
            for (auto& kv : cats_)
                for (size_t i = 0; i < kv.second.slides.size(); ++i)
                    if (kv.second.slides[i].name == o.name) { kv.second.slides.erase(kv.second.slides.begin() + long(i)); break; }
        }
        /* ★ The Alert parameter (6.2.10, table 4): 1 is an emergency warning and the receiver
         *  "shall switch back to the normal mode of presentation". We do not take over anyone's
         *  screen — same reasoning as the announcement lamp — but the pane says so. */
        slideAlert_ = o.alert;
        slideClickUrl_ = o.clickUrl;
        slide_.bytes = std::move(o.body); slide_.mime = o.mime(); slide_.name = o.name;
        slide_.sid = sid_; slide_.seq = ++slideSeq_;
        /* ★★★ AND KEEP A COPY. `slide_` is cleared by every retune, which is right for the pane
         *  and is exactly why the picture kept having to be read off the air again. */
        if (!sid_ || slide_.bytes.empty()) return;
        slideRemember(sid_, slide_);
        const char* ext = slideExt(slide_.mime);
        if (slideDir_.empty() || !ext || !rx_.ensemble().eid) return;
        mkdir(slideDir_.c_str(), 0755);
        const std::string p = slidePath(sid_, ext);
        const std::string tmp = p + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            const bool ok = fwrite(slide_.bytes.data(), 1, slide_.bytes.size(), f) == slide_.bytes.size();
            fclose(f);
            if (ok) rename(tmp.c_str(), p.c_str()); else remove(tmp.c_str());
        }
    }
    double knownRatio_ = AacDecoder::kExactFrames ? 1.0 : 0.0;   // samples returned / samples due, once measured
    int aacDeviantRun_ = 0;   // consecutive units on which the watched window disagrees with the known ratio
    std::string ratioFile_;
    void saveRatio() {   // caller holds m_
        if (ratioFile_.empty() || knownRatio_ <= 0.0) return;
        const std::string tmp = ratioFile_ + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) { fprintf(f, "%.6f\n", knownRatio_); fclose(f); rename(tmp.c_str(), ratioFile_.c_str()); }
    }   // ★ an exact decoder has nothing to learn — no "setting the clock" flash before its first unit
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
    const DabTxDb* txdb_ = nullptr;
    double rxLat_ = NAN, rxLon_ = NAN;
    uint32_t sid_ = 0, want_ = 0;
};

}  // namespace vibedab
