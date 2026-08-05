// vibeserver_config — the settings a VibeServer OWNS, as a file the server reads AND WRITES.
//
// ★★★ WHY THIS EXISTS. Configuration used to be `VIBESERVER_ARGS="--flags…"` in a systemd
// EnvironmentFile: a command line pretending to be a config file. That was fine while the only
// editors were a human and a curses screen, and it stops being fine the moment a BROWSER PAGE
// saves settings — a checkbox list of blocked demodulators does not survive a flag string, and
// nothing can round-trip a value it had to re-parse out of shell words.
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
#include <vector>

namespace vsconfig {

/** How the owner has chosen to run this receiver. Stored, not inferred.
 *  ★ `Locked` was previously an EMERGENT state — it happened whenever lockFreq > 0, and every
 *  gate in the shim keyed off that. Making it an explicit, named mode is what lets the setup page
 *  ask the question once and lets everything else read the answer. */
enum class Mode {
    SingleUser,   // one listener at a time; they get the full settings surface
    LockedRange,  // owner sets the window and the policy; listeners get a view inside it
};

/** The stored configuration. Mirrors the CLI surface, plus what only a browser page can express. */
struct Config {
    // ★★★ HAS THE OWNER FINISHED SETUP? Distinct from "is an admin password set" — the wizard
    // makes the password mandatory, so that alone can no longer mean "unconfigured". This is what
    // puts the browser on the setup page instead of the receiver.
    bool configured = false;
    Mode mode = Mode::SingleUser;

    // Identity
    std::string name, place, country, locator, lat, lon;

    // Discovery. ★ mDNS stays OFF until setup completes, so an unconfigured server cannot be
    // discovered — which is what keeps the app from ever meeting one it has no flow for.
    bool        mdnsAdvertise = true;
    std::string mdnsName;          // friendly name; the .local label is DERIVED from it, once

    // Access
    std::string pin, adminPass;
    int         sessionLimitMin = 0;

    // Radio / window
    double freq = 9'410'000, rate = 2'400'000, lockFreq = 0, lockRate = 0;
    int    gain = -1;
    std::string demodMode = "am";        // landing mode for a new listener
    double landingFreq = 0;              // 0 = same as freq

    // Listeners and ceilings
    int    users = 1;
    double maxBw = 0, maxFps = 0, fftRate = 15;
    int    uncompressed = 0;
    bool   forceIdleSaver = false;
    double idleGrace = 300;

    // Front end
    bool rfNotch = false, dabNotch = false, zoomSpectrum = false;

    /** ★★ Demodulators and decoders the OWNER has switched OFF. Empty = everything allowed.
     *  Stored as what is BLOCKED rather than what is allowed, so a build that adds a new
     *  demodulator does not silently disable it on every existing server. */
    std::vector<std::string> blocked;

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
bool fromJson(const std::string& json, Config& cfg, std::string& err);

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
