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
WefaxDecoder::WefaxDecoder(int sampleRate, const Config& cfg)
    : lpm(cfg.lpm), imageWidth(cfg.imageWidth), bandwidth(cfg.bandwidth),
      carrier(cfg.carrier), deviation(cfg.deviation),
      usePhasing(cfg.usePhasing), autoStop(cfg.autoStop), autoStart(cfg.autoStart),
      includeHeaders(cfg.includeHeaders),
      samplesPerSec((double)sampleRate),
      firI(cfg.bandwidth), firQ(cfg.bandwidth) {

    skipHeaderDetection = !usePhasing && !autoStop && !autoStart;
    samplesPerLine = (int)(samplesPerSec * 60.0 / (double)lpm);

    samples.assign(samplesPerLine, 0);
    demodData.assign(samplesPerLine, 0);
    phasingPos.assign(phasingLines, 0);

    imgData.assign((size_t)imageWidth * imgHeight, 0);
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
        if (imageLine >= imgHeight) {
            imgHeight *= 2;
            imgData.resize((size_t)imageWidth * imgHeight, 0);
        }
        bool shouldDecode = !autoStopped && (!autoStart || autoStarted);
        if (shouldDecode) decodeImageLine();

        phasingSkipData %= samplesPerLine;
        if (phasingSkipData != 0 && usePhasing && !havePhasing) {
            skip = phasingSkipData;
            havePhasing = true;
        }
        imgPos += imageWidth;
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

    // Line blending for sample-rate adaptation.
    bool emit = false;
    if (lineIncrAcc >= 1.0) {
        lineIncrAcc -= 1.0;
        if (imageLine != 0 && lineIncrAcc != 0) {
            double lineNextBlend = lineIncrAcc / lineBlend;
            double linePrevBlend = 1.0 - lineNextBlend;
            int prevLineStart = imgPos - imageWidth;
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
