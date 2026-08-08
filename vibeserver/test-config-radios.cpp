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

    std::printf("\nA machine with NO radio ready still knows its own secrets\n");
    {
        // ★★★ THE FRESH-INSTALL STATE, and it locked the owner out of their own setup page. The TUI
        //     wizard writes the admin password, sets configured=false because the browser finishes
        //     setup, and points the owner at the web page — which then refused the password they
        //     had just chosen. main.cpp applied every machine-wide setting only when it had found a
        //     radio that was enabled AND configured, and on a brand-new install there is no such
        //     radio by definition. Composing against a DEFAULT radio is what the fix relies on, so
        //     that is what this pins down.
        ServerConfig s;
        s.configured = false;                 // ← the browser has not finished setup yet
        s.adminPass  = "Test123";
        s.pin        = "1234";
        s.name       = "Pi500";
        s.trustedProxies = "127.0.0.1";
        RadioConfig detected;                 // enabled by the wizard, never configured
        detected.serial = "00000001"; detected.enabled = true; detected.configured = false;
        s.radios.push_back(detected);

        const Config c = effectiveFor(s, RadioConfig{});
        ok(c.adminPass == "Test123",
           "★★★ the admin password survives with no radio configured — the setup page needs it",
           c.adminPass);
        ok(c.pin == "1234", "and the listening PIN", c.pin);
        ok(c.name == "Pi500", "and the machine's name", c.name);
        ok(c.trustedProxies == "127.0.0.1", "and the trusted proxies", c.trustedProxies);
        ok(!c.configured, "still not configured — this does not pretend setup is finished");
    }

    std::printf("\nWhere an unlocked radio starts\n");
    {
        // ★★★ The owner types a landing frequency; the radio must OPEN there. It used to open on
        //     `freq` regardless, so an RTL told to land on 648 kHz still came up at 145 MHz — the
        //     waterfall, the spectrogram and the landing page all showed a band nobody wanted, and
        //     the first listener paid for a 144 MHz retune to reach where the owner had already
        //     said (Stuart, 2026-08-08: "it always keeps going back to 145MHz").
        ServerConfig s; s.configured = true;
        RadioConfig r; r.configured = true; r.mode = Mode::SingleUser;
        r.freq = 145000000; r.landingFreq = 648000; r.demodMode = "am";
        s.radios.push_back(r);
        const Config c = effectiveFor(s, s.radios[0]);
        ok(c.freq == 648000, "★ an unlocked radio opens on its landing frequency",
           std::to_string((long long)c.freq));
        ok(c.landingFreq == 648000, "and still lands listeners there");

        // ★★ A LOCKED radio's centre is the owner's window and must NOT be dragged about by a
        //    landing frequency inside it — that would move the whole band under every listener.
        ServerConfig s2; s2.configured = true;
        RadioConfig r2; r2.configured = true; r2.mode = Mode::LockedRange;
        r2.freq = 6500000; r2.landingFreq = 7074000;
        s2.radios.push_back(r2);
        ok(effectiveFor(s2, s2.radios[0]).freq == 6500000,
           "★ a LOCKED radio keeps its own centre");

        // 0 means "same as freq" and must not be read as "tune to DC".
        ServerConfig s3; s3.configured = true;
        RadioConfig r3; r3.configured = true; r3.mode = Mode::SingleUser;
        r3.freq = 145000000; r3.landingFreq = 0;
        s3.radios.push_back(r3);
        ok(effectiveFor(s3, s3.radios[0]).freq == 145000000,
           "★ no landing frequency set leaves the centre alone");
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

    std::printf("\n★ A live PATCH must not be mistaken for an old-format file\n");
    {
        // The server persists live changes as fragments — {"gain":123} when an admin nudges it.
        ServerConfig s;
        s.configured = true; s.adminPass = "pw";
        RadioConfig a; a.serial="A"; a.label="HF"; a.enabled=true;  a.configured=true;
        RadioConfig b; b.serial="B"; b.label="FM"; b.enabled=true;  b.configured=false;
        s.radios = {a, b};

        ok(fromJson("{\"gain\":123}", s, err), "a one-field patch applies", err);
        ok(s.radios.size() == 2, "★ it did not invent a radio", std::to_string(s.radios.size()));
        if (s.radios.size() == 2) {
            ok(!s.radios[1].configured,
               "★ a radio that was NOT set up is still not set up");
            ok(s.radios[0].label == "HF" && s.radios[1].label == "FM",
               "★ and neither radio was rewritten");
        }
        ok(s.adminPass == "pw", "the admin password survived a patch");
    }

    std::printf("\n★ SIMPLE keeps the port it has always had; FULL gets a front door\n");
    {
        // A plain single-radio receiver, exactly as thousands were set up: no lock, one listener.
        const std::string simpleOld = R"({"configured":true,"mode":"single","users":1,
            "name":"Loft","adminPass":"x","freq":9410000,"rate":2400000})";
        ServerConfig s;
        ok(fromJson(simpleOld, s, err), "an old SIMPLE config loads", err);
        ok(!s.fullMode, "★ it stays SIMPLE — the switch did not exist, so it was never chosen");
        ok(!needsFrontDoor(s), "★ no front door");
        ok(portForRadio(s, 0) == 48000,
           "★ its receiver is on 48000, exactly where it has always been",
           std::to_string(portForRadio(s, 0)));

        // The demo Pi: one radio, but locked and shared — a FULL server by any reading.
        const std::string fullOld = R"({"configured":true,"mode":"locked","users":30,
            "name":"Pi500","adminPass":"x","freq":6800000,"lockFreq":6800000,"rate":8000000})";
        ServerConfig f;
        ok(fromJson(fullOld, f, err), "an old FULL config loads", err);
        ok(f.fullMode, "★ a locked, many-listener server is recognised as FULL");
        ok(needsFrontDoor(f), "★ and gets a front door — with ONE radio");
        ok(portForRadio(f, 0) == 48001, "★ its radio moves behind the front door",
           std::to_string(portForRadio(f, 0)));
    }

    std::printf("\n★ Three radios in FULL mode queue behind the front door\n");
    {
        ServerConfig s; s.configured = true; s.fullMode = true; s.port = 48000;
        RadioConfig a; a.serial="A"; a.enabled=true; a.configured=true;
        RadioConfig b; b.serial="B"; b.enabled=true; b.configured=true;
        RadioConfig c; c.serial="C"; c.enabled=true; c.configured=true;
        s.radios = {a,b,c};
        ok(portForRadio(s,0) == 48001 && portForRadio(s,1) == 48002 && portForRadio(s,2) == 48003,
           "★ 48001, 48002, 48003 — and nothing on 48000 but the front door",
           std::to_string(portForRadio(s,0)) + "," + std::to_string(portForRadio(s,1))
           + "," + std::to_string(portForRadio(s,2)));

        // ★ And the same three radios in SIMPLE mode keep the old shape.
        s.fullMode = false;
        ok(portForRadio(s,0) == 48000, "★ in SIMPLE the first radio still owns 48000",
           std::to_string(portForRadio(s,0)));
    }

    std::printf("\n★ A save must NEVER be able to delete radios\n");
    {
        ServerConfig s; s.configured = true; s.adminPass = "pw";
        RadioConfig a; a.serial="A"; a.label="HF"; a.enabled=true; a.configured=true;
        RadioConfig b; b.serial="B"; b.label="FM"; b.enabled=true; b.configured=true;
        s.radios = {a, b};

        // Exactly what a stale page posted, and it emptied the machine.
        ok(fromJson("{\"radios\":[]}", s, err), "an empty radios array is accepted", err);
        ok(s.radios.size() == 2, "★ but BOTH radios are still there",
           std::to_string(s.radios.size()));

        // A save that mentions only one radio must not remove the other.
        ok(fromJson("{\"radios\":[{\"serial\":\"A\",\"label\":\"HF\",\"users\":9}]}", s, err),
           "a save mentioning one radio applies", err);
        ok(s.radios.size() == 2, "★ the unmentioned radio survives", std::to_string(s.radios.size()));
        if (s.radios.size() == 2) {
            ok(s.radios[0].users == 9, "★ and the mentioned one was updated",
               std::to_string(s.radios[0].users));
            ok(s.radios[1].label == "FM", "★ while the other is untouched", s.radios[1].label);
        }

        // A radio we have never seen IS added — that is how new hardware arrives.
        ok(fromJson("{\"radios\":[{\"serial\":\"C\",\"label\":\"VHF\"}]}", s, err), "a new radio applies", err);
        ok(s.radios.size() == 3, "★ a genuinely new radio is added", std::to_string(s.radios.size()));
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
