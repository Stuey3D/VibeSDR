// balloon_rx.h — V12 balloons: ONE receiver for everything a balloon payload sends on its frequency (docs/v12/BALLOONS.md).
//
// A listener tunes a payload in USB (434 MHz, usually) and opens BALLOONS. Everything below runs on their audio, at once:
//   • UKHAS RTTY — the RTTY decoder we already have (decoders/fsk_decoder.h, its demodulator, ATC and start-bit UART),
//     run as a bank of candidates: 50 / 75 / 100 / 150 / 300 / 600 baud × 7 or 8 data bits × either polarity, all on the
//     tone pair the spectrum shows (any shift 150–1200 Hz — balloon transmitters are not on the teleprinter shifts, so
//     nothing is snapped). A 1-stop decoder that waits for each start bit also reads 1.5- and 2-stop signals, so stop bits
//     need no search. ★ The CRC is the judge: a sentence from ANY candidate whose CRC-16 passes is telemetry (the same
//     sentence from several candidates is shown once), and that candidate is "what it is sending". Frame scores only
//     name the settings while nothing has passed yet.
//   • SSDV pictures — the 8-bit candidates' raw bytes go to a packet framer (balloon_ssdv.h): CRC-32, Reed–Solomon.
//   • Horus Binary v1 / v2 — four equally spaced tones in the same spectrum start our 4-FSK demodulator
//     (balloon_horus.h); its packets are CRC-16 checked and written out as UKHAS sentences too.
// ★ MANUAL CONTROLS UNDER AUTO (Stuart's rule for automatic decoders): any of baud, data bits, polarity and shift can be
//   pinned — the search then runs only what is left — and Horus's baud and tone spacing likewise.
// ★ Bounded: a fixed bank (at most 24 RTTY decoders), sentence framers of 300 characters, a 256-byte SSDV window per
//   8-bit candidate, three pictures in progress. Runs on the decoder host's own thread (decoders are last in Stuart's
//   order: Network > Audio > Spectrum > Decoders); the 48 → 12 kHz front end and the Horus tone bank are SIMD.
#pragma once
#include "balloon_dsp.h"
#include "balloon_horus.h"
#include "balloon_ssdv.h"
#include "balloon_ukhas.h"
#include "fsk_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace balloon {

struct Config {
    bool rtty = true, horus = true;
    double rttyBaud = 0;     ///< 0 = search 50…600
    int    rttyBits = 0;     ///< 0 = both 7 and 8
    int    rttyReverse = -1; ///< −1 = both polarities
    double rttyShift = 0;    ///< 0 = whatever the spectrum shows (150–1200 Hz)
    double horusBaud = 100;
    double horusSpacing = 0; ///< 0 = whatever the spectrum shows (80–1500 Hz)
};

inline const std::vector<double>& searchBauds() { static const std::vector<double> b{ 50, 75, 100, 150, 300, 600 }; return b; }

class Receiver {
public:
    static constexpr int kFs = 12000;
    std::function<void(const Telemetry&)> onTelemetry;
    /** A picture got a new packet (`im` is valid during the call only). */
    std::function<void(ssdv::Image& im, const ssdv::Packet& p)> onImagePacket;
    /** A line that looked like a sentence but failed its CRC (`cand` = which RTTY candidate) — for logs and tests. */
    std::function<void(const std::string& line, int cand)> onRejected;

    explicit Receiver(const Config& c = Config()) : cfg_(c), horus_(c.horusBaud) {
        horus_.onPacket = [this](const Telemetry& t) { lastHorusAt_ = sec_; deliver_(t, -1); };
    }
    const Config& config() const { return cfg_; }

    /** The listener's 48 kHz audio (±1). */
    void feed48k(const float* x, int n) {
        d12_.clear();
        dec_.process(x, n, d12_);
        if (!d12_.empty()) feed12k(d12_.data(), (int)d12_.size());
    }
    /** 12 kHz audio (tests may feed this directly). */
    void feed12k(const float* x, int n) {
        samples_ += n; sec_ = samples_ / (double)kFs;
        if (spec_.feed(x, n)) onSpectrum_();   // ★ may start a decoder, which then reads the history (older than x)
        if (cfg_.horus) horus_.process(x, n);
        if (cfg_.rtty && !cands_.empty()) { toPcm_(x, n, pcm_); for (auto& c : cands_) c->d->process(pcm_.data(), n); }
        // ★ the last few seconds are kept, so a decoder the spectrum starts (or restarts) reads what it was late for — the
        //   first sentence, packet or picture packet after a quiet carrier would otherwise be lost while the spectrum settles
        for (int i = 0; i < n; ++i) { hist_[(size_t)histPos_] = x[i]; if (++histPos_ == kHist) histPos_ = 0; }
        histFill_ = std::min<long long>(kHist, histFill_ + n);
    }

    // ── what it is doing, for the status line ──
    /** 0 nothing heard · 1 a signal (tones) · 3 decoding (a sentence or packet in the last 30 s). */
    int state() const {
        if (sec_ - lastGoodAt_ < 30 && lastGoodAt_ > 0) return 3;
        return (centre_ > 0 && sec_ - pairAt_ < 5) || (horus_.haveTones() && sec_ - fourAt_ < 5) ? 1 : 0;
    }
    double rttyCentre() const { return centre_; }
    double rttyShift() const { return shift_; }
    /** What the RTTY is sending, once a sentence has passed ("300 Bd · 8N1 · 425 Hz"); "" before. */
    std::string rttySettings() const { return rttyWinner_; }
    /** The Horus tones — only while Horus is DECODING (a packet in the last minute); a placement on its own is a try. */
    bool   horusTones(double t[4]) const { if (!horus_.haveTones() || !horusRecent()) return false; for (int k = 0; k < 4; ++k) t[k] = horus_.placed()[k]; return true; }
    bool   horusRecent() const { return lastHorusAt_ > -100 && sec_ - lastHorusAt_ < 60; }
    const horus::Demod& horus() const { return horus_; }
    int    candidates() const { return (int)cands_.size(); }
    /** "300 Bd 8N1" for candidate i (logs). */
    std::string candName(int i) const { if (i < 0 || i >= (int)cands_.size()) return "?"; char b[48]; std::snprintf(b, sizeof b, "%g Bd %dN1%s", cands_[(size_t)i]->baud, cands_[(size_t)i]->bits, cands_[(size_t)i]->inv ? " rev" : ""); return b; }
    unsigned long sentences() const { return sentences_; }
    unsigned long crcFails() const { unsigned long n = 0; for (auto& c : cands_) n += c->fr.bad; return n; }
    unsigned long ssdvPackets() const { return ssdvPackets_; }
    unsigned long bankBuilds() const { return builds_; }
    ssdv::Collector& pictures() { return pics_; }
    double seconds() const { return sec_; }

private:
    struct Cand {
        std::unique_ptr<FskDecoder> d;
        double baud; int bits; bool inv;
        SentenceFramer fr;
        std::unique_ptr<ssdv::Framer> sf;
    };
    void onSpectrum_() {
        // ★ Horus first. ★★ The spectrum of fast RTTY (300 Bd and up) can show four evenly spaced peaks of its own, so
        //   four tones are only BELIEVED as Horus once a packet has passed: until then a placement is a try (harmless —
        //   nothing passes its CRC), and once Horus has decoded in the last minute a placement far from its tones is
        //   ignored (the RTTY bursts between its packets must not knock it off), and only then do four tones keep the
        //   RTTY bank from following the spectrum.
        double t4[4];
        const bool four = cfg_.horus && findFour(spec_, t4, cfg_.horusSpacing);
        const bool horusLive = horusRecent();
        if (four) {
            bool far = !horus_.haveTones();
            if (!far) for (int k = 0; k < 4; ++k) far |= std::fabs(t4[k] - horus_.tones()[k]) > 30;
            if (!(horusLive && far)) {
                const unsigned long sets = horus_.toneSets;
                horus_.setTones(t4); fourAt_ = sec_;
                if (horus_.toneSets != sets) { std::vector<float> h; history_(h); horus_.process(h.data(), (int)h.size()); }
            }
        }
        else if (horus_.haveTones() && sec_ - fourAt_ > 8 && !horusLive) horus_.clearTones();
        if (!cfg_.rtty || (four && horusLive)) return;
        double lo, hi;
        if (!findPair(spec_, lo, hi, cfg_.rttyShift)) return;
        pairAt_ = sec_;
        const double c = (lo + hi) / 2, sh = hi - lo;
        if (cands_.empty()) { build_(c, sh); return; }
        // ★ a DIFFERENT shift restarts the bank only when it holds for three spectra in a row: at 300 baud and up the two
        //   peaks are broad and their spacing wanders by tens of hertz, and every restart would throw the lock away
        if (std::fabs(sh - shift_) > std::max(25.0, 0.1 * shift_)) { if (++shiftVotes_ >= 3) { shiftVotes_ = 0; build_(c, sh); } return; }
        shiftVotes_ = 0;
        // ★ the same signal a little moved (drift, a retune): every candidate follows, keeping its lock
        if (std::fabs(c - centre_) > 6) { centre_ = c; for (auto& k : cands_) k->d->retune(c); }
    }
    void build_(double centre, double shift) {
        cands_.clear();
        centre_ = centre; shift_ = shift;
        std::vector<double> bauds = cfg_.rttyBaud > 0 ? std::vector<double>{ cfg_.rttyBaud } : searchBauds();
        std::vector<int> bits = cfg_.rttyBits == 7 || cfg_.rttyBits == 8 ? std::vector<int>{ cfg_.rttyBits } : std::vector<int>{ 7, 8 };
        std::vector<int> pols = cfg_.rttyReverse >= 0 ? std::vector<int>{ cfg_.rttyReverse ? 1 : 0 } : std::vector<int>{ 0, 1 };
        for (double b : bauds) for (int nb : bits) for (int pol : pols) {
            std::unique_ptr<Cand> c(new Cand());
            c->baud = b; c->bits = nb; c->inv = pol != 0;
            c->d.reset(new FskDecoder(kFs, centre, shift, b, nb == 7 ? "7N1" : "8N1", "ASCII", c->inv));
            c->d->setAfc(false);   // ★ the bank follows the spectrum instead (onSpectrum_) — one measurement for all
            Cand* cp = c.get();
            const int idx = (int)cands_.size();
            c->fr.onSentence = [this, idx](const Telemetry& t) { deliver_(t, idx); };
            c->fr.onRejected = [this, idx](const std::string& l) { if (onRejected) onRejected(l, idx); };
            c->d->onChar = [cp](char32_t ch) { if (ch < 0x80) cp->fr.put((char)ch); };
            if (nb == 8) {
                c->sf.reset(new ssdv::Framer());
                c->sf->onPacket = [this](const ssdv::Packet& p) { packet_(p); };
                c->d->onData = [cp](uint8_t v) { cp->sf->put(v); };
            }
            cands_.push_back(std::move(c));
        }
        ++builds_;
        // ★ replay what was heard before the bank existed (see feed12k)
        std::vector<float> h; history_(h);
        if (!h.empty()) { std::vector<int16_t> p; toPcm_(h.data(), (int)h.size(), p); for (auto& k : cands_) k->d->process(p.data(), (int)p.size()); }
    }
    void history_(std::vector<float>& h) const {
        h.resize((size_t)histFill_);
        const long long start = (histPos_ - histFill_ + kHist) % kHist;
        for (long long i = 0; i < histFill_; ++i) h[(size_t)i] = hist_[(size_t)((start + i) % kHist)];
    }
    static void toPcm_(const float* x, int n, std::vector<int16_t>& p) {
        p.resize((size_t)n);
        for (int i = 0; i < n; ++i) { const float v = x[i] * 32767.0f; p[(size_t)i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    }
    void deliver_(const Telemetry& t, int cand) {
        // ★ the same sentence from several candidates (7 and 8 bits, two speeds that both frame it) is shown once
        const std::string key = t.sentence;
        for (auto& r : recent_) if (r.first == key && sec_ - r.second < 120) return;
        recent_.push_back({ key, sec_ });
        if (recent_.size() > 16) recent_.pop_front();
        if (cand >= 0 && cand < (int)cands_.size()) {
            const Cand& c = *cands_[(size_t)cand];
            char b[96];
            std::snprintf(b, sizeof b, "%g Bd \xc2\xb7 %dN1 \xc2\xb7 %.0f Hz%s", c.baud, c.bits, shift_, c.inv ? " \xc2\xb7 reverse" : "");
            rttyWinner_ = b;
        }
        lastGoodAt_ = sec_;
        ++sentences_;
        if (onTelemetry) onTelemetry(t);
    }
    void packet_(const ssdv::Packet& p) {
        ssdv::Image* im = pics_.add(p, (int64_t)sec_);
        if (!im) return;
        ++ssdvPackets_;
        lastGoodAt_ = sec_;
        if (onImagePacket) onImagePacket(*im, p);
    }

    Config cfg_;
    Decim4 dec_;
    Spectrum spec_;
    horus::Demod horus_;
    std::vector<std::unique_ptr<Cand>> cands_;
    std::vector<float> d12_;
    std::vector<int16_t> pcm_;
    static constexpr int kHist = kFs * 6;   ///< six seconds of 12 kHz audio (288 KB) — one 600-baud SSDV packet and more
    std::vector<float> hist_ = std::vector<float>((size_t)kHist, 0.f);
    int histPos_ = 0; long long histFill_ = 0; int shiftVotes_ = 0;
    ssdv::Collector pics_;
    std::deque<std::pair<std::string, double>> recent_;
    std::string rttyWinner_;
    long long samples_ = 0;
    double sec_ = 0, centre_ = 0, shift_ = 0, pairAt_ = -100, fourAt_ = -100, lastHorusAt_ = -100, lastGoodAt_ = 0;
    unsigned long sentences_ = 0, ssdvPackets_ = 0, builds_ = 0;
};

} // namespace balloon
} // namespace vibe
