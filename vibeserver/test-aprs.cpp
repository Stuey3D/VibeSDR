// test-aprs.cpp — V12 APRS (vibe_hdlc.h, vibe_aprs.h, vibe_aprs_run.h), docs/v12/AIS-APRS.md.
//
//  1. AX.25: addresses, SSIDs, the has-been-repeated mark, TNC2, a non-UI frame refused.
//  2. The APRS formats against the WORKED EXAMPLES IN APRS 1.0.1 (uncompressed and compressed positions, weather,
//     objects, items, messages, status, altitude, the Mic-E destination "S32U6T"), plus Mic-E, NMEA and third-party
//     packets encoded from the spec and decoded back.
//  3. The modem on synthetic audio: Bell 202 1200 Bd at several SNRs and with the two tones 6 dB apart either way
//     (pre-/de-emphasis); 300 Bd HF at −50 / +30 Hz mistuning.
//  4. Noise only, ten minutes of audio per modem: ZERO frames.
//  5. The run: the duplicate rule, digipeater paths, the station list and its JSON, messages.
//  6. CPU per audio second (this Mac).
#include "vibe_aprs_run.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ── an AX.25 UI frame ────────────────────────────────────────────────────────────────────────────────────────────────
static void addr(std::vector<uint8_t>& b, const std::string& call, int ssid, bool last, bool h = false) {
    for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < int(call.size()) ? call[size_t(i)] : ' ') << 1));
    b.push_back(uint8_t(0x60 | (ssid << 1) | (last ? 1 : 0) | (h ? 0x80 : 0)));
}
static std::vector<uint8_t> ui(const std::string& src, int ssid, const std::string& dst, const std::vector<std::pair<std::string, bool>>& digis, const std::string& info) {
    std::vector<uint8_t> b;
    addr(b, dst, 0, false);
    addr(b, src, ssid, digis.empty());
    for (size_t i = 0; i < digis.size(); ++i) {
        std::string c = digis[i].first; int s = 0;
        const size_t d = c.find('-'); if (d != std::string::npos) { s = std::atoi(c.c_str() + d + 1); c.resize(d); }
        addr(b, c, s, i + 1 == digis.size(), digis[i].second);
    }
    b.push_back(0x03); b.push_back(0xF0);
    b.insert(b.end(), info.begin(), info.end());
    return b;
}
static vibeaprs::Packet parse(const std::string& info, const std::string& dest = "APRS") {
    vibeaprs::Ax25 f; vibeaprs::parseAx25(ui("M0ABC", 9, dest, {}, info), f);
    return vibeaprs::parsePacket(f);
}

// ── the AFSK modulator ──────────────────────────────────────────────────────────────────────────────────────────────
struct Rng {
    uint64_t s; explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 7) {}
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double(s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(2 * M_PI * u()); }
};
/** Frame bytes → audio at 48 kHz: `flags` 0x7E first (TXDELAY), the stuffed frame + FCS, two closing flags; NRZI; a
 *  continuous-phase AFSK. `spaceDb` sets the space tone's level against the mark (pre-/de-emphasis). */
static void modulate(std::vector<float>& out, const std::vector<uint8_t>& frame, int baud, double mark, double space, double spaceDb,
                     double amp, int flags = 30, double offHz = 0) {
    std::vector<uint8_t> f = frame; vibehdlc::appendFcs(f);
    std::vector<int> bits;
    for (int i = 0; i < flags; ++i) vibehdlc::flagBits(bits);
    vibehdlc::stuffBits(f, bits);
    vibehdlc::flagBits(bits); vibehdlc::flagBits(bits);
    std::vector<int> lv; int level = 1; vibehdlc::nrzi(bits, lv, level);
    const double fs = 48000, spb = fs / baud, sg = std::pow(10.0, spaceDb / 20);
    static double ph = 0;
    double t = 0;
    for (size_t k = 0; k < lv.size(); ++k) {
        const double hz = (lv[k] ? mark : space) + offHz, a = amp * (lv[k] ? 1.0 : sg);
        const double end = (k + 1) * spb;
        for (; t < end; t += 1) { ph += 2 * M_PI * hz / fs; out.push_back(float(a * std::sin(ph))); }
    }
}

static void testAx25() {
    std::printf("── AX.25 ──\n");
    vibeaprs::Ax25 f;
    CHECK(vibeaprs::parseAx25(ui("M0ABC", 9, "APRS", {{"MB7UH", true}, {"WIDE2-1", false}}, "!5130.00N/00010.00W>hello"), f), "a UI frame");
    CHECK(f.src.call == "M0ABC" && f.src.ssid == 9 && f.dest.call == "APRS" && f.digis.size() == 2 && f.digis[0].h && !f.digis[1].h && f.digis[1].ssid == 1,
          "addresses, SSIDs, the H bit");
    CHECK(vibeaprs::tnc2(f) == "M0ABC-9>APRS,MB7UH*,WIDE2-1:!5130.00N/00010.00W>hello", "TNC2: %s", vibeaprs::tnc2(f).c_str());
    std::vector<uint8_t> b = ui("M0ABC", 0, "APRS", {}, "x"); b[14] = 0x3F;   // an I frame
    CHECK(!vibeaprs::parseAx25(b, f), "not a UI frame: refused");
    b = ui("M0ABC", 0, "APRS", {}, "x"); b[2] = 'a' << 1;
    CHECK(!vibeaprs::parseAx25(b, f), "a lower-case address: refused");
}

static void testFormats() {
    std::printf("── APRS 1.0.1 worked examples ──\n");
    vibeaprs::Packet p = parse("!4903.50N/07201.75W-Test 001234");
    CHECK(p.kind == "position" && std::fabs(p.lat - 49.058333) < 1e-5 && std::fabs(p.lon + 72.029167) < 1e-5 && p.symTable == '/' && p.symCode == '-'
          && p.comment == "Test 001234", "uncompressed position (%s %.6f %.6f '%s')", p.kind.c_str(), p.lat, p.lon, p.comment.c_str());
    p = parse("=4903.50N/07201.75W#PHG5132");
    CHECK(p.messaging && p.phgPower == 5 && p.phgHeight == 1 && p.phgGain == 3 && p.phgDir == 2 && p.symCode == '#', "PHG");
    p = parse("!49  .  N/072  .  W-");
    CHECK(p.hasPos && p.ambiguity == 4 && std::fabs(p.lat - 49.5) < 1e-9 && std::fabs(p.lon + 72.5) < 1e-9, "ambiguity 4 (%.4f %.4f)", p.lat, p.lon);
    p = parse("=/5L!!<*e7>7P[");
    CHECK(p.hasPos && std::fabs(p.lat - 49.5) < 1e-4 && std::fabs(p.lon + 72.75) < 1e-4 && p.symCode == '>', "compressed 49.5 −72.75 (%.5f %.5f)", p.lat, p.lon);
    CHECK(std::fabs(p.course - 88) < 1e-9 && std::fabs(p.speedKn - (std::pow(1.08, 'P' - 33) - 1)) < 1e-6, "compressed course/speed (%.0f %.2f)", p.course, p.speedKn);
    p = parse("_10090556c220s004g005t077r000p000P000h50b09900wRSW");
    CHECK(p.kind == "weather" && p.wx.windDir == 220 && p.wx.windMph == 4 && p.wx.gustMph == 5 && p.wx.tempF == 77 && p.wx.hum == 50
          && std::fabs(p.wx.baroMbar - 990.0) < 1e-9, "positionless weather");
    p = parse("@092345z4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900wRSW");
    CHECK(p.kind == "weather" && p.hasPos && p.wx.windDir == 220 && p.wx.windMph == 4 && p.wx.tempF == 77, "weather with a position");
    p = parse(";LEADER   *092345z4903.50N/07201.75W>088/036");
    CHECK(p.kind == "object" && p.name == "LEADER" && !p.killed && p.course == 88 && p.speedKn == 36, "object (%s '%s')", p.kind.c_str(), p.name.c_str());
    p = parse(")AID #2!4903.50N/07201.75WA");
    CHECK(p.kind == "item" && p.name == "AID #2" && p.symCode == 'A', "item '%s'", p.name.c_str());
    p = parse(":WU2Z     :Testing{003");
    CHECK(p.kind == "message" && p.to == "WU2Z" && p.text == "Testing" && p.msgId == "003", "message");
    p = parse(":KB2ICI-14:ack003");
    CHECK(p.kind == "ack" && p.to == "KB2ICI-14" && p.msgId == "003", "ack");
    p = parse(":BLN1     :Snow expected in Tampa RSN");
    CHECK(p.kind == "bulletin" && p.text == "Snow expected in Tampa RSN", "bulletin");
    p = parse(">Net Control Center without timestamp");
    CHECK(p.kind == "status" && p.status == "Net Control Center without timestamp", "status");
    p = parse("!4903.50N/07201.75W>/A=001234 hi");
    CHECK(p.altFt == 1234 && p.comment == "hi", "altitude in the comment (%.0f '%s')", p.altFt, p.comment.c_str());
    p = parse("!4903.50N/07201.75W>!W12!");
    CHECK(std::fabs(p.lat - (49 + 3.501 / 60)) < 1e-7 && std::fabs(p.lon + (72 + 1.752 / 60)) < 1e-7, "!DAO! precision");
    p = parse("$GPRMC,063909,A,3349.4302,N,11700.3721,W,43.022,89.3,291099,13.1,E*52");
    CHECK(p.kind == "nmea" && std::fabs(p.lat - (33 + 49.4302 / 60)) < 1e-6 && std::fabs(p.lon + (117 + 0.3721 / 60)) < 1e-6 && std::fabs(p.speedKn - 43.022) < 1e-6, "NMEA RMC");
    p = parse("}W1ABC>APRS,TCPIP,WIDE:!4903.50N/07201.75W-");
    CHECK(p.from == "W1ABC" && p.hasPos && p.comment.find("via M0ABC-9") != std::string::npos, "third party ('%s', '%s')", p.from.c_str(), p.comment.c_str());

    std::printf("── Mic-E (APRS 1.0.1 ch. 10) ──\n");
    // the spec's destination example: S32U6T → 33 25.64 N, W, message bits 1 0 0 = "Returning"
    // info: lon 112 07.74 W needs the +100 offset → 4th dest char selects; here '6' is no offset, so a lon of 12 07.74
    std::string info = "`";
    info += char(12 + 28); info += char(7 + 28); info += char(74 + 28);
    // speed 20 kn, course 251: SP = 2, DC = 0*10 + 2 (course hundreds), SE = 51
    info += char(2 + 28); info += char(0 * 10 + 2 + 28); info += char(51 + 28);
    info += ">/";            // symbol car, primary table
    info += "]=";            // a device mark, then the end
    p = parse(info, "S32U6T");
    CHECK(p.kind == "mice" && std::fabs(p.lat - (33 + 25.64 / 60)) < 1e-6 && std::fabs(p.lon + (12 + 7.74 / 60)) < 1e-6, "Mic-E position (%.6f %.6f)", p.lat, p.lon);
    CHECK(p.micE == "Returning" && p.speedKn == 20 && p.course == 251 && p.symCode == '>' && p.symTable == '/', "Mic-E message/speed/course ('%s' %.0f %.0f)", p.micE.c_str(), p.speedKn, p.course);
    // the +100 longitude offset and altitude "xxx}"
    std::string info2 = "`";
    info2 += char(12 + 28); info2 += char(7 + 28); info2 += char(74 + 28);
    info2 += char(0 + 28); info2 += char(0 + 28); info2 += char(0 + 28);
    info2 += ">/";
    { const int alt = 10000 + 120; info2 += char(33 + alt / 8281); info2 += char(33 + (alt / 91) % 91); info2 += char(33 + alt % 91); info2 += "}"; }
    info2 += "going home";
    p = parse(info2, "S32UPT");
    CHECK(std::fabs(p.lon + (112 + 7.74 / 60)) < 1e-6 && std::fabs(p.altFt - 120 * 3.28084) < 0.01 && p.comment == "going home",
          "Mic-E +100° offset, altitude, comment (%.5f %.1f '%s')", p.lon, p.altFt, p.comment.c_str());
    std::string line = vibeaprs::readable(parse(info, "S32U6T"));
    CHECK(line.find("M0ABC-9 \xc2\xb7 car \xc2\xb7 37 km/h W") == 0 && line.find("Mic-E Returning") != std::string::npos, "readable: %s", line.c_str());
    CHECK(std::fabs(vibeaprs::regionalHz(51.5, -0.1) - 144.800e6) < 1 && std::fabs(vibeaprs::regionalHz(40, -100) - 144.390e6) < 1
          && std::fabs(vibeaprs::regionalHz(-33.9, 151.2) - 145.175e6) < 1, "the region's frequency");
}

struct Res { int sent = 0, heard = 0, wrong = 0, repaired = 0; double cpu = 0, secs = 0; };
static Res modemRun(int baud, double snrDb, double spaceDb, int n, uint64_t seed, double offHz = 0, bool single = false) {
    Res r;
    std::vector<float> a;
    std::vector<std::vector<uint8_t>> sent;
    const double mark = baud == 300 ? 1600 : 1200, space = baud == 300 ? 1800 : 2200, amp = 0.3;
    Rng rng(seed);
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 4800; ++k) a.push_back(0);
        char info[96];
        std::snprintf(info, sizeof info, "!%02d%02d.%02dN/00%d%02d.%02dW>packet %d of the modem test %llu", 50 + i % 5, i % 60, (i * 7) % 100, i % 9, (i * 3) % 60, i % 100, i, (unsigned long long)seed);
        auto f = ui("G4ABC", i % 16, "APRS", {{"WIDE1-1", false}}, info);
        modulate(a, f, baud, mark, space, spaceDb, amp, baud == 300 ? 12 : 30, offHz);
        sent.push_back(f);
    }
    for (int k = 0; k < 4800; ++k) a.push_back(0);
    // SNR in the modem's band: 1200 Bd → 3 kHz (300–3300 Hz); 300 Bd → 500 Hz. Noise is white at 48 kHz.
    const double bw = baud == 300 ? 500 : 3000, sigPow = amp * amp / 2;
    const double nPow = sigPow / std::pow(10, snrDb / 10) * (24000 / bw);
    for (auto& x : a) x += float(std::sqrt(nPow) * rng.g());
    vibeaprs::AfskDemod d(48000, baud, single);
    std::vector<std::vector<uint8_t>> got;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o < a.size(); o += 4800)
        d.feed(a.data() + o, int(std::min<size_t>(4800, a.size() - o)), [&](const vibeaprs::Frame& f) { got.push_back(f.bytes); if (f.repaired) ++r.repaired; });
    r.cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.secs = a.size() / 48000.0;
    r.sent = n;
    for (auto& g : got) { bool m = false; for (auto& s : sent) if (s == g) m = true; if (m) ++r.heard; else ++r.wrong; }
    return r;
}

static void testModem() {
    std::printf("── the modem: Bell 202, 1200 Bd ──\n");
    vibeaprs::AfskDemod d(48000, 1200);
    std::printf("   %d slicers at %.0f S/s\n", d.slicers(), d.rate());
    std::printf("   SNR in 3 kHz (dB)   flat        space +6 dB   space −6 dB   (of 40; repaired; wrong)\n");
    int flat10 = 0, tilt = 0, flat20 = 0;
    for (double snr : { 4.0, 6.0, 8.0, 10.0, 14.0, 20.0 }) {
        Res a = modemRun(1200, snr, 0, 40, 10 + uint64_t(snr)), b = modemRun(1200, snr, 6, 40, 20 + uint64_t(snr)), c = modemRun(1200, snr, -6, 40, 30 + uint64_t(snr));
        std::printf("   %5.0f               %2d (%d;%d)    %2d (%d;%d)      %2d (%d;%d)\n", snr, a.heard, a.repaired, a.wrong, b.heard, b.repaired, b.wrong, c.heard, c.repaired, c.wrong);
        CHECK(a.wrong + b.wrong + c.wrong == 0, "no wrong frames at %.0f dB", snr);
        if (snr == 10) flat10 = a.heard;
        if (snr == 20) { flat20 = a.heard; tilt = b.heard + c.heard; }
    }
    CHECK(flat20 == 40, "all 40 at 20 dB (%d)", flat20);
    CHECK(tilt == 80, "all 80 at 20 dB with the tones 6 dB apart either way (%d)", tilt);
    CHECK(flat10 >= 36, "≥ 36 of 40 at 10 dB (%d)", flat10);
    // ★ what the extra slicers and the repair buy, against one balanced slicer (same audio, same seeds)
    int one = 0, all = 0;
    for (double snr : { 6.0, 8.0, 10.0 }) for (double tl : { 0.0, 6.0, -6.0 }) {
        one += modemRun(1200, snr, tl, 40, 50 + uint64_t(snr * 3 + tl + 10), 0, true).heard;
        all += modemRun(1200, snr, tl, 40, 50 + uint64_t(snr * 3 + tl + 10), 0, false).heard;
    }
    std::printf("   6/8/10 dB × flat/±6 dB (360 frames): one slicer %d, five slicers + repair %d\n", one, all);
    CHECK(all > one, "the slicers gain");

    std::printf("── the modem: HF 300 Bd ──\n");
    for (double off : { 0.0, -50.0, 30.0 }) {
        Res a = modemRun(300, 12, 0, 12, 40 + uint64_t(off + 100), off);
        std::printf("   tuned %+3.0f Hz, 12 dB in 500 Hz: %d of %d (%d repaired), %d wrong\n", off, a.heard, a.sent, a.repaired, a.wrong);
        CHECK(a.heard >= 10 && a.wrong == 0, "300 Bd at %+.0f Hz", off);
    }
}

static void testNoise() {
    std::printf("── noise only: zero frames ──\n");
    for (int baud : { 1200, 300 }) {
        vibeaprs::AfskDemod d(48000, baud);
        Rng rng(99 + uint64_t(baud));
        std::vector<float> blk(48000);
        int frames = 0;
        for (int s = 0; s < 600; ++s) {
            for (auto& x : blk) x = float(0.2 * rng.g());
            d.feed(blk.data(), int(blk.size()), [&](const vibeaprs::Frame&) { ++frames; });
        }
        CHECK(frames == 0, "%d Bd: %d frames in 600 s of noise (%d slicers, repair on)", baud, frames, d.slicers());
    }
}

static void testRun() {
    std::printf("── the run: duplicates, paths, the list ──\n");
    std::vector<std::string> out;
    std::vector<vibestation::Heard> heard;
    double clock = 100;
    vibeaprs::Run::Opts o;
    o.json = [&](const std::string& j) { out.push_back(j); };
    o.heard = [&](const vibestation::Heard& h) { heard.push_back(h); };
    o.clock = [&] { return clock; };
    vibeaprs::Run run(1200, o);
    auto put = [&](const std::vector<uint8_t>& b) { vibeaprs::Frame f; f.bytes = b; run.onFrame(f); };
    put(ui("M0ABC", 9, "APRS", {{"WIDE1-1", false}, {"WIDE2-1", false}}, "!5130.00N/00010.00W>on the way"));
    clock += 2;
    put(ui("M0ABC", 9, "APRS", {{"MB7UH", true}, {"WIDE1", true}, {"WIDE2-1", false}}, "!5130.00N/00010.00W>on the way"));
    CHECK(run.stationCount() == 1 && heard.size() == 1, "a digipeated copy is the same packet (%zu stations, %zu lines)", run.stationCount(), heard.size());
    std::string l = run.listJson(true);
    CHECK(l.find("\"via\":\"MB7UH\"") != std::string::npos, "via the digipeater that repeated it: %s", l.c_str());
    clock += 40;
    put(ui("M0ABC", 9, "APRS", {}, "!5131.00N/00011.00W>on the way"));
    put(ui("G4XYZ", 0, "APRS", {}, ":M0ABC-9  :See you at the club{12"));
    put(ui("MB7UH", 0, "APRS", {}, "@261200z5128.00N/00012.00W_090/005g010t050h80b10120"));
    l = run.listJson(true);
    CHECK(run.stationCount() == 3, "3 stations (%zu)", run.stationCount());
    CHECK(l.find("\"trail\":[[51.50000,-0.16667],[51.51667,-0.18333]]") != std::string::npos, "the track: %s", l.substr(0, 400).c_str());
    CHECK(l.find("\"messages\":[{") != std::string::npos && l.find("See you at the club") != std::string::npos && l.find("\"id\":\"12\"") != std::string::npos, "the message");
    CHECK(l.find("\"wx\":{\"tempC\":10.0,\"hum\":80,\"windDir\":90,\"windKmh\":8.0,\"gustKmh\":16.1,\"hPa\":1012.0}") != std::string::npos, "weather in metric");
    CHECK(out.size() >= 4 && out[0].find("\"type\":\"aprs_packet\"") != std::string::npos, "a packet frame per new packet");
    CHECK(heard.size() == 4 && heard[3].line.find("MB7UH \xc2\xb7 weather station \xc2\xb7 10.0 \xc2\xb0" "C \xc2\xb7 80 % RH \xc2\xb7 wind 8 km/h E") == 0,
          "the gallery line: %s", heard.size() > 3 ? heard[3].line.c_str() : "");
    // invalid UTF-8 in a comment never breaks the JSON
    put(ui("F1ABC", 0, "APRS", {}, std::string("!4800.00N/00200.00E-caf\xe9 ok")));
    l = run.listJson(false);
    CHECK(l.find("caf? ok") != std::string::npos, "Latin-1 replaced");
}

static void testCpu() {
    std::printf("── CPU ──\n");
    Res a = modemRun(1200, 20, 0, 40, 5);
    std::printf("   1200 Bd: %.3f s for %.1f s of audio = %.2f %% of one core (this Mac)\n", a.cpu, a.secs, 100 * a.cpu / a.secs);
    Res b = modemRun(300, 20, 0, 6, 6);
    std::printf("   300 Bd: %.2f %% of one core\n", 100 * b.cpu / b.secs);
    CHECK(a.cpu < a.secs / 10, "under a tenth of real time");
}

/** `test-aprs --write-clip <file.cu8> <rate>`: APRS packets, Bell 202 AFSK FM-modulated (±3 kHz, NFM) onto complex IQ at
 *  `rate`, CENTRED on the channel (fake-rtl-tcp --hf --hf-centre 144800000 mixes it to wherever the dongle sits). */
static int writeClip(const char* path, double rate) {
    std::vector<float> a;
    const char* pk[][2] = {
        { "G4ABC", "!5130.00N/00010.00W>car on the A1 /A=000120" },
        { "MB7UH", "@261200z5128.00N/00012.00W_090/005g010t050h80b10120" },
        { "M0XYZ", ":G4ABC-9  :See you at the club{7" },
        { "G8DEF", ";REPEATER *261200z5132.00N/00005.00W#GB3XX 145.725" },
    };
    for (int rep = 0; rep < 2; ++rep)
        for (auto& p : pk) {
            for (int k = 0; k < 24000; ++k) a.push_back(0);
            modulate(a, ui(p[0], p[0][1] == '4' ? 9 : 0, "APRS", {{"WIDE1-1", false}}, p[1]), 1200, 1200, 2200, 0, 0.5, 30);
        }
    for (int k = 0; k < 24000; ++k) a.push_back(0);
    // 48 kHz audio → FM at `rate`: linear interpolation of the audio, phase += 2π·3000·x/rate
    const double up = rate / 48000.0, dev = 3000.0 / 0.5;
    Rng rng(4242);
    std::vector<uint8_t> u;
    u.reserve(size_t(a.size() * up * 2) + 16);
    double ph = 0;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        for (int j = 0; j < int(up); ++j) {
            const double x = a[i] + (a[i + 1] - a[i]) * (j / up);
            ph += 2 * M_PI * dev * x / rate;
            const double I = 0.4 * std::cos(ph) + 0.01 * rng.g(), Q = 0.4 * std::sin(ph) + 0.01 * rng.g();
            u.push_back(uint8_t(std::max(0.0, std::min(255.0, 127.5 + I * 127))));
            u.push_back(uint8_t(std::max(0.0, std::min(255.0, 127.5 + Q * 127))));
        }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f); std::fclose(f);
    std::fprintf(stderr, "wrote %s: %.1f s at %.0f S/s, 8 APRS packets (4 stations, twice)\n", path, u.size() / 2 / rate, rate);
    return 0;
}

/** `test-aprs --wav <file.wav> [300|1200]`: decode a recording (16-bit PCM WAV, any rate — resampled to 48 kHz) and print
 *  every packet in TNC2 form with its readable line; the count, and how many a single slicer alone would have found. */
static int decodeWav(const char* path, int baud) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::vector<uint8_t> b; uint8_t tmp[65536]; size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) { std::fprintf(stderr, "not a WAV\n"); return 2; }
    size_t p = 12; uint32_t rate = 0; uint16_t ch = 1, bits = 16; size_t dataAt = 0, dataLen = 0;
    while (p + 8 <= b.size()) {
        const uint32_t len = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | uint32_t(b[p + 7]) << 24;
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = uint16_t(b[p + 10] | b[p + 11] << 8); rate = b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | uint32_t(b[p + 15]) << 24; bits = uint16_t(b[p + 22] | b[p + 23] << 8); }
        if (!std::memcmp(&b[p], "data", 4)) { dataAt = p + 8; dataLen = std::min<size_t>(len, b.size() - dataAt); break; }
        p += 8 + len + (len & 1);
    }
    if (!rate || bits != 16 || !dataAt) { std::fprintf(stderr, "need 16-bit PCM\n"); return 2; }
    const size_t frames = dataLen / (2 * ch);
    std::vector<float> in(frames);
    for (size_t i = 0; i < frames; ++i) in[i] = int16_t(b[dataAt + i * 2 * ch] | b[dataAt + i * 2 * ch + 1] << 8) / 32768.f;
    std::vector<float> a(size_t(frames * 48000.0 / rate));
    for (size_t i = 0; i < a.size(); ++i) { const double x = i * double(rate) / 48000.0; const size_t k = size_t(x); const double fr = x - k;
        a[i] = k + 1 < frames ? float(in[k] * (1 - fr) + in[k + 1] * fr) : 0.f; }
    int all = 0, one = 0;
    for (int pass = 0; pass < 2; ++pass) {
        vibeaprs::AfskDemod d(48000, baud, pass == 1);
        for (size_t o = 0; o < a.size(); o += 4800) d.feed(a.data() + o, int(std::min<size_t>(4800, a.size() - o)), [&](const vibeaprs::Frame& fr) {
            vibeaprs::Ax25 ax; if (!vibeaprs::parseAx25(fr.bytes, ax)) return;
            if (pass == 0) { ++all; vibeaprs::Packet pk = vibeaprs::parsePacket(ax);
                std::printf("%7.1f s  %s%s\n           %s\n", double(o) / 48000, pk.raw.c_str(), fr.repaired ? "  [one bit repaired]" : "", vibeaprs::readable(pk).c_str()); }
            else ++one; });
    }
    std::printf("%s: %.0f s at %u Hz, %d Bd — %d packets (one balanced slicer alone: %d)\n", path, frames / double(rate), rate, baud, all, one);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--write-clip") == 0) return writeClip(argv[2], std::atof(argv[3]));
    if (argc >= 3 && std::strcmp(argv[1], "--wav") == 0) return decodeWav(argv[2], argc >= 4 ? std::atoi(argv[3]) : 1200);
    testAx25();
    testFormats();
    testModem();
    testNoise();
    testRun();
    testCpu();
    std::printf("\ntest-aprs: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
