// test-new-modes — ★★★ beta96 A MODE AN UPDATE ADDS STAYS OFF ON A RESTRICTED RADIO (vibe_decoder_reach.h newModesState,
// docs/v12/NEW-MODES-OFF.md). Stuart 11-26: Paul's radio was set up ADS-B only; later betas added modes his blocked list
// could not name, they came in ON, and listeners could leave ADS-B again.
//
// §1 migration, a list written before beta96 (no "seen@"): unrestricted, restricted, single-mode — and Paul's exact shape
// §2 the exit lock (onlyModeLeft on the ENFORCED list) holds after new modes appear, whatever the owner's new-modes answer
// §3 the owner's words: "seen@N", "newmodes@on|off", "+id" — and none of them ever published (hwinfo `blocked`)
// §4 the table: every switch the Setup page and the app draw has a generation; the page's copy equals this one
// §5 source contracts: onlyModeNow asks onlyModeLeft; hwinfo `blocked` / `modes` read the enforced list
// ★ Silent; no radio, no network. Run from the repository root (reads the page, the app screen and the shim).
#include "vibe_decoder_reach.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace vibereach;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::set<std::string> toSet(const std::string& csv) { std::set<std::string> s; for (auto& t : tokens(csv)) s.insert(t); return s; }
static bool in(const std::string& csv, const char* id) { return toSet(csv).count(id) > 0; }
static std::string only(const std::string& effective) {
    const std::set<std::string> s = toSet(effective);
    return onlyModeLeft([&](const char* m) { return s.count(m) > 0; });
}
/** Every plain switch up to generation `g`, less `except` — a list saved by a release of that generation with "all off". */
static std::string allOffAt(int g, const std::set<std::string>& except) {
    std::string out;
    for (const auto& m : modeGens()) if (!m.option && m.gen <= g && !except.count(m.id)) out += (out.empty() ? "" : ",") + std::string(m.id);
    return out;
}

int main() {
    std::printf("§1 migration (no seen@)\n");
    {
        const NewModesState s = newModesState(tokens(""));
        CHECK(!s.restricted && !s.stamped && s.seen == kModeGenNow && !s.off, "a fresh / unrestricted list: everything seen, new modes on");
        CHECK(effectiveBlocked("", 0) == "", "…nothing enforced off");
        CHECK(effectiveBlocked("lora+all,avdata+text", 0) == "lora+all,avdata+text", "options alone are not a restriction");
        CHECK(effectiveBlocked("+same", 1) == "", "a region tick alone is not a restriction");
    }
    {   // restricted, several modes still on, no stamp: a short list cannot be dated, so it counts as having seen the
        // previous release's list (kUnstampedRestrictedSeen) — only the switches new in THIS update (SATELLITE) come in off
        const std::string own = "dab,drm";
        const NewModesState s = newModesState(tokens(own));
        CHECK(s.restricted && !s.single && s.seen == kUnstampedRestrictedSeen && s.off, "restricted (dab,drm): seen = %d, new modes off, not single", s.seen);
        const std::string e = effectiveBlocked(own, 0);
        CHECK(e == "dab,drm,sat", "…only SATELLITE (new in 96) off: %s", e.c_str());
        CHECK(!in(e, "adsb") && !in(e, "ism") && !in(e, "dvsig") && !in(e, "vormap"), "…★ ADS-B, ISM, DV signalling, VOR MAP keep the owner's ticks (on)");
        CHECK(only(e).empty(), "…several modes left: no exit lock");
        CHECK(!in(effectiveBlocked(own + ",newmodes@on", 0), "sat"), "…the owner's 'switch them on': on");
        CHECK(!in(effectiveBlocked(own + ",+sat", 0), "sat"), "…ticked by the owner (+sat): on");
        CHECK(effectiveBlocked("rade", 0) == "rade,sat", "a benchmark-style list (rade): only SATELLITE off");
        CHECK(newModesState(tokens("dab,seen@80")).seen == 80 && in(effectiveBlocked("dab,seen@80", 0), "dvsig"),
              "…but a STAMPED restricted list is judged from its stamp (80: DV signalling new, off)");
    }
    {   // ★★★ PAUL — every switch off except ADS-B, saved by a beta of generation 80 (11-18 build of the only-mode lock)
        const std::string paul = allOffAt(80, {"adsb", "commb"});
        CHECK(!in(paul, "dvsig") && !in(paul, "vormap") && in(paul, "freedv"), "the shape: names nothing newer than 80");
        const NewModesState s = newModesState(tokens(paul));
        CHECK(s.restricted && s.single && s.seen == 80 && s.off, "Paul: restricted, single-mode, seen = 80");
        const std::string e = effectiveBlocked(paul, 0);
        for (const char* id : { "dvsig", "varatag", "lrpt", "vor", "sarsat", "ale", "fctlm", "satlabels", "orbcomm", "vormap" })
            CHECK(in(e, id), "Paul: new switch %s off", id);
        CHECK(!in(e, "adsb") && !in(e, "commb"), "Paul: ADS-B (and Comm-B under it) still on");
        CHECK(only(e) == "adsb", "★★★ Paul after the update: ADS-B only, the exit lock ON (got '%s')", only(e).c_str());
        CHECK(only(paul) != "adsb", "…and without the rule it was NOT (the bug: dvsig and vormap came in on)");
        CHECK(only(effectiveBlocked(paul + ",newmodes@on", 0)) == "adsb", "★ single-mode wins over 'switch them on': still ADS-B only");
        CHECK(only(effectiveBlocked(paul + ",+vormap", 0)).empty(), "the owner ticks VOR MAP: two modes — their choice, no lock");
        // the same list from a beta of 87 (it names dvsig): the 90+ switches are new
        const std::string p87 = allOffAt(87, {"adsb", "commb"});
        const std::string e87 = effectiveBlocked(p87, 0);
        CHECK(in(e87, "vormap") && in(e87, "orbcomm") && only(e87) == "adsb", "an ADS-B-only list from 87: VOR MAP / ORBCOMM off, locked");
    }
    {   // single-mode, a demodulator-free DAB-only radio from 63
        const std::string dabOnly = allOffAt(63, {"dab"});
        const std::string e = effectiveBlocked(dabOnly, 0);
        CHECK(in(e, "freedv") && in(e, "m17") && in(e, "dvsig") && in(e, "avdata") && in(e, "sondes") && in(e, "vormap"),
              "a DAB-only list from 63: every newer demodulator and whole-radio mode off");
        CHECK(only(e) == "dab", "…and it is DAB only (got '%s')", only(e).c_str());
    }

    std::printf("§2 stamped lists (saved by beta96 or later)\n");
    {
        CHECK(effectiveBlocked("seen@87", 0) == "", "unrestricted, seen 87: the 90+ switches come in ON (as ever)");
        const std::string e = effectiveBlocked("seen@87,newmodes@off", 0);
        CHECK(in(e, "fctlm") && in(e, "vormap") && !in(e, "dvsig"), "seen 87, 'leave them off': 90+ off, 87 not new");
        CHECK(in(effectiveBlocked("dab,seen@87", 0), "vormap"), "restricted, seen 87, automatic: new off");
        CHECK(!in(effectiveBlocked("dab,seen@87,newmodes@on", 0), "vormap"), "restricted, seen 87, 'switch them on': on");
        CHECK(effectiveBlocked("dab,seen@96", 0) == "dab", "seen = this build: nothing is new");
        // a stamped single-mode list: the lock holds for anything newer than the stamp, whatever the answer
        const std::string p = allOffAt(87, {"adsb", "commb"}) + ",seen@87,newmodes@on";
        CHECK(only(effectiveBlocked(p, 0)) == "adsb", "stamped ADS-B only + 'switch them on': still locked");
        CHECK(newModesState(tokens("seen@9999")).seen == kModeGenNow && newModesState(tokens("seen@-4")).seen == 0, "a stamp is clamped");
    }

    std::printf("§3 the owner's words are never published\n");
    {
        CHECK(effectiveBlocked("seen@96,newmodes@off", 0) == "", "seen@ / newmodes@ are not modes");
        CHECK(effectiveBlocked("wfm,seen@96,newmodes@on,+same", 1) == "wfm", "…nor '+' ticks");
        CHECK(isNewModesWord("seen@96") && isNewModesWord("newmodes@on") && !isNewModesWord("seen") && !isNewModesWord("wfm"), "isNewModesWord");
        CHECK(!newModeOff("lora+all", tokens(allOffAt(63, {"adsb"}))), "an option is never a new mode");
        CHECK(newModeOff("vormap", tokens(allOffAt(80, {"adsb"}))) && !newModeOff("vormap", tokens(allOffAt(80, {"adsb"}) + ",vormap")),
              "newModeOff: once, not when the owner already said off");
    }

    std::printf("§4 the table\n");
    {
        std::set<std::string> seen;
        for (const auto& m : modeGens()) {
            CHECK(seen.insert(m.id).second, "%s listed once", m.id);
            CHECK(m.gen >= 0 && m.gen <= kModeGenNow, "%s generation in range", m.id);
            CHECK((std::string(m.id).find('+') != std::string::npos || std::string(m.id) == "ft8-deep") == m.option, "%s option flag", m.id);
        }
        for (const char* d : listeningDemods()) CHECK(modeGen(d) != nullptr, "demodulator %s has a generation", d);
        for (const char* w : wholeRadioModes()) CHECK(modeGen(w) != nullptr, "whole-radio mode %s has a generation", w);
        const std::string page = readFile("android/app/src/main/cpp/vibe_setup_page.h");
        CHECK(!page.empty(), "read the Setup page (run from the repo root)");
        const size_t a = page.find("const BLOCKABLE = ["), b = page.find("\n];", a);
        CHECK(a != std::string::npos && b != std::string::npos, "the page's BLOCKABLE");
        const std::string blk = page.substr(a, b - a);
        std::regex re("\\{ id: \"([a-z0-9+-]+)\"([^}]*)\\}");
        int n = 0;
        for (std::sregex_iterator it(blk.begin(), blk.end(), re), e; it != e; ++it) {
            const std::string id = (*it)[1], rest = (*it)[2];
            if (rest.find("node:") != std::string::npos) continue;
            ++n;
            CHECK(modeGen(id) != nullptr, "★ the page's switch '%s' is in modeGens() — add it with the release it ships in", id.c_str());
        }
        CHECK(n > 60, "the page lists %d switches", n);
        // the page's copy of the table (MODE_GENS) must be this table, exactly
        const size_t ga = page.find("const MODE_GENS = {"), gb = page.find("};", ga);
        CHECK(ga != std::string::npos, "the page carries MODE_GENS");
        if (ga != std::string::npos) {
            const std::string g = page.substr(ga, gb - ga);
            std::regex pr("([a-z0-9]+): ([0-9]+)");
            std::set<std::string> pageIds;
            for (std::sregex_iterator it(g.begin(), g.end(), pr), e; it != e; ++it) {
                const std::string id = (*it)[1]; const int gen = std::stoi((*it)[2]);
                pageIds.insert(id);
                const ModeGen* m = modeGen(id);
                CHECK(m && !m->option && m->gen == gen, "page MODE_GENS %s: %d matches the server", id.c_str(), gen);
            }
            for (const auto& m : modeGens()) if (!m.option) CHECK(pageIds.count(m.id), "page MODE_GENS has %s", m.id);
            CHECK(page.find("const MODE_GEN_NOW = " + std::to_string(kModeGenNow) + ";") != std::string::npos, "page MODE_GEN_NOW = %d", kModeGenNow);
            CHECK(page.find("const MODE_GEN_UNSTAMPED = " + std::to_string(kUnstampedRestrictedSeen) + ";") != std::string::npos, "page MODE_GEN_UNSTAMPED = %d", kUnstampedRestrictedSeen);
        }
        // the app's switch list (ServerModeScreen BLOCKABLE) is covered too
        const std::string app = readFile("src/screens/ServerModeScreen.tsx");
        const size_t c = app.find("const BLOCKABLE: ModeEntry[] = ["), d = app.find("\n];", c);
        CHECK(c != std::string::npos, "the app's BLOCKABLE");
        if (c != std::string::npos) {
            const std::string ab = app.substr(c, d - c);
            std::regex are("\\{ id: '([a-z0-9+-]+)'([^}]*)\\}");
            for (std::sregex_iterator it(ab.begin(), ab.end(), are), e; it != e; ++it) {
                const std::string id = (*it)[1], rest = (*it)[2];
                if (rest.find("node:") != std::string::npos) continue;
                CHECK(modeGen(id) != nullptr, "the app's switch '%s' is in modeGens()", id.c_str());
            }
        }
        const std::string j = modeGensJson();
        CHECK(j.find("\"modeGen\":96") != std::string::npos && j.find("\"dvsig\":87") != std::string::npos
              && j.find("\"vormap\":93") != std::string::npos && j.find("lora+all") == std::string::npos, "modeGensJson: the table, no options");
    }

    std::printf("§5 source contracts\n");
    {
        const std::string shim = readFile("android/app/src/main/cpp/local_sdr_shim.cpp");
        const size_t a = shim.find("std::string onlyModeNow() const {");
        const std::string body = a == std::string::npos ? "" : shim.substr(a, 900);
        CHECK(body.find("vibereach::onlyModeLeft([](const char* m) { return vsModeBlocked(m); })") != std::string::npos,
              "onlyModeNow asks onlyModeLeft over the ENFORCED list (vsModeBlocked)");
        CHECK(shim.find("g_vsBlockedModesCsv = vibereach::effectiveBlocked(csv, g_vsRegionKnown);") != std::string::npos,
              "the owner's list is enforced through effectiveBlocked");
        CHECK(shim.find("\"blocked\":\" + vsBlockedModesJson()") != std::string::npos || shim.find("vsBlockedModesJson()") != std::string::npos,
              "hwinfo `blocked` is the enforced list");
        for (const char* w : { "vsAdsbWhyNot", "vsIsmWhyNot", "vsNdbWhyNot", "vsAisWhyNot", "vsVormapWhyNot" }) {
            const size_t p = shim.find(std::string("static std::string ") + w + "() {");
            CHECK(p != std::string::npos && shim.substr(p, 1200).find("vsModeBlocked(") != std::string::npos,
                  "%s (hwinfo `modes` for its mode) asks the enforced list", w);
        }
        CHECK(shim.find("vibereach::regionJson(g_vsRegionKnown) + vibereach::modeGensJson()") != std::string::npos,
              "the app's engine call carries the table");
    }

    std::printf("\ntest-new-modes: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
