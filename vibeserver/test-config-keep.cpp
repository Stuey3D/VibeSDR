// ★★★ GOING BACK FROM A BETA MUST NOT COST THE OWNER THEIR SETTINGS (11.0.10-4, "Receive beta updates", 2026-10-22).
//
// A V12 beta writes keys 11.x has never heard of — at the top level and inside each radio (an Icom's port, baud and
// CI-V address, for a start). Until now 11.x read past them and its next save DROPPED them, so an owner who unticked
// beta, went back to 11 and changed one setting would find V12's settings gone when they ticked it again.
// Checks: 11.x STARTS on a V12 file (every known setting read as written); the unknown keys at both levels survive a
// load + save — strings, numbers, booleans, null, nested objects and arrays, exactly as written; a settings POST
// that knows nothing of them (a patch, a page posting a radio) keeps them; a key 11.x owns is never duplicated.

#include "vibeserver_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static int failures = 0, passes = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (cond) passes++; else failures++;
}
static int count(const std::string& s, const std::string& sub) {
    int n = 0; for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) n++; return n;
}

// ★ Shaped like a real V12 config.json: this version's own keys first (as its writer puts them), then the new ones.
static const char* kV12 = R"({
  "configured": true,
  "fullMode": true,
  "sharing": "public",
  "name": "Loft",
  "adminPass": "s3cret-pass",
  "pin": "",
  "port": 48000,
  "v12Theme": "nixie",
  "v12Etiquette": {"askFirst": true, "timeoutSec": 15, "name": "not the server name"},
  "v12List": [1, 2, {"x": "y"}],
  "v12Null": null,
  "radios": [
    {"serial":"00000001","driver":"rtlsdr","label":"VHF","gain":296,"enabled":true,"configured":true,
     "icomPort":"","icomBaud":0,"binauralWidth":0.75,"v12Nested":{"gain":999,"label":"nested, not mine"}},
    {"serial":"IC7300-1234","driver":"icom","label":"IC-7300","enabled":true,"configured":true,
     "icomPort":"/dev/ttyUSB0","icomBaud":115200,"icomCiv":148,"icomAudio":"hw:CARD=CODEC","icomScopeSpeed":2,
     "icomOwnerLock":true,"icomRfOpen":false,"icomTtyOk":true}
  ]
}
)";

int main() {
    std::printf("config — keys this version does not know are KEPT (going back from a V12 beta)\n");
    char tmpl[] = "/tmp/vs-cfgkeep-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/config.json";
    { std::ofstream f(path); f << kV12; }

    std::string err;
    vsconfig::ServerConfig c;
    ok(vsconfig::loadServer(path, c, err) && err.empty(), "11.x loads a V12 config.json");
    ok(c.name == "Loft" && c.adminPass == "s3cret-pass" && c.fullMode && c.port == 48000,
       "and reads every setting it knows as written (name, admin password, mode, port)");
    ok(c.radios.size() == 2, "both radios are there (the Icom too — 11.x treats it as a radio it cannot find)");
    ok(c.radios.size() == 2 && c.radios[0].gain == 296 && c.radios[0].label == "VHF",
       "a nested object's \"gain\" / \"label\" does not shadow the radio's own");

    ok(vsconfig::saveServer(path, c, err), "and saves it");
    std::stringstream ss; { std::ifstream f(path); ss << f.rdbuf(); }
    const std::string out = ss.str();
    ok(out.find("\"v12Theme\": \"nixie\"") != std::string::npos, "top level: a string kept");
    ok(out.find("\"v12Etiquette\": {\"askFirst\": true, \"timeoutSec\": 15, \"name\": \"not the server name\"}") != std::string::npos,
       "top level: a nested object kept exactly as written");
    ok(out.find("\"v12List\": [1, 2, {\"x\": \"y\"}]") != std::string::npos, "top level: an array kept");
    ok(out.find("\"v12Null\": null") != std::string::npos, "top level: null kept");
    ok(out.find("\"icomPort\":\"/dev/ttyUSB0\"") != std::string::npos && out.find("\"icomBaud\":115200") != std::string::npos
       && out.find("\"icomCiv\":148") != std::string::npos && out.find("\"icomAudio\":\"hw:CARD=CODEC\"") != std::string::npos
       && out.find("\"icomOwnerLock\":true") != std::string::npos && out.find("\"icomTtyOk\":true") != std::string::npos,
       "per radio: every Icom setting V12 wrote is kept");
    ok(out.find("\"binauralWidth\":0.75") != std::string::npos, "per radio: a number kept");
    ok(out.find("\"v12Nested\":{\"gain\":999,\"label\":\"nested, not mine\"}") != std::string::npos, "per radio: a nested object kept");
    ok(count(out, "\"name\"") == 2, "no key 11.x owns is written twice (name: ours + the nested one)");
    ok(count(out, "\"gain\":") == 3, "gain: each radio's own once, plus the nested one — never duplicated");
    ok(count(out, "\"radios\"") == 1 && count(out, "\"serial\":\"00000001\"") == 1, "one radios array, each radio once");

    // ★ And again: a second round trip changes nothing (no growth, no drift).
    vsconfig::ServerConfig c2;
    ok(vsconfig::loadServer(path, c2, err), "the saved file loads");
    const std::string again = vsconfig::toJson(c2);
    ok(again == out, "a second load + save is byte-identical");

    // ★★ A settings change through the paths that know NOTHING of V12: a live patch, and a page posting a radio.
    vsconfig::ServerConfig p = c2;
    ok(vsconfig::fromJson("{\"name\":\"Attic\"}", p, err), "a patch {\"name\":\"Attic\"} applies");
    ok(p.name == "Attic" && vsconfig::toJson(p).find("\"v12Theme\": \"nixie\"") != std::string::npos,
       "and the kept top-level keys are still there");
    ok(vsconfig::fromJson("{\"radios\":[{\"serial\":\"IC7300-1234\",\"driver\":\"icom\",\"label\":\"Shack\"}]}", p, err),
       "a page posts the Icom radio with only the keys it knows");
    const std::string after = vsconfig::toJson(p);
    ok(after.find("\"label\":\"Shack\"") != std::string::npos, "the post's change lands");
    ok(after.find("\"icomPort\":\"/dev/ttyUSB0\"") != std::string::npos && after.find("\"icomCiv\":148") != std::string::npos,
       "and that radio's V12 keys survive it");
    ok(vsconfig::fromJson("{\"radios\":[{\"serial\":\"IC7300-1234\",\"icomCiv\":150}]}", p, err)
       && vsconfig::toJson(p).find("\"icomCiv\":150") != std::string::npos
       && count(vsconfig::toJson(p), "\"icomCiv\"") == 1,
       "a post that DOES carry a kept key updates it in place, once");

    // ★ An old single-radio file (no radios array) is migrated, as ever — its leftovers are not "newer settings".
    vsconfig::ServerConfig m;
    ok(vsconfig::fromJson("{\"name\":\"Old\",\"gain\":100,\"users\":3}", m, err) && m.unknownKeys.empty(),
       "a pre-3.0 single-radio file is migrated, nothing kept from it");

    // ★ A malformed document keeps nothing rather than something half-read.
    vsconfig::ServerConfig bad;
    vsconfig::fromJson("{\"radios\":[], \"broken\": {\"a\": 1, ", bad, err);
    ok(bad.unknownKeys.empty(), "a malformed document keeps nothing");

    unlink(path.c_str()); unlink((path + ".bak").c_str()); rmdir(dir.c_str());
    std::printf(failures ? "\n%d FAILED, %d passed\n" : "\nall %d passed\n", failures ? failures : passes, passes);
    return failures ? 1 : 0;
}
