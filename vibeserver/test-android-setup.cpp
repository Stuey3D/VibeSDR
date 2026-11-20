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
    if (argc == 3 && std::string(argv[1]) == "--patch") {
        std::string patch, err; bool restart = false;
        if (!settingsPatch(slurp(argv[2]), patch, restart, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
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
    ok(sup && sup->a.size() == supportedControls().size(), "the supported control list");
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
        ok(settingsPatch(dump(body), patch, restart, err), "the save is accepted", err);
        ok(restart, "restart:true is passed on");
        J pj; parse(patch, pj);
        J st; parse(settings, st);
        int diff = 0; std::string which;
        for (auto& kv : pj.o) {
            const J* was = st.get(kv.first);
            bool eq = was && dump(*was) == dump(kv.second);
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

    std::printf("== every supported control has both directions\n");
    {
        // The page ids → the config keys the page's collect()/collectRadio() send them as.
        const char* pairs[][2] = {
            {"name","name"},{"mdns","mdnsAdvertise"},{"landingMessage","landingMessage"},{"landingLinkUrl","landingLinkUrl"},
            {"landingLinkLabel","landingLinkLabel"},{"uncompressed","uncompressed"},{"trustedProxies","trustedProxies"},
            {"maxRadiosPerIp","maxRadiosPerIp"},{"decoderMax","decoderMax"},{"galleryMb","galleryMb"},{"galleryPublic","galleryPublic"},
            {"gallerySaveLive","gallerySaveLive"},{"batteryPauseAt","batteryPauseAt"},
            {"radioName","r:label"},{"antenna","r:antenna"},{"users","r:users"},{"askFirst","r:askBeforeTuning"},
            {"zoomSpectrum","r:zoomSpectrum"},{"landingFreq","r:landingFreq"},{"demodMode","r:demodMode"},
            {"sessionLimit","r:sessionLimitMin"},{"sessionLimitMode","r:sessionLimitSoft"},{"idleKick","r:idleKickMin"},
            {"nbWide","r:nbWide"},{"vfoB","r:vfoB"},{"idleGrace","r:idleGrace"},{"radioPin","r:pin"},{"radioPinClear","r:pin"},
        };
        std::set<std::string> listed(supportedControls().begin(), supportedControls().end());
        size_t n = 0;
        for (auto& p : pairs) {
            ++n;
            if (!listed.count(p[0])) { ok(false, "pair names an unsupported control", p[0]); continue; }
            std::string key = p[1];
            const bool radio = key.rfind("r:", 0) == 0;
            if (radio) key = key.substr(2);
            const J* v = radio ? r0.get(key) : pg.get(key);
            ok(v != nullptr, (std::string("the page config carries ") + p[1]).c_str());
        }
        ok(n == supportedControls().size(), "every supported control is in this table");
    }

    std::printf(fails ? "\nFAILED: %d\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
