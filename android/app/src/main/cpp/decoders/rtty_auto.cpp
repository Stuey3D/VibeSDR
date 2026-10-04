// rtty_auto — see rtty_auto.h.
#include "rtty_auto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace vibe {

namespace {
// In-place radix-2 FFT (n a power of two). Run twice a second on 8192 points — a few ms on a Pi 2.
void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * (float)M_PI / (float)len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
            }
        }
    }
}
const double kBauds[] = { 45.45, 50.0, 75.0 };
const double kShifts[] = { 170.0, 200.0, 425.0, 450.0, 850.0 };
}

RttyAuto::RttyAuto(int sampleRate) : sr_(sampleRate), ring_(kN, 0.0f), psd_(kN / 2 + 1, 0.0) {}

void RttyAuto::say_(const std::string& s) {
    if (!onChar) return;
    for (char c : s) onChar((char32_t)(unsigned char)c);
}

void RttyAuto::startCandidates_(double centre, double shift) {
    cands_.clear(); winner_ = -1; chosenText_.clear(); searchSamples_ = 0; badWinnerSec_ = 0;
    centre_ = centre; shift_ = shift;
    for (double b : kBauds) for (int inv = 0; inv < 2; inv++) {
        Cand c; c.baud = b; c.inv = inv != 0;
        c.d.reset(new FskDecoder(sr_, centre, shift, b, "5N1.5", "ITA2", c.inv));
        cands_.push_back(std::move(c));
    }
    for (size_t i = 0; i < cands_.size(); i++) {
        cands_[i].d->onChar = [this, i](char32_t ch) {
            if ((int)i == winner_) { if (onChar) onChar(ch); return; }
            auto& p = cands_[i].pending;
            if (ch < 0x80) { p.push_back((char)ch); if (p.size() > 400) p.erase(0, p.size() - 400); }
        };
        cands_[i].d->onState = [this, i](int st) {
            if ((int)i == winner_ && st != lastState_) { lastState_ = st; if (onState) onState(st); }
        };
    }
    if (onState && lastState_ != 1) { lastState_ = 1; onState(1); }   // Sync1: hunting
}

// Two peaks in the smoothed spectrum, 100–1000 Hz apart, both well clear of the noise → centre and shift.
void RttyAuto::spectrumStep_() {
    std::vector<std::complex<float>> a(kN);
    for (int i = 0; i < kN; i++) {
        const float w = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (kN - 1));
        a[i] = { ring_[(ringPos_ + i) % kN] * w, 0.0f };
    }
    fft(a);
    const double k = psdFrames_ < 4 ? 1.0 / (psdFrames_ + 1) : 0.25;   // ~2 s memory at 2 FFTs a second
    for (int b = 0; b <= kN / 2; b++) psd_[b] += (std::norm(a[b]) - psd_[b]) * k;
    psdFrames_++;
    if (psdFrames_ < 4) return;
    const double bin = (double)sr_ / kN;
    const int lo = (int)(250 / bin), hi = (int)(3200 / bin);
    std::vector<double> sorted(psd_.begin() + lo, psd_.begin() + hi);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double noise = std::max(1e-12, sorted[sorted.size() / 2]);
    auto peakAt = [&](int b) { return b > lo && b < hi && psd_[b] >= psd_[b - 1] && psd_[b] >= psd_[b + 1]; };
    int p1 = -1;
    for (int b = lo + 1; b < hi; b++) if (peakAt(b) && (p1 < 0 || psd_[b] > psd_[p1])) p1 = b;
    if (p1 < 0 || psd_[p1] < noise * 30) return;                       // nothing tone-like (≥ ~15 dB)
    int p2 = -1;
    for (int b = lo + 1; b < hi; b++) {
        const double d = std::fabs(b - p1) * bin;
        if (d < 100 || d > 1000 || !peakAt(b)) continue;
        if (p2 < 0 || psd_[b] > psd_[p2]) p2 = b;
    }
    if (p2 < 0 || psd_[p2] < noise * 30 || psd_[p2] < psd_[p1] * 0.03) return;   // second tone within 15 dB of the first
    // Sub-bin centres (parabolic), then the standard shift if it is close.
    auto refine = [&](int b) {
        const double y0 = std::log(psd_[b - 1] + 1e-12), y1 = std::log(psd_[b] + 1e-12), y2 = std::log(psd_[b + 1] + 1e-12);
        const double den = y0 - 2 * y1 + y2;
        return (b + (den != 0 ? 0.5 * (y0 - y2) / den : 0.0)) * bin;
    };
    const double f1 = refine(p1), f2 = refine(p2);
    const double centre = (f1 + f2) / 2;
    double shift = std::fabs(f1 - f2);
    { double bestS = 0, bestD = 1e9;   // ★ the NEAREST standard shift within 12 % (445 Hz measured is DWD's 450, not 425)
      for (double s : kShifts) { const double d = std::fabs(shift - s); if (d <= s * 0.12 && d < bestD) { bestD = d; bestS = s; } }
      if (bestS > 0) shift = bestS; }
    // New tones, or the old ones moved (a retune): start again.
    if (centre_ == 0 || std::fabs(centre - centre_) > 25 || std::fabs(shift - shift_) > 30) {
        if (centre_ != 0 && winner_ >= 0) say_("\n");
        startCandidates_(centre, shift);
    }
}

void RttyAuto::evaluate_() {
    if (cands_.empty()) return;
    int best = -1;
    for (size_t i = 0; i < cands_.size(); i++) {
        auto& c = cands_[i];
        const unsigned long g = c.d->goodFrames(), b = c.d->framingErrors();
        const double dg = (double)(g - c.lastGood), db = (double)(b - c.lastBad);
        c.lastGood = g; c.lastBad = b;
        c.score = c.score * 0.7 + (dg - 3.0 * db);
        if (best < 0 || c.score > cands_[best].score) best = (int)i;
    }
    char buf[96];
    if (winner_ < 0) {
        // ★ Choose once the best is clearly a decode: a few seconds of mostly clean frames, and ahead of the rest.
        double second = -1e9;
        for (size_t i = 0; i < cands_.size(); i++) if ((int)i != best) second = std::max(second, cands_[i].score);
        if (cands_[best].score >= 12 && cands_[best].score >= second + 6) {
            winner_ = best;
            const auto& c = cands_[best];
            std::snprintf(buf, sizeof buf, "[RTTY auto: %s baud, %.0f Hz shift%s]\n",
                          c.baud == 45.45 ? "45.45" : (c.baud == 50 ? "50" : "75"), shift_, c.inv ? ", reverse" : "");
            chosenText_ = buf;
            say_(buf);
            for (char ch : c.pending) if (onChar) onChar((char32_t)(unsigned char)ch);
            for (auto& x : cands_) x.pending.clear();
            lastState_ = -1;
            if (onState) { lastState_ = c.d->stateNow(); onState(lastState_); }
        }
        return;
    }
    // ★ A winner whose frames have gone bad for a while (the station changed its settings) — look again.
    const auto& w = cands_[winner_];
    if (w.score < -6) { if (++badWinnerSec_ >= 8) { say_("\n"); startCandidates_(centre_, shift_); } }
    else badWinnerSec_ = 0;
}

void RttyAuto::process(const int16_t* s, int count) {
    for (int i = 0; i < count; i++) {
        ring_[ringPos_] = (float)s[i];
        ringPos_ = (ringPos_ + 1) % kN;
    }
    sinceFft_ += count;
    if (sinceFft_ >= sr_ / 2) { sinceFft_ = 0; spectrumStep_(); }
    for (auto& c : cands_) c.d->process(s, count);
    samplesSinceEval_ += count;
    if (samplesSinceEval_ >= sr_) { samplesSinceEval_ = 0; evaluate_(); }
}

unsigned long RttyAuto::resyncs() const { return cands_.empty() ? 0 : cands_[winner_ >= 0 ? winner_ : 0].d->resyncs(); }
double RttyAuto::audioLevel() const { return cands_.empty() ? 0 : cands_[winner_ >= 0 ? winner_ : 0].d->audioLevel(); }
double RttyAuto::audioThreshold() const { return cands_.empty() ? 256 : cands_[winner_ >= 0 ? winner_ : 0].d->audioThreshold(); }
int RttyAuto::stateNow() const { return winner_ >= 0 ? cands_[winner_].d->stateNow() : (cands_.empty() ? 0 : 1); }

} // namespace vibe
