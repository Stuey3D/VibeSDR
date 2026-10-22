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
#include <dirent.h>
#include <sys/stat.h>
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
    a.push("ZCZC GB9", 100000); a.push("8\r\nGALE WARNING\r\nNN", 101000); a.push("NN\r\n", 102000);
    ok(got.size() == 1 && got[0].id == "GB98" && got[0].text == "GALE WARNING" && !got[0].endLost && got[0].startedAt == 101,
       "a message split across chunks: one, id GB98, text exact");
    ok(gallery::NavtexAsm::title(got[0]).find("Station G") == 0 && gallery::NavtexAsm::title(got[0]).find("Met warning") != std::string::npos, "titled as the box titles it");
    got.clear();
    a.push("ZC_C EA12\r\nNAV WARNING ONE\r\nZCZC EA13\r\nSECOND\r\nNN_N\r\n", 200000);
    ok(got.size() == 2 && got[0].id == "EA12" && got[0].endLost && got[1].id == "EA13" && !got[1].endLost,
       "a damaged ZCZC still opens; a new header ends the last [end lost]; NN_N closes");
    got.clear();
    a.push("ZCZC FA01\r\nHALF A MESSAGE THEN THE STATION FADES", 300000);
    a.tick(300000 + 74000); ok(got.empty(), "74 s of quiet: still arriving");
    a.tick(300000 + 76000); ok(got.size() == 1 && got[0].endLost, "76 s: closed [end lost]");
    got.clear();
    a.push("ZCZC FA02\r\nLOCK LOST HERE", 400000); a.lockState(3, 400000); a.lockState(1, 401000);
    a.tick(402000); ok(got.empty(), "lock lost 1 s: held open (it may be a blip)");
    a.tick(402600); ok(got.empty(), "lock lost 1.6 s: still open (beta4: a fade does not close a message)");
    a.tick(400000 + 76000);
    ok(got.size() == 1 && got[0].endLost && got[0].id == "FA02" && got[0].text == "LOCK LOST HERE", "silent for good (75 s): the message ends at the loss");
    got.clear();
    // ★★★ FADES (Lenovo, beta3, 517.5 kHz: lock lost 5–15 s at a time): ONE item, two gap markers, "gaps: 2 (27 s)".
    {
        gallery::NavtexAsm f; std::vector<gallery::NavtexMsg> m;
        f.onMessage = [&](const gallery::NavtexMsg& x) { m.push_back(x); };
        int64_t t = 600000;
        f.lockState(3, t);
        f.push("ZCZC VA28\r\nDOVER STRAIT WIND SOUTHWEST 6\r\n", t += 3000);
        f.lockState(1, t += 100); f.push("XQ_", t + 500); f.lockState(3, t += 12000);
        f.push("THAMES SOUTHWEST 5 TO 7\r\n", t += 2000);
        f.lockState(1, t += 100); f.lockState(3, t += 15000);
        f.push("DOVER RAIN LATER\r\nNNNN\r\n", t += 2000);
        ok(m.size() == 1 && m[0].id == "VA28" && !m[0].endLost && !m[0].startLost, "★ two long fades mid-message: ONE message, ZCZC to NNNN");
        ok(m.size() == 1 && m[0].text.find("signal lost 12 s") != std::string::npos && m[0].text.find("signal lost 15 s") != std::string::npos
           && m[0].text.find("XQ_") == std::string::npos, "...two gap markers, the hunting garbage dropped: " + (m.empty() ? std::string() : m[0].text));
        ok(m.size() == 1 && gallery::NavtexAsm::title(m[0]).find("gaps: 2 (27 s)") != std::string::npos, "...and the title says gaps: 2 (27 s): " + (m.empty() ? std::string() : gallery::NavtexAsm::title(m[0])));
        m.clear();
        f.lockState(3, t += 1000);
        f.push("ZCZC VA29\r\nFIRST STATION\r\n", t += 2000);
        f.lockState(1, t += 100); f.lockState(3, t += 8000);
        f.push("ZCZC LB04\r\nSECOND STATION\r\nNNNN\r\n", t += 2000);
        ok(m.size() == 2 && m[0].id == "VA29" && m[0].endLost && m[1].id == "LB04" && m[1].text.find("FIRST") == std::string::npos,
           "★ a fade then a DIFFERENT ZCZC: two items (the takeover protection)");
    }
    // ★★ THE LENOVO SPLIT (NickB, 490 kHz, 2026-10-22): a real-shaped stream — header, text, four resyncs each relocked
    //    within a second, NNNN — is ONE message, ZCZC to NNNN.
    {
        gallery::NavtexAsm b; std::vector<gallery::NavtexMsg> m;
        b.onMessage = [&](const gallery::NavtexMsg& x) { m.push_back(x); };
        int64_t t = 500000;
        b.lockState(3, t);
        b.push("ZCZC BA09\r\n070825 UTC OCT\r\nOOSTENDERADIO LOKAAL BERICHT LB013/26\r\n", t += 4000);
        for (int k = 0; k < 4; k++) {
            b.lockState(1, t += 500); b.lockState(3, t += 800); b.tick(t += 100);
            b.push("KUSTROUTE DEEL " + std::to_string(k) + " BIJZONDERE MARKERINGSBOEI\r\n", t += 3000);
        }
        b.push("NNNN\r\n", t += 1000);
        ok(m.size() == 1 && m[0].id == "BA09" && !m[0].endLost && !m[0].startLost && m[0].text.find("DEEL 3") != std::string::npos,
           "★ four lock blips mid-message: ONE message, ZCZC BA09 to NNNN (" + std::to_string(m.size()) + " item(s))");
        ok(b.lockLosses() == 4, "...and the blips were seen (4 lock losses, for the log)");
    }
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
    {   // ★★★ THE SERIAL NEVER REACHES THE GALLERY (Lenovo, 2026-10-22: "r":"SDRplay RSP1A 2235030199" on every item)
        gallery::Store g; g.setDir(tmpDir("serial"));
        g.setRadioLabel("SDRplay RSP1A 2235030199");                  // an older build: no redaction yet
        gallery::Meta mm; mm.kind = "navtex"; mm.origin = "background"; mm.freqHz = 489500; mm.mode = "usb"; mm.title = "Station B";
        const std::string old = g.add(mm, "txt", "ZCZC BA09\nNNNN\n");
        ok(g.listJson("", 10).find("2235030199") != std::string::npos, "(an item an older build stored WITH the serial)");
        g.setRedact({ "2235030199", "00000003" });
        g.setRadioLabel("SDRplay RSP1A 2235030199");
        const std::string fresh = g.add(mm, "txt", "ZCZC BA10\nNNNN\n");
        const std::string j = g.listJson("", 10);
        ok(j.find("2235030199") == std::string::npos && j.find("\"r\":\"SDRplay RSP1A\"") != std::string::npos,
           "★ no serial in gallery.json — old item stripped, new item stored without it: " + j.substr(0, 160));
        std::string idx; { FILE* f = std::fopen((g.dir() + "/index.jsonl").c_str(), "r"); char b[4096]; size_t k; while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) idx.append(b, k); if (f) std::fclose(f); }
        ok(idx.find("2235030199") == std::string::npos, "★ ...and the index on disk was rewritten without it");
        std::string p1, c1;
        bool clean = true;
        for (const auto& n : { old, fresh }) { if (n.find("2235030199") != std::string::npos) clean = false;
            if (g.resolve(n, p1, c1)) { FILE* f = std::fopen(p1.c_str(), "r"); char b[512]; size_t k; std::string body; while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, k); if (f) std::fclose(f); if (body.find("2235030199") != std::string::npos) clean = false; } }
        ok(clean, "★ ...nor in any file name or served file");
        ok(gallery::stripTokens("RTL-SDR (00000003)", { "00000003" }) == "RTL-SDR", "a serial in brackets leaves no empty brackets");
    }
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

// ── 5b. a real-shaped NAVTEX transmission with a short GAP mid-message: one item, ZCZC to NNNN ─────────────────────
static void gapMidMessage() {
    std::printf("── a short gap mid-message (a fade, a resync) does not split the message ──\n");
    gallery::Store s; s.setDir(tmpDir("gap"));
    DecoderHost::Env env;
    env.dialHz = [] { return 489500.0; };
    env.tap = [&s](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
        gallery::Meta m; m.kind = bg::galleryKindFor(ext, msg); m.origin = "background"; m.freqHz = 489500; m.mode = "usb"; m.decoder = "navtex";
        return bg::makeGalleryTap(s, m);
    };
    auto h = std::make_shared<DecoderHost>(env, "gap");
    h->start("navtex", "{}");
    const std::string text = "ZCZC BA09\r\n070825 UTC OCT\r\nOOSTENDERADIO LOKAAL BERICHT LB013/26\r\nKUSTROUTE DE PANNE OOSTENDE\r\n"
                             "BIJZONDERE MARKERINGSBOEI JACHTTON ZAL WORDEN GEPLAATST OP POSITIE\r\nNNNN\r\n";
    auto a = fskAudio(navBits(text), 100, 500, 170, +1);
    // ★ A 0.4 s fade two-thirds of the way through (in the text, after the header): the transmitter carries on, so the
    //   samples are SILENCED, not removed — the decoder loses characters, and the lock, and must pick the message up again.
    const size_t cut = a.size() * 2 / 3;
    for (size_t i = cut; i < cut + (size_t)(48000 * 0.4) && i < a.size(); i++) a[i] = 0;
    std::vector<float> f(a.size()); for (size_t i = 0; i < a.size(); i++) f[i] = a[i] / 32768.0f;
    for (size_t i = 0; i < f.size(); i += 4800) {
        h->feed(&f[i], (int)std::min<size_t>(4800, f.size() - i));
        while (h->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));    // ★ ~30× real time: the blip rule runs on the wall clock
    }
    while (h->queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h->stop();
    const std::string list = s.listJson("navtex", 10);
    size_t n = 0; for (size_t at = 0; (at = list.find("\"f\":", at)) != std::string::npos; at++) n++;
    // ★ V12 beta 4: no position here, but the text says OOSTENDERADIO — so the title names it (navtexStations below)
    ok(n == 1 && list.find("B \xc2\xb7 Oostende") != std::string::npos && list.find("\"partial\":true") == std::string::npos,
       "★ ONE item, from its header, not partial (" + std::to_string(n) + "): " + list.substr(0, 220));
}

// ── 5c. ★ V12 beta 4: the NAVTEX STATION by name — the same cases as scripts/test_navtex.ts, so the rule's two readers
//        (vibe_navtex_stations.h, navtexStations.generated.ts) cannot drift ────────────────────────────────────────────
static void navtexStations() {
    std::printf("── NAVTEX stations: letter + band + this server's position (NGA Pub. 117) ──\n");
    const double LA = 52.3125, LO = -0.875;   // IO92nh, the Lenovo
    auto L = [&](char b1, double hz, const std::string& text = "", double la = 52.3125, double lo = -0.875) {
        return navtex::label(b1, hz, la, lo, text);
    };
    auto starts = [](const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; };
    ok(L('B', 489500) == "B \xc2\xb7 Oostende, Belgium \xf0\x9f\x87\xa7\xf0\x9f\x87\xaa", "B on 490 = Oostende, Belgium + flag: " + L('B', 489500));
    ok(starts(L('U', 489500), "U \xc2\xb7 Cullercoats, United Kingdom"), "U on 490 = Cullercoats");
    ok(starts(L('A', 489500), "A \xc2\xb7 Malin Head, Ireland"), "A on 490 = Malin Head");
    ok(starts(L('L', 489500), "L \xc2\xb7 Hamburg, Germany"), "L on 490 = Hamburg");
    ok(starts(L('G', 517500), "G \xc2\xb7 Cullercoats"), "G on 518 = Cullercoats");
    ok(starts(L('V', 517500), "V \xc2\xb7 Oostende"), "V on 518 = Oostende");
    ok(starts(L('L', 517500), "L \xc2\xb7 Rogaland, Norway"), "L on 518 = Rogaland");
    ok(starts(L('J', 517500), "J \xc2\xb7 Stockholm, Sweden"), "J on 518 = Stockholm");
    ok(starts(L('I', 517500), "I \xc2\xb7 Varberg, Sweden"), "I on 518 = Varberg");
    ok(starts(L('N', 517500), "N \xc2\xb7 Orlandet, Norway"), "N on 518 = Orlandet");
    ok(L('M', 517500) == "Station M", "M on 518: out of reach — the letter only");
    ok(L('W', 517500) == "Station W", "W on 518: Corsen / Valentia ambiguous — the letter only");
    ok(starts(L('W', 517500, "VALENTIA RADIO GALE WARNING"), "W \xc2\xb7 Valentia, Ireland"), "the text names Valentia");
    ok(starts(L('W', 517500, "CROSS CORSEN"), "W \xc2\xb7 Corsen, France"), "the text names Corsen");
    ok(L('W', 517500, "MRSC MALIN, EJM.") == "Station W", "a name not on W/518 does not count");
    ok(L('B', 489500, "", NAN, NAN) == "Station B", "no position → the letter only");
    ok(starts(L('B', 489500, "OOSTENDERADIO NAV WARNING", NAN, NAN), "B \xc2\xb7 Oostende"), "no position, the text names it");
    ok(starts(L('L', 489500, "NCC-HAMBURG", -33.9, 18.4), "L \xc2\xb7 Hamburg"), "the text wins wherever the server is");
    ok(L('B', 4209500) == "Station B", "HF: no HF letters in Pub. 117");
    ok(L('B', 10e6) == "Station B", "not a NAVTEX channel");
    ok(L('B', 489500, "", -33.9, 18.4) == "Station B", "Cape Town: nothing within reach");
    // the gallery's title, through the tap
    gallery::Store s; s.setDir(tmpDir("stations"));
    gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 489500; m.mode = "usb"; m.decoder = "navtex";
    m.lat = LA; m.lon = LO;
    {
        bg::GalleryTap t(s, m);
        t.text("ZCZC BA10\r\nOOSTENDERADIO\r\nNAV WARNING\r\nNNNN\r\n");
        t.finish(false);
    }
    const std::string list = s.listJson("navtex", 10);
    ok(list.find("B \xc2\xb7 Oostende, Belgium") != std::string::npos && list.find("Nav warning") != std::string::npos,
       "★ the gallery item is titled with the station: " + list.substr(0, 260));
    gallery::Meta m2 = m; m2.lat = NAN; m2.lon = NAN;
    gallery::Store s2; s2.setDir(tmpDir("stations2"));
    { bg::GalleryTap t(s2, m2); t.text("ZCZC BA11\r\nNAV WARNING\r\nNNNN\r\n"); t.finish(false); }
    ok(s2.listJson("navtex", 10).find("Station B") != std::string::npos, "no position, no name in the text → \"Station B\" as before");
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

// ── 7. phase 2: pictures, FT8 summaries, the decoders' demodulators ─────────────────────────────────────────────
static std::vector<uint8_t> be32(uint32_t v) { return { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; }
static std::string readFile(const std::string& p) {
    std::string b; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return b;
    char t[4096]; size_t k; while ((k = std::fread(t, 1, sizeof t, f)) > 0) b.append(t, k); std::fclose(f); return b;
}
static void phase2() {
    std::printf("── phase 2: which decoder on which demodulator ──\n");
    std::string err;
    bg::Config c; c.on = true;
    bg::Profile p; p.centreHz = 8000000; p.vfos = { bg::Vfo{8038100, "lsb", "wefax"} };
    c.profiles = { p };
    ok(!bg::validate(c, 2048000, err) && err.find("WEFAX (120 LPM) does not work on LSB") != std::string::npos && err.find("needs USB") != std::string::npos,
       "WEFAX on LSB is refused, saying why (a mirrored chart is a negative): " + err);
    c.profiles[0].vfos = { bg::Vfo{8038100, "usb", "wefax"}, bg::Vfo{8000000, "usb", "ft8"} };
    ok(bg::validate(c, 2048000, err), "WEFAX + FT8 on USB validate");
    ok(std::fabs(bg::signalHz(c.profiles[0].vfos[0]) - 8040000) < 1, "WEFAX dial 8038.1 USB is the 8040.0 kHz carrier");
    ok(bg::attachMsg(c.profiles[0].vfos[0]).find("\"auto_start\":true") != std::string::npos, "...started on its START TONE, ended on its stop tone");
    bg::Profile iss; iss.centreHz = 145800000; iss.vfos = { bg::Vfo{145800000, "nfm", "sstv"} };
    c.profiles = { iss };
    ok(bg::validate(c, 2048000, err) && std::fabs(bg::signalHz(iss.vfos[0]) - 145800000) < 1, "SSTV on NFM (the ISS, 145.800) validates");
    c.profiles[0].vfos[0].dec = "navtex";
    ok(!bg::validate(c, 2048000, err), "NAVTEX on NFM is refused: " + err);

    std::printf("── WEFAX and SSTV pictures: complete, PARTIAL, and noise not kept ──\n");
    gallery::Store s; s.setDir(tmpDir("pics"));
    gallery::Meta m; m.kind = "wefax"; m.origin = "background"; m.freqHz = 8038100; m.mode = "usb"; m.decoder = "wefax"; m.profile = "Profile 1";
    auto wline = [](uint32_t ln, uint32_t w, uint8_t v) {
        std::vector<uint8_t> f{ 0x01 }; for (auto b : be32(ln)) f.push_back(b); for (auto b : be32(w)) f.push_back(b);
        for (uint32_t i = 0; i < w; i++) f.push_back((uint8_t)(i * 255 / w ^ v)); return f; };
    {
        auto t = bg::makeGalleryTap(s, m);
        const uint8_t st = 0x02, sp = 0x03;
        t->frame(&st, 1);
        for (uint32_t i = 0; i < 200; i++) { auto f = wline(i, 1809, (uint8_t)i); t->frame(f.data(), f.size()); }
        t->frame(&sp, 1);
        t->frame(&st, 1);                                                    // the next chart starts…
        for (uint32_t i = 0; i < 80; i++) { auto f = wline(i, 1809, 0); t->frame(f.data(), f.size()); }
        t->finish(true);                                                     // …and a listener arrives
        auto t2 = bg::makeGalleryTap(s, m);
        t2->frame(&st, 1);
        for (uint32_t i = 0; i < 10; i++) { auto f = wline(i, 1809, 0); t2->frame(f.data(), f.size()); }
        t2->finish(true);
    }
    const std::string wl = s.listJson("wefax", 10);
    int complete = 0, partial = 0;
    for (size_t at = 0; (at = wl.find("\"f\":", at)) != std::string::npos; at++) {
        const std::string item = wl.substr(at, wl.find('}', at) - at);
        if (item.find("\"partial\":true") != std::string::npos) partial++; else complete++;
    }
    ok(complete == 1 && wl.find("1809\xc3\x97" "200") != std::string::npos, "★ a chart ended by its stop tone: one PNG, 1809×200");
    ok(partial == 1 && wl.find("1809\xc3\x97" "80") != std::string::npos, "★ a chart cut off by a listener: saved ONCE, marked partial");
    ok(complete + partial == 2, "...and 10 lines of noise after a false start tone are not kept");
    {
        std::string path, ct; const std::string f = gallery::lineStr(wl, "f");
        const std::string png = s.resolve(f, path, ct) ? readFile(path) : "";
        ok(ct == "image/png" && png.size() > 33 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && png.compare(12, 4, "IHDR") == 0
           && png.compare(png.size() - 8, 4, "IEND") == 0, "it is a PNG (" + std::to_string(png.size()) + " bytes)");
        const uint32_t crcOk = vibegz::crc32((const uint8_t*)png.data() + 12, 17);
        const uint32_t crcIn = (uint32_t)(uint8_t)png[29] << 24 | (uint32_t)(uint8_t)png[30] << 16 | (uint32_t)(uint8_t)png[31] << 8 | (uint8_t)png[32];
        ok(crcOk == crcIn, "...its IHDR CRC checks");
        std::FILE* o = std::fopen((std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/bgdec-wefax.png").c_str(), "wb");
        if (o) { std::fwrite(png.data(), 1, png.size(), o); std::fclose(o); }
    }
    {
        gallery::Meta sm = m; sm.kind = "sstv"; sm.decoder = "sstv"; sm.freqHz = 14230000;
        auto t = bg::makeGalleryTap(s, sm);
        std::vector<uint8_t> st{ 0x07 }; for (auto b : be32(320)) st.push_back(b); for (auto b : be32(256)) st.push_back(b);
        t->frame(st.data(), st.size());
        const std::string mode = "Scottie 1";
        std::vector<uint8_t> mf{ 0x02, 0, (uint8_t)mode.size() }; mf.insert(mf.end(), mode.begin(), mode.end()); t->frame(mf.data(), mf.size());
        for (uint32_t y = 0; y < 256; y++) {
            std::vector<uint8_t> f{ 0x01 }; for (auto b : be32(y)) f.push_back(b); for (auto b : be32(320)) f.push_back(b);
            for (int i = 0; i < 320 * 3; i++) f.push_back((uint8_t)(i + y)); t->frame(f.data(), f.size());
        }
        std::vector<uint8_t> done{ 0x05, 0, 0, 0, 0 }; t->frame(done.data(), done.size());
        t->frame(st.data(), st.size());
        for (uint32_t y = 0; y < 100; y++) {
            std::vector<uint8_t> f{ 0x01 }; for (auto b : be32(y)) f.push_back(b); for (auto b : be32(320)) f.push_back(b);
            for (int i = 0; i < 320 * 3; i++) f.push_back(7); t->frame(f.data(), f.size());
        }
        t->finish(true);
    }
    const std::string sl = s.listJson("sstv", 10);
    ok(sl.find("SSTV \xc2\xb7 Scottie 1 \xc2\xb7 320\xc3\x97" "256") != std::string::npos, "★ an SSTV picture, complete, titled with its mode");
    ok(sl.find("320\xc3\x97" "100") != std::string::npos && sl.find("\"partial\":true") != std::string::npos, "★ a picture cut off is saved, partial, as far as it got");

    std::printf("── FT8 / FT4: a summary, not the spots ──\n");
    ok(callsign::country("G4IRX") == "England" && callsign::country("DL1ABC") == "Germany" && callsign::country("VK2XYZ") == "Australia"
       && callsign::country("F/DL1ABC") == "Germany" && callsign::country("Q0QQQ").empty(), "callsign → country (the app's own table, generated)");
    double la, lo;
    ok(bg::gridToLatLon("IO92", la, lo) && std::fabs(la - 52.5) < 0.01 && std::fabs(lo + 1.0) < 0.01, "IO92 is 52.5 N 1.0 W");
    ok(!bg::gridToLatLon("RR73", la, lo) && !bg::gridToLatLon("73", la, lo), "RR73 / 73 are not grids");
    {
        gallery::Meta fm; fm.kind = "ft8"; fm.origin = "background"; fm.freqHz = 14074000; fm.mode = "usb"; fm.decoder = "ft8"; fm.profile = "Profile 2";
        double sla, slo; bg::gridToLatLon("IO92nh", sla, slo);
        bg::Ft8SummaryTap t(s, fm, sla, slo);
        for (int i = 0; i < 5; i++) t.spot(false, "DL1ABC", "JO31", -5, 14075000);
        t.spot(false, "DL2XYZ", "JN58", -9, 14075500);
        t.spot(true, "VK2XYZ", "QF56", -20, 14081000);
        t.spot(false, "G4IRX", "IO92", 3, 14074500);
        t.spot(false, "Q0QQQ", "RR73", 0, 14074600);
        const std::string txt = t.text(gallery::nowUtc());
        ok(txt.find("Spots heard:       9 (FT8 8, FT4 1) from 5 stations") != std::string::npos, "spots heard, by mode, and stations");
        ok(txt.find("Countries:         3") != std::string::npos, "3 countries (an unknown prefix is not one)");
        ok(txt.find("Furthest:          VK2XYZ (Australia, QF56)") != std::string::npos && txt.find(" km") != std::string::npos, "★ furthest: VK2XYZ, Australia, with km from this server");
        ok(txt.find("Most frequent:     Germany (6 spots)") != std::string::npos, "most frequent country: Germany");
        ok(txt.find("Rarest:            Australia (1 spot)") != std::string::npos, "rarest country: Australia (1 spot, the first of the ties)");
        std::printf("%s", txt.c_str());
        t.finish(true);
        ok(s.listJson("ft8", 10).find("FT8 summary \xc2\xb7 9 spots \xc2\xb7 3 countries \xc2\xb7 furthest Australia") != std::string::npos,
           "★ one summary item in the gallery, titled with the headline");
        bg::Ft8SummaryTap quiet(s, fm, sla, slo);
        quiet.finish(true);
        size_t n = 0; for (size_t at = 0; (at = s.listJson("ft8", 10).find("\"f\":", at)) != std::string::npos; at++) n++;
        ok(n == 1, "a run that heard nothing writes nothing");
    }
}

// ── 7b. ★ V12 beta 4: the FT8 summary is rewritten IN PLACE every 10 min while the run goes on — one card per run ────
static void ft8Periodic() {
    std::printf("── FT8 summary: one item per run, rewritten in place (atomically), only when the counts changed ──\n");
    gallery::Store s; s.setDir(tmpDir("ft8p"));
    gallery::Meta m; m.kind = "ft8"; m.origin = "background"; m.freqHz = 1840000; m.mode = "usb"; m.decoder = "ft8";
    m.liveSpots = std::make_shared<std::atomic<uint64_t>>(0); m.liveCountries = std::make_shared<std::atomic<uint64_t>>(0);
    auto count = [&] { size_t n = 0; const std::string l = s.listJson("ft8", 50); for (size_t at = 0; (at = l.find("\"f\":", at)) != std::string::npos; at++) n++; return n; };
    bg::Ft8SummaryTap t(s, m, 52.3125, -0.875);
    t.spot(false, "G4IRX", "IO92", -5, 1840500); t.spot(false, "DL1ABC", "JO31", -12, 1841000); t.spot(false, "EA4XYZ", "IN80", -15, 1841200);
    ok(m.liveSpots->load() == 3 && m.liveCountries->load() == 3, "★ the live count for the admin line: 3 spots, 3 countries (" + std::to_string(m.liveSpots->load()) + ", " + std::to_string(m.liveCountries->load()) + ")");
    ok(count() == 0, "nothing written before the first 10 minutes");
    t.writeNowForTest();
    const std::string name = t.itemName();
    ok(count() == 1 && !name.empty() && s.listJson("ft8", 5).find("3 spots") != std::string::npos, "★ the first rewrite: ONE item, \"3 spots\"");
    std::string path, ct; s.resolve(name, path, ct);
    struct stat st1{}; ::stat(path.c_str(), &st1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    t.writeNowForTest();
    struct stat st2{}; ::stat(path.c_str(), &st2);
    ok(st1.st_mtime == st2.st_mtime && st1.st_ino == st2.st_ino, "★ counts unchanged → not rewritten");
    t.spot(true, "OH2XX", "KP20", -8, 1842000);
    t.writeNowForTest();
    struct stat st3{}; ::stat(path.c_str(), &st3);
    const std::string l2 = s.listJson("ft8", 5);
    ok(count() == 1 && t.itemName() == name && l2.find("4 spots") != std::string::npos && l2.find("4 countries") != std::string::npos,
       "★ more spots → the SAME item rewritten: one card, \"4 spots · 4 countries\"");
    ok(st3.st_ino != st1.st_ino, "...replaced by a rename (a new inode — never written over in place)");
    { DIR* d = ::opendir(s.dir().c_str()); bool tmp = false; while (dirent* e = d ? ::readdir(d) : nullptr) if (std::string(e->d_name).find(".tmp") != std::string::npos) tmp = true; if (d) ::closedir(d);
      ok(!tmp, "no temp file left behind"); }
    t.finish(false);
    const std::string l3 = s.listJson("ft8", 5);
    ok(count() == 1 && l3.find(name) != std::string::npos, "★ the run's end writes the same item, still one card");
    { std::string body; FILE* f = std::fopen(path.c_str(), "r"); char b[4096]; size_t n; while (f && (n = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, n); if (f) std::fclose(f);
      ok(body.find("Spots heard:       4") != std::string::npos, "the card holds the whole run's figures"); }
    // ★ the item went (the owner cleared the folder) — the next write starts a new one, it never fails silently
    bg::Ft8SummaryTap t2(s, m, NAN, NAN);
    t2.spot(false, "G4IRX", "IO92", -5, 1840500); t2.writeNowForTest();
    const std::string n2 = t2.itemName(); std::string p2; s.resolve(n2, p2, ct); ::unlink(p2.c_str());
    t2.spot(false, "M0ABC", "IO91", -5, 1840500); t2.writeNowForTest();
    ok(!t2.itemName().empty() && t2.itemName() != n2, "an item removed under a run → the next write makes a new one");
}

int main() {
    config();
    phase2();
    ft8Periodic();
    sideband();
    navtexAsm();
    store();
    hostTap();
    gapMidMessage();
    navtexStations();
    runner();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
