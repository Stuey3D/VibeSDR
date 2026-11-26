// vibe_maritime_combo.h — V12 beta96 THE MARITIME COMBINATION: AIS + VHF DSC channel 70 from ONE wide capture
// (docs/v12/MARITIME-COMBO.md).
//
// ★★★ STUART (11-26): "do this for HackRFs, SDRplay etc" — the ACARS + VDL2 combination's idea for the sea. AIS (161.975 /
//     162.025 MHz) and DSC channel 70 (156.525 MHz) are 5.5 MHz apart: a radio whose usable capture holds ~6.2 MS/s
//     (an RSP at 8–10, an Airspy R2 at 10, a HackRF, a USRP) sits at 159.2625 MHz — the middle, 2.74 MHz from both, so
//     its DC remnant and the IF filter's roll-off touch neither — and decodes both at once.
// ★★★ THE DATALINK'S MACHINERY, NOT A NEW ONE: vibeavi::MultiChannel (vibe_avi_front.h) — the half-band cascade (NEON /
//     SSE2 / scalar) and ONE forward FFT a block (PFFFT), each channel a slice of its bins inverse-transformed at its own
//     rate. Two slices of ~250 kS/s: one centred on 162.000 MHz holding both AIS channels (their ±7 kHz, the crystal's
//     ±12 kHz AFC, flat to ±62 kHz), fed to AIS's own Front at that rate (vibe_ais.h — the same receiver, the same AFC);
//     one centred on channel 70, cut to ~40 kS/s by a vibedsp::FirDecimator (NEON / SSE2), FM-discriminated and given
//     to the existing DSC receiver (vibe_dsc_run.h, VHF) as 48 kHz audio — exactly what a listener's NFM decoder gets.
// ★★ OFF THE DSP THREAD: all of it runs on the AIS service's own thread (vibe_ais_service.h, PreFront), which drops the
//    OLDEST block rather than wait (Network > Audio > Spectrum > Decoders). The mode sends no audio at all.
// ★ BOUNDED: the MultiChannel's buffers (one FFT block), a 16 k-sample DSC buffer, the DSC receiver's own ~40 KB, the
//   last 20 calls for the report.
// ★ OFFERED only where the capture holds it (fits(): the running USABLE span against minRateHz(), the datalink's 10 % edge
//   rule) — elsewhere AIS stays alone and the combination is not drawn (AGENTS.md: no dead control).
#pragma once
#include "vibe_avi_front.h"
#include "vibe_ais.h"
#include "vibe_ais_service.h"   // vibeais::PreFront — how the AIS service runs the combination
#include "vibe_dsc_run.h"
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vibemar {

using cf32 = vibedsp::cf32;

static constexpr double kDscHz = 156.525e6;          ///< VHF DSC, channel 70
static constexpr const char* kBand = "ais+dsc";      ///< the AIS mode's band word for the combination
static constexpr const char* kName = "AIS + DSC";
/** Each channel's half-width the capture must hold (the datalink bands' convention: a 25 kHz channel). */
static constexpr double kChanHalfHz = 12.5e3;
/** The lowest edge and highest edge held: channel 70 and AIS 2, each ± half a channel. */
inline double loHz() { return kDscHz - kChanHalfHz; }
inline double hiHz() { return vibeais::kChanHz[1] + kChanHalfHz; }
/** ★ Where the radio sits: halfway between channel 70 and AIS's own centre (162.000), so the DC remnant is 2.74 MHz from both. */
inline double centreHz() { return 0.5 * (kDscHz + vibeais::kCentreHz); }
/** The narrowest capture that holds both, with the channeliser's 10 % edge (vibeavi::Band::minRateHz's rule). ≈ 6.17 MS/s. */
inline double minRateHz() { return 2.0 * std::max(centreHz() - loHz(), hiHz() - centreHz()) / 0.9; }
/** ★ Is the combination offered on a radio whose running capture's USABLE span is `usableHz`? (0 = not known: no.) */
inline bool fits(double usableHz) { return usableHz > 0 && usableHz >= minRateHz(); }
/** "AIS + DSC needs a capture of 6.2 MS/s or more — this receiver runs at 2.400 MS/s" */
inline std::string rateWhy(double usableHz) {
    char b[200];
    std::snprintf(b, sizeof b, "AIS + DSC together need a capture of %.1f MS/s or more \xe2\x80\x94 this receiver runs at %.3f MS/s",
                  std::ceil(minRateHz() / 1e5) / 10.0, usableHz / 1e6);
    return b;
}

/** The two slices: AIS (centred on 162.000 MHz) and channel 70. */
static constexpr double kAisSliceHalfHz = 62e3;      ///< what the AIS slice must pass flat (±25 kHz + ±14 kHz filter + ±12 kHz AFC)
static constexpr double kMinSliceRate = 4 * kAisSliceHalfHz * 0.82;   ///< ≥ ~203 kS/s: the slice is flat over half its rate
static constexpr double kDscDevHz = 5000.0;          ///< FM deviation that reads as 1.0 in the DSC audio
static constexpr double kDscPassHz = 8000.0;         ///< channel 70's FM signal (1300/2100 Hz tones, ±5 kHz): ±8 kHz passed

class Combo {
public:
    /** What the DSC side gives: each call (the gallery's copy — line, JSON, repeat), once a second (the gallery's flush),
     *  and at the end. Called on the AIS service's thread. */
    struct Hooks {
        std::function<void(const vibe::dsc::Call&, const std::string& line, const std::string& json, bool repeat)> call;
        std::function<void()> tick, finish;
    };
    /** `rate` the capture rate (the source's, not the usable span), `hwCentreHz` where the radio is tuned (centreHz()). */
    Combo(double rate, double hwCentreHz, Hooks hooks)
        : mc_(rate, hwCentreHz, { vibeais::kCentreHz, kDscHz }, kAisSliceHalfHz, kMinSliceRate, 2500.0), hooks_(std::move(hooks)) {
        const double R = mc_.chanRate();
        const int d = std::max(1, int(std::floor(R / 40e3)));
        dscRate_ = R / d;
        const double pass = kDscPassHz, stop = std::min(dscRate_ - kDscPassHz, 20e3);
        dscDec_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(0.5 * (pass + stop) / R, (stop - pass) / R, true), d));
        step_ = dscRate_ / 48000.0;
        vibe::DscRun::Out o;
        o.json = [this](const std::string& j) { onJson_(j); };
        o.call = [this](const vibe::dsc::Call& c, const std::string& line, const std::string& j, bool rep) {
            if (hooks_.call) hooks_.call(c, line, j, rep);
        };
        dsc_.reset(new vibe::DscRun(true, 1700, [] { return kDscHz; }, std::move(o)));
    }
    ~Combo() { finish(); }

    /** Both slices fall inside the capture (always so where fits(); a test's own rate may not be). */
    bool ok() const { return mc_.inside(0) && mc_.inside(1); }
    double rate() const { return mc_.rate(); }
    double aisRate() const { return mc_.chanRate(); }
    double dscRate() const { return dscRate_; }
    int fftSize() const { return mc_.fftSize(); }
    int stages() const { return mc_.stages(); }

    /** A block of the wide capture: `ais(const cf32* z, int n)` gets the AIS slice (at aisRate(), centred on 162.000 MHz);
     *  channel 70 is decoded here. */
    template <class Ais> void feed(const cf32* iq, size_t n, Ais&& ais) {
        mc_.feed(iq, n, [&](int c, const cf32* z, int k) {
            if (c == 0) ais(z, k);
            else dscSlice_(z, k);
        });
    }
    void tick() { if (hooks_.tick) hooks_.tick(); }
    void finish() { if (!finished_) { finished_ = true; if (hooks_.finish) hooks_.finish(); } }

    unsigned long calls() const { std::lock_guard<std::mutex> lk(m_); return calls_; }
    unsigned long syncs() const { return dsc_ ? dsc_->stats().syncs : 0; }
    /** ,"combo":"ais+dsc","hw":…,"dsc":{"hz":156525000,"calls":N,"syncs":S,"recent":[…the last calls' data, newest last…]} */
    std::string reportFields(double hwCentreHz) const {
        std::lock_guard<std::mutex> lk(m_);
        char b[200];
        std::snprintf(b, sizeof b, ",\"combo\":\"%s\",\"hw\":%.0f,\"dsc\":{\"hz\":%.0f,\"calls\":%lu,\"syncs\":%lu,\"recent\":[",
                      kBand, hwCentreHz, kDscHz, calls_, dsc_ ? dsc_->stats().syncs : 0ul);
        std::string o = b;
        for (size_t i = 0; i < recent_.size(); ++i) { if (i) o += ','; o += recent_[i]; }
        return o + "]}";
    }

private:
    /** Channel 70's slice → ~40 kS/s → the FM discriminator → 48 kHz → the DSC receiver. */
    void dscSlice_(const cf32* z, int k) {
        nb_.resize(size_t(dscDec_->maxOut(k)) + 16);
        const int m = dscDec_->process(z, k, nb_.data());
        audio_.clear();
        const float scale = float(dscRate_ / (2.0 * M_PI * kDscDevHz));
        for (int i = 0; i < m; ++i) {
            const cf32 s = nb_[size_t(i)];
            const cf32 p = s * std::conj(prev_);
            prev_ = s;
            const float x = std::atan2(p.imag(), p.real()) * scale;
            // ★ linear interpolation up to exactly 48 kHz (the tones are ≤ 2.1 kHz at ~40 kS/s: images far above them)
            while (t_ <= 1.0) { audio_.push_back(float(prevA_ + t_ * (x - prevA_))); t_ += step_; }
            t_ -= 1.0;
            prevA_ = x;
        }
        if (!audio_.empty()) dsc_->feedAudio(audio_.data(), int(audio_.size()));
    }
    void onJson_(const std::string& j) {
        // {"type":"dsc_call","data":{…}} → keep the data object (bounded: the last 20)
        const std::string key = "\"data\":";
        const size_t p = j.find(key);
        if (p == std::string::npos || j.size() < p + key.size() + 1) return;
        std::string d = j.substr(p + key.size(), j.size() - (p + key.size()) - 1);
        std::lock_guard<std::mutex> lk(m_);
        // a repeat of a call already listed replaces it (the same id)
        const size_t ip = d.find("\"id\":");
        const std::string id = ip == std::string::npos ? std::string() : d.substr(ip, d.find(',', ip) - ip);
        bool replaced = false;
        if (!id.empty()) for (auto& r : recent_) if (r.find(id + ",") != std::string::npos) { r = d; replaced = true; break; }
        if (!replaced) { recent_.push_back(std::move(d)); ++calls_; while (recent_.size() > kRecent) recent_.pop_front(); }
    }

    static constexpr size_t kRecent = 20;
    vibeavi::MultiChannel mc_;
    Hooks hooks_;
    std::unique_ptr<vibedsp::FirDecimator> dscDec_;
    std::unique_ptr<vibe::DscRun> dsc_;
    double dscRate_ = 0, step_ = 1, t_ = 0;
    float prevA_ = 0;
    cf32 prev_{1, 0};
    std::vector<cf32> nb_;
    std::vector<float> audio_;
    mutable std::mutex m_;
    std::deque<std::string> recent_;
    unsigned long calls_ = 0;
    bool finished_ = false;
};

/** ★ The combination as the AIS service's pre-front (vibeais::Service::start(…, pre)): one Service, one thread, the same
 *  ship list, store and gallery as AIS alone — channel 70's calls beside them. */
class ComboFront : public vibeais::PreFront {
public:
    ComboFront(double rate, double hwCentreHz, Combo::Hooks hooks) : c_(rate, hwCentreHz, std::move(hooks)) {}
    double aisRate() const override { return c_.aisRate(); }
    void feed(const vibeais::cf* iq, size_t n, const std::function<void(const vibeais::cf*, int)>& ais) override {
        c_.feed(reinterpret_cast<const cf32*>(iq), n, [&](const cf32* z, int k) { ais(reinterpret_cast<const vibeais::cf*>(z), k); });
    }
    void tick() override { c_.tick(); }
    void finish() override { c_.finish(); }
    std::string reportFields(double hwCentreHz) const override { return c_.reportFields(hwCentreHz); }
    Combo& combo() { return c_; }
private:
    Combo c_;
};

}  // namespace vibemar
