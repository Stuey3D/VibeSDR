// ★★★ "Remote access to server settings" on Android (beta88) — vibe_android_setup.h, the translation between the phone's
// settings and the /setup page's config. What must hold:
//   1. The JSON reader takes what org.json and JSON.stringify write (escapes, \u, surrogate pairs, nesting) and refuses
//      junk, truncation, control characters and absurd nesting.
//   2. Android → page → Android is the identity for every supported control (a save of an untouched page changes nothing).
//   3. A save carries ONLY supported controls: a governor, a port, a Wi-Fi network, a second radio are ignored.
//   4. The phone's own rules hold: resume = pause + 20, landing 0 = absent (null in the patch), PIN digits only ≤ 12 and
//      left alone when the page does not send it, a blank name refused, numbers clamped.
//   5. The device facts come through untouched, for the page's read-only card.
#include "vibe_android_setup.h"
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <string>

using namespace vibe::androidsetup;

static int fails = 0;
static void ok(bool c, const char* what, const std::string& extra = "") {
    std::printf("   %s %s %s\n", c ? "ok  " : "FAIL", what, extra.c_str());
    if (!c) ++fails;
}

static J parsed(const std::string& s) { J j; parse(s, j); return j; }

static std::string slurp(const char* path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int argc, char** argv) {
    // ★ Two helper modes for scripts/test-setup-android-e2e.mjs: the page's real save goes through THIS translation.
    if (argc == 3 && std::string(argv[1]) == "--page") { std::printf("%s", pageConfig(slurp(argv[2])).c_str()); return 0; }
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--patch") {
        std::string patch, err; bool restart = false;
        if (!settingsPatch(slurp(argv[2]), argc == 4 ? slurp(argv[3]) : std::string(), patch, restart, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::printf("%s", patch.c_str()); return 0;
    }
    std::printf("== the JSON reader\n");
    {
        J j;
        ok(parse("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\\"y\\\\z\\n\\u00e9\\ud83d\\ude00\"}", j), "parses escapes, arrays, literals");
        ok(j.get("c") && j.get("c")->s == std::string("x\"y\\z\n\xc3\xa9\xf0\x9f\x98\x80"), "decodes \\u and a surrogate pair to UTF-8");
        ok(!parse("{\"a\":1", j), "refuses a truncated object");
        ok(!parse("{\"a\":1} x", j), "refuses trailing junk");
        ok(!parse("{\"a\":\"line\nbreak\"}", j), "refuses a raw control character in a string");
        ok(!parse("\"\\ud83d\"", j), "refuses a lone high surrogate");
        std::string deep(40, '['); deep += std::string(40, ']');
        ok(!parse(deep, j), "refuses nesting deeper than 32");
        ok(parse("  {\"n\":-1.5e3}  ", j) && j.get("n")->n == -1500, "numbers and surrounding whitespace");
        J r; parse(dump(parsed("{\"s\":\"a\\u0001b\\\"\"}")), r);
        ok(r.get("s") && r.get("s")->s == std::string("a\x01" "b\""), "dump → parse round-trips control characters and quotes");
    }

    // A phone's settings, as VibeServerBoot reads them (nativeServerConfig's keys).
    const std::string settings = R"({
        "name":"Allotment Moto","advertise":true,"landingMessage":"Hello","landingLinkUrl":"https://example.org",
        "landingLinkLabel":"Club","uncompressedAudio":1,"trustedProxies":"10.0.0.0/8","oneRadioPerIp":false,
        "decoderMax":3,"galleryMb":250,"galleryPublic":false,"gallerySaveLive":true,"batteryPauseAt":15,"batteryResumeAt":35,
        "pskrOn":true,"pskrCall":"G4ABC","pskrAntenna":"Discone on the mast","wsprnetOn":true,"wsprnetCall":"G4ABC","dmrIdLookup":true,
        "radioLabel":"V4 on the mast","antenna":"Discone","maxUsers":4,"askBeforeTuning":true,"zoomSpectrum":false,
        "centerFreq":198000,"mode":"am","sessionLimitMin":30,"sessionLimitSoft":true,"idleKickMin":10,"nbWide":2,
        "vfoB":0,"idleGraceSec":120,"pin":"123456","adminPassword":"never-on-the-page","sampleRate":2400000
    })";
    const std::string raw = std::string("{\"settings\":") + settings +
        ",\"device\":{\"batteryOptimisation\":\"off\",\"startOnPower\":null,\"usbPermission\":true},"
        "\"radio\":{\"driver\":\"rtl\",\"model\":\"RTL-SDR Blog V4\"}}";

    std::printf("== Android → page\n");
    const std::string page = pageConfig(raw);
    J pg; ok(parse(page, pg), "the page config is JSON", page.substr(0, 60));
    ok(pg.get("host") && pg.get("host")->s == "android", "says it is an Android server");
    ok(pg.get("name")->s == "Allotment Moto" && pg.get("maxRadiosPerIp")->n == 0, "server fields (name, one-radio-per-IP off → 0)");
    const J& r0 = pg.get("radios")->a.at(0);
    ok(r0.get("landingFreq")->n == 198000 && r0.get("demodMode")->s == "am", "landing frequency and mode");
    ok(r0.get("users")->n == 4 && r0.get("idleGrace")->n == 120 && r0.get("vfoB")->n == 0, "users, idle grace, VFO B");
    ok(r0.get("driver")->s == "rtl", "the radio's driver, so the page branches on the radio");
    ok(page.find("never-on-the-page") == std::string::npos, "the admin password is NOT in the page config");
    const J* sup = pg.get("android")->get("supported");
    { Platform rtl; ok(sup && sup->a.size() == supportedControls(rtl).size(), "the supported control list (this radio's)"); }
    ok(pg.get("android")->get("device")->get("usbPermission")->b, "device facts carried through for the read-only card");
    ok(pageConfig("not json").empty(), "a bad raw config gives an empty answer, not a crash");

    std::printf("== page → Android: an untouched save changes nothing\n");
    {
        // The page posts what it was given (plus its own extra fields), no PIN typed: collectRadio omits "pin".
        J body = pg;
        J& rr = body.getMut("radios")->a[0];
        J pinless = J::obj();
        for (auto& kv : rr.o) if (kv.first != "pin") pinless.set(kv.first, kv.second);
        rr = pinless;
        body.set("cpuGovernor", J::str("powersave"));
        body.set("port", J::num(8080));
        body.set("restart", J::boolean(true));
        J second = J::obj(); second.set("serial", J::str("00000002")); second.set("users", J::num(9));
        body.getMut("radios")->a.push_back(second);
        std::string patch, err; bool restart = false;
        ok(settingsPatch(dump(body), raw, patch, restart, err), "the save is accepted", err);
        ok(restart, "restart:true is passed on");
        J pj; parse(patch, pj);
        J st; parse(settings, st);
        int diff = 0; std::string which;
        for (auto& kv : pj.o) {
            const J* was = st.get(kv.first);
            if (!was) continue;   // ★ a key this older fixture does not set comes back at its default (the full fixture below checks them)
            bool eq = dump(*was) == dump(kv.second);
            if (!eq && kv.first == "batteryResumeAt") eq = true;   // checked below: the phone's rule re-derives it
            if (!eq) { ++diff; which += kv.first + " "; }
        }
        ok(diff == 0, "every patched key equals the phone's value (identity)", which);
        ok(!pj.get("cpuGovernor") && !pj.get("port") && !pj.get("pin"), "unsupported fields and an untyped PIN are not in the patch");
        ok(pj.get("maxUsers") && pj.get("maxUsers")->n == 4, "the SECOND radio's users did not leak into the phone's one radio");
        ok(pj.get("batteryResumeAt")->n == 35, "resume = pause + 20, the phone's rule");
    }

    std::printf("== page → Android: the phone's rules\n");
    {
        std::string patch, err; bool restart = true;
        ok(settingsPatch(R"({"radios":[{"landingFreq":0,"demodMode":"am"}]})", patch, restart, err), "landing 0 accepted", err);
        J pj; parse(patch, pj);
        ok(pj.get("centerFreq") && pj.get("centerFreq")->t == J::Null && pj.get("mode")->t == J::Null,
           "landing 0 → centerFreq/mode REMOVED (null), never 0 Hz");
        ok(!restart, "no restart unless the page asks (Save radio settings)");

        ok(settingsPatch(R"({"radios":[{"pin":""}]})", patch, restart, err) && parsed(patch).get("pin")->s.empty(),
           "the clear-PIN tick sends an empty PIN, which clears it");
        ok(settingsPatch(R"({"radios":[{"pin":"12-34 56"}]})", patch, restart, err) && parsed(patch).get("pin")->s == "123456",
           "a PIN keeps its digits only");
        ok(!settingsPatch(R"({"radios":[{"pin":"abcd"}]})", patch, restart, err), "a PIN with no digits is refused", err);
        ok(!settingsPatch(R"({"radios":[{"pin":"1234567890123"}]})", patch, restart, err), "a 13-digit PIN is refused", err);
        ok(!settingsPatch(R"({"name":"   "})", patch, restart, err), "a blank name is refused", err);
        ok(!settingsPatch(R"({"radios":[{"landingFreq":1000,"demodMode":"<script>"}]})", patch, restart, err),
           "an unknown demodulator is refused", err);
        ok(settingsPatch(R"({"radios":[{"users":500,"nbWide":-3,"idleKickMin":1e9}],"uncompressed":7})", patch, restart, err), "out-of-range numbers accepted…");
        J c = parsed(patch);
        ok(c.get("maxUsers")->n == 50 && c.get("nbWide")->n == 0 && c.get("idleKickMin")->n == 1440 && c.get("uncompressedAudio")->n == 2,
           "…and clamped to the phone's ranges", patch);
        ok(settingsPatch(R"({"batteryPauseAt":0})", patch, restart, err) && parsed(patch).get("batteryResumeAt")->n == 40,
           "no pause → resume 40, the phone's default");
        ok(!settingsPatch("[1,2]", patch, restart, err) && !settingsPatch("{", patch, restart, err), "non-object bodies are refused");
        std::string big = "{\"landingMessage\":\"" + std::string(5000, 'x') + "\"}";
        ok(settingsPatch(big, patch, restart, err) && parsed(patch).get("landingMessage")->s.size() == 500, "text is cut to the phone's length");
    }

    // ═══ beta88, every setting remote ═══════════════════════════════════════════════════════════════════════════════
    // A phone's settings with EVERY remote key at a value that is not its default, for each kind of radio.
    auto fullSettings = [](const std::string& extra) {
        return std::string(R"({
        "name":"Allotment Moto","advertise":false,"landingMessage":"Hello","landingLinkUrl":"https://example.org",
        "landingLinkLabel":"Club","uncompressedAudio":1,"trustedProxies":"10.0.0.0/8","oneRadioPerIp":false,
        "decoderMax":3,"rttySkimWorkers":2,"galleryMb":250,"galleryPublic":false,"gallerySaveLive":false,"communityMap":true,"communityOff":"vor,aprs",
        "batteryPauseAt":15,"batteryResumeAt":35,
        "pskrOn":true,"pskrCall":"G4ABC","pskrAntenna":"Discone on the mast","wsprnetOn":true,"wsprnetCall":"G4ABC","dmrIdLookup":true,
        "serverLocation":{"mode":"manual","lat":52.25,"lon":-0.9,"label":"Moulton","country":"GB","moving":true},
        "publicName":"Moulton V4","publicShareSec":86400,"webServer":true,"compressAudio":false,"advanced":true,
        "radioLabel":"V4 on the mast","antenna":"Discone","maxUsers":4,"askBeforeTuning":true,"zoomSpectrum":false,
        "radioUse":"locked","lockedCentre":100000000,"spectrogram":true,
        "centerFreq":198000,"mode":"am","sessionLimitMin":30,"sessionLimitSoft":true,"idleKickMin":10,"nbWide":2,
        "vfoB":0,"idleGraceSec":120,"pin":"123456","adminPassword":"never-on-the-page",
        "sampleRate":2048000,"lockedRate":2048000,"rateLock":true,"maxFftRate":20,
        "blockedModes":"drm,ft8","dabRateBoost":true,"dabScanLabels":1,
        "gainLimits":"all:250,fm:150","gainLocks":"fm","gainSplits":"fm:60","gainCurves":"fm:1",
        "allowRanges":"fm,mw","blockRanges":"air",
        "landingDabChannel":5,"landingDabSid":49537,"landingDabService":"BBC R4","landingAdsb":true,"adsbKeepRunning":true,"landingMap":"avmar","landingModes":"adsb","mapKeep":true,
        "fmDeemph":50,"fmStereo":false,"fmNr":true,"fmIms":false,"fmCeq":false,"fmAutoBw":false,"fmNb":false,"fmRemember":true,
        "antennaIcon":"discone","antennaRanges":"25-1300","antennaFilters":"fm","antennaNames":"A=Loop",
        "antennaSwitch":"{\"enabled\":true,\"host\":\"10.0.0.9\",\"port\":1883,\"antennas\":[{\"name\":\"Loop\",\"cmd\":\"a\"},{\"name\":\"Dipole\",\"cmd\":\"b\"}]}",
        "antennaMap":"hf Loop, fm Dipole","antennaLocked":true,
        "bgDecode":"{\"on\":true,\"profiles\":[]}","dxScan":"{\"on\":true}",
        "rawIq":1,"rawIqMax":1,"rawIqLanMaxHz":"250000",
        "converterOffsetHz":-125000000,"converterInputLoHz":1000000,"converterInputHiHz":30000000)") + extra + "}";
    };
    auto rawFor = [](const std::string& st, const char* driver, const std::string& dev) {
        return std::string("{\"settings\":") + st + ",\"device\":{" + dev + "},\"radio\":{\"driver\":\"" + driver + "\",\"model\":\"x\"}}";
    };
    // The deep equality of two values, objects in any order.
    std::function<bool(const J&, const J&)> same = [&](const J& a, const J& b) -> bool {
        if (a.t != b.t) return false;
        switch (a.t) {
            case J::Null: return true; case J::Bool: return a.b == b.b; case J::Num: return std::fabs(a.n - b.n) < 1e-6;
            case J::Str: return a.s == b.s;
            case J::Arr: { if (a.a.size() != b.a.size()) return false; for (size_t i = 0; i < a.a.size(); ++i) if (!same(a.a[i], b.a[i])) return false; return true; }
            case J::Obj: { if (a.o.size() != b.o.size()) return false; for (auto& kv : a.o) { const J* x = b.get(kv.first); if (!x || !same(kv.second, *x)) return false; } return true; }
        }
        return false;
    };
    // What the page's own collect()/collectRadio() add on a phone (androidSrvCollect / androidRadioCollect).
    auto asPagePosts = [](const J& page) {
        J body = page;
        const J* srv = page.get("android")->get("server");
        for (const char* k : { "webServer", "compressAudio", "advanced" }) body.set(k, *srv->get(k));
        J& rr = body.getMut("radios")->a[0];
        J pinless = J::obj();
        for (auto& kv : rr.o) if (kv.first != "pin") pinless.set(kv.first, kv.second);
        pinless.set("androidPage", J::boolean(true));
        rr = pinless;
        body.set("restart", J::boolean(true));
        return body;
    };
    struct Case { const char* driver; std::string extra; std::string dev; };
    const std::vector<Case> cases = {
        { "rtl", R"(,"adsb256":true,"ism256":true,"ifBwLimits":"fm:1400","restGain":250,"agcLock":true,"rtlAgc":true,"tunerBwAuto":true,"ppm":3,"biasT":true,"autoDirectSampling":true,"directSamplingBelowHz":20000000,"directSampling":2)",
          "\"tunnelSupported\":true,\"listing\":{\"wanted\":false}" },
        { "sdrplay", R"(,"ppm":-2,"biasT":true,"rspVibeAgc":2,"rfNotch":true,"dabNotch":true,"rspAmNotch":true,"rspHdr":true,"rspExtRef":true,"rspDabDecim":true,"rfAgc":true,"autoNotch":true,"userNotch":false,"dabAgcOverride":false,"dabAgcTarget":-30,"agcSet":-40,"agcSetLock":true)",
          "\"tunnelSupported\":true" },
        { "airspyhf", R"(,"ppb":-1200,"lockedRate":912000,"sampleRate":912000)", "\"tunnelSupported\":true" },
        { "hackrf", R"(,"biasT":true,"lockedRate":2000000,"sampleRate":2000000)", "\"lite\":true,\"tunnelSupported\":false" },
        { "icom", R"(,"icomBaud":19200,"icomCiv":148,"icomScopeSpeed":2,"icomTuneLock":true,"icomPowerOn":true,"icomAutoOff":false,"icomOffDelayMin":15)",
          "\"tv\":true" },
    };
    // Keys a save of the untouched page legitimately does not carry back for that radio (not offered there), or derives.
    std::printf("== every setting, every radio: Android → page → Android is the identity\n");
    for (const auto& c : cases) {
        const std::string st = fullSettings(c.extra);
        const std::string rw = rawFor(st, c.driver, c.dev);
        J pgc; ok(parse(pageConfig(rw), pgc), (std::string(c.driver) + ": page config").c_str());
        std::string patch, err; bool restart = false;
        ok(settingsPatch(dump(asPagePosts(pgc)), rw, patch, restart, err), (std::string(c.driver) + ": the untouched save is accepted").c_str(), err);
        J pj; parse(patch, pj); J sj; parse(st, sj);
        std::string differs, unsent;
        for (auto& kv : pj.o) {
            const J* was = sj.get(kv.first);
            if (kv.first == "batteryResumeAt" || kv.first == "publicListed") continue;
            if (!was || !same(*was, kv.second)) differs += kv.first + "=" + dump(kv.second) + " ";
        }
        ok(differs.empty(), (std::string(c.driver) + ": every key the save carries equals the phone's value").c_str(), differs);
        // …and every key of the phone's that the save does NOT carry is one this radio is not offered.
        Platform pf = platformOf(parsed(rw));
        for (auto& kv : sj.o) {
            if (pj.get(kv.first) || kv.first == "adminPassword" || kv.first == "pin") continue;
            unsent += kv.first + " ";
        }
        const std::string u = unsent;
        auto has = [&](const char* k) { return u.find(std::string(k) + " ") != std::string::npos; };
        if (pf.driver == D_RTL) ok(u == "dabScanLabels ", "rtl: every setting comes back from the page (the DAB label scan is Lite's)", u);
        if (pf.driver == D_RSP) ok(u == "dabScanLabels ", "sdrplay: every setting comes back from the page (the DAB label scan is Lite's)", u);
        if (pf.driver == D_AHF) ok(!has("ppb") && has("converterOffsetHz") && !has("name"), "airspyhf: the HF+'s calibration comes back; no converter (none on an HF+)", u);
        if (pf.driver == D_HRF) ok(!has("dabScanLabels") && has("rawIq") && has("publicName"), "hackrf on Lite: no raw IQ, no listing (no tunnel binary)", u);
        if (pf.driver == D_ICOM) ok(!has("icomBaud") && !has("icomOffDelayMin") && has("lockedRate") && has("batteryPauseAt") && has("nbWide"),
                                    "icom on a TV: the Icom's own come back; no capture width, no battery, no blanker", u);
        ok(pgc.get("android")->get("device") && page.find("never-on-the-page") == std::string::npos, (std::string(c.driver) + ": no admin password").c_str());
    }

    std::printf("== which controls each radio is offered (AGENTS.md: never one the radio cannot use)\n");
    {
        auto sup = [](unsigned d, bool lite = false, bool tv = false, bool tunnel = true) {
            Platform pf; pf.driver = d; pf.lite = lite; pf.tv = tv; pf.tunnel = tunnel;
            auto v = supportedControls(pf); return std::set<std::string>(v.begin(), v.end());
        };
        auto rtl = sup(D_RTL), rsp = sup(D_RSP), ahf = sup(D_AHF), asp = sup(D_ASP), hrf = sup(D_HRF), icom = sup(D_ICOM);
        ok(rtl.count("ppm") && rtl.count("adsb256") && rtl.count("rtlAgc") && rtl.count("androidTunerBw") && rtl.count("autoDs") && rtl.count("gainIfBw"), "RTL: ppm, ADS-B at 2.56, VibeAGC, IF filter, direct sampling");
        ok(!rtl.count("ppb") && !rtl.count("rfNotch") && !rtl.count("icomBaud") && !rtl.count("antennaMap"), "RTL: no HF+ calibration, no SDRplay card, no Icom, no RSP sockets");
        ok(rsp.count("rfNotch") && rsp.count("rspVibeAgc") && rsp.count("ppm") && rsp.count("antennaMap") && rsp.count("biasT"), "RSP: its card, ppm, sockets by band, bias-T");
        ok(!rsp.count("adsb256") && !rsp.count("rtlAgc") && !rsp.count("autoDs") && !rsp.count("gainIfBw"), "RSP: none of the RTL's");
        ok(ahf.count("ppb") && !ahf.count("ppm") && !ahf.count("biasT") && !ahf.count("convLo") && !ahf.count("rtlAgc"), "HF+: ppb only — no ppm, bias-T, converter or gain controls of a dongle");
        ok(asp.count("biasT") && asp.count("convLo") && !asp.count("ppm") && !asp.count("ppb"), "Airspy R2/Mini: bias-T and converter, no ppm/ppb");
        ok(hrf.count("convLo") && !hrf.count("adsb256") && !hrf.count("rfNotch"), "HackRF: converter, none of the RTL's or RSP's");
        ok(icom.count("icomBaud") && icom.count("icomOffDelayMin") && !icom.count("rate") && !icom.count("nbWide") && !icom.count("vfoB")
           && !icom.count("bgOn") && !icom.count("fmDeemph") && !icom.count("modeLocked") && !icom.count("biasT") && !icom.count("gainMax"),
           "Icom: its own settings; no capture width, blanker, VFO B, background decoding, FM defaults, locked range, bias-T or gain rules");
        ok(!sup(D_RTL, true).count("rawIqOn") && sup(D_RTL, true).count("dabScanLabels") && !rtl.count("dabScanLabels"), "Lite: no raw IQ out; the DAB label scan only on Lite");
        ok(!sup(D_RTL, false, true).count("batteryPauseAt") && rtl.count("batteryPauseAt"), "a television: no battery pause");
        ok(!sup(D_RTL, false, false, false).count("dirList") && rtl.count("dirList"), "no tunnel binary: no public listing");
        Platform pf; std::vector<std::string> adv; supportedControls(pf, &adv);
        std::set<std::string> a(adv.begin(), adv.end());
        ok(a.count("users") && a.count("allowPick") && a.count("gainMax") && a.count("trustedProxies") && a.count("modeLocked") && !a.count("name") && !a.count("rate"),
           "the Advanced-only list: sharing, tuning limits, gain rules, proxies, the locked range — not the name or the capture");
        J pgc; parse(pageConfig(rawFor(fullSettings(""), "rtl", "")), pgc);
        ok(pgc.get("android")->get("advancedOnly") && pgc.get("android")->get("advancedOnly")->a.size() == adv.size(), "the page is told which are Advanced-only");
    }

    std::printf("== a save of a control the radio does not have is ignored\n");
    {
        std::string patch, err; bool restart = false;
        const std::string rtlRaw = rawFor(fullSettings(""), "rtl", "");
        ok(settingsPatch(R"({"radios":[{"rfNotch":true,"ppb":500,"icomBaud":9600,"rspVibeAgc":1,"ppm":7}]})", rtlRaw, patch, restart, err), "accepted");
        J pj = parsed(patch);
        ok(!pj.get("rfNotch") && !pj.get("ppb") && !pj.get("icomBaud") && !pj.get("rspVibeAgc") && pj.get("ppm") && pj.get("ppm")->n == 7,
           "RTL: the SDRplay card, the HF+ ppb and the Icom's baud are dropped; ppm kept", patch);
        const std::string ahfRaw = rawFor(fullSettings(""), "airspyhf", "");
        ok(settingsPatch(R"({"radios":[{"ppm":7,"biasT":true,"adsb256":true,"ppb":-300,"converterOffsetHz":5}]})", ahfRaw, patch, restart, err), "accepted");
        pj = parsed(patch);
        ok(!pj.get("ppm") && !pj.get("biasT") && !pj.get("adsb256") && !pj.get("converterOffsetHz") && pj.get("ppb")->n == -300, "HF+: only the ppb", patch);
        const std::string icomRaw = rawFor(fullSettings(""), "icom", "");
        ok(settingsPatch(R"({"radios":[{"rate":2048000,"rateLock":true,"nbWide":0,"mode":"locked","icomBaud":20000}]})", icomRaw, patch, restart, err), "accepted");
        pj = parsed(patch);
        ok(!pj.get("lockedRate") && !pj.get("nbWide") && !pj.get("radioUse") && pj.get("icomBaud")->n == 19200, "Icom: no capture, blanker or locked range; the baud snaps to one the radio offers", patch);
        ok(settingsPatch(R"({"dirList":true,"dirName":"x"})", rawFor(fullSettings(""), "rtl", "\"tunnelSupported\":false"), patch, restart, err)
           && !parsed(patch).get("publicListed") && !parsed(patch).get("publicName"), "no tunnel binary: the listing is not touched");
        ok(settingsPatch(R"({"batteryPauseAt":30})", rawFor(fullSettings(""), "rtl", "\"tv\":true"), patch, restart, err)
           && !parsed(patch).get("batteryPauseAt"), "a television: the battery pause is not touched");
    }

    std::printf("== the phone's rules for the new settings\n");
    {
        std::string patch, err; bool restart = false;
        const std::string rtlRaw = rawFor(fullSettings(""), "rtl", "\"tunnelSupported\":true");
        ok(settingsPatch(R"({"radios":[{"rate":2050000,"rateLock":true,"maxFps":13}]})", rtlRaw, patch, restart, err), "accepted");
        J pj = parsed(patch);
        ok(pj.get("lockedRate")->n == 2048000 && pj.get("sampleRate")->n == 2048000 && pj.get("rateLock")->b && pj.get("maxFftRate")->n == 10,
           "a pinned rate snaps to the phone's list; the waterfall to 5/10/20", patch);
        ok(settingsPatch(R"({"radios":[{"rate":1024000,"rateLock":false}]})", rtlRaw, patch, restart, err), "accepted");
        pj = parsed(patch);
        ok(pj.get("lockedRate")->n == 0 && pj.get("sampleRate")->n == 2400000 && !pj.get("rateLock")->b, "unpinned = the listener's choice (rate 0), started full width");
        ok(settingsPatch(R"({"radios":[{"rate":2400000,"rateLock":true}]})", rawFor(fullSettings(""), "airspyhf", ""), patch, restart, err)
           && parsed(patch).get("lockedRate")->n == 912000, "an HF+ is never given a dongle's rate (snaps to 912 k)");
        ok(settingsPatch(R"({"webServer":false,"advanced":true})", rtlRaw, patch, restart, err) && !parsed(patch).get("advanced")->b,
           "no web client → not Advanced (the admin page IS the web client)");
        ok(settingsPatch(R"({"radios":[{"blockedModes":"DAB,drm","landingDabChannel":5,"landingDabSid":7,"landingDabService":"X"}]})", rtlRaw, patch, restart, err), "accepted");
        pj = parsed(patch);
        ok(pj.get("landingDabChannel")->n == -1 && pj.get("landingDabSid")->n == 0 && pj.get("landingDabService")->s.empty(), "never a DAB landing beside a DAB block");
        ok(settingsPatch(R"({"radios":[{"landingAdsb":false,"adsbKeepRunning":true}]})", rtlRaw, patch, restart, err)
           && !parsed(patch).get("adsbKeepRunning")->b, "ADS-B keep-running never without the ADS-B landing");
        // ★★★ beta96 a beta95 phone config (the ADS-B pair, no map keys) reads on the page as the map + ADS-B, kept
        {
            J pc; ok(parse(pageConfig(rawFor(R"({"landingAdsb":true,"adsbKeepRunning":true})", "rtl", "")), pc), "a beta95 config reads");
            const J* r0 = pc.get("radios") && !pc.get("radios")->a.empty() ? &pc.get("radios")->a[0] : nullptr;
            ok(r0 && r0->get("landingMap") && r0->get("landingMap")->s == "avmar" && r0->get("landingModes")->s == "adsb" && r0->get("mapKeep")->b,
               "★ beta95's ADS-B landing + keep shows as the map, ADS-B, kept");
        }
        // ★★★ beta96 the landing map: the page's words win, the ADS-B pair mirrored from them
        ok(settingsPatch(R"({"radios":[{"landingMap":"avmar","landingModes":"vdl2","mapKeep":true,"landingAdsb":true,"adsbKeepRunning":true}]})", rtlRaw, patch, restart, err), "a VDL2 keep accepted", err);
        pj = parsed(patch);
        ok(pj.get("landingMap")->s == "avmar" && pj.get("landingModes")->s == "vdl2" && pj.get("mapKeep")->b
           && !pj.get("landingAdsb")->b && !pj.get("adsbKeepRunning")->b, "VDL2 kept stored; the ADS-B pair OFF (an older engine never lands in ADS-B)", patch);
        ok(settingsPatch(R"({"radios":[{"landingMap":"avmar","landingModes":"adsb","mapKeep":true}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("landingAdsb")->b && parsed(patch).get("adsbKeepRunning")->b, "ADS-B on the map mirrors the pair (the boot's setVibeServerLandingAdsb)");
        ok(settingsPatch(R"({"radios":[{"landingMap":"avmar","landingModes":"ais,adsb","mapKeep":true}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("landingModes")->s == "adsb", "two modes no single mode decodes: the first (table order)");
        ok(settingsPatch(R"({"radios":[{"landingMap":"","landingModes":"vdl2","mapKeep":true}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("landingModes")->s.empty() && !parsed(patch).get("mapKeep")->b, "no map: nothing kept");
        ok(settingsPatch(R"({"place":"Northampton","lat":"52.2405","lon":"-0.9027","country":"gb","locFollow":false})", rtlRaw, patch, restart, err), "a place accepted");
        pj = parsed(patch);
        const J* loc = pj.get("serverLocation");
        ok(loc && loc->get("mode")->s == "manual" && loc->get("label")->s == "Northampton" && loc->get("country")->s == "GB" && std::fabs(loc->get("lat")->n - 52.2405) < 1e-9,
           "→ manual, the place, the country in capitals", patch);
        ok(!settingsPatch(R"({"lat":"95","lon":"0"})", rtlRaw, patch, restart, err), "a latitude past 90 is refused", err);
        ok(settingsPatch(R"({"place":"","lat":"","lon":"","locFollow":true})", rtlRaw, patch, restart, err)
           && parsed(patch).get("serverLocation")->get("mode")->s == "off" && parsed(patch).get("serverLocation")->get("moving")->b, "blank = no home published; moving kept");
        ok(gridOf(52.2405, -0.9027) == "IO92nf", "the grid square of Northampton", gridOf(52.2405, -0.9027));
        const std::string noPpm = rawFor(R"({"name":"x"})", "rtl", "");
        ok(settingsPatch(R"({"radios":[{"ppm":0}]})", noPpm, patch, restart, err) && !parsed(patch).get("ppm"), "ppm 0 on a radio left alone stays absent (leave the radio alone)");
        ok(settingsPatch(R"({"radios":[{"ppm":0}]})", rawFor(R"({"ppm":4})", "rtl", ""), patch, restart, err) && parsed(patch).get("ppm") && parsed(patch).get("ppm")->n == 0, "…but 0 over a set ppm is a real 0");
        ok(settingsPatch(R"({"radios":[{"converterInputLoHz":0}]})", rtlRaw, patch, restart, err) && !parsed(patch).get("converterInputLoHz"),
           "the Linux page's always-0 converter low end does not wipe the phone's");
        ok(settingsPatch(R"({"radios":[{"androidPage":true,"converterInputLoHz":2000000,"directSampling":9}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("converterInputLoHz")->n == 2000000 && parsed(patch).get("directSampling")->n == 2, "…the phone's own page does; a fixed branch clamps to 0–2");
        ok(!settingsPatch(R"({"radios":[{"antennaSwitch":"not json"}]})", rtlRaw, patch, restart, err), "an antenna switch that is not JSON is refused", err);
        ok(!settingsPatch(R"({"radios":[{"bgDecode":"[1,2]"}]})", rtlRaw, patch, restart, err), "background decoding that is not a JSON object is refused", err);
        ok(settingsPatch(R"({"radios":[{"bgDecode":""}]})", rtlRaw, patch, restart, err) && parsed(patch).get("bgDecode")->s.empty(), "…and \"\" switches it off");
        ok(settingsPatch(R"({"radios":[{"rawIqLanMaxHz":300000}]})", rtlRaw, patch, restart, err) && parsed(patch).get("rawIqLanMaxHz")->t == J::Str
           && parsed(patch).get("rawIqLanMaxHz")->s == "300000", "the raw-IQ LAN ceiling travels as TEXT, the way VibeServerBoot reads it");
        ok(settingsPatch(R"({"radios":[{"fmDeemph":60,"icomOffDelayMin":7}]})", rawFor(fullSettings(""), "icom", ""), patch, restart, err)
           && parsed(patch).get("icomOffDelayMin")->n == 5 && !parsed(patch).get("fmDeemph"), "Icom: the delay snaps to the radio's choices; no FM defaults on an Icom");
        ok(settingsPatch(R"({"radios":[{"fmDeemph":60}]})", rtlRaw, patch, restart, err) && parsed(patch).get("fmDeemph")->n == -1, "an unknown de-emphasis = by region (-1)");
        ok(settingsPatch(R"({"dirList":true,"dirName":"Moulton","dirShareSec":3600})", rtlRaw, patch, restart, err)
           && parsed(patch).get("publicListed")->b && parsed(patch).get("publicName")->s == "Moulton" && parsed(patch).get("publicShareSec")->n == 3600,
           "the listing: an action (publicListed) plus the name and the length");
        ok(settingsPatch(R"({"radios":[{"mode":"locked","lockFreq":0}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("radioUse")->s == "locked" && !parsed(patch).get("lockedCentre"), "a locked range without a centre keeps the phone's centre");
        ok(settingsPatch(R"({"radios":[{"agcLock":1,"restGain":-7}]})", rtlRaw, patch, restart, err)
           && parsed(patch).get("agcLock")->t == J::Bool && parsed(patch).get("restGain")->n == -1, "the AGC lock is the phone's yes/no; the resting gain clamps to -1 (leave alone)");
    }

    std::printf("== every control in the table has a home in the page config\n");
    {
        // ids → the page config key (r: = on the radio) its controls show; the containers name what they edit.
        const std::map<std::string, std::string> home = {
            {"name","name"},{"mdns","mdnsAdvertise"},{"landingMessage","landingMessage"},{"landingLinkUrl","landingLinkUrl"},
            {"landingLinkLabel","landingLinkLabel"},{"uncompressed","uncompressed"},{"trustedProxies","trustedProxies"},
            {"maxRadiosPerIp","maxRadiosPerIp"},{"decoderMax","decoderMax"},{"rttySkimWorkers","rttySkimWorkers"},{"galleryMb","galleryMb"},
            {"galleryPublic","galleryPublic"},{"gallerySaveLive","gallerySaveLive"},{"communityMap","communityMap"},{"communityModes","communityOff"},{"batteryPauseAt","batteryPauseAt"},
            // ★ V12 beta90 PSK Reporter / WSPRnet / the DMR ID lookup
            {"pskrOn","pskrOn"},{"pskrCall","pskrCall"},{"pskrAntenna","pskrAntenna"},{"wsprnetOn","wsprnetOn"},{"wsprnetCall","wsprnetCall"},{"dmrIdLookup","dmrIdLookup"},
            {"place","place"},{"lat","lat"},{"lon","lon"},{"locFollow","locFollow"},{"country","country"},{"locator","locator"},
            {"dirList","dirList"},{"dirName","dirName"},{"dirShareSec","dirShareSec"},
            {"androidWebServer","a:webServer"},{"androidCompress","a:compressAudio"},{"androidAdvanced","a:advanced"},
            {"radioName","r:label"},{"antenna","r:antenna"},{"radioPin","r:pin"},{"radioPinClear","r:pin"},{"users","r:users"},
            {"askFirst","r:askBeforeTuning"},{"modeSingle","r:mode"},{"modeLocked","r:mode"},{"lockFreq","r:lockFreq"},
            {"zoomSpectrum","r:zoomSpectrum"},{"landingFreq","r:landingFreq"},{"demodMode","r:demodMode"},
            {"sessionLimit","r:sessionLimitMin"},{"sessionLimitMode","r:sessionLimitSoft"},{"idleKick","r:idleKickMin"},
            {"nbWide","r:nbWide"},{"vfoB","r:vfoB"},{"idleGrace","r:idleGrace"},{"spectrogram","r:spectrogram"},
            {"rate","r:rate"},{"rateLock","r:rateLock"},{"maxFps","r:maxFps"},{"modeBlockList","r:blockedModes"},{"bandModeList","r:blockedModes"},
            {"dabRateBoost","r:dabRateBoost"},{"adsb256","r:adsb256"},{"ism256","r:ism256"},{"dabScanLabels","r:dabScanLabels"},
            {"gainRestSlider","r:restGain"},{"gainRest","r:restGain"},{"gainRestLock","r:restGain"},{"rtlAgc","r:rtlAgc"},
            {"androidTunerBw","r:tunerBwAuto"},{"gainAgcLock","r:agcLock"},{"gainPick","r:gainLimits"},{"gainCurve","r:gainCurves"},
            {"gainMaxSlider","r:gainLimits"},{"gainMax","r:gainLimits"},{"gainIfSlider","r:gainLimits"},{"gainIfMax","r:gainLimits"},
            {"gainSplitSlider","r:gainSplits"},{"gainAspLock","r:gainLocks"},{"gainLock","r:gainLocks"},{"bandCopyFrom","r:gainLimits"},
            {"gainList","r:gainLimits"},{"gainAdd","r:gainLimits"},{"gainIfBw","r:ifBwLimits"},
            {"allowPick","r:allowRanges"},{"allowLo","r:allowRanges"},{"allowHi","r:allowRanges"},
            {"blockPick","r:blockRanges"},{"blockLo","r:blockRanges"},{"blockHi","r:blockRanges"},
            {"dabLandOn","r:landingDabChannel"},{"dabLandBlock","r:landingDabChannel"},{"dabLandStation","r:landingDabSid"},
            {"adsbLandOn","r:landingAdsb"},{"adsbKeep","r:adsbKeepRunning"},{"mapLandOn","r:landingMap"},{"mapLandWhich","r:landingMap"},{"mapModeList","r:landingModes"},{"mapKeep","r:mapKeep"},
            {"fmDeemph","r:fmDeemph"},{"fmStereo","r:fmStereo"},{"fmNr","r:fmNr"},{"fmIms","r:fmIms"},{"fmCeq","r:fmCeq"},
            {"fmAutoBw","r:fmAutoBw"},{"fmNb","r:fmNb"},{"fmReturn","r:fmRemember"},{"fmRemember","r:fmRemember"},
            {"antRangePreset","r:antennaRanges"},{"antFilterPreset","r:antennaFilters"},{"antPortNames","r:antennaNames"},
            {"swYes","r:antennaSwitch"},{"swNo","r:antennaSwitch"},{"swBroker","r:antennaSwitch"},{"swUser","r:antennaSwitch"},
            {"swPass","r:antennaSwitch"},{"swLocked","r:antennaPortLocked"},{"swAntennas","r:antennaMap"},
            {"antennaMap","r:antennaMap"},{"antennaPortLocked","r:antennaPortLocked"},{"ppm","r:ppm"},{"ppb","r:ppb"},{"biasT","r:biasT"},
            {"autoDs","r:autoDirectSampling"},{"autoDsBelow","r:directSamplingBelowHz"},{"androidDsFixed","r:directSampling"},
            {"convLo","r:converterOffsetHz"},{"convDown","r:converterOffsetHz"},{"convHi","r:converterInputHiHz"},{"androidConvInLo","r:converterInputLoHz"},
            {"icomBaud","r:icomBaud"},{"icomCiv","r:icomCiv"},{"icomScopeSpeed","r:icomScopeSpeed"},{"icomTuneLock","r:icomTuneLock"},
            {"icomPowerOn","r:icomPowerOn"},{"icomAutoOff","r:icomAutoOff"},{"icomOffDelayMin","r:icomOffDelayMin"},
            {"bgOn","r:bgDecode"},{"bgWait","r:bgDecode"},{"bgKeepInRange","r:bgDecode"},{"bgProfiles","r:bgDecode"},
            {"dxOn","r:dxScan"},{"dxWait","r:dxScan"},{"dxBands","r:dxScan"},{"dxRaster","r:dxScan"},{"dxOirt","r:dxScan"},{"dxExt","r:dxScan"},{"dxViewAdmin","r:dxScan"},
            {"rawIqOn","r:rawIq"},{"rawIqLan","r:rawIqLanMaxHz"},{"rawIqStreams","r:rawIqMax"},{"rawIqLocalOnly","r:rawIq"},
            {"rspVibeAgc","r:rspVibeAgc"},{"rfNotch","r:rfNotch"},{"dabNotch","r:dabNotch"},{"rspAmNotch","r:rspAmNotch"},{"rspHdr","r:rspHdr"},
            {"rspExtRef","r:rspExtRef"},{"rspDabDecim","r:rspDabDecim"},{"rfAgc","r:rfAgc"},{"autoNotch","r:autoNotch"},{"userNotch","r:userNotch"},
            {"dabAgcOverride","r:dabAgcOverride"},{"dabAgcTarget","r:dabAgcTarget"},{"agcSet","r:agcSet"},{"agcSetLock","r:agcSetLock"},
        };
        J pgc; parse(pageConfig(rawFor(fullSettings(""), "rtl", "\"tunnelSupported\":true")), pgc);
        const J& rr = pgc.get("radios")->a.at(0);
        std::string missing, noHome;
        for (const auto& id : allControlIds()) {
            auto it = home.find(id);
            if (it == home.end()) { noHome += id + " "; continue; }
            const std::string& k = it->second;
            const J* v = k.rfind("r:", 0) == 0 ? rr.get(k.substr(2))
                       : k.rfind("a:", 0) == 0 ? pgc.get("android")->get("server")->get(k.substr(2)) : pgc.get(k);
            if (!v) missing += k + " ";
        }
        ok(noHome.empty(), "every control in the table is listed here", noHome);
        ok(missing.empty(), "every one has its value in the page config", missing);
        ok(allControlIds().size() > 150, "the table is the whole screen, not the first 28", std::to_string(allControlIds().size()));
    }
    std::printf(fails ? "\nFAILED: %d\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
