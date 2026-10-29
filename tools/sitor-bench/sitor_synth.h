// sitor_synth.h — a SITOR-A link for the tests (vibeserver/test-sitora.cpp): both stations, turn by turn, as a third
// station hears them. The same protocol as synth_sitor_a.py (ITU-R M.476-5 / M.625-4), in C++ and deterministic, so
// a test can say exactly which blocks the IRS asked for again.
#pragma once
#include "decoders/sitor_a.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace sitorsynth {
using namespace vibe::sitora;

struct Turn { bool master; std::vector<uint8_t> codes; };   // codes empty = silent

/** The traffic signals for `text` (letters/figures shifts inserted; '\n' = CR LF), and what a listener prints. */
inline std::vector<uint8_t> encode(const std::string& text, std::string& printed) {
    std::vector<uint8_t> c{ kLtrs }; bool fig = false;
    for (char ch : text) {
        if (ch == '\n') { c.push_back(kCr); c.push_back(kLf); printed += '\n'; continue; }
        if (ch == ' ') { c.push_back(kSpace); printed += ' '; continue; }
        const uint8_t l = letterSignal((char32_t)ch), f = figureSignal((char32_t)ch);
        if (l && !fig) c.push_back(l);
        else if (f && fig) c.push_back(f);
        else if (f) { c.push_back(kFigs); c.push_back(f); fig = true; }
        else if (l) { c.push_back(kLtrs); c.push_back(l); fig = false; }
        else continue;
        printed += ch;
    }
    return c;
}
inline std::vector<std::vector<uint8_t>> blocks(std::vector<uint8_t> c) {
    while (c.size() % 3) c.push_back(kBeta);
    std::vector<std::vector<uint8_t>> b;
    for (size_t i = 0; i < c.size(); i += 3) b.push_back({ c[i], c[i + 1], c[i + 2] });
    return b;
}

struct Plan {
    std::vector<Turn> turns;
    std::string printed;      // all stations, in order
    int irsRepeats = 0;       // blocks the IRS asked for again
};

/** The whole link. `repeatEvery` > 0: the IRS "mutilates" every n-th block it gets (so the ISS repeats it); `csEvery`
 *  > 0: the ISS mutilates every n-th control signal (so it sends RQ RQ RQ). Texts end "+?" (the change-over). */
inline Plan makeLink(const std::string& calledNumber, const std::string& callerNumber, const std::string& t1,
                     const std::string& t2, const std::string& t3, int repeatEvery = 4, int csEvery = 7) {
    Plan L;
    const std::string called = identitySignals(calledNumber), caller = identitySignals(callerNumber);
    auto M = [&](std::vector<uint8_t> c) { L.turns.push_back({ true, std::move(c) }); };
    auto S = [&](std::vector<uint8_t> c) { L.turns.push_back({ false, std::move(c) }); };
    auto ls = [](char c) { return letterSignal((char32_t)c); };
    // the call, three times round, then the answer (M.625 §3.5.3–3.5.5)
    std::vector<std::vector<uint8_t>> cb;
    if (called.size() == 7) cb = { { ls(called[0]), kRq, ls(called[1]) }, { kRq, ls(called[2]), ls(called[3]) }, { ls(called[4]), ls(called[5]), ls(called[6]) } };
    else cb = { { ls(called[0]), kRq, ls(called[1]) }, { ls(called[2]), ls(called[3]), kRq } };
    for (int r = 0; r < 3; r++) for (auto& b : cb) { M(b); S({}); }
    L.turns.back().codes = { called.size() == 7 ? kCs4 : kCs1 };
    if (called.size() == 7) {   // identification (M.625 §3.6)
        const std::string ck = checksums(called);
        M({ ls(caller[0]), kAlpha, ls(caller[1]) }); S({ ls(ck[0]) });
        M({ kAlpha, ls(caller[2]), ls(caller[3]) }); S({ ls(ck[1]) });
        M({ ls(caller[4]), ls(caller[5]), ls(caller[6]) }); S({ ls(ck[2]) });
        M({ kRq, kRq, kRq }); S({ kCs1 });
    }
    std::string p1, p2, p3;
    auto q1 = blocks(encode(t1 + "+?", p1)), q2 = blocks(encode(t2 + "+?", p2)), q3 = blocks(encode(t3, p3));
    L.printed = p1 + p2 + p3;
    int nBlk = 0, nCs = 0;
    bool issMaster = true;
    uint8_t lastCs = kCs1;
    auto turn = [&](bool master, std::vector<uint8_t> c) { L.turns.push_back({ master, std::move(c) }); };
    // one passage of traffic from the ISS, ending in the change-over (or the end of communication)
    auto passage = [&](std::vector<std::vector<uint8_t>>& q, bool last) {
        size_t next = 0; int curNum = 0; std::vector<uint8_t> cur;
        for (;;) {
            std::vector<uint8_t> blk;
            if (csEvery > 0 && ++nCs % csEvery == 0) blk = { kRq, kRq, kRq };
            else if (lastCs == kCs3) blk = { kBeta, kAlpha, kBeta };
            else {
                const int num = lastCs == kCs1 ? 1 : 2;
                if (num != curNum) { cur = next < q.size() ? q[next++] : (last ? std::vector<uint8_t>{ kAlpha, kAlpha, kAlpha } : std::vector<uint8_t>{ kBeta, kBeta, kBeta }); curNum = num; }
                blk = cur;
            }
            turn(issMaster, blk);
            const bool hasRq = blk[0] == kRq;
            if (!hasRq && repeatEvery > 0 && blk[0] != kAlpha && ++nBlk % repeatEvery == 0) { turn(!issMaster, { lastCs }); L.irsRepeats++; continue; }
            if (hasRq) { turn(!issMaster, { lastCs }); continue; }
            if (blk == std::vector<uint8_t>{ kAlpha, kAlpha, kAlpha }) { turn(!issMaster, { lastCs == kCs1 ? kCs2 : kCs1 }); return; }
            if (blk == std::vector<uint8_t>{ kBeta, kAlpha, kBeta }) {
                // the IRS becomes ISS (M.625 §3.7.11.5): RQ RQ RQ as slave, one RQ as master; the old ISS starts the flow
                turn(!issMaster, issMaster ? std::vector<uint8_t>{ kRq, kRq, kRq } : std::vector<uint8_t>{ kRq });
                turn(issMaster, { kCs1 });
                issMaster = !issMaster; lastCs = kCs1;
                return;
            }
            const bool over = std::find(blk.begin(), blk.end(), figureSignal(U'?')) != blk.end() && next == q.size();
            lastCs = over ? kCs3 : (lastCs == kCs1 ? kCs2 : kCs1);
            turn(!issMaster, { lastCs });
        }
    };
    passage(q1, false); passage(q2, false); passage(q3, true);
    for (int k = 0; k < 6; k++) { M({}); S({}); }
    return L;
}

/** The audio: the master at each cycle's start, the slave `dMs` after the master's signal ends (M.476 §3.1.1.6), each
 *  with its own level and frequency error; white noise for `snrDb` in 3 kHz against the master. B = the higher tone. */
inline std::vector<float> render(const Plan& L, double rate, double centre, double snrDb, double slaveDb, uint32_t seed,
                                 double masterHz = 0, double slaveHz = 12, double dMs = 30, double startS = 0.3, bool invert = false) {
    const double cyc = 0.45, spb = rate / 100.0;
    const size_t cycles = (L.turns.size() + 1) / 2;
    std::vector<float> y((size_t)((startS + cycles * cyc + 1) * rate), 0.0f);
    std::mt19937 g(seed); std::uniform_real_distribution<double> U(0, 2 * M_PI); std::normal_distribution<double> N(0, 1);
    for (size_t k = 0; k + 1 <= L.turns.size(); k += 2) {
        const double tm = startS + (k / 2) * cyc;
        const size_t mLen = L.turns[k].codes.empty() ? 1 : L.turns[k].codes.size();
        for (int j = 0; j < 2 && k + j < L.turns.size(); j++) {
            const Turn& t = L.turns[k + j];
            if (t.codes.empty()) continue;
            const double start = t.master ? tm : tm + mLen * 0.07 + dMs / 1000;
            const double amp = t.master ? 1.0 : std::pow(10.0, slaveDb / 20), off = t.master ? masterHz : slaveHz;
            double ph = U(g);
            const size_t a0 = (size_t)std::llround(start * rate), n = (size_t)std::llround(t.codes.size() * 7 * spb);
            for (size_t i = 0; i < n && a0 + i < y.size(); i++) {
                const size_t bit = std::min((size_t)(i / spb), t.codes.size() * 7 - 1);
                const bool b = ((t.codes[bit / 7] >> (bit % 7)) & 1) != invert;
                ph += 2 * M_PI * (centre + off + (b ? 85 : -85)) / rate;
                y[a0 + i] += (float)(0.25 * amp * std::cos(ph));
            }
        }
    }
    const double sd = 0.25 * std::sqrt(0.5) * std::pow(10.0, -snrDb / 20) * std::sqrt(rate / 3000);
    for (auto& v : y) v += (float)(sd * N(g));
    return y;
}

} // namespace sitorsynth
