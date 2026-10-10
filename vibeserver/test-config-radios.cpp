// ★★★ SEVERAL RADIOS IN ONE CONFIG FILE — and, more importantly, ONE radio in the OLD one.
//
// The migration is the dangerous half. Every install in the field has today's single-radio file,
// and this change gives radios two gates they never had (`enabled`, `configured`). Default either
// to false and every working receiver goes dark on upgrade — which is precisely what happened when
// config.json itself was introduced and took the demo off the air. A test on a FRESH config can
// never catch that, so the first thing here is a REAL old-format file.
#include "vibeserver_config.h"
#include "sdr_presence.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

    std::printf("\nWhat a LISTENER sees never carries a hardware serial\n");
    {
        // ★★★ TWO PLACES SHOW A RADIO'S NAME and both were leaking it: the landing page's list and
        //     the name painted across the waterfall once connected. The second was missed first
        //     time round — "SDRplay RSP1B 240513CA60" on the receiver screen (Stuart, 2026-08-09).
        ok(publicLabel("SDRplay RSP1B 240513CA60") == "SDRplay RSP1B",
           "★ a bare trailing serial goes", publicLabel("SDRplay RSP1B 240513CA60"));
        ok(publicLabel("Airspy HF+ (DD52B980BE4946DA)") == "Airspy HF+",
           "★ and a bracketed one", publicLabel("Airspy HF+ (DD52B980BE4946DA)"));
        ok(publicLabel("RTLSDRBlog Blog V4") == "RTLSDRBlog Blog V4",
           "★★ but a NAME is untouched — V4 is too short to be a serial");
        ok(publicLabel("Shack RSP1B") == "Shack RSP1B",
           "★★ and RSP1B survives, because R, S and P are not hex digits");
        ok(publicLabel("Loft antenna") == "Loft antenna", "an owner's own label is left alone");

        ServerConfig s3;
        s3.configured = true; s3.name = "Pi500";
        RadioConfig rr; rr.serial = "240513CA60"; rr.driver = "sdrplay";
        rr.label = "SDRplay RSP1B 240513CA60"; rr.configured = true;
        s3.radios.push_back(rr);
        ok(effectiveFor(s3, s3.radios[0]).name == "SDRplay RSP1B",
           "★★★ and the name a listener is shown has it stripped",
           effectiveFor(s3, s3.radios[0]).name);
    }

    std::printf("\nA machine-wide setting survives a per-radio patch\n");
    {
        // ★★★ THE ADMIN PAGE'S UPDATE SCHEDULE RIDES THE PER-RADIO PERSIST CHANNEL. main.cpp folds
        //     such a patch into one radio's entry; the schedule belongs to the MACHINE, was never
        //     copied across, and the file kept its old value while the page — reading the RUNNING
        //     value back — insisted it had saved. Only a restart showed otherwise.
        ServerConfig s2;
        s2.configured = true; s2.adminPass = "pw";
        s2.updateSrvHour = 3; s2.updateSrvDay = 0;     // Sundays at 03:00
        s2.updateAllHour = -1; s2.updateAllDay = -1;   // never
        RadioConfig r; r.serial = "A"; r.driver = "rtl"; r.configured = true; r.enabled = true;
        r.gain = 100;
        s2.radios.push_back(r);

        std::string js = toJson(s2), err;
        ServerConfig back;
        ok(fromJson(js, back, err), "the machine round-trips", err);
        ok(back.updateSrvHour == 3 && back.updateSrvDay == 0,
           "★★★ Sundays at 03:00 survives a save",
           std::to_string(back.updateSrvHour) + "/" + std::to_string(back.updateSrvDay));
        ok(back.updateAllHour == -1, "and 'never' stays never");

        // And the effective config a radio process runs with must carry it, since that is where
        // the persist handler reads it back from.
        const Config eff = effectiveFor(s2, s2.radios[0]);
        ok(eff.updateSrvHour == 3 && eff.updateSrvDay == 0,
           "★ and reaches the radio's effective config, which is what gets written back");
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

    // ★★★ ONE RADIO PER ADDRESS — a machine setting whose DEFAULT is the safety property.
    //     Caught here first time: the toggle SAVED but never LOADED, because the getBool went into
    //     the legacy single-radio parser and not ServerConfig's. It would have read as "the setting
    //     will not stick", on multi-radio servers only, with the file on disk looking perfectly
    //     correct — the worst kind of bug to chase from a screenshot.
    std::printf("\n★ One radio per address: absent means ENFORCED, and OFF must survive a save\n");
    {
        ServerConfig a;
        ok(fromJson("{\"name\":\"x\",\"radios\":[]}", a, err), "a config predating it loads", err);
        ok(a.oneRadioPerIp, "★★ absent = ENFORCED — an older file must not read as switched off");

        ServerConfig b;
        ok(fromJson("{\"name\":\"x\",\"oneRadioPerIp\":false,\"radios\":[]}", b, err),
           "an explicit OFF loads", err);
        ok(!b.oneRadioPerIp, "★ ...and is actually off");

        ServerConfig c;
        ok(fromJson(toJson(b), c, err), "it round-trips through a save", err);
        ok(!c.oneRadioPerIp, "★★ OFF SURVIVES THE SAVE — this is what was broken");

        // ★ And the per-radio view each running radio is handed must carry it, or the process that
        //   actually enforces the rule never hears about the owner's choice.
        RadioConfig r; b.radios.push_back(r);
        ok(!effectiveFor(b, b.radios[0]).oneRadioPerIp, "★★ it reaches the radio via effectiveFor");
    }

    // ★★ THE AIRSPY R2 / MINI's CURVE PER BAND travels with the limits it belongs to — saved,
    //    loaded, and handed to the running radio. A curve that did not survive a save would put the
    //    band back on Linearity at the next restart without a word (vibe_airspy_limit.h).
    std::printf("\n★ Airspy R2 / Mini: the per-band curve round-trips with its limit and lock\n");
    {
        ServerConfig s; s.configured = true;
        RadioConfig r; r.driver = "airspy"; r.serial = "A1B2C3D4E5F60708";
        r.gainLimits = "fm:100,air:150"; r.gainLocks = "air:1"; r.gainCurves = "fm:1,air:0";
        s.radios.push_back(r);
        ServerConfig back;
        ok(fromJson(toJson(s), back, err), "it saves and loads", err);
        ok(back.radios.size() == 1 && back.radios[0].gainCurves == "fm:1,air:0",
           "★★ gainCurves survives the save", back.radios.empty() ? "" : back.radios[0].gainCurves);
        ok(back.radios.size() == 1 && back.radios[0].gainLimits == "fm:100,air:150"
           && back.radios[0].gainLocks == "air:1", "and the limit and lock beside it");
        if (!back.radios.empty())
            ok(effectiveFor(back, back.radios[0]).gainCurves == "fm:1,air:0",
               "★★ it reaches the running radio via effectiveFor");
        ServerConfig old;
        ok(fromJson("{\"name\":\"x\",\"radios\":[{\"serial\":\"S1\",\"driver\":\"airspy\",\"gainLimits\":\"fm:100\"}]}",
                    old, err), "a config written before the curve existed loads", err);
        ok(old.radios.size() == 1 && old.radios[0].gainCurves.empty(),
           "★ ...with no curve, which the server reads as Linearity");
    }

    // ★★★ THE AERIAL'S RANGES AND FILTERS (2026-10-06). An old file has neither and must load with
    //     both empty (= nothing shown anywhere); a new one must survive the writer the setup page
    //     reads and the reader behind it — a field in only one of them is the fault this file
    //     keeps recording.
    std::printf("\nAntenna ranges and filters: old files load empty, new ones round-trip\n");
    {
        ServerConfig old;
        ok(fromJson("{\"name\":\"x\",\"radios\":[{\"serial\":\"S1\",\"driver\":\"rtlsdr\","
                    "\"antenna\":\"Discone\",\"antennaIcon\":\"discone\"}]}", old, err),
           "a config written before ranges/filters existed loads", err);
        ok(old.radios.size() == 1 && old.radios[0].antennaRanges.empty() && old.radios[0].antennaFilters.empty(),
           "★ ...with no ranges and no filters, so nothing is shown");
        ok(old.radios[0].antenna == "Discone" && old.radios[0].antennaIcon == "discone",
           "and the aerial it had is untouched");

        ServerConfig s; s.configured = true; s.fullMode = true;
        RadioConfig a; a.serial = "P2"; a.driver = "rtlsdr"; a.configured = true;
        a.antenna = "Loop"; a.antennaRanges = "0-300MHz Wideband loop; [B] 144-146MHz 2 m";
        a.antennaFilters = "bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz \"MW\" filter";
        s.radios = {a};
        ServerConfig back;
        ok(fromJson(toJson(s), back, err) && back.radios.size() == 1, "a radio with both saves and loads", err);
        ok(back.radios[0].antennaRanges == a.antennaRanges, "★★ the ranges survive the writer",
           back.radios[0].antennaRanges);
        ok(back.radios[0].antennaFilters == a.antennaFilters, "★★ the filters survive it — quotes and all",
           back.radios[0].antennaFilters);
        ok(back.radios[0].antenna == "Loop", "the description beside them is not mixed up with them",
           back.radios[0].antenna);

        // ★ An over-long list is cut at a whole entry, never mid-name.
        std::string longer;
        for (int k = 0; k < 60; k++) longer += (k ? "; " : "") + std::string("144-146MHz Two metres");
        ServerConfig big;
        ok(fromJson("{\"name\":\"x\",\"radios\":[{\"serial\":\"S2\",\"driver\":\"rtlsdr\","
                    "\"antennaRanges\":\"" + longer + "\"}]}", big, err), "an over-long list loads", err);
        const std::string& got = big.radios.empty() ? std::string() : big.radios[0].antennaRanges;
        ok(!got.empty() && got.size() <= 800 && got.substr(got.size() - 6) == "metres",
           "★ ...clamped to 800 characters at an entry boundary", std::to_string(got.size()));
    }

    // ★★★ THE USB-CHANGE ACTIONS (Stuart, 2026-10-04): a radio that has gone away is REMOVED or
    //     PAUSED from the setup page, and a new one is ADDED or REPLACES an old one. Each is a pure
    //     edit of the config, so each is checked here without a server.
    auto threeRadios = []() {
        ServerConfig s; s.configured = true; s.fullMode = true; s.port = 48000;
        RadioConfig a; a.serial = "00000001"; a.driver = "rtlsdr";  a.label = "V4 FM";
                       a.configured = true; a.gainLimits = "fm:250"; a.antenna = "Discone"; a.usbPath = "1-2";
        RadioConfig b; b.serial = "240513CA60"; b.driver = "sdrplay"; b.label = "HF";
                       b.configured = true; b.users = 30;
        RadioConfig c; c.serial = "DD52B980BE4946DA"; c.driver = "airspyhf"; c.label = "HF+";
                       c.configured = true;
        s.radios = {a, b, c};
        return s;
    };

    std::printf("\nREMOVE deletes exactly one radio\n");
    {
        ServerConfig s = threeRadios();
        ok(applySdrChange(s, "remove", "240513CA60", "", "", "", err), "remove succeeds", err);
        ok(s.radios.size() == 2 && s.radios[0].serial == "00000001" && s.radios[1].serial == "DD52B980BE4946DA",
           "★ only the named radio went, the others kept their place");
        ok(!applySdrChange(s, "remove", "240513CA60", "", "", "", err), "removing it again is refused");
        ok(!applySdrChange(s, "remove", "../etc", "", "", "", err), "★ a serial outside the alphabet is refused");
        ServerConfig back;
        ok(fromJson(toJson(s), back, err) && back.radios.size() == 2, "the removal survives a save", err);
    }

    std::printf("\nPAUSE keeps every setting; RESUME brings it back\n");
    {
        ServerConfig s = threeRadios();
        ok(applySdrChange(s, "pause", "00000001", "", "", "", err), "pause succeeds", err);
        const auto& r = s.radios[0];
        ok(!r.enabled, "★ paused = enabled false (the reconcile stops its unit)");
        ok(r.configured && r.gainLimits == "fm:250" && r.antenna == "Discone" && r.usbPath == "1-2",
           "★★ every setting is still there");
        ok(primaryRadio(s) == 1, "a paused radio is not the primary", std::to_string(primaryRadio(s)));
        ServerConfig back;
        ok(fromJson(toJson(s), back, err) && !back.radios[0].enabled && back.radios[0].gainLimits == "fm:250",
           "the pause survives a save, settings and all");
        ok(applySdrChange(back, "resume", "00000001", "", "", "", err) && back.radios[0].enabled,
           "resume sets enabled again", err);
        ok(back.radios[0].gainLimits == "fm:250" && back.radios[0].configured, "★ with its settings intact");
    }

    std::printf("\nREPLACE re-points the serial and keeps everything else\n");
    {
        ServerConfig s = threeRadios();
        ok(applySdrChange(s, "replace", "00000001", "00000007", "rtlsdr", "", err), "replace succeeds", err);
        const auto& r = s.radios[0];
        ok(r.serial == "00000007", "★ the entry now names the new radio", r.serial);
        ok(r.usbPath.empty(), "★ the old USB socket is forgotten");
        ok(r.label == "V4 FM" && r.gainLimits == "fm:250" && r.antenna == "Discone" && r.configured && r.enabled,
           "★★ label, limits, aerial and both gates are kept");
        ok(s.radios.size() == 3, "no radio was added or lost");
        ServerConfig t = threeRadios();
        ok(!applySdrChange(t, "replace", "00000001", "999", "airspy", "", err),
           "★ a different driver is refused — the settings are that driver's");
        ok(!applySdrChange(t, "replace", "00000001", "240513CA60", "rtlsdr", "", err),
           "★ a serial already in the config is refused");
        ok(!applySdrChange(t, "replace", "00000001", "-rf", "rtlsdr", "", err),
           "★ a new serial systemctl would read as an option is refused");
    }

    std::printf("\nADD adopts a radio the same way the TUI does\n");
    {
        ServerConfig s = threeRadios();
        ok(applySdrChange(s, "add", "00000009", "", "rtlsdr", "RTL-SDR Blog V4", err), "add succeeds", err);
        ok(s.radios.size() == 4 && s.radios[3].serial == "00000009" && s.radios[3].enabled && !s.radios[3].configured,
           "★ appended, enabled, NOT configured — on air once its tab is saved");
        ok(!applySdrChange(s, "add", "00000009", "", "rtlsdr", "", err), "adding it twice is refused");
        ok(s.radios[3].rate == 2'400'000, "an added RTL keeps the RTL rate (2.4 MS/s)");
        ServerConfig h;
        ok(applySdrChange(h, "add", "DD52B980BE4946DA", "", "airspyhf", "Airspy HF+", err), "add an HF+", err);
        ok(h.radios[0].rate == 912'000,
           "★ an added HF+ gets ITS OWN rate (912 kS/s), not the RTL default (Stuart's pocket box, 2026-10-10)");
    }

    std::printf("\nDISPLAY ORDER moves the cards and NOTHING ELSE\n");
    {
        ServerConfig s = threeRadios();
        int portsBefore[3]; for (size_t i = 0; i < 3; i++) portsBefore[i] = portForRadio(s, i);
        const int primBefore = primaryRadio(s);
        auto d0 = displayOrder(s);
        ok(d0.size() == 3 && d0[0] == 0 && d0[1] == 1 && d0[2] == 2, "★ no order set = array order");
        ok(setDisplayOrder(s, {"DD52B980BE4946DA", "00000001", "240513CA60"}, err), "the owner drags them", err);
        auto d = displayOrder(s);
        ok(d.size() == 3 && d[0] == 2 && d[1] == 0 && d[2] == 1, "★ shown in the dragged order");
        bool same = true; for (size_t i = 0; i < 3; i++) same = same && portForRadio(s, i) == portsBefore[i];
        ok(same, "★★★ every radio keeps its PORT — ports follow the array, never the display order");
        ok(primaryRadio(s) == primBefore, "★★★ the primary is unchanged");
        ok(s.radios[0].serial == "00000001", "★ the array itself was not reordered");
        ServerConfig back;
        ok(fromJson(toJson(s), back, err) && displayOrder(back)[0] == 2, "the order survives a save", err);
        // A page that predates `order` posts the radios without it — must not undo the owner's order.
        ServerConfig merged = back;
        ok(fromJson(R"({"radios":[{"serial":"DD52B980BE4946DA","driver":"airspyhf","label":"HF+ renamed","configured":true}]})",
                    merged, err), "an old page's save merges", err);
        ok(merged.radios[2].order == 0 && merged.radios[2].label == "HF+ renamed",
           "★★ an absent `order` keeps the owner's order", std::to_string(merged.radios[2].order));
        ok(!setDisplayOrder(s, {"00000001", "00000001"}, err), "a radio listed twice is refused");
        ok(!setDisplayOrder(s, {"nope"}, err), "an unknown radio is refused");
        // Partial list: the unlisted follow, in their current display order.
        ServerConfig p = threeRadios();
        ok(setDisplayOrder(p, {"240513CA60"}, err), "a partial order is accepted", err);
        auto dp = displayOrder(p);
        ok(dp[0] == 1 && dp[1] == 0 && dp[2] == 2, "★ the unlisted follow, nothing dropped");
        // Stability: equal keys keep array order.
        ServerConfig q = threeRadios(); q.radios[0].order = 5; q.radios[1].order = 5; q.radios[2].order = 1;
        auto dq = displayOrder(q);
        ok(dq[0] == 2 && dq[1] == 0 && dq[2] == 1, "★ the sort is stable for equal positions");
    }

    std::printf("\nvalidSerial matches vibeserver-radios' alphabet\n");
    {
        ok(validSerial("00000001") && validSerial("240513CA60") && validSerial("a1:b2_c3.d-4"), "real serials pass");
        ok(!validSerial("") && !validSerial("-x") && !validSerial(".x") && !validSerial("a/b")
           && !validSerial("a b") && !validSerial("a@b") && !validSerial(std::string(65, 'a')),
           "★ empty, leading -/., slash, space, @, over 64 are refused");
    }

    // ★★★ STUART'S HARD RULE: a radio LENT to another program, or held by anything else, is NEVER
    //     reported as unplugged. "Absent" needs the BUS as a witness (sdr_presence.h).
    std::printf("\nPresence: busy and lent are never 'absent'\n");
    {
        using vibe::Presence; using vibe::decidePresence; using vibe::DetectedRadio;
        auto det = [](const char* drv, const char* ser) { DetectedRadio d; d.driver = drv; d.serial = ser; return d; };
        // An RSP lent to OpenWebRX: the SDRplay API omits it, but the bus still counts it.
        ok(decidePresence("sdrplay", "240513CA60", {}, 1, 0) == Presence::Uncertain,
           "★★★ RSP released to OpenWebRX (API cannot see it, bus can) → NOT absent");
        ok(decidePresence("sdrplay", "240513CA60", {}, 0, 0) == Presence::Absent,
           "an RSP with nothing of its kind on the bus → absent");
        // A dongle another program holds, whose serial could not be read (open failed).
        ok(decidePresence("rtlsdr", "00000003", {det("rtlsdr", "")}, 1, 0) == Presence::Uncertain,
           "★★★ RTL held elsewhere, serial unreadable → NOT absent");
        // Two dongles on the bus, both named, neither ours: provably gone.
        ok(decidePresence("rtlsdr", "00000003", {det("rtlsdr", "00000001"), det("rtlsdr", "00000002")}, 2, 0)
           == Presence::Absent, "every RTL on the bus named, none ours → absent");
        ok(decidePresence("rtlsdr", "00000001", {det("rtlsdr", "00000001")}, 1, 0) == Presence::Attached,
           "named by the driver → attached");
        // A sibling radio process of ours streams the only RSP the API cannot see; ours is gone.
        ok(decidePresence("sdrplay", "AAA", {}, 1, 1) == Presence::Absent,
           "★ the only unseen RSP is our own sibling's → this one is absent");
        ok(decidePresence("airspyhf", "DD52", {}, -1, 0) == Presence::Uncertain,
           "★★ the bus walk failed → no witness, never absent");
        ok(decidePresence("hackrf", "", {}, 0, 0) == Presence::Uncertain, "no serial to look for → never absent");
        ok(decidePresence("airspy", "X", {det("airspyhf", "X")}, 0, 0) == Presence::Absent,
           "★ another DRIVER's device with the same serial is not this radio");
    }

    // ★★★ "IN USE BY ANOTHER APP" IS SAID ONLY WHEN KNOWN (Stuart, 2026-10-04).
    std::printf("\nIn use elsewhere: known, never guessed\n");
    {
        using vibe::knownInUseElsewhere;
        ok(knownInUseElsewhere(true, -1, -1, 0), "★ an RTL's own LIBUSB_ERROR_BUSY is proof");
        ok(!knownInUseElsewhere(false, 1, -1, 0), "★★ no claim information (macOS) → never said");
        ok(!knownInUseElsewhere(false, -1, 1, 0), "★★ no bus count → never said");
        ok(knownInUseElsewhere(false, 1, 1, 0), "one HF+ on the bus, claimed, none of ours running → known");
        ok(!knownInUseElsewhere(false, 1, 0, 0), "one on the bus, nobody has claimed it → not 'in use'");
        ok(!knownInUseElsewhere(false, 2, 1, 1), "★ our sibling holds the only claimed one → not this radio");
        ok(knownInUseElsewhere(false, 2, 2, 1), "two RSPs: our sibling holds one, another program the other → known");
        ok(!knownInUseElsewhere(false, 3, 2, 1),
           "★ an extra unclaimed RSP on the bus makes it ambiguous which is ours → never said");
    }

    // The sysfs reader, against a fake tree shaped like /sys/bus/usb/devices.
    std::printf("\nsysfs: who has claimed which radio\n");
    {
        char tmpl[] = "/tmp/vs-sysfs-XXXXXX";
        const char* rootC = mkdtemp(tmpl);
        ok(rootC != nullptr, "a scratch tree");
        if (rootC) {
            const std::string root = rootC;
            auto mk = [&](const std::string& rel) { ::mkdir((root + "/" + rel).c_str(), 0755); };
            auto put = [&](const std::string& rel, const std::string& v) {
                if (FILE* f = std::fopen((root + "/" + rel).c_str(), "w")) { std::fputs(v.c_str(), f); std::fclose(f); }
            };
            auto dev = [&](const std::string& d, const char* vid, const char* pid, const char* itfDriver) {
                mk(d); put(d + "/idVendor", vid); put(d + "/idProduct", pid);
                mk(d + ":1.0");
                if (itfDriver) ::symlink((std::string("../../../bus/usb/drivers/") + itfDriver).c_str(),
                                         (root + "/" + d + ":1.0/driver").c_str());
            };
            dev("1-1",   "03eb\n", "800c\n", "usbfs");       // an HF+ another program holds
            dev("1-2",   "1df7\n", "3050\n", nullptr);       // an RSP1B nobody holds
            dev("1-3",   "1df7\n", "3000\n", "usbfs");       // an RSP1A somebody holds
            dev("1-4",   "1d50\n", "6089\n", "usbfs");       // a HackRF somebody holds
            dev("1-5",   "1d50\n", "60a1\n", "uvcvideo");    // an Airspy bound to some other kernel driver
            dev("2-1",   "046d\n", "c52b\n", "usbfs");       // a mouse receiver: not a radio
            int sp = -1, ahf = -1, hrf = -1, asp = -1;
            ok(vibe::sysfsClaimedCounts(root, sp, ahf, hrf, asp), "the tree reads");
            ok(ahf == 1 && sp == 1 && hrf == 1 && asp == 0,
               "★ claimed through usbfs: HF+ 1, RSP 1 of 2, HackRF 1; a kernel driver is not usbfs",
               std::to_string(ahf) + " " + std::to_string(sp) + " " + std::to_string(hrf) + " " + std::to_string(asp));
            ok(!vibe::sysfsClaimedCounts(root + "/nope", sp, ahf, hrf, asp), "an unreadable root = unknown");
            std::string cmd = "rm -rf '" + root + "'";
            (void)!std::system(cmd.c_str());
        }
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
