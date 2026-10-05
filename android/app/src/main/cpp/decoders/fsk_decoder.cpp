// VibeSDR V4 — FSK (RTTY) decoder, C++ port of UberSDR's ka9q fsk extension; NAVTEX (NavtexRx, below) is a port of
// fldigi's navtex.cxx (2026-10-05).
#include "fsk_decoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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
    // Parse framing <data><parity><stop> (e.g. 5N1.5, 7E1, 8N2) — parity N/E/O/M/S (2026-10-04, the full RTTY spec).
    // ★ 5..8 data bits only (audit 2026-10-03): anything else shifts past the 16-bit code word.
    if (framing.size() >= 3 && std::string("NEOMS").find(framing[1]) != std::string::npos
        && framing[0] >= '5' && framing[0] <= '8') {
        dataBits = framing[0] - '0';
        parity_ = framing[1];
        double stop = 1.0;
        std::string s = framing.substr(2);
        if (s == "1.5") stop = 1.5; else if (s == "2") stop = 2.0; else stop = 1.0;
        double total = 1.0 + dataBits + (parity_ != 'N' ? 1 : 0) + stop;       // start + data + parity + stop
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
    const uint16_t data = v & (uint16_t)((1 << (unsigned)dataBits) - 1);
    v >>= (unsigned)dataBits;
    if (parity_ != 'N') {
        // ★ The parity bit follows the data. Even/odd count the data's ones; mark/space are a fixed 1/0.
        int ones = 0; for (uint16_t d = data; d; d &= d - 1) ones++;
        const int p = v & 1; v >>= 1;
        const int want = parity_ == 'E' ? (ones & 1) : parity_ == 'O' ? !(ones & 1) : parity_ == 'M' ? 1 : 0;
        if (p != want) return false;
    }
    int stopBits = nbits_ - 1 - dataBits - (parity_ != 'N' ? 1 : 0);
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
        // ★ Bit 0 is the START bit (checkBits above): the data are bits 1..dataBits. This read bits 0..4 — the start bit
        //   and four data bits — and was never noticed because every client sent 5N1.5 until AUTO tried 1 stop (PBB, 2026-10-04).
        dataB = (uint8_t)((code >> 1) & ((1 << (unsigned)dataBits) - 1));
    }
    if (ascii) {
        // ★ ASCII: the character itself — printable, or CR / LF / TAB; anything else (NUL, idle, control) is dropped.
        const uint8_t c = dataB & 0x7f;
        return (c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t' ? (char32_t)c : 0;
    }
    if (firstChar) { lastCode = dataB; firstChar = false; return 0; }
    char32_t out = 0;
    if (lastCode == letters)      shift = false;
    else if (lastCode == figures) shift = true;
    else                          out = codeToChar(lastCode, shift);
    if (usos && out == U' ') shift = false;   // ★ unshift on space (option, off by default)
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
        U,U,U,U,U,U,U,U'-',U,U,U,U'\'',U,U'8',U'7',U,   // 0x4B = BEL: printed as ' (below)
        U,U,U,U'$',U,U'4',U'3',U,U,U',',U,U,U' ',U,U,U,
        U,U,U,U'"',U,U')',U,U,U,U'#',U,U,U'\n',U,U,U,
        U,U'9',U'?',U,U'5',U,U,U,U'\r',U,U,U,U,U,U,U };
    /* ★ FIGS 0x4B IS BEL (audit 2026-10-04, row 13). The table held a raw 7, which went through the host to the UI as a
     *  control byte. fldigi's filter_print prints an apostrophe for it ("it should be a beep, but French NAVTEX
     *  displays a quote") — the same here, in the table, so no path can emit the 7. */
    for (int c = 0; c < 128; c++) { ltrs[c] = L[c]; figs[c] = F[c];
        if (fourMarkBits((uint8_t)c)) { validCodes[c] = true;
            if (L[c] != U'_') codeLtrs[(uint8_t)c] = L[c];
            if (F[c] != U'_') codeFigs[(uint8_t)c] = F[c]; } }
}
uint8_t Ccir476::hardCode(const double* w) {
    uint8_t c = 0;
    for (int i = 0; i < 7; i++) if (w[i] > 0) c |= (uint8_t)(1 << i);
    return c;
}
char32_t Ccir476::codeToChar(uint8_t code, bool fig) const {
    auto& m = fig ? codeFigs : codeLtrs; auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}
/* ★★ FLIP THE LEAST CERTAIN BIT — CORRECTLY (2026-10-05). A valid word has 4 marks and 3 spaces; a word one bit from
 *  valid has 3 marks (flip the weakest space to mark) or 5 (flip the weakest mark to space). fldigi's flip_smallest_bit
 *  starts its mark count at 1, so the 5-mark case reads as 6 and never fires — half the repairs it means to make are
 *  never tried — and it flips the bit in the receive buffer itself, so the next tier (and the character sync's next
 *  look at that second) see a corrupted copy. Here: both cases, on a copy. Returns -1 when no single flip helps. */
static int flipWeakest(const double* w) {
    int marks = 0, weakMark = -1, weakSpace = -1;
    for (int i = 0; i < 7; i++) {
        if (w[i] > 0) { marks++; if (weakMark < 0 || w[i] < w[weakMark]) weakMark = i; }
        else if (weakSpace < 0 || w[i] > w[weakSpace]) weakSpace = i;
    }
    const int flip = marks == 3 ? weakSpace : marks == 5 ? weakMark : -1;
    if (flip < 0) return -1;
    return Ccir476::hardCode(w) ^ (1 << flip);
}
int Ccir476::decodeSoft(const double* rx, const double* dx, bool softFec, int& code, bool vote) {
    code = -1;
    const int r = hardCode(rx), d = dx ? hardCode(dx) : -1;
    const bool rOk = fourMarkBits((uint8_t)r), dOk = d >= 0 && fourMarkBits((uint8_t)d);
    const bool dReal = dOk && d != codeRep && d != codeAlpha;
    if (rOk && !(dReal && (r == codeRep || r == codeAlpha))) {
        /* ★★ TWO VALID COPIES THAT DISAGREE (2026-10-05, beyond fldigi). A 4-of-7 word two bits wrong is another valid
         *  word, so a burst on the RX copy prints the wrong letter with full confidence — fldigi never looks at the DX
         *  copy when the RX copy is valid. When both are valid and differ, the copy the two together favour wins:
         *  each candidate scored against the summed soft bits. Measured: bursts 20/min at +20 dB, 15 dB SNR. */
        if (vote && dReal && d != r) {
            double sr = 0, sd = 0;
            for (int i = 0; i < 7; i++) {
                const double w = rx[i] + dx[i];
                sr += (r >> i & 1) ? w : -w; sd += (d >> i & 1) ? w : -w;
            }
            if (sd > sr) { code = d; return 0; }
        }
        code = r; return 1;
    }
    if (dOk) { if (d != codeRep) code = d; return 0; }     // a DX rep is phasing: nothing to print
    if (!dx) return -1;                                   // no DX copy yet (the first second after a lock)
    if (!softFec) return -2;
    double sum[7];
    for (int i = 0; i < 7; i++) sum[i] = rx[i] + dx[i];
    int c = hardCode(sum);
    if (fourMarkBits((uint8_t)c)) { code = c; return -1; }
    for (const double* w : { rx, dx, (const double*)sum }) {
        c = flipWeakest(w);
        if (c >= 0) { code = c; return -1; }
    }
    return -2;
}
char32_t Ccir476::emit(int code, bool& phaseWrong) {
    phaseWrong = false;
    char32_t out = 0;
    if (code < 0) {
        /* ★ ITU-R M.476: an unrecoverable character prints an error symbol — but only in TEXT. Between messages the
         *  sender is phasing (and the first second after a lock is the receiver settling); a '_' there is noise
         *  dressed as a missing letter. So: after a real character, and the last good word stays the reference. */
        const bool inText = lastCode >= 0 && lastCode != codeRep && lastCode != codeAlpha && lastCode != codeChar32;
        return inText ? U'_' : 0;
    }
    if (code == codeRep) phaseWrong = lastCode == codeRep;
    else if (code == codeAlpha || code == codeBeta || code == codeChar32) {}
    else if (code == letters) shift = false;
    else if (code == figures) shift = true;
    else out = codeToChar((uint8_t)code, shift);
    lastCode = code;
    return out;
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
    if (f.size() < 3 || f[0] < '5' || f[0] > '8' || std::string("NEOMS").find(f[1]) == std::string::npos) return false;
    const std::string stop = f.substr(2);
    // ★ 1.5 stop is the 5-bit, no-parity Baudot case only (it uses the half-bit sampler).
    return stop == "1" || stop == "2" || (stop == "1.5" && f[0] == '5' && f[1] == 'N');
}

FskDecoder::~FskDecoder() { delete ita2; delete navtex_; }
FskDecoder::FskDecoder(int sr, double cf, double sh, double baud,
                       const std::string& fr, const std::string& enc, bool inv, const NavtexOptions* navtexOpts)
    : sampleRate((double)sr), centerFrequency(clampFinite(cf, 100.0, 10000.0, 1000.0)),
      shiftHz(clampFinite(sh, 10.0, 2000.0, 170.0)), baudRate(clampFinite(baud, 10.0, 1200.0, 45.45)),
      inverted(inv), framing(validItaFraming(fr) ? fr : "5N1.5"), encoding(enc) {
    deviationF = shiftHz / 2.0;
    audioAverageTC = 1000.0 / sampleRate;
    if (baudRate < 10) baudRate = 10;
    if (encoding == "CCIR476") {
        // ★★ NAVTEX has its own receiver (2026-10-05): none of the RTTY machinery below applies to it.
        navtex_ = new NavtexRx(sr, centerFrequency, shiftHz, baudRate, inverted, navtexOpts ? *navtexOpts : NavtexOptions());
        navtex_->onChar  = [this](char32_t c) { if (onChar) onChar(c); };
        navtex_->onState = [this](int st) { if (onState) onState(st); };
        nbits = 7; msb = 0x40;
    } else {
        ita2 = new Ita2(framing.empty() ? "5N1.5" : framing);
        ita2->ascii = encoding == "ASCII";   // ★ ASCII is decoded now (it was silently ITA2 until 2026-10-04)
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
    if (navtex_) { navtex_->process(samples, count); return; }
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
        else if (state == NoSignal) syncSetup = true;   // signal lost: a new start
        if (!pulseEdgeEvent) { sampleCount++; continue; }
        processBit(averagedMarkState);
        sampleCount++;
    }
}
// coder helper (ITA2 / ASCII — NAVTEX never reaches processBit)
static inline bool ckCode(Ita2* i, uint16_t code) { return i && i->checkBits(code); }

void FskDecoder::processBit(bool bit) {
    uint16_t bitVal = bit ? 1 : 0;
    if (syncSetup) {
        bitCount = 0; codeBits = 0; errorCount = 0; validCount = 0; lockRequired_ = 2;
        if (ita2) ita2->reset();
        syncChars.clear(); setState(Sync1); syncSetup = false;
    }
    switch (state) {
        case NoSignal: break;
        case Sync1: {
            codeBits = (uint16_t)((codeBits >> 1) | (bitVal * msb));
            if (ckCode(ita2, codeBits)) {
                syncChars.push_back(codeBits); validCount++;
                bitCount = 0; codeBits = 0; setState(Sync2); waiting = true;
            }
            break;
        }
        case Sync2: {
            /* ★★★ WAIT FOR THE START BIT AT EVERY STOP LENGTH (Stuart, 2026-10-04: PBB Den Helder, 75 Bd 850 Hz reverse,
             *  1 stop bit, would not decode at any setting). Only 5N1.5 waited; 5N1 / 5N2 assumed characters back to back,
             *  so the idle mark between PBB's characters (69 % of its airtime) knocked every frame out of step. A real
             *  teleprinter is asynchronous: after the stop it waits on mark for the next start bit. Measured on Stuart's
             *  recording: a textbook start-bit UART decodes 538 frames, 0 bad, "02A 04B 06A 08B 12X 17B 22X 26Y PBB". */
            if ((stopVariable || ita2) && waiting && bit) return;   // ★ ITA2 is ASYNC at every stop length — see Sync2
            waiting = false;
            codeBits = (uint16_t)((codeBits >> 1) | (bitVal * msb)); bitCount++;
            if (bitCount == nbits) {
                if (ckCode(ita2, codeBits)) {
                    syncChars.push_back(codeBits); codeBits = 0; bitCount = 0; validCount++;
                    // ★ ITA2: two good characters after the first before anything prints (three in a row) — one alone
                    //   was enough to lock on noise or mid-character. After a framing slip (lockRequired_ = 1) the
                    //   decoder was already in step a moment ago, so one is enough to resume.
                    if (validCount >= lockRequired_) {
                        for (uint16_t c : syncChars) processCharacter(c);
                        setState(ReadData);
                    }
                } else { codeBits = 0; bitCount = 0; syncSetup = true; }
                waiting = true;
            }
            break;
        }
        case ReadData: {
            if ((stopVariable || ita2) && waiting && bit) return;
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
                goodFrames_++;
                processCharacter(codeBits);
                if (errorCount > 0) errorCount--;   // every frame that reached here is good
                codeBits = 0; bitCount = 0; waiting = true;
            }
            break;
        }
    }
}
void FskDecoder::processCharacter(uint16_t code) {
    if (!ita2) return;
    const char32_t ch = ita2->processChar(code);
    if (ch != 0 && onChar) onChar(ch);
}

// ── NavtexRx (2026-10-05) ────────────────────────────────────────────────────
/* ★★★ NAVTEX GOT ITS OWN RECEIVER (Stuart, 2026-10-05: "make ours the best it can be now whilst we have it in front of
 *  us"). It had been the RTTY decoder with a 7-bit coder bolted on: a bit clock from zero crossings, a character lock on
 *  four valid words in a row (27 % of RANDOM 7-bit words are valid 4-of-7 words, so noise locked it), a DX/RX phase it
 *  could only guess until phasing came round, and FEC that was "the RX copy, else the DX copy". This is fldigi's NAVTEX
 *  receiver (navtex.cxx — Rémi Chateauneu F4ECW, Rik van Riel AB1KW; via madpsy/ubersdr_navtex, Franco Venturi's port;
 *  GPL-3.0-or-later), ported piece by piece and each piece MEASURED (NavtexOptions):
 *   · character sync from one second of SOFT bit values: every bit and DX/RX offset tried (14), in both polarities,
 *     locking only where ≥3 characters match their DX copies — so it also joins mid-message, phase and all;
 *   · soft FEC: RX, DX, the two summed, then the least certain bit flipped (Ccir476::decodeSoft);
 *   · an early/prompt/late bit clock on log-compressed decision levels, at the exact (fractional) bit length;
 *   · the tones mixed to baseband and filtered by a raised cosine matched to 100 baud (after a cheap decimation).
 *  RTTY never comes here — FskDecoder's own path is untouched. */
static const int kDx = 35;   // a character's DX copy: five slots (35 bits) before its RX copy
static inline double decayAvg(double avg, double in, double w) { return w <= 1 ? in : avg + (in - avg) / w; }

NavtexRx::NavtexRx(int sr, double cf, double sh, double baud, bool inv, const NavtexOptions& o)
    : o_(o), fs_((double)sr), cf_(cf), shift_(sh), baud_(baud), pol_(inv ? -1 : 1) {
    if (o_.rcDemod) {
        /* ★ Decimate first: the raised cosine spans four bits, which is 1920 taps × 4 at 48 kHz and 240 at 6 kHz. The
         *  largest step that keeps the upper tone (+ the filter's 140 Hz) well inside the new Nyquist. */
        const double top = cf_ + shift_ / 2 + 150;
        for (int d : { 8, 6, 4, 3, 2, 1 }) if (d == 1 || fs_ / d / 2 >= top * 1.25) { decim_ = d; break; }
        mixRate_ = fs_ / decim_;
        /* ★ ...and the raised cosine is only READ every rcStep_-th sample: at baseband the signal is ±140 Hz wide, so
         *  ~20 samples a bit (2 kHz) carry it all. Measured: the same CER as reading every sample, a third of the cost. */
        rcStep_ = std::max(1, (int)(mixRate_ / (20 * baud_)));
        frontRate_ = mixRate_ / rcStep_;
        if (decim_ > 1) {
            // Blackman windowed sinc, cut at the new Nyquist; transition = the room between the upper tone and its alias
            const double tw = frontRate_ / 2 - top;
            int n = (int)std::ceil(5.5 * fs_ / (2 * tw)) | 1; n = std::min(255, std::max(15, n));
            decH_.resize(n); double sum = 0;
            for (int k = 0; k < n; k++) {
                const double t = k - (n - 1) / 2.0, fc = 0.5 / decim_;
                const double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
                const double w = 0.42 - 0.5 * std::cos(2 * M_PI * k / (n - 1)) + 0.08 * std::cos(4 * M_PI * k / (n - 1));
                decH_[k] = sinc * w; sum += decH_[k];
            }
            for (double& h : decH_) h /= sum;
            decBuf_.assign(2 * n, 0.0);
        }
        /* ★ fldigi's rtty_filter: a raised cosine (cos²) falling to zero at 1.4 × the baud rate — the factor fldigi
         *  measured best for CER at -9 dB SNR — so 140 Hz at 100 baud, and the other tone (170 Hz away) is rejected.
         *  Built from its frequency response, ±2 bits long, Hann-tapered. */
        const double W = 1.4 * baud_;
        const int half = (int)std::lround(2 * mixRate_ / baud_);
        rcH_.resize(2 * half + 1); double sum = 0;
        for (int k = -half; k <= half; k++) {
            double h = 0; const int steps = 400;
            for (int i = 0; i <= steps; i++) {
                const double F = W * i / steps, c = std::cos(M_PI * F / (2 * W));
                h += (i == 0 || i == steps ? 0.5 : 1.0) * c * c * std::cos(2 * M_PI * F * k / mixRate_);
            }
            h *= 0.5 + 0.5 * std::cos(M_PI * k / (half + 1));
            rcH_[k + half] = h; sum += h;
        }
        for (double& h : rcH_) h /= sum;
        for (auto& b : rcBuf_) b.assign(2 * rcH_.size(), 0.0);
    } else {
        // the RTTY decoder's front end, as it was (Q ≈ 3 bandpasses with ka9q's qv offset, 140 Hz envelope lowpass)
        frontRate_ = mixRate_ = fs_;
        const double q = 6.0 * cf_ / 1000.0, qv = cf_ + 4000.0 / cf_;
        bpMark_.configure(BiQuad::Bandpass, qv + shift_ / 2, fs_, q);
        bpSpace_.configure(BiQuad::Bandpass, qv - shift_ / 2, fs_, q);
        lpMark_.configure(BiQuad::Lowpass, 140.0, fs_, 1.0 / std::sqrt(2.0));
        lpSpace_.configure(BiQuad::Lowpass, 140.0, fs_, 1.0 / std::sqrt(2.0));
    }
    bitSamples_ = frontRate_ / baud_;
    audioTC_ = std::min(1.0, 1000.0 / frontRate_);   // the RTTY decoder's 1 ms level average
    nextEarly_ = 0; nextPrompt_ = bitSamples_ / 5; nextLate_ = bitSamples_ * 2 / 5;   // fldigi: ±1/5 bit
    zcBitCount_ = std::max(4, (int)std::lround(bitSamples_)); zcHalf_ = zcBitCount_ / 2;
    zcHist_.assign((zcBitCount_ + 3) / 4, 0);
    retune();
}
void NavtexRx::retune() {
    const double fm = cf_ + afcHz_ + shift_ / 2, fsp = cf_ + afcHz_ - shift_ / 2;
    mStepRe_ = std::cos(2 * M_PI * fm / mixRate_);  mStepIm_ = -std::sin(2 * M_PI * fm / mixRate_);
    sStepRe_ = std::cos(2 * M_PI * fsp / mixRate_); sStepIm_ = -std::sin(2 * M_PI * fsp / mixRate_);
}
void NavtexRx::setState(State s) {
    if (s == state_) return;
    state_ = s;
    if (onState) onState((int)s);
}
void NavtexRx::process(const int16_t* samples, int count) {
    for (int n = 0; n < count; n++) {
        const double dv = (double)samples[n];
        if (!o_.rcDemod) {
            const double ma = std::fabs(bpMark_.filter(dv)), sa = std::fabs(bpSpace_.filter(dv));
            frontSample(std::max(0.0, lpMark_.filter(ma)), std::max(0.0, lpSpace_.filter(sa)), std::max(ma, sa));
            continue;
        }
        double y = dv;
        if (decim_ > 1) {
            const int N = (int)decH_.size();
            decPos_ = (decPos_ + 1) % N; decBuf_[decPos_] = decBuf_[decPos_ + N] = dv;
            if (++decCount_ < decim_) continue;
            decCount_ = 0; y = 0;
            const double* b = &decBuf_[decPos_ + 1];
            for (int k = 0; k < N; k++) y += decH_[k] * b[k];
        }
        // mix each tone to 0 Hz, then the raised cosine on I and Q
        const double in[4] = { y * mRe_, y * mIm_, y * sRe_, y * sIm_ };
        double t = mRe_ * mStepRe_ - mIm_ * mStepIm_; mIm_ = mRe_ * mStepIm_ + mIm_ * mStepRe_; mRe_ = t;
        t = sRe_ * sStepRe_ - sIm_ * sStepIm_; sIm_ = sRe_ * sStepIm_ + sIm_ * sStepRe_; sRe_ = t;
        if ((++oscCount_ & 1023) == 0) {   // keep the phasors on the unit circle
            double g = 1.0 / std::hypot(mRe_, mIm_); mRe_ *= g; mIm_ *= g;
            g = 1.0 / std::hypot(sRe_, sIm_); sRe_ *= g; sIm_ *= g;
        }
        const int N = (int)rcH_.size();
        rcPos_ = (rcPos_ + 1) % N;
        for (int c = 0; c < 4; c++) rcBuf_[c][rcPos_] = rcBuf_[c][rcPos_ + N] = in[c];
        if (++rcCount_ < rcStep_) continue;
        rcCount_ = 0;
        double out[4];
        for (int c = 0; c < 4; c++) {
            const double* p = &rcBuf_[c][rcPos_ + 1]; double acc = 0;
            for (int k = 0; k < N; k++) acc += rcH_[k] * p[k];
            out[c] = acc;
        }
        // ×2: a tone of amplitude A mixes to A/2 at 0 Hz. The level gate is the RTTY decoder's (mean |bandpass| = 2A/π).
        const double m = 2 * std::hypot(out[0], out[1]), s = 2 * std::hypot(out[2], out[3]);
        frontSample(m, s, (2 / M_PI) * std::max(m, s));
    }
}
void NavtexRx::frontSample(double m, double s, double level) {
    audioAverage_ += (level - audioAverage_) * audioTC_;
    audioAverage_ = std::max(0.1, audioAverage_);
    const double bs = bitSamples_;
    double logic;
    if (o_.atcHalf) {
        /* ★ W7AY's automatic threshold correction, as fldigi's NAVTEX has it: each tone against its own peak and its own
         *  noise, both clipped to [noise floor, peak], and the threshold half way: ½[(M − n)² − (S − n)²]. */
        markEnv_  = decayAvg(markEnv_,  m, m > markEnv_  ? bs / 4 : bs * 16);
        spaceEnv_ = decayAvg(spaceEnv_, s, s > spaceEnv_ ? bs / 4 : bs * 16);
        markNoise_  = decayAvg(markNoise_,  m, m < markNoise_  ? bs / 4 : bs * 48);
        spaceNoise_ = decayAvg(spaceNoise_, s, s < spaceNoise_ ? bs / 4 : bs * 48);
        const double nf = (markNoise_ + spaceNoise_) / 2;
        const double mc = std::max(std::min(m, markEnv_), nf), sc = std::max(std::min(s, spaceEnv_), nf);
        const double M = markEnv_ - nf, S = spaceEnv_ - nf;
        logic = (mc - nf) * M - (sc - nf) * S - 0.5 * (M * M - S * S);
    } else {
        // the RTTY decoder's optimal ATC (FskDecoder::process): a shared floor from the weaker tone, ¼
        markEnv_  = decayAvg(markEnv_,  m, m > markEnv_  ? bs / 4 : bs * 16);
        spaceEnv_ = decayAvg(spaceEnv_, s, s > spaceEnv_ ? bs / 4 : bs * 16);
        const double lo = std::min(m, s);
        noiseFloor_ = decayAvg(noiseFloor_, lo, lo < noiseFloor_ ? bs / 4 : bs * 48);
        const double mc = std::min(m, markEnv_) - noiseFloor_, sc = std::min(s, spaceEnv_) - noiseFloor_;
        const double M = markEnv_ - noiseFloor_, S = spaceEnv_ - noiseFloor_;
        logic = mc * M - sc * S - 0.25 * (M * M - S * S);
    }
    /* ★ fldigi: "the logarithm of the logic level tells the bit synchronization and character decoding which samples
     *  were decoded well, and which poorly" — a bit is the sum of its samples, each weighted by log(1 + |level|). */
    const double v = o_.logSoft ? std::copysign(std::log1p(std::fabs(logic)), logic) : (logic > 0 ? 1.0 : -1.0);

    /* ★★ THE LEVEL GATE ONLY SAYS "NO SIGNAL" — IT NO LONGER DROPS THE LOCK (2026-10-05). The RTTY decoder's 1 ms level
     *  check threw the whole decoder state away whenever the audio dipped under 256; a NAVTEX signal in a deep fade
     *  dips there and comes straight back, and on the 20 dB selective-fading bench it cost 12 lock losses in 2 minutes
     *  (CER 61 % against 13 % without). The lock is now the characters' business alone (errorCount_); the level picks
     *  "no signal" or "searching" while there is no lock. */
    quiet_ = audioAverage_ < audioMinimum_;
    if (state_ != ReadData) setState(quiet_ ? NoSignal : Hunting);

    if (o_.earlyLate) {
        /* ★★ EARLY / PROMPT / LATE (fldigi, Rik van Riel AB1KW): the bit is integrated three times, a fifth of a bit apart;
         *  the prompt sum is the bit. Every 8 bits the clock slides toward whichever of early/late integrates the bigger
         *  magnitude — by a small step, or straight onto it when prompt sits in a trough between the two — so it centres
         *  on the bits without leaning on noisy zero crossings. The bit length is exact (fractional), not rounded. */
        if (sampleCount_ % (long long)(bs * 8) == 0) {
            double slope = avgLate_ - avgEarly_;
            if (avgPrompt_ * 1.05 < avgEarly_ && avgPrompt_ * 1.05 < avgLate_) {
                if (avgEarly_ > avgLate_) {
                    slope = std::fmod(nextEarly_ - nextPrompt_ - bs, bs);
                    avgLate_ = avgPrompt_; avgPrompt_ = avgEarly_;
                } else {
                    slope = std::fmod(nextLate_ - nextPrompt_ + bs, bs);
                    avgEarly_ = avgPrompt_; avgPrompt_ = avgLate_;
                }
            } else slope /= 1024;
            nextEarly_ += slope; nextPrompt_ += slope; nextLate_ += slope;
        }
        early_ += v; prompt_ += v; late_ += v;
        const double t = (double)sampleCount_;
        if (t >= nextEarly_) { avgEarly_ = decayAvg(avgEarly_, std::fabs(early_), 64); nextEarly_ += bs; early_ = 0; }
        if (t >= nextLate_)  { avgLate_  = decayAvg(avgLate_,  std::fabs(late_),  64); nextLate_  += bs; late_  = 0; }
        if (t >= nextPrompt_) {
            avgPrompt_ = decayAvg(avgPrompt_, std::fabs(prompt_), 64); nextPrompt_ += bs;
            const double bit = prompt_; prompt_ = 0;
            sampleCount_++;
            pushBit(bit);
            return;
        }
    } else {
        // the RTTY decoder's zero-crossing histogram (FskDecoder::process), integer bit length
        const int N = zcBitCount_;
        const bool mark = v > 0;
        prompt_ += v; zcDuration_++;
        if (mark != zcOld_) {
            if ((zcDuration_ % N) > zcHalf_) {
                long long idx = (sampleCount_ - zcNext_ + (long long)N * 8) % N; if (idx < 0) idx += N;
                const size_t zi = (size_t)(idx / 4); if (zi < zcHist_.size()) zcHist_[zi]++;
            }
            zcDuration_ = 0;
        }
        zcOld_ = mark;
        if (sampleCount_ % N == 0 && ++zcRounds_ >= 16) {
            int best = 0, bi = 0;
            for (int j = 0; j < (int)zcHist_.size(); j++) { if (zcHist_[j] > best) { best = zcHist_[j]; bi = j; } zcHist_[j] = 0; }
            if (best > 0) { bi *= 4; bi = ((bi + zcHalf_) % N) - zcHalf_; zcDelta_ = bi / 8; }
            zcRounds_ = 0;
        }
        if (sampleCount_ >= zcNext_) {
            zcNext_ = (int)(sampleCount_ + N + (int)(zcDelta_ + 0.5)); zcDelta_ = 0;
            const double bit = prompt_; prompt_ = 0;
            sampleCount_++;
            pushBit(bit);
            return;
        }
    }
    sampleCount_++;
}
/* ★★★ fldigi's find_alpha_characters, in both polarities. One second of bit values holds 14 characters; with 7 bits a
 *  character and DX/RX interleaved, the first RX character with its DX copy in view can start at any of 14 offsets.
 *  Each offset scores its valid words, plus one for every word that MATCHES its DX copy 35 bits back (or a phasing
 *  0x0F with its 0x66 before it). Lock needs ≥3 such matches and a score of 9 or more: four valid words in a row, the
 *  old rule, happen by chance in noise (27 % of 7-bit words are valid); three DX/RX matches essentially never do. The
 *  match also says which slots are RX — the DX/RX phase — so a receiver that joins MID-MESSAGE, with no phasing to
 *  come, is in phase from its first character. Inverted tones turn every 4-mark word into a 3-mark one, never valid:
 *  the other polarity cannot score, so trying both is free and decides the polarity itself. */
int NavtexRx::findAlpha(int& polOut) const {
    auto code = [this](int p, int i) {
        uint8_t c = 0;
        for (int b = 0; b < 7; b++) if (p * bits_[i + b] > 0) c |= (uint8_t)(1 << b);
        return c;
    };
    int best = 0, bestOff = -1, bestPol = pol_;
    for (int p : { pol_, -pol_ }) {
        if (p != pol_ && !o_.autoInvert) continue;
        for (int off = kDx; off < kDx + 14; off++) {
            int score = 0, reps = 0;
            for (int i = off; i < kBits - 7; i += 7) {
                const uint8_t c = code(p, i);
                if (!Ccir476::fourMarkBits(c)) continue;
                score++;
                const uint8_t dx = code(p, i - kDx);
                if (c == dx) {
                    if (c == 0x0f || c == 0x66) { score = 0; continue; }   // phasing in step with itself: DX/RX are odd
                    reps++;
                } else if (c == 0x0f && code(p, i - 7) == 0x66) reps++;
            }
            if (reps >= 3 && score + reps > best) { best = score + reps; bestOff = off; bestPol = p; }
        }
    }
    if (best <= 8) return -1;
    polOut = bestPol;
    return bestOff;
}
void NavtexRx::pushBit(double v) {
    std::memmove(bits_, bits_ + 1, (kBits - 1) * sizeof(double));
    bits_[kBits - 1] = v;
    absBit_++;
    if (cursor_ > 0) cursor_--;
    auto absOf = [this](int pos) { return absBit_ - kBits + pos; };
    if (state_ != ReadData) {
        int pol = pol_;
        const int off = findAlpha(pol);
        if (off >= 0) {
            pol_ = pol; cursor_ = off; alphaPhase_ = true; errorCount_ = 0;
            // ★ Five seconds or more without a lock is a new transmission: letters, as every message starts
            if (absBit_ - lostAt_ > 5 * (long long)baud_) coder_.reset();
            /* ★ fldigi re-decodes the whole second it locked on — after a resync that printed those characters twice.
             *  RX slots already decoded are skipped (half a slot either way, so a one-bit slip is not read twice). */
            while (absOf(cursor_) < lastRxAbs_ + 7) cursor_ += 14;
            setState(ReadData);
        }
    }
    if (state_ != ReadData || cursor_ >= kBits - 7) return;
    if (alphaPhase_) processRx(cursor_);
    alphaPhase_ = !alphaPhase_;
    cursor_ += 7;
}
void NavtexRx::processRx(int pos) {
    double rx[7], dx[7];
    const bool haveDx = pos >= kDx;
    for (int b = 0; b < 7; b++) {
        rx[b] = pol_ * bits_[pos + b];
        if (haveDx) dx[b] = pol_ * bits_[pos - kDx + b];
    }
    int code = -1;
    const int r = Ccir476::decodeSoft(rx, haveDx ? dx : nullptr, o_.softFec, code, o_.fecVote);
    lastRxAbs_ = absBit_ - kBits + pos;
    if (code >= 0 || r == -2) {
        bool phaseWrong = false;
        const char32_t c = coder_.emit(code, phaseWrong);
        if (code >= 0 ? code != 0x66 && code != 0x0f : c != 0) {   // count characters, not phasing
            Counts* cs[2] = { &total, &message };
            for (Counts* k : cs) { if (r == 1) k->clean++; else if (r == -2) k->failed++; else k->repaired++; }
        }
        // ★ fldigi's process_char: a second phasing rep in an RX slot means DX/RX are swapped — step one slot
        if (phaseWrong) alphaPhase_ = false;
        if (c) {
            tail_ += (char)(c < 128 ? c : '?');
            if (tail_.size() > 4) tail_.erase(0, tail_.size() - 4);
            if (tail_ == "ZCZC") message = Counts();
            else if (tail_ == "NNNN") { lastMessage = message; message = Counts(); }
            if (onChar) onChar(c);
        }
    }
    errorCount_ -= r;
    if (errorCount_ < 0) errorCount_ = 0;
    /* ★ fldigi's handle_bit_value: the lock goes past 5 (+1 a clean RX copy, 0 repaired, -1 soft repair, -2 lost).
     *  Letters/figures are the sender's and are kept; the next lock may be the same one, a bit over, or the other phase. */
    if (errorCount_ > 5) { resyncs_++; lostAt_ = absBit_; setState(quiet_ ? NoSignal : Hunting); }
}

} // namespace vibe
