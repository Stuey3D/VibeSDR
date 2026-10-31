// vibe_lora_service.h — V12 ISM mode's LoRa engine: the channel plans by band, the governor that decides how many
// channel × spreading-factor detectors run, and the worker thread that channelises the capture and runs them.
// docs/v12/LORA-MESH.md.
//
// ★★★ ITS OWN THREADS, AND IT DROPS RATHER THAN WAITS (Network > Audio > Spectrum > Decoders — ADS-B's and ISM's rule).
//     feed() copies the block and returns; a full queue (half a second of capture) loses its OLDEST block and counts it.
// ★★★ THREE THREADS (2026-10-31, the Pi 2 thread split): vibe-lora the channeliser's capture FFT and each channel's bins
//     × filter; vibe-lora2 each channel's inverse FFT, rotation and resampler; vibe-lora3 the detectors. The later two one
//     notch lower. Half a second of work may wait before each; past that the OLDEST goes, counted with the rest. On the
//     Pi 2 the default 433 set was one thread at ~70 % of a core, the 868 default set (five channels, twenty detectors)
//     over a whole core (it dropped). Cut where the Pi 2's own profile put the weight: the FFT, the channels, the detectors.
//     The sensor decoder (vibe_ism_service.h) has its own queue, so a LoRa backlog never costs a weather station.
// ★★ ONLY CHANNELS INSIDE THE CAPTURE RUN. A plan channel whose edge falls past ±45 % of the capture rate is left out
//    (the report says how many the capture holds).
#pragma once
#include "vibe_lora.h"
#include "vibe_lorawan.h"
#include "vibe_meshtastic.h"
#include "vibe_meshcore.h"
#include "vibe_thread.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace vibelora {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE CHANNEL PLANS — what is listened for, by band
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct PlanCh {
    const char* name;          ///< "LoRaWAN 868.1"
    const char* net;           ///< "lorawan" | "meshtastic" | "meshcore" — what is expected there (the sync word decides)
    double hz, bw;
    std::vector<int> sfs;
    std::vector<uint8_t> syncs;
    bool inv;                  ///< inverted IQ (LoRaWAN downlinks)
    int tier;                  ///< 0 = the default set (runs on the minimum-spec Pi 2); 1, 2 = added as the budget allows
};
/** ★ Sources per channel in LORA-MESH.md §Plans: LoRaWAN Regional Parameters (EU868 default channels 868.1/.3/.5 and the
 *  common 867.1-867.9 additions, RX2 869.525; EU433 433.175/.375/.575; US915 sub-band 8); meshtastic.org radio-settings
 *  (EU_868 869.4-869.65 MHz, one 250 kHz slot at 869.525; EU_433 433-434 MHz, LongFast on 433.875); MeshCore's preset
 *  list (EU/UK Narrow 869.618 SF8 BW62.5; NL/PT/SK/HU 869.618 SF7; CZ 869.432 SF7; EU 433 Narrow 433.650 SF8). */
inline const std::vector<PlanCh>& planFor(const std::string& band) {
    static const std::vector<uint8_t> LW = {0x34, 0x12}, MT = {0x2B, 0x12}, MC = {0x12};
    static const std::vector<int> ALL = {7, 8, 9, 10, 11, 12};
    static const std::vector<PlanCh> eu868 = {
        {"Meshtastic LongFast 869.525", "meshtastic", 869.525e6, 250e3, {11}, MT, false, 0},
        {"MeshCore EU/UK 869.618", "meshcore", 869.618e6, 62.5e3, {8}, MC, false, 0},
        {"LoRaWAN 868.1", "lorawan", 868.1e6, 125e3, ALL, LW, false, 0},
        {"LoRaWAN 868.3", "lorawan", 868.3e6, 125e3, ALL, LW, false, 0},
        {"LoRaWAN 868.5", "lorawan", 868.5e6, 125e3, ALL, LW, false, 0},
        {"MeshCore 869.618 SF7", "meshcore", 869.618e6, 62.5e3, {7}, MC, false, 1},
        {"Meshtastic 869.525 other presets", "meshtastic", 869.525e6, 250e3, {7, 8, 9, 10}, MT, false, 1},
        {"LoRaWAN 867.1", "lorawan", 867.1e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 867.3", "lorawan", 867.3e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 867.5", "lorawan", 867.5e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 867.7", "lorawan", 867.7e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 867.9", "lorawan", 867.9e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN RX2 869.525 (downlink)", "lorawan", 869.525e6, 125e3, {9, 12}, LW, true, 2},
        {"Meshtastic LongModerate 869.4625", "meshtastic", 869.4625e6, 125e3, {11}, MT, false, 2},
        {"Meshtastic LongModerate 869.5875", "meshtastic", 869.5875e6, 125e3, {11}, MT, false, 2},
        {"MeshCore CZ 869.432", "meshcore", 869.432e6, 62.5e3, {7}, MC, false, 2},
    };
    static const std::vector<PlanCh> eu433 = {
        {"Meshtastic LongFast 433.875", "meshtastic", 433.875e6, 250e3, {11}, MT, false, 0},
        {"MeshCore EU 433 Narrow 433.650", "meshcore", 433.650e6, 62.5e3, {8}, MC, false, 0},
        {"LoRaWAN 433.175", "lorawan", 433.175e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 433.375", "lorawan", 433.375e6, 125e3, ALL, LW, false, 1},
        {"LoRaWAN 433.575", "lorawan", 433.575e6, 125e3, ALL, LW, false, 1},
        {"MeshCore EU 433 Long Range 433.650", "meshcore", 433.650e6, 250e3, {11}, MC, false, 2},
        {"MeshCore PT 433.375", "meshcore", 433.375e6, 62.5e3, {9}, MC, false, 2},
    };
    static const std::vector<PlanCh> us915 = {
        {"LoRaWAN US915 914.3", "lorawan", 914.3e6, 125e3, {7, 8, 9, 10}, LW, false, 0},
        {"LoRaWAN US915 914.5", "lorawan", 914.5e6, 125e3, {7, 8, 9, 10}, LW, false, 0},
        {"LoRaWAN US915 914.7", "lorawan", 914.7e6, 125e3, {7, 8, 9, 10}, LW, false, 1},
        {"LoRaWAN US915 914.9", "lorawan", 914.9e6, 125e3, {7, 8, 9, 10}, LW, false, 1},
    };
    static const std::vector<PlanCh> none;
    if (band == "868") return eu868;
    if (band == "433") return eu433;
    if (band == "915") return us915;
    return none;
}

/** ★ A detector's relative cost: its sample rate × (the FFT's log2 N + the dechirp and peak search). One unit = an SF7
 *  detector on a 125 kHz channel. The channeliser's own FFT is a fixed cost and is not in the budget. */
inline double detCost(double bw, int sf) { return bw / 125e3 * (sf + 3.0) / 10.0; }
inline double chanCost(const PlanCh& c) { double u = 0; for (int sf : c.sfs) u += detCost(c.bw, sf) * (c.inv ? 1.0 : 1.0); return u; }

/** Which plan channels run: everything inside the capture, the default tier always (never hidden by the benchmark —
 *  only the owner switches LoRa off), then the next tiers while `budgetUnits` lasts (∞ = all; "all" forced by the owner). */
inline std::vector<const PlanCh*> choose(const std::vector<PlanCh>& plan, double hwCentreHz, double rateHz, double budgetUnits,
                                         double edge = 0.45) {
    std::vector<const PlanCh*> out;
    double used = 0;
    for (int tier = 0; tier <= 2; ++tier)
        for (const auto& c : plan) {
            if (c.tier != tier) continue;
            if (std::fabs(c.hz - hwCentreHz) + c.bw / 2 > edge * rateHz + 1) continue;
            const double u = chanCost(c);
            if (tier > 0 && used + u > budgetUnits) continue;
            used += u; out.push_back(&c);
        }
    return out;
}

/** ★★ THE BENCHMARK SETS THE DEFAULT, NEVER HIDES (Stuart's rule). `pctDefault`: the benchmark's dec_lora row — the 868
 *  default tier's cost, % of one core, as the WHOLE engine's work: since LoRa runs on three threads the row is scored by
 *  its hottest thread, and the shim's vsLoraBenchPct reads the sum of its vibe-lora threads from the row's detail (a
 *  result from before the split carries the inline total, the same quantity) — so this arithmetic is unchanged. The default tier always runs while LoRa is on; further tiers are added while the
 *  whole LoRa engine would stay under kCorePct of one core. No benchmark yet: every tier on 64-bit machines, the default
 *  tier only on 32-bit ARM (the Pi 2 — the minimum spec). The owner can force everything ("lora+all") or switch LoRa off. */
static constexpr double kCorePct = 25.0;
inline double budgetFromBench(double pctDefault) {
    double units = 0;
    for (const auto& c : planFor("868")) if (c.tier == 0) units += chanCost(c);
    if (!(pctDefault > 0)) {
#if defined(__arm__)
        return 0;
#else
        return INFINITY;
#endif
    }
    return units * (kCorePct / pctDefault);
}

/** One decoded frame with what it said. */
struct Heard { Frame f; Msg m; };

/** The protocol of a frame: by its sync word first, then what the channel expects. */
inline Msg interpret(const Frame& f, const char* expect) {
    Msg m;
    bool ok = false;
    if (f.sync == 0x2B) ok = meshtastic::parse(f.bytes, m);
    else if (f.sync == 0x34) ok = lorawan::parse(f.bytes, f.crcOn, m);
    else if (f.sync == 0x12) {
        if (std::string(expect) == "meshcore" || std::string(expect) == "meshtastic") ok = meshcore::parse(f.bytes, m);
        if (!ok) { m = Msg(); ok = lorawan::parse(f.bytes, f.crcOn, m); if (ok) m.net = "private network"; }
    }
    if (!ok) { m = Msg(); m.proto = "lora"; m.what = "frame"; }
    m.sf = f.sf; m.bw = f.bw; m.cr = f.cr; m.sync = f.sync; m.len = int(f.bytes.size());
    m.freqHz = f.freqHz; m.snrDb = f.snrDb; m.levelDb = f.levelDb;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE ENGINE
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Engine {
public:
    ~Engine() { stop(); }
    /** Start on `band` ("868"…) with the radio at `hwCentreHz`, `rateHz`. Returns the channels that run. */
    size_t start(const std::string& band, double hwCentreHz, double rateHz, double budgetUnits, double edge = 0.45) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        hc_ = hwCentreHz; rate_ = rateHz;
        chans_.clear(); demods_.clear(); streams_.clear(); names_.clear();
        const auto& plan = planFor(band);
        planSize_ = plan.size();
        chosen_ = choose(plan, hwCentreHz, rateHz, budgetUnits, edge);
        if (chosen_.empty()) return 0;
        ch_.reset(new Channeliser(rateHz));
        // ★ one stream per distinct (frequency, bandwidth) — Meshtastic's two 869.525 entries share theirs
        for (const PlanCh* c : chosen_) {
            int si = -1;
            for (size_t i = 0; i < chans_.size(); ++i) if (chans_[i].hz == c->hz && chans_[i].bw == c->bw) si = int(i);
            if (si < 0) {
                streams_.emplace_back(new Stream(c->bw, 17));
                ch_->add(c->hz - hwCentreHz, c->bw, streams_.back().get());
                chans_.push_back({c->hz, c->bw});
                si = int(streams_.size()) - 1;
            }
            for (int sf : c->sfs) {
                Demod::Cfg k; k.sf = sf; k.bw = c->bw; k.invertIq = c->inv; k.carrierHz = c->hz; k.syncs = c->syncs; k.name = c->name;
                k.minPreamble = 4;
                k.searchStride = sf >= 11 ? 2 : 1;   // ★ SF11/12 search every other window: half their search cost, no loss (test-lora)
                demods_.push_back({std::unique_ptr<Demod>(new Demod(k)), size_t(si), c});
            }
        }
        queue_.clear(); queued_ = 0; dropped_ = 0;
        specQ_.clear(); specQueued_ = 0; droppedSpec_ = 0;
        specCap_ = size_t(0.5 * rateHz / (0.75 * ch_->fftSize())) + 1;   // half a second of blocks (a block steps 3/4 of the FFT)
        chipQ_.clear(); chipQueued_ = 0; droppedChip_ = 0;
        { double bw = 0; for (const auto& c : chans_) bw += c.bw; chipCap_ = size_t(0.5 * bw) + 1; }   // half a second of chips
        run_ = true;
        worker_ = std::thread([this] { loop(); });
        worker2_ = std::thread([this] { loop2(); });
        worker3_ = std::thread([this] { loop3(); });
        return chosen_.size();
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        cv_.notify_all(); space_.notify_all();
        { std::lock_guard<std::mutex> lk(sMtx_); } sCv_.notify_all(); sSpace_.notify_all();
        { std::lock_guard<std::mutex> lk(cMtx_); } cCv_.notify_all(); cSpace_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (worker2_.joinable()) worker2_.join();
        if (worker3_.joinable()) worker3_.join();
    }
    /** ★ Tests and ism-offline only, before start(): full queues WAIT instead of dropping. */
    void setLossless(bool on) { lossless_ = on; }
    /** ★ Tests: nothing queued and nothing being worked on. */
    bool idle() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!queue_.empty() || busy1_) return false; }
        { std::lock_guard<std::mutex> lk(sMtx_); if (!specQ_.empty() || busy2_) return false; }
        { std::lock_guard<std::mutex> lk(cMtx_); if (!chipQ_.empty() || busy3_) return false; }
        return true;
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    /** From the DSP thread (via the ISM service): copy and return; never waits on the decoder. */
    void feed(const cf* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || !n) return;
        std::vector<cf> b(iq, iq + n);
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (lossless_) space_.wait(lk, [&] { return !run_ || queue_.empty() || queued_ + n <= size_t(rate_ * 0.5); });
            queued_ += n;
            queue_.push_back(std::move(b));
            while (queued_ > size_t(rate_ * 0.5) && queue_.size() > 1) { queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_; }
        }
        cv_.notify_one();
    }
    /** Everything decoded since the last call. */
    std::vector<Heard> take() { std::lock_guard<std::mutex> lk(outMtx_); std::vector<Heard> v; v.swap(out_); return v; }
    /** Capture blocks lost before the first thread, transformed blocks before the second, chips before the third. */
    uint64_t dropped() {
        uint64_t n = 0;
        { std::lock_guard<std::mutex> lk(mtx_); n += dropped_; }
        { std::lock_guard<std::mutex> lk(sMtx_); n += droppedSpec_; }
        { std::lock_guard<std::mutex> lk(cMtx_); n += droppedChip_; }
        return n;
    }
    size_t channelsRun() const { return chosen_.size(); }
    size_t planSize() const { return planSize_; }
    size_t detectors() const { return demods_.size(); }
    std::vector<std::string> channelNames() const { std::vector<std::string> v; for (auto* c : chosen_) v.push_back(c->name); return v; }
    /** Frames with a good CRC / headers that failed / CRCs that failed, across every detector (for the status). */
    void counts(uint64_t& ok, uint64_t& badHdr, uint64_t& badCrc) {
        std::lock_guard<std::mutex> lk(statMtx_); ok = okN_; badHdr = hdrN_; badCrc = crcN_;
    }

    /** Per detector: preambles seen, SFDs aligned, headers failed, CRCs failed, frames — for lora-offline and the tests. */
    std::string stats() const {
        std::string s;
        for (const auto& dt : demods_) {
            const Demod& d = *dt.d;
            if (!d.preambles()) continue;
            char b[200];
            std::snprintf(b, sizeof b, "  %-34s SF%-2d preambles %4llu  SFDs %4llu  bad headers %3llu  bad CRCs %3llu  frames %4llu\n",
                          dt.ch->name, d.cfg().sf, (unsigned long long)d.preambles(), (unsigned long long)d.sfds(),
                          (unsigned long long)d.badHeaders(), (unsigned long long)d.badCrc(), (unsigned long long)d.found());
            s += b;
        }
        return s;
    }
#if defined(VIBE_ISM_PROF)
    double profChan = 0, profDemod = 0;   ///< seconds: the channeliser, and every detector (ism-offline's profile)
    const Channeliser* channeliser() const { return ch_.get(); }
#endif
    /** ★ For the tests and the offline runner: the same work as the thread, inline. */
    void process(const cf* x, size_t n) { work(x, n); }
    /** `edge`: how far out a channel may reach, as a share of the rate (0.5 = a narrow recording made for ONE channel). */
    void startInline(const std::string& band, double hwCentreHz, double rateHz, double budgetUnits, double edge = 0.45) {
        start(band, hwCentreHz, rateHz, budgetUnits, edge);
        stop();      // the worker is not wanted; the channels and detectors stay built
    }

private:
    struct Det { std::unique_ptr<Demod> d; size_t stream; const PlanCh* ch; };
    struct ChF { double hz, bw; };
    void work(const cf* x, size_t n) {
        if (!ch_) return;
        VIBE_ISM_P(const double tp0 = vibeism::ismProfNow();)
        ch_->feed(x, n);
        VIBE_ISM_P(profChan += vibeism::ismProfNow() - tp0;)
        detectAll();
    }
    /** Every detector over what its stream now holds; frames out, counts kept. */
    void detectAll() {
        VIBE_ISM_P(const double tp1 = vibeism::ismProfNow();)
        std::vector<Heard> got;
        for (auto& dt : demods_) {
            dt.d->run(*streams_[dt.stream], [&](Frame&& f) {
                f.freqHz = dt.ch->hz + f.cfoHz;
                Heard h; h.m = interpret(f, dt.ch->net); h.f = std::move(f);
                got.push_back(std::move(h));
            });
        }
        VIBE_ISM_P(profDemod += vibeism::ismProfNow() - tp1;)
        uint64_t hb = 0, cb = 0;
        for (auto& dt : demods_) { hb += dt.d->badHeaders(); cb += dt.d->badCrc(); }
        { std::lock_guard<std::mutex> lk(statMtx_); okN_ += got.size(); hdrN_ = hb; crcN_ = cb; }
        if (!got.empty()) {
            std::lock_guard<std::mutex> lk(outMtx_);
            for (auto& h : got) out_.push_back(std::move(h));
            if (out_.size() > 512) out_.erase(out_.begin(), out_.begin() + long(out_.size() - 512));
        }
    }
    // ── vibe-lora: the transforms (the expensive half of the channeliser) ──
    void loop() {
        /* ★★ NAMED AND AT THE DECODERS' RUNG (2026-10-31). Unnamed, it inherited whatever thread started ISM mode — on the
         *  Pi 2 a connection thread, so /proc called it "vibe-conn" at nice 0, level with nothing it should be level with. */
        vibeDecoderThread("vibe-lora");
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<cf> blk = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= blk.size();
                busy1_ = true;
                lk.unlock();
                space_.notify_all();
                std::vector<Channeliser::Spectra> batch;
                ch_->feedSpectra(blk.data(), blk.size(), [&](Channeliser::Spectra&& sp) { batch.push_back(std::move(sp)); });
                // ★ one batch per capture block: the detectors then run exactly as often as they did on one thread
                if (!batch.empty()) pushSpec(std::move(batch));
                lk.lock();
                busy1_ = false;
            }
        }
    }
    void pushSpec(std::vector<Channeliser::Spectra>&& batch) {
        {
            std::unique_lock<std::mutex> lk(sMtx_);
            if (lossless_) sSpace_.wait(lk, [&] { return !run_ || specQ_.empty() || specQueued_ + batch.size() <= specCap_; });
            specQueued_ += batch.size();
            specQ_.push_back(std::move(batch));
            while (specQueued_ > specCap_ && specQ_.size() > 1) { specQueued_ -= specQ_.front().size(); specQ_.pop_front(); ++droppedSpec_; }
        }
        sCv_.notify_one();
    }
    // ── vibe-lora2: each channel's inverse FFT, rotation and resampler — may lag the transforms ──
    void loop2() {
        vibeDecoderLagThread("vibe-lora2");
        std::unique_lock<std::mutex> lk(sMtx_);
        while (run_) {
            sCv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !specQ_.empty(); });
            while (run_ && !specQ_.empty()) {
                std::vector<Channeliser::Spectra> batch = std::move(specQ_.front());
                specQ_.pop_front();
                specQueued_ -= batch.size();
                busy2_ = true;
                lk.unlock();
                sSpace_.notify_all();
                Channeliser::ChipOut out;
                for (auto& sp : batch) ch_->deliver(sp, &out);
                pushChip(std::move(out));   // ★ one per capture block: the detectors run exactly as often as on one thread
                lk.lock();
                busy2_ = false;
            }
        }
    }
    static size_t chipsOf(const Channeliser::ChipOut& o) { size_t n = 0; for (const auto& v : o) n += v.size(); return n; }
    void pushChip(Channeliser::ChipOut&& out) {
        const size_t n = chipsOf(out);
        {
            std::unique_lock<std::mutex> lk(cMtx_);
            if (lossless_) cSpace_.wait(lk, [&] { return !run_ || chipQ_.empty() || chipQueued_ + n <= chipCap_; });
            chipQueued_ += n;
            chipQ_.push_back(std::move(out));
            while (chipQueued_ > chipCap_ && chipQ_.size() > 1) { chipQueued_ -= chipsOf(chipQ_.front()); chipQ_.pop_front(); ++droppedChip_; }
        }
        cCv_.notify_one();
    }
    // ── vibe-lora3: the detectors over what each stream now holds ──
    void loop3() {
        vibeDecoderLagThread("vibe-lora3");
        std::unique_lock<std::mutex> lk(cMtx_);
        while (run_) {
            cCv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !chipQ_.empty(); });
            while (run_ && !chipQ_.empty()) {
                Channeliser::ChipOut out = std::move(chipQ_.front());
                chipQ_.pop_front();
                chipQueued_ -= chipsOf(out);
                busy3_ = true;
                lk.unlock();
                cSpace_.notify_all();
                ch_->pushOut(out);
                detectAll();
                lk.lock();
                busy3_ = false;
            }
        }
    }

    std::mutex mtx_, outMtx_, statMtx_;
    std::condition_variable cv_, space_;
    std::thread worker_, worker2_, worker3_;
    bool lossless_ = false, busy1_ = false, busy2_ = false, busy3_ = false;
    // ★ between the two threads: transformed blocks, one batch per capture block
    std::mutex sMtx_;
    std::condition_variable sCv_, sSpace_;
    std::deque<std::vector<Channeliser::Spectra>> specQ_;
    size_t specQueued_ = 0, specCap_ = 0;
    uint64_t droppedSpec_ = 0;
    // ★ between the second and third: each channel's chip-rate samples, one batch per capture block
    std::mutex cMtx_;
    std::condition_variable cCv_, cSpace_;
    std::deque<Channeliser::ChipOut> chipQ_;
    size_t chipQueued_ = 0, chipCap_ = 0;
    uint64_t droppedChip_ = 0;
    std::atomic<bool> run_{false};
    double hc_ = 0, rate_ = 0;
    std::unique_ptr<Channeliser> ch_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::vector<ChF> chans_;
    std::vector<Det> demods_;
    std::vector<const PlanCh*> chosen_;
    std::vector<std::string> names_;
    size_t planSize_ = 0;
    std::deque<std::vector<cf>> queue_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0, okN_ = 0, hdrN_ = 0, crcN_ = 0;
    std::vector<Heard> out_;
};

} // namespace vibelora
