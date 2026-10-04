// VibeSDR V4 — FSK (RTTY) decoder, C++ port of UberSDR's ka9q fsk extension.
#include "fsk_decoder.h"
#include <cmath>

namespace vibe {

// ── BiQuad ──────────────────────────────────────────────────────────────────
void BiQuad::configure(Type type, double freq, double sampleRate, double q) {
    double omega = 2.0 * M_PI * freq / sampleRate;
    double sinO = std::sin(omega), cosO = std::cos(omega);
    double alpha = sinO / (2.0 * q);
    double A0, A1, A2, B0, B1, B2;
    if (type == Bandpass) {
        B0 = alpha; B1 = 0.0; B2 = -alpha;
        A0 = 1.0 + alpha; A1 = -2.0 * cosO; A2 = 1.0 - alpha;
    } else { // Lowpass
        B0 = (1.0 - cosO) / 2.0; B1 = 1.0 - cosO; B2 = (1.0 - cosO) / 2.0;
        A0 = 1.0 + alpha; A1 = -2.0 * cosO; A2 = 1.0 - alpha;
    }
    b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
}
double BiQuad::filter(double in) {
    double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = in; y2 = y1; y1 = out;
    return out;
}

// ── ITA2 ─────────────────────────────────────────────────────────────────────
Ita2::Ita2(const std::string& framing) {
    // Parse framing <data>N<stop> (e.g. 5N1.5).
    // ★ 5..8 data bits only (audit 2026-10-03): anything else shifts past the 16-bit code word.
    if (framing.size() >= 3 && framing[1] == 'N' && framing[0] >= '5' && framing[0] <= '8') {
        dataBits = framing[0] - '0';
        double stop = 1.0;
        std::string s = framing.substr(2);
        if (s == "1.5") stop = 1.5; else if (s == "2") stop = 2.0; else stop = 1.0;
        double total = 1.0 + dataBits + stop;       // start + data + stop
        nbits_ = (stop == 1.5) ? (int)(total * 2) : (int)total;
    } else {
        nbits_ = dataBits;
    }
    const char32_t NUL = 0, QUO = U'\'', LF = U'\n', CR = U'\r', BEL = 7, GAP = U'_';
    const char32_t L[32] = {
        NUL,U'E',LF,U'A',U' ',U'S',U'I',U'U',CR,U'D',U'R',U'J',U'N',U'F',U'C',U'K',
        U'T',U'Z',U'L',U'W',U'H',U'Y',U'P',U'Q',U'O',U'B',U'G',GAP,U'M',U'X',U'V',GAP };
    const char32_t F[32] = {
        NUL,U'3',LF,U'-',U' ',BEL,U'8',U'7',CR,U'$',U'4',QUO,U',',U'!',U':',U'(',
        U'5',U'"',U')',U'2',U'#',U'6',U'0',U'1',U'9',U'?',U'&',GAP,U'.',U'/',U';',GAP };
    for (int c = 0; c < 32; c++) { ltrs[c] = L[c]; figs[c] = F[c];
        if (L[c] != U'_') codeLtrs[(uint8_t)c] = L[c];
        if (F[c] != U'_') codeFigs[(uint8_t)c] = F[c]; }
}
bool Ita2::checkBits(uint16_t code) const {
    uint16_t v = code;
    if (nbits_ == 15) { // 5N1.5 doubled
        if ((v & 3) != 0) return false; v >>= 2;
        for (int b = 0; b < dataBits; b++) { uint16_t d = v & 3; if (d != 0 && d != 3) return false; v >>= 2; }
        if ((v & 7) != 7) return false; v >>= 3;
        return v == 0;
    }
    if ((v & 1) != 0) return false; v >>= 1;
    v >>= (unsigned)dataBits;
    int stopBits = nbits_ - 1 - dataBits;
    uint16_t mask = (uint16_t)((1 << (unsigned)stopBits) - 1);
    return (v & mask) == mask;
}
char32_t Ita2::codeToChar(uint8_t code, bool fig) const {
    auto& m = fig ? codeFigs : codeLtrs;
    auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}
char32_t Ita2::processChar(uint16_t code) {
    uint8_t dataB;
    if (nbits_ == 15) {
        uint16_t v = code; v >>= 2;
        dataB = 0; uint8_t dMSB = (uint8_t)(1 << (dataBits - 1));
        for (int b = 0; b < dataBits; b++) { uint16_t d = v & 3; dataB = (uint8_t)((dataB >> 1) | (d != 0 ? dMSB : 0)); v >>= 2; }
    } else {
        dataB = (uint8_t)(code & ((1 << (unsigned)dataBits) - 1));
    }
    if (firstChar) { lastCode = dataB; firstChar = false; return 0; }
    char32_t out = 0;
    if (lastCode == letters)      shift = false;
    else if (lastCode == figures) shift = true;
    else                          out = codeToChar(lastCode, shift);
    lastCode = dataB;
    return out;
}

// ── CCIR476 (NAVTEX) ─────────────────────────────────────────────────────────
bool Ccir476::fourMarkBits(uint8_t v) {
    int c = 0; while (v) { c++; v &= v - 1; } return c == 4;
}
Ccir476::Ccir476() {
    const char32_t U = U'_';
    const char32_t L[128] = {
        U,U,U,U,U,U,U,U,U,U,U,U,U,U,U,U,
        U,U,U,U,U,U,U,U'J',U,U,U,U'F',U,U'C',U'K',U,
        U,U,U,U,U,U,U,U'W',U,U,U,U'Y',U,U'P',U'Q',U,
        U,U,U,U,U,U'G',U,U,U,U'M',U'X',U,U'V',U,U,U,
        U,U,U,U,U,U,U,U'A',U,U,U,U'S',U,U'I',U'U',U,
        U,U,U,U'D',U,U'R',U'E',U,U,U'N',U,U,U' ',U,U,U,
        U,U,U,U'Z',U,U'L',U,U,U,U'H',U,U,U'\n',U,U,U,
        U,U'O',U'B',U,U'T',U,U,U,U'\r',U,U,U,U,U,U,U };
    const char32_t F[128] = {
        U,U,U,U,U,U,U,U,U,U,U,U,U,U,U,U,
        U,U,U,U,U,U,U,U'\'',U,U,U,U'!',U,U':',U'(',U,
        U,U,U,U,U,U,U,U'2',U,U,U,U'6',U,U'0',U'1',U,
        U,U,U,U,U,U'&',U,U,U,U'.',U'/',U,U';',U,U,U,
        U,U,U,U,U,U,U,U'-',U,U,U,7,U,U'8',U'7',U,
        U,U,U,U'$',U,U'4',U'3',U,U,U',',U,U,U' ',U,U,U,
        U,U,U,U'"',U,U')',U,U,U,U'#',U,U,U'\n',U,U,U,
        U,U'9',U'?',U,U'5',U,U,U,U'\r',U,U,U,U,U,U,U };
    for (int c = 0; c < 128; c++) { ltrs[c] = L[c]; figs[c] = F[c];
        if (fourMarkBits((uint8_t)c)) { validCodes[c] = true;
            if (L[c] != U'_') codeLtrs[(uint8_t)c] = L[c];
            if (F[c] != U'_') codeFigs[(uint8_t)c] = F[c]; } }
}
bool Ccir476::checkBits(uint16_t code) const { return code <= 0xFF && validCodes[code & 0x7F]; }
char32_t Ccir476::codeToChar(uint8_t code, bool fig) const {
    auto& m = fig ? codeFigs : codeLtrs; auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}
char32_t Ccir476::processChar(uint16_t code, bool& success) {
    uint8_t code7 = (uint8_t)(code & 0x7F);
    success = fourMarkBits(code7);
    uint8_t chr = 0xff;
    if (code7 == codeRep) alphaPhase = false;
    else if (code7 == codeAlpha) alphaPhase = true;
    if (!alphaPhase) { c1 = c2; c2 = c3; c3 = code7; }
    else {
        if (success && c1 == code7) chr = code7;
        else if (success) chr = code7;
        else if (fourMarkBits(c1)) chr = c1;
        if (chr != 0xff) {
            alphaPhase = !alphaPhase;
            if (chr == codeRep || chr == codeAlpha || chr == codeBeta || chr == codeChar32) return 0;
            if (chr == letters) { shift = false; return 0; }
            if (chr == figures) { shift = true; return 0; }
            return codeToChar(chr, shift);
        }
    }
    alphaPhase = !alphaPhase;
    return 0;
}

// ── FskDecoder ───────────────────────────────────────────────────────────────
/* ★★★ THE ATTACH PARAMETERS ARE A STRANGER'S (audit 2026-10-03). Any listener's JSON reaches this
 *  constructor. A NaN or huge baud made bitSampleCount 0 or negative (a modulo by zero per sample),
 *  a centre of 0 divided by zero in updateFilters, and a framing of "9N2" or "0N1" gave the ITA2
 *  coder shifts past its 16-bit code word. Real RTTY runs 45.45 to a few hundred baud, NAVTEX 100:
 *  10..1200 is the range, the tones must sit inside the 48 kHz audio, and framing is <5-8>N<1|1.5|2>
 *  (1.5 only with 5 data bits — the doubled-rate trick below is only defined for 5N1.5). */
static double clampFinite(double v, double lo, double hi, double dflt) {
    if (!std::isfinite(v)) return dflt;
    return v < lo ? lo : (v > hi ? hi : v);
}
static bool validItaFraming(const std::string& f) {
    if (f.size() < 3 || f[0] < '5' || f[0] > '8' || f[1] != 'N') return false;
    const std::string stop = f.substr(2);
    return stop == "1" || stop == "2" || (stop == "1.5" && f[0] == '5');
}

FskDecoder::FskDecoder(int sr, double cf, double sh, double baud,
                       const std::string& fr, const std::string& enc, bool inv)
    : sampleRate((double)sr), centerFrequency(clampFinite(cf, 100.0, 10000.0, 1000.0)),
      shiftHz(clampFinite(sh, 10.0, 2000.0, 170.0)), baudRate(clampFinite(baud, 10.0, 1200.0, 45.45)),
      inverted(inv), framing(validItaFraming(fr) ? fr : "5N1.5"), encoding(enc) {
    deviationF = shiftHz / 2.0;
    audioAverageTC = 1000.0 / sampleRate;
    if (baudRate < 10) baudRate = 10;
    if (encoding == "CCIR476") {
        ccir476 = new Ccir476();
        nbits = ccir476->nbits(); msb = ccir476->msb();
    } else {
        ita2 = new Ita2(framing.empty() ? "5N1.5" : framing);
        nbits = ita2->nbits(); msb = ita2->msb();
        if (framing == "5N1.5") { baudRate *= 2; stopVariable = true; }
    }
    double bitDur = 1.0 / baudRate;
    bitSampleCount = (int)(sampleRate * bitDur + 0.5);
    if (bitSampleCount < zeroCrossingsDivisor) bitSampleCount = zeroCrossingsDivisor;   // never 0: it is a divisor
    halfBitSampleCount = bitSampleCount / 2;
    /* ★ ROUNDED UP (audit 2026-10-03): process() indexes it with (0..bitSampleCount-1) / divisor,
     *  so a bitSampleCount that is not a multiple of 4 wrote one past the end with the floor. */
    zeroCrossings.assign((bitSampleCount + zeroCrossingsDivisor - 1) / zeroCrossingsDivisor, 0);
    // One SIGNALLING element in samples (5N1.5 doubles baudRate above to sample half-bits; the ATC's time
    // constants are in whole symbols).
    symLen = sampleRate / (stopVariable ? baudRate / 2.0 : baudRate);
    updateFilters();
}
void FskDecoder::updateFilters() {
    markSpaceFilterQ = 6.0 * centerFrequency / 1000.0;
    double qv = centerFrequency + (4.0 * 1000.0 / centerFrequency);
    markF = qv + deviationF; spaceF = qv - deviationF;
    biquadMark.configure(BiQuad::Bandpass, markF, sampleRate, markSpaceFilterQ);
    biquadSpace.configure(BiQuad::Bandpass, spaceF, sampleRate, markSpaceFilterQ);
    biquadLowpass.configure(BiQuad::Lowpass, lowpassFilterF, sampleRate, 1.0 / std::sqrt(2.0));
    biquadLpMark.configure(BiQuad::Lowpass, lowpassFilterF, sampleRate, 1.0 / std::sqrt(2.0));
    biquadLpSpace.configure(BiQuad::Lowpass, lowpassFilterF, sampleRate, 1.0 / std::sqrt(2.0));
}
void FskDecoder::setState(State s) {
    if (s == state) return;
    // ★ Count only the drops INTO NoSignal — that is the event that discards decoder state.
    //   Sync1/Sync2/ReadData transitions are the normal life of a working decoder.
    if (s == NoSignal) resyncCount_++;
    state = s;
    if (onState) onState((int)s);
}
void FskDecoder::process(const int16_t* samples, int count) {
    for (int n = 0; n < count; n++) {
        double dv = (double)samples[n];
        double markAbs = std::fabs(biquadMark.filter(dv));
        double spaceAbs = std::fabs(biquadSpace.filter(dv));
        double maxAbs = std::max(markAbs, spaceAbs);
        audioAverage += (maxAbs - audioAverage) * audioAverageTC;
        audioAverage = std::max(0.1, audioAverage);
        /* ★★★ OPTIMAL ATC, NOT A STRAIGHT DIFFERENCE (Stuart, 2026-10-04: "I can hear it clearly but the text is breaking
         *  up"). On HF the two tones fade INDEPENDENTLY; comparing mark − space directly means a faded tone loses every
         *  decision even while it is still keyed — measured on a synthetic DWD signal, 12 dB SNR with 20 dB selective
         *  fades: 39 % of the text recovered, 337 of 581 characters garbage. Each tone's envelope is tracked against its
         *  OWN peak and a shared noise floor, and the decision is Kahn's optimum combiner (fldigi's "optimal ATC"):
         *    v = (m̂ − n)(M − n) − (ŝ − n)(S − n) − ¼[(M − n)² − (S − n)²],   m̂ = min(m, M), ŝ = min(s, S)
         *  Peaks: attack in ¼ symbol, decay over 16; noise: down in ¼, up over 48 (fldigi's constants). */
        const double m = std::max(0.0, biquadLpMark.filter(markAbs)), sp = std::max(0.0, biquadLpSpace.filter(spaceAbs));
        auto track = [](double& avg, double in, double w) { avg += (in - avg) / std::max(1.0, w); };
        track(markEnv,  m,  m  > markEnv  ? symLen / 4 : symLen * 16);
        track(spaceEnv, sp, sp > spaceEnv ? symLen / 4 : symLen * 16);
        const double lo = std::min(m, sp);
        track(noiseFloor, lo, lo < noiseFloor ? symLen / 4 : symLen * 48);
        const double mc = std::min(m, markEnv) - noiseFloor, sc = std::min(sp, spaceEnv) - noiseFloor;
        const double M = markEnv - noiseFloor, S = spaceEnv - noiseFloor;
        const double logic = mc * M - sc * S - 0.25 * (M * M - S * S);
        bool markState = logic > 0;
        signalAccumulator += markState ? 1 : -1;
        bitDuration++;
        if (markState != oldMarkState) {
            if ((bitDuration % bitSampleCount) > halfBitSampleCount) {
                int index = (sampleCount - nextEventCount + bitSampleCount * 8) % bitSampleCount;
                if (index < 0) index += bitSampleCount;
                const size_t zi = (size_t)(index / zeroCrossingsDivisor);
                if (zi < zeroCrossings.size()) zeroCrossings[zi]++;
            }
            bitDuration = 0;
        }
        oldMarkState = markState;
        if (sampleCount % bitSampleCount == 0) {
            zeroCrossingCount++;
            if (zeroCrossingCount >= zeroCrossingSamples) {
                int best = 0, bestIndex = 0;
                for (int j = 0; j < (int)zeroCrossings.size(); j++) {
                    if (zeroCrossings[j] > best) { best = zeroCrossings[j]; bestIndex = j; }
                    zeroCrossings[j] = 0;
                }
                if (best > 0) {
                    bestIndex *= zeroCrossingsDivisor;
                    bestIndex = ((bestIndex + halfBitSampleCount) % bitSampleCount) - halfBitSampleCount;
                    bestIndex /= 8;
                    syncDelta = (double)bestIndex;
                }
                zeroCrossingCount = 0;
            }
        }
        pulseEdgeEvent = (sampleCount >= nextEventCount);
        if (pulseEdgeEvent) {
            averagedMarkState = (signalAccumulator > 0) != inverted;
            signalAccumulator = 0;
            nextEventCount = sampleCount + bitSampleCount + (int)(syncDelta + 0.5);
            syncDelta = 0;
        }
        if (audioAverage < audioMinimum && state != NoSignal) setState(NoSignal);
        else if (state == NoSignal) syncSetup = true;
        if (!pulseEdgeEvent) { sampleCount++; continue; }
        processBit(averagedMarkState);
        sampleCount++;
    }
}
// coder helpers (ITA2 RTTY or CCIR476 NAVTEX)
static inline bool ckCode(Ita2* i, Ccir476* c, uint16_t code) {
    return (i && i->checkBits(code)) || (c && c->checkBits(code));
}

void FskDecoder::processBit(bool bit) {
    uint16_t bitVal = bit ? 1 : 0;
    if (syncSetup) {
        bitCount = 0; codeBits = 0; errorCount = 0; validCount = 0; lockRequired_ = 2;
        if (ita2) ita2->reset();
        if (ccir476) ccir476->reset();
        syncChars.clear(); setState(Sync1); syncSetup = false;
    }
    switch (state) {
        case NoSignal: break;
        case Sync1: {
            codeBits = (uint16_t)((codeBits >> 1) | (bitVal * msb));
            if (ckCode(ita2, ccir476, codeBits)) {
                syncChars.push_back(codeBits); validCount++;
                bitCount = 0; codeBits = 0; setState(Sync2); waiting = true;
            }
            break;
        }
        case Sync2: {
            if (stopVariable && waiting && bit) return;
            waiting = false;
            codeBits = (uint16_t)((codeBits >> 1) | (bitVal * msb)); bitCount++;
            if (bitCount == nbits) {
                if (ckCode(ita2, ccir476, codeBits)) {
                    syncChars.push_back(codeBits); codeBits = 0; bitCount = 0; validCount++;
                    // ★ ITA2: two good characters after the first before anything prints (three in a row) — one alone
                    //   was enough to lock on noise or mid-character. After a framing slip (lockRequired_ = 1) the
                    //   decoder was already in step a moment ago, so one is enough to resume.
                    int required = ccir476 ? 4 : lockRequired_;
                    if (validCount >= required) {
                        for (uint16_t c : syncChars) processCharacter(c);
                        setState(ReadData);
                    }
                } else { codeBits = 0; bitCount = 0; syncSetup = true; }
                waiting = true;
            }
            break;
        }
        case ReadData: {
            if (stopVariable && waiting && bit) return;
            waiting = false;
            codeBits = (uint16_t)((codeBits >> 1) | (bitVal * msb)); bitCount++;
            if (bitCount == nbits) {
                /* ★★★ CHECK THE FRAME (Stuart, 2026-10-04: S9+20 DWD printing "CQ CQ CQNDZPXX0XXVQPXXMMMAZQ…"). The start and
                 *  stop bits were checked only while locking on; once decoding, ITA2 never looked again, so one misread
                 *  bit put the decoder out of step and it printed garbage until the signal dropped out altogether. A bad
                 *  frame is now dropped and the start bit re-hunted from the bits already in hand — the shift state
                 *  (letters/figures) is kept, since the sender's has not changed. */
                if (ita2 && !ita2->checkBits(codeBits)) {
                    framingErrors_++;
                    syncChars.clear(); validCount = 0; lockRequired_ = 1;
                    bitCount = 0; waiting = false;
                    setState(Sync1);          // codeBits stays: Sync1 slides it one bit at a time to the next frame
                    break;
                }
                bool ok = processCharacter(codeBits);
                if (ok) { if (errorCount > 0) errorCount--; }
                else { errorCount++; if (errorCount > 2) syncSetup = true; }
                codeBits = 0; bitCount = 0; waiting = true;
            }
            break;
        }
    }
}
bool FskDecoder::processCharacter(uint16_t code) {
    if (ita2) {
        char32_t ch = ita2->processChar(code);
        if (ch != 0 && onChar) onChar(ch);
        return true;   // ITA2 has no error correction
    }
    if (ccir476) {
        bool ok = false;
        char32_t ch = ccir476->processChar(code, ok);
        if (ch != 0 && onChar) onChar(ch);
        return ok;
    }
    return false;
}

} // namespace vibe
