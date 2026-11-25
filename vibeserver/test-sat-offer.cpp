// test-sat-offer — ★★ beta94 "sat" IN THE PUBLISHED `modes` (vibe_sat_offer.h): the directory's satellite badge
// (Stuart 11-25: "the directory page should have a little satellite icon for servers that have satellite mode enabled").
//
// §1 the rule, every combination of reach / location / the owner's switches: offered only with a downlink in reach, a
//    location, "sat" not blocked and at least one of NFM / USB / LSB left on.
// §2 the shim (source contract): vsSatOffered asks exactly vsSatAnyReach, the engine's observer and vsModeBlocked; "sat"
//    is appended to `modes` AFTER the kOffer loop (it is not a Setup switch, so rc.has() would drop it) and inside the
//    same builder that publishes `"modes":`; never added to kOffer itself.
// ★ Silent; no radio, no network.
#include "vibe_sat_offer.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main() {
    std::printf("§1 the rule\n");
    const char* kIds[] = { "sat", "nfm", "usb", "lsb" };
    int offered = 0;
    for (int reach = 0; reach < 2; ++reach)
    for (int located = 0; located < 2; ++located)
    for (int mask = 0; mask < 16; ++mask) {           // bit i = kIds[i] blocked
        std::set<std::string> blocked;
        for (int i = 0; i < 4; ++i) if (mask & (1 << i)) blocked.insert(kIds[i]);
        auto isBlocked = [&](const char* m) { return blocked.count(m) > 0; };
        const bool got = vibe::sat::offeredInModes(reach != 0, located != 0, isBlocked);
        const bool demod = !(blocked.count("nfm") && blocked.count("usb") && blocked.count("lsb"));
        const bool want = reach && located && !blocked.count("sat") && demod;
        CHECK(got == want, "reach=%d located=%d blocked mask=%d: got %d want %d", reach, located, mask, got, want);
        offered += got;
    }
    // reach + located + "sat" free: 8 masks of nfm/usb/lsb, all but "all three off" = 7
    CHECK(offered == 7, "offered in %d of 64 combinations (want 7)", offered);
    // the plain cases, said out loud
    auto none = [](const char*) { return false; };
    CHECK(vibe::sat::offeredInModes(true, true, none), "a radio in reach, located, nothing blocked: offered");
    CHECK(!vibe::sat::offeredInModes(false, true, none), "no downlink in reach (HF-only, an R36S): not offered");
    CHECK(!vibe::sat::offeredInModes(true, false, none), "no location set: not offered");
    auto onlyNfm = [](const char* m) { const std::string k = m; return k != "nfm" && k != "sat"; };   // every demodulator off but NFM
    CHECK(vibe::sat::offeredInModes(true, true, onlyNfm), "only NFM left on: offered (the FM birds)");
    auto allOff = [](const char*) { return true; };
    CHECK(!vibe::sat::offeredInModes(true, true, allOff), "every demodulator off (an only-mode receiver): not offered");

    std::printf("§2 the shim\n");
    const std::string src = readFile("android/app/src/main/cpp/local_sdr_shim.cpp");
    CHECK(!src.empty(), "read local_sdr_shim.cpp (run from the repository root)");
    if (!src.empty()) {
        CHECK(src.find("#include \"vibe_sat_offer.h\"") != std::string::npos, "the shim includes vibe_sat_offer.h");
        const size_t f = src.find("static bool vsSatOffered() {");
        CHECK(f != std::string::npos, "vsSatOffered is defined");
        if (f != std::string::npos) {
            const std::string body = src.substr(f, src.find("\n}\n", f) - f);
            CHECK(body.find("offeredInModes(vsSatAnyReach(), satEngine().haveObserver()") != std::string::npos,
                  "vsSatOffered asks vsSatAnyReach and the engine's observer");
            CHECK(body.find("vsModeBlocked(m)") != std::string::npos, "…and the owner's blocked list");
        }
        const size_t k = src.find("static const char* kOffer[] = {");
        CHECK(k != std::string::npos, "found the kOffer list");
        if (k != std::string::npos) {
            const size_t kEnd = src.find("};", k);
            CHECK(src.substr(k, kEnd - k).find("\"sat\"") == std::string::npos, "\"sat\" is not in kOffer (rc.has would drop it)");
            const size_t app = src.find("if (vsSatOffered()) { m += first ? \"\\\"sat\\\"\" : \",\\\"sat\\\"\"; first = false; }", kEnd);
            const size_t close = src.find("m += \"]\";", kEnd);
            const size_t pub = src.find("j += \",\\\"modes\\\":\" + m;", kEnd);
            CHECK(app != std::string::npos, "\"sat\" is appended to the modes list");
            CHECK(app != std::string::npos && close != std::string::npos && app < close, "…before the list is closed");
            CHECK(pub != std::string::npos && close < pub && pub - k < 12000, "…in the builder that publishes \"modes\"");
        }
    }

    std::printf("%s — %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
