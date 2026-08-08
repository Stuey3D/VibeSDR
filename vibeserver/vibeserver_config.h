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
    std::string trustedProxies;   ///< see ServerConfig::trustedProxies
    std::string allowRanges, blockRanges;   ///< see RadioConfig::allowRanges
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
    /** See RadioConfig::biasT — asserted at every start so it is never inherited. */
    bool biasT = false;
    int  ppm = 0, ppb = 0, directSampling = -1;   // see RadioConfig

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

/** ★★★ ONE RADIO, INSIDE A SERVER THAT MAY HAVE SEVERAL.
 *
 *  Deliberately holds ONLY what differs per radio. Everything the machine shares — the admin
 *  password, the PIN, where the receiver is, the update schedule — stays in ServerConfig and is
 *  stated once, so it cannot drift between radios.
 *
 *  ★★ IDENTITY IS NOT AN INDEX. Enumeration order changes when a dongle is plugged into a
 *     different socket or a new one is added, and settings that follow the order rather than the
 *     hardware then apply to the WRONG radio — including a locked frequency range, which is the
 *     one that puts a receiver somewhere its owner never agreed to.
 *  ★★ AND THE SOURCE DIFFERS BY DRIVER, measured on the Pi with all three attached: an RTL dongle
 *     and an Airspy HF+ carry USB serials, but an **SDRplay RSP presents no USB serial at all** —
 *     its serial comes from the SDRplay API. So identity is asked of the DRIVER, never of the USB
 *     bus as though that were the single source of truth.
 *  ★ RTL serials are not unique either (stock dongles are all "00000001"), which is what `usbPath`
 *    is for and why `vibeserver --set-rtl-serial` exists. */
struct RadioConfig {
    std::string serial;      // as the DRIVER reports it — empty until the radio is first seen
    std::string driver;      // "rtlsdr" | "sdrplay" | "airspyhf"
    std::string usbPath;     // physical socket, e.g. "1-2" — the tie-break when serials collide
    std::string label;       // what the owner calls it; shown to listeners

    /** ★★ TWO GATES, AND THEY MEAN DIFFERENT THINGS. `enabled` is the owner saying "serve this
     *  radio" (the TUI toggle); `configured` is "I have said what it should do" (its setup tab was
     *  saved). Both must be true to serve. Collapsing them would either put a half-set-up receiver
     *  on air at whatever the defaults happen to be, or lose an owner's settings when they
     *  temporarily take a radio out of service. `enabled` wins: un-ticking a fully configured radio
     *  takes it off the air and KEEPS its settings. */
    bool enabled = true;
    bool configured = false;

    int  port = 0;           // 0 = assigned automatically; bound to loopback behind the front door

    // Everything below mirrors the same field in Config, for this radio alone.
    Mode   mode = Mode::SingleUser;
    double freq = 9'410'000, rate = 2'400'000, lockFreq = 0, lockRate = 0;
    int    gain = -1, lnaState = -1, ifGr = -1, ifAgc = -1;
    std::string demodMode = "am";
    double landingFreq = 0;
    int    users = 1;
    double maxBw = 0, maxFps = 0, fftRate = 15;
    int    uncompressed = 0;
    bool   forceIdleSaver = false, releaseWhenIdle = false;
    double idleGrace = 300;
    /** ★★★ WHERE LISTENERS MAY TUNE THIS RADIO. Comma separated, each entry either a named band
     *  ("fm", "air", "mw") or a range in any unit ("87.5MHz-108MHz"). See vibe_bands.h.
     *  ★★ An EMPTY allow list means "everything the hardware can reach", never "nothing" — an
     *  owner who only wanted to block the airband must not take their receiver off the air.
     *  ★ Only meaningful in single-user mode: a locked range IS the constraint in shared mode, so
     *  the setup page does not offer these there (Stuart, 2026-08-08: "blocked bands wont be
     *  needed in shared mode as the locked nature of it is the block/allow"). */
    std::string allowRanges, blockRanges;
    bool   rfNotch = false, dabNotch = false, zoomSpectrum = false;
    /** ★★★ DC ON THE FEEDLINE, REMEMBERED AND ASSERTED. A dongle's bias-T survives whatever set
     *  it — it is a GPIO that stays put for as long as the device has power — so a receiver that
     *  never states a preference inherits one. That is how a V4 came up with its red light on and
     *  4.5 V going out to the antenna, from a Windows tool used hours earlier on another machine
     *  (2026-08-08). Default false, and applied at every start so the answer is never "whatever
     *  the last program left". Same principle as starting at minimum gain. */
    bool   biasT = false;
    /** ★★ THE REST OF THE PROTECTED SET, per radio. These are the controls the admin password
     *  exists to guard — the ones that can leave a receiver broken for the next person — so with
     *  several radios they must be settable per radio and not only on whichever one happens to be
     *  running. -1 / 0 mean "not set": leave the radio's own default alone.
     *  ★ Which of them a radio HAS differs by driver, and offering one that can never apply is
     *    the "control that only works on one radio" fault. The page draws them per driver. */
    int    ppm = 0;             // RTL frequency correction, parts per million
    int    ppb = 0;             // Airspy HF+ calibration, parts per billion
    int    directSampling = -1; // RTL: 0 off, 1 I, 2 Q; -1 = leave alone (not needed on a V4)

    /** ★★★ DOES THE LANDING PAGE'S SPECTROGRAM — AND THE BAND CONDITIONS — COME FROM THIS RADIO?
     *
     *  ★ ONE SETTING, because they are one measurement. Both are taken from the same wide FFT: the
     *    spectrogram is what that window looked like over hours, and the measured band conditions
     *    are what it can hear right now. A radio that cannot honestly draw one cannot honestly
     *    report the other, so they are never chosen separately.
     *
     *  ★★ ONLY A FIXED-RANGE RADIO CAN ANSWER YES, and that is not a technicality. The
     *     spectrogram is a picture of one window over hours; a radio a listener can retune at will
     *     would contribute a smear with a large hole in it every time somebody used it. Stuart,
     *     2026-08-08: "this only applies to radios with a fixed frequency range not radios that
     *     are single user full radio control mode as those will have large gaps when people are
     *     using them."
     *  ★ And a radio that RELEASES when idle cannot draw one either — it spends its idle time
     *    letting go of the device, which is exactly when the picture would be drawn. If every
     *    radio releases, there is simply no spectrogram, and the page says so rather than
     *    showing an hour of blank. */
    bool   spectrogram = false;

    /** ★★★ PER RADIO, AND IT MATTERS MOST ON THE ONE-LISTENER RADIOS. This used to be a single
     *  machine-wide setting that the page only sent for a LOCKED receiver, so a single-user radio
     *  was always unlimited — which is exactly backwards: a shared receiver has room for everyone,
     *  a one-at-a-time radio is the one somebody can sit on all evening while others wait
     *  (Stuart, 2026-08-08: "no time limits for the single user radios").
     *  ★ 0 = unlimited. Loopback and admin sessions are exempt wherever this is enforced. */
    int    sessionLimitMin = 0;
};

/** The whole machine: what every radio shares, plus the radios themselves. */
struct ServerConfig {
    bool configured = false;
    /** ★★★ SIMPLE or FULL — see needsFrontDoor(). Simple is the original plug-in-and-share
     *  receiver; Full is the shared-server product with a landing page and a front door.
     *  ★ Defaults to FALSE so a config that predates the switch stays Simple unless it plainly is
     *    not — the migration below decides that from what the server was ALREADY doing, because a
     *    setting that did not exist cannot have been chosen. */
    bool fullMode = false;
    Sharing sharing = Sharing::Local;
    std::string place, country, locator, lat, lon;   // ★ the SITE — one machine, one location
    std::string name;                                // the machine's name, shown above the list
    bool        mdnsAdvertise = true;
    std::string mdnsName;
    std::string pin, adminPass;
    int         sessionLimitMin = 0;
    int         updateSrvHour = -1, updateSrvDay = -1;
    int         updateAllHour = -1, updateAllDay = -1;
    int         adminIdleMin = 30;
    std::string cpuGovernor = "performance";
    /** ★★★ ONE CHOICE FOR THE MACHINE. Opus or uncompressed is about what this server's UPLINK can
     *  carry, and the uplink is shared by every radio on it — asking per radio invited three
     *  answers to a question that has one, and three ways to get it wrong (Stuart, 2026-08-08:
     *  "only one selection Opus applies to all radios doesnt need to be every radio").
     *  ★ RadioConfig::uncompressed is kept only so an older file still loads; the machine value is
     *    what is applied. */
    int         uncompressed = 0;
    /** ★★★ THE SPECTRUM SLOWDOWN WHEN NOBODY IS LOOKING, and it belongs to the MACHINE: it is
     *  about the CPU and the uplink this server has, both of which are shared by every radio on
     *  it. Not to be confused with RadioConfig::releaseWhenIdle, which hands a DEVICE to another
     *  program and is per radio by nature (Stuart, 2026-08-08: "This is the spectrum slowdown and
     *  not the radio releasing when not in use, that one needs to stay per radio"). */
    bool        forceIdleSaver = false;
    /** ★★★ Reverse proxies whose X-Forwarded-For we believe — comma separated, addresses or
     *  CIDRs ("127.0.0.1, 10.0.0.0/8"). EMPTY BY DEFAULT and empty means "read no headers":
     *  the header is client-supplied text, so trusting it from anyone would let a stranger forge
     *  any address and walk through the ban list. Behind a proxy WITHOUT this, every listener
     *  shares the proxy's address — one ban hits them all. See vibe_proxy.h. */
    std::string trustedProxies;
    int         port = 0;      // the ONE port that leaves the machine
    bool        web = true;
    std::vector<RadioConfig> radios;
};

/** ★★★ READS BOTH FORMATS. A file with no `radios` array is today's single-radio config, and is
 *  migrated into a one-entry list — enabled AND configured, because it is a receiver that is
 *  already working and must not be taken off the air by gaining a gate it never had. */
bool loadServer(const std::string& path, ServerConfig& cfg, std::string& err);
bool saveServer(const std::string& path, const ServerConfig& cfg, std::string& err);
std::string toJson(const ServerConfig& cfg);
bool fromJson(const std::string& json, ServerConfig& cfg, std::string& err);

/** ★★★ DOES THIS MACHINE NEED A FRONT DOOR? It is the SIMPLE/FULL switch that decides — NOT how
 *  many radios are attached, which is what this first tried and got wrong.
 *
 *  ★★ THE TWO MODES ARE DIFFERENT PRODUCTS, and Stuart put it best (2026-08-08):
 *
 *      SIMPLE is "more akin to Spy Server / rtl_tcp — designed for speed and ease of use,
 *      primarily for a user to quickly hook up a radio to a device and share it on the local
 *      network. Like it already did." Plug in, press start. No front door, no extra process, the
 *      receiver on the port it has always been on. This is what people love and it must not move.
 *
 *      FULL is "more akin to UberSDR / OWRX — a full server interface". Locked ranges, many
 *      listeners, a landing page, admin. That is worth a front door EVEN WITH ONE RADIO, which is
 *      exactly what the demo Pi has been running all along: one RSP, 2.8-10.8 MHz locked, 30
 *      listeners.
 *
 *  ★ So the radio count decides nothing. A Full single-radio server gets the front door; a Simple
 *    three-radio server does not.
 */
bool needsFrontDoor(const ServerConfig& cfg);

/** ★ May this radio drive the landing page's spectrogram? Fixed window, not released when idle,
 *  and actually being served. See RadioConfig::spectrogram for why each of those matters. */
bool canDrawSpectrogram(const RadioConfig& r);

/** Which radio draws it, or -1 when none can — which is a real answer, not a failure. */
int spectrogramRadio(const ServerConfig& cfg);

/** ★★★ WHICH RADIO ANSWERS ON THE MACHINE'S MAIN PORT — only meaningful in SIMPLE mode.
 *  In FULL mode the front door has that port and no radio is special, so this returns -1's
 *  meaning by construction: nothing to single out.
 *  ★ Kept because the GUI builds (macOS, Android) still offer Simple. The first one that is both enabled and
 *  configured. Returns -1 when none is ready — a real state, and not an error: the machine still
 *  runs and serves the setup page, which is where the owner goes to make one ready.
 *  ★ For every existing single-radio install this is radio 0, so nothing moves. */
int primaryRadio(const ServerConfig& cfg);

/** The port a given radio listens on. ★ DETERMINISTIC, from the radios[] ORDER — not from
 *  detection order, and not assigned and written back. Detection order changes when a radio is
 *  busy or unplugged; a stored port that is recomputed differently each boot would move receivers
 *  between addresses under their listeners. The primary always takes the machine's own port so a
 *  single-radio server keeps answering exactly where it always has. */
int portForRadio(const ServerConfig& cfg, size_t index);

/** Flatten the shared settings and one radio's settings into the Config a single VibeServer
 *  process consumes. ★ This is what makes process-per-radio cheap: the existing single-radio code
 *  never learns that it has siblings. */
Config effectiveFor(const ServerConfig& srv, const RadioConfig& radio);

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
