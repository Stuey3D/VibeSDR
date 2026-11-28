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
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vibe_bgdecode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <mutex>
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

// ── 4b. ★ V12 beta 4: the gallery in PAGES — offset/limit, the filter's total, the index read once ───────────────────
static void galleryPages() {
    std::printf("── the gallery in pages: offset + limit, the filter's total, an in-memory index ──\n");
    gallery::Store s; s.setDir(tmpDir("pages"));
    for (int i = 0; i < 60; i++) {
        gallery::Meta m; m.kind = i % 3 == 0 ? "ft8" : "navtex"; m.origin = "background"; m.freqHz = 518000; m.mode = "usb";
        m.decoder = m.kind; m.utc = 1792700000 + i; m.title = "item " + std::to_string(i);
        s.add(m, "txt", "body " + std::to_string(i));
    }
    auto num = [](const std::string& j, const char* k) { const size_t p = j.find(std::string("\"") + k + "\":"); return p == std::string::npos ? -1L : std::atol(j.c_str() + p + std::strlen(k) + 3); };
    auto n = [](const std::string& j) { size_t c = 0; for (size_t at = 0; (at = j.find("\"f\":", at)) != std::string::npos; at++) c++; return c; };
    const std::string p1 = s.listJson("", 25, 0), p3 = s.listJson("", 25, 50);
    ok(n(p1) == 25 && num(p1, "total") == 60 && num(p1, "offset") == 0 && p1.find("\"item 59\"") != std::string::npos,
       "page 1: 25 items, newest first, total 60");
    ok(n(p3) == 10 && p3.find("\"item 9\"") != std::string::npos && p3.find("\"item 0\"") != std::string::npos, "page 3: the last 10, down to the oldest");
    const std::string f1 = s.listJson("ft8", 25, 0);
    ok(n(f1) == 20 && num(f1, "total") == 20 && f1.find("\"k\":\"navtex\"") == std::string::npos, "★ filtered: FT8 only, total 20 for the filter");
    ok(num(s.listJson("navtex", 25, 0), "total") == 40, "NAVTEX: total 40");
    const std::string old = s.listJson("", 300);
    ok(n(old) == 60 && old.find("\"total\"") == std::string::npos && num(old, "count") == 60, "★ an old request (no offset): every item, the old shape");
    const unsigned long b0 = s.cacheBuilds();
    for (int i = 0; i < 5; i++) s.listJson("", 25, i * 25);
    ok(s.cacheBuilds() == b0, "★ pages from the cache: the index is not re-read per page");
    gallery::Meta m; m.kind = "navtex"; m.origin = "live"; m.freqHz = 490000; m.mode = "usb"; m.decoder = "navtex"; m.title = "item new";
    s.add(m, "txt", "new");
    const std::string after = s.listJson("", 25, 0);
    ok(s.cacheBuilds() == b0 + 1 && num(after, "total") == 61 && after.find("\"item new\"") != std::string::npos, "an append invalidates it: the new item at once");
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
    // ★★ beta96 the real host end to end: the 21-byte op 0x07 and the op 0x08 SNR slices reach the record
    ok(list.find("\"nqc\":") != std::string::npos && list.find("\"nql\":0") != std::string::npos,
       "★ beta96: the decode-quality split (nqc / nqf / nql) is in the record");
    ok(list.find("\"snr\":") != std::string::npos && list.find("\"tl\":[") != std::string::npos && list.find("\"tc\":[") != std::string::npos,
       "★ beta96: …and the SNR timeline (op 0x08 slices tied to the text): " + list.substr(list.find("\"nqc\"") == std::string::npos ? 0 : list.find("\"nqc\""), 160));
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
    ok(starts(L('L', 489500), "L \xc2\xb7 Pinneberg, Germany"), "L on 490 = Pinneberg");
    ok(starts(L('G', 517500), "G \xc2\xb7 Cullercoats"), "G on 518 = Cullercoats");
    ok(starts(L('V', 517500), "V \xc2\xb7 Oostende"), "V on 518 = Oostende");
    ok(starts(L('L', 517500), "L \xc2\xb7 Rogaland, Norway"), "L on 518 = Rogaland");
    ok(starts(L('J', 517500), "J \xc2\xb7 Stockholm, Sweden"), "J on 518 = Stockholm");
    ok(starts(L('I', 517500), "I \xc2\xb7 Varberg, Sweden"), "I on 518 = Varberg");
    ok(starts(L('N', 517500), "N \xc2\xb7 Orlandet, Norway"), "N on 518 = Orlandet");
    ok(L('M', 517500) == "M \xc2\xb7 Jel\xc3\xb8y \xf0\x9f\x87\xb3\xf0\x9f\x87\xb4 or Casablanca \xf0\x9f\x87\xb2\xf0\x9f\x87\xa6?",
       "M on 518, no evidence: Jel\xc3\xb8y (supplement) or Casablanca: " + L('M', 517500));
    ok(L('W', 517500) == "W \xc2\xb7 Corsen \xf0\x9f\x87\xab\xf0\x9f\x87\xb7 or Valentia \xf0\x9f\x87\xae\xf0\x9f\x87\xaa?", "W, no evidence: both, asked: " + L('W', 517500));
    ok(starts(L('W', 517500, "VALENTIA RADIO GALE WARNING"), "W \xc2\xb7 Valentia, Ireland"), "the text names Valentia");
    ok(starts(L('W', 517500, "CROSS CORSEN"), "W \xc2\xb7 Corsen, France"), "the text names Corsen");
    { const std::string w = L('W', 517500, "ZCZC WB51\nMRSC MALIN, EJM.\nGALE WARNING THURSDAY 22 OCTOBER 2149 UTC 12\nSHANNON\nVIOLENT STORM FORCE 11\nNNNN");
      const std::string tail = " or Valentia \xf0\x9f\x87\xae\xf0\x9f\x87\xaa?";
      ok(w.size() > tail.size() && w.compare(w.size() - tail.size(), tail.size(), tail) == 0, "tonight's W (MALIN not on W/518): still the two"); }
    ok(starts(L('W', 517500, "IRISH COASTAL WARNING\nSUBMARINE CABLE OPERATIONS IN POSITION 51-30.2N 010-45.0W AND 51-41N 010-30W"), "W \xc2\xb7 Valentia, Ireland"), "W off SW Ireland → Valentia");
    ok(starts(L('W', 517500, "AVURNAV BREST 1234\nZONE 48-10N 005-20W\n48-15.5N 005-05.0W"), "W \xc2\xb7 Corsen, France"), "W off Brittany → Corsen");
    ok(starts(L('B', 489500, "ZCZC BA10\nKUSTWACHT OOSTENDE BERICHT\nWRAK OP POSITIE 51-14.13N 002-54.22E\nNNNN"), "B \xc2\xb7 Oostende, Belgium"), "BA10 with Belgian coordinates → Oostende");
    { const std::string r = L('R', 517500, "ZCZC RA21\nNAVAREA IV 0912/26\nCARIBBEAN SEA. PUERTO RICO.\nUNLIGHTED BUOY ADRIFT IN 18-20.5N 066-30.2W AND 18-11N 065-58W.\nNNNN");
      ok(starts(r, "R \xc2\xb7 Isabella, United States") && r.find(" \xc2\xb7 DX 6,") != std::string::npos, "★ the Caribbean warning → Isabella, DX: " + r); }
    ok(starts(L('M', 517500, "SWEDISH NAV WARN 1/26\nSKAGERRAK. KOSTER.\nSOUTH CARDINAL MARK MISSING\nPSN 58-41.9N 010-51.2E"), "M \xc2\xb7 Jel\xc3\xb8y, Norway"),
       "tonight's M: Skagerrak positions → Jel\xc3\xb8y (supplement)");
    // ★★ 2026-10-23 MONDOLFO (489.5 kHz USB, labelled "E · Corsen, France") — the same cases as scripts/test_navtex.ts
    { const std::string IT = "\xf0\x9f\x87\xae\xf0\x9f\x87\xb9";
      const std::string MONDOLFO = "ZCZC EA17\n230440 UTC OCT 26\nMONDOLFO RADIO\n\nCOSTAVURNAV NR. 576/26 OTT 17 (1 RILANCIO)\nMAR ADRIATICO  RAVENNA\n"
          "SULLE SEGUENTI PIATTAFORME IN PSN:\n 44 31.4N A   012 30.6E  GARIBALDI A NR. 4039.2 EFF\n"
          "I SISTEMI DI AUSILIO ALLA NAVIGAZIONE ISTALLATI NON SONO FUNZIONANTI\nNNNN";
      ok(L('E', 489500, MONDOLFO) == "E \xc2\xb7 Mondolfo, Italy " + IT, "\xe2\x98\x85 the Mondolfo message \xe2\x86\x92 Mondolfo, Italy: " + L('E', 489500, MONDOLFO));
      std::string ancona = MONDOLFO; ancona.replace(ancona.find("ZCZC EA17"), 9, "ZCZC TA17"); ancona.replace(ancona.find("MONDOLFO RADIO"), 14, "ANCONA RADIO");
      // ★ 2026-11-01: a name the table does not have is NEVER shown ("Condogno Radio, Italy?" did not exist) — the letter only
      ok(L('T', 489500, ancona) == "Station T", "(b) not in the table, positions + Italian rule out Niton/Tarifa \xe2\x86\x92 the letter only: " + L('T', 489500, ancona));
      ok(L('T', 489500, "ZCZC TA17\nANCONA RADIO\nI SISTEMI DI AUSILIO ALLA NAVIGAZIONE NON SONO FUNZIONANTI SULLE PIATTAFORME") == "Station T",
         "(b) Italian, no positions, an unknown name \xe2\x86\x92 the letter only");
      ok(L('T', 489500, "ZCZC TA17\nWIDGET RADIO\nGALE WARNING\nSTORM IN 30-00N 040-00W").find("Widget") == std::string::npos, "(b) never 'Widget Radio'");
      ok(L('M', 517500, "ZCZC MA01\nOOSTENDERADIO\nWRAK OP POSITIE 51-14.13N 002-54.22E") == "M \xc2\xb7 Suspected Oostende, Belgium \xf0\x9f\x87\xa7\xf0\x9f\x87\xaa",
         "(b) a table station on another letter names itself \xe2\x86\x92 Suspected");
      ok(starts(L('T', 489500, "ZCZC TA17\nWIDGET RADIO\nTHE BUOY IS MISSING AND WILL BE REPLACED FROM MONDAY"), "T \xc2\xb7 Niton, United Kingdom"), "(b) English Niton speaks \xe2\x86\x92 Niton");
      ok(starts(L('T', 489500, "ZCZC TA17\nWIDGET RADIO\nWRECK 50-30N 001-10W"), "T \xc2\xb7 Niton, United Kingdom"), "(b) positions that fit Niton \xe2\x86\x92 Niton");
      const std::string NITON_FR = "ZCZC TA45\n230800 UTC OCT 26\nORIGINE METEO-FRANCE\nBULLETIN METEOROLOGIQUE SPECIAL POUR LA MANCHE\n"
          "AVIS DE GRAND FRAIS A COUP DE VENT SUR LES ZONES ET DANS LE PAS DE CALAIS. MER AGITEE A FORTE ENTRE LE CAP ET LA HAGUE\nNNNN";
      ok(L('T', 489500, NITON_FR) == "T \xc2\xb7 Niton, United Kingdom \xf0\x9f\x87\xac\xf0\x9f\x87\xa7", "\xe2\x98\x85 Niton T in French stays Niton: " + L('T', 489500, NITON_FR));
      ok(starts(L('T', 489500, NITON_FR + "\n49-50N 001-20W 50-10.5N 000-30.0E"), "T \xc2\xb7 Niton, United Kingdom"), "Niton T French with Channel positions");
      ok(starts(L('S', 517500, "NCC-HAMBURG\nNAUT. WARN."), "S \xc2\xb7 Pinneberg, Germany"), "S 518 = Pinneberg (supplement)");
      ok(starts(L('S', 4209500, "GREEK NAVTEX 4209.5\n35-20N 025-10E"), "S \xc2\xb7 Irakleio, Greece"), "HF 4209.5 letter from the supplement");
      ok(navtex::nameable('M', 517500, LA, LO, "ZCZC MA01\nOOSTENDERADIO\nWRAK OP POSITIE 51-14.13N 002-54.22E") && !navtex::nameable('W', 517500, LA, LO, ""),
         "the gallery names a self-named station, never the 'or' pair");
      auto p = navtex::positions(" 44 31.4N A   012 30.6E  GARIBALDI A");
      ok(p.size() == 1 && std::fabs(p[0].first - 44.5233) < 0.01 && std::fabs(p[0].second - 12.51) < 0.01 && navtex::positions("51-10N AND 002-00E").empty(),
         "ITA2: a lost '-' printed as A between the halves; a word is not"); }
    ok(starts(L('L', 489500, "NCC-HAMBURG\nNAUT. WARN. NR. 520\nMUNITIONSFUNDE AUF POSITION 54-19,2N 005-44,2E"), "L \xc2\xb7 Pinneberg, Germany"), "German comma decimals");
    ok(starts(L('J', 517500, "SWEDISH NAV WARN 225/26\nBOTHNIAN SEA. 60-40.1N 017-55.0E"), "J \xc2\xb7 Stockholm, Sweden"), "J with a Baltic position → Stockholm");
    ok(L('L', 489500, "NCC-HAMBURG", -33.9, 18.4).find(" \xc2\xb7 DX 9,") != std::string::npos, "named from far away → DX 9,…: " + L('L', 489500, "NCC-HAMBURG", -33.9, 18.4));
    { auto P = navtex::positions;
      auto one = [&](const char* t, double la, double lo) { auto p = P(t); return p.size() == 1 && std::fabs(p[0].first - la) < 0.01 && std::fabs(p[0].second - lo) < 0.01; };
      ok(one("IN 51-14.13N 002-54.22E.", 51.2355, 2.9037) && one("5114N 00254E", 51.2333, 2.9) && one("51 14.1N 002 54.2E", 51.235, 2.9033)
         && one("54-19,2N  005-44,2E", 54.32, 5.7367) && one("33-55S 018-25E", -33.9167, 18.4167), "positions: every NAVTEX format, as the clients read them");
      ok(P("PSN 58-41.9N 010-51._E").empty() && P("99-10N 010-10E 51-75N 002-00E 51-10N 190-00E").empty() && P("TEL 1234567 2149 UTC 12 CHART 28").empty(),
         "a lost '_', out-of-range values and plain numbers are not positions"); }
    ok(L('B', 489500, "", NAN, NAN) == "Station B", "no position → the letter only");
    ok(starts(L('B', 489500, "OOSTENDERADIO NAV WARNING", NAN, NAN), "B \xc2\xb7 Oostende"), "no position, the text names it");
    ok(starts(L('L', 489500, "NCC-HAMBURG", -33.9, 18.4), "L \xc2\xb7 Pinneberg"), "the text wins wherever the server is");
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

// ── 6b. ★★★ b95 — THE LENOVO'S NAVTEX COLLAPSE (Stuart 11-25: "why has my navtex dropped in performance?") ──────────
//     beta92 (adf09a108) made BgRunner::feedIq TRY-lock its queue and DROP a chunk the worker held; b94 (3962c77a4) holds
//     it instead. A NAVTEX VFO is an RxPipeline, NOT a Band VFO: a dropped chunk is never bridged, the samples either side
//     just abut, so every miss pulls the 100-baud bit clock early by the chunk's length. The Lenovo (RSP1A, 3 MS/s,
//     489.5 + 517.5 USB) went from ~85 % of files with a ZCZC header to ~10 % from the 11-24 01:08 beta92 install until
//     the 11-25 22:11 beta94-2 install, and its bad files read "Start lost … 0% of characters lost" over garbage.
//     Here: the Lenovo's rate, a USB packet a chunk (1 ms), the worker's queue busy for 1 chunk in kBusyEvery.
static std::string navtexThroughBusyQueue(int busyEvery, uint64_t& held, uint64_t& dropped, int& files) {
    gallery::Store s; s.setDir(tmpDir(busyEvery ? "busyq" : "quietq"));
    FileSlots slots("", [] { return 4; });
    const double fs = 3000000, centre = 1200000;   // ★ the Lenovo's rate and its profile's centre
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.slots = &slots; p.store = &s;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{517500, "usb", "navtex"} };
    r.start(p);
    const std::string text = "ZCZC GA41\r\n251830 UTC NOV 26\r\nNITON NAVTEX WZ 1234\r\nDOVER STRAIT\r\n"
                             "LIGHT BUOY 51-01.2N 001-31.8E UNLIT\r\nNNNN\r\n";
    const auto bits = navBits(text);
    const double spb = fs / 100.0;
    const size_t chunk = (size_t)(fs / 1000);   // ★ 1 ms — the RSP's DSP-thread chunk (a USB packet or two)
    std::vector<vibedsp::cf32> iq; iq.reserve(chunk);
    double ph = 0, acc = 0; size_t made = 0; int nChunk = 0;
    std::mt19937 g(11); std::normal_distribution<float> n(0.0f, 0.002f);
    auto flush = [&] {
        if (busyEvery > 0 && ++nChunk % busyEvery == 0) { auto lk = r.holdQueueForTest(); r.feedIq(iq.data(), (int)iq.size()); }
        else r.feedIq(iq.data(), (int)iq.size());
        iq.clear();
        while (r.queuedForTest() > (size_t)(fs * 0.2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // ★ paced: never the queue's own cap
    };
    auto emit = [&](double rf, double samples) {
        acc += samples; const size_t k = (size_t)std::lround(acc);
        const double w = 2 * M_PI * (rf - centre) / fs;
        for (; made < k; made++) {
            ph += w; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            iq.push_back(vibedsp::cf32((float)(0.1 * std::cos(ph)) + n(g), (float)(0.1 * std::sin(ph)) + n(g)));
            if (iq.size() == chunk) flush();
        }
    };
    emit(518000 + 85, fs);
    for (int b : bits) emit(518000 + (b > 0 ? 85 : -85), spb);
    emit(518000 + 85, 2 * fs);
    if (!iq.empty()) flush();
    while (r.queuedForTest() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    held = r.busyHeld(); dropped = r.droppedSamples();
    r.stop();
    const std::string list = s.listJson("navtex", 20);
    files = 0; std::string all;
    for (size_t at = 0; (at = list.find("\"f\":", at)) != std::string::npos; at++) {
        files++;
        std::string p0, ct;
        if (!s.resolve(gallery::lineStr(list.substr(at - 1), "f"), p0, ct)) continue;
        FILE* f = std::fopen(p0.c_str(), "r"); char b[512]; size_t k;
        while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) all.append(b, k);
        if (f) std::fclose(f);
    }
    return all;
}
static void lenovoBusyQueue() {
    std::printf("── ★★★ b95: NAVTEX through a busy background queue — the Lenovo's 3 MS/s RSP, 1 ms chunks ──\n");
    const int kBusyEvery = 50;   // ★ 2 % of chunks: ~1.2 s a minute (the Pi 500 measured 0.3–0.4 s at 8 MS/s)
    uint64_t h0, d0, h1, d1; int f0, f1;
    const std::string quiet = navtexThroughBusyQueue(0, h0, d0, f0);
    /* ★★★ THE REAL FAULT NEEDS NO FORCED BUSY CHUNK: with the beta92 feedIq this very case failed — the worker popping
     *  1 ms chunks collided with the feeder's try-lock by itself (54 chunks, 162 000 samples in ~10 s on the M5) and the
     *  message came out "ZCZC GA41 … NITON N_KXYCQ_OFJFYV0_ … [end lost]". */
    ok(d0 == 0, "a quiet queue: nothing dropped (" + std::to_string((unsigned long long)d0) + " samples; "
       + std::to_string((unsigned long long)h0) + " chunk(s) found it busy by themselves and were held)");
    ok(quiet.find("ZCZC GA41") != std::string::npos && quiet.find("LIGHT BUOY 51-01.2N 001-31.8E UNLIT") != std::string::npos,
       "a quiet queue (no forced busy chunk): the message decodes (" + std::to_string(f0) + " file(s); the queue was busy by itself for "
       + std::to_string((unsigned long long)h0) + " chunk(s), " + std::to_string((unsigned long long)d0) + " samples dropped):\n" + quiet.substr(0, 400));
    const std::string busy = navtexThroughBusyQueue(kBusyEvery, h1, d1, f1);
    ok(h1 > 0, "the queue WAS busy for some chunks (" + std::to_string((unsigned long long)h1) + " held)");
    ok(d1 == 0, "★★★ none of them was dropped (" + std::to_string((unsigned long long)d1) + " samples) — beta92 dropped every one");
    ok(busy.find("ZCZC GA41") != std::string::npos && busy.find("LIGHT BUOY 51-01.2N 001-31.8E UNLIT") != std::string::npos
       && busy.find("Start lost") == std::string::npos,
       "★★★ the SAME message, header and all, through the busy queue (" + std::to_string(f1) + " file(s)):\n" + busy.substr(0, 400));
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
    // ★★ beta98 (Stuart 11-28): 7171 kHz LSB is 40 m's SSTV calling frequency — SSTV's tones decode the same from an LSB
    //    demodulation of an LSB station (the runner's LSB audio path is proven end to end by the NAVTEX LSB VFO above)
    bg::Profile sstv40; sstv40.centreHz = 7100000; sstv40.vfos = { bg::Vfo{7171000, "lsb", "sstv"} };
    c.profiles = { sstv40 };
    ok(bg::validate(c, 2048000, err) && bg::decoderFitsDemod("sstv", "lsb") && std::fabs(bg::signalHz(sstv40.vfos[0]) - 7171000) < 1,
       "★ SSTV on LSB (7171 kHz) validates, its dial is the frequency: " + err);
    ok(!bg::decoderFitsDemod("ft8", "lsb") && !bg::decoderFitsDemod("wefax", "lsb"), "FT8 and WEFAX stay USB only");

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
    std::printf("── ★★★ beta92 a saved WEFAX chart is FINISHED as the client draws it (auto-align + clean paper), background and listener ──\n");
    {
        // A Northwood 8040 chart as received: grey speckle paper (~138), a 7-px margin 300 px in walking −0.06 px/line, a few
        // lines lost on the way. The PNG kept must be exactly vibe_wefax_finish.h's rendering of those lines (that port is held
        // to the TypeScript pixel for pixel by test-wefax-finish), not the raw lines.
        const uint32_t CW = 1809, CH = 700;
        std::mt19937 rg(5);
        std::vector<uint8_t> raw((size_t)CW * CH, 255), got(CH, 0);
        for (uint32_t y = 0; y < CH; y++) {
            if (y == 50 || (y >= 400 && y < 403)) continue;                     // lost lines (filled from the line above)
            got[y] = 1;
            uint8_t* r = &raw[(size_t)y * CW];
            for (uint32_t x = 0; x < CW; x++) { int v = 138 + (int)(rg() % 121) - 60; if (x % 40 >= 20 && x % 40 < 24) v = 10; /* ~10 % ink, as a chart has (test_wefax_crisp) */ r[x] = (uint8_t)std::max(0, std::min(255, v)); }
            const int mg = (int)std::lround(300 - 0.06 * y);
            for (int k = -3; k <= 3; k++) r[((mg + k) % (int)CW + CW) % CW] = 0;
        }
        std::vector<uint8_t> want = raw;
        const auto fr = wefaxfinish::finishChart(want.data(), CW, CH, got.data(), 8038100);
        ok(fr.hasAl && fr.via == wefaxfinish::VIA_MARGIN && fr.crisp, "the finisher finds the margin (shift " + std::to_string(fr.shift) + ") and the paper");
        const std::string wantPng = png::encode((int)CW, (int)CH, 1, want.data()), rawPng = png::encode((int)CW, (int)CH, 1, raw.data());
        for (const char* origin : {"background", "live"}) {
            gallery::Store fs; fs.setDir(tmpDir((std::string("finish-") + origin).c_str()));
            gallery::Meta fm = m; fm.origin = origin; fm.freqHz = 8038100;
            auto t = bg::makeGalleryTap(fs, fm);
            const uint8_t st = 0x02, sp = 0x03;
            t->frame(&st, 1);
            for (uint32_t y = 0; y < CH; y++) {
                if (!got[y]) continue;
                std::vector<uint8_t> f{0x01};
                for (auto b : be32(y)) f.push_back(b);
                for (auto b : be32(CW)) f.push_back(b);
                f.insert(f.end(), raw.begin() + (size_t)y * CW, raw.begin() + (size_t)(y + 1) * CW);
                t->frame(f.data(), f.size());
            }
            t->frame(&sp, 1);
            t->finish(false);
            const std::string fl = fs.listJson("wefax", 10);
            std::string path, ct; const std::string f = gallery::lineStr(fl, "f");
            const std::string png = fs.resolve(f, path, ct) ? readFile(path) : "";
            ok(png == wantPng && png != rawPng, std::string("★ a ") + origin + " chart is saved aligned + crisp (" + std::to_string(png.size()) + " bytes; raw would be " + std::to_string(rawPng.size()) + ")");
        }
        // the margin, after finishing, sits at the left edge; the paper is white
        // the paper as wefaxCrisp defines it: the 70th-percentile grey of the chart
        auto paperOf = [](const std::vector<uint8_t>& v) { size_t h[256] = {0}; for (uint8_t g : v) h[g]++; size_t a = 0; for (int g = 0; g < 256; g++) { a += h[g]; if (a >= v.size() * 7 / 10) return g; } return 255; };
        int edge = 0;
        for (uint32_t y = 0; y < CH; y++) { int d = 0; for (int x = 0; x <= 4; x++) d += want[(size_t)y * CW + x] < 60; edge += d >= 4; }
        const int pw = paperOf(want), pr = paperOf(raw);
        ok(edge >= (int)(CH * 0.95) && pw >= 230,
           "…margin at the left edge on " + std::to_string(edge) + "/" + std::to_string(CH) + " lines; paper " + std::to_string(pr) + " → " + std::to_string(pw));
    }
    std::printf("── ★★ a LISTENER's WEFAX: kept only with a start tone / phasing, or 600 lines (Stuart: \"dead wefax false starts\") ──\n");
    {
        gallery::Store ls; ls.setDir(tmpDir("lpics"));
        gallery::Meta lm = m; lm.origin = "live"; lm.profile.clear();
        auto count = [&](gallery::Store& st) { int n = 0; const std::string l = st.listJson("wefax", 50);
            for (size_t at = 0; (at = l.find("\"f\":", at)) != std::string::npos; at++) n++; return n; };
        auto feed = [&](const DecoderTapPtr& t, uint32_t lines) { for (uint32_t i = 0; i < lines; i++) { auto f = wline(i, 1809, (uint8_t)(i * 7)); t->frame(f.data(), f.size()); } };
        const uint8_t st = 0x02, phasing[2] = { 0x04, 2 }, tone[2] = { 0x04, 1 }, standby[2] = { 0x04, 0 }, image[2] = { 0x04, 3 };
        auto a = bg::makeGalleryTap(ls, lm);                      // tuned onto noise with WEFAX open, then retuned
        a->frame(standby, 2); feed(a, 80); a->frame(image, 2); feed(a, 20); a->finish(true);
        const auto* ga = dynamic_cast<bg::GalleryImageTap*>(a.get());
        ok(count(ls) == 0 && ga && ga->dropped() == 1, "★ 100 lines of noise, no start tone, no phasing: dropped silently (" + std::to_string(count(ls)) + " kept)");
        auto b = bg::makeGalleryTap(ls, lm);                      // a start tone, then 120 lines, then the listener left
        b->frame(&st, 1); feed(b, 120); b->finish(true);
        ok(count(ls) == 1, "★ a start tone, then 120 lines: kept (partial)");
        auto c = bg::makeGalleryTap(ls, lm);                      // no start tone caught, but its phasing was
        c->frame(phasing, 2); feed(c, 90); c->finish(true);
        auto c2 = bg::makeGalleryTap(ls, lm);                     // …or only its start tone's phase report
        feed(c2, 10); c2->frame(tone, 2); feed(c2, 70); c2->finish(true);
        ok(count(ls) == 3, "★ phasing seen (no 0x02), 90 lines — and a start tone seen part-way, 80 lines: both kept");
        auto d = bg::makeGalleryTap(ls, lm);                      // joined a chart part-way: no start of its own, but long
        feed(d, 650); d->finish(true);
        ok(count(ls) == 4, "★ a chart joined part-way: no start tone or phasing, but 650 lines (≥ 600, 5 min at 120 LPM): kept");
        auto e = bg::makeGalleryTap(ls, lm);                      // 599 lines and no start: still noise by the rule
        feed(e, 599); e->finish(true);
        ok(count(ls) == 4, "...and 599 lines with neither: dropped");
        auto f = bg::makeGalleryTap(ls, lm);                      // a chart with a start, then the next one's start tone
        f->frame(&st, 1); feed(f, 200); f->frame(&st, 1); feed(f, 60); f->frame(standby, 2); f->finish(true);
        ok(count(ls) == 6, "★ two charts each after its own start tone: both kept (the second 60 lines, partial)");
        gallery::Meta bm = m; gallery::Store bs; bs.setDir(tmpDir("bpics"));
        auto g = bg::makeGalleryTap(bs, bm);                      // background: a start tone (reported, then its START), 80 lines
        g->frame(standby, 2); g->frame(tone, 2); g->frame(&st, 1); feed(g, 80); g->finish(true);
        ok(count(bs) == 1, "background: a chart begun on its start tone, cut at 80 lines — kept, as before");
        // ★★ a background chart begun OUT OF THE NOISE (no start tone, no phasing: the last report was 'standing by')
        auto h = bg::makeGalleryTap(bs, bm);
        h->frame(standby, 2); h->frame(&st, 1); h->frame(image, 2); feed(h, 300); h->frame(standby, 2); const uint8_t sp = 0x03; h->frame(&sp, 1);
        ok(count(bs) == 1, "★ background, a chart out of the noise that ran 300 lines: dropped (a false start, or too little of a chart)");
        auto k = bg::makeGalleryTap(bs, bm);
        k->frame(standby, 2); k->frame(&st, 1); k->frame(image, 2); feed(k, 700); k->frame(&sp, 1); k->finish(false);
        const std::string bl = bs.listJson("wefax", 10);
        ok(count(bs) == 2 && bl.find("1809\xc3\x97" "700") != std::string::npos && bl.find("\"partial\":true") != std::string::npos,
           "★ …one that ran 700 lines to its stop tone: kept, marked PARTIAL (its top is missing)");
        auto q = bg::makeGalleryTap(bs, bm);
        q->frame(standby, 2); q->frame(&st, 1); q->frame(phasing, 2); feed(q, 120); q->frame(&sp, 1); q->finish(false);
        ok(count(bs) == 3, "…and one whose phasing came after its START (toneless phasing): a chart, kept whole");
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
        t.spot(false, "Q0QQQ", "RR73", 0, 14074600);        // ★ beta92: Q is allocated to nobody — a false decode, not a spot
        const std::string txt = t.text(gallery::nowUtc());
        ok(txt.find("Spots heard:       8 (FT8 7, FT4 1) from 4 stations") != std::string::npos, "spots heard, by mode, and stations (Q0QQQ set aside)");
        ok(txt.find("Countries:         3") != std::string::npos, "3 countries (an unknown prefix is not one)");
        ok(txt.find("Furthest:          VK2XYZ (Australia, QF56)") != std::string::npos && txt.find(" km") != std::string::npos, "★ furthest: VK2XYZ, Australia, with km from this server");
        ok(txt.find("Most frequent:     Germany (6 spots)") != std::string::npos, "most frequent country: Germany");
        ok(txt.find("Rarest:            Australia (1 spot)") != std::string::npos, "rarest country: Australia (1 spot, the first of the ties)");
        std::printf("%s", txt.c_str());
        t.finish(true);
        const std::string fl = s.listJson("ft8", 10);
        ok(fl.find(" UTC \xc2\xb7 8 spots \xc2\xb7 3 countries \xc2\xb7 furthest Australia") != std::string::npos
           && fl.find("FT8+FT4 last 10 min to ") != std::string::npos,
           "★ the run's end: its hour so far (partial) and the last 10 minutes, both titled with the headline — and the modes heard (V12: \"FT8+FT4\")");
        size_t n0 = 0; for (size_t at = 0; (at = fl.find("\"f\":", at)) != std::string::npos; at++) n0++;
        bg::Ft8SummaryTap quiet(s, fm, sla, slo);
        quiet.finish(true);
        size_t n = 0; for (size_t at = 0; (at = s.listJson("ft8", 10).find("\"f\":", at)) != std::string::npos; at++) n++;
        ok(n0 == 2 && n == 2, "two cards for the run; a run that heard nothing writes nothing (" + std::to_string(n0) + ", " + std::to_string(n) + ")");
    }
}

// ── 7c. ★★ V12 beta92: the last-10 card is ONE PER SERVER — two radios (forked processes) on 1840 kHz ──────────────────
static void ft8SharedLast10() {
    std::printf("── beta92: two radios, one last-10 card (each transmission counted once) ──\n");
    const std::string dir = tmpDir("ft8shared");
    static const char* const ids[2] = { "aa11bb22", "cc33dd44" };
    const int64_t T0 = 1792778400 + 5 * 60;
    std::vector<pid_t> kids;
    for (int p = 0; p < 2; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            logradio::setMine(ids[p]);
            gallery::Store s; s.setDir(dir); s.setLimitBytes(50ull << 20); s.setRadioLabel(p ? "RTL" : "RSP1A");
            gallery::Meta m; m.kind = "ft8"; m.origin = p ? "live" : "background"; m.freqHz = 1840000; m.mode = "usb"; m.decoder = "ft8";
            m.profile = p ? "" : "Profile 1";
            int64_t T = T0 + p;   // ★ the same slot's decodes, a second apart
            bg::Ft8SummaryTap t(s, m, 52.3125, -0.875, [&] { return T; });
            t.spot(false, "G4IRX", "IO92", -5, 1840500);       // ★ both radios decode G4IRX and DL1ABC
            t.spot(false, "DL1ABC", "JO31", -12, 1841000);
            t.spot(false, p ? "VK2XYZ" : "EA4XYZ", p ? "QF56" : "IN80", -15, 1841200);   // ★ and one each
            T += 61; t.tick();
            T += 30; t.spot(false, "G4IRX", "IO92", -6, 1840500);   // ★ G4IRX's next transmission, both again
            T += 61; t.tick();
            std::_Exit(0);
        }
        kids.push_back(pid);
    }
    for (pid_t k : kids) { int stt = 0; ::waitpid(k, &stt, 0); }
    logradio::setMine("");
    gallery::Store r; r.setDir(dir); r.setLimitBytes(50ull << 20);
    int cards = 0; std::string name, title;
    for (const auto& n : r.names("ft8")) if (r.field(n, "s").rfind("last10", 0) == 0) { ++cards; name = n; title = r.field(n, "title"); }
    ok(cards == 1, "ONE last-10 card for the server (" + std::to_string(cards) + ")");
    ok(title.find("5 spots") != std::string::npos && title.find("4 countries") != std::string::npos,
       "each transmission once: G4IRX ×2, DL1ABC, EA4XYZ, VK2XYZ = 5 spots, 4 countries (" + title + ")");
    const std::string ri = r.field(name, "ri");
    ok(ri.find(ids[0]) != std::string::npos && ri.find(ids[1]) != std::string::npos, "the card lists both radios (" + ri + ")");
    std::string body; r.readItem(name, body);
    ok(body.find("every radio on this server") != std::string::npos, "it says it is every radio's");
}

// ── 7b. ★★ 2026-10-23: the FT8 summary = a standing LAST 10 MINUTES card + one card per UTC HOUR ──────────────────────
static void ft8Periodic() {
    std::printf("── FT8 summary: the last 10 minutes (one card, rewritten atomically) + an hourly record ──\n");
    gallery::Store s; s.setDir(tmpDir("ft8p"));
    gallery::Meta m; m.kind = "ft8"; m.origin = "background"; m.freqHz = 1840000; m.mode = "usb"; m.decoder = "ft8"; m.profile = "Profile 1";
    m.liveSpots = std::make_shared<std::atomic<uint64_t>>(0); m.liveCountries = std::make_shared<std::atomic<uint64_t>>(0);
    auto list = [&] { return s.listJson("ft8", 50); };
    auto count = [&] { size_t n = 0; const std::string l = list(); for (size_t at = 0; (at = l.find("\"f\":", at)) != std::string::npos; at++) n++; return n; };
    auto body = [&](const std::string& name) { std::string p, ct, b; s.resolve(name, p, ct); FILE* f = std::fopen(p.c_str(), "r"); char x[4096]; size_t k;
                                                while (f && (k = std::fread(x, 1, sizeof x, f)) > 0) b.append(x, k); if (f) std::fclose(f); return b; };
    auto ino = [&](const std::string& name) { std::string p, ct; s.resolve(name, p, ct); struct stat st{}; ::stat(p.c_str(), &st); return (unsigned long long)st.st_ino; };
    const int64_t H18 = 1792778400;            // 2026-10-23 18:00:00 UTC
    int64_t T = H18 + 5 * 60;                  // the run starts at 18:05
    auto clock = [&] { return T; };
    bg::Ft8SummaryTap t(s, m, 52.3125, -0.875, clock);
    t.spot(false, "G4IRX", "IO92", -5, 1840500); t.spot(false, "DL1ABC", "JO31", -12, 1841000); t.spot(false, "EA4XYZ", "IN80", -15, 1841200);
    ok(m.liveSpots->load() == 3 && m.liveCountries->load() == 3, "★ the live count for the admin line: 3 spots, 3 countries");
    T += 30; t.tick();
    ok(count() == 0, "nothing written in the first minute");
    T += 31; t.tick();
    const std::string roll = t.rollingName();
    ok(count() == 1 && !roll.empty() && list().find("FT8 last 10 min \xc2\xb7 3 spots \xc2\xb7 3 countries") != std::string::npos,
       "★ after a minute: ONE card, \"FT8 last 10 min · 3 spots · 3 countries\"");
    ok(list().find("\"s\":\"last10-ft8-1840\"") != std::string::npos, "the card is the server's standing last10 slot for this kind and dial (beta92)");
    const unsigned long long i1 = ino(roll);
    T += 5; t.spot(true, "OH2XX", "KP20", -8, 1842000);       // 18:06:06
    t.tick();
    ok(ino(roll) == i1, "a new spot waits for the minute (at most one rewrite a minute)");
    T += 70; t.tick();                                          // 18:07:16
    const unsigned long long i2 = ino(roll);
    ok(count() == 1 && t.rollingName() == roll && list().find("FT8+FT4 last 10 min \xc2\xb7 4 spots \xc2\xb7 4 countries") != std::string::npos,
       "★ more spots → the SAME card rewritten: \"FT8+FT4 last 10 min · 4 spots · 4 countries\" (an FT4 spot joined: the title names both modes)");
    ok(i2 != i1, "...replaced by a rename (a new inode — never written over in place)");
    T += 70; t.tick();
    ok(ino(roll) == i2, "★ nothing changed in the window → not rewritten");
    { DIR* d = ::opendir(s.dir().c_str()); bool tmp = false; while (dirent* e = d ? ::readdir(d) : nullptr) if (std::string(e->d_name).find(".tmp") != std::string::npos) tmp = true; if (d) ::closedir(d);
      ok(!tmp, "no temp file left behind"); }
    // a NAVTEX message arrives; the next rewrite moves the FT8 card back to the top of the list
    { gallery::Meta nm; nm.kind = "navtex"; nm.origin = "background"; nm.freqHz = 489500; nm.mode = "usb"; nm.decoder = "navtex"; nm.title = "a navtex";
      s.add(nm, "txt", "ZCZC"); }
    T = H18 + 15 * 60 + 30; t.tick();                           // 18:15:30 — the 18:05 spots have aged out, OH2XX (18:06:06) has not
    { const std::string all = s.listJson("", 5); const size_t a1 = all.find(roll), a2 = all.find("a navtex");
      ok(a1 != std::string::npos && a2 != std::string::npos && a1 < a2, "★ the rewritten card lists FIRST (moved to the newest)"); }
    ok(list().find("FT4 last 10 min \xc2\xb7 1 spot \xc2\xb7 1 country") != std::string::npos, "★ the window rolls: the 18:05 spots aged out → \"FT4 last 10 min · 1 spot · 1 country\" (only OH2XX's FT4 left)");
    ok(body(roll).find("Spots heard:       1 (FT8 0, FT4 1) from 1 stations") != std::string::npos, "the card's figures are the window's");
    T = H18 + 18 * 60; t.tick();                                // 18:18 — nothing left in the window
    ok(list().find("FT8+FT4 last 10 min \xc2\xb7 nothing heard since 18:06") != std::string::npos, "an empty window says so, with the last spot's time (named by the run's modes)");
    ok(m.liveSpots->load() == 4, "the admin line's count is the run's, not the window's (4)");
    // the hour closes
    t.spot(false, "K1ABC", "FN42", -18, 1840700);              // 18:18
    T = H18 + 3600 + 5; t.tick();                               // 19:00:05
    { const std::string l = list();
      ok(count() == 2 && l.find("FT8+FT4 18:05\xe2\x80\x93" "19:00 UTC \xc2\xb7 5 spots \xc2\xb7 5 countries \xc2\xb7 furthest United States") != std::string::npos,
         "★ 19:00: the hour's card, \"FT8+FT4 18:05–19:00 UTC · 5 spots · 5 countries · furthest United States\":\n" + l.substr(0, 600));
      ok(l.find("\"partial\":true") != std::string::npos, "a run that began at 18:05 marks its first hour partial"); }
    // a full hour, then the run ends inside the next
    for (int i = 0; i < 4; i++) { T = H18 + 3600 + 600 * (i + 1); t.spot(false, i % 2 ? "VK2XYZ" : "JA1ABC", i % 2 ? "QF56" : "PM95", -20, 1841000); t.tick(); }
    T = H18 + 2 * 3600 + 2; t.tick();                           // 20:00:02
    { const std::string l = list(); const size_t at = l.find("FT8 19:00\xe2\x80\x93" "20:00 UTC \xc2\xb7 4 spots \xc2\xb7 2 countries");
      ok(at != std::string::npos, "★ 20:00: the 19:00 hour's card, 4 spots, 2 countries");
      const size_t ls = l.rfind("{", at), le = l.find("}", at);
      ok(ls != std::string::npos && l.substr(ls, le - ls).find("partial") == std::string::npos, "...a WHOLE hour, not partial"); }
    std::string h19;
    { const std::string l = list(); const size_t at = l.find("FT8 19:00"); const size_t f = l.rfind("\"f\":\"", at); h19 = l.substr(f + 5, l.find('"', f + 5) - f - 5); }
    ok(body(h19).find("19:00 UTC \xc2\xb7 the hour") == std::string::npos && body(h19).find("20:00 UTC \xc2\xb7 the hour") != std::string::npos
       && body(h19).find("Furthest:          VK2XYZ (Australia, QF56)") != std::string::npos && body(h19).find("Spots heard:       4 (FT8 4, FT4 0) from 2 stations") != std::string::npos,
       "★ the hourly card holds the antenna figures: spots, stations, furthest with km, countries:\n" + body(h19).substr(0, 400));
    T = H18 + 2 * 3600 + 300; t.spot(false, "G4IRX", "IO92", -5, 1840500);   // 20:05
    T = H18 + 2 * 3600 + 20 * 60; t.finish(false);                         // 20:20 — the run ends
    { const std::string l = list();
      ok(count() == 4 && l.find("FT8 20:00\xe2\x80\x93" "20:20 UTC \xc2\xb7 1 spot") != std::string::npos, "★ the run's end: the hour so far, partial (20:00–20:20)");
      ok(l.find("FT8+FT4 last 10 min to 20:20 UTC") != std::string::npos && t.rollingName() == roll, "★ ...and the last-10 card kept, \"to 20:20 UTC\" — still ONE card");
      ok(body(roll).find("The last 10 minutes of the run, to 20:20 UTC") != std::string::npos, "the kept card says it is the run's last 10 minutes"); }
    // ★ the next run on the same VFO finds the same card again (a restart, a crash) — never a second one
    {
        int64_t T2 = H18 + 3 * 3600;
        bg::Ft8SummaryTap t2(s, m, 52.3125, -0.875, [&] { return T2; });
        t2.spot(false, "M0ABC", "IO91", -5, 1840500);
        T2 += 61; t2.tick();
        ok(t2.rollingName() == roll && count() == 4 && list().find("FT8 last 10 min \xc2\xb7 1 spot") != std::string::npos, "★ a new run adopts the VFO's card (" + t2.rollingName() + ")");
        gallery::Meta other = m; other.freqHz = 7074000;
        bg::Ft8SummaryTap t3(s, other, 52.3125, -0.875, [&] { return T2; });
        t3.spot(false, "M0ABC", "IO91", -5, 7074500); T2 += 61; t3.tick();
        ok(!t3.rollingName().empty() && t3.rollingName() != roll && count() == 5, "another dial has a card of its own");
        // ★ the card went (the owner cleared the folder) — the next write makes a new one, it never fails silently
        std::string p, ct; s.resolve(roll, p, ct); ::unlink(p.c_str());
        t2.spot(false, "G4IRX", "IO92", -5, 1840500); T2 += 61; t2.tick();
        ok(!t2.rollingName().empty() && t2.rollingName() != roll, "a card removed under a run → the next write makes a new one");
        t3.finish(false); t2.finish(false);
    }
    // ★ the pager still pages: hourly cards are ordinary items
    ok(s.listJson("ft8", 25, 0).find("\"total\":") != std::string::npos, "the 25-a-page list still answers with a total");
    // ★★ V12 2026-10-26: a BAND run's card names the band and every mode it heard (Stuart: the summary cards carry the mode)
    {
        gallery::Store bs; bs.setDir(tmpDir("ft8band"));
        gallery::Meta bm; bm.kind = "ft8"; bm.origin = "live"; bm.freqHz = 14085700; bm.mode = "usb"; bm.decoder = "ft8band";
        int64_t T3 = H18;
        bg::Ft8SummaryTap tb(bs, bm, 52.3125, -0.875, [&] { return T3; });
        tb.spot(std::string("FT8"), "G4IRX", "IO92", -5, 14074500); tb.spot(std::string("FT8"), "DL1ABC", "JO31", -9, 14075000);
        tb.spot(std::string("WSPR"), "K1ABC", "FN42", -24, 14097050);
        T3 += 61; tb.tick();
        const std::string l = bs.listJson("ft8", 10);
        ok(l.find("FT8+WSPR last 10 min \xc2\xb7 3 spots") != std::string::npos, "★ Band: the card's title names its modes, most spots first: \"FT8+WSPR last 10 min · 3 spots\"");
        const std::string txt = tb.text(T3);
        ok(txt.find("Digital spots summary \xc2\xb7 Band 20 m") == 0, "★ Band: the card says which band — \"Digital spots summary · Band 20 m\"");
        ok(txt.find("Spots heard:       3 (FT8 2, FT4 0, WSPR 1) from 3 stations") != std::string::npos, "★ Band: the figures count every mode: " + txt.substr(txt.find("Spots heard"), 70));
        tb.finish(true);
    }
    // ★ JS8 and FT2 (V12 2026-10-26, digimodes/js8.h, ft2.h): counted and named like any mode
    {
        gallery::Store bs; bs.setDir(tmpDir("ft8bandjs8"));
        gallery::Meta bm; bm.kind = "ft8"; bm.origin = "live"; bm.freqHz = 14080800; bm.mode = "usb"; bm.decoder = "ft8band";
        int64_t T3 = H18;
        bg::Ft8SummaryTap tb(bs, bm, 52.3125, -0.875, [&] { return T3; });
        tb.spot(std::string("JS8"), "G4JSN", "IO91", -14, 14079000); tb.spot(std::string("JS8"), "W1JST", "FN31", -18, 14080200);
        tb.spot(std::string("FT2"), "IU8AAA", "JN70", -6, 14084900);
        T3 += 61; tb.tick();
        const std::string l = bs.listJson("ft8", 10);
        ok(l.find("JS8+FT2 last 10 min \xc2\xb7 3 spots") != std::string::npos, "★ JS8 and FT2 spots: the card is \"JS8+FT2 last 10 min · 3 spots\"");
        const std::string txt = tb.text(T3);
        const size_t at = txt.find("Spots heard");
        ok(at != std::string::npos && txt.find("JS8 2", at) != std::string::npos && txt.find("FT2 1", at) != std::string::npos,
           "★ …and its figures count them: " + (at != std::string::npos ? txt.substr(at, 70) : std::string("?")));
        tb.finish(true);
    }
}

// ── 7c. ★ beta 4: the FT8 summary counts only real callsigns and their own grids (the Lenovo's "Furthest: DE … AA00") ──
static void ft8Validation() {
    std::printf("── FT8 summary: real callsigns, their own grids, a nameable country for the furthest ──\n");
    std::string c, g;
    ok(bg::ft8StationFromText("CQ DE G4ABC IO92", c, g) && c == "G4ABC" && g == "IO92", "\"CQ DE G4ABC IO92\" → G4ABC / IO92 (not DE / AA00): " + c + " " + g);
    ok(bg::ft8StationFromText("CQ DX K1ABC FN42", c, g) && c == "K1ABC" && g == "FN42", "\"CQ DX K1ABC FN42\" → K1ABC / FN42");
    ok(bg::ft8StationFromText("CQ K1ABC FN42", c, g) && c == "K1ABC" && g == "FN42", "\"CQ K1ABC FN42\" → K1ABC / FN42");
    ok(bg::ft8StationFromText("G4ABC K1ABC FN42", c, g) && c == "K1ABC" && g == "FN42", "\"CALL1 CALL2 GRID\": the grid is CALL2's");
    ok(bg::ft8StationFromText("G4ABC K1ABC RR73", c, g) && c == "K1ABC" && g.empty(), "RR73 is not a grid");
    ok(bg::ft8StationFromText("G4ABC K1ABC AA00", c, g) && c == "K1ABC" && g.empty(), "AA00 is not a grid");
    ok(bg::ft8StationFromText("EA8/G4ABC/P DL1XYZ -12", c, g) && c == "DL1XYZ" && g.empty(), "a report is not a grid");
    ok(!bg::ft8StationFromText("CQ DE AA00", c, g) && !bg::ft8StationFromText("TNX 73 RRR", c, g) && !bg::ft8StationFromText("XQ#@ 12 ZZ", c, g)
       && !bg::ft8StationFromText("", c, g), "junk lines produce nothing");
    for (const char* w : { "DE", "CQ", "QRZ", "TO", "TU", "TNX", "RR73", "RRR", "73", "IO92", "AA00", "ABC", "1234" })
        ok(!bg::ft8IsCallsign(w), std::string("not a callsign: ") + w);
    for (const char* w : { "G4ABC", "K1ABC", "4X1AB", "9A1A", "EA8/G4ABC/P", "3DA0RU", "M0XYZ/QRP" })
        ok(bg::ft8IsCallsign(w), std::string("a callsign: ") + w);
    // the summary itself, through the tap
    gallery::Store s; s.setDir(tmpDir("ft8v"));
    gallery::Meta m; m.kind = "ft8"; m.origin = "background"; m.freqHz = 1840000; m.mode = "usb"; m.decoder = "ft8";
    m.liveSpots = std::make_shared<std::atomic<uint64_t>>(0);
    {
        bg::Ft8SummaryTap t(s, m, 52.3125, -0.875);
        t.spot(false, "DE", "AA00", -5, 1840500);          // ★ the Lenovo's
        t.spot(false, "CQ", "IO92", -5, 1840500);
        t.spot(false, "73", "", -5, 1840500);
        ok(m.liveSpots->load() == 0, "★ DE / CQ / 73 are not spots (0 counted)");
        t.spotText(false, "CQ DE G4ABC IO92");
        t.spotText(false, "CQ DX K1ABC FN42");
        t.spotText(false, "G4ABC K1ABC RR73");
        t.spot(false, "Q1ABC", "QG00", -20, 1840500);       // ★ a call no country table knows (Q is unallocated), far away
        t.spot(false, "7Q8FAZ", "OB45", -20, 1840500);      // ★ beta92: a Malawi call at -74.5° (Stuart's Pi 500) — kept, unplaced
        t.spotText(false, "junk junk junk");
        ok(m.liveSpots->load() == 4, "4 spots, all real callsigns — Q1ABC is set aside as a false decode (beta92) (" + std::to_string(m.liveSpots->load()) + ")");
        const std::string txt = t.text(gallery::nowUtc());
        ok(txt.find("from 3 stations") != std::string::npos, "3 stations (G4ABC, K1ABC, 7Q8FAZ)");
        ok(txt.find("Malawi") == std::string::npos || txt.find("Furthest:          K1ABC") != std::string::npos, "★ beta92: 7Q8FAZ's impossible grid is never the furthest");
        ok(txt.find("Furthest:          K1ABC") != std::string::npos && txt.find("country unknown") == std::string::npos
           && txt.find("DE (") == std::string::npos && txt.find("AA00") == std::string::npos,
           "★ the furthest is K1ABC (FN42) — never DE/AA00, never a station whose country is unknown:\n" + txt.substr(0, 400));
    }
}

// ── 9. ★★ the background WEFAX VFO's HOURLY LINE — what it heard, so "no signal" and "missed the start" differ ─────
/** Fax audio at 48 kHz: black 1500 Hz, white 2300 Hz (the decoder's carrier 1900 ± 400). */
struct FaxSynth {
    std::vector<float> out; double ph = 0; std::mt19937 rng{7};
    void v(double g) { ph += 2 * M_PI * (1500.0 + 800.0 * g) / 48000.0; out.push_back((float)(0.35 * std::sin(ph))); }
    void noise(int n) { std::uniform_real_distribution<double> u(0, 1); for (int i = 0; i < n; i++) v(u(rng)); }
    void startTone(int lines) { for (int k = 0; k < lines * 24000; k++) v(((k * 600) / 48000) & 1 ? 1.0 : 0.0); }
    void phasing(int lines) { for (int l = 0; l < lines; l++) for (int i = 0; i < 24000; i++) v(i >= 12000 && i < 13200 ? 1.0 : 0.0); }
    void chart(int lines) { for (int l = 0; l < lines; l++) for (int i = 0; i < 24000; i++) v(((i / 900) & 1) ? 0.85 : 0.15); }
};
static void wefaxHourly() {
    std::printf("── ★★ the background WEFAX VFO's hourly log line ──\n");
    gallery::Store gs; gs.setDir(tmpDir("wfhour"));
    auto run = [&gs](const FaxSynth& a, double hourSecs, std::vector<std::string>& lines) {
        std::mutex lm;
        DecoderHost::Env env;
        env.tap = [&gs](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
            gallery::Meta m; m.kind = bg::galleryKindFor(ext, msg); m.origin = "background"; m.freqHz = 4608100; m.mode = "usb"; m.decoder = "wefax";
            return bg::makeGalleryTap(gs, m); };
        env.dialHz = [] { return 4608100.0; };
        env.wefaxHourly = "background VFO 8 (4608.1 kHz USB)";
        env.wefaxHourSecs = hourSecs;
        env.log = [&](const std::string& l) { if (l.rfind("wefax: ", 0) == 0 && l.find("VFO 8") != std::string::npos) { std::lock_guard<std::mutex> g(lm); lines.push_back(l); } };
        auto h = std::make_shared<DecoderHost>(env, "bg:7");
        h->start("wefax", "{\"lpm\":120,\"image_width\":1809,\"carrier\":1900,\"deviation\":400,\"auto_start\":true,\"auto_stop\":true}");
        for (size_t i = 0; i < a.out.size(); i += 4800) {
            h->feed(&a.out[i], (int)std::min<size_t>(4800, a.out.size() - i));
            while (h->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        while (h->queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        h->stop();
    };
    auto field = [](const std::string& l, const std::string& before) { const size_t at = l.find(before); return at == std::string::npos ? -1 : std::atoi(l.c_str() + at + before.size()); };
    {   // noise only: lines read, no start, best tone well short
        FaxSynth a; a.noise(48000 * 90);
        std::vector<std::string> L; run(a, 2.4, L);
        int lines = 0, best = 0; bool begun = false;
        for (auto& l : L) { const size_t at = l.find(" lines read"); size_t b = at; while (b > 0 && std::isdigit((unsigned char)l[b - 1])) --b;
                            lines += std::atoi(l.c_str() + b); best = std::max(best, field(l, "start tone at best "));
                            if (l.find("0 chart(s) begun, 0 saved") == std::string::npos) begun = true; }
        ok(!L.empty() && L.back().find("the part-hour before it stopped on background VFO 8 (4608.1 kHz USB)") != std::string::npos,
           "★ a stopped run says its part-hour: " + (L.empty() ? std::string("(none)") : L.back()));
        ok(!L.empty() && L.back().find("dBFS rms (peak") != std::string::npos && lines == 180,
           "...with the audio level and every line read, across its lines (90 s = 180: " + std::to_string(lines) + ")");
        ok(!L.empty() && best < 7 && !begun, "...and on noise: the start tone at best " + std::to_string(best) + " of 10 (7 start a chart), no chart begun");
    }
    {   // a chart with its start tone, phasing and stop tone — every "hour" (0.4 s of wall clock here) has its line
        FaxSynth a; a.noise(48000 * 20); a.startTone(10); a.phasing(40); a.chart(300); a.startTone(0);
        for (int k = 0; k < 10 * 24000; k++) a.v(((k * 900) / 48000) & 1 ? 1.0 : 0.0);   // the stop tone (450 Hz)
        a.noise(48000 * 20);
        std::vector<std::string> L; run(a, 0.4, L);
        auto before = [](const std::string& l, const std::string& what) {   // the number just before `what`
            const size_t at = l.find(what); if (at == std::string::npos || at == 0) return -1;
            size_t b = at; while (b > 0 && std::isdigit((unsigned char)l[b - 1])) --b; return b == at ? -1 : std::atoi(l.c_str() + b); };
        int begun = 0, bestStart = 0, used = 0;
        for (auto& l : L) { begun += std::max(0, before(l, " chart(s) begun")); bestStart = std::max(bestStart, field(l, "start tone at best "));
                            used += std::max(0, before(l, " used, ")); }
        std::string all; for (auto& l : L) all += "\n      " + l;
        ok(L.size() >= 2, "★ hourly lines while it runs, then the part-hour (" + std::to_string(L.size()) + " lines):" + all.substr(0, 900));
        int saved = 0; for (auto& l : L) saved += std::max(0, before(l, " saved; "));
        ok(bestStart >= 7 && begun == 1 && used == 1 && saved == 1, "★ the chart: start tone at best ≥ 7 of 10 (" + std::to_string(bestStart) + "), 1 chart begun ("
           + std::to_string(begun) + "), its phasing used, 1 saved on its stop tone (" + std::to_string(saved) + ")");
    }
}

// ── 10. ★★ WEFAX STATIONS: several stations' frequencies in ONE decoder slot (Stuart 2026-11-22) ─────────────────
static void wefaxStations() {
    std::printf("── WEFAX stations: the list, the save, the runner (two charts at once, one slot) ──\n");
    namespace ws = wefaxst;
    ok(ws::known(4610000) && ws::known(7880000) && ws::known(13882500) && ws::known(2618500) && ws::known(11086500),
       "Northwood 2618.5 / 4610 / 11086.5 and DWD 7880 / 13882.5 kHz are in the list");
    ok(ws::label(4610000) == "Northwood GYA 4610.0 kHz" && ws::label(17146400) == "New Orleans NMG / Valpara\xc3\xadso CBV 17146.4 kHz"
       && ws::label(7880000) == "Pinneberg (DWD) 7880.0 kHz",
       "labels: " + ws::label(4610000) + " · " + ws::label(17146400) + " · " + ws::label(7880000) + " (a call sign per frequency is left out)");
    ok(std::fabs(ws::dialOf(4610000) - 4608100) < 0.5, "the dial is 1.9 kHz below (4610.0 → 4608.1 USB)");
    ok(!ws::known(5807000), "Wellington ZKLF (stopped 2023) is not offered");
    const std::string hfOnly = ws::stationsJson([](double lo, double hi) { return lo >= 3000000 && hi <= 9000000; });
    ok(hfOnly.find("\"id\":\"gya\"") != std::string::npos && hfOnly.find("[4610000,8040000]") != std::string::npos
       && hfOnly.find("2618500") == std::string::npos && hfOnly.find("\"id\":\"kvm70\"") == std::string::npos,
       "★ the JSON offers only what the radio reaches (3–9 MHz: Northwood 4610 + 8040, no 2618.5, no Honolulu)");

    bg::Config c; c.on = true; bg::Profile p; p.centreHz = 4250000;
    bg::Vfo v; v.hz = 0; v.mode = "usb"; v.dec = "wefax-stations"; v.fax = { 4235000, 4317900, 4346000 };
    p.vfos = { v }; c.profiles = { p };
    std::string err;
    ok(bg::validate(c, 250000, err), "a WEFAX stations VFO, NMF 4235 + NMG 4317.9 + NMC 4346, centre 4250 at 250 kS/s: saved (" + err + ")");
    bg::Config back; ok(bg::parse(bg::toJson(c), back, err) && back.profiles[0].vfos[0].fax.size() == 3 && back.profiles[0].vfos[0].fax[1] == 4317900,
       "its ticks round-trip: " + bg::toJson(c).substr(0, 140));
    ok(bg::toJson(c).find("\"fax\":[4235000,4317900,4346000]") != std::string::npos, "...stored as \"fax\":[Hz…]");
    auto refused = [&](std::vector<double> f, double centre, const std::string& says) {
        bg::Config k = c; k.profiles[0].vfos[0].fax = f; k.profiles[0].centreHz = centre;
        std::string e; const bool r = !bg::validate(k, 250000, e) && e.find(says) != std::string::npos;
        ok(r, "refused, in words: " + e); };
    refused({}, 4250000, "tick at least one WEFAX station frequency");
    refused({ 4235000, 4236000 }, 4250000, "4236.0 kHz is not a WEFAX station frequency in the list");
    refused({ 4235000, 4235000 }, 4250000, "ticked twice");
    refused({ 8040000 }, 4250000, "none of the ticked WEFAX frequencies is inside this radio's window");
    refused({ 2618500, 3855000, 4610000, 7880000, 8040000, 11086500, 13882500, 4235000, 6340500 }, 4250000, "one VFO runs at most 8");
    { bg::Config k = c; k.profiles[0].vfos[0].mode = "lsb"; std::string e; ok(!bg::validate(k, 250000, e) && e.find("run on USB") != std::string::npos, "LSB refused: " + e); }
    ok(bg::attachMsg(v).find("\"auto_start\":true") != std::string::npos, "each channel starts on its start tone, as a WEFAX VFO does");

    // ── the runner: two charts on two stations at once, one slot ──
    const std::string dir = tmpDir("faxst");
    FileSlots sl(dir, [] { return 4; });
    gallery::Store gs; gs.setDir(tmpDir("faxst-gal"));
    std::vector<std::string> logs; std::mutex lm;
    auto logged = [&](const std::string& k) { std::lock_guard<std::mutex> g(lm); for (auto& l : logs) if (l.find(k) != std::string::npos) return true; return false; };
    const double fs = 250000, centre = 4250000;
    bg::BgRunner r;
    bg::BgRunner::Params rp; rp.sampleRate = fs; rp.centreHz = centre; rp.fftSize = 8192; rp.slots = &sl; rp.store = &gs;
    rp.profile = p; rp.log = [&](const std::string& t) { std::lock_guard<std::mutex> g(lm); logs.push_back(t); };
    r.start(rp);
    ok(r.decodersRunning() == 1 && sl.inUse() == 1, "★ ONE decoder slot for the VFO (" + std::to_string(sl.inUse()) + " in use)");
    ok(logged("decoding 2 of 3 WEFAX frequencies"), "logged: decoding 2 of 3 (NMC 4346 is outside the window)");
    // two fax transmitters: NMF a chart of vertical bars, NMG one of finer bars — each start tone, phasing, chart, stop tone
    struct Tx { double centre; int period; double ph = 0; };
    Tx tx[2] = { { 4235000, 900 }, { 4317900, 300 } };
    const int spl = (int)(fs / 2);
    std::vector<vibedsp::cf32> iq;
    auto gray = [&](const Tx& t, long k) -> double {   // what transmitter t sends at sample k
        const long line = k / spl, i = k % spl;
        if (line < 10) return ((k * 600) / (long)fs) & 1 ? 1.0 : 0.0;                    // start tone
        if (line < 50) return i >= spl / 3 && i < spl / 3 + spl / 20 ? 1.0 : 0.0;          // phasing
        if (line < 170) return ((i / t.period) & 1) ? 0.85 : 0.15;                         // the chart
        if (line < 180) return ((k * 900) / (long)fs) & 1 ? 1.0 : 0.0;                     // stop tone
        return -1;                                                                          // off air
    };
    for (long k = 0; k < 190L * spl; k++) {
        vibedsp::cf32 x(0, 0);
        for (auto& t : tx) {
            const double g = gray(t, k);
            if (g < 0) continue;
            t.ph += 2 * M_PI * (t.centre - 1900 + 1500 + 800 * g - centre) / fs; if (t.ph > M_PI) t.ph -= 2 * M_PI; if (t.ph < -M_PI) t.ph += 2 * M_PI;
            x += vibedsp::cf32((float)(0.04 * std::cos(t.ph)), (float)(0.04 * std::sin(t.ph)));
        }
        iq.push_back(x);
        if (iq.size() == 8192) {
            while (r.queuedForTest() > 60000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            r.feedIq(iq.data(), (int)iq.size()); iq.clear();
        }
    }
    if (!iq.empty()) r.feedIq(iq.data(), (int)iq.size());
    for (int t = 0; t < 200 && r.queuedForTest() > 0; t++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const std::string st = r.statusJson();
    ok(st.find("\"fax\":[{\"hz\":4235000,\"label\":\"Boston NMF 4235.0 kHz\"") != std::string::npos && st.find("\"state\":\"out\"") != std::string::npos,
       "★ the admin line lists each frequency and its state: " + st.substr(st.find("\"fax\""), 260));
    const std::string wl = gs.listJson("wefax", 10);
    ok(wl.find("WEFAX \xc2\xb7 Boston NMF 4235.0 kHz \xc2\xb7 1809\xc3\x97") != std::string::npos && wl.find("\"hz\":4233100") != std::string::npos,
       "★ NMF's chart: its own item, titled by station and frequency, filed at its dial 4233.1");
    ok(wl.find("WEFAX \xc2\xb7 New Orleans NMG 4317.9 kHz \xc2\xb7 1809\xc3\x97") != std::string::npos && wl.find("\"hz\":4316000") != std::string::npos,
       "★ NMG's chart, at the same time, in the same slot: its own item (" + wl.substr(0, 300) + ")");
    ok(wl.find("\"partial\":true") == std::string::npos, "...both ended on their stop tones (not partial)");
    r.retarget(4330000, 0, false);
    { const std::string s2 = r.statusJson();
      ok(r.decodersRunning() == 1 && sl.inUse() == 1 && s2.find("{\"hz\":4235000,\"label\":\"Boston NMF 4235.0 kHz\",\"state\":\"out\"}") != std::string::npos
         && s2.find("{\"hz\":4346000,\"label\":\"Point Reyes NMC 4346.0 kHz\",\"state\":\"running\"}") != std::string::npos,
         "★ the radio moves to 4330: still one slot — NMF stops (out of the window), NMC starts"); }
    r.stop();
    ok(sl.inUse() == 0, "stopped: the slot back");
}

int main() {
    config();
    phase2();
    ft8Periodic();
    ft8SharedLast10();
    ft8Validation();
    sideband();
    navtexAsm();
    store();
    galleryPages();
    hostTap();
    gapMidMessage();
    wefaxHourly();
    wefaxStations();
    navtexStations();
    runner();
    lenovoBusyQueue();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
