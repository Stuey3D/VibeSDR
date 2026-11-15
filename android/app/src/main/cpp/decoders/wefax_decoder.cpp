// VibeSDR V4 — WEFAX decoder (C++ port of ka9q audio_extensions/wefax/decoder.go).
#include "wefax_decoder.h"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vibe {

// ── 17-tap FIR (ACfax low-pass coefficients) ────────────────────────────────
double WefaxFIR::apply(double sample) {
    static const double lpf[3][17] = {
        {-7, -18, -15, 11, 56, 116, 177, 223, 240, 223, 177, 116, 56, 11, -15, -18, -7},  // narrow
        {0, -18, -38, -39, 0, 83, 191, 284, 320, 284, 191, 83, 0, -39, -38, -18, 0},      // middle
        {6, 20, 7, -42, -74, -12, 159, 353, 440, 353, 159, -12, -74, -42, 7, 20, 6},      // wide
    };
    const double* c = lpf[bw < 0 ? 0 : (bw > 2 ? 2 : bw)];
    buffer[current] = sample;
    double sum = 0.0;
    int idx = current;
    for (int i = 0; i < 17; i++) {
        sum += buffer[idx] * c[i];
        if (++idx >= 17) idx = 0;
    }
    if (--current < 0) current = 16;
    return sum;
}

// ── Helpers ─────────────────────────────────────────────────────────────────
static int medianOf(std::vector<int> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ── Decoder ─────────────────────────────────────────────────────────────────
/* ★★★ THE ATTACH MESSAGE IS A STRANGER'S (audit 2026-10-03). lpm and image_width arrive from any
 *  listener's JSON: lpm 0 divided by zero, a width of 0 or 2^31 sized every buffer below, and a NaN
 *  deviation turned every pixel into undefined behaviour. WEFAX is transmitted at 60, 90, 120 or 240
 *  lines per minute and nothing else; IOC 576 is 1809 pixels and IOC 288 is 904, so 256..4096
 *  covers every real chart with room to spare. Anything else falls back to the defaults. */
static int saneLpm(int v) { return (v == 60 || v == 90 || v == 120 || v == 240) ? v : 120; }
static int saneWidth(int v) { return v < 256 ? 256 : (v > 4096 ? 4096 : v); }
static double saneHz(double v, double lo, double hi, double dflt) { return (std::isfinite(v) && v >= lo && v <= hi) ? v : dflt; }

WefaxDecoder::WefaxDecoder(int sampleRate, const Config& cfg)
    : lpm(saneLpm(cfg.lpm)), imageWidth(saneWidth(cfg.imageWidth)), bandwidth(cfg.bandwidth),
      carrier(saneHz(cfg.carrier, 100.0, 20000.0, 1900.0)), deviation(saneHz(cfg.deviation, 10.0, 5000.0, 400.0)),
      usePhasing(cfg.usePhasing), autoStop(cfg.autoStop), autoStart(cfg.autoStart),
      includeHeaders(cfg.includeHeaders),
      samplesPerSec((double)sampleRate),
      firI(cfg.bandwidth), firQ(cfg.bandwidth) {

    skipHeaderDetection = !usePhasing && !autoStop && !autoStart;
    samplesPerLine = (int)(samplesPerSec * 60.0 / (double)lpm);

    samples.assign(samplesPerLine, 0);
    demodData.assign(samplesPerLine, 0);
    phasingPos.assign(phasingLines, 0);
    std::fill(lookRing, lookRing + LOOK_WINDOW, -1);

    imgData.assign((size_t)imageWidth * 2, 0);   // ★ two lines, not the image — see decodeFaxLine
    outImage.assign(imageWidth, 0);
    lineIncrFrac = (double)imageWidth / (M_PI * 576.0);
}

void WefaxDecoder::process(const int16_t* samps, int count) {
    int i = 0;
    if (skip > 0) {
        int s = std::min(skip, count);
        i += s;
        skip -= s;
    }
    while (i < count) {
        while (i < count && sampIdx < samplesPerLine) {
            samples[sampIdx++] = samps[i++];
        }
        if (sampIdx == samplesPerLine) {
            decodeFaxLine();
            sampIdx = 0;
        }
    }
}

void WefaxDecoder::demodulateData() {
    double phaseInc = carrier / samplesPerSec;
    double phase = 0.0;
    double scale = -1.3 * (samplesPerSec / deviation / 8.0);

    for (int i = 0; i < samplesPerLine; i++) {
        double samp = (double)samples[i] / 32768.0;
        double iCur = firI.apply(samp * std::cos(2 * M_PI * phase));
        double qCur = firQ.apply(samp * std::sin(2 * M_PI * phase));
        phase += phaseInc;
        if (phase > 1.0) phase -= 1.0;

        double mag = std::sqrt(qCur * qCur + iCur * iCur);
        if (mag > 0) { iCur /= mag; qCur /= mag; }

        double x = (iCur * (qCur - qPrev) - qCur * (iCur - iPrev)) * scale;
        x = x / 2.0 + 0.5;
        int pixel = (int)(x * 255.0);
        pixel = pixel < 0 ? 0 : (pixel > 255 ? 255 : pixel);
        demodData[i] = (uint8_t)pixel;

        iPrev = iCur; qPrev = qCur;
    }
}

double WefaxDecoder::fourierTransformSub(const uint8_t* buf, int len, int freq) {
    double k = -2 * M_PI * (double)freq * 60.0 / (double)lpm / (double)samplesPerLine;
    double retr = 0.0, reti = 0.0;
    for (int n = 0; n < len; n++) {
        retr += (double)buf[n] * std::cos(k * n);
        reti += (double)buf[n] * std::sin(k * n);
    }
    return std::sqrt(retr * retr + reti * reti);
}

WefaxDecoder::HeaderType WefaxDecoder::detectLineType(const uint8_t* buf, int len) {
    // ★ 3.5 on the whole line (2026-10-07) — see TONE_HITS: a tone is judged by how many recent lines carry it.
    const double threshold = TONE_LEVEL;
    double startDet = fourierTransformSub(buf, len, startIOC576Frequency) / (double)len;
    double stopDet  = fourierTransformSub(buf, len, stopFrequency) / (double)len;
#ifdef WEFAX_DIAG_TONES
    if (onDiag) { char m[96]; std::snprintf(m, sizeof m, "tone? start %.2f stop %.2f", startDet, stopDet); onDiag(m); }
#endif
    if (startDet > threshold) return HeaderStart;
    if (stopDet  > threshold) return HeaderStop;
    return HeaderImage;
}

/* ★★ ON BIN AVERAGES, NOT RAW SAMPLES (2026-11-15). The pulse search ran on the raw demod: at 48 kHz the 17-tap FIR passes
 *  the mixer's 2f image, so a black or white run demodulates as a ripple that saturates sample by sample (a phasing line
 *  is 0 and 255 interleaved, its bins average ~16 black and ~45 white), and the ripple follows the carrier's phase against
 *  the line. Some alignments then read a pulse that is not there, on EVERY line — the "one phasing line in seven reads
 *  3.7 %" artefact the phasing test recorded, and, found while building the new-chart rule, a synthetic phasing at 30 %
 *  read 3.5 % on all 38 lines and was phased a quarter of a line wrong. Averaged over LOOK_BINS bins (~47 samples; the
 *  old search stepped 52) the ripple is gone. Same window (7 % of the line, wedge-weighted), same answer (its centre,
 *  in samples) — and 512 × 36 adds per line instead of 462 × 420. */
void WefaxDecoder::binLine(const uint8_t* image) {
    for (int b = 0; b < LOOK_BINS; b++) {
        const int lo = (int)((long)b * samplesPerLine / LOOK_BINS), hi = (int)((long)(b + 1) * samplesPerLine / LOOK_BINS);
        int s = 0;
        for (int i = lo; i < hi; i++) s += image[i];
        lookBin[b] = hi > lo ? s / (hi - lo) : 0;
    }
}

int WefaxDecoder::pulseFromBins() const {
    const int nb = std::max(3, (int)std::lround(LOOK_BINS * 0.07)), h = nb / 2;
    long best = -1; int at = 0;
    for (int i = 0; i < LOOK_BINS; i++) {
        long t = 0;
        for (int j = 0; j < nb; j++) t += (long)(h - std::abs(j - h)) * lookBin[(i + j) % LOOK_BINS];
        if (t > best) { best = t; at = i; }
    }
    // the window's centre bin, at that bin's middle, in samples
    return (int)(((2L * (at + h) + 1) * samplesPerLine / (2L * LOOK_BINS)) % samplesPerLine);
}

int WefaxDecoder::faxPhasingLinePosition(const uint8_t* image) {
    binLine(image);
    return pulseFromBins();
}

/* ★ DOES THIS LINE LOOK LIKE PHASING? (see the ★★★ note in decodeFaxLine). Phasing is black across the line but for
 *  ONE short white pulse (5 % of the line sent; DDK off air and our own synthesis both read 3.1–3.3 % above half
 *  level). One pass of adds folds the line into LOOK_BINS bins; then, all on the bins: the brightest pulse-wide window,
 *  how far it stands above the rest (the contrast), how wide the bright part is, and how LEVEL the rest is — the rest
 *  judged on LOOK_SEGS coarse segments, so receiver noise (which averages out over a segment) does not count against
 *  it but chart content (paper and ink, which does not) does. The position it
 *  returns is the phasing window's own measure (pulseFromBins, on the same bins), so the positions handed to the
 *  window agree with the ones it takes itself. Returns the pulse position in samples, or −1. */
int WefaxDecoder::phasingLook() {
    if (samplesPerLine < LOOK_BINS) return -1;
    binLine(demodData.data());
    const int* bin = lookBin;
    // the brightest window one pulse wide (circular, running sum)
    const int pw = LOOK_BINS / 20;            // 5 % of the line: the pulse as sent
    int run = 0;
    for (int i = 0; i < pw; i++) run += bin[i];
    int best = run, at = 0;
    for (int i = 1; i < LOOK_BINS; i++) {
        run += bin[(i + pw - 1) % LOOK_BINS] - bin[i - 1];
        if (run > best) { best = run; at = i; }
    }
    const int c = (at + pw / 2) % LOOK_BINS;  // its centre
    auto cd = [&](int b) { int d = std::abs(b - c); return std::min(d, LOOK_BINS - d); };
    // the rest: everything clear of the pulse and its edges, in LOOK_SEGS segments
    const int clear = pw;                     // a pulse width either side of the centre
    long restSum = 0; int restN = 0;
    int segSum[LOOK_SEGS] = {0}, segN[LOOK_SEGS] = {0};
    for (int k = 0; k < LOOK_BINS; k++) {
        const int b = (c + k) % LOOK_BINS;    // walk from the pulse round the line, so the segments are contiguous
        if (cd(b) <= clear) continue;
        restSum += bin[b]; restN++;
        const int sg = (int)((long)k * LOOK_SEGS / LOOK_BINS);
        segSum[sg] += bin[b]; segN[sg]++;
    }
    if (restN == 0) return -1;
    const double rest = (double)restSum / restN, pulse = (double)best / pw;
    const double contrast = pulse - rest;
    // the spread of the segments' levels, the single highest and lowest set aside (one fade or click is not content)
    double seg[LOOK_SEGS]; int ns = 0;
    for (int sg = 0; sg < LOOK_SEGS; sg++) if (segN[sg] > 0) seg[ns++] = (double)segSum[sg] / segN[sg];
    if (ns < 4) return -1;
    std::sort(seg, seg + ns);
    const double spread = seg[ns - 2] - seg[1];
    // how wide the bright part is: out from the centre while above half-way between the rest and the pulse
    //   (one dark bin is a noise dip, two end it)
    const double half = rest + contrast / 2;
    int wide = 1;
    for (int dir = -1; dir <= 1; dir += 2) {
        int miss = 0;
        for (int k = 1; k < LOOK_BINS / 4 && miss < 2; k++) {
            if (bin[((c + dir * k) % LOOK_BINS + LOOK_BINS) % LOOK_BINS] > half) { wide += 1 + miss; miss = 0; } else miss++;
        }
    }
    const double frac = (double)wide / LOOK_BINS;
#ifdef WEFAX_DIAG_LOOK
    if (onDiag) { std::string m = "look: " + std::to_string(contrast) + " " + std::to_string(frac) + " "
        + std::to_string(100.0 * pulseFromBins() / samplesPerLine) + " segs";
        for (int sg = 0; sg < LOOK_SEGS; sg++) m += " " + std::to_string(segN[sg] ? segSum[sg] / segN[sg] : -1);
        onDiag(m); }
#endif
    if (contrast < LOOK_CONTRAST || frac < LOOK_PULSE_MIN || frac > LOOK_PULSE_MAX || spread > LOOK_REST_SPREAD * contrast)
        return -1;
    return pulseFromBins();
}

void WefaxDecoder::decodeFaxLine() {
    const int phasingSkipLines = 2;
    demodulateData();

    HeaderType lineType;
    if (skipHeaderDetection) {
        lineType = HeaderImage;
    } else {
        /* ★★★ THE WHOLE LINE, NOT ITS FIRST 62 ms (2026-10-07, NMF Boston off K3FEF: 50 minutes, four charts, ONE start
         *  tone caught). The tone was measured over the first 3000 samples of each line. On a 1000 km HF path the start
         *  tone FADES — its 300 Hz content swung 7…200 within single half-second lines — so a 62 ms look caught a fade
         *  on one line and a peak on the next: 7.4, 4.1, 8.4, 4.4, 7.8 … across a threshold of 5, the run counted up,
         *  down, up and never reached six. Measured over the whole line the same tone reads high on every line.
         *  (A whole-cycle 66.7 ms window was tried first and changed nothing — it was never the window's edges.) */
        int bufferLen = samplesPerLine;
        lineType = detectLineType(demodData.data(), bufferLen);
    }

    /* ★★★ HOW MANY OF THE LAST TONE_WINDOW LINES CARRY THE TONE — NOT AN UNBROKEN RUN (2026-10-07). A START needed six
     *  tone lines IN A ROW (one miss counted the run back down), so a fading tone never got there: NMF Boston off K3FEF
     *  caught 1 of 3 start tones in 50 minutes, JMH off WESSEX and NMC off WT8P 0. Per-line tone readings on real air
     *  run 1.8…10 through a tone and up to ~7 on single lines of a chart or noise, so no per-line threshold can tell
     *  them apart — persistence can. Measured on 25 recordings (~3 h: NMF, NMG-less nights, JMH, NMC, VMW, test cards,
     *  other modes): ≥ TONE_HITS of the last TONE_WINDOW at ≥ TONE_LEVEL found NMF 3/3, JMH-WESSEX and NMC 2013 (both
     *  missed before), every tone found before, and no false start anywhere. Each fires ONCE, latched until the tone has
     *  died away (≤ TONE_CLEAR of the window) — the run counter's `==` fired twice on a wobbling tone. */
    toneRing[toneRingPos] = (uint8_t)lineType;
    toneRingPos = (toneRingPos + 1) % TONE_WINDOW;
    int startHits = 0, stopHits = 0;
    for (int i = 0; i < TONE_WINDOW; i++) { startHits += toneRing[i] == HeaderStart; stopHits += toneRing[i] == HeaderStop; }
    if (startHits <= TONE_CLEAR) startLatched = false;
    if (stopHits <= TONE_CLEAR) stopLatched = false;
    if (!skipHeaderDetection && startHits >= TONE_HITS && !startLatched) {
        startLatched = true;
        if (!includeHeaders) { imageLine = 0; imgPos = 0; lineIncrAcc = 0; }
        phasingLinesLeft = phasingLines;
        phasingSkipData = 0;
        havePhasing = false;
        autoStopped = false;
        if (autoStart && !autoStarted) autoStarted = true;
        chartOver = false; chartLines = 0;
        std::fill(lookRing, lookRing + LOOK_WINDOW, -1);
        if (onDiag) onDiag("start tone on " + std::to_string(startHits) + " of the last " + std::to_string(TONE_WINDOW)
                           + " lines — new chart, phasing window open (" + std::to_string(phasingLines) + " lines)");
        if (onStart) onStart();
    }
    if (!skipHeaderDetection && stopHits >= TONE_HITS && !stopLatched) {
        stopLatched = true;
        chartOver = true;   // ★ the chart is over: whatever chart follows is a NEW one, start tone or not
        if (autoStop) autoStopped = true;
        if (autoStart && autoStarted) autoStarted = false;
        if (onDiag) onDiag("stop tone on " + std::to_string(stopHits) + " of the last " + std::to_string(TONE_WINDOW) + " lines");
        if (onStop) onStop();
    }

    /* ★★★ A NEW CHART WITHOUT ITS START TONE (2026-11-15, Stuart's R36S): a chart ended on its stop tone, the next
     *  transmission's start tone was lost in a fade, its phasing was heard clearly — and the decoder kept counting lines
     *  from the old chart and never sent "image start", so every client drew the new chart onto the end of the old one
     *  and the per-chart auto-align judged the mixture. The start tone is not the only mark of a new chart:
     *   1. AFTER A STOP TONE the chart is over (chartOver). When chart-like lines come back (the corrAvg chart verdict
     *      below) they are a NEW chart: line 0, "image start". If that line looks like phasing the phasing window opens
     *      on it, as the start tone's does; if not (chart content, no phasing) nothing is swallowed — the old phasing
     *      still holds, the station's line timing does not change between charts.
     *   2. PHASING ON ITS OWN (no tone either side): LOOK_HITS of the last LOOK_WINDOW lines are phasing-like
     *      (phasingLook: black across the line but for one short white pulse) with their pulse within ±LOOK_TOL (1 %) of
     *      this line's — while no phasing window is open, and only once the picture is MIN_CHART_LINES old (or a stop tone
     *      came before), so the tail of a phasing the start tone already opened (60 lines sent, the window takes ~43)
     *      cannot start a second chart. The lines that found it are handed to the phasing window as its first
     *      measurements, so it is judged on the same 38 lines a start tone's would be.
     *   ✗ NOT A STANDING-BY GAP. Ending a chart after minutes of "standing by" was tried and dropped: a real chart can
     *      read standing-by for longer than any fade — NOJ's Alaska sea-ice analysis (grey shading, corpus PTCA89) did for
     *      over 3 minutes of its own content under noise, and was split in two. What follows a gap is a new chart only
     *      if it brings a start tone, a phasing, or comes after a stop tone; a fade, however long, never splits one.
     *  ★ MEASURED (2026-11-15) — LOOK_HITS 14 of LOOK_WINDOW 20 within ±LOOK_TOL 1 %:
     *    · Northwood 4610 off air, 70 min live from Stuart's RX888: no start tone heard at 19:24 or 19:36 UTC; the
     *      phasing reached 16 and 19 of 20 (15 when it fired) — two new charts, both phased (36/38, 38/38), where the
     *      old decoder drew all four charts as one 8364-line picture (the station's own addon drew two of them as one).
     *    · 88 real charts re-sent as audio under noise (69 Northwood/DDK off-air decodes from that addon's archive, the
     *      16 NOAA charts of the WEFAX-world corpus, 3 from the R36S gallery), each alone, each after a stop tone and
     *      noise, each before a toneless phasing: NO split inside any chart's content. Toneless phasing found 88 of 88.
     *      Four archive images already held two charts the addon drew as one (phasing band mid-image, checked by eye):
     *      split there, and only there. One trap shaped the thresholds: a NOAA IR satellite picture (corpus
     *      evpz11), where cloud at the line's edge on dark ocean drifts like a pulse, was split at 10 of 16 within 2 %
     *      and contrast 10 — so 14 of 20 within 1 %, and the contrast floor 20 (cloud on ocean reads 7–17, phasing
     *      26–35 off air). A chart after the stop tone with no phasing was found 74 of 88; the 14 missed are pictures
     *      the chart/noise verdict never accepts within 300 lines (noise, or streaked decodes) — drawn on as before.
     *    · No new chart in DDK 7880 (70 min live, noise; 15 min, one start tone), the Northwood chart of 2026-10-05, or
     *      3.5 h of other modes and noise (NAVTEX, FT8, SSTV, MSF/DCF77). */
    int lookAt = -1;
    {
        const bool windowOpen = usePhasing && phasingLinesLeft >= -phasingSkipLines;
        if (!skipHeaderDetection && !windowOpen && lineType == HeaderImage) lookAt = phasingLook();
        lookRing[lookPos] = lookAt;
        lookPos = (lookPos + 1) % LOOK_WINDOW;
        if (lookAt >= 0 && (chartOver || chartLines >= MIN_CHART_LINES)) {
            const int tol = std::max(1, (int)std::lround(samplesPerLine * LOOK_TOL));
            int agree = 0;
            for (int i = 0; i < LOOK_WINDOW; i++)
                if (lookRing[i] >= 0) { int d = std::abs(lookRing[i] - lookAt) % samplesPerLine; if (std::min(d, samplesPerLine - d) <= tol) agree++; }
            if (agree >= LOOK_HITS)
                beginChart("phasing without a start tone (" + std::to_string(agree) + " of the last " + std::to_string(LOOK_WINDOW)
                           + " lines" + (chartOver ? ", after the stop tone" : ", " + std::to_string(chartLines) + " lines into the old chart") + ")",
                           true, lookAt);
        }
    }

    {
        // ★ The phase this line belongs to, reported on change only (see onPhase in the header).
        // ★ A TONE ONLY COUNTS WHEN IT HOLDS — one noisy line reads as start or stop on its own, which is why the
        //   start/stop events above count the window. Here: the tone on at least TONE_SHOW of the last TONE_WINDOW.
        const bool startTone = startHits >= TONE_SHOW && lineType == HeaderStart;
        const bool stopTone = stopHits >= TONE_SHOW && lineType == HeaderStop;
        int phase = startTone ? 1 : stopTone ? 4
                  : (usePhasing && phasingLinesLeft > 0) ? 2 : 3;
        // ★ A start tone or phasing IS a transmission: assume a chart follows (the measure below only sees
        //   image lines, so it would otherwise still be remembering the noise from before the start tone).
        if (phase == 1 || phase == 2) corrAvg = 1.0;
        //   …and a stop tone ENDS it: what follows is noise until it proves to be another chart.
        if (phase == 4) corrAvg = 0.0;
        // ★ "Image" lines that do not look like a chart are noise: 0, standing by.
        /* ★★ TWO THRESHOLDS, NOT ONE (2026-10-07 — the "standing by / receiving" status flickered 80 times through 50
         *  minutes of NMF off K3FEF and 149 times through a noisy JMH chart). A weak chart's smoothed line-to-line
         *  correlation hovers round a single threshold and crosses it every few lines. Now: a chart is noticed at
         *  CORR_ON and only given up below CORR_OFF. */
        if (phase == 3) {
            const bool chart = lastPhase == 3 ? corrAvg >= CORR_OFF : corrAvg >= CORR_ON;
            if (!chart) phase = 0;
        }
        // ★ A chart after the stop tone is a new chart, start tone or not — see the ★★★ note above.
        if (!skipHeaderDetection && phase == 3 && chartOver) {
            beginChart(std::string("a chart after the stop tone, no start tone") + (lookAt >= 0 ? " — it looks like phasing" : ""),
                       lookAt >= 0, lookAt);
            if (usePhasing && phasingLinesLeft > 0) { phase = 2; corrAvg = 1.0; }
        }
        // ★ And a change must hold for two lines before it is reported, so the status cannot flicker.
        if (phase == pendingPhase) pendingCount++; else { pendingPhase = phase; pendingCount = 1; }
        if (pendingCount >= 2 && phase != lastPhase) { lastPhase = phase; if (onPhase) onPhase(phase); }
    }

    if (usePhasing && phasingLinesLeft > 0 && phasingLinesLeft <= phasingLines - phasingSkipLines)
        phasingPos[phasingLinesLeft - 1] = faxPhasingLinePosition(demodData.data());

    if (usePhasing && lineType == HeaderImage && phasingLinesLeft >= -phasingSkipLines) {
        phasingLinesLeft--;
        if (phasingLinesLeft == 0) {
            std::vector<int> slice(phasingPos.begin(), phasingPos.begin() + (phasingLines - phasingSkipLines));
            /* ★★★ A LINE IS A CIRCLE, AND A FEW BAD LINES ARE NOT A BAD PHASING (2026-10-07, Stuart's JMH off the Saitama
             *  Kiwi: start tone caught, phasing ignored — intermittently, chart to chart).
             *  1. The pulse position WRAPS at the line's end: a pulse at 98 % reads 97–99 % on some lines and 0–2 % on
             *     others, and the plain median and 10–90 % spread read that as 1 % → 99 %.
             *  2. On real air some lines fade or catch noise. JMH's 21:00 UTC chart: 27 of 38 phasing lines within ±1 %
             *     of each other, 11 scattered anywhere — and the 10–90 % spread (33 %) counted the 11 and threw away the 27.
             *  So: the ±PHASE_TOL window ON THE CIRCLE that holds the most pulses; phased if at least a third of the lines
             *  agree (see below), at the median of the ones that do. Pure noise never gets there (a scattered recording: 5). */
            const int n = (int)slice.size();
            const int tol = std::max(1, (int)std::lround(samplesPerLine * PHASE_TOL));
            auto cdist = [&](int a, int b) { int d = std::abs(a - b) % samplesPerLine; return std::min(d, samplesPerLine - d); };
            int bestCount = 0, centre = 0;
            for (int i = 0; i < n; i++) {
                int c = 0;
                for (int j = 0; j < n; j++) if (cdist(slice[i], slice[j]) <= tol) c++;
                if (c > bestCount) { bestCount = c; centre = slice[i]; }
            }
            std::vector<int> agree;
            const int half = samplesPerLine / 2;
            for (int p : slice) if (cdist(p, centre) <= tol)
                agree.push_back(((p - centre + half) % samplesPerLine + samplesPerLine) % samplesPerLine - half);
            phasingSkipData = ((centre + (agree.empty() ? 0 : medianOf(agree))) % samplesPerLine + samplesPerLine) % samplesPerLine;
            /* ★ A THIRD, NOT HALF (measured, test-wefax-phasing): the decoder itself misread ~1 phasing line in 7 even on
             *  perfect audio (until 2026-11-15 — see pulseFromBins), so a quarter of the lines faded on top left 17 of 38 —
             *  a real phasing thrown away. Real fading still needs the third. Chance
             *  agreement is tiny: 38 random positions put ~5–6 in any 4 %-wide window (70 % noise managed 9). */
            const bool rejected = bestCount * 3 < n || bestCount < 8;
#ifdef WEFAX_DIAG_POSITIONS
            { std::string s = "positions %:"; for (int i = n - 1; i >= 0; i--) { char b[16]; std::snprintf(b, sizeof b, " %.1f", 100.0 * slice[i] / samplesPerLine); s += b; } if (onDiag) onDiag(s); }
#endif
            if (onDiag) {
                char m[200];
                std::snprintf(m, sizeof m, "phasing: pulse at %.1f%% of the line, %d of %d lines agree within ±%.0f%% — %s",
                              100.0 * phasingSkipData / samplesPerLine, bestCount, n, 100.0 * PHASE_TOL,
                              rejected ? "REJECTED (fewer than a third agree), chart left unphased" : "used");
                onDiag(m);
            }
            if (rejected) phasingSkipData = 0;
        }
    }

    if (includeHeaders || !usePhasing ||
        (lineType == HeaderImage && phasingLinesLeft < -phasingSkipLines)) {
        /* ★★★ TWO LINES, NOT THE WHOLE FAX (audit 2026-10-03). imgData doubled every time the line
         *  count passed it and was never trimmed, so a receiver left on a fax frequency (no stop tone
         *  with auto-stop off) grew without end — and nothing ever reads more than this line and the
         *  one before it, for the blend. Line N lives in slot N & 1. */
        imgPos = (imageLine & 1) * imageWidth;
        bool shouldDecode = !autoStopped && (!autoStart || autoStarted);
        if (shouldDecode) decodeImageLine();

        phasingSkipData %= samplesPerLine;
        if (phasingSkipData != 0 && usePhasing && !havePhasing) {
            skip = phasingSkipData;
            havePhasing = true;
        }
        imageLine++;
        chartLines++;
    }
}

/** ★ A new chart found without its start tone — see the ★★★ note in decodeFaxLine. Same reset as the start tone's;
 *  `curPos` (this line's pulse, ≥ 0 when it looks like phasing) seeds the phasing window with the run that found it. */
void WefaxDecoder::beginChart(const std::string& why, bool openPhasing, int curPos) {
    if (!includeHeaders) { imageLine = 0; imgPos = 0; lineIncrAcc = 0; }
    chartOver = false; chartLines = 0;
    if (openPhasing && usePhasing && curPos >= 0) {
        const int win = phasingLines - 2;   // phasingSkipLines: the window's measured lines
        const int tol = std::max(1, (int)std::lround(samplesPerLine * LOOK_TOL));
        int k = 0;
        // oldest first; the newest slot (this line) is left for the window code, which measures it next
        for (int i = 0; i < LOOK_WINDOW - 1 && k < win - 1; i++) {
            const int r = lookRing[(lookPos + i) % LOOK_WINDOW];
            if (r < 0) continue;
            int d = std::abs(r - curPos) % samplesPerLine;
            if (std::min(d, samplesPerLine - d) <= tol) phasingPos[win - 1 - k++] = r;
        }
        phasingLinesLeft = win - k;
        phasingSkipData = 0;
        havePhasing = false;
    }
    std::fill(lookRing, lookRing + LOOK_WINDOW, -1);
    autoStopped = false;
    if (autoStart) autoStarted = true;
    if (onDiag) onDiag(why + " — new chart" + (usePhasing && phasingLinesLeft > 0 ? ", phasing window open" : ""));
    if (onStart) onStart();
}

// Running contrast stretch: accumulate this line into a lifetime histogram, then rebuild a LUT that
// maps the 2nd…98th percentile to full [0,255]. Cheap (one 256-bin pass per line) and it converges
// after a few lines, so the live image is bright from the top instead of relying on a final pass.
void WefaxDecoder::updateAutoLevel(const uint8_t* line, int w) {
    for (int i = 0; i < w; i++) levelHist[line[i]]++;
    levelCount += (uint64_t)w;
    const uint64_t loCount = levelCount * 2 / 100;    // 2nd percentile from the bottom
    const uint64_t topCount = levelCount * 2 / 100;   // 2% from the top → 98th percentile
    uint64_t acc = 0; int lo = 0, hi = 255;
    for (int v = 0; v < 256; v++)   { acc += levelHist[v]; if (acc >= loCount)  { lo = v; break; } }
    acc = 0;
    for (int v = 255; v >= 0; v--)  { acc += levelHist[v]; if (acc >= topCount) { hi = v; break; } }
    if (hi <= lo) hi = lo + 1;
    const double scale = 255.0 / (double)(hi - lo);
    for (int v = 0; v < 256; v++) {
        double x = (v - lo) * scale;
        levelLut[v] = x < 0 ? 0 : (x > 255 ? 255 : (uint8_t)(x + 0.5));
    }
    levelLutReady = true;
}

void WefaxDecoder::decodeImageLine() {
    // Resample one demod line to imageWidth pixels.
    for (int i = 0; i < imageWidth; i++) {
        int firstSample = samplesPerLine * i / imageWidth;
        int lastSample  = samplesPerLine * (i + 1) / imageWidth - 1;
        int pixel = 0, n = 0;
        for (int s = firstSample; s <= lastSample; s++) { pixel += demodData[s]; n++; }
        if (n > 0) pixel /= n;
        imgData[imgPos + i] = (uint8_t)pixel;
    }

    /* ★★ IS THIS A CHART OR NOISE? A fax line looks like the one before it (coastlines and isobars run
     *  DOWN the page); noise does not. Pearson correlation with the previous line, smoothed — the
     *  "standing by" vs "receiving" in the clients' status (onPhase 0 vs 3). */
    /* ★★ ON 8-PIXEL AVERAGES, NOT RAW PIXELS (measured on Northwood 4610, Stuart's HF+, 2026-10-04): a real
     *  chart's raw lines correlate only ~0.27 (median) — the speckle — which sat on the threshold, so a chart
     *  joined mid-way read "standing by". Averaged over 8 px the same lines read ~0.68 (p10 0.44); noise
     *  stays near 0 either way. */
    if (imageLine > 0) {
        const int other = ((imageLine + 1) & 1) * imageWidth;
        const int K = 8, blocks = imageWidth / K;
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        for (int j = 0; j < blocks; j++) {
            double a = 0, b = 0;
            for (int k = 0; k < K; k++) { a += imgData[imgPos + j * K + k]; b += imgData[other + j * K + k]; }
            sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
        }
        const double n = blocks;
        const double va = saa - sa * sa / n, vb = sbb - sb * sb / n, cov = sab - sa * sb / n;
        /* ★★ A FLAT LINE IS NO EVIDENCE EITHER WAY (Stuart, 2026-10-05: DDK sends its phasing, then a pure
         *  continuous tone for a minute or two, and we read "standing by" until the chart started). A steady tone
         *  demodulates to a line of one grey; its variance is ~0, so it scored 0 — noise — and the status dropped.
         *  Noise is never flat (8-px block sums of noise vary by ~200), so a flat pair just HOLDS the verdict:
         *  after a start tone/phasing it stays "receiving", after noise it stays "standing by".
         *  ★★ THRESHOLD MEASURED ON DDK 7880 OFF AIR (Stuart's RX888, 2026-10-05), not on a clean synthetic tone:
         *     the real tone carries receiver noise — its block sums vary by 19–24 (p10–p90); chart lines 59–105.
         *     40 sits between them. (16, set from a noiseless synthetic tone, never fired on the real one.) */
        const double flatVar = 40.0 * 40.0 * n;
        if (va > flatVar || vb > flatVar) {
            const double c = (va > 1e-9 && vb > 1e-9) ? cov / std::sqrt(va * vb) : 0.0;
            corrAvg = 0.8 * corrAvg + 0.2 * c;
        }
    }

    // Line blending for sample-rate adaptation.
    bool emit = false;
    if (lineIncrAcc >= 1.0) {
        lineIncrAcc -= 1.0;
        if (imageLine != 0 && lineIncrAcc != 0) {
            double lineNextBlend = lineIncrAcc / lineBlend;
            double linePrevBlend = 1.0 - lineNextBlend;
            int prevLineStart = ((imageLine + 1) & 1) * imageWidth;   // the other slot — see decodeFaxLine
            for (int i = 0; i < imageWidth; i++) {
                double pixel = (double)imgData[imgPos + i] * lineNextBlend +
                               (double)imgData[prevLineStart + i] * linePrevBlend;
                if (pixel > 255) pixel = 255;
                outImage[i] = (uint8_t)pixel;
            }
            lineBlend = lineIncrFrac;
        } else {
            std::memcpy(outImage.data(), imgData.data() + imgPos, imageWidth);
        }
        emit = true;
    } else {
        lineBlend += lineIncrFrac;
    }
    lineIncrAcc += lineIncrFrac;

    if (emit) {
        // Live contrast stretch so the emitted line is bright for EVERY client (web/phone/watch),
        // not just the phone's finished-image post-process. Accumulate raw values first, then map.
        updateAutoLevel(outImage.data(), imageWidth);
        for (int i = 0; i < imageWidth; i++) outImage[i] = levelLut[outImage[i]];
        if (onLine) onLine((uint32_t)imageLine, (uint32_t)imageWidth, outImage.data());
    }
}

} // namespace vibe
