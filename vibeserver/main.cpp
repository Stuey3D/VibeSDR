// VibeServer — standalone daemon (macOS today, Linux/Raspberry Pi next).
//
// WHY THIS EXISTS. The server is not a phone feature that happens to run on a Mac; it is a C++
// core (`vibedsp` + `local_sdr_shim` + `net_shim`) that Android merely wraps in JNI. Building it
// as a plain executable gives us:
//
//   1. A DEV LOOP MEASURED IN SECONDS. Every server-side change previously cost an APK build, an
//      install, and a device dance. Here it is `cmake --build` and run. That is the whole reason
//      link management gets built here FIRST and back-ported to Android after — see
//      BRIEF-vibeserver-macos.md, which already calls the Mac app "the shim's native test harness".
//   2. The headless Pi daemon, nearly for free — same core, same config, systemd instead of a GUI.
//
// NO USB, DELIBERATELY. The IQ comes from an rtl_tcp server (`--tcp host:port`), which is the
// exact configuration iOS already ships: `rtl_sdr_stub.h` stands in for librtlsdr, so the USB path
// compiles but is never entered. That defers libusb/librtlsdr vendoring (BRIEF §2) without blocking
// any of the protocol or link-management work, which is what we actually want to iterate on.
//
// This is the CLI harness only. The menu-bar app of BRIEF-vibeserver-macos.md is a GUI wrapped
// around this same core later; nothing here should grow Mac-specific behaviour.

#include "local_sdr_shim.h"
#include <cctype>
#include <fstream>
#include <unistd.h>
#include "airspyhf_source.h"
#include "sdrplay_source.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "vibeserver_config.h"
#include "eibi.h"
#include "solar.h"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

struct Opts {
    int         device  = 0;             // USB device index; <0 = use rtl_tcp instead
    bool        useUsb  = true;          // default: drive the dongle directly
    bool        deviceGiven = false;     // ★ an explicit --device N names an RTL index and wins
    // ★★★ THE ADMIN / OPERATOR SETTINGS. Without these a Pi CANNOT SAFELY BE MADE PUBLIC — Stuart,
    // 2026-07-31. The admin password is not a convenience: it is what stands between a stranger and
    // BIAS-T (DC on the feedline), DIRECT SAMPLING (reconfigures the front end) and CALIBRATION
    // (miscalibrates the radio invisibly and permanently). The Mac has had all of this since v10;
    // the headless build shipped without any of it, which made it the LEAST safe place to host.
    std::string adminPass;
    int         sessionLimitMin = 0;     // per-listener minutes; 0 = unlimited
    bool        forceIdleSaver  = false; // listeners may not switch idle power-saving off
    int         uncompressed    = 0;     // 0 = off, 1 = listener's choice, 2 = compatibility only
    // Receiver identity — published to every listener, and the reason a directory entry is useful.
    std::string rxName, rxPlace, rxIso, rxGrid, rxLat, rxLon;
    std::string tcpHost = "127.0.0.1";   // rtl_tcp source (when --tcp is given)
    int         tcpPort = 1234;
    double      freq    = 9'410'000;     // Hz
    double      rate    = 2'400'000;     // capture sample rate, Hz
    int         gain    = -1;            // tenths of dB; <0 = auto
    int         fftSize = 4096;
    double      fftRate = 15;            // spectrum frames/sec
    std::string mode    = "am";
    std::string pin;                     // empty = open access
    double      maxBw   = 0;             // 0 = no cap  ─┐ link-management ceilings
    double      maxFps  = 0;             // 0 = default ─┘
    double      lockRate = 0;            // 0 = client-controlled
    // ★★★ THE SHARED-RADIO SETTINGS. The model: the OWNER sets the radio up and locks it, and a
    // listener gets a view and a VFO inside it — no hardware control at all (Stuart, 2026-08-02).
    double      lockFreq = 0;            // 0 = the centre follows the VFO, as it always has
    int         users    = 1;            // expected listeners; 1 = personal receiver
    std::string channels;                // "" = derive from `users`; else "direct" | "shared"
    // ★★★ OFF BY DEFAULT. It gives real bins at deep zoom, but the WIDE path is a tuned, known-good
    // waterfall and the zoom path SUPPRESSES it — so switching it on replaces something already
    // good with something still being tuned (Stuart, 2026-08-02: "the web client's view was
    // already perfect before this"). Sharper bins are not worth a worse picture. Opt in with
    // --zoom-spectrum until it can beat the wide path on LOOK as well as resolution.
    bool        zoomSpectrum = false;
    // ★ RSP front-end notches. OFF unless the operator asks: the RF notch covers broadcast FM.
    double      idleGrace = 300.0;   // seconds before an unattended radio idle-parks
    bool        rfNotch  = false;
    bool        dabNotch = false;
    int         port    = 0;             // 0 = auto (48000-48049)
    bool        web     = true;
};

/** Read VIBESERVER_ARGS="…" out of a config file and splice it into the argument list.
 *  ★ Honours single and double quotes so a value may contain spaces — the whole reason this
 *  exists. Comments (#) and blank lines are ignored; anything that is not VIBESERVER_ARGS is
 *  ignored too, so the file can carry notes without confusing us. */
static bool loadConfigFile(const std::string& path, std::vector<std::string>& out) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "VibeServer: cannot read %s\n", path.c_str()); return false; }
    std::string line, raw;
    while (std::getline(f, line)) {
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == '#') continue;
        if (line.compare(p, 16, "VIBESERVER_ARGS=") != 0) continue;
        raw = line.substr(p + 16);
        break;
    }
    // ★★★ STRIP THE ASSIGNMENT'S OWN QUOTES FIRST. The file holds VIBESERVER_ARGS="…" — those outer
    // quotes belong to the shell-style assignment, not to any argument. Treating them as grouping
    // swallowed the ENTIRE line into a single token, so the first attempt at this fix failed in a
    // new way while looking like the old one. Strip them, THEN split, so inner quotes can do the
    // job they are actually for: holding a value that contains a space.
    {
        size_t b = raw.find_first_not_of(" \t\r\n");
        size_t e = raw.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) raw = raw.substr(b, e - b + 1); else raw.clear();
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') && raw.back() == raw.front())
            raw = raw.substr(1, raw.size() - 2);
    }
    // shell-ish split: inner quotes group, everything else splits on whitespace
    std::string cur; char quote = 0; bool any = false;
    for (char c : raw) {
        if (quote) { if (c == quote) quote = 0; else cur += c; continue; }
        if (c == '"' || c == '\'') { quote = c; any = true; continue; }
        if (c == ' ' || c == '\t') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
        cur += c;
    }
    (void)any;
    if (!cur.empty()) out.push_back(cur);
    return true;
}

void usage() {
    std::printf(
        "VibeServer (standalone)\n\n"
        "  --device N        RTL-SDR index to open directly (default 0)\n"
        "  --tcp HOST:PORT   use an rtl_tcp source instead of USB\n"
        "  --freq HZ         initial centre frequency     (default 9410000)\n"
        "  --rate HZ         capture sample rate          (default 2400000)\n"
        "  --gain TENTHS_DB  tuner gain, <0 = auto        (default auto)\n"
        "  --mode MODE       am|lsb|usb|nfm|wfm|cw        (default am)\n"
        "  --fft N           FFT size                     (default 4096)\n"
        "  --fps N           spectrum frames/sec          (default 15)\n"
        "  --port N          listen on this port          (default: first free 48000-48049)\n"
        "  --pin SECRET      require a PIN from NETWORK clients (this machine never needs it)\n"
        "  --max-bw HZ       server-enforced bandwidth ceiling\n"
        "  --max-fps N       server-enforced spectrum-rate ceiling\n"
        "  --lock-rate HZ    pin the capture rate (clients cannot change it)\n"
        "  --lock-freq HZ    pin the CENTRE frequency. Listeners tune freely inside the\n"
        "                    captured window but cannot move the radio for everybody.\n"
        "                    Required by --channels shared.\n"
        "  --users N         how many listeners this receiver is for (default 1). This\n"
        "                    picks the channel method: 1 = direct, 2+ = shared.\n"
        "  --channels MODE   override that choice: direct | shared\n"
        "  --zoom-spectrum   EXPERIMENTAL: real bins at deep zoom instead of interpolation.\n"
        "  --idle-grace SEC  wait SEC after the last listener leaves before idling the radio\n"
        "                    (default 300). Stops a page reload driving a park/wake cycle,\n"
                    "                    which is what wedges an RSP. 0 = idle immediately.\n"
        "  --rf-notch        SDRplay only: broadcast NOTCH (covers MW AND FM). Use on HF or\n"
        "                    airband where a local FM transmitter overloads the front end.\n"
                    "                    NEVER use it to receive the FM band — it removes it.\n"
        "  --dab-notch       SDRplay only: DAB band notch, same reasoning as --rf-notch.\n"
        "                    Off by default — it replaces the tuned wide-path waterfall.\n"
        "                      direct  cheapest for ONE listener; cost rises with each\n"
        "                              extra one and runs out of a core at about eight.\n"
        "                      shared  one FFT serves everybody, so the ninth listener\n"
        "                              costs about as much as the first. Higher fixed\n"
        "                              cost, so it pays from about three listeners up.\n"
        "                    Both give the same detail at high zoom.\n"
        "  --no-web          do not serve the browser client at GET /\n"
        "  -h, --help\n\n"
        "By default the dongle is driven DIRECTLY over libusb — nothing else to install or run.\n"
        "--tcp is for development against a remote or synthetic source.\n"
        "\nReceiver identity (published to listeners)\n"
        "  --name TEXT       receiver name shown over the spectrum\n"
        "  --place TEXT      town or area shown beneath it\n"
        "  --country XX      two-letter code; sets the flag and the ITU band plan\n"
        "  --locator GRID    Maidenhead square (deliberately coarse ~4 km)\n"
        "  --lat N --lon N   exact coordinates; these win over a locator\n"
        "\nAccess and operator limits\n"
        "  --pin SECRET          who may CONNECT at all\n"
        "  --admin-pass SECRET   who may change settings that can DAMAGE the radio\n"
        "                        (bias-T, direct sampling, calibration). Set this on any\n"
        "                        receiver reachable from the internet.\n"
        "  --session-limit MIN   per-listener time limit; 0 = unlimited\n"
        "  --force-idle-saver    listeners may not switch idle power-saving off\n"
        "  --uncompressed MODE   off | choice | compat  (raw audio is ~20x the bytes)\n");
}

bool parse(int argc, char** argv, Opts& o) {
    // ★★ TWO PASSES, so --config can contribute arguments. The first pass flattens argv and splices
    // in anything a config file supplies; the second parses the result. File options come FIRST,
    // so an explicit command-line flag always overrides the file — which is what anyone would
    // expect when they run the binary by hand to test something.
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config") {
            if (i + 1 >= argc) { std::fprintf(stderr, "--config needs a path\n"); std::exit(2); }
            if (!loadConfigFile(argv[++i], args)) return false;
        } else args.push_back(a);
    }
    const int n = (int)args.size();
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= n) { std::fprintf(stderr, "%s needs a value\n", args[i].c_str()); std::exit(2); }
        return args[++i].c_str();
    };
    for (int i = 0; i < n; i++) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help") { usage(); return false; }
        else if (a == "--device") { o.device = std::atoi(need(i)); o.useUsb = true; o.deviceGiven = true; }
        else if (a == "--tcp") {
            o.useUsb = false;
            std::string v = need(i);
            auto c = v.rfind(':');
            if (c == std::string::npos) { std::fprintf(stderr, "--tcp wants HOST:PORT\n"); std::exit(2); }
            o.tcpHost = v.substr(0, c);
            o.tcpPort = std::atoi(v.c_str() + c + 1);
        }
        else if (a == "--freq")      o.freq     = std::atof(need(i));
        else if (a == "--rate")      o.rate     = std::atof(need(i));
        else if (a == "--gain")      o.gain     = std::atoi(need(i));
        else if (a == "--mode")      o.mode     = need(i);
        else if (a == "--fft")       o.fftSize  = std::atoi(need(i));
        else if (a == "--fps")       o.fftRate  = std::atof(need(i));
        else if (a == "--port")      o.port     = std::atoi(need(i));
        else if (a == "--pin")       o.pin      = need(i);
        else if (a == "--max-bw")    o.maxBw    = std::atof(need(i));
        else if (a == "--max-fps")   o.maxFps   = std::atof(need(i));
        else if (a == "--lock-rate") o.lockRate = std::atof(need(i));
        else if (a == "--lock-freq") o.lockFreq = std::atof(need(i));
        else if (a == "--users")     o.users    = std::atoi(need(i));
        else if (a == "--channels")  o.channels = need(i);
        else if (a == "--no-zoom-spectrum") o.zoomSpectrum = false;
        else if (a == "--zoom-spectrum")    o.zoomSpectrum = true;
        else if (a == "--idle-grace") o.idleGrace = std::atof(need(i));
        else if (a == "--rf-notch")   o.rfNotch  = true;
        else if (a == "--dab-notch")  o.dabNotch = true;
        else if (a == "--no-web")    o.web      = false;
        else if (a == "--admin-pass")     o.adminPass       = need(i);
        else if (a == "--session-limit")  o.sessionLimitMin = std::atoi(need(i));
        else if (a == "--force-idle-saver") o.forceIdleSaver = true;
        else if (a == "--uncompressed")   { std::string v = need(i);
            o.uncompressed = (v == "choice") ? 1 : (v == "compat") ? 2 : 0; }
        else if (a == "--name")     o.rxName  = need(i);
        else if (a == "--place")    o.rxPlace = need(i);
        else if (a == "--country")  o.rxIso   = need(i);
        else if (a == "--locator")  o.rxGrid  = need(i);
        else if (a == "--lat")      o.rxLat   = need(i);
        else if (a == "--lon")      o.rxLon   = need(i);
        // ★★★ AN UNKNOWN ARGUMENT MUST NOT KEEP A RECEIVER OFFLINE. This printed the usage and
        // exited 2, which for a SERVICE means systemd restarts it, it exits 2 again, and the radio
        // stays down until somebody SSHs in — from one stray token in a config file
        // (Stuart, 2026-08-01: the service would not come back after a save).
        // ★★ For a box in a loft, resilience beats strictness: say so loudly enough that it cannot
        // be missed in the journal, ignore the token, and serve. One ignored setting is a far
        // better outcome than no receiver.
        // ★ Typed at a terminal it is still obvious — the warning is the first thing on screen, and
        // `vibeserver --help` is one command away.
        else std::fprintf(stderr, "VibeServer: ignoring unknown option \"%s\" "
                                  "(run `vibeserver --help` for the list)\n", a.c_str());
    }
    return true;
}

/** ★ The address to publish in the mDNS A record: this machine's first non-loopback IPv4.
 *  Deliberately the same thing `hostname -I | awk '{print $1}'` gives, because that is the address
 *  the TUI prints and the two must agree — an owner told one address and advertised another has
 *  no way to work out which is real. */
std::string primaryIpv4() {
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return "";
    std::string out;
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sin = (struct sockaddr_in*)p->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) { out = buf; break; }
    }
    freeifaddrs(ifa);
    return out;
}

/** Seed `o` from a stored config. ★ Called BEFORE the command-line pass, which is the whole
 *  precedence rule in one line: config.json supplies the defaults, the command line overrides
 *  them. Someone running the binary by hand to test something must always win over the file. */
void applyConfig(const vsconfig::Config& c, Opts& o) {
    o.rxName = c.name; o.rxPlace = c.place; o.rxIso = c.country;
    o.rxGrid = c.locator; o.rxLat = c.lat; o.rxLon = c.lon;
    o.pin = c.pin; o.adminPass = c.adminPass;
    o.sessionLimitMin = c.sessionLimitMin;
    o.freq = c.landingFreq > 0 ? c.landingFreq : c.freq;
    o.rate = c.rate;
    o.lockFreq = c.lockFreq; o.lockRate = c.lockRate;
    o.gain = c.gain;
    o.mode = c.demodMode;
    o.users = c.users;
    o.maxBw = c.maxBw; o.maxFps = c.maxFps; o.fftRate = c.fftRate;
    o.uncompressed = c.uncompressed;
    o.forceIdleSaver = c.forceIdleSaver;
    o.idleGrace = c.idleGrace;
    o.rfNotch = c.rfNotch; o.dabNotch = c.dabNotch; o.zoomSpectrum = c.zoomSpectrum;
    o.port = c.port; o.web = c.web;
}

/** The reverse, so a running server can hand the setup page what it is ACTUALLY doing rather than
 *  what the file last said — the two differ the moment anyone passes a flag. */
void configFromOpts(const Opts& o, vsconfig::Config& c) {
    c.name = o.rxName; c.place = o.rxPlace; c.country = o.rxIso;
    c.locator = o.rxGrid; c.lat = o.rxLat; c.lon = o.rxLon;
    c.pin = o.pin; c.adminPass = o.adminPass;
    c.sessionLimitMin = o.sessionLimitMin;
    c.freq = o.freq; c.rate = o.rate;
    c.lockFreq = o.lockFreq; c.lockRate = o.lockRate;
    c.gain = o.gain; c.demodMode = o.mode;
    c.users = o.users;
    c.maxBw = o.maxBw; c.maxFps = o.maxFps; c.fftRate = o.fftRate;
    c.uncompressed = o.uncompressed;
    c.forceIdleSaver = o.forceIdleSaver;
    c.idleGrace = o.idleGrace;
    c.rfNotch = o.rfNotch; c.dabNotch = o.dabNotch; c.zoomSpectrum = o.zoomSpectrum;
    c.port = o.port; c.web = o.web;
    c.mode = o.lockFreq > 0 ? vsconfig::Mode::LockedRange : vsconfig::Mode::SingleUser;
}

}  // namespace

namespace { std::string g_configPath; vsconfig::Config g_runtimeConfig;
            std::atomic<bool> g_restartRequested{false}; }

int main(int argc, char** argv) {
    // ★★★ NO ARGUMENTS AND A TERMINAL ⇒ A HUMAN TYPED `vibeserver`, so show the settings screen.
    // That is the one thing a command-line-shy person will try, and answering it with a wall of
    // flags tells them they are in the wrong place (Stuart, 2026-07-31).
    // ★★ SAFE FOR THE SERVICE: systemd runs us with NO TTY, so an empty VIBESERVER_ARGS still
    // starts the daemon. The TTY test is what separates the two, not the argument count alone.
#ifdef VIBE_HAVE_TUI
    if (argc == 1 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        extern int vibeserverTui();
        return vibeserverTui();
    }
#endif
    // ── STORED CONFIG FIRST, COMMAND LINE SECOND ────────────────────────────────────────────
    // ★★ The file supplies defaults; every flag overrides one. See vibeserver_config.h for why
    //    the storage moved from a flag string to JSON: a browser page has to round-trip these.
    // ★ A missing file is the NORMAL state of a fresh install — it means "not set up yet", not an
    //   error. A malformed one is reported and IGNORED: a receiver must never stay offline
    //   because one value is wrong.
    Opts o;
    vsconfig::Config cfg;
    bool hadConfigFile = false;
    {
        const char* p = getenv("VIBESERVER_CONFIG");
        g_configPath = p && *p ? p : vsconfig::defaultPath();
        std::string err;
        if (vsconfig::load(g_configPath, cfg, err)) {
            applyConfig(cfg, o);
            hadConfigFile = true;
        } else if (!err.empty()) {
            std::fprintf(stderr, "VibeServer: ignoring %s — %s\n", g_configPath.c_str(), err.c_str());
        }
    }
    if (!parse(argc, argv, o)) return 0;
    // What the server is ACTUALLY running, flags included — this is what the setup page reads.
    g_runtimeConfig = cfg;
    configFromOpts(o, g_runtimeConfig);

    // ★★★ AN EXISTING INSTALL MUST NOT BE DEMOTED TO "NOT SET UP" BY AN UPGRADE.
    // Every VibeServer deployed before config.json existed is configured through VIBESERVER_ARGS
    // and has no config file — so on first run of a new build, `configured` would be false and
    // GET / would answer the SETUP PAGE instead of the receiver. That is not a cosmetic
    // regression: a working public receiver would start asking every visitor for an admin
    // password. It happened to the live Pi demo the moment this shipped (2026-08-05).
    // ★★ So: no config file, but the command line supplied REAL settings ⇒ this is an existing
    //    deployment and it is already configured. A genuinely fresh install has an EMPTY
    //    VIBESERVER_ARGS (that is what the package ships), so it still gets the setup page.
    // ★ Deliberately generous about what counts. Being wrong in this direction leaves an owner
    //   configuring by hand as they always have; being wrong the other way takes a working
    //   receiver off the air.
    if (!hadConfigFile) {
        g_runtimeConfig.configured =
            !o.adminPass.empty() || !o.pin.empty() || !o.rxName.empty() ||
            o.lockFreq > 0 || o.users > 1 || o.sessionLimitMin > 0 || o.lockRate > 0;
        if (g_runtimeConfig.configured)
            std::fprintf(stderr, "VibeServer: no %s yet — running from the command line as before. "
                                 "Settings you save in the browser will create it.\n",
                         g_configPath.c_str());
    }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    // A client vanishing mid-write must not kill the daemon.
    std::signal(SIGPIPE, SIG_IGN);

    using vibe::LocalSdrShim;

    // ── Server-side policy, ALL of it before start() (the shim reads these once). ────────────
    // This is the whole point of the harness: these are the levers the operator gets, and the
    // ones Jr has been asking the server to honour with nothing on the other end.
    LocalSdrShim::setServeOnLan(true);          // bind 0.0.0.0, not loopback
    LocalSdrShim::setVibeServerPort(o.port);    // 0 = auto-scan
    LocalSdrShim::setVibeServerAuth(o.pin);     // empty = open; loopback is always exempt
    LocalSdrShim::setVibeServerLimits(o.maxBw, o.maxFps);
    LocalSdrShim::setVibeServerLockedRate(o.lockRate);

    // ── Channel method: decided ONCE, here, and never again while the process runs ──────────
    // Stuart, 2026-08-02: "never switch methods live, it is in the setup". Switching mid-stream
    // would swap filter state under a running demodulator — the same discontinuity that leaves
    // RDS half-dead after an idle resume.
    bool shared = o.channels.empty() ? (o.users > 1) : (o.channels == "shared");
    if (!o.channels.empty() && o.channels != "shared" && o.channels != "direct") {
        std::fprintf(stderr, "VibeServer: --channels must be 'direct' or 'shared'\n");
        return 2;
    }
    // ★★★ Shared channels are slices of ONE FFT of ONE captured band. If a listener can move the
    // hardware centre, that band moves under everybody and the slices stop meaning anything — so
    // this is a precondition, not a preference. Refused loudly rather than quietly downgraded: an
    // operator who asked for a multi-user receiver should not discover at showtime that it built
    // a single-user one.
    if (shared && o.lockFreq <= 0.0) {
        std::fprintf(stderr,
            "VibeServer: a shared receiver needs a locked centre — add --lock-freq HZ\n"
            "            (e.g. --lock-freq 6500000 --rate 8000000 covers 2.5-10.5 MHz)\n");
        return 2;
    }
    if (o.lockFreq > 0.0) {
        LocalSdrShim::setVibeServerLockedCentre(o.lockFreq);
        o.freq = o.lockFreq;              // the locked centre IS the capture centre
        std::printf("VibeServer: centre LOCKED at %.3f MHz — listeners tune inside %.3f-%.3f MHz\n",
                    o.lockFreq / 1e6, (o.lockFreq - o.rate / 2) / 1e6, (o.lockFreq + o.rate / 2) / 1e6);
    }
    LocalSdrShim::setVibeServerSharedChannels(shared);
    LocalSdrShim::setVibeServerZoomSpectrum(o.zoomSpectrum);
    LocalSdrShim::setVibeServerIdleGrace(o.idleGrace);
    LocalSdrShim::setVibeServerRfNotch(o.rfNotch);
    // ★ The front end as the owner last left it. Applied after the start-up AGC sequence — see
    //   the kick's final step, which is the only point where it will not be immediately undone.
    LocalSdrShim::setVibeServerSavedFrontEnd(g_runtimeConfig.lnaState, g_runtimeConfig.ifGr,
                                             g_runtimeConfig.ifAgc);
    LocalSdrShim::setVibeServerDabNotch(o.dabNotch);
    // ★ SAY WHAT THE FRONT END WILL DO. These are set once at startup and a listener cannot
    //   change them on a locked receiver, so if the operator's intent and the radio disagree
    //   there is otherwise NOTHING on screen or in the log to reveal it — which is exactly the
    //   position the notches were in (2026-08-03).
    std::printf("VibeServer: front end — RF notch %s, DAB notch %s, idle grace %.0fs\n",
                o.rfNotch ? "ON" : "off", o.dabNotch ? "ON" : "off", o.idleGrace);
    // ★ --users is now the real listener cap, not just the channel-method hint.
    LocalSdrShim::setVibeServerMaxUsers(o.users);

    // ── ★★★ THE CONFIG API ────────────────────────────────────────────────────────────────
    // The daemon owns persistence; the shim owns the HTTP surface. Registering these is what
    // turns GET/POST /vibeserver/config from 501 into a working endpoint — and the browser setup
    // page is a CLIENT of it, so the app becomes a second client later with no work here.
    LocalSdrShim::setConfigured(g_runtimeConfig.configured);
    // ★ Where a NEW session starts. Falls back to the startup frequency when the owner has not
    //   set a separate landing one, so "where new listeners start" always means something.
    LocalSdrShim::setVibeServerLanding(
        g_runtimeConfig.landingFreq > 0 ? g_runtimeConfig.landingFreq : g_runtimeConfig.freq,
        g_runtimeConfig.demodMode);

    // ── ★★★ mDNS: ADVERTISE THE NAME THE OWNER CHOSE ────────────────────────────────────────
    // This was STORED and never acted on — main.cpp never called startMdns at all, so the Linux
    // daemon has never advertised itself. `vibeserver.local` appearing to work on the Pi was the
    // OS's own avahi publishing the machine's HOSTNAME; the name set in the setup page did
    // nothing (Stuart, 2026-08-05: "the mdns address doesnt appear to be working, direct IP is").
    // ★★ A setting that is saved, echoed back and never acted on is worse than one that is
    //    missing — the page tells the owner it worked.
    // ★ Gated on `configured`, so an unconfigured server is never discoverable: that is what
    //   keeps the app from meeting a server it has no setup flow for.
    // ★★★ NEVER ADVERTISE WITHOUT A NAME THE OWNER CHOSE. mdnsLabel("") falls back to
    //     "vibeserver", so an unnamed server claims `vibeserver.local` — and EVERY unnamed server
    //     on the network claims the SAME one. Worse than two of ours clashing: a Raspberry Pi
    //     whose hostname is "VibeServer" already publishes that name through avahi, so an unnamed
    //     instance elsewhere on the LAN silently steals the address people use to reach the real
    //     receiver. Seen live on 2026-08-05 — a test server on a laptop took vibeserver.local
    //     away from the Pi and SSH to it started failing.
    //     ★ The friendly name is set in the browser during setup, which is exactly when a server
    //       becomes worth finding — so "no name yet" and "not ready to advertise" are one state.
    const std::string wantName = g_runtimeConfig.mdnsName.empty()
                               ? g_runtimeConfig.name : g_runtimeConfig.mdnsName;
    if (g_runtimeConfig.configured && g_runtimeConfig.mdnsAdvertise && !wantName.empty()) {
        const std::string label = vsconfig::mdnsLabel(wantName);
        const std::string ip = primaryIpv4();
        if (!label.empty() && !ip.empty()) {
            LocalSdrShim::startMdns(label, ip);
            std::printf("VibeServer: advertising as %s.local (%s)\n", label.c_str(), ip.c_str());
        } else {
            std::fprintf(stderr, "VibeServer: cannot advertise on mDNS — %s\n",
                         ip.empty() ? "no IPv4 address found" : "no name set");
        }
    }
    LocalSdrShim::setConfigHandlers(
        []() -> std::string {
            // ★ What the server is ACTUALLY RUNNING, not what the file last said. The two differ
            //   the moment anyone passes a flag, and a settings page that shows the file while the
            //   radio obeys something else is a page that lies.
            return vsconfig::toJson(g_runtimeConfig);
        },
        [](const std::string& json, std::string& err) -> bool {
            // Apply over the RUNNING config so a partial POST is a patch, not a wipe.
            vsconfig::Config next = g_runtimeConfig;
            if (!vsconfig::fromJson(json, next, err)) return false;
            // ★★ SAVING IS WHAT MARKS IT CONFIGURED. The owner pressing Save on the setup page is
            //    the only event that means "I have finished" — nothing else should set this.
            next.configured = true;
            if (!vsconfig::save(g_configPath, next, err)) return false;
            g_runtimeConfig = next;
            LocalSdrShim::setConfigured(true);
            // ★★★ RESTART TO APPLY. Almost everything here is read ONCE at start (the channel
            //     method most of all — "never switch methods live, it is in the setup"), so
            //     applying half of it live would leave the server in a state no code path was
            //     written for. systemd brings us straight back; the page reconnects and lands on
            //     the normal receiver screen. Under systemd this is the documented way to reload.
            g_restartRequested.store(true);
            return true;
        });

    // ── Space weather ───────────────────────────────────────────────────────────────────────
    // ★ One request an hour on a detached thread, and the handler only ever READS the cache — so
    //   a page load never waits on NOAA and a network outage costs freshness, not the page.
    LocalSdrShim::setSolarHandler([]() -> std::string {
        const auto s = vssolar::current();
        if (!s.valid()) return "";
        // ★ Daylight AT THE RECEIVER decides which column applies, and the receiver's longitude is
        //   the only thing that can answer that — a UTC hour would be right for one meridian only.
        //   Rough local solar time is plenty: we are choosing between "day" and "night", not
        //   computing a sunrise.
        // ★★★ THE REAL SUN, AND THE LOCATOR IF THAT IS ALL WE HAVE. lat/lon are optional on the
        //     setup page and were EMPTY on the very server this was written for — but the locator
        //     was filled in, and a locator IS a position. Deriving one from the other is what
        //     makes the setting the owner actually filled in do the work (Stuart, 2026-08-06).
        //     ★ And the day/night test is now sun-above-horizon rather than a 07:00-19:00 window,
        //       which was wrong by hours: at 52°N in August the sun is up at 04:30, so the server
        //       reported NIGHT through a summer morning and predicted 80m "Excellent" on a band
        //       that was plainly a daytime Fair.
        double lat = 0, lon = 0;
        bool havePos = false;
        try {
            if (!g_runtimeConfig.lat.empty() && !g_runtimeConfig.lon.empty()) {
                lat = std::stod(g_runtimeConfig.lat);
                lon = std::stod(g_runtimeConfig.lon);
                havePos = true;
            }
        } catch (...) { havePos = false; }
        if (!havePos && !g_runtimeConfig.locator.empty())
            havePos = vssolar::gridToLatLon(g_runtimeConfig.locator, lat, lon);
        const std::time_t t = std::time(nullptr);
        // ★ With no position at all, fall back to Greenwich rather than refusing: a day/night
        //   verdict that is right for most of Europe beats no conditions panel at all, and the
        //   owner can fix it by filling in one field.
        const bool day = vssolar::sunUp(havePos ? lat : 51.5, havePos ? lon : 0.0, t);

        char head[256];
        snprintf(head, sizeof head,
                 "{\"sfi\":%.0f,\"kp\":%.1f,\"updated\":\"%s\",\"day\":%s,\"bands\":{",
                 s.sfi, s.kp, s.updated.c_str(), day ? "true" : "false");
        std::string j = head;
        static const char* kBands[] = {"160m","80m","60m","40m","30m","20m","17m","15m","12m","10m"};
        bool first = true;
        for (const char* b : kBands) {
            const std::string v = vssolar::bandVerdict(s, b, day);
            if (v.empty()) continue;
            if (!first) j += ",";
            first = false;
            j += "\"" + std::string(b) + "\":\"" + v + "\"";
        }
        return j + "}}";
    });
    {
        std::thread([]{
            std::this_thread::sleep_for(std::chrono::seconds(25));
            for (;;) {
                if (vssolar::needsRefresh()) {
                    std::string e;
                    if (!vssolar::fetch(e) && !e.empty())
                        std::fprintf(stderr, "VibeServer: space weather unavailable — %s\n", e.c_str());
                }
                std::this_thread::sleep_for(std::chrono::minutes(15));
            }
        }).detach();
    }

    // ── EiBi ────────────────────────────────────────────────────────────────────────────────
    // ★ Published from the cache at start-up so search works immediately, then refreshed in the
    //   BACKGROUND: a receiver must not wait on eibispace.de to start serving listeners.
    LocalSdrShim::setEibiHandler([](bool refresh, std::string& err, std::string& updated) -> int {
        int n = refresh ? vseibi::refresh(err) : 0;
        if (!refresh || n == 0) { const int c = vseibi::loadFromCache(); if (c > n) n = c; }
        updated = vseibi::status();
        return n;
    });
    {
        const int n = vseibi::loadFromCache();
        if (n > 0) std::printf("VibeServer: EiBi schedule — %d entries (cached %s)\n",
                               n, vseibi::status().c_str());
        else       std::printf("VibeServer: no EiBi schedule yet — download it from the setup page\n"
                               "            so listeners can search by station name.\n");
        // ★★ A DAILY REFRESH, DETACHED. Once a day matches the app, and a season roll-over is
        //    picked up because the filename is derived from the date each time. Detached because
        //    nothing here may ever block the radio: a hung download must cost a schedule, not a
        //    receiver.
        std::thread([]{
            // ★★★ CHECK FIRST, THEN SLEEP — and decide from the CACHE'S AGE, never from uptime.
            //     Sleeping 24 h before the first attempt meant the timer restarted whenever the
            //     server did, and saving anything on the setup page restarts the server: a
            //     receiver its owner adjusts every few days would have refreshed NEVER while
            //     looking like it refreshed daily.
            //     ★ A short delay before the first go, so start-up is not competing with the
            //       radio coming up for the network.
            std::this_thread::sleep_for(std::chrono::seconds(20));
            for (;;) {
                if (vseibi::needsRefresh()) {
                    std::string e;
                    const int n2 = vseibi::refresh(e);
                    if (n2 > 0) std::printf("VibeServer: EiBi refreshed — %d entries\n", n2);
                    else if (!e.empty())
                        std::fprintf(stderr, "VibeServer: EiBi refresh failed — %s\n", e.c_str());
                }
                // ★ Hourly wake, daily WORK: needsRefresh() is a stat() and a tiny read, so the
                //   cost is nothing, and it means a server that was off over a season rollover
                //   picks it up within the hour rather than within the day.
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        }).detach();
    }

    // ★★ THE LIVE-SETTING PATH, deliberately separate from the one above. That one is the setup
    //    page pressing Save and it RESTARTS to apply; this one is an admin nudging the RF gain
    //    while listening, where a restart would be absurd — the radio has already been told, and
    //    all that is left is to remember it. Merge, write, carry on.
    LocalSdrShim::setConfigPersistHandler([](const std::string& patch) {
        std::string err;
        vsconfig::Config next = g_runtimeConfig;
        if (!vsconfig::fromJson(patch, next, err, /*validate=*/false)) {
            std::fprintf(stderr, "VibeServer: could not apply setting (%s)\n", err.c_str());
            return;
        }
        // ★ Do NOT touch `configured` here. Only the setup page finishing means "set up"; a gain
        //   tweak on a half-configured server must not silently declare it done.
        if (!vsconfig::save(g_configPath, next, err)) {
            std::fprintf(stderr, "VibeServer: could not save setting (%s)\n", err.c_str());
            return;
        }
        g_runtimeConfig = next;
    });
    std::printf("VibeServer: channel method = %s (for %d listener%s)\n",
                shared ? "shared / fast convolution" : "direct", o.users, o.users == 1 ? "" : "s");
    LocalSdrShim::setVibeServerWebEnabled(o.web);
    // ★★★ THE OPERATOR SETTINGS — every one of these setters already existed and the CLI simply
    // never called any of them. So a headless VibeServer had no admin password, no per-listener
    // limit, no audio policy and no identity: it was the LEAST safe place to host a public
    // receiver, which is the exact opposite of what a Pi appliance is for
    // (Stuart, 2026-07-31: "the Pi isn't safe to use to make a server public").
    // ★★ ADMIN PASSWORD IS THE IMPORTANT ONE. The PIN decides who may LISTEN; this decides who may
    // touch bias-T (DC on the feedline), direct sampling (reconfigures the front end) and
    // calibration (miscalibrates the radio invisibly and permanently). A public receiver typically
    // wants NO pin and a STRONG admin password.
    LocalSdrShim::setVibeServerAdminSecret(o.adminPass);
    LocalSdrShim::setVibeServerSessionLimit(o.sessionLimitMin);
    LocalSdrShim::setVibeServerForceIdleSaver(o.forceIdleSaver);
    LocalSdrShim::setVibeServerUncompressedAudio(o.uncompressed);
    // ★ Identity, published to every listener — and what makes a directory entry worth anything.
    // Built here in exactly the shape the Mac app produces (VibeServerApp.swift locationJson), so
    // the same receiver looks the same however it is hosted.
    {
        // ★★★ DERIVE lat/lon FROM THE LOCATOR. The first version passed the grid straight through
        // and computed no coordinates, so a receiver with only a locator set showed no place under
        // its name and did not appear on any map (Stuart, 2026-08-01). The Mac has always done this
        // (VibeServerApp.swift gridCentre) and only emits "grid" INSIDE the lat/lon block — so
        // without coordinates the grid never reaches a client either.
        // ★★ THE CENTRE OF THE SQUARE, NOT THE CORNER. A corner biases every distance by half a
        // square in a fixed direction, which is a systematic error rather than the honest rounding
        // the operator asked for by giving a locator in the first place. Same comment as the Mac's,
        // and the same arithmetic, so the two agree to the metre.
        auto gridCentre = [](const std::string& g, double& lat, double& lon) -> bool {
            std::string u; for (char c : g) u += (char)toupper((unsigned char)c);
            if (u.size() != 4 && u.size() != 6) return false;
            if (!isalpha((unsigned char)u[0]) || !isalpha((unsigned char)u[1])
                || !isdigit((unsigned char)u[2]) || !isdigit((unsigned char)u[3])) return false;
            int f1 = u[0] - 'A', f2 = u[1] - 'A';
            if (f1 < 0 || f1 >= 18 || f2 < 0 || f2 >= 18) return false;
            int s1 = u[2] - '0',  s2 = u[3] - '0';
            lon = f1 * 20.0 + s1 * 2.0 - 180.0;
            lat = f2 * 10.0 + s2 * 1.0 - 90.0;
            if (u.size() == 6) {
                if (!isalpha((unsigned char)u[4]) || !isalpha((unsigned char)u[5])) return false;
                int t1 = u[4] - 'A', t2 = u[5] - 'A';
                if (t1 < 0 || t1 >= 24 || t2 < 0 || t2 >= 24) return false;
                lon += t1 * (2.0 / 24.0) + (1.0 / 24.0);      // + half a sub-square
                lat += t2 * (1.0 / 24.0) + (0.5 / 24.0);
            } else { lon += 1.0; lat += 0.5; }                // + half a square
            return true;
        };
        double la = 0, lo = 0; bool haveLL = false;
        if (!o.rxLat.empty() && !o.rxLon.empty()) {
            la = atof(o.rxLat.c_str()); lo = atof(o.rxLon.c_str()); haveLL = true;
        } else if (!o.rxGrid.empty()) {
            haveLL = gridCentre(o.rxGrid, la, lo);            // exact coordinates win; this is the fallback
            if (!haveLL) std::fprintf(stderr, "VibeServer: --locator \"%s\" is not a Maidenhead square "
                                              "(expected 4 or 6 characters, e.g. IO92nh)\n", o.rxGrid.c_str());
        }
        std::string j; auto add = [&](const std::string& k, const std::string& v, bool quote) {
            if (v.empty()) return;
            if (!j.empty()) j += ",";
            j += "\"" + k + "\":" + (quote ? "\"" + v + "\"" : v);
        };
        add("name",  o.rxName,  true);
        add("iso",   o.rxIso,   true);
        add("label", o.rxPlace, true);
        if (haveLL && la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
            char b[64];
            std::snprintf(b, sizeof b, "%.6f", la); add("lat", b, false);
            std::snprintf(b, sizeof b, "%.6f", lo); add("lon", b, false);
            // ★ Echo the operator's OWN locator when they gave one — recomputing it from the
            // square's centre returns the same square, but showing back exactly what was typed is
            // less confusing than showing a value they did not enter. (The Mac's words.)
            add("grid", o.rxGrid, true);
        }
        if (!j.empty()) LocalSdrShim::setLocationJson("{" + j + "}");
    }
    // ★★ SAY SO WHEN IT IS WIDE OPEN. Someone putting a receiver on the internet should be told at
    // the moment they start it, not discover it from a stranger changing their bias-T.
    if (o.adminPass.empty())
        std::fprintf(stderr,
            "VibeServer: NO ADMIN PASSWORD SET — anyone who can reach this server may change\n"
            "            bias-T, direct sampling and calibration. Set --admin-pass before\n"
            "            putting this receiver on a public address.\n");

    // The shim is a singleton — one radio, one pipeline, one server per process.
    LocalSdrShim& shim = LocalSdrShim::instance();
    std::string err;
    // fd < 0 selects the desktop USB path: -1 = device 0, -2 = device 1, … (see local_sdr_shim.cpp).
    // ★★★ FIND WHATEVER RADIO IS ACTUALLY PLUGGED IN. This used to call the RTL path and nothing
    // else, so an Airspy HF+ sitting on the USB bus — visible in lsusb, working perfectly — was
    // reported as "no SDR found — is it plugged in?" (Raspberry Pi, 2026-07-31). The shim has had
    // startAirspyHf() all along; the CLI simply never asked for it, because on Android the driver
    // is chosen from the USB descriptor and on the Mac the menu-bar app picks it.
    // ★★ A HEADLESS BOX HAS NOBODY TO ASK. It boots with whatever is in the port and must work it
    // out itself, so: try the RTL path, and if there is no dongle but there IS an HF+, use that.
    // ★ Order is deliberate — an explicit --device N means the user named an RTL index, so that
    // still wins. Same "never infer what the hardware can tell you" rule, applied to discovery.
    int port;
    if (!o.useUsb) {
        port = shim.startTcp(o.tcpHost, o.tcpPort, o.freq, o.rate, o.gain,
                             o.fftSize, o.fftRate, o.mode, err);
    } else if (!o.deviceGiven && vibe::AirspyHfSource::deviceCount() > 0) {
        std::printf("VibeServer: Airspy HF+ detected\n");
        port = shim.startAirspyHf(0, o.freq, o.rate, o.gain,
                                  o.fftSize, o.fftRate, o.mode, err);
    } else if (!o.deviceGiven && vibe::SdrplaySource::deviceCount() > 0) {
        // ★★★ AND THE RSP, WHICH THIS CHAIN STILL DID NOT ASK FOR. Exactly the fault the note
        // above describes for the Airspy, one radio later: an RSP1B on the bus — named correctly
        // by lsusb, and sdrplay_api_GetDevices returning it with a serial — fell through to the
        // RTL branch, found no dongle, and was reported as "no SDR found — is it plugged in?"
        // (Raspberry Pi, 2026-08-02). The shim has had startSdrplay() all along; nothing called it
        // on a headless box, because until today that box had no SDRplay API installed to build
        // against, so the gap could not show.
        // ★ A three-radio project needs the discovery chain to know about three radios. If a
        //   fourth is ever added, this is the list that has to grow with it.
        std::printf("VibeServer: SDRplay RSP detected\n");
        port = shim.startSdrplay(0, o.freq, o.rate, o.gain,
                                 o.fftSize, o.fftRate, o.mode, err);
    } else {
        port = shim.start(-(o.device + 1), 0, 0, o.freq, o.rate, o.gain,
                          o.fftSize, o.fftRate, o.mode, err);
    }
    if (port <= 0) {
        std::fprintf(stderr, "VibeServer: failed to start — %s\n",
                     err.empty() ? "(no reason given)" : err.c_str());
        // Only offer the source hint when the failure could plausibly BE the source — a port
        // clash has nothing to do with rtl_tcp, and a wrong hint sends people hunting in the
        // wrong place.
        if (!o.useUsb && err.find("port") == std::string::npos)
            std::fprintf(stderr, "Is there an rtl_tcp listening on %s:%d?\n", o.tcpHost.c_str(), o.tcpPort);
        return 1;
    }

    if (o.useUsb) std::printf("VibeServer listening on port %d\n", port);
    else          std::printf("VibeServer listening on port %d  (IQ from rtl_tcp %s:%d)\n",
                              port, o.tcpHost.c_str(), o.tcpPort);
    std::printf("  clients: http://<this-machine>:%d/    auth: %s\n",
                port, o.pin.empty() ? "OPEN (no PIN)" : "PIN required (except from this machine)");
    if (o.maxBw > 0 || o.maxFps > 0)
        std::printf("  ceilings: bandwidth %.0f Hz, spectrum %.0f fps\n", o.maxBw, o.maxFps);
    std::printf("Ctrl-C to stop.\n");

    // Status line once a second — the CLI's whole UI. Deliberately the same numbers the menu-bar
    // status view will show (BRIEF §3), so the GUI is a renderer of this, not its own accounting.
    // ★ Set AFTER start(), because loading needs the window (centre and span) to know whether the
    //   stored history belongs to this profile at all.
    LocalSdrShim::instance().setSpectrogramPath("/var/lib/vibeserver/spectrogram.bin");

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // ★ The DSP path only raises a FLAG; the write happens here, off that thread.
        LocalSdrShim::instance().saveSpectrogramIfDue();
        if (!shim.isRunning()) {
            std::fprintf(stderr, "VibeServer: capture stopped unexpectedly.\n");
            break;
        }
        // ★★ A SAVE FROM THE SETUP PAGE ASKS FOR A RESTART. Exit cleanly and let systemd bring us
        //    back with the new config — the settings that matter most (the channel method, the
        //    locked window, the capture rate) are read ONCE at start, and Stuart's rule is "never
        //    switch methods live, it is in the setup".
        // ★ Exit 0, not a failure code: this is a requested restart, and `Restart=always` must
        //   treat it as routine. A non-zero exit here would look like a crash in the journal of a
        //   box nobody can see.
        if (g_restartRequested.load()) {
            std::printf("\nConfiguration saved — restarting to apply it.\n");
            // ★★ SAVE BEFORE A RESTART. This is the common case by far — every settings save
            //    restarts the server, and losing a day of history to a one-field change is what
            //    made persistence necessary in the first place.
            LocalSdrShim::instance().saveSpectrogram();
            LocalSdrShim::stopMdns();
            shim.stop();
            return 0;
        }
    }

    std::printf("\nStopping…\n");
    // ★★★ AND ON A PLAIN STOP, which is the path `systemctl restart` and a reboot actually take —
    //     SIGTERM sets g_stop and we arrive here. The first cut of this only saved on the
    //     SETUP-PAGE restart, so a reboot silently lost the history it was written to protect; the
    //     edit had matched the wrong `stopMdns()` of the two. Both exits save now.
    LocalSdrShim::instance().saveSpectrogram();
    // ★★★ STOP THE mDNS RESPONDER, or every exit is an ABRT. It keeps a STATIC std::thread, so
    //     at process exit its destructor runs on a still-joinable thread and std::terminate
    //     fires: "terminate called without an active exception", status=6/ABRT in the journal on
    //     EVERY restart from the moment advertising was first wired in (2026-08-05, 16:40 — the
    //     timeline is what identified it, since the message names nothing).
    //     ★ Harmless in itself, and exactly the kind of noise that hides the first REAL crash.
    LocalSdrShim::stopMdns();
    shim.stop();
    return 0;
}
