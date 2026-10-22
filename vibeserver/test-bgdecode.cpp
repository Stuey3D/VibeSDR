// test-bgdecode.cpp — BACKGROUND DECODING and THE GALLERY, below the server (V12, 2026-10-22).
//
// ★★ WHAT THIS PINS (the live half — the radio moving, a listener stopping it — is scripts/test-bg-decode.mjs):
//    · the config: Nick's case (centre 500 kHz, 489.5 + 517.5 USB NAVTEX, all day) parses, round-trips and validates
//      on an Airspy HF+ (768 kS/s); a VFO outside the window is REFUSED and the refusal says why; overlapping profiles
//      are refused and NAMED (all-day + anything, wrapping midnight); which profile runs at which UTC minute.
//    · ★★ LSB INVERTS MARK AND SPACE (NickB asked for LSB to dodge interference): the same RTTY and NAVTEX signal heard
//      on USB and on LSB decodes to the SAME text when the decoder is told the sideband (attachMsg) — and NAVTEX with
//      its auto-polarity switched OFF, so the flag is what does it, not the decoder's own search.
//    · the NAVTEX message cutter (a port of src/utils/navtex.ts): whole messages, a damaged header, end lost, the FEC count.
//    · the gallery: newest first, filters, the size limit evicting the OLDEST, path traversal refused three ways, RTTY
//      appended in pieces (never per character).
//    · end to end without a server: synthetic IQ with a NAVTEX station at 518 kHz, centre 500 kHz, through BgRunner's
//      channelizer + demodulators + decoders → a NAVTEX message in the gallery, from a USB VFO AND an LSB VFO.
#include "vibe_bgdecode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/bgdec-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}

// ── signals ─────────────────────────────────────────────────────────────────────────────────────────────────────
// NAVTEX (CCIR 476 + SITOR-B FEC), the encoder test-navtex-fec.cpp uses (fldigi's create_fec).
static const char* NLT = "________________" "_______J___F_CK_" "_______W___Y_PQ_" "_____G___MX_V___"
                         "_______A___S_IU_" "___D_RE__N__ ___" "___Z_L___H__\n___" "_OB_T___\r_______";
static const char* NFG = "________________" "_______'___!_:(_" "_______2___6_01_" "_____&___./_;___"
                         "_______-___\x07_87_" "___$_43__,__ ___" "___\"_)___#__\n___" "_9?_5___\r_______";
static const uint8_t NLTRS = 0x5A, NFIGS = 0x36, ALPHA = 0x0F, REP = 0x66, CHAR32 = 0x6A;
static int nfind(const char* t, char c) { for (int i = 0; i < 128; i++) if (t[i] == c && c != '_') return i; return -1; }
static std::vector<uint8_t> navEncode(const std::string& text) {
    std::vector<uint8_t> v{NLTRS}; bool fig = false;
    for (char c : text) {
        const int l = nfind(NLT, c), f = nfind(NFG, c);
        if (c == ' ' || c == '\r' || c == '\n') v.push_back((uint8_t)l);
        else if (l >= 0 && !fig) v.push_back((uint8_t)l);
        else if (f >= 0 && fig) v.push_back((uint8_t)f);
        else if (f >= 0) { v.push_back(NFIGS); v.push_back((uint8_t)f); fig = true; }
        else { v.push_back(NLTRS); v.push_back((uint8_t)l); fig = false; }
    }
    return v;
}
/** The bit stream of a whole NAVTEX transmission: phasing, the message with FEC, phasing after. +1 = mark. */
static std::vector<int> navBits(const std::string& text) {
    const auto c = navEncode(text);
    std::vector<uint8_t> s;
    for (int i = 0; i < 40; i++) { s.push_back(REP); s.push_back(ALPHA); }
    for (size_t i = 0; i < c.size(); i++) { s.push_back(c[i]); s.push_back(i >= 2 ? c[i - 2] : ALPHA); }
    s.push_back(CHAR32); s.push_back(c[c.size() - 2]); s.push_back(CHAR32); s.push_back(c.back());
    for (int i = 0; i < 40; i++) { s.push_back(REP); s.push_back(ALPHA); }
    std::vector<int> b;
    for (uint8_t w : s) for (int i = 0; i < 7; i++) b.push_back((w >> i & 1) ? 1 : -1);
    return b;
}
// RTTY ITA2 5N1.5 (test-rtty-shift.cpp's table).
static const char* RLT = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
static std::vector<int> rttyBits(const std::string& text) {
    std::vector<int> b;
    auto code = [&](int c) { b.push_back(-1); for (int k = 0; k < 5; k++) b.push_back((c >> k & 1) ? 1 : -1); b.push_back(1); b.push_back(2); };   // 2 = half a stop bit
    for (int i = 0; i < 6; i++) code(31);   // LTRS
    for (char ch : text) { int cc = -1; for (int i = 1; i < 32; i++) if (RLT[i] == ch) cc = i; if (cc > 0) code(cc); }
    for (int i = 0; i < 8; i++) code(31);
    return b;
}
/** 48 kHz audio from bits: mark at centre + dir·shift/2 (dir = +1 USB-normal, -1 mirrored by LSB). */
static std::vector<int16_t> fskAudio(const std::vector<int>& bits, double baud, double centre, double shift, int dir) {
    std::vector<int16_t> s; double ph = 0; const double fs = 48000, spb = fs / baud;
    double acc = 0;
    auto tone = [&](double f, double n) { acc += n; const int k = (int)std::lround(acc); for (int i = (int)s.size(); i < k; i++) { ph += 2 * M_PI * f / fs; s.push_back((int16_t)(9000 * std::sin(ph))); } };
    tone(centre + dir * shift / 2, fs);      // a second of mark first
    for (int v : bits) {
        if (v == 2) { tone(centre + dir * shift / 2, spb / 2); continue; }
        tone(centre + (v > 0 ? dir : -dir) * shift / 2, spb);
    }
    tone(centre + dir * shift / 2, fs);
    return s;
}
static std::string decodeAudio(const std::vector<int16_t>& a, double cf, double shift, double baud, bool navtex, bool inverted,
                               bool autoInvert = true) {
    NavtexOptions o; o.autoInvert = autoInvert;
    FskDecoder d(48000, cf, shift, baud, navtex ? "4/7" : "5N1.5", navtex ? "CCIR476" : "ITA2", inverted, navtex ? &o : nullptr);
    std::string out;
    d.onChar = [&](char32_t c) { out += c < 128 ? (char)c : '?'; };
    for (size_t i = 0; i < a.size(); i += 960) d.process(&a[i], (int)std::min<size_t>(960, a.size() - i));
    return out;
}
static bool invOf(const std::string& msg) { return msg.find("\"inverted\":true") != std::string::npos; }

// ── 1. config ───────────────────────────────────────────────────────────────────────────────────────────────────
static void config() {
    std::printf("── config: Nick's case, the window, overlaps, the clock ──\n");
    const std::string nick = R"({"on":true,"waitMin":5,"profiles":[{"centre":500000,"from":"00:00","to":"00:00",)"
                             R"("vfos":[{"hz":489500,"mode":"usb","dec":"navtex"},{"hz":517500,"mode":"usb","dec":"navtex"}]}]})";
    bg::Config c; std::string err;
    ok(bg::parse(nick, c, err) && c.on && c.waitSec == 300 && c.profiles.size() == 1 && c.profiles[0].vfos.size() == 2,
       "Nick's profile parses: on, 5 minutes, 1 profile, 2 VFOs");
    ok(c.profiles[0].allDay() && bg::activeProfile(c, 0) == 0 && bg::activeProfile(c, 1439) == 0, "00:00–00:00 is all day");
    ok(bg::validate(c, 768000, err), "...and validates on an Airspy HF+ at 768 kS/s (" + err + ")");
    bg::Config back; ok(bg::parse(bg::toJson(c), back, err) && bg::toJson(back) == bg::toJson(c), "...and round-trips: " + bg::toJson(c));
    ok(std::fabs(bg::signalHz(c.profiles[0].vfos[1]) - 518000) < 1, "517.5 USB NAVTEX listens to 518.0 kHz");
    bg::Vfo lsb{518500, "lsb", "navtex"};
    ok(std::fabs(bg::signalHz(lsb) - 518000) < 1, "518.5 LSB NAVTEX listens to 518.0 kHz too");
    ok(bg::parse("", c, err) && !c.on && c.profiles.empty(), "no setting = off (every radio before this existed)");

    bg::Config far; bg::parse(nick, far, err);
    far.profiles[0].vfos[1].hz = 900000;
    err.clear();
    ok(!bg::validate(far, 768000, err) && err.find("VFO 2") != std::string::npos && err.find("900.0 kHz") != std::string::npos
       && err.find("768 kS/s") != std::string::npos, "a VFO 400 kHz from the centre at 768 kS/s is refused, saying why: " + err);
    ok(bg::validate(far, 2000000, err), "...and allowed at 2 MS/s, where the radio does see it");

    bg::Config two; bg::parse(nick, two, err);
    bg::Profile day = two.profiles[0]; day.fromMin = 6 * 60; day.toMin = 18 * 60; day.centreHz = 7.04e6;
    day.vfos = { bg::Vfo{7040000, "usb", "rtty"} };
    two.profiles.push_back(day);
    err.clear();
    ok(!bg::validate(two, 768000, err) && err.find("Profile 1 (all day)") != std::string::npos && err.find("Profile 2 (06:00") != std::string::npos,
       "an all-day profile and a 06:00–18:00 one are refused, both named: " + err);
    two.profiles[0].fromMin = 18 * 60; two.profiles[0].toMin = 6 * 60;   // night: 18:00 → 06:00, wrapping midnight
    err.clear();
    ok(bg::validate(two, 768000, err), "night 18:00–06:00 + day 06:00–18:00 is fine (" + err + ")");
    ok(bg::activeProfile(two, 23 * 60) == 0 && bg::activeProfile(two, 3 * 60) == 0 && bg::activeProfile(two, 6 * 60) == 1
       && bg::activeProfile(two, 18 * 60 - 1) == 1 && bg::activeProfile(two, 18 * 60) == 0, "...and the clock picks the right one at each edge");
    two.profiles[1].toMin = 19 * 60;
    err.clear();
    ok(!bg::validate(two, 768000, err) && err.find("18:00 UTC") != std::string::npos, "an hour's overlap is refused at the minute it starts: " + err);
    bg::Config bad; bg::parse(nick, bad, err); bad.profiles[0].vfos[0].mode = "am";
    ok(!bg::validate(bad, 768000, err), "a decoder on AM is refused (no decoder of ours works there): " + err);
}

// ── 2. LSB inverts mark and space ───────────────────────────────────────────────────────────────────────────────
static void sideband() {
    std::printf("── LSB: the same signal, mirrored, decodes the same when the decoder is told ──\n");
    const bg::Vfo u{4583000, "usb", "rtty-ham"}, l{4585000, "lsb", "rtty-ham"};
    ok(!invOf(bg::attachMsg(u)) && invOf(bg::attachMsg(l)), "RTTY ham: USB normal, LSB inverted");
    const bg::Vfo wu{4583000, "usb", "rtty-weather"}, wl{4585000, "lsb", "rtty-weather"};
    ok(invOf(bg::attachMsg(wu)) && !invOf(bg::attachMsg(wl)), "RTTY weather (reverse on USB): LSB flips it back");
    const bg::Vfo nu{517500, "usb", "navtex"}, nl{518500, "lsb", "navtex"};
    ok(!invOf(bg::attachMsg(nu)) && invOf(bg::attachMsg(nl)), "NAVTEX: USB normal, LSB inverted");

    const std::string rt = "CQ CQ DE G4IRX G4IRX STORM AUSTEN TONIGHT ";
    const auto rb = rttyBits(rt);
    const auto aU = fskAudio(rb, 45.45, 1000, 170, +1), aL = fskAudio(rb, 45.45, 1000, 170, -1);
    const std::string dU = decodeAudio(aU, 1000, 170, 45.45, false, invOf(bg::attachMsg(u)));
    const std::string dL = decodeAudio(aL, 1000, 170, 45.45, false, invOf(bg::attachMsg(l)));
    ok(dU.find("STORM AUSTEN") != std::string::npos, "RTTY on USB decodes (\"" + dU + "\")");
    ok(dU == dL, "★ RTTY on LSB, told the sideband, decodes IDENTICALLY (\"" + dL + "\")");
    const std::string dWrong = decodeAudio(aL, 1000, 170, 45.45, false, false);
    ok(dWrong.find("STORM AUSTEN") == std::string::npos, "...and NOT told, it does not (the flag is doing the work)");

    const std::string nm = "ZCZC GB98\r\nGALE WARNING STORM AUSTEN\r\nNNNN\r\n";
    const auto nb = navBits(nm);
    const auto nU = fskAudio(nb, 100, 500, 170, +1), nL = fskAudio(nb, 100, 500, 170, -1);
    const std::string xU = decodeAudio(nU, 500, 170, 100, true, invOf(bg::attachMsg(nu)), /*autoInvert=*/false);
    const std::string xL = decodeAudio(nL, 500, 170, 100, true, invOf(bg::attachMsg(nl)), /*autoInvert=*/false);
    ok(xU.find("GALE WARNING STORM AUSTEN") != std::string::npos, "NAVTEX on USB decodes, auto-polarity OFF");
    ok(xU == xL, "★ NAVTEX on LSB, told the sideband, decodes IDENTICALLY with auto-polarity OFF");
    const std::string xWrong = decodeAudio(nL, 500, 170, 100, true, false, false);
    ok(xWrong.find("STORM AUSTEN") == std::string::npos, "...and NOT told (auto-polarity off), it does not");
}

// ── 3. NAVTEX messages ──────────────────────────────────────────────────────────────────────────────────────────
static void navtexAsm() {
    std::printf("── NAVTEX messages (src/utils/navtex.ts, ported) ──\n");
    gallery::NavtexAsm a; std::vector<gallery::NavtexMsg> got;
    a.onMessage = [&](const gallery::NavtexMsg& m) { got.push_back(m); };
    a.push("ZCZC GB9", 100); a.push("8\r\nGALE WARNING\r\nNN", 101); a.push("NN\r\n", 102);
    ok(got.size() == 1 && got[0].id == "GB98" && got[0].text == "GALE WARNING" && !got[0].endLost && got[0].startedAt == 101,
       "a message split across chunks: one, id GB98, text exact");
    ok(gallery::NavtexAsm::title(got[0]).find("Station G") == 0 && gallery::NavtexAsm::title(got[0]).find("Met warning") != std::string::npos, "titled as the box titles it");
    got.clear();
    a.push("ZC_C EA12\r\nNAV WARNING ONE\r\nZCZC EA13\r\nSECOND\r\nNN_N\r\n", 200);
    ok(got.size() == 2 && got[0].id == "EA12" && got[0].endLost && got[1].id == "EA13" && !got[1].endLost,
       "a damaged ZCZC still opens; a new header ends the last [end lost]; NN_N closes");
    got.clear();
    a.push("ZCZC FA01\r\nHALF A MESSAGE THEN THE STATION FADES", 300);
    a.tick(300 + 74); ok(got.empty(), "74 s of quiet: still arriving");
    a.tick(300 + 76); ok(got.size() == 1 && got[0].endLost, "76 s: closed [end lost]");
    got.clear();
    a.push("ZCZC FA02\r\nLOCK LOST HERE", 400); a.lockState(3, 400); a.lockState(1, 401);
    ok(got.size() == 1 && got[0].endLost && got[0].id == "FA02", "the decoder losing its lock ends the message");
}

// ── 4. the gallery ──────────────────────────────────────────────────────────────────────────────────────────────
static void store() {
    std::printf("── the gallery store ──\n");
    gallery::Store s; s.setDir(tmpDir("store")); s.setLimitBytes(10000);
    gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 517500; m.mode = "usb"; m.decoder = "navtex"; m.profile = "Profile 1";
    std::vector<std::string> names;
    for (int i = 0; i < 6; i++) { m.utc = 1761170000 + i; m.title = "msg " + std::to_string(i); names.push_back(s.add(m, "txt", std::string(1500, 'A' + i))); }
    ok(!names[0].empty() && gallery::Store::safeName(names[0]), "items saved under safe names (" + names[0] + ")");
    std::string list = s.listJson("", 100);
    ok(list.find("\"msg 5\"") < list.find("\"msg 4\""), "listed NEWEST first");
    ok(list.find("\"o\":\"background\"") != std::string::npos && list.find("\"p\":\"Profile 1\"") != std::string::npos, "...labelled background, with the profile");
    m.kind = "rtty"; m.title = "rtty run"; m.utc = 1761170100;
    const std::string run = s.create(m, "txt", "RTTY\n");
    ok(s.append(run, "HELLO ") && s.append(run, "WORLD"), "an RTTY run appended in pieces");
    ok(s.listJson("rtty", 100).find("rtty run") != std::string::npos && s.listJson("rtty", 100).find("msg 5") == std::string::npos, "filtered by decoder");
    // 6 × 1500 + the run = 9,000-odd: under 10,000. Three more must evict the oldest.
    m.kind = "navtex";
    for (int i = 6; i < 9; i++) { m.utc = 1761170200 + i; m.title = "msg " + std::to_string(i); s.add(m, "txt", std::string(1500, 'a')); }
    list = s.listJson("", 100);
    ok(s.usedBytes() <= 10000, "★ the size limit holds: " + std::to_string((unsigned long long)s.usedBytes()) + " bytes of 10000");
    ok(list.find("\"msg 0\"") == std::string::npos && list.find("\"msg 8\"") != std::string::npos, "★ the OLDEST went first; the newest is kept");
    ok(list.find("rtty run") != std::string::npos, "...and an RTTY run still being written is never evicted");
    std::string p, ct;
    ok(s.resolve(names.back(), p, ct) && ct.find("text/plain") == 0, "a listed item resolves to its file");
    ok(!s.resolve("../config.json", p, ct) && !s.resolve("..%2Fconfig.json", p, ct) && !s.resolve(".lock", p, ct)
       && !s.resolve("index.jsonl", p, ct) && !s.resolve("/etc/passwd", p, ct), "★ ../, %2F, dotfiles, the index and absolute paths: refused");
    { FILE* f = std::fopen((s.dir() + "/notlisted.txt").c_str(), "w"); if (f) { std::fputs("x", f); std::fclose(f); } }
    ok(!s.resolve("notlisted.txt", p, ct), "★ a file in the folder that is NOT in the index is refused");
    ok(!s.resolve(names[0], p, ct), "an evicted item no longer resolves");
    ok(gallery::Store::freeBytesAt(s.dir()) > 0, "free space on the disk is known");
    s.setLimitBytes(0);
    ok(s.add(m, "txt", "x").empty(), "size 0 = the gallery keeps nothing");
}

// ── 5. a decoder host's tap writes the gallery ──────────────────────────────────────────────────────────────────
static void hostTap() {
    std::printf("── a listener's decoder: the tap files NAVTEX messages as 'live' ──\n");
    gallery::Store s; s.setDir(tmpDir("tap"));
    DecoderHost::Env env;
    env.dialHz = [] { return 517500.0; };
    env.tap = [&s](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
        gallery::Meta m; m.kind = bg::galleryKindFor(ext, msg); if (m.kind.empty()) return nullptr;
        m.origin = "live"; m.freqHz = 517500; m.mode = "usb"; m.decoder = ext;
        return std::make_shared<bg::GalleryTap>(s, m);
    };
    auto h = std::make_shared<DecoderHost>(env, "sess");
    ok(h->start("navtex", "{}") == DecoderHost::Start::Ok, "NAVTEX started");
    const auto a = fskAudio(navBits("ZCZC GA42\r\nWIND SW 6 TO 8\r\nNNNN\r\n"), 100, 500, 170, +1);
    std::vector<float> f(a.size()); for (size_t i = 0; i < a.size(); i++) f[i] = a[i] / 32768.0f;
    for (size_t i = 0; i < f.size(); i += 4800) {
        h->feed(&f[i], (int)std::min<size_t>(4800, f.size() - i));
        while (h->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (h->queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h->stop();
    const std::string list = s.listJson("navtex", 10);
    ok(list.find("\"o\":\"live\"") != std::string::npos && list.find("Station G") != std::string::npos,
       "★ the message is in the gallery, labelled live: " + list.substr(0, 200));
}

// ── 6. end to end: IQ → channels → demodulators → decoders → gallery ────────────────────────────────────────────
static void runner() {
    std::printf("── BgRunner: Nick's profile on synthetic IQ (NAVTEX on 518 kHz, centre 500 kHz) ──\n");
    gallery::Store s; s.setDir(tmpDir("runner"));
    FileSlots slots("", [] { return 4; });
    const double fs = 250000, centre = 500000;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &s;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{517500, "usb", "navtex"}, bg::Vfo{518500, "lsb", "navtex"}, bg::Vfo{489500, "usb", "navtex"} };
    std::vector<std::string> logs;
    std::mutex lm;
    p.log = [&](const std::string& t) { std::lock_guard<std::mutex> lk(lm); logs.push_back(t); };
    r.start(p);
    ok(r.running() && r.decodersRunning() == 3 && slots.inUse() == 3, "3 VFOs, 3 decoders, 3 slots taken");
    // The station: mark = the HIGHER RF tone (518 kHz + 85 Hz), as USB-normal hears it.
    const auto bits = navBits("ZCZC GB98\r\nGALE WARNING STORM AUSTEN\r\nNNNN\r\n");
    const double spb = fs / 100.0;
    std::vector<vibedsp::cf32> iq; iq.reserve(65536);
    double ph = 0, acc = 0; size_t made = 0;
    std::mt19937 g(7); std::normal_distribution<float> n(0.0f, 0.002f);
    auto emit = [&](double rf, double samples) {
        acc += samples; const size_t k = (size_t)std::lround(acc);
        for (; made < k; made++) {
            ph += 2 * M_PI * (rf - centre) / fs; if (ph > M_PI) ph -= 2 * M_PI;
            iq.push_back(vibedsp::cf32((float)(0.1 * std::cos(ph)) + n(g), (float)(0.1 * std::sin(ph)) + n(g)));
            if (iq.size() == 16384) {
                r.feedIq(iq.data(), (int)iq.size()); iq.clear();
                std::this_thread::sleep_for(std::chrono::microseconds(300));   // ★ paced: the runner drops past 0.5 s
            }
        }
    };
    emit(518000 + 85, fs);
    for (int b : bits) emit(518000 + (b > 0 ? 85 : -85), spb);
    emit(518000 + 85, 2 * fs);
    if (!iq.empty()) r.feedIq(iq.data(), (int)iq.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    r.stop();
    ok(slots.inUse() == 0, "stopped: every slot back");
    const std::string list = s.listJson("navtex", 20);
    int usb = 0, lsbN = 0;
    for (size_t at = 0; (at = list.find("\"f\":", at)) != std::string::npos; at++) {
        const size_t e = list.find('}', at);
        const std::string item = list.substr(at, e - at);
        if (item.find("Station G") == std::string::npos) continue;
        if (item.find("\"m\":\"usb\"") != std::string::npos && item.find("\"hz\":517500") != std::string::npos) usb++;
        if (item.find("\"m\":\"lsb\"") != std::string::npos) lsbN++;
    }
    ok(usb == 1, "★ VFO 1 (517.5 USB) filed the message GB98");
    ok(lsbN == 1, "★ VFO 2 (518.5 LSB) filed the SAME message — LSB inversion end to end");
    ok(list.find("489500") == std::string::npos, "VFO 3 (489.5) heard nothing and filed nothing");
    std::string p0, ct;
    const std::string first = gallery::lineStr(list, "f");
    if (s.resolve(first, p0, ct)) {
        FILE* f = std::fopen(p0.c_str(), "r"); std::string body; char b[512]; size_t k;
        while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, k);
        if (f) std::fclose(f);
        ok(body.find("GALE WARNING STORM AUSTEN") != std::string::npos && body.find("ZCZC GB98") != std::string::npos
           && body.find("background decoding (Profile 1)") != std::string::npos, "the file reads as the box shows it:\n" + body);
    } else ok(false, "the item resolves");
    ok(r.droppedSamples() == 0, "no IQ dropped at this pace (" + std::to_string((unsigned long long)r.droppedSamples()) + ")");
}

int main() {
    config();
    sideband();
    navtexAsm();
    store();
    hostTap();
    runner();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
