// test-drm.cpp — the DRM receiver (decoders/drm/) against the published standard, synthetic signals and real recordings.
//
//   1. The standard's own tables: cell counts per mode/occupancy (tables 25, 41–44, the 65 FAC cells), SDC lengths (21),
//      the PRBS's first 16 bits (table 26).
//   2. The coding layer in the cell domain: every MSC protection level, 16- and 64-QAM, EEP and UEP, at several SNRs.
//   3. End to end through the test transmitter and annex B's channels: every robustness mode, AWGN SNR sweeps, the
//      multipath channels, ±1.2 kHz tuning error, ±30 ppm clock — every logical frame byte-checked, the label, the text.
//   4. False locks: ten minutes of noise and an AM broadcast must give NOTHING (no lock, no FAC, no frame).
//   5. Audio framing: xHE-AAC super frames with known access units (including a border delayed into the next super
//      frame), AAC super frames, the DRM → standard USAC config, LOAS.
//   6. Real recordings (VIBE_TEST_AUDIO=<folder holding drm/>): Stuart's UberSDR, RRI on 9720 kHz (2026-10-26) — mode,
//      occupancy, QAM and the FAC must be read. NOT RUN (exit 3 for that part only) when the folder is absent.
//   7. CPU per DRM listener, measured on this machine.
#include "decoders/drm/drm_audio.h"
#include "decoders/drm/drm_receiver.h"
#include "decoders/drm/drm_tx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace vibedrm;

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++fails; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct RunResult { int64_t good = 0, bad = 0; bool locked = false; std::string label, text; RxStatus st; double secs = 0; double dspSecs = 0; };

static RunResult runChain(TxConfig c, int channel, double snr, double fo, double ppm, int superFrames, uint32_t seed = 7) {
    Transmitter tx(c);
    int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
    Channel ch(channel, snr, fo, ppm, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, seed);
    Receiver rx;
    RunResult r;
    rx.onLogicalFrame = [&](const LogicalFrame& lf) {
        if (lf.bytes.size() < 8) { ++r.bad; return; }
        const int64_t n = (int64_t(lf.bytes[0]) << 24) | (lf.bytes[1] << 16) | (lf.bytes[2] << 8) | lf.bytes[3];
        const auto ref = testPayload(n, lf.bytes.size() - 4);
        if (std::equal(ref.begin(), ref.end(), lf.bytes.begin())) ++r.good; else ++r.bad;
    };
    for (int sf = 0; sf < superFrames; ++sf) {
        const auto x = tx.superFrame();
        const auto y = ch.run(x);
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(y.data(), int(y.size()), 24000);
        r.dspSecs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.secs += double(y.size()) / kFs;
    }
    r.st = rx.status(); r.locked = r.st.locked; r.label = r.st.sdc.svc[0].label; r.text = r.st.text;
    return r;
}

static void tables() {
    std::printf("── 1. the standard's tables\n");
    for (int m = 0; m < 4; ++m)
        for (int so = 0; so < 4; ++so) {
            CellMap c;
            if (!c.build(Mode(m), so)) continue;
            CHECK(c.nFac == 65, "mode %c SO%d: %d FAC cells", modeLetter(Mode(m)), so, c.nFac);
            CHECK(c.nSdc == sdcCellsTable(Mode(m), so), "mode %c SO%d: %d SDC cells, table 25 says %d", modeLetter(Mode(m)), so, c.nSdc, sdcCellsTable(Mode(m), so));
            CHECK(c.nMux == muxCellsTable(Mode(m), so), "mode %c SO%d: %d MSC cells/mux frame, tables 41-44 say %d", modeLetter(Mode(m)), so, c.nMux, muxCellsTable(Mode(m), so));
            for (int lv = 1; lv <= 2; ++lv) {
                MlcLayout L; L.buildSdc(lv, c.nSdc);
                const int want = 4 + 8 * sdcDataBytesTable(Mode(m), lv == 2 ? 0 : 1, so) + 16;
                CHECK(L.l2 >= want && L.l2 - want < 8, "mode %c SO%d SDC %d-QAM: L=%d bits vs table 21's %d (+ <8 padding)",
                      modeLetter(Mode(m)), so, lv == 2 ? 16 : 4, L.l2, want);
            }
        }
    uint8_t z[16] = {0}; prbs(z, 16);
    const uint8_t want[16] = {0,0,0,0,0,1,1,1,1,0,1,1,1,1,1,0};
    CHECK(std::memcmp(z, want, 16) == 0, "PRBS first 16 bits differ from table 26");
    MlcLayout F; F.buildFac();
    CHECK(F.sch[0].outBits == 130, "FAC coded length %zu, want 130", F.sch[0].outBits);
    std::printf("   %d checks\n", checks);
}

static void coding() {
    std::printf("── 2. the coding layer (cell domain, AWGN, 10 blocks each)\n");
    std::mt19937 rng(1); std::normal_distribution<float> g(0, 1);
    for (int lv : {2, 3})
        for (int prot = 0; prot < (lv == 3 ? 4 : 2); ++prot)
            for (int lenA : {0, 40}) {
                MlcLayout L;
                if (!L.buildMsc(lv, 2337, prot > 0 ? prot - 1 : 0, prot, lenA)) { CHECK(false, "layout %d-QAM prot %d", 1 << (2 * lv), prot); continue; }
                std::printf("   %2d-QAM prot %d %s:", 1 << (2 * lv), prot, lenA ? "UEP" : "EEP");
                for (float snr : {10.f, 14.f, 18.f, 22.f, 26.f}) {
                    int blk = 0;
                    for (int t = 0; t < 10; ++t) {
                        std::vector<uint8_t> u(size_t(L.l1 + L.l2)); for (auto& b : u) b = uint8_t(rng() & 1);
                        auto c = mlcEncode(u, L);
                        const float sig = std::pow(10.f, -snr / 20.f) / std::sqrt(2.f);
                        std::vector<float> w(c.size(), 1.f);
                        for (auto& v : c) v += std::complex<float>(g(rng) * sig, g(rng) * sig);
                        MlcDecoder d; auto r = d.decode(c.data(), w.data(), L, 2);
                        blk += r != u;
                    }
                    std::printf(" %2.0fdB:%2d/10", snr, 10 - blk);
                    // ★ the standard's own simulated figure for 64-QAM R=0.6 is 14.9 dB (annex A, ideal channel knowledge);
                    //   at 22 dB every level must be clean
                    if (snr >= 22) CHECK(blk == 0, "%d-QAM prot %d at %.0f dB: %d/10 blocks wrong", 1 << (2 * lv), prot, snr, blk);
                }
                std::printf("\n");
            }
}

static void endToEnd() {
    std::printf("── 3. end to end (transmitter → annex B channel → receiver; every frame byte-checked)\n");
    // every mode, clean
    for (int m = 0; m < 4; ++m) {
        TxConfig c; c.mode = Mode(m); c.mscLevels = 3; c.protB = 1; c.sdcLevels = m == 3 ? 2 : 1;
        const RunResult r = runChain(c, 1, 30, 0, 0, 20);
        std::printf("   mode %c 64-QAM AWGN 30 dB: locked %d, frames %lld good / %lld bad, label '%s', text '%s', FAC MER %.1f dB, MSC MER %.1f dB\n",
                    modeLetter(Mode(m)), r.locked, (long long)r.good, (long long)r.bad, r.label.c_str(), r.text.c_str(), r.st.facMerDb, r.st.mscMerDb);
        CHECK(r.locked && r.good >= 40 && r.bad == 0, "mode %c clean: %lld good %lld bad", modeLetter(Mode(m)), (long long)r.good, (long long)r.bad);
        CHECK(r.label == "VIBE TEST", "mode %c label '%s'", modeLetter(Mode(m)), r.label.c_str());
        CHECK(r.text == "Hello from the VibeSDR DRM test transmitter", "mode %c text '%s'", modeLetter(Mode(m)), r.text.c_str());
        CHECK(r.st.mode == Mode(m) && r.st.so == 3 && r.st.mscLevels == 3, "mode %c: status says %c SO%d", modeLetter(Mode(m)), modeLetter(r.st.mode), r.st.so);
        CHECK(r.st.sdc.svc[0].lang3 == "eng" && r.st.sdc.svc[0].country2 == "gb", "mode %c: language/country", modeLetter(Mode(m)));
        CHECK(r.st.fac.serviceId == 0xABCDEF && r.st.fac.language == 5 && r.st.fac.descriptor == 9, "mode %c: FAC service fields", modeLetter(Mode(m)));
    }
    // ★ SNR sweeps — the decode rate is the fraction of logical frames correct after lock (40 s each)
    struct Sw { Mode m; int lv; int prot; int ch; };
    const Sw sweeps[] = { {Mode::B, 3, 1, 1}, {Mode::B, 2, 1, 1}, {Mode::A, 3, 1, 2}, {Mode::B, 3, 1, 3}, {Mode::B, 3, 1, 4}, {Mode::B, 3, 1, 5}, {Mode::D, 2, 0, 5} };
    for (const Sw& s : sweeps) {
        std::printf("   mode %c %d-QAM prot %d, channel %d:", modeLetter(s.m), 1 << (2 * s.lv), s.prot, s.ch);
        for (double snr : {10.0, 14.0, 18.0, 22.0, 26.0, 30.0}) {
            TxConfig c; c.mode = s.m; c.mscLevels = s.lv; c.protB = s.prot; c.sdcLevels = s.m == Mode::D ? 2 : 1;
            const RunResult r = runChain(c, s.ch, snr, 0, 0, 34, 11);
            const double rate = r.good + r.bad > 0 ? double(r.good) / double(r.good + r.bad) : 0.0;
            std::printf(" %2.0f:%3.0f%%", snr, 100.0 * rate * (r.good + r.bad) / 96.0);
            if (s.ch == 1 && s.lv == 3 && snr >= 22) CHECK(r.bad == 0 && r.good > 80, "AWGN %.0f dB: %lld good %lld bad", snr, (long long)r.good, (long long)r.bad);
            if (s.ch == 1 && s.lv == 2 && snr >= 18) CHECK(r.bad == 0 && r.good > 80, "16-QAM AWGN %.0f dB: %lld good %lld bad", snr, (long long)r.good, (long long)r.bad);
        }
        std::printf("  (%% of the 96 frames sent)\n");
    }
    // tuning and clock errors
    for (double fo : {-1200.0, -333.0, 230.0, 1150.0})
        for (double ppm : {-30.0, 0.0, 30.0}) {
            TxConfig c; c.mode = Mode::B; c.mscLevels = 3; c.protB = 1;
            const RunResult r = runChain(c, 1, 30, fo, ppm, 20);
            CHECK(r.locked && r.bad == 0 && r.good >= 40 && std::abs(r.st.freqOffsetHz - fo) < 2.0,
                  "offset %.0f Hz clock %.0f ppm: locked %d, %lld good %lld bad, offset read %.2f Hz", fo, ppm, r.locked, (long long)r.good, (long long)r.bad, r.st.freqOffsetHz);
            if (ppm != 0) CHECK(std::abs(r.st.sroPpm + ppm) < 6.0 || std::abs(r.st.sroPpm - ppm) < 6.0, "clock %.0f ppm read as %.1f", ppm, r.st.sroPpm);
        }
    std::printf("   tuning ±1.2 kHz × clock ±30 ppm: done\n");
}

static void falseLocks() {
    std::printf("── 4. false locks\n");
    std::mt19937 rng(99); std::normal_distribution<float> g(0, 1);
    Receiver rx; int64_t frames = 0; bool everFac = false, everLock = false;
    rx.onLogicalFrame = [&](const LogicalFrame&) { ++frames; };
    std::vector<cf> x(24000);
    for (int s = 0; s < 600; ++s) {
        for (auto& v : x) v = cf(g(rng), g(rng));
        rx.feed(x.data(), int(x.size()), 24000);
        everFac |= rx.status().facGood > 0; everLock |= rx.status().locked;
    }
    std::printf("   10 min of noise: FAC passes %lld, locked %d, frames %lld\n", (long long)rx.status().facGood, everLock, (long long)frames);
    CHECK(!everLock && !everFac && frames == 0, "noise produced a station");
    // an AM broadcast: carrier + noise-like audio, 4.5 kHz
    Receiver ra; int64_t fa = 0; bool lockA = false;
    ra.onLogicalFrame = [&](const LogicalFrame&) { ++fa; };
    float lp = 0; double ph = 0;
    for (int s = 0; s < 120; ++s) {
        for (auto& v : x) { lp = 0.9f * lp + 0.1f * g(rng); ph += 2 * M_PI * 37.0 / kFs; v = cf(1.0f + 0.8f * lp, 0) * std::polar(1.0f, float(ph)) + cf(0.01f * g(rng), 0.01f * g(rng)); }
        ra.feed(x.data(), int(x.size()), 24000);
        lockA |= ra.status().locked;
    }
    std::printf("   2 min of AM broadcast: FAC passes %lld, locked %d, frames %lld\n", (long long)ra.status().facGood, lockA, (long long)fa);
    CHECK(!lockA && fa == 0 && ra.status().facGood == 0, "an AM signal produced a station");
}

static void audio() {
    std::printf("── 5. audio framing\n");
    // xHE-AAC: three access units with CRCs, the second super frame carrying a border delayed to 0xFFF
    auto frameOf = [](int n, uint8_t seed) { std::vector<uint8_t> f(static_cast<size_t>(n)); for (int i = 0; i < n; ++i) f[size_t(i)] = uint8_t(seed + i * 7);
        const uint32_t c = crcBytes(f.data(), f.size(), kCrc16Poly, 16); f.push_back(uint8_t(c >> 8)); f.push_back(uint8_t(c)); return f; };
    const auto a1 = frameOf(30, 1), a2 = frameOf(41, 2), a3 = frameOf(25, 3);
    std::vector<uint8_t> stream; stream.insert(stream.end(), a1.begin(), a1.end()); stream.insert(stream.end(), a2.begin(), a2.end()); stream.insert(stream.end(), a3.begin(), a3.end());
    // super frame 1: payload = first 50 bytes (a1 starts at 0, a2 at 32); super frame 2: the rest (a3 starts at 75 → index 25)
    auto superFrame = [](const std::vector<uint8_t>& pay, const std::vector<int>& borders) {
        std::vector<uint8_t> sf; const int b = int(borders.size());
        const uint8_t h = uint8_t((b << 4) | 3); sf.push_back(h);
        std::vector<uint8_t> hb; putBits(hb, h, 8); sf.push_back(uint8_t(crcBits(hb.data(), 8, kCrc8Poly, 8)));
        sf.insert(sf.end(), pay.begin(), pay.end());
        for (int i = b - 1; i >= 0; --i) { sf.push_back(uint8_t(borders[size_t(i)] >> 4)); sf.push_back(uint8_t(((borders[size_t(i)] & 15) << 4) | b)); }
        return sf;
    };
    AudioInfo ai; ai.present = true; ai.coding = 3; ai.srCode = 4; ai.audioMode = 0; ai.config = {0x80};   // index 2+1 (2:1 SBR)…
    AudioFramer fr; fr.configure(ai);
    std::vector<AccessUnit> out;
    fr.push(superFrame(std::vector<uint8_t>(stream.begin(), stream.begin() + 50), {0, 32}).data(), 50 + 2 + 4, out);
    fr.push(superFrame(std::vector<uint8_t>(stream.begin() + 50, stream.end()), {25}).data(), size_t(stream.size() - 50 + 2 + 2), out);
    // a1 completes at the second border; a2 completes when a3's border arrives
    CHECK(out.size() == 2 && out[0].crcOk && out[1].crcOk && out[0].data.size() == 30 && out[1].data.size() == 41,
          "xHE-AAC framing: %zu units", out.size());
    // a border in the previous super frame's last byte (0xFFF)
    {
        AudioFramer f2; f2.configure(ai); std::vector<AccessUnit> o2;
        const auto b1 = frameOf(20, 9), b2 = frameOf(20, 10);
        std::vector<uint8_t> p1(b1.begin(), b1.end()); p1.push_back(b2[0]);         // b2 begins on p1's last byte
        f2.push(superFrame(p1, {0}).data(), p1.size() + 4, o2);
        std::vector<uint8_t> p2(b2.begin() + 1, b2.end());
        const auto sf2 = superFrame(p2, {0xFFF, int(p2.size())});
        // ★ the delayed border's element is the FIRST directory element (located at the end) — superFrame() writes the
        //   borders in reverse, so {0xFFF, …} puts 0xFFF last in the directory, i.e. first in payload order. Correct.
        f2.push(sf2.data(), sf2.size(), o2);
        CHECK(o2.size() == 2 && o2[0].crcOk && o2[1].crcOk && o2[1].data.size() == 20, "xHE-AAC delayed border: %zu units", o2.size());
    }
    // AAC: 10 frames at 24 kHz
    {
        AudioInfo aa; aa.present = true; aa.coding = 0; aa.srCode = 3; aa.sbr = 1;
        AudioFramer f3; f3.configure(aa); std::vector<AccessUnit> o3;
        std::vector<uint8_t> sf; BitWriter w; int pos = 0; std::vector<int> lens = {20, 25, 18, 30, 22, 21, 19, 27, 24, 26};
        for (int i = 0; i < 9; ++i) { pos += lens[size_t(i)]; w.put(uint32_t(pos), 12); }
        w.put(0, 4);
        sf = w.b; for (int i = 0; i < 10; ++i) sf.push_back(0xAA);
        for (int i = 0; i < 10; ++i) for (int j = 0; j < lens[size_t(i)]; ++j) sf.push_back(uint8_t(i));
        f3.push(sf.data(), sf.size(), o3);
        bool ok = o3.size() == 10;
        for (size_t i = 0; ok && i < 10; ++i) ok = o3[i].data.size() == size_t(lens[i]) && o3[i].data[0] == uint8_t(i);
        CHECK(ok, "AAC framing: %zu frames", o3.size());
    }
    // the DRM static config → UsacConfig: mono, coreSbrFrameLengthIndexDrm=2 (→ 3, 2:1 SBR), noiseFilling 1, SbrConfig with
    // no extras, no extension elements
    {
        BitWriter w; w.put(2, 2); w.put(1, 1); w.put(0, 3); w.put(5, 4); w.put(9, 4); w.put(0, 2); w.escaped(0, 2, 4, 8); w.put(0, 1); w.align();
        AudioInfo x; x.present = true; x.coding = 3; x.srCode = 7; x.audioMode = 0; x.config = w.b;
        const UsacInfo u = usacFromDrm(x, 48000);
        BitWriter e; e.put(31, 5); e.put(10, 6); e.put(3, 4); e.put(1, 4);       // AOT 42, 48 kHz index 3, mono
        e.put(3, 5); e.put(3, 3); e.put(1, 5); e.escaped(0, 4, 8, 16);           // UsacConfig: sf index, coreSbr 3, mono, 1 element
        e.put(0, 2); e.put(0, 1); e.put(1, 1); e.put(0, 3); e.put(5, 4); e.put(9, 4); e.put(0, 2); e.put(0, 1);
        CHECK(u.ok && u.ascBits == size_t(e.nbits) && u.asc == (e.align(), e.b) && u.sbrRatioIndex == 3 && u.outputFrameLength == 2048,
              "DRM → USAC config (%s, %zu bits vs %d)", u.why.c_str(), u.ascBits, e.nbits);
    }
    std::printf("   framing checks done\n");
}

static int realRecordings() {
    const char* dir = getenv("VIBE_TEST_AUDIO");
    std::printf("── 6. real recordings\n");
    if (!dir) { std::printf("   NOT RUN — VIBE_TEST_AUDIO is not set (needs drm/uber_rri_9720000_iq48.wav)\n"); return 3; }
    const std::string path = std::string(dir) + "/drm/uber_rri_9720000_iq48.wav";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("   NOT RUN — %s missing\n", path.c_str()); return 3; }
    uint8_t h[44]; if (std::fread(h, 1, 44, f) != 44) { std::fclose(f); return 3; }
    int rate; std::memcpy(&rate, h + 24, 4);
    Receiver rx;
    std::vector<int16_t> b(9600); std::vector<cf> x(4800); size_t n;
    const auto t0 = std::chrono::steady_clock::now(); double secs = 0;
    while ((n = std::fread(b.data(), 4, 4800, f)) > 0) {
        for (size_t i = 0; i < n; ++i) x[i] = cf(b[2 * i], b[2 * i + 1]);
        rx.feed(x.data(), int(n), rate); secs += double(n) / rate;
    }
    std::fclose(f);
    const double dsp = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const RxStatus& s = rx.status();
    std::printf("   RRI 9720 kHz (%.0f s): mode %c SO%d MSC %d-QAM SDC %d-QAM, FAC %lld good / %lld bad, SDC %lld/%lld, FAC MER %.1f dB, "
                "offset %.1f Hz, blanked %.0f %% (a 40 Hz pulsed interferer), %.1f %% of a core\n",
                secs, modeLetter(s.mode), s.so, 1 << (2 * s.mscLevels), 1 << (2 * s.sdcLevels), (long long)s.facGood, (long long)s.facBad,
                (long long)s.sdcGood, (long long)(s.sdcGood + s.sdcBad), s.facMerDb, s.freqOffsetHz, 100 * rx.blankedFraction(), 100 * dsp / secs);
    CHECK(s.mode == Mode::B && s.so == 3 && s.mscLevels == 2 && s.facGood > 200, "RRI 9720: mode/SO/QAM/FAC");
    return 0;
}

static void cpu() {
    std::printf("── 7. CPU per DRM listener (this machine)\n");
    for (int m = 0; m < 4; ++m) {
        TxConfig c; c.mode = Mode(m); c.mscLevels = 3; c.protB = 1; c.sdcLevels = m == 3 ? 2 : 1;
        const RunResult r = runChain(c, 1, 25, 0, 0, 25);
        std::printf("   mode %c 64-QAM: %.2f %% of one core (%.2f s DSP for %.0f s of signal)\n", modeLetter(Mode(m)), 100 * r.dspSecs / r.secs, r.dspSecs, r.secs);
    }
}

int main() {
    tables();
    coding();
    endToEnd();
    falseLocks();
    audio();
    const int real = realRecordings();
    cpu();
    std::printf("\n%d checks, %d failed%s\n", checks, fails, real == 3 ? " (real recordings NOT RUN)" : "");
    return fails ? 1 : 0;
}
