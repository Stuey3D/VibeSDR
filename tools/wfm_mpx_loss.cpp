// Does the WFM channel filter cost us RDS? — the open question from the Pira calibration.
//
// Against HansVanEijsden's analyser our RDS deviation reads ~1.3 dB low on six stations out of
// six, while PILOT deviation is exact on all six. The suspicion (rds.cpp) is that the channel
// filter clips the FM sidebands, which attenuates the TOP of the demodulated MPX and so hits
// 57 kHz while leaving 19 kHz alone. If true we are RECEIVING RDS weak and losing decode
// margin, not merely displaying it low.
//
// This synthesises a realistic FM broadcast signal with KNOWN pilot and RDS deviations, runs it
// through the EXACT filter cascade pipeline.cpp builds for WFM, demodulates, and measures both
// tones — against the same signal demodulated with no channel filter at all.
//
// Build and run:
//   V=android/app/src/main/cpp/vibedsp
//   clang++ -O2 -std=c++17 -I$V tools/wfm_mpx_loss.cpp $V/ddc.cpp -o /tmp/wfm_mpx_loss \
//     && /tmp/wfm_mpx_loss
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <random>

using vibedsp::designLowpass;
using vibedsp::FirDecimator;
using cf = std::complex<float>;

static const double PILOT_HZ = 19000.0, RDS_HZ = 57000.0, AUDIO_HZ = 1000.0;

/** Amplitude of a tone at f in a real signal, by direct correlation (Goertzel would do; this
 *  is clearer and the cost is irrelevant). Returns peak amplitude in the signal's own units. */
static double toneAmp(const std::vector<float>& x, double fs, double f, int skip) {
    double re = 0, im = 0; long n = 0;
    for (size_t i = skip; i < x.size(); ++i, ++n) {
        const double t = (double)i / fs;
        re += x[i] * std::cos(2.0 * M_PI * f * t);
        im += x[i] * std::sin(2.0 * M_PI * f * t);
    }
    if (!n) return 0.0;
    return 2.0 * std::sqrt(re * re + im * im) / (double)n;
}

static void runAll(double fsIn);

struct Result { int decim; double chFs, chHalf, trans;
                double pilotLoss, rdsLoss, adjRej, audioSnr, rdsSnr; };

/** Build the WFM cascade for a given channel half-width and target rate, exactly as
 *  pipeline.cpp does, then measure what it costs the MPX and what it rejects at 200 kHz. */
static Result run(const std::vector<cf>& iq, const std::vector<cf>& adj,
                  const std::vector<cf>& noisy, double fsIn,
                  double chHalfWant, double targetCh) {
    Result R{};
    const int chDecim = std::max(1, (int)std::floor(fsIn / targetCh));
    const double chFs = fsIn / chDecim;
    const double chHalf  = std::max(1.0, chHalfWant);
    const double transHz = std::max(chHalf * 0.5, chFs * 0.25 - chHalf);
    R.decim = chDecim; R.chFs = chFs; R.chHalf = chHalf; R.trans = transHz;

    std::vector<int> stages;
    { int d = chDecim;
      while (d > 1) { int f = 1;
        for (int q = 8; q >= 2; --q) if (d % q == 0) { f = q; break; }
        if (f == 1) { stages.push_back(d); break; }
        stages.push_back(f); d /= f; }
      if (stages.empty()) stages.push_back(1); }

    auto build = [&]() {
        std::vector<std::unique_ptr<FirDecimator>> decs;
        double fs = fsIn;
        for (size_t i = 0; i < stages.size(); ++i) {
            const int D = stages[i];
            const double fsOut = fs / D;
            const bool last = (i + 1 == stages.size());
            double cutoff, trans;
            if (last) { cutoff = std::min(0.45 / D, chHalf / fs);
                        trans  = std::max(cutoff * 0.5, transHz / fs); }
            else      { cutoff = chHalf / fs;
                        trans  = std::max((fsOut - chHalf) / fs - cutoff, cutoff * 0.5); }
            decs.push_back(std::make_unique<FirDecimator>(
                designLowpass(cutoff, std::max(trans, 1e-3), /*deepStop=*/last), D));
            fs = fsOut;
        }
        return decs;
    };

    auto push = [&](const std::vector<cf>& src) {
        auto decs = build();
        std::vector<cf> in = src, out;
        for (auto& d : decs) {
            out.resize(in.size() + 8);
            const int n = d->process(in.data(), (int)in.size(), out.data());
            out.resize(n); in = out;
        }
        return in;
    };

    auto demod = [&](const std::vector<cf>& s, double fs) {
        std::vector<float> mpx(s.size());
        for (size_t i = 1; i < s.size(); ++i) {
            const cf d = s[i] * std::conj(s[i - 1]);
            mpx[i] = (float)(std::atan2((double)d.imag(), (double)d.real()) * fs / (2.0 * M_PI));
        }
        return mpx;
    };

    const std::vector<cf> filt = push(iq);
    std::vector<cf> ctrl; ctrl.reserve(iq.size() / chDecim + 1);
    for (size_t i = 0; i < iq.size(); i += chDecim) ctrl.push_back(iq[i]);

    // ★★ AUDIO SNR — the cost of widening, and the thing a listener actually hears. A wider
    // channel admits more noise into the discriminator, so every dB of RDS bought here is
    // partly paid for at the FM threshold, which is precisely where the weak stations live.
    // Measured on a SEPARATE noisy copy: recovered 1 kHz tone against everything else in the
    // audio band. (Stuart raised this — widening for RDS must not quietly make the audio worse.)
    const std::vector<cf> nf = push(noisy);
    const std::vector<float> mN = demod(nf, chFs);
    {
        const int sk = (int)(mN.size() / 5);
        const double sig = toneAmp(mN, chFs, AUDIO_HZ, sk);
        double tot = 0; long cnt = 0;
        for (size_t i = sk; i < mN.size(); ++i) {
            // Audio band only: everything below the pilot.
            tot += (double)mN[i] * mN[i]; ++cnt;
        }
        const double noisePow = std::max(1e-9, tot / cnt - 0.5 * sig * sig);
        R.audioSnr = 10.0 * std::log10((0.5 * sig * sig) / noisePow);

        // ★★★ THE METRIC THAT ACTUALLY DECIDES WHETHER RDS DECODES: subcarrier SNR at 57 kHz,
        // not subcarrier AMPLITUDE. Everything above measures how much 57 kHz tone survives the
        // channel filter — and widening does recover some. But FM's noise triangle puts noise
        // power up as the SQUARE of baseband frequency, so a wider IF dumps disproportionately
        // more noise at exactly 57 kHz. Measuring the numerator and ignoring the denominator is
        // how "widening helps RDS" survived simulation and then failed on air: Stuart's A/B on
        // a strong local showed constellation scatter going 16% -> 39% in FM-DX (2026-07-27).
        // ★ Noise taken at 63 kHz — clear of the subcarrier, same corner of the MPX, so it is
        // the noise the RDS decoder is actually fighting.
        const double rdsAmp   = toneAmp(mN, chFs, RDS_HZ, sk);
        const double nearAmp  = toneAmp(mN, chFs, RDS_HZ + 6000.0, sk);
        R.rdsSnr = 20.0 * std::log10(std::max(1e-9, rdsAmp) / std::max(1e-9, nearAmp));
    }

    const std::vector<float> mF = demod(filt, chFs), mC = demod(ctrl, chFs);
    const int skip = (int)(mF.size() / 5);
    const double pF = toneAmp(mF, chFs, PILOT_HZ, skip), rF = toneAmp(mF, chFs, RDS_HZ, skip);
    const double pC = toneAmp(mC, chFs, PILOT_HZ, skip), rC = toneAmp(mC, chFs, RDS_HZ, skip);
    R.pilotLoss = 20.0 * std::log10(pF / pC);
    R.rdsLoss   = 20.0 * std::log10(rF / rC);

    // ★ ADJACENT-CHANNEL REJECTION. A pure carrier 200 kHz off tune (the FM channel spacing):
    // how much of its POWER survives. This is the thing the filter exists to do, and the thing
    // any widening spends — see the deepStop note in pipeline.cpp.
    const std::vector<cf> af = push(adj);
    double eIn = 0, eOut = 0;
    for (size_t i = adj.size() / 5; i < adj.size(); ++i) eIn += std::norm(adj[i]);
    for (size_t i = af.size() / 5; i < af.size(); ++i)  eOut += std::norm(af[i]);
    const double nIn = (double)(adj.size() - adj.size() / 5);
    const double nOut = (double)(af.size() - af.size() / 5);
    R.adjRej = 10.0 * std::log10(((eOut / nOut) + 1e-30) / (eIn / nIn));
    return R;
}

int main() {
    // ★ BOTH capture rates. The geometry is NOT the same: at 768 kHz (an Airspy HF+) a 200 kHz
    // channel decimates by 2 either way, so FM-DX's wider filter has to fit its stopband under
    // a 192 kHz Nyquist instead of the 200 kHz it gets on a dongle. Testing only at 2.4 MSPS is
    // how a mode that measured well ended up making RDS visibly WORSE on air (2026-07-27).
    for (double fsIn : {2400000.0, 768000.0})
    runAll(fsIn);
    return 0;
}

static void runAll(double fsIn) {
    const double audioDev = 55000.0, pilotDev = 6750.0, rdsDev = 2000.0;
    const long N = (long)(fsIn * 0.25);

    std::vector<cf> iq(N), adj(N), noisy(N);
    std::mt19937 rng(7);
    std::normal_distribution<double> gauss(0.0, 1.0);
    // ★ Noise level chosen to sit near the FM threshold, where the trade actually bites. Well
    // above threshold the capture effect hides it and any filter looks fine.
    const double nAmp = 0.55;
    double phase = 0.0;
    for (long i = 0; i < N; ++i) {
        const double t = (double)i / fsIn;
        const double mpx = audioDev * std::sin(2.0 * M_PI * AUDIO_HZ * t)
                         + pilotDev * std::sin(2.0 * M_PI * PILOT_HZ * t)
                         + rdsDev   * std::sin(2.0 * M_PI * RDS_HZ   * t);
        phase += 2.0 * M_PI * mpx / fsIn;
        iq[i]  = cf((float)std::cos(phase), (float)std::sin(phase));
        adj[i] = cf((float)std::cos(2.0 * M_PI * 200000.0 * t),
                    (float)std::sin(2.0 * M_PI * 200000.0 * t));
        noisy[i] = iq[i] + cf((float)(nAmp * gauss(rng)), (float)(nAmp * gauss(rng)));
    }

    struct Cand { const char* name; double chHalf, targetCh; };
    const Cand cands[] = {
        { "current  (bw/2)",      100000.0, 300000.0 },
        { "110 kHz @ 300k",       110000.0, 300000.0 },
        { "120 kHz @ 400k",       120000.0, 400000.0 },
        { "128 kHz @ 400k",       128000.0, 400000.0 },
        { "128 kHz @ 480k",       128000.0, 480000.0 },
        { "140 kHz @ 480k",       140000.0, 480000.0 },
    };
    printf("== capture %.3f MSPS ==\n", fsIn / 1e6);
    printf("  %-18s %5s %8s %9s %11s %10s %9s\n",
           "channel filter", "decim", "chFs", "RDS ampl", "adj@200k", "audio SNR", "RDS SNR");
    for (const auto& c : cands) {
        const Result r = run(iq, adj, noisy, fsIn, c.chHalf, c.targetCh);
        printf("  %-18s %5d %8.0f %+9.2f %+11.1f %+10.2f %+9.2f\n",
               c.name, r.decim, r.chFs, r.rdsLoss, r.adjRej, r.audioSnr, r.rdsSnr);
    }
    printf("\n");
}
