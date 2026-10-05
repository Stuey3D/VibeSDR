// VibeSDR V4 — SSTV decoder (C++ port of ka9q audio_extensions/sstv → slowrx).
#include "sstv_decoder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vibe {

// ── Mode specs (slowrx timings) ──────────────────────────────────────────────
static const SstvMode kModes[] = {
    {"Unknown",0,0,0,0,0,0,0,0,SSTV_BW,true},
    {"Martin M1",4.862e-3,0.572e-3,0.572e-3,0.4576e-3,446.446e-3,320,256,1,SSTV_GBR,false},
    {"Martin M2",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,226.7986e-3,320,256,1,SSTV_GBR,false},
    {"Martin M3",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,446.446e-3,320,128,2,SSTV_GBR,false},
    {"Martin M4",4.862e-3,0.572e-3,0.572e-3,0.2288e-3,226.7986e-3,320,128,2,SSTV_GBR,false},
    // ★★ S1's line is 428.22 ms (9 + 1.5 + 3×138.24 + 2×1.5 — the spec, QSSTV and MMSSTV). slowrx's
    //    428.38 is the one entry here that is not the sum of its own parts; slowrx always applied its
    //    slant fix, which hid it. Ours did not, so every S1 picture leaned 95 px over the frame — the
    //    line fit measured it as a "+373 ppm sender" on a perfect one (test-sstv-quality, 2026-10-04).
    {"Scottie S1",9e-3,1.5e-3,1.5e-3,0.4320e-3,428.22e-3,320,256,1,SSTV_GBR,false},
    {"Scottie S2",9e-3,1.5e-3,1.5e-3,0.2752e-3,277.692e-3,320,256,1,SSTV_GBR,false},
    {"Scottie DX",9e-3,1.5e-3,1.5e-3,1.08053e-3,1050.3e-3,320,256,1,SSTV_GBR,false},
    // ★★ R72 is Y 138 ms + R-Y 69 ms + B-Y 69 ms, each chroma after a 4.5 ms separator and a 1.5 ms
    //    porch (the Dayton spec; MMSSTV) — 9+3+138+6+69+6+69 = 300. slowrx's 0.2875 ms pixel cut it
    //    into three EQUAL 92 ms channels: Y read two-thirds of the line, chroma read Y. Measured on a
    //    generated R72 (tools/sstv_bench, 2026-10-05, audit "Robot 72 timing"): SSIM 0.17 → see the
    //    commit. pixelTime is the CHROMA pixel; Y is twice it, as Robot 36 has it.
    {"Robot 72",9e-3,3e-3,6e-3,0.215625e-3,300e-3,320,240,1,SSTV_YUV,false},
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

// ★ ROUNDED, not truncated (2026-10-04): truncation took half a level off every pixel on average.
static inline uint8_t clip(double v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)std::lround(v)); }

// ★★★ ROBOT AND PD SEND STUDIO-RANGE YUV — BT.601, Y 16–235 and chroma 16–240 about 128 (audit
// 2026-10-04, row 4; the Dayton/Barber spec, and what MMSSTV and QSSTV transmit). It was decoded as
// FULL range (slowrx's R = Y + 1.40(V−128)): black came out at 16, white at 235 and the colours ~13 %
// short of saturation — measured 18 levels of error on 100 % bars. One function, so the live
// lines and the redraw cannot disagree.
static inline void yuvToRgb(uint8_t y, uint8_t v, uint8_t u, uint8_t* rgb) {
    const double Y = 1.164 * ((double)y - 16.0), V = (double)v - 128.0, U = (double)u - 128.0;
    rgb[0] = clip(Y + 1.596 * V);
    rgb[1] = clip(Y - 0.813 * V - 0.392 * U);
    rgb[2] = clip(Y + 2.017 * U);
}
// ★★★ PD IS A FAMILY, NOT A WIDTH (2026-10-05). PD sends TWO picture lines per sync — Y, R-Y, B-Y, Y —
// and every PD branch here was chosen by `YUV && imgWidth >= 512`, slowrx's shortcut that leaves out
// PD-50 and PD-90: they are 320 wide. Both were decoded as a one-line-per-sync mode — the whole
// picture squeezed into the TOP HALF of the frame and 25 s of after-the-end noise smeared through
// the bottom half (the "horizontal streaks in the lower half" of Stuart's PD-50 of 15:40 UTC
// 2026-10-05, where UberSDR — fixed upstream with a PDFormat flag — still read "HA7BJ").
// PD-50 is one of 20 m's commonest modes. Measured on tools/sstv_bench, PD-50 at 20 dB:
// SSIM 0.06 → see the commit table.
static inline bool isPD(const SstvMode* m) { return m->name[0] == 'P' && m->name[1] == 'D'; }
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
SstvBuffer::SstvBuffer(int requested, bool exact) {
    int minSize = exact ? 4096 : 8 * 1024 * 1024;
    size = requested > minSize ? requested : minSize;
    buf.assign(size, 0);
}
int SstvBuffer::availableLocked() {
    return writePos >= wptr ? writePos - wptr : (size - wptr) + writePos;
}
void SstvBuffer::write(const int16_t* s, int n) {
    std::lock_guard<std::mutex> lk(mu);
    // ★ `total` counts samples STORED: the initial fill drops whatever overshoots 1024, and the
    //   restart arithmetic (writtenTotal vs 512 + consumed) must be in the buffer's own coordinates.
    if (!primed) {
        for (int i = 0; i < n && fillPos < 1024; i++) { buf[fillPos++] = s[i]; total++; }
        if (fillPos >= 1024) { wptr = 512; writePos = fillPos; primed = true; }
    } else {
        for (int i = 0; i < n; i++) { buf[writePos] = s[i]; writePos = (writePos + 1) % size; }
        total += n;
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
void SstvBuffer::advanceWindow(int n) { std::lock_guard<std::mutex> lk(mu); wptr = ((wptr + n) % size + size) % size; advanced += n; }
long long SstvBuffer::writtenTotal() { std::lock_guard<std::mutex> lk(mu); return total; }
long long SstvBuffer::consumed() { std::lock_guard<std::mutex> lk(mu); return advanced; }
int  SstvBuffer::windowPtr() { std::lock_guard<std::mutex> lk(mu); return wptr; }
int  SstvBuffer::available() { std::lock_guard<std::mutex> lk(mu); return availableLocked(); }
void SstvBuffer::reset() { std::lock_guard<std::mutex> lk(mu); std::fill(buf.begin(), buf.end(), 0); wptr = writePos = fillPos = 0; total = advanced = 0; primed = false; }

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
    if (isPD(m))
        maxLen = (int)(m->lineTime*m->numLines/2*sr*1.3) + 15000;
    else
        maxLen = (int)(m->lineTime*m->numLines*sr*1.3) + 15000;
    hasSync.assign(maxLen/13 + 1, 0);
    syncLevel.assign(hasSync.size(), -1.0f);
    storedLum.assign(maxLen, 0);
    fin.assign(1024, 0);
}

std::vector<SstvPixel> SstvVideo::pixelGrid(double rate, int skip) {
    double chanStart[4] = {0,0,0,0}, chanLen[4] = {0,0,0,0};
    int numChans = 3;
    std::string nm = m->name;
    bool robot = (nm == "Robot 36" || nm == "Robot 24");
    bool scottie = (nm == "Scottie S1" || nm == "Scottie S2" || nm == "Scottie DX");
    bool pd = isPD(m);

    if (robot) {
        chanLen[0] = m->pixelTime*m->imgWidth*2; chanLen[1] = m->pixelTime*m->imgWidth; chanLen[2] = chanLen[1];
        chanStart[0] = m->syncTime + m->porchTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime; chanStart[2] = chanStart[1];
        numChans = 2;
    } else if (nm == "Robot 72") {
        chanLen[0] = m->pixelTime*m->imgWidth*2; chanLen[1] = chanLen[2] = m->pixelTime*m->imgWidth;
        chanStart[0] = m->syncTime + m->porchTime;
        chanStart[1] = chanStart[0] + chanLen[0] + m->septrTime;
        chanStart[2] = chanStart[1] + chanLen[1] + m->septrTime;
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
                    // ★ The CENTRE of pixel x, as the PD branch above has it (2026-10-04). slowrx's
                    //   (x − 0.5) sampled the centre of pixel x−1, so every Martin/Scottie/Robot
                    //   picture sat one pixel right — measured +1.0 px on the test card.
                    double t = (double)y*m->lineTime + chanStart[c] + ((double)x+0.5)/m->imgWidth*chanLen[ch];
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
    // ★★★ CENTRE THE WINDOW ON THE SAMPLE IT REPORTS (audit 2026-10-04, row 3). This read 64
    // samples from -32 — slowrx's numbers at 44.1 kHz — but at 12 kHz the Hann window is 17 long,
    // so only samples -32..-16 were weighted: every flag described the signal 24 samples (2 ms)
    // BEFORE the sample it was filed under, and every aligned picture came out 4–10 px off.
    const int L = std::min((int)hannWins[1].size(), (int)fin.size());
    int16_t s[1024]; pcm.getWindow(-L/2, L, s);   // L <= fin.size() == 1024
    for (int i = 0; i < L; i++) fin[i] = (float)(s[i]/32768.0*hannWins[1][i]);
    for (int i = L; i < (int)fin.size(); i++) fin[i] = 0;
    fft.run(fin.data());
    double pRaw = 0, pSync = 0;
    int minB = getBin(1500.0+headerShift), maxB = getBin(2300.0+headerShift);
    for (int i = minB; i <= maxB; i++) pRaw += fft.power(i);
    for (int i = targetBin-1; i <= targetBin+1; i++) { double w = 1.0 - 0.5*std::fabs((double)(targetBin-i)); pSync += fft.power(i)*w; }
    pRaw /= (double)(maxB - minB); pSync /= 2.0;
    if (idx < (int)hasSync.size()) {
        hasSync[idx] = (pSync > 2*pRaw) ? 1 : 0;
        syncLevel[idx] = (float)std::log10((pSync + 1e-30) / (2*pRaw + 1e-30));
    }
}

double SstvVideo::estimateSNR(SstvBuffer& pcm) {
    // ★ Same fault as detectSync (2026-10-04): 1024 from -512 with a 279-sample window measured the
    //   SNR 31 ms in the past. Centred now; it only picks the demod window size, so the effect is
    //   small, but it was the same arithmetic.
    const int L = std::min((int)hannWins[6].size(), (int)fin.size());
    int16_t s[1024]; pcm.getWindow(-L/2, L, s);
    for (int i = 0; i < (int)fin.size(); i++) fin[i] = (i < L) ? (float)(s[i]/32768.0*hannWins[6][i]) : 0;
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

double SstvVideo::demodFreq(SstvBuffer& pcm, double snr, int ahead) {
    int winIdx = 0;
    if (adaptive) {
        if (snr >= 20) winIdx = 0; else if (snr >= 10) winIdx = 1; else if (snr >= 9) winIdx = 2;
        else if (snr >= 3) winIdx = 3; else if (snr >= -5) winIdx = 4; else if (snr >= -10) winIdx = 5; else winIdx = 6;
        if (std::string(m->name) == "Scottie DX" && winIdx < 6) winIdx++;
    }
    int L = hannLens[winIdx];
    std::vector<int16_t> s(L); pcm.getWindow(-L/2 + ahead, L, s.data());
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
                           const std::atomic<bool>& abort, const std::atomic<bool>* interrupt) {
    auto grid = pixelGrid(rate, skip);
    int length;
    if (isPD(m)) length = (int)(m->lineTime*m->numLines/2*sampleRate);
    else length = (int)(m->lineTime*m->numLines*sampleRate);
    // ★★ CAPTURE A LITTLE PAST THE NOMINAL END (2026-10-04). A sender whose clock runs SLOW makes
    //    the picture longer than the mode says — 0.05 % at -500 ppm — and a start a few ms early
    //    pushes the end later too. The slant correction can only redraw from samples it has, so
    //    keep 0.25 % (the 2000 ppm gate in findSync, plus room) and 20 ms more. Lines are emitted
    //    on the grid exactly as before; this only extends storedLum and the sync record.
    length += (int)(length * 0.0025) + (int)(0.02 * sampleRate);
    int syncTargetBin = getBin(1200.0 + headerShift);
    const bool pd = isPD(m);

    int numChans = 3;
    std::string nm = m->name;
    if (nm == "Robot 36" || nm == "Robot 24") numChans = 2;
    else if (isPD(m)) numChans = 4;
    else if (m->color == SSTV_BW) numChans = 1;

    // image[x][y][3]
    std::vector<uint8_t> img((size_t)m->imgWidth * m->numLines * 3, 0);
    auto IMG = [&](int x, int y, int c) -> uint8_t& { return img[((size_t)y*m->imgWidth + x)*3 + c]; };

    int pixelIdx = 0, nextSync = 0, nextSNR = 0, syncSampleNum = 0;
    double snr = 0, freq = 0;

    // ★★★ A VIS IS A PROMISE, THE SYNC TRAIN IS THE PICTURE (2026-10-05). The VIS detector can be
    // satisfied by a VIS that no picture follows, and once it was, the decoder painted the mode's
    // whole length (58 s for a Martin M2) and stayed DEAF to every VIS meanwhile — Stuart's 20 m
    // recording of 15:38 UTC: an M2 VIS, ~4 s of tone, and the Scottie S2 that began 10 s later
    // was never heard. A picture has a sync pulse at the same place on every line; noise does not.
    //   • The picture is not STARTED on the client until CONFIRM lines have shown that pulse where
    //     the mode puts it. Its lines are held, then sent in one go (about a second on a good signal).
    //   • No confirmation within the first `abortN` lines → it was never a picture: dropped unseen.
    //   • Once confirmed, at most 15 % of the last `lostN` lines (≥ 8 s) with a pulse → the sender
    //     stopped or faded out: end the picture there and listen for the next VIS.
    // The audit's "abort after ~20 lines with no sync". Measured: see the commit table.
    const double P = rate * m->lineTime;                               // samples per sync line
    const int syncLines = pd ? m->numLines / 2 : m->numLines;
    const double syncS = m->syncTime * sampleRate;
    const bool scottie = (nm == "Scottie S1" || nm == "Scottie S2" || nm == "Scottie DX");
    // Scottie: the starting pulse, then the line's two colours ahead of its own pulse.
    const double syncOff = scottie ? (m->syncTime + 2.0 * m->septrTime + 2.0 * m->pixelTime * m->imgWidth) * rate : 0.0;
    const int abortN = std::max(12, std::min(20, (int)std::ceil(12.0 * sampleRate / P)));
    const int lostN  = std::max(20, (int)std::ceil(8.0 * sampleRate / P));
    const int CONFIRM = 5;
    bool confirmed = !gateOnSync;
    int evalLine = 0, synced = 0;
    std::vector<uint8_t> lineSync;                                     // per sync line, 1 = pulse found
    std::vector<std::pair<int, std::vector<uint8_t>>> held;
    endReason = EndComplete;
    syncLinesSeen = 0;
    auto emit = [&](int y, const uint8_t* rgb) {
        if (confirmed) { lineSender(y, rgb); return; }
        held.emplace_back(y, std::vector<uint8_t>(rgb, rgb + (size_t)m->imgWidth * 3));
    };
    // ★★ WHERE TO LOOK: near the LAST pulse found, one line on — not at k·P from the VIS. From the
    //    VIS the window has to widen with the sender's possible clock error (1500 ppm × 255 M2 lines
    //    = 87 ms, most of the line), and a window that wide finds a "pulse" in anything: the first
    //    version of this confirmed 250 of 256 lines of that M2 VIS's tone. Anchored, it is ±8 ms.
    double anchor = -1; int anchorK = -1;
    auto window = [&](int k, double& c, double& slack) {
        if (anchorK >= 0 && k - anchorK <= 12) { c = anchor + (k - anchorK) * P; slack = 0.008 * sampleRate + 1500e-6 * (k - anchorK) * P; }
        else { c = k * P + syncOff; slack = 0.020 * sampleRate + 1000e-6 * k * P; }  // VIS hand-off + clock
    };
    // Is there a PULSE in the window — a pulse-wide box mostly flagged — standing out of a line that
    // mostly is NOT flagged? ★ Both halves: a steady tone just below 1500 Hz (the 17-sample detector
    // window is ~700 Hz wide) can flag "sync" everywhere.
    double lastBox = 0, lastBg = 0;
    auto lineHasSync = [&](int k) -> bool {
        double c, slack; window(k, c, slack);
        const int n = (int)hasSync.size();
        const int i0 = std::max(0, (int)std::floor((c - slack) / 13.0));
        const int i1 = std::min(n - 1, (int)std::ceil((c + syncS + slack) / 13.0));
        const int bw = std::max(2, (int)std::lround(syncS / 13.0));
        int best = 0, bestAt = i0;
        for (int i = i0; i + bw - 1 <= i1; i++) {
            int a = 0; for (int j = 0; j < bw; j++) a += hasSync[i + j];
            if (a > best) { best = a; bestAt = i; }
        }
        // the rest of this line: from a pulse after this one to a pulse before the next
        const int b0 = bestAt + 2 * bw, b1 = std::min(n - 1, bestAt + (int)((P - syncS) / 13.0));
        int on = 0, cnt = 0; for (int i = b0; i <= b1; i++) { on += hasSync[i]; cnt++; }
        lastBox = (double)best / bw; lastBg = cnt ? (double)on / cnt : 1.0;
        // ★ A third of a long pulse is plenty against a 1–5 % background: the weak PD-50 of 15:40 UTC
        //   showed 6–8 of its 18 flags per pulse, and half (the first threshold) dropped it unseen.
        const bool ok = best >= 2 && lastBox >= 0.3 && lastBg <= 0.3 && lastBox >= lastBg + 0.25;
        if (ok) { anchor = bestAt * 13.0; anchorK = k; }
        return ok;
    };

    for (int sampleNum = 0; sampleNum < length; sampleNum++) {
        if (abort.load()) return;
        if (interrupt && interrupt->load()) { endReason = confirmed ? EndInterrupted : EndNoSync; return; }
        // Judge each sync line once its window and the line after it have been flagged.
        while (evalLine < syncLines) {
            double c, slack; window(evalLine, c, slack);
            if (sampleNum <= c + P + slack + 32) break;
            const bool s = lineHasSync(evalLine++);
            lineSync.push_back(s ? 1 : 0);
            if (s) { synced++; syncLinesSeen++; }
            if (!confirmed) {
                if (synced >= CONFIRM) {
                    confirmed = true;
                    if (onConfirmed) onConfirmed();
                    for (auto& h : held) lineSender(h.first, h.second.data());
                    held.clear();
                } else if (evalLine >= abortN) { endReason = EndNoSync; return; }
            } else if (gateOnSync && evalLine >= lostN) {
                int recent = 0; for (int i = evalLine - lostN; i < evalLine; i++) recent += lineSync[i];
                if (recent <= std::max(1, lostN * 15 / 100)) { endReason = EndSignalLost; return; }
            }
        }
        if (pcm.available() < 1024) {
            for (int i = 0; i < 500 && pcm.available() < 1024; i++) {
                if (abort.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pcm.available() < 1024) break;
        }
        if (sampleNum == nextSync) { detectSync(pcm, syncTargetBin, syncSampleNum); nextSync += 13; syncSampleNum++; }
        if (sampleNum == nextSNR) { snr = estimateSNR(pcm); nextSNR += 256; }
        // ★ The estimate is HELD for the next 6 samples, so centre it on them (s+2.5), not on the
        //   first: centred on `s` it lagged the picture by 2.5 samples — half a Martin pixel, most
        //   of a Robot one (2026-10-04, measured with the line fit in place).
        if (sampleNum % 6 == 0) freq = demodFreq(pcm, snr, 3);

        uint8_t lum = clip((freq - (1500.0 + headerShift)) / 3.1372549);
        if (sampleNum < (int)storedLum.size()) { storedLum[sampleNum] = lum; storedLumWritten = sampleNum + 1; }

        while (pixelIdx < (int)grid.size() && grid[pixelIdx].time == sampleNum) {
            SstvPixel p = grid[pixelIdx];
            if (p.x < m->imgWidth && p.y < m->numLines) IMG(p.x, p.y, p.channel) = lum;
            if (p.channel > 0 && (nm == "Robot 36" || nm == "Robot 24") && p.y+1 < m->numLines)
                IMG(p.x, p.y+1, p.channel) = lum;

            // ★★ PD NEVER SENT A LIVE LINE (found 2026-10-04 by test-sstv-quality). Its last pixel of a
            //    pair is the second line's Y — channel 0 — so `channel >= numChans-1` (3) was never
            //    true: a PD picture (the ISS's mode) stayed blank until the redraw, and stayed blank
            //    for good whenever the redraw was refused. A pair is complete on that pixel.
            const bool lineDone = pd ? (p.x == m->imgWidth-1 && p.channel == 0 && (p.y & 1))
                                     : (p.x == m->imgWidth-1 && (int)p.channel >= numChans-1);
            if (lineSender && lineDone) {
                std::vector<uint8_t> line((size_t)m->imgWidth*3);
                for (int y = pd ? p.y - 1 : p.y; y <= p.y; y++) {
                    if (y < 0 || y >= m->numLines) continue;
                    for (int x = 0; x < m->imgWidth; x++) {
                        int o = x*3;
                        uint8_t c0=IMG(x,y,0), c1=IMG(x,y,1), c2=IMG(x,y,2);
                        switch (m->color) {
                            case SSTV_RGB: line[o]=c0; line[o+1]=c1; line[o+2]=c2; break;
                            case SSTV_GBR: line[o]=c2; line[o+1]=c0; line[o+2]=c1; break;
                            case SSTV_YUV: yuvToRgb(c0, c1, c2, &line[o]); break;
                            case SSTV_BW: line[o]=line[o+1]=line[o+2]=c0; break;
                        }
                    }
                    emit(y, line.data());
                    if (y + 1 > linesReceived) linesReceived = y + 1;
                }
            }
            pixelIdx++;
        }
        pcm.advanceWindow(1);
    }
    if (!confirmed) endReason = EndNoSync;   // the audio ran out before the picture proved itself
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
                case SSTV_YUV: yuvToRgb(c0, c1, c2, &rgb[o]); break;
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
// ★★★ WHERE THE SLANT COMES FROM (audit 2026-10-04, row 2). The TRANSMITTING station's sound card:
// it believes it runs at 48000 Hz and really runs at 48000·(1+ε), with ε of 100–1000 ppm on
// ordinary PC hardware. Every line it sends is 1/(1+ε) as long as the mode says, and an exact
// receive clock — an SDR's — receives that error faithfully. 100 ppm is about 25 px over a Martin
// M1 frame. It is not a receiver fault, and no receiver clock can remove it.
//
// Two stages, both measured by vibeserver/test-sstv-quality.cpp:
//   1. slowrx's Linear Hough on the sync image — a COARSE rate, good to a fraction of a sync pulse
//      over the frame. Its only job is to seed stage 2.
//   2. A least-squares line through the sync pulse on every line — the rate is the slope, the
//      start of the picture is the intercept. Every line votes, outliers are thrown out, and the
//      slope comes with a standard error, so "is this slant real" is a number, not a hope. The
//      audit's suggestion; the Hough alone resolves only 0.5° — 132 ppm per step on Robot 36.
static const int SyncStep = 13;   // hasSync holds one flag per 13 samples (demodulate's nextSync)

void SstvSync::findSync(double& rateOut, int& skipOut, double* confOut) {
    const bool pd = isPD(m);
    // ★ PD sends ONE sync per PAIR of lines and its lineTime is the pair — so it has numLines/2
    //   sync lines. Scanning numLines of them fed the search half a picture of empty space.
    const int syncLines = pd ? m->numLines / 2 : m->numLines;
    double rate = sampleRate;
    int lineWidth = (int)(m->lineTime / m->syncTime * 4);
    if (lineWidth < 1) lineWidth = 1;
    int retries = 0, maxRetries = 3;

    for (;;) {
        // draw sync image
        std::vector<std::vector<uint8_t>> syncImg(lineWidth, std::vector<uint8_t>(syncLines, 0));
        for (int y = 0; y < syncLines; y++)
            for (int x = 0; x < lineWidth; x++) {
                double t = ((double)y + (double)x/lineWidth) * m->lineTime;
                int sn = (int)(t * rate / (double)SyncStep);
                if (sn >= 0 && sn < (int)hasSync.size()) syncImg[x][y] = hasSync[sn];
            }
        // Hough
        std::vector<std::vector<uint16_t>> lines(600, std::vector<uint16_t>((MaxSlant_-MinSlant_)*2, 0));
        for (int cy = 0; cy < syncLines; cy++)
            for (int cx = 0; cx < lineWidth; cx++) {
                if (!syncImg[cx][cy]) continue;
                for (int q = MinSlant_*2; q < MaxSlant_*2; q++) {
                    double ang = deg2rad(q/2.0);
                    int d = lineWidth + (int)std::lround(-(double)cx*std::sin(ang) + (double)cy*std::cos(ang));
                    if (d > 0 && d < lineWidth && d < (int)lines.size()) lines[d][q - MinSlant_*2]++;
                }
            }
        // ★★★ THE PEAK IS FOUND AFTER THE VOTE, NOT DURING IT (audit 2026-10-04, row 1). The running
        // maximum started at qMost = 0, so its index into the table was 0 - MinSlant_*2 = -60, the
        // `>= 0` guard rejected it, and it was never updated: `qMost == 0` broke out on the first
        // pass, every time, since a8b7464c. The rate this function returned was ALWAYS nominal.
        // ★★ So the "shear" blamed in ed1ebd12 and ca092485 can never have been applied — those
        //    pictures were torn by the offset (the half-line wrap below) and the unwritten tail.
        int qMost = 0; unsigned best = 0;
        for (int d = 1; d < lineWidth && d < (int)lines.size(); d++)
            for (int qi = 0; qi < (int)lines[d].size(); qi++)
                if (lines[d][qi] > best) { best = lines[d][qi]; qMost = qi + MinSlant_*2; }
        if (best == 0) break;
        double slant = qMost/2.0;
        rate += std::tan(deg2rad(90-slant)) / (double)lineWidth * rate;
        if (slant > 89.0 && slant < 91.0) break;
        if (retries >= maxRetries) { rate = sampleRate; break; }
        retries++;
    }
    houghPpm = (sampleRate / rate - 1.0) * 1e6;

    // ── Stage 2: least squares through the sync pulses ──────────────────────────────────────
    const double P0 = rate * m->lineTime;                 // samples per sync line, seeded by stage 1
    const double syncS = m->syncTime * sampleRate;        // pulse length, samples
    const int nFlags = (int)hasSync.size();
    // Phase: fold every flag onto one line at the seed rate and take the densest pulse-wide window.
    const int NB = 700;
    std::vector<double> hist(NB, 0.0);
    for (int i = 0; i < nFlags; i++)
        if (hasSync[i]) hist[(int)(std::fmod((double)i * SyncStep, P0) / P0 * NB) % NB] += 1.0;
    const int boxW = std::max(1, (int)std::lround(syncS / P0 * NB));
    double bestBox = -1; int bestStart = 0;
    for (int b = 0; b < NB; b++) {
        double acc = 0; for (int k = 0; k < boxW; k++) acc += hist[(b + k) % NB];
        if (acc > bestBox) { bestBox = acc; bestStart = b; }
    }
    const double phase = (bestStart + boxW / 2.0) / NB * P0;   // a pulse centre, samples

    // Measure the pulse on each line near the predicted one: the longest run of flags in a ±1-pulse
    // window (a line without a run of at least half a pulse abstains), placed by its TRAILING edge.
    // ★★★ THE TRAILING EDGE, NOT THE MIDDLE. Every mode follows the pulse with a 1500 Hz porch, so
    //     that edge has the same neighbour on every line and its bias is a CONSTANT — calibrated
    //     out below. The leading edge borders PICTURE: a white or a black last pixel moves it by
    //     samples, so a picture whose bottom differs from its top tilted a both-edges fit
    //     (measured −1.4 ppm on PD-120 from a checkerboard in the bottom quarter — a 1 px lean, on
    //     a perfect signal), and Robot's 1900 Hz chroma neighbour put the middle 2 samples early.
    // ★ The constant: the detector's 17-sample window says "sync" until a little of the porch is
    //   in it, so the edge it reports is ~2 samples before the real one at 12 kHz (0.12 of the
    //   window; measured +3 M1, +1.5 S1, +2 R36, +1 PD, unchanged from 20 to 10 dB SNR once the
    //   gaps below are bridged).
    const int hannLen = std::max(8, (int)std::lround(64.0 * sampleRate / 44100.0));   // detectSync's window (SstvVideo)
    const double trailBias = 0.12 * (double)hannLen;
    std::vector<double> ks, ms;
    std::vector<uint8_t> keep;
    auto measure = [&](double A0, double B0) {
        ks.clear(); ms.clear();
        for (int k = -1; k <= syncLines + 1; k++) {
            const double c = A0 + B0 * k;
            const int i0 = std::max(0, (int)std::floor((c - syncS) / SyncStep));
            const int i1 = std::min(nFlags - 1, (int)std::ceil((c + syncS) / SyncStep));
            if (i1 <= i0) continue;
            // ★ Runs are bridged across gaps of up to MaxGap flags. In noise a long pulse (PD's is 18
            //   flags) loses a flag or two in the middle, the "longest run" became a FRAGMENT, and
            //   its edge was not the pulse's edge — 23 samples off on PD-120 at 10 dB.
            const int MaxGap = 2;
            int bestFirst = -1, bestLast = -1, curFirst = -1, curLast = -1;
            for (int i = i0; i <= i1; i++) {
                if (!hasSync[i]) continue;
                if (curFirst >= 0 && i - curLast <= MaxGap + 1) curLast = i;
                else { curFirst = curLast = i; }
                if (curLast - curFirst > bestLast - bestFirst) { bestFirst = curFirst; bestLast = curLast; }
            }
            if (bestFirst < 0 || (bestLast - bestFirst + 1) * SyncStep < syncS * 0.5) continue;
            // The whole run, even where it leaves the window — a clipped run has a false edge.
            int first = bestFirst, last = bestLast;
            const int maxRun = (int)(2.0 * syncS / SyncStep) + 2;
            auto onNear = [&](int i, int dir) { for (int g = 1; g <= MaxGap + 1; g++) { const int j = i + dir * g; if (j < 0 || j >= nFlags) return -1; if (hasSync[j]) return j; } return -1; };
            for (int j; last - first < maxRun && (j = onNear(first, -1)) >= 0; ) first = j;
            for (int j; last - first < maxRun && (j = onNear(last, +1)) >= 0; ) last = j;
            // ★★ THE EDGE BETWEEN TWO FLAGS. A flag is every 13 samples — nearly a whole Robot 36
            //    chroma pixel. The level each flag was thresholded from says where between the last
            //    flag on and the first flag off the threshold was actually crossed.
            double trail = last + 0.5;
            if (level && (int)level->size() == nFlags && last + 1 < nFlags) {
                const double a = (*level)[last], b = (*level)[last + 1];
                if (a > b) trail = last + a / (a - b);
            }
            ks.push_back(k);
            ms.push_back(trail * SyncStep + trailBias - syncS / 2.0);   // → the pulse's centre
        }
    };
    double A = phase, B = P0, seB = 1e9; int inliers = 0;
    // ★ Robust: fit, drop anything more than 3 robust sigmas (and at least half a pulse) off the
    //   line, refit. A noise burst that looks like a sync must not drag the whole picture with it.
    auto fit = [&]() -> bool {
        keep.assign(ks.size(), 1);
        for (int pass = 0; pass < 3; pass++) {
            double n = 0, sk = 0, sm = 0, skk = 0, skm = 0;
            for (size_t i = 0; i < ks.size(); i++) if (keep[i]) { n++; sk += ks[i]; sm += ms[i]; skk += ks[i]*ks[i]; skm += ks[i]*ms[i]; }
            const double den = n * skk - sk * sk;
            if (n < 8 || den <= 0) return false;
            B = (n * skm - sk * sm) / den; A = (sm - B * sk) / n;
            std::vector<double> r;
            for (size_t i = 0; i < ks.size(); i++) if (keep[i]) r.push_back(std::fabs(ms[i] - (A + B * ks[i])));
            std::nth_element(r.begin(), r.begin() + r.size()/2, r.end());
            const double rs = std::max(1.4826 * r[r.size()/2], (double)SyncStep / 2.0);
            const double cut = std::max(3.0 * rs, syncS * 0.5);
            inliers = 0; double ss = 0, kbar = 0, kk = 0;
            for (size_t i = 0; i < ks.size(); i++) {
                const double e = ms[i] - (A + B * ks[i]);
                keep[i] = std::fabs(e) <= cut;
                if (keep[i]) { inliers++; ss += e * e; kbar += ks[i]; }
            }
            if (inliers < 8) return false;
            kbar /= inliers;
            for (size_t i = 0; i < ks.size(); i++) if (keep[i]) kk += (ks[i] - kbar) * (ks[i] - kbar);
            const double sigma = std::sqrt(ss / std::max(1, inliers - 2));
            seB = (kk > 0) ? sigma / std::sqrt(kk) : 1e9;
        }
        return true;
    };
    bool fitted = false;
    // ★ Repeated: each pass measures around the FITTED line, which matters at the bottom of the
    //   frame when the seed rate was a little off and the windows had begun to walk off the pulse.
    for (int it = 0; it < 3; it++) { measure(A, B); fitted = fit(); if (!fitted) break; }

    // ★★★ APPLY THE RATE ONLY WHEN IT IS BOTH CONFIDENT AND REAL — the gate ca092485 asked for
    // before this was switched back on. A shear on a picture with no slant lines up the top and
    // walks the rest away, so all four must hold:
    //   • a quarter of the frame's sync lines are inliers of the fit;
    //   • the slant is beyond 30 ppm (≈ 7 px over an M1 frame — below that the eye cannot see it,
    //     and a correction can only add risk);
    //   • it is at least 4 standard errors from zero — MEASURED, not noise;
    //   • and it is inside 2000 ppm — no sound card is that far out, so beyond it the fit has locked
    //     onto something that is not the sync train.
    // Otherwise the rate stays nominal. Either way the offset is the MEDIAN over every inlier line
    // of its pulse centre less the chosen slope — taken from the whole frame, not the top line.
    const double nomB = sampleRate * m->lineTime;
    rateApplied = false; fitPpm = 0; fitPpmSE = 0;
    if (fitted) {
        fitPpm = (nomB / B - 1.0) * 1e6;                  // + = the transmitter's clock runs fast
        fitPpmSE = seB / B * 1e6;
        const bool enough = inliers >= std::max(8, syncLines / 4);
        if (enough && std::fabs(fitPpm) > 30.0 && std::fabs(fitPpm) > 4.0 * fitPpmSE && std::fabs(fitPpm) < 2000.0)
            rateApplied = true;
        if (!rateApplied) B = nomB;
        std::vector<double> a0;
        for (size_t i = 0; i < ks.size(); i++) if (keep[i]) a0.push_back(ms[i] - B * ks[i]);
        if (!a0.empty()) { std::nth_element(a0.begin(), a0.begin() + a0.size()/2, a0.end()); A = a0[a0.size()/2]; }
    } else {
        A = phase; B = nomB;
    }
    rate = B / m->lineTime;

    // ── Where the picture starts ─────────────────────────────────────────────────────────────
    // `A` is the CENTRE of a sync pulse; pixelGrid's line starts at the pulse's START — except
    // Scottie, whose pulse sits two colour channels into the line.
    std::string nm = m->name;
    const bool scottie = (nm == "Scottie S1" || nm == "Scottie S2" || nm == "Scottie DX");
    double syncInLine = 0, expect = 0;
    if (scottie) {
        syncInLine = 2.0 * m->septrTime + 2.0 * m->pixelTime * m->imgWidth;
        // ★ Scottie opens with ONE extra sync pulse before line 0, so line 0 begins a pulse late.
        expect = m->syncTime;
    }
    double startS = A - (m->syncTime / 2.0 + syncInLine) * rate;
    // ★★★ WRAP BY A WHOLE LINE, TO THE START NEAREST WHERE THE PICTURE SHOULD BEGIN (audit
    // 2026-10-04, row 5). The old `if (xMax > 350) xMax -= 350` (from slowrx) took HALF a line off a
    // position measured in 700ths of one: a start a few ms late — line 0's sync just BEFORE the
    // decode began — folded to the far end of the line and came back half a line out. The VIS
    // hand-off puts the start within about ±10 ms, so the nearest whole-line image of the measured
    // start is the right one, for every mode, Scottie's mid-line pulse included.
    startS -= std::round((startS - expect * rate) / B) * B;
    rateOut = rate;
    skipOut = (int)std::lround(startS);

    // ★★★ HOW MUCH DID THE DATA ACTUALLY SUPPORT THIS? The fraction of the frame's sync lines that
    // agreed with the fitted line. On a weak or noisy signal few pulses are found, and an offset
    // taken from them is a RANDOM SHIFT of the whole picture.
    // ★★ That is the failure Stuart hit on 2026-07-31: a Martin M2 at S4 decoded roughly aligned,
    // then "Correcting slant..." displaced the middle and bottom and made it unreadable.
    if (confOut) *confOut = (fitted && syncLines > 0) ? std::min(1.0, (double)inliers / syncLines) : 0.0;
}

// ── Top-level decoder ────────────────────────────────────────────────────────
SstvDecoder::SstvDecoder(double sr, bool autoSync_, bool adaptive_)
    : sampleRate(sr), autoSync(autoSync_), adaptive(adaptive_), pcm(16384), visWatchPcm(32768, true) {
    samps10ms = (int)(sr * 10e-3);
    accum.reserve(samps10ms * 2);
}
SstvDecoder::~SstvDecoder() {
    abort.store(true);
    if (vthread.joinable()) vthread.join();
    delete vis;
    delete visWatch;
}

void SstvDecoder::process(const int16_t* mono, int count) {
    if (!statusSent) { if (onStatus) onStatus("Waiting for signal..."); statusSent = true; }
    // Initial buffer fill.
    if (!pcm.ready()) { pcm.write(mono, count); return; }

    accum.insert(accum.end(), mono, mono + count);
    while ((int)accum.size() >= samps10ms) {
        chunk.assign(accum.begin(), accum.begin() + samps10ms);
        pcm.write(chunk.data(), samps10ms);
        accum.erase(accum.begin(), accum.begin() + samps10ms);

        if (state.load() == WaitingVIS) {
            // ★ visReset is set BEFORE the video thread stores WaitingVIS, so it is seen here.
            if (visReset.exchange(false)) { delete vis; vis = nullptr; }
            if (!vis) { vis = new SstvVIS(sampleRate); vis->onTone = [](double){}; }
            uint8_t modeIdx; int shift;
            if (vis->process(pcm, modeIdx, shift)) {
                mode = sstvModeByIndex(modeIdx); headerShift = shift;
                if (!mode || mode->unsupported) { if (onStatus) onStatus("Mode not supported"); continue; }
                if (onMode) onMode(modeIdx, mode->name);
                // ★ onImageStart is sent by the video thread once the sync train confirms the
                //   picture (2026-10-05) — a VIS on its own no longer opens a picture on the client.
                watchReset = true;
                state.store(Decoding);
                abort.store(false);
                if (vthread.joinable()) vthread.join();
                vthread = std::thread([this]{ videoThread(); });
            }
        }
        else {
            // ★★★ LISTEN FOR THE NEXT VIS WHILE DECODING (2026-10-05; the audit's "VIS during decode,
            // restart on new VIS"). Decoding used to be deaf: a picture that is abandoned, or one we
            // took for longer than it was, swallowed every VIS until its nominal end — the S2 of
            // 15:38:53 UTC on Stuart's recording began 10 s into an abandoned M2 and was lost. A second
            // detector watches its own small copy of the audio; a VIS there ends the current picture
            // (it keeps what arrived) and the video thread jumps to the new one's first sample.
            if (watchReset) {
                watchReset = false; visWatchPcm.reset(); delete visWatch; visWatch = nullptr;
            }
            visWatchPcm.write(chunk.data(), samps10ms);
            if (!visWatchPcm.ready()) continue;
            if (!visWatch) { visWatch = new SstvVIS(sampleRate); visWatch->onTone = [](double){}; }
            uint8_t modeIdx; int shift;
            if (visWatch->process(visWatchPcm, modeIdx, shift)) {
                const SstvMode* nm = sstvModeByIndex(modeIdx);
                watchReset = true;
                if (!nm || nm->unsupported) continue;
                std::lock_guard<std::mutex> lk(ctlMu);
                if (state.load() != Decoding) continue;
                // The new video starts `back` samples behind the write head — the same in both rings.
                const int back = visWatchPcm.available();
                nextStart = pcm.writtenTotal() - back;
                nextMode = nm; nextModeIdx = modeIdx; nextShift = shift;
                restartPending = true;
                interrupt.store(true);
            }
        }
    }
}

void SstvDecoder::videoThread() {
    for (;;) {
        decodePicture();
        if (abort.load()) return;
        std::lock_guard<std::mutex> lk(ctlMu);
        if (restartPending) {
            // ★ The next picture's VIS was heard while this one was still decoding (see process()):
            //   skip the window to where its video begins — the audio is all still in the ring — and
            //   go straight on. NOT a pcm.reset(): that would throw the new picture away.
            restartPending = false; interrupt.store(false);
            mode = nextMode; headerShift = nextShift;
            const long long at = 512 + pcm.consumed();
            pcm.advanceWindow((int)(nextStart - at));
            if (onMode) onMode(nextModeIdx, mode->name);
            continue;
        }
        // Reset for the next image. ★ The state is published LAST, and `vis` is not touched here: it
        // belongs to the process() thread, which is free to use it the moment it sees WaitingVIS
        // (audit 2026-10-03 — this deleted it out from under that thread).
        pcm.reset();
        visReset.store(true);
        state.store(WaitingVIS);
        return;
    }
}

void SstvDecoder::decodePicture() {
    if (onStatus) onStatus(std::string("Decoding ") + mode->name + "...");
    SstvVideo video(mode, sampleRate, headerShift, adaptive);
    auto sender = [this](int y, const uint8_t* rgb) { if (onLine) onLine(y, mode->imgWidth, rgb); };
    video.onConfirmed = [this]() { if (onImageStart) onImageStart(mode->imgWidth, mode->numLines); };
    video.demodulate(pcm, sampleRate, 0, sender, abort, &interrupt);
    if (abort.load()) return;
    if (video.endReason == SstvVideo::EndNoSync) {
        // ★ Never a picture: nothing was opened on the client, so there is nothing to close.
        if (onStatus) onStatus(std::string("ignored a ") + mode->name + " VIS — no picture followed");
        return;
    }
    if (onComplete) onComplete();
    if (video.endReason == SstvVideo::EndSignalLost && onStatus)
        onStatus("signal lost after " + std::to_string(video.linesReceived) + " lines");
    if (video.endReason == SstvVideo::EndInterrupted && onStatus)
        onStatus("cut short after " + std::to_string(video.linesReceived) + " lines \u2014 a new picture began");

    if (autoSync) {
        if (onStatus) onStatus("Correcting slant...");
        SstvSync sync(mode, sampleRate, video.syncFlags(), &video.syncLevels());
        double aRate; int aSkip; double conf = 0;
        sync.findSync(aRate, aSkip, &conf);
        // ★★★ DO NOT "CORRECT" ON EVIDENCE THIS WEAK. Below this the sync peak is indistinguishable
        // from noise and the shift is arbitrary — applying it turns a readable picture into an
        // unreadable one, which is the worst possible trade for a decoder. 0.25 = a quarter of the
        // lines contributing a clean sync edge; a genuine signal clears it easily.
        static const double MIN_SYNC_CONF = 0.25;
        if (conf < MIN_SYNC_CONF) {
            if (onStatus) onStatus("slant not corrected \u2014 sync too weak");
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
        // ★★ WHAT THE PICTURES ACTUALLY SHOW (Stuart, over an evening of live SSTV): "some come
        // through slanted, but MOST have been fine just with a tiny strip of the right side on the
        // left." So the common defect by far is a pure constant offset, and the shear is the rare
        // case — the common case must never be sheared.
        // ★★★ BUT THE SLANT IS REAL, AND IT IS NOT OURS (audit 2026-10-04, row 2). This used to say
        // an SDR's locked clock leaves nothing to correct. The slant comes from the TRANSMITTING
        // station's sound card (100–1000 ppm), which an exact receive clock records faithfully —
        // 300 ppm measured 75 px over a Martin M1 frame (test-sstv-quality). And the shear this
        // comment blamed was never applied: findSync's Hough could not return anything but the
        // nominal rate until 2026-10-04 (see the note there).
        // ★ So `aRate` IS applied now — but only through findSync's gate: a line fit with a quarter
        //   of the frame's lines behind it, |ε| > 30 ppm, more than 4 standard errors from zero and
        //   under 2000 ppm. Anything less and findSync returns the nominal rate, so the common
        //   offset-only picture is shifted, never sheared — exactly as before.
        bool ok = false;
        auto pixels = video.redrawFromLuminance(aRate, aSkip, &ok);
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
            // ★ Say when a SHEAR was applied, with its size: it is the rarer, riskier correction,
            //   and the number is what tells a real sound-card slant from a fit that went wrong.
            if (sync.rateApplied && !partialOk && onStatus) {
                char b[96];
                std::snprintf(b, sizeof b, "slant corrected — sender's clock %+.0f ppm", sync.fitPpm);
                onStatus(b);
            }
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

}

} // namespace vibe
