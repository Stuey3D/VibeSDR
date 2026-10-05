// VibeSDR V4 — WEFAX decoder (C++ port of ka9q audio_extensions/wefax/decoder.go).
#include "wefax_decoder.h"
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
static int percentileOf(std::vector<int> v, int pct) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    int idx = (int)v.size() * pct / 100;
    if (idx >= (int)v.size()) idx = (int)v.size() - 1;
    return v[idx];
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
    const double threshold = 5.0;
    double startDet = fourierTransformSub(buf, len, startIOC576Frequency) / (double)len;
    double stopDet  = fourierTransformSub(buf, len, stopFrequency) / (double)len;
    if (startDet > threshold) return HeaderStart;
    if (stopDet  > threshold) return HeaderStop;
    return HeaderImage;
}

int WefaxDecoder::faxPhasingLinePosition(const uint8_t* image) {
    int n = (int)((double)samplesPerLine * 0.07);
    int minTotal = -1, minPos = 0;
    int pixelResolution = 4;
    int sampsIncr = (samplesPerLine / imageWidth) * pixelResolution;
    if (sampsIncr < 1) sampsIncr = 1;

    for (int i = 0; i < samplesPerLine; i += sampsIncr) {
        int total = 0;
        for (int j = 0; j < n; j += pixelResolution) {
            int wedge = n / 2 - std::abs(j - n / 2);
            int idx = (i + j) % samplesPerLine;
            total += wedge * (255 - (int)image[idx]);
        }
        if (total < minTotal || minTotal == -1) { minTotal = total; minPos = i; }
    }
    return (minPos + n / 2) % samplesPerLine;
}

void WefaxDecoder::decodeFaxLine() {
    const int phasingSkipLines = 2;
    demodulateData();

    HeaderType lineType;
    if (skipHeaderDetection) {
        lineType = HeaderImage;
    } else {
        int bufferLen = std::min(samplesPerLine, 3000);
        lineType = detectLineType(demodData.data(), bufferLen);
    }

    if (lineType == lastType && lineType != HeaderImage) typeCount++;
    else { typeCount--; if (typeCount < 0) typeCount = 0; }
    lastType = lineType;

    if (lineType != HeaderImage) {
        int leewayLines = 4;
        int threshold = startStopLength * lpm / 60 - leewayLines;
        if (typeCount == threshold) {
            if (lineType == HeaderStart) {
                if (!includeHeaders) { imageLine = 0; imgPos = 0; lineIncrAcc = 0; }
                phasingLinesLeft = phasingLines;
                phasingSkipData = 0;
                havePhasing = false;
                autoStopped = false;
                if (autoStart && !autoStarted) autoStarted = true;
                if (onStart) onStart();
            } else if (lineType == HeaderStop) {
                if (autoStop) autoStopped = true;
                if (autoStart && autoStarted) autoStarted = false;
                if (onStop) onStop();
            }
        }
    }

    {
        // ★ The phase this line belongs to, reported on change only (see onPhase in the header).
        // ★ A TONE ONLY COUNTS WHEN IT HOLDS — one noisy line reads as start or stop on its own, which is why
        //   the start/stop events above wait for a run (typeCount). Three in a row here.
        const bool tone = lineType != HeaderImage && lineType == lastType && typeCount >= 3;
        int phase = tone ? (lineType == HeaderStart ? 1 : 4)
                  : (usePhasing && phasingLinesLeft > 0) ? 2 : 3;
        // ★ A start tone or phasing IS a transmission: assume a chart follows (the measure below only sees
        //   image lines, so it would otherwise still be remembering the noise from before the start tone).
        if (phase == 1 || phase == 2) corrAvg = 1.0;
        //   …and a stop tone ENDS it: what follows is noise until it proves to be another chart.
        if (phase == 4) corrAvg = 0.0;
        // ★ "Image" lines that do not look like a chart are noise: 0, standing by.
        if (phase == 3 && corrAvg < 0.25) phase = 0;
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
            phasingSkipData = medianOf(slice);
            int tenPct = percentileOf(slice, 10);
            int ninetyPct = percentileOf(slice, 90);
            if ((ninetyPct - tenPct) > samplesPerLine / 6) phasingSkipData = 0;
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
    }
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
         *  after a start tone/phasing it stays "receiving", after noise it stays "standing by". Threshold: block
         *  sums within ~16 grey (2 per pixel) of their mean. */
        const double flatVar = 16.0 * 16.0 * n;
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
