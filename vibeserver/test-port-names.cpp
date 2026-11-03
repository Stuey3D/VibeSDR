// ★★★ THE OWNER'S NAMES FOR A RADIO'S OWN ANTENNA PORTS — vibe_port_names.h (V12, 2026-11-03).
// The parse / clean / bound / escape rules every way in goes through: the config load, the setup save and the shim's
// hwinfo. Checked:
//   1. a stored blob round-trips, in the owner's order, for every driver's port names (USRP, RSP2, RSPdx, RSPduo);
//   2. bounds — name 40 bytes cut on a UTF-8 boundary, port 32, at most 16 ports, a blob over 4000 bytes dropped whole;
//   3. cleaning — control characters, C1 controls, stray bytes gone; whitespace collapsed; an empty name = no entry;
//   4. escaping — quotes, backslashes, newlines and < > & never reach the JSON raw (so never a <script> close);
//   5. malformed input — not an object, a bad \u escape, a non-string value — is "no names", never a crash;
//   6. what a person reads: "Discone (RX2)", "Discone · RX2 (A)", or the bare port.
#include "vibe_port_names.h"

#include <cstdio>
#include <string>

using namespace vibe;
static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what.c_str()); if (!ok) fails++; }

int main() {
    std::printf("test-port-names\n");
    namespace pn = portnames;

    // 1. round trip, order kept
    {
        const std::string blob = "{\"TX/RX (A)\":\"FM Yagi\",\"RX2 (A)\":\"Discone\",\"RX2 (B)\":\"Sky loop\"}";
        const auto n = pn::parse(blob);
        check(n.size() == 3, "three USRP ports parsed");
        check(n.size() == 3 && n[0].first == "TX/RX (A)" && n[1].first == "RX2 (A)" && n[2].first == "RX2 (B)",
              "…in the order the owner gave them, not sorted");
        check(pn::nameFor(n, "RX2 (A)") == "Discone", "nameFor finds a port");
        check(pn::nameFor(n, "TX/RX (B)").empty(), "…and a port with no name has none");
        check(pn::parse(pn::toJson(n)) == n, "toJson → parse is the identity");
        check(pn::normalise(blob) == blob, "normalise keeps a clean blob byte for byte: " + pn::normalise(blob));
        const auto duo = pn::parse("{\"Tuner 1 50\\u03a9\":\"Long wire\",\"Tuner 1 Hi-Z\":\"Mini-Whip\",\"Tuner 2 50\xce\xa9\":\"Discone\"}");
        check(pn::nameFor(duo, "Tuner 1 50\xce\xa9") == "Long wire", "an RSPduo port spelt \\u03a9 in the file is the port spelt Ω");
        check(pn::nameFor(duo, "Tuner 2 50\xce\xa9") == "Discone", "…and spelt raw too");
        const auto rsp2 = pn::parse("{\"A\":\"Dipole\",\"B\":\"Vertical\",\"Hi-Z\":\"Long wire\"}");
        check(rsp2.size() == 3 && pn::nameFor(rsp2, "Hi-Z") == "Long wire", "RSP2 A / B / Hi-Z");
        check(pn::normalise("").empty() && pn::normalise("{}").empty(), "no names is stored as nothing at all");
    }
    // 2. bounds
    {
        pn::Names n;
        pn::set(n, "A", std::string(100, 'x'));
        check(n.size() == 1 && n[0].second.size() == pn::kMaxNameBytes, "a 100-byte name is cut to 40");
        std::string greek;
        for (int i = 0; i < 30; i++) greek += "\xce\xa9";   // 60 bytes of Ω
        pn::set(n, "B", greek);
        const std::string& g = n[1].second;
        check(g.size() == 40 && (static_cast<unsigned char>(g.back()) & 0xC0) != 0xC0, "…cut on a UTF-8 boundary, never half a glyph");
        std::string odd = "x"; for (int i = 0; i < 25; i++) odd += "\xce\xa9";
        pn::set(n, "C", odd);
        check(n[2].second.size() == 39, "an odd-length cut backs off to the last whole glyph (39 bytes)");
        pn::set(n, std::string(60, 'p'), "Long port");
        check(n.back().first.size() == pn::kMaxPortBytes, "a port key is bounded to 32");
        pn::Names many;
        for (int i = 0; i < 30; i++) pn::set(many, "P" + std::to_string(i), "N" + std::to_string(i));
        check(many.size() == pn::kMaxPorts, "at most 16 ports");
        check(pn::parse("{\"A\":\"" + std::string(5000, 'y') + "\"}").empty(), "a blob over 4000 bytes is dropped whole");
    }
    // 3. cleaning
    {
        pn::Names n;
        pn::set(n, "A", "  Disc\tone \n  in\x01 the\x7f loft  ");
        check(pn::nameFor(n, "A") == "Disc one in the loft", "control characters gone, whitespace collapsed and trimmed: '" + pn::nameFor(n, "A") + "'");
        pn::set(n, "B", "Bad\xc2\x85" "C1 \xff\xfe stray");
        check(pn::nameFor(n, "B") == "BadC1 stray", "C1 controls and stray bytes dropped: '" + pn::nameFor(n, "B") + "'");
        pn::set(n, "A", "   ");
        check(pn::nameFor(n, "A").empty() && n.size() == 1, "an all-space name REMOVES the entry");
        pn::set(n, "B", "Renamed");
        check(pn::nameFor(n, "B") == "Renamed" && n.size() == 1, "setting a port again replaces its name");
        check(pn::parse("{\"A\":\"\",\"B\":\"x\"}").size() == 1, "an empty name in the file is no entry");
    }
    // 4. escaping
    {
        pn::Names n;
        pn::set(n, "A", "</script><b onclick=\"x\">&amp;\\");
        const std::string j = pn::toJson(n);
        check(j.find('<') == std::string::npos && j.find('>') == std::string::npos && j.find('&') == std::string::npos,
              "no raw < > & in the JSON: " + j);
        check(j.find("\\\"x\\\"") != std::string::npos && j.find("\\\\") != std::string::npos, "quotes and backslashes escaped");
        check(pn::parse(j) == n, "…and it parses back to the same name");
        pn::Names q;
        pn::set(q, "TX/RX \"A\"", "Q");
        check(pn::parse(pn::toJson(q)) == q, "a quote in a PORT key survives too");
    }
    // 5. malformed
    {
        check(pn::parse("[\"A\",\"B\"]").empty(), "an array is not names");
        check(pn::parse("{\"A\":\"x\"").empty(), "a truncated object is not names (never half-applied)");
        check(pn::parse("{\"A\":\"\\uZZZZ\"}").empty(), "a bad \\u escape is no names, not an exception");
        const auto n = pn::parse("{\"A\":7,\"B\":\"Vertical\",\"C\":{\"x\":1}}");
        check(n.size() == 1 && pn::nameFor(n, "B") == "Vertical", "non-string values skipped, the rest kept");
        check(pn::parse("not json").empty() && pn::parse("\"A\"").empty(), "plain text is no names");
    }
    // 6. display
    {
        const auto n = pn::parse("{\"RX2\":\"Discone\",\"RX2 (A)\":\"FM Yagi\"}");
        check(pn::display(n, "RX2") == "Discone (RX2)", "Discone (RX2)");
        check(pn::display(n, "RX2 (A)") == "FM Yagi \xC2\xB7 RX2 (A)", "FM Yagi · RX2 (A) — no nested brackets");
        check(pn::display(n, "TX/RX") == "TX/RX", "a port with no name reads as the port");
    }
    // 7. into hwinfo (withNames) — only beside a listed port, never twice
    {
        const std::string names = pn::toJson(pn::parse("{\"RX2 (A)\":\"Discone\"}"));
        const std::string usrp = "{\"driver\":\"usrp\",\"rates\":[1000000],\"antennas\":[\"RX2 (A)\",\"TX/RX (A)\"],\"antenna\":\"RX2 (A)\"}";
        const std::string withN = pn::withNames(usrp, names);
        check(withN.find(",\"antennaNames\":{\"RX2 (A)\":\"Discone\"}}") != std::string::npos, "a USRP's caps gain antennaNames: " + withN);
        check(pn::withNames(withN, names) == withN, "…never twice");
        const std::string rtl = "{\"driver\":\"rtl\",\"antennas\":[],\"gains\":[0,9]}";
        check(pn::withNames(rtl, names) == rtl, "a radio with one input (empty antennas) gets none — nothing to name on screen");
        check(pn::withNames("{\"driver\":\"airspyhf\"}", names) == "{\"driver\":\"airspyhf\"}", "a radio with no antennas list gets none");
        check(pn::withNames(usrp, "") == usrp && pn::withNames(usrp, "{}") == usrp, "no names: caps unchanged");
        const std::string rsp = "{\"driver\":\"sdrplay\",\"antennas\":[\"A\",\"B\",\"Hi-Z\"],\"antenna\":\"A\",\"antennaLocked\":false}";
        check(pn::withNames(rsp, pn::toJson(pn::parse("{\"Hi-Z\":\"Long wire\"}"))).find("\"antennaNames\":{\"Hi-Z\":\"Long wire\"}") != std::string::npos,
              "an RSP2 gets them the same way — every driver that lists ports");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
