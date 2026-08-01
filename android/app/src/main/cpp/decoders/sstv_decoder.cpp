// VibeSDR V4 — SSTV decoder (C++ port of ka9q audio_extensions/sstv → slowrx).
#include "sstv_decoder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vibe {

// ── Mode specs (slowrx timings) ──────────────────────────────────────────────
static const SstvMode kModes[] = {
    {"Unknown",0,0,0,0,0,0,0,0,SSTV_BW,true},
    {"Martin M1",4.862e-3,0.572e-3,0.572e-3,0.4576e-3,446.446e-3,320,256,1,SSTV_GBR,false},
    {"Martin M2",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,226.7986e-3,320,256,1,SSTV_GBR,false},
    {"Martin M3",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,446.446e-3,320,128,2,SSTV_GBR,false},
    {"Martin M4",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,226.7986e-3,320,128,2,SSTV_GBR,false},
    {"Scottie S1",9e-3,1.5e-3,1.5e-3,0.4320e-3,428.38e-3,320,256,1,SSTV_GBR,false},
    {"Scottie S2",9e-3,1.5e-3,1.5e-3,0.2752e-3,277.692e-3,320,256,1,SSTV_GBR,false},
    {"Scottie DX",9e-3,1.5e-3,1.5e-3,1.08053e-3,1050.3e-3,320,256,1,SSTV_GBR,false},
    {"Robot 72",9e-3,3e-3,4.7e-3,0.2875e-3,300e-3,320,240,1,SSTV_YUV,false},
    {"Robot 36",9e-3,3e-3,6e-3,0.1375e-3,150e-3,320,240,1,SSTV_YUV,false},
    {"Robot 24",9e-3,3e-3,6e-3,0.1375e-3,150e-3,320,240,1,SSTV_YUV,false},
    {"Robot 24 B/W",7e-3,0,0,0.291e-3,100e-3,320,240,1,SSTV_BW,false},
    {"Robot 12 B/W",7e-3,0,0,0.291e-3,100e-3,320,120,2,SSTV_BW,false},
    {"Robot 8 B/W",7e-3,0,0,0.1871875e-3,66.9e-3,320,120,2,SSTV_BW,false},
    {"PD-50",20e-3,2.08e-3,0,0.286e-3,388.16e-3,320,256,1,SSTV_YUV,false},
    {"PD-90",20e-3,2.08e-3,0,0.532e-3,703.04e-3,320,256,1,SSTV_YUV,false},
    {"PD-120",20e-3,2.08e-3,0,0.19e-3,508.48e-3,640,496,1,SSTV_YUV,false},
    {"PD-160",20e-3,2.08e-3,0,0.382e-3,804.416e-3,512,400,1,SSTV_YUV,false},
    {"PD-180",20e-3,2.08e-3,0,0.286e-3,754.24e-3,640,496,1,SSTV_YUV,false},
    {"PD-240",20e-3,2.08e-3,0,0.382e-3,1000e-3,640,496,1,SSTV_YUV,false},
    {"PD-290",20e-3,2.08e-3,0,0.286e-3,937.28e-3,800,616,1,SSTV_YUV,false},
    {"Pasokon P3",5.208e-3,1.042e-3,1.042e-3,0.2083e-3,409.375e-3,640,496,1,SSTV_RGB,false},
    {"Pasokon P5",7.813e-3,1.563e-3,1.563e-3,0.3125e-3,614.065e-3,640,496,1,SSTV_RGB,false},
    {"Pasokon P7",10.417e-3,2.083e-3,2.083e-3,0.4167e-3,818.747e-3,640,496,1,SSTV_RGB,false},
    {"Wraase SC-2 120",5.5225e-3,0.5e-3,0,0.489039081e-3,475.530018e-3,320,256,1,SSTV_RGB,false},
    {"Wraase SC-2 180",5.5225e-3,0.5e-3,0,0.734532e-3,711.0225e-3,320,256,1,SSTV_RGB,false},
};
enum { M_M1=1,M_M2=2,M_M3=3,M_M4=4,M_S1=5,M_S2=6,M_SDX=7,M_R72=8,M_R36=9,M_R24=10,
       M_R24BW=11,M_R12BW=12,M_R8BW=13,M_PD50=14,M_PD90=15,M_PD120=16,M_PD160=17,
       M_PD180=18,M_PD240=19,M_PD290=20,M_P3=21,M_P5=22,M_P7=23,M_W2120=24,M_W2180=25 };

static const uint8_t kVisMap[128] = {
    0,0,M_R8BW,0,M_R24,0,M_R12BW,0, M_R36,0,M_R24BW,0,M_R72,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    M_M4,0,0,0,M_M3,0,0,0, M_M2,0,0,0,M_M1,0,0,0,
    0,0,0,0,0,0,0,M_W2180, M_S2,0,0,0,M_S1,0,0,M_W2120,
    0,0,0,0,0,0,0,0, 0,0,0,0,M_SDX,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,M_PD50,M_PD290,M_PD120,
    M_PD180,M_PD240,M_PD160,M_PD90,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,M_P3,M_P5,M_P7,0,0,0,0, 0,0,0,0,0,0,0,0,
};
const SstvMode* sstvModeByIndex(uint8_t i) { return i < (sizeof(kModes)/sizeof(kModes[0])) ? &kModes[i] : nullptr; }
uint8_t sstvModeByVis(uint8_t v) { return v < 128 ? kVisMap[v] : 0; }

static inline uint8_t clip(double v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }
static double deg2rad(double d) { return d * M_PI / 180.0; }
static const int MinSlant_ = 30, MaxSlant_ = 150;   // slant search range (degrees)

// ── FFT ──────────────────────────────────────────────────────────────────────
SstvFFT::SstvFFT(int n_) : n(n_) { cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr); out.resize(n / 2 + 1); }
SstvFFT::~SstvFFT() { kiss_fftr_free(cfg); }
void SstvFFT::run(const float* in) { kiss_fftr(cfg, in, out.data()); }
double SstvFFT::power(int b) const { if (b < 0 || b > n / 2) return 0; return (double)out[b].r * out[b].r + (double)out[b].i * out[b].i; }
double SstvFFT::re(int b) const { return (b < 0 || b > n / 2) ? 0 : out[b].r; }
double SstvFFT::im(int b) const { return (b < 0 || b > n / 2) ? 0 : out[b].i; }

// ── Circular buffer ──────────────────────────────────────────────────────────
SstvBuffer::SstvBuffer(int requested) {
    int minSize = 8 * 1024 * 1024;
    size = requested > minSize ? requested : minSize;
    buf.assign(size, 0);
}
int SstvBuffer::availableLocked() {
    return writePos >= wptr ? writePos - wptr : (size - wptr) + writePos;
}
void SstvBuffer::write(const int16_t* s, int n) {
    std::lock_guard<std::mutex> lk(mu);
    if (wptr == 0) {
        for (int i = 0; i < n && fillPos < 1024; i++) buf[fillPos++] = s[i];
        if (fillPos >= 1024) { wptr = 512; writePos = fillPos; }
    } else {
        for (int i = 0; i < n; i++) { buf[writePos] = s[i]; writePos = (writePos + 1) % size; }
    }
}
bool SstvBuffer::getWindow(int offset, int length, int16_t* out) {
    std::lock_guard<std::mutex> lk(mu);
    for (int i = 0; i < length; i++) {
        int pos = (wptr + offset + i) % size; if (pos < 0) pos += size;
        out[i] = buf[pos];
    }
    return true;
}
void SstvBuffer::advanceWindow(int n) { std::lock_guard<std::mutex> lk(mu); wptr = (wptr + n) % size; }
int  SstvBuffer::windowPtr() { std::lock_guard<std::mutex> lk(mu); return wptr; }
int  SstvBuffer::available() { std::lock_guard<std::mutex> lk(mu); return availableLocked(); }
void SstvBuffer::reset() { std::lock_guard<std::mutex> lk(mu); std::fill(buf.begin(), buf.end(), 0); wptr = writePos = fillPos = 0; }

// ── VIS detector ─────────────────────────────────────────────────────────────
SstvVIS::SstvVIS(double sr) : sampleRate(sr), fft(2048) {
    int samps20 = (int)(sr * 20e-3);
    hann.resize(samps20);
    for (int i = 0; i < samps20; i++) hann[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (samps20 - 1)));
    headerBuf.assign(45, 0); toneBuf.assign(45, 0);
    fin.assign(2048, 0);
}
bool SstvVIS::checkRange(int idx, double lo, double hi) {
    if (idx < 0 || idx >= (int)toneBuf.size()) return false;
    double f = toneBuf[idx]; return f > lo && f < hi;
}
bool SstvVIS::process(SstvBuffer& pcm, uint8_t& modeOut, int& shiftOut) {
    int samps10 = (int)(sampleRate * 10e-3);
    int samps20 = (int)hann.size();
    iter++;
    if (pcm.available() < samps20) return false;

    std::vector<int16_t> win(samps20);
    pcm.getWindow(-samps10, samps20, win.data());
    std::fill(fin.begin(), fin.end(), 0.0f);
    for (int i = 0; i < samps20 && i < (int)fin.size(); i++) fin[i] = (float)(win[i] / 32768.0 * hann[i]);
    fft.run(fin.data());

    int minBin = getBin(500.0), maxBinLimit = getBin(3300.0), maxBin = 0;
    std::vector<double> powers(fftSize / 2);
    for (int i = 0; i < fftSize / 2; i++) {
        powers[i] = fft.power(i);
        if (i >= minBin && i < maxBinLimit && (maxBin == 0 || powers[i] > powers[maxBin])) maxBin = i;
    }
    double peak;
    if (maxBin > minBin && maxBin < maxBinLimit - 1 && powers[maxBin] > 0 && powers[maxBin-1] > 0 && powers[maxBin+1] > 0) {
        double num = powers[maxBin+1] / powers[maxBin-1];
        double den = (powers[maxBin]*powers[maxBin]) / (powers[maxBin+1]*powers[maxBin-1]);
        if (num > 0 && den > 0 && std::fabs(std::log(den)) > 1e-9)
            peak = (maxBin + std::log(num) / (2.0*std::log(den))) / (double)fftSize * sampleRate;
        else peak = (double)maxBin / fftSize * sampleRate;
    } else {
        int prev = (headerPtr - 1 + (int)headerBuf.size()) % (int)headerBuf.size();
        peak = headerBuf[prev];
    }
    headerBuf[headerPtr] = peak;
    headerPtr = (headerPtr + 1) % (int)headerBuf.size();
    if (onTone && iter % 50 == 0) onTone(peak);
    for (int i = 0; i < (int)toneBuf.size(); i++) toneBuf[i] = headerBuf[(headerPtr + i) % headerBuf.size()];

    if (iter < 45) { pcm.advanceWindow(samps10); return false; }

    double tol = sampleRate > 40000 ? 25.0 : 50.0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 20; j++) {
            double ref = toneBuf[0 + j];
            if (!checkRange(1*3+i, ref-tol, ref+tol) || !checkRange(2*3+i, ref-tol, ref+tol) ||
                !checkRange(3*3+i, ref-tol, ref+tol) || !checkRange(4*3+i, ref-tol, ref+tol) ||
                !checkRange(5*3+i, ref-700-tol, ref-700+tol) || !checkRange(14*3+i, ref-700-tol, ref-700+tol))
                continue;
            uint8_t bits[8]; bool valid = true;
            for (int k = 0; k < 8; k++) {
                int ti = 6*3 + i + 3*k;
                if (ti >= (int)toneBuf.size()) { valid = false; break; }
                double f = toneBuf[ti];
                double b0 = ref - 600, b1 = ref - 800, bt = tol;
                if (f > b0-bt && f < b0+bt) bits[k] = 0;
                else if (f > b1-bt && f < b1+bt) bits[k] = 1;
                else { valid = false; break; }
            }
            if (!valid) continue;
            uint8_t vis = bits[0]|(bits[1]<<1)|(bits[2]<<2)|(bits[3]<<3)|(bits[4]<<4)|(bits[5]<<5)|(bits[6]<<6);
            uint8_t parityBit = bits[7];
            uint8_t parity = bits[0]^bits[1]^bits[2]^bits[3]^bits[4]^bits[5]^bits[6];
            if (kVisMap[vis] == M_R12BW) parity = 1 - parity;
            if (parity != parityBit) continue;
            uint8_t mode = sstvModeByVis(vis);
            if (mode == 0) continue;
            const SstvMode* ms = sstvModeByIndex(mode);
            if (!ms || ms->unsupported) continue;
            shiftOut = (int)(ref - 1900);
            modeOut = mode;
            pcm.advanceWindow((int)(20e-3 * sampleRate));   // skip stop bit → video start
            return true;
        }
    }
    pcm.advanceWindow(samps10);
    return false;
}

// ── Video demodulator ────────────────────────────────────────────────────────
SstvVideo::SstvVideo(const SstvMode* mode, double sr, int shift, bool ad)
    : m(mode), sampleRate(sr), headerShift(shift), adaptive(ad), fft(1024) {
    double sf = sr / 44100.0;
    int base[7] = {48,64,96,128,256,512,1024};
    hannLens.resize(7);
    for (int i = 0; i < 7; i++) { hannLens[i] = (int)std::lround(base[i]*sf); if (hannLens[i] < 8) hannLens[i] = 8; }
    hannWins.resize(7);
    for (int j = 0; j < 7; j++) {
        int L = hannLens[j]; hannWins[j].resize(L);
        for (int i = 0; i < L; i++) hannWins[j][i] = 0.5 * (1.0 - std::cos(2.0*M_PI*i/(L-1)));
    }
    int maxLen;
    if (m->color == SSTV_YUV && m->imgWidth >= 512)
        maxLen = (int)(m->lineTime*m->numLines/2*sr*1.3) + 15000;
    else
        maxLen = (int)(m->lineTime*m->numLines*sr*1.3) + 15000;
    hasSync.assign(maxLen/13 + 1, 0);
    storedLum.assign(maxLen, 0);
    fin.assign(1024, 0);
}

std::vector<SstvPixel> SstvVideo::pixelGrid(double rate, int skip) {
    double chanStart[4] = {0,0,0,0}, chanLen[4] = {0,0,0,0};
    int numChans = 3;
    std::string nm = m->name;
    bool robot = (nm == "Robot 36" || nm == "Robot 24");
    bool scottie = (nm == "Scottie S1" || nm == "Scottie S2" || nm == "Scottie DX");
    bool pd = (m->color == SSTV_YUV && m->imgWidth >= 512);

    if (robot) {
        chanLen[0] = m->pixelTime*m->imgWidth*2; chanLen[1] = m->pixelTime*m->imgWidth; chanLen[2] = chanLen[1];
        chanStart[0] = m->syncTime + m->porchTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime; chanStart[2] = chanStart[1];
        numChans = 2;
    } else if (scottie) {
        chanLen[0]=chanLen[1]=chanLen[2]=m->pixelTime*m->imgWidth;
        chanStart[0] = m->septrTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime;
        chanStart[2] = chanStart[1] + chanLen[1] + m->syncTime + m->porchTime;
    } else if (pd) {
        for (int c=0;c<4;c++) chanLen[c]=m->pixelTime*m->imgWidth;
        chanStart[0] = m->syncTime + m->porchTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime;
        chanStart[2] = chanStart[1] + chanLen[1] + m->septrTime;
        chanStart[3] = chanStart[2] + chanLen[2] + m->septrTime;
        numChans = 4;
    } else if (m->color == SSTV_BW) {
        chanLen[0] = m->pixelTime*m->imgWidth; chanStart[0] = m->syncTime + m->porchTime; numChans = 1;
    } else {
        chanLen[0]=chanLen[1]=chanLen[2]=m->pixelTime*m->imgWidth;
        chanStart[0] = m->syncTime + m->porchTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime;
        chanStart[2] = chanStart[1] + chanLen[1] + m->septrTime;
    }

    std::vector<SstvPixel> px;
    if (numChans == 4) {
        for (int y = 0; y < m->numLines; y += 2)
            for (int c = 0; c < 4; c++)
                for (int x = 0; x < m->imgWidth; x++) {
                    double t = (double)y/2*m->lineTime + chanStart[c] + m->pixelTime*(x+0.5);
                    int sn = (int)std::lround(rate*t) + skip;
                    if (c == 0) px.push_back({sn,x,y,0});
                    else if (c == 1 || c == 2) { px.push_back({sn,x,y,(uint8_t)c}); px.push_back({sn,x,y+1,(uint8_t)c}); }
                    else px.push_back({sn,x,y+1,0});
                }
    } else {
        for (int y = 0; y < m->numLines; y++)
            for (int c = 0; c < numChans; c++)
                for (int x = 0; x < m->imgWidth; x++) {
                    uint8_t ch;
                    if (robot) ch = (c == 1) ? (y%2==0 ? 1 : 2) : 0;
                    else ch = (uint8_t)c;
                    double t = (double)y*m->lineTime + chanStart[c] + ((double)x-0.5)/m->imgWidth*chanLen[ch];
                    int sn = (int)std::lround(rate*t) + skip;
                    px.push_back({sn,x,y,ch});
                }
    }
    std::vector<SstvPixel> out;
    out.reserve(px.size());
    for (auto& p : px) if (p.time >= 0) out.push_back(p);
    return out;
}

void SstvVideo::detectSync(SstvBuffer& pcm, int targetBin, int idx) {
    int16_t s[64]; pcm.getWindow(-32, 64, s);
    for (int i = 0; i < 64; i++) fin[i] = (i < (int)hannWins[1].size()) ? (float)(s[i]/32768.0*hannWins[1][i]) : 0;
    for (int i = 64; i < (int)fin.size(); i++) fin[i] = 0;
    fft.run(fin.data());
    double pRaw = 0, pSync = 0;
    int minB = getBin(1500.0+headerShift), maxB = getBin(2300.0+headerShift);
    for (int i = minB; i <= maxB; i++) pRaw += fft.power(i);
    for (int i = targetBin-1; i <= targetBin+1; i++) { double w = 1.0 - 0.5*std::fabs((double)(targetBin-i)); pSync += fft.power(i)*w; }
    pRaw /= (double)(maxB - minB); pSync /= 2.0;
    if (idx < (int)hasSync.size()) hasSync[idx] = (pSync > 2*pRaw) ? 1 : 0;
}

double SstvVideo::estimateSNR(SstvBuffer& pcm) {
    int16_t s[1024]; pcm.getWindow(-512, 1024, s);
    for (int i = 0; i < 1024; i++) fin[i] = (i < (int)hannWins[6].size()) ? (float)(s[i]/32768.0*hannWins[6][i]) : 0;
    fft.run(fin.data());
    double pVN = 0; int minB = getBin(1500.0+headerShift), maxB = getBin(2300.0+headerShift);
    for (int i = minB; i <= maxB; i++) pVN += fft.power(i);
    double pNO = 0;
    for (int i = getBin(400.0+headerShift); i <= getBin(800.0+headerShift); i++) pNO += fft.power(i);
    for (int i = getBin(2700.0+headerShift); i <= getBin(3400.0+headerShift); i++) pNO += fft.power(i);
    int videoBins = maxB - minB + 1;
    int noiseBins = (getBin(800.0)-getBin(400.0)+1) + (getBin(3400.0)-getBin(2700.0)+1);
    int rxBins = getBin(3400.0) - getBin(400.0);
    double pNoise = pNO * (double)rxBins / noiseBins;
    double pSignal = pVN - pNO * (double)videoBins / noiseBins;
    if (pNoise <= 0 || pSignal/pNoise < 0.01) return -20.0;
    return 10.0 * std::log10(pSignal/pNoise);
}

double SstvVideo::demodFreq(SstvBuffer& pcm, double snr) {
    int winIdx = 0;
    if (adaptive) {
        if (snr >= 20) winIdx = 0; else if (snr >= 10) winIdx = 1; else if (snr >= 9) winIdx = 2;
        else if (snr >= 3) winIdx = 3; else if (snr >= -5) winIdx = 4; else if (snr >= -10) winIdx = 5; else winIdx = 6;
        if (std::string(m->name) == "Scottie DX" && winIdx < 6) winIdx++;
    }
    int L = hannLens[winIdx];
    std::vector<int16_t> s(L); pcm.getWindow(-L/2, L, s.data());
    std::fill(fin.begin(), fin.end(), 0.0f);
    for (int i = 0; i < L && i < (int)fin.size(); i++) fin[i] = (float)(s[i]/32768.0*hannWins[winIdx][i]);
    fft.run(fin.data());
    int minB = getBin(1500.0+headerShift) - 1, maxBL = getBin(2300.0+headerShift) + 1;
    int maxBin = 0; double maxP = 0;
    std::vector<double> powers(fftSize, 0);
    for (int i = minB; i <= maxBL && i < fftSize; i++) { powers[i] = fft.power(i); if (powers[i] > maxP) { maxP = powers[i]; maxBin = i; } }
    double freq;
    if (maxBin > minB && maxBin < maxBL && powers[maxBin] > 0 && powers[maxBin-1] > 0 && powers[maxBin+1] > 0) {
        double num = powers[maxBin+1]/powers[maxBin-1];
        double den = (powers[maxBin]*powers[maxBin])/(powers[maxBin+1]*powers[maxBin-1]);
        if (num > 0 && den > 0) freq = (maxBin + std::log(num)/(2.0*std::log(den)))/(double)fftSize*sampleRate;
        else freq = (double)maxBin/fftSize*sampleRate;
    } else {
        freq = (maxBin > getBin(1900.0+headerShift)) ? 2300.0+headerShift : 1500.0+headerShift;
    }
    return freq;
}

void SstvVideo::demodulate(SstvBuffer& pcm, double rate, int skip,
                           const std::function<void(int, const uint8_t*)>& lineSender,
                           const std::atomic<bool>& abort) {
    auto grid = pixelGrid(rate, skip);
    int length;
    if (m->color == SSTV_YUV && m->imgWidth >= 512) length = (int)(m->lineTime*m->numLines/2*sampleRate);
    else length = (int)(m->lineTime*m->numLines*sampleRate);
    int syncTargetBin = getBin(1200.0 + headerShift);

    int numChans = 3;
    std::string nm = m->name;
    if (nm == "Robot 36" || nm == "Robot 24") numChans = 2;
    else if (m->color == SSTV_YUV && m->imgWidth >= 512) numChans = 4;
    else if (m->color == SSTV_BW) numChans = 1;

    // image[x][y][3]
    std::vector<uint8_t> img((size_t)m->imgWidth * m->numLines * 3, 0);
    auto IMG = [&](int x, int y, int c) -> uint8_t& { return img[((size_t)y*m->imgWidth + x)*3 + c]; };

    int pixelIdx = 0, nextSync = 0, nextSNR = 0, syncSampleNum = 0;
    double snr = 0, freq = 0;

    for (int sampleNum = 0; sampleNum < length; sampleNum++) {
        if (abort.load()) return;
        if (pcm.available() < 1024) {
            for (int i = 0; i < 500 && pcm.available() < 1024; i++) {
                if (abort.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pcm.available() < 1024) break;
        }
        if (sampleNum == nextSync) { detectSync(pcm, syncTargetBin, syncSampleNum); nextSync += 13; syncSampleNum++; }
        if (sampleNum == nextSNR) { snr = estimateSNR(pcm); nextSNR += 256; }
        if (sampleNum % 6 == 0) freq = demodFreq(pcm, snr);

        uint8_t lum = clip((freq - (1500.0 + headerShift)) / 3.1372549);
        if (sampleNum < (int)storedLum.size()) { storedLum[sampleNum] = lum; storedLumWritten = sampleNum + 1; }

        while (pixelIdx < (int)grid.size() && grid[pixelIdx].time == sampleNum) {
            SstvPixel p = grid[pixelIdx];
            if (p.x < m->imgWidth && p.y < m->numLines) IMG(p.x, p.y, p.channel) = lum;
            if (p.channel > 0 && (nm == "Robot 36" || nm == "Robot 24") && p.y+1 < m->numLines)
                IMG(p.x, p.y+1, p.channel) = lum;

            if (lineSender && p.x == m->imgWidth-1 && (int)p.channel >= numChans-1) {
                std::vector<uint8_t> line((size_t)m->imgWidth*3);
                for (int x = 0; x < m->imgWidth; x++) {
                    int o = x*3;
                    uint8_t c0=IMG(x,p.y,0), c1=IMG(x,p.y,1), c2=IMG(x,p.y,2);
                    switch (m->color) {
                        case SSTV_RGB: line[o]=c0; line[o+1]=c1; line[o+2]=c2; break;
                        case SSTV_GBR: line[o]=c2; line[o+1]=c0; line[o+2]=c1; break;
                        case SSTV_YUV:
                            line[o]   = clip((100*(double)c0 + 140*(double)c1 - 17850)/100.0);
                            line[o+1] = clip((100*(double)c0 - 71*(double)c1 - 33*(double)c2 + 13260)/100.0);
                            line[o+2] = clip((100*(double)c0 + 178*(double)c2 - 22695)/100.0);
                            break;
                        case SSTV_BW: line[o]=line[o+1]=line[o+2]=c0; break;
                    }
                }
                lineSender(p.y, line.data());
                if (p.y + 1 > linesReceived) linesReceived = p.y + 1;
            }
            pixelIdx++;
        }
        pcm.advanceWindow(1);
    }
}

std::vector<uint8_t> SstvVideo::toRGB(const std::vector<uint8_t>& img) {
    std::vector<uint8_t> rgb((size_t)m->imgWidth*m->numLines*3);
    auto IMG = [&](int x,int y,int c){ return img[((size_t)y*m->imgWidth+x)*3+c]; };
    for (int y = 0; y < m->numLines; y++)
        for (int x = 0; x < m->imgWidth; x++) {
            int o = (y*m->imgWidth + x)*3;
            uint8_t c0=IMG(x,y,0),c1=IMG(x,y,1),c2=IMG(x,y,2);
            switch (m->color) {
                case SSTV_RGB: rgb[o]=c0; rgb[o+1]=c1; rgb[o+2]=c2; break;
                case SSTV_GBR: rgb[o]=c2; rgb[o+1]=c0; rgb[o+2]=c1; break;
                case SSTV_YUV:
                    rgb[o]   = clip((100*(double)c0 + 140*(double)c1 - 17850)/100.0);
                    rgb[o+1] = clip((100*(double)c0 - 71*(double)c1 - 33*(double)c2 + 13260)/100.0);
                    rgb[o+2] = clip((100*(double)c0 + 178*(double)c2 - 22695)/100.0);
                    break;
                case SSTV_BW: rgb[o]=rgb[o+1]=rgb[o+2]=c0; break;
            }
        }
    return rgb;
}

std::vector<uint8_t> SstvVideo::redrawFromLuminance(double rate, int skip, bool* okOut) {
    auto grid = pixelGrid(rate, skip);
    // ★★★ REPORT WHETHER THIS REDRAW IS EVEN POSSIBLE. The corrected grid asks for sample times
    // that may lie past the end of what was actually captured — the decode loop breaks early when
    // the audio runs short, and the tail of storedLum is then zeros that were never samples.
    // The old code silently substituted storedLum.back() for those, which is why a slant-corrected
    // picture could line up perfectly at the TOP and drift apart at the BOTTOM (Stuart,
    // 2026-07-31): the top mapped to real samples, the bottom to a single repeated value.
    // ★★★ REFUSE A DRIFTING OVERRUN, TOLERATE A BOUNDED ONE. The first version of this guard
    // rejected the redraw if ANY pixel fell past the captured samples — and that threw away a
    // perfectly corrected picture because the last hundred pixels of the bottom line had no
    // source. Measured on the Essex Ham fixtures: 3 of 4 refused, all of them correctable.
    //   • A pure SKIP is a sync offset within one line, so the shortfall it causes is bounded at
    //     ONE LINE — a sliver in the bottom-right corner, which is what the reference
    //     implementation (UberSDR's 24/7 addon, GPL-3.0, same lineage) simply leaves black. Its
    //     gallery is proof that this is invisible in practice.
    //   • A RATE correction is the dangerous one: the error grows down the image, so the bottom
    //     can land far past the buffer. That is what tore the picture and what this guard is FOR.
    // Two lines of slack tells those two cases apart by their own arithmetic, rather than by a
    // flag we would have to remember to keep in step with whether rate correction is enabled.
    const int allowShort = (int)(m->lineTime * rate * 2.0);
    int worstOver = 0;
    // ★ The first line the correction cannot fully cover. Everything above it is correctable.
    int firstBadLine = m->numLines;
    if (okOut) *okOut = true;
    std::vector<uint8_t> img((size_t)m->imgWidth*m->numLines*3, 0);
    auto IMG = [&](int x,int y,int c)->uint8_t&{ return img[((size_t)y*m->imgWidth+x)*3+c]; };
    std::string nm = m->name;
    for (auto& p : grid) {
        uint8_t lum;
        // ★ storedLumWritten, NOT storedLum.size(): the buffer is over-allocated and the tail is
        // zeros that were never captured. Reading them produces a plausible-looking image that is
        // simply wrong, which is worse than not correcting at all — so those pixels are LEFT as
        // they are (black), never substituted with the last sample.
        if (p.time >= 0 && p.time < storedLumWritten) lum = storedLum[p.time];
        else if (p.time >= storedLumWritten) {
            const int over = p.time - storedLumWritten + 1;
            if (over > worstOver) worstOver = over;
            if (over > allowShort && okOut) *okOut = false;
            if (p.y < firstBadLine) firstBadLine = p.y;
            continue;
        }
        else continue;
        if (p.x < m->imgWidth && p.y < m->numLines) IMG(p.x,p.y,p.channel) = lum;
        if (p.channel > 0 && (nm == "Robot 36" || nm == "Robot 24") && p.y+1 < m->numLines)
            IMG(p.x,p.y+1,p.channel) = lum;
    }
    lastGoodLines = firstBadLine;
    if (worstOver > 0) {
        // Worth saying out loud: it is the difference between "corrected, with a sliver missing"
        // and "not corrected at all", and the number tells which case this was.
        lastShortfallSamples = worstOver;
    }
    return toRGB(img);
}

// ── Sync corrector ───────────────────────────────────────────────────────────
void SstvSync::findSync(double& rateOut, int& skipOut, double* confOut) {
    double rate = sampleRate;
    int lineWidth = (int)(m->lineTime / m->syncTime * 4);
    if (lineWidth < 1) lineWidth = 1;
    int retries = 0, maxRetries = 3;

    for (;;) {
        // draw sync image
        std::vector<std::vector<uint8_t>> syncImg(lineWidth, std::vector<uint8_t>(m->numLines, 0));
        for (int y = 0; y < m->numLines; y++)
            for (int x = 0; x < lineWidth; x++) {
                double t = ((double)y + (double)x/lineWidth) * m->lineTime;
                int sn = (int)(t * rate / 13.0);
                if (sn >= 0 && sn < (int)hasSync.size()) syncImg[x][y] = hasSync[sn];
            }
        // Hough
        std::vector<std::vector<uint16_t>> lines(600, std::vector<uint16_t>((MaxSlant_-MinSlant_)*2, 0));
        int dMost = 0, qMost = 0;
        for (int cy = 0; cy < m->numLines; cy++)
            for (int cx = 0; cx < lineWidth; cx++) {
                if (!syncImg[cx][cy]) continue;
                for (int q = MinSlant_*2; q < MaxSlant_*2; q++) {
                    double ang = deg2rad(q/2.0);
                    int d = lineWidth + (int)std::lround(-(double)cx*std::sin(ang) + (double)cy*std::cos(ang));
                    if (d > 0 && d < lineWidth && d < (int)lines.size()) {
                        int qi = q - MinSlant_*2;
                        if (qi >= 0 && qi < (int)lines[d].size()) {
                            lines[d][qi]++;
                            int qmi = qMost - MinSlant_*2;
                            if (qmi >= 0 && qmi < (int)lines[dMost].size() && lines[d][qi] > lines[dMost][qmi]) { dMost = d; qMost = q; }
                        }
                    }
                }
            }
        if (qMost == 0) break;
        double slant = qMost/2.0;
        rate += std::tan(deg2rad(90-slant)) / (double)lineWidth * rate;
        if (slant > 89.0 && slant < 91.0) break;
        if (retries >= maxRetries) { rate = sampleRate; break; }
        retries++;
    }

    // find sync position
    std::vector<uint16_t> xAcc(700, 0);
    for (int y = 0; y < m->numLines; y++)
        for (int x = 0; x < 700; x++) {
            double t = (double)y*m->lineTime + (double)x/700.0*m->lineTime;
            int sn = (int)(t / (13.0/sampleRate) * rate / sampleRate);
            if (sn >= 0 && sn < (int)hasSync.size() && hasSync[sn]) xAcc[x]++;
        }
    double conv[8] = {1,1,1,1,-1,-1,-1,-1};
    double maxC = 0; int xMax = 0;
    for (int x = 0; x < 700-8; x++) {
        double c = 0; for (int i = 0; i < 8; i++) c += xAcc[x+i]*conv[i];
        if (c > maxC) { maxC = c; xMax = x+4; }
    }
    if (xMax > 350) xMax -= 350;
    double skipTime = (double)xMax/700.0*m->lineTime - m->syncTime;
    std::string nm = m->name;
    if (nm == "Scottie S1" || nm == "Scottie S2" || nm == "Scottie DX")
        skipTime += m->porchTime*2 - m->pixelTime*m->imgWidth/2.0;
    rateOut = rate;
    skipOut = (int)(skipTime * rate);

    // ★★★ HOW MUCH DID THE DATA ACTUALLY SUPPORT THIS? `maxC` is the strength of the convolution
    // peak that chose xMax — the horizontal shift applied to EVERY line. On a clean signal roughly
    // one sync edge per line contributes, so maxC scales with numLines. On a weak or noisy signal
    // few sync pulses are detected, the peak is essentially arbitrary, and the "correction" becomes
    // a RANDOM SHIFT of the whole picture.
    // ★★ That is the failure Stuart hit on 2026-07-31: a Martin M2 at S4 decoded roughly aligned,
    // then "Correcting slant..." displaced the middle and bottom and made it unreadable. The rate
    // loop already gives up safely (it restores sampleRate after maxRetries); the sync POSITION had
    // no such protection and was applied however weak the evidence.
    if (confOut) *confOut = (m->numLines > 0) ? (maxC / (double)m->numLines) : 0.0;
}

// ── Top-level decoder ────────────────────────────────────────────────────────
SstvDecoder::SstvDecoder(double sr, bool autoSync_, bool adaptive_)
    : sampleRate(sr), autoSync(autoSync_), adaptive(adaptive_), pcm(16384) {
    samps10ms = (int)(sr * 10e-3);
    accum.reserve(samps10ms * 2);
}
SstvDecoder::~SstvDecoder() {
    abort.store(true);
    if (vthread.joinable()) vthread.join();
    delete vis;
}

void SstvDecoder::process(const int16_t* mono, int count) {
    if (!statusSent) { if (onStatus) onStatus("Waiting for signal..."); statusSent = true; }
    // Initial buffer fill.
    if (pcm.windowPtr() == 0) { pcm.write(mono, count); return; }

    accum.insert(accum.end(), mono, mono + count);
    while ((int)accum.size() >= samps10ms) {
        pcm.write(accum.data(), samps10ms);
        accum.erase(accum.begin(), accum.begin() + samps10ms);

        if (state.load() == WaitingVIS) {
            if (!vis) { vis = new SstvVIS(sampleRate); vis->onTone = [](double){}; }
            uint8_t modeIdx; int shift;
            if (vis->process(pcm, modeIdx, shift)) {
                mode = sstvModeByIndex(modeIdx); headerShift = shift;
                if (!mode || mode->unsupported) { if (onStatus) onStatus("Mode not supported"); continue; }
                if (onMode) onMode(modeIdx, mode->name);
                if (onImageStart) onImageStart(mode->imgWidth, mode->numLines);
                state.store(Decoding);
                abort.store(false);
                if (vthread.joinable()) vthread.join();
                vthread = std::thread([this]{ videoThread(); });
            }
        }
        // While Decoding, the video thread consumes pcm; we keep feeding it.
    }
}

void SstvDecoder::videoThread() {
    if (onStatus) onStatus(std::string("Decoding ") + mode->name + "...");
    SstvVideo video(mode, sampleRate, headerShift, adaptive);
    auto sender = [this](int y, const uint8_t* rgb) { if (onLine) onLine(y, mode->imgWidth, rgb); };
    video.demodulate(pcm, sampleRate, 0, sender, abort);
    if (abort.load()) return;
    if (onComplete) onComplete();

    if (autoSync) {
        if (onStatus) onStatus("Correcting slant...");
        SstvSync sync(mode, sampleRate, video.syncFlags());
        double aRate; int aSkip; double conf = 0;
        sync.findSync(aRate, aSkip, &conf);
        // ★★★ DO NOT "CORRECT" ON EVIDENCE THIS WEAK. Below this the sync peak is indistinguishable
        // from noise and the shift is arbitrary — applying it turns a readable picture into an
        // unreadable one, which is the worst possible trade for a decoder. 0.25 = a quarter of the
        // lines contributing a clean sync edge; a genuine signal clears it easily.
        static const double MIN_SYNC_CONF = 0.25;
        if (conf < MIN_SYNC_CONF) {
            if (onStatus) onStatus("slant not corrected \u2014 sync too weak");
            state.store(WaitingVIS); pcm.reset(); delete vis; vis = nullptr;
            return;
        }
        // ★★★ APPLY THE OFFSET TO THE WHOLE PICTURE, NOT A SHEAR. findSync returns TWO corrections:
        //   • `skip` — a CONSTANT horizontal offset, identical on every line.
        //   • `rate` — a SHEAR, shifting each line slightly more than the last (a true "slant").
        // We applied both. But the defect here is the OFFSET: the decoder latched onto the wrong
        // point in the line, so the picture arrives WRAPPED — the right-hand edge down the left, by
        // the SAME amount on every row.
        //
        // ★★ Applying a shear on top of that is exactly why the top came out aligned and everything
        // below it walked away — corrected as a slice rather than as a picture. Reproduced on
        // Scottie S2 and Martin M2 (Stuart, 2026-07-31): "all you have to do is literally do the
        // alignment at the top to the whole picture, not just a slice."
        //
        // ★★★ AND THERE SHOULD BE NO SLANT TO CORRECT ON AN SDR. Slant correction exists for
        // SOUNDCARD CLOCK DRIFT — a sample rate that is not quite what it claims to be. An SDR's
        // clock is locked and its rate exact, so the timebase is already right and only the line
        // START is wrong. Estimating a shear from sync pulses in noise then invents a correction for
        // a fault that does not exist, and can only make the picture worse.
        // ★★ WHAT THE PICTURES ACTUALLY SHOW (Stuart, over an evening of live SSTV): "some come
        // through slanted, but MOST have been fine just with a tiny strip of the right side on the
        // left." So the common defect by far is a pure constant offset, and the shear is the rare
        // case — which is the wrong way round from what the code assumed, since it applied the
        // shear every time and thereby broke the many to serve the few.
        // ★ `aRate` is still computed — findSync needs it internally to locate the sync position —
        // and deliberately NOT applied. Re-enabling it should be gated on a slant estimate that is
        // both CONFIDENT and LARGE ENOUGH TO BE REAL, so the common offset-only case is never
        // sheared. Do not simply switch it back on.
        bool ok = false;
        auto pixels = video.redrawFromLuminance(sampleRate, aSkip, &ok);
        (void)aRate;
        // ★★★ REPLACE THE PICTURE UNLESS THE CORRECTION WOULD TEAR IT. A redraw that runs FAR past
        // the captured samples produces an image whose top is aligned and whose bottom is not —
        // "sliced up and assembled out of alignment" (Stuart, 2026-07-31, on a Scottie S2 that had
        // decoded cleanly). ★★ AND THE UNCORRECTED PICTURE IS THE BETTER ONE THERE: a readable image
        // with a constant horizontal wrap beats a torn one, because the eye can read past a wrap and
        // cannot read past a tear. When we cannot finish the job, leave the user what they had.
        // ★★★ BUT "runs past at all" IS NOT THE SAME TEST AS "would tear it", and using the first
        // for the second is what left three of the four Essex Ham fixtures uncorrected — every one
        // of them correctable, refused over a sliver in the bottom line. See redrawFromLuminance:
        // the shortfall is now measured, and a skip-sized one (bounded by a single line) is
        // accepted with those few pixels left black — which is exactly what the reference
        // implementation does, and its 24/7 gallery is the evidence that it is invisible.
        // ★★★ A PICTURE THAT STOPPED EARLY CAN STILL BE ALIGNED. A fade, a QSY or a transmission
        //     that simply ends leaves the lower part of the frame blank — and because those lines
        //     have no samples, `ok` goes false and the WHOLE correction was abandoned, leaving the
        //     part that did arrive wrapped. On HF that is not an edge case, it is most of them.
        //     ★★ THE TEST IS NOT "how many lines can I correct" — correcting only the top of a
        //     COMPLETE picture is precisely the tear this guard exists to prevent ("the top third
        //     sits where the whole image should be and the rest stays put"). It is: CAN I CORRECT
        //     EVERY LINE THAT ACTUALLY ARRIVED? If so there is nothing below the boundary to
        //     mismatch — the rest of the frame is blank either way — and no seam is possible.
        const int have = video.linesReceived;
        const bool partialOk = !ok && have > 0 && video.lastGoodLines >= have;
        if (ok || partialOk) {
            if (onRedrawStart) onRedrawStart();
            const int upto = ok ? mode->numLines : have;
            for (int y = 0; y < upto; y++)
                if (onLine) onLine(y, mode->imgWidth, pixels.data() + (size_t)y*mode->imgWidth*3);
            if (onComplete) onComplete();
            if (partialOk && onStatus)
                onStatus("aligned \u2014 signal ended after " + std::to_string(have) + " lines");
        } else if (onStatus) {
            // ★ Say WHICH case this was. "Incomplete audio" was reported for a one-line shortfall
            //   and for a tearing one alike, so the message could not tell a real failure from an
            //   over-strict guard — and it read as a fault when nothing was wrong.
            // ★ WITH THE NUMBERS. "Would tear the picture" alone cannot be acted on or debugged:
            //   short-by tells you whether it missed by a sliver or by a mile, and the line counts
            //   say whether the picture was complete when it was refused.
            onStatus("alignment skipped \u2014 would tear the picture (short by "
                     + std::to_string(video.lastShortfallSamples) + " samples; "
                     + std::to_string(video.lastGoodLines) + " of "
                     + std::to_string(video.linesReceived) + " received lines correctable)");
        }
    }

    // Reset for the next image.
    state.store(WaitingVIS);
    pcm.reset();
    delete vis; vis = nullptr;
}

} // namespace vibe
