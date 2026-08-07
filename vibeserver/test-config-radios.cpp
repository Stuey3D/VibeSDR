// ★★★ SEVERAL RADIOS IN ONE CONFIG FILE — and, more importantly, ONE radio in the OLD one.
//
// The migration is the dangerous half. Every install in the field has today's single-radio file,
// and this change gives radios two gates they never had (`enabled`, `configured`). Default either
// to false and every working receiver goes dark on upgrade — which is precisely what happened when
// config.json itself was introduced and took the demo off the air. A test on a FRESH config can
// never catch that, so the first thing here is a REAL old-format file.
#include "vibeserver_config.h"
#include <cstdio>
#include <string>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

int main() {
    using namespace vsconfig;
    std::string err;

    std::printf("\nAn EXISTING single-radio config must survive the upgrade\n");
    {
        // Shaped like a real one: a locked, multi-listener HF receiver, exactly like the demo Pi.
        const std::string old = R"({
          "configured": true, "mode": "locked", "sharing": "local",
          "name": "Pi500", "place": "Northampton", "locator": "IO92nh",
          "pin": "", "adminPass": "Test123",
          "freq": 6800000, "rate": 8000000, "lockFreq": 6800000, "lockRate": 8000000,
          "users": 30, "gain": 40, "rfNotch": true, "zoomSpectrum": true,
          "cpuGovernor": "performance", "port": 48000
        })";
        ServerConfig s;
        ok(fromJson(old, s, err), "it loads", err);
        ok(s.radios.size() == 1, "it became a one-radio machine", std::to_string(s.radios.size()));
        if (s.radios.size() == 1) {
            const auto& r = s.radios[0];
            ok(r.enabled,    "★ the radio is ENABLED — it was serving before the upgrade");
            ok(r.configured, "★ the radio is CONFIGURED — it must not go dark waiting for a tab");
            ok(r.mode == Mode::LockedRange, "its locked window survived");
            ok(r.lockFreq == 6800000, "the locked centre survived", std::to_string((long long)r.lockFreq));
            ok(r.users == 30, "its listener limit survived", std::to_string(r.users));
            ok(r.rfNotch && r.zoomSpectrum, "its front-end settings survived");
        }
        ok(s.adminPass == "Test123", "the admin password moved to the machine", s.adminPass);
        ok(s.place == "Northampton", "the location moved to the machine", s.place);
        ok(s.configured, "the machine is configured");
    }

    std::printf("\nThree radios, as the Pi will actually run them\n");
    {
        ServerConfig s;
        s.configured = true; s.adminPass = "Test123"; s.place = "Northampton";
        RadioConfig hf;  hf.serial = "240513CA60"; hf.driver = "sdrplay"; hf.label = "HF";
                         hf.mode = Mode::LockedRange; hf.users = 30; hf.lockFreq = 6800000;
                         hf.configured = true;
        RadioConfig fm;  fm.serial = "DD52B980BE4946DA"; fm.driver = "airspyhf"; fm.label = "FM";
                         fm.users = 1; fm.freq = 96600000; fm.demodMode = "wfm";
                         fm.configured = true;
        RadioConfig rtl; rtl.serial = "00000003"; rtl.driver = "rtlsdr"; rtl.label = "VHF";
                         rtl.enabled = false;      // switched off in the TUI
        s.radios = {hf, fm, rtl};

        const std::string j = toJson(s);
        ServerConfig back;
        ok(fromJson(j, back, err), "a three-radio config round-trips", err);
        ok(back.radios.size() == 3, "all three came back", std::to_string(back.radios.size()));
        if (back.radios.size() == 3) {
            ok(back.radios[0].serial == "240513CA60" && back.radios[0].driver == "sdrplay",
               "★ radio 0 is the RSP, by serial", back.radios[0].serial);
            ok(back.radios[1].demodMode == "wfm" && back.radios[1].freq == 96600000,
               "★ radio 1 is the Airspy on FM");
            ok(!back.radios[2].enabled, "★ radio 2 is disabled, and stayed disabled");
            ok(back.radios[0].users == 30 && back.radios[1].users == 1,
               "★ per-radio listener limits are independent");
        }
        ok(back.adminPass == "Test123", "the shared password is stated once");
    }

    std::printf("\nComposing what one radio's process actually runs\n");
    {
        ServerConfig s;
        s.configured = true; s.adminPass = "pw"; s.place = "Northampton"; s.name = "Pi500";
        RadioConfig r; r.label = "FM"; r.configured = true; r.users = 1;
        r.freq = 96600000; r.demodMode = "wfm"; r.port = 48001;
        s.radios.push_back(r);

        const Config c = effectiveFor(s, s.radios[0]);
        ok(c.adminPass == "pw", "it inherits the shared password");
        ok(c.place == "Northampton", "it inherits the site");
        ok(c.name == "FM", "★ listeners see the RADIO's label, not the machine name", c.name);
        ok(c.freq == 96600000 && c.demodMode == "wfm", "it gets its own radio settings");
        ok(c.port == 48001, "it gets its own port");
        ok(c.configured, "configured, because BOTH the machine and the radio are");
    }

    std::printf("\n★ BOTH gates must hold, and they mean different things\n");
    {
        ServerConfig s; s.configured = true;
        RadioConfig r; r.configured = false;    // never opened its tab
        ok(!effectiveFor(s, r).configured, "★ a radio whose tab was never saved is NOT configured");
        s.configured = false; r.configured = true;
        ok(!effectiveFor(s, r).configured, "★ nor is one on a machine that is not set up");
    }

    std::printf("\nThe array splitter has to survive real text\n");
    {
        // ★★★ AN UNBALANCED BRACE, AND THAT IS THE WHOLE POINT. The first version of this test used
        //     "Loft { HF }" — which naive depth counting survives, because the braces BALANCE. It
        //     passed against an implementation with the string tracking deliberately ripped out,
        //     i.e. it proved nothing at all. A single "{" cannot balance, so depth counting that
        //     ignores strings runs past the end of this radio's object and swallows the next one.
        // ★ Every test here was run against a broken implementation before being believed. This is
        //   the third time on this project that a green test turned out to be testing nothing.
        ServerConfig s;
        RadioConfig a; a.label = "Loft {"; a.serial = "AAA"; a.users = 7;
        RadioConfig b; b.label = "Shack"; b.serial = "BBB"; b.users = 3;
        s.radios = {a, b};
        ServerConfig back;
        ok(fromJson(toJson(s), back, err), "it round-trips with an unbalanced brace in a label", err);
        ok(back.radios.size() == 2, "★ still two radios, not one or three",
           std::to_string(back.radios.size()));
        if (back.radios.size() == 2) {
            ok(back.radios[0].label == "Loft {", "★ the label is intact", back.radios[0].label);
            ok(back.radios[0].users == 7 && back.radios[1].users == 3,
               "★ and neither radio read the other's settings");
        }
    }

    std::printf("\nPorts: the primary keeps the machine's port, the rest queue behind it\n");
    {
        ServerConfig s; s.configured = true; s.port = 48000;
        RadioConfig a; a.serial="A"; a.enabled=true;  a.configured=true;
        RadioConfig b; b.serial="B"; b.enabled=true;  b.configured=true;
        RadioConfig c; c.serial="C"; c.enabled=true;  c.configured=true;
        s.radios = {a,b,c};
        ok(primaryRadio(s) == 0, "the first ready radio is primary");
        ok(portForRadio(s,0) == 48000, "★ the primary keeps 48000", std::to_string(portForRadio(s,0)));
        ok(portForRadio(s,1) == 48001, "the second gets 48001", std::to_string(portForRadio(s,1)));
        ok(portForRadio(s,2) == 48002, "the third gets 48002", std::to_string(portForRadio(s,2)));

        // ★ A LATER radio going dark must not move an EARLIER one's port — listeners are on it.
        s.radios[2].enabled = false;
        ok(portForRadio(s,1) == 48001, "★ switching off radio 3 leaves radio 2 where it was",
           std::to_string(portForRadio(s,1)));

        // The first radio not being ready hands the machine port to the next one that is.
        s.radios[2].enabled = true;
        s.radios[0].configured = false;
        ok(primaryRadio(s) == 1, "an unconfigured first radio is not primary");
        ok(portForRadio(s,1) == 48000, "★ the machine's port follows the primary",
           std::to_string(portForRadio(s,1)));

        // An owner who pinned a port for a router rule keeps it.
        s.radios[2].port = 49000;
        ok(portForRadio(s,2) == 49000, "an explicit port wins", std::to_string(portForRadio(s,2)));
    }

    std::printf("\nNo radio ready is a state, not an error\n");
    {
        ServerConfig s; s.configured = true;
        RadioConfig a; a.enabled = true; a.configured = false;   // ticked, never set up
        s.radios = {a};
        ok(primaryRadio(s) == -1, "★ nothing is primary, and nothing crashes");
        ok(portForRadio(s,0) == 48001, "and it does not squat on the machine's port",
           std::to_string(portForRadio(s,0)));
    }

    std::printf("\nA machine with no radios is a valid answer, not an error\n");
    {
        ServerConfig s; s.configured = true;
        ServerConfig back;
        ok(fromJson(toJson(s), back, err), "an empty radio list round-trips", err);
        ok(back.radios.empty(), "★ and stays empty rather than inventing one");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
