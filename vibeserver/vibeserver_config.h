// vibeserver_config — the settings a VibeServer OWNS, as a file the server reads AND WRITES.
//
// ★★★ WHY THIS EXISTS. Configuration used to be `VIBESERVER_ARGS="--flags…"` in a systemd
// EnvironmentFile: a command line pretending to be a config file. That was fine while the only
// editors were a human and a curses screen, and it stops being fine the moment a BROWSER PAGE
// saves settings: nothing can round-trip a value it had to re-parse out of shell words, and a
// page that reads its own settings back needs a format that survives the trip.
//
// ★★ THE PRECEDENCE IS DELIBERATE AND UNCHANGED: command line > config.json > defaults.
// Someone who runs the binary by hand to test something must always win over the stored config,
// because that is what anyone would expect and because it is the escape hatch when a saved
// setting is what is wrong.
//
// ★ THE DAEMON OWNS THIS, NOT THE SHARED CORE. `local_sdr_shim.cpp` is compiled into the Android
// and iOS apps too, and a phone has no /etc and no business writing one. The shim exposes the
// config endpoints and calls back into handlers the daemon registers; on a phone they are simply
// never registered and the endpoints report that they are unavailable.
#pragma once
#include <string>

namespace vsconfig {

/** How the owner has chosen to run this receiver. Stored, not inferred.
 *  ★ `Locked` was previously an EMERGENT state — it happened whenever lockFreq > 0, and every
 *  gate in the shim keyed off that. Making it an explicit, named mode is what lets the setup page
 *  ask the question once and lets everything else read the answer. */
enum class Mode {
    SingleUser,   // one listener at a time; they get the full settings surface
    LockedRange,  // owner sets the window and the policy; listeners get a view inside it
};

/** ★★★ WHO IS THIS RECEIVER FOR? Deliberately SEPARATE from `Mode` above, which is about the
 *  RADIO's window (does the owner pin it and hand out views inside it). This is about the
 *  AUDIENCE, and therefore about how much management the owner needs on screen.
 *
 *  ★★ WHY IT IS ASKED RATHER THAN INFERRED. The obvious shortcut is `users > 1` — and it is
 *  wrong: a household sharing a receiver on the LAN is multi-user AND exactly the person who
 *  wants none of this. Inference about intent can be wrong; asking cannot (Stuart, 2026-08-07).
 *
 *  ★★ AND IT HIDES THE UI, NOT THE RECORDING. A Local server still keeps its connection log and
 *  still honours its ban list — it simply does not put them on screen. So switching to Public
 *  later is instant and arrives with history already there, instead of starting from nothing.
 *  Hiding a panel is a display decision; not collecting the data would be a different product. */
enum class Sharing {
    Local,    // yourself, your household, a few people on your network — the plug-and-play case
    Public,   // listed, reachable from the internet: you will need to manage strangers
};

/** The stored configuration. Mirrors the CLI surface, plus what only a browser page can express. */
struct Config {
    // ★★★ HAS THE OWNER FINISHED SETUP? Distinct from "is an admin password set" — the wizard
    // makes the password mandatory, so that alone can no longer mean "unconfigured". This is what
    // puts the browser on the setup page instead of the receiver.
    bool configured = false;
    Mode mode = Mode::SingleUser;
    /** ★ Defaults to Local: the mode that behaves exactly as VibeServer always has. A new
     *  setting must never change what an existing install does. */
    Sharing sharing = Sharing::Local;

    // Identity
    std::string name, place, country, locator, lat, lon;

    // Discovery. ★ mDNS stays OFF until setup completes, so an unconfigured server cannot be
    // discovered — which is what keeps the app from ever meeting one it has no flow for.
    bool        mdnsAdvertise = true;
    std::string mdnsName;          // friendly name; the .local label is DERIVED from it, once

    // Access
    std::string pin, adminPass;
    int         sessionLimitMin = 0;
    /** ★★ Minutes of no interaction after which an ADMIN session's controls re-lock. The
     *  session, its audio and any decoder keep running — only the ability to CHANGE anything
     *  goes away. 0 = never. Defends against a forgotten admin tab, not a guessed password. */
    /** ★★ SCHEDULED UPDATES. A receiver in a loft gets logged into roughly never, so without
     *  this it runs whatever it was installed with for ever — and that is the machine most
     *  exposed, because it is the one nobody looks at.
     *  ★ Off by default: installing software on someone's machine on a timer is not something to
     *    switch on for them. `updateHour` is local time; -1 = disabled.
     *  ★★ `updateAll` widens it from "just VibeServer" to "every package". Deliberately separate:
     *     upgrading one package we build is a very different risk from upgrading the OS
     *     unattended, and the owner should be choosing between them knowingly. */
    /** ★★ TWO INDEPENDENT SCHEDULES, not one with a scope switch. They deserve different
     *  cadences: a VibeServer update is one small package we build and can be daily without
     *  much thought, while a full system upgrade is bigger, occasionally wants attention, and
     *  suits a weekly slot. Forcing one schedule would make the owner choose which risk to
     *  accept for both (Stuart, 2026-08-07: "schedule both types of updates automatically").
     *  ★ Hour is local time; -1 = that schedule is off. Day 0=Sun..6=Sat; -1 = every day. */
    int         updateSrvHour = -1, updateSrvDay = -1;   // VibeServer only
    int         updateAllHour = -1, updateAllDay = -1;   // every package on the machine

    int         adminIdleMin = 30;

    // Radio / window
    double freq = 9'410'000, rate = 2'400'000, lockFreq = 0, lockRate = 0;
    int    gain = -1;
    // ★★ THE FRONT END AS THE OWNER LEFT IT. An admin who unlocked the controls and turned the RF
    //    gain down did it for a reason — 3955 kHz blasting the front end, in the case that prompted
    //    this — and losing that on the next restart means the receiver comes back overloaded and
    //    the owner has to notice and fix it again. -1 = never set, so leave the radio's default.
    //    ★ Deliberately NOT in the setup wizard: these are live adjustments made while listening,
    //      and the place they are made is the place they must stick.
    int    lnaState = -1;      // RSP LNA state (raw, as the API numbers it)
    int    ifGr = -1;          // RSP IF gain reduction, dB
    int    ifAgc = -1;         // RSP IF AGC: 1 on, 0 off, -1 not set
    std::string demodMode = "am";        // landing mode for a new listener
    double landingFreq = 0;              // 0 = same as freq

    // Listeners and ceilings
    int    users = 1;
    double maxBw = 0, maxFps = 0, fftRate = 15;
    int    uncompressed = 0;
    bool   forceIdleSaver = false;
    /** ★ Let another program (OpenWebRX, a decoder) open the SDR while nobody is listening.
     *  OFF by default: it costs the spectrogram and the band-conditions history, which almost
     *  every server would rather keep. See LocalSdrShim::releaseRadio. */
    bool   releaseWhenIdle = false;
    double idleGrace = 300;

    // Front end
    bool rfNotch = false, dabNotch = false, zoomSpectrum = false;

    /** ★★★ CPU GOVERNOR — a SETTING, because the wrong one costs 25% of the machine silently.
     *  Raspberry Pi OS defaults to `ondemand`, which decides how hard to run from how busy each
     *  core looks. That is exactly wrong for this workload: the DSP is spread across every core,
     *  so each one looks half-idle and the governor clocks the whole chip down — 2.4 GHz to
     *  1.9 GHz, measured, at 44°C with no thermal throttling. The work did not grow; every cycle
     *  just took 26% longer, and a job with a real-time deadline started missing it. Parallelising
     *  the load made it SLOWER.
     *  ★ "performance" by default: a receiver serving listeners is not an idle desktop, and the
     *    Pi's power difference is a couple of watts. An owner running on solar can choose
     *    otherwise, which is why it is a setting rather than a hard-coded write. */
    std::string cpuGovernor = "performance";

    int  port = 0;
    bool web = true;
};

/** Read `path`. Returns false and leaves `cfg` untouched if the file is absent or unreadable;
 *  a malformed file is reported in `err` and treated as absent — a receiver must not stay
 *  offline because one value is wrong. */
bool load(const std::string& path, Config& cfg, std::string& err);

/** Serialise to JSON. Stable key order, so a diff of two saved configs is readable. */
std::string toJson(const Config& cfg);

/** Parse JSON into `cfg`. Unknown keys are ignored (forward compatibility); absent keys keep
 *  whatever `cfg` already holds, so this doubles as a PATCH. */
/** @param validate run the whole-config coherence rules (multi-user needs a lock, a configured
 *  server needs an admin password, ...). TRUE for the setup page, which is submitting a complete
 *  configuration and must be told when it is contradictory.
 *  ★★ FALSE for a LIVE ONE-FIELD PATCH — saving the RF gain an admin just set must not be
 *     refused because some UNRELATED part of the running config would not pass the setup page's
 *     rules. It did exactly that: a server started from the command line is marked configured by
 *     the migration rule but has no admin password, so every gain save was rejected with a
 *     message about admin passwords. A patch cannot fix a field it does not mention. */
bool fromJson(const std::string& json, Config& cfg, std::string& err, bool validate = true);

/** Write atomically: temp file in the same directory, fsync, rename. ★ A half-written config is
 *  read at every boot, so the failure mode of a naive write is a receiver that will not start. */
bool save(const std::string& path, const Config& cfg, std::string& err);

/** The default location. Overridable for tests and for a non-root run. */
const char* defaultPath();

/** ★ The mDNS label derived from a friendly name — lowercased, ASCII, hyphenated, trimmed.
 *  DERIVED IN ONE PLACE ONLY. The setup page shows the user what their name becomes, and the
 *  responder claims it; if those two derive it separately they drift, and the address the owner
 *  was shown stops being the address that works. */
std::string mdnsLabel(const std::string& friendlyName);

} // namespace vsconfig
