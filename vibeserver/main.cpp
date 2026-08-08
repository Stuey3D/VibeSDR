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
#include <sys/stat.h>
#include <ctime>
#include <cstring>
#include <rtl-sdr.h>
#include "rtl_eeprom.h"
#include "radios.h"
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
#include "geoip.h"
#include "asndb.h"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

struct Opts {
    int         device  = 0;             // USB device index; <0 = use rtl_tcp instead
    bool        useUsb  = true;          // default: drive the dongle directly
    int         radio   = 0;             // index into the FLAT list (dongles, then RSPs, then HF+)
    std::string radioSerial;             // ★ preferred: identity that does not move
    bool        portGiven = false;       // an explicit --port always wins
    bool        radioGiven = false;
    bool        deviceGiven = false;     // ★ an explicit --device N names an RTL index and wins
    // ★★★ THE ADMIN / OPERATOR SETTINGS. Without these a Pi CANNOT SAFELY BE MADE PUBLIC — Stuart,
    // 2026-07-31. The admin password is not a convenience: it is what stands between a stranger and
    // BIAS-T (DC on the feedline), DIRECT SAMPLING (reconfigures the front end) and CALIBRATION
    // (miscalibrates the radio invisibly and permanently). The Mac has had all of this since v10;
    // the headless build shipped without any of it, which made it the LEAST safe place to host.
    std::string adminPass;
    int         sessionLimitMin = 0;     // per-listener minutes; 0 = unlimited
    int         adminIdleMin = 30;      // admin controls re-lock after this idle; 0 = never
    // ★ Local by default — the mode that behaves exactly as VibeServer always has. A new setting
    //   must never change what an existing install does.
    bool        publicSharing = false;
    int         updateSrvHour = -1, updateSrvDay = -1;   // VibeServer only
    int         updateAllHour = -1, updateAllDay = -1;   // every package
    bool        forceIdleSaver  = false; // listeners may not switch idle power-saving off
    bool        releaseWhenIdle = false; // hand the SDR to another program while nobody listens
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
        "  --public              this receiver is shared with strangers: the admin page adds\n"
        "                        listeners, blocking and connection history. Without it the\n"
        "                        server behaves exactly as before — those panels are about\n"
        "                        managing people you do not know.\n"
        "  --admin-idle MIN      re-lock admin controls after MIN idle (default 30, 0 = never).\n"
        "                        The session keeps running — only the controls lock.\n"
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
        // ★ --radio indexes the FLAT list across all three drivers; --device is the old
        //   dongle-only index, kept because existing installs pass it.
        // ★★★ A NUMBER OR A SERIAL, and the serial is the one to trust. Indices are positions in
        //     a list that CHANGES: an SDRplay held by another program does not enumerate at all,
        //     so the moment the RSP was busy the Airspy moved from 2 to 1 — and `--radio 1` then
        //     means a different radio than it did a minute earlier. Measured on the Pi, 2026-08-08,
        //     while the demo itself was pinned to `--radio 1`.
        //   ★ Anything non-numeric is a serial. Serials are what the config file stores.
        else if (a == "--radio") {
            const std::string v = need(i);
            o.useUsb = true; o.radioGiven = true;
            // ★★★ A SERIAL FIRST, AN INDEX ONLY IF IT IS NOT ONE. RTL serials are ALL DIGITS —
            //     "00000003" on this Pi, "00000001" from the factory — so "numeric means index"
            //     read a serial as index 3 and started the wrong radio entirely (measured
            //     2026-08-08: `--radio 00000003` reported "using Airspy HF+ 2"). Match against
            //     what is actually attached before falling back to a position.
            o.radioSerial.clear(); o.radio = -1;
            for (const auto& r : vibe::detectRadios())
                if (r.serial == v) { o.radioSerial = v; break; }
            if (o.radioSerial.empty()) {
                if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos)
                    o.radio = std::atoi(v.c_str());
                else
                    o.radioSerial = v;   // a serial we cannot see yet; fail with a clear reason later
            }
        }
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
        else if (a == "--port")      { o.port = std::atoi(need(i)); o.portGiven = true; }
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
        else if (a == "--admin-idle")     o.adminIdleMin    = std::atoi(need(i));
        else if (a == "--public")         o.publicSharing   = true;
        // ★★★ "RUN THE SERVER, USING THE STORED CONFIG, AND DO NOT SHOW THE SETUP SCREEN."
        //     It sets nothing — the config file already has it all. It exists because the ONLY
        //     way to reach the server was to pass some other flag, and a user on a machine with
        //     no systemd found that out the hard way: `vibeserver` gave them the setup screen,
        //     whose "restart the server" does nothing without a service, so no server ever ran —
        //     while `vibeserver --device 0` worked perfectly and looked like a lucky accident.
        //     Now there is an honest way to say it, and the setup screen can offer it.
        else if (a == "--serve")          { /* config supplies everything */ }
        else if (a == "--force-idle-saver") o.forceIdleSaver = true;
        else if (a == "--release-when-idle") o.releaseWhenIdle = true;
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
    o.adminIdleMin    = c.adminIdleMin;
    o.publicSharing   = (c.sharing == vsconfig::Sharing::Public);
    o.updateSrvHour = c.updateSrvHour; o.updateSrvDay = c.updateSrvDay;
    o.updateAllHour = c.updateAllHour; o.updateAllDay = c.updateAllDay;
    o.freq = c.landingFreq > 0 ? c.landingFreq : c.freq;
    o.rate = c.rate;
    o.lockFreq = c.lockFreq; o.lockRate = c.lockRate;
    o.gain = c.gain;
    o.mode = c.demodMode;
    o.users = c.users;
    o.maxBw = c.maxBw; o.maxFps = c.maxFps; o.fftRate = c.fftRate;
    o.uncompressed = c.uncompressed;
    o.forceIdleSaver = c.forceIdleSaver;
    o.releaseWhenIdle = c.releaseWhenIdle;
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
    c.adminIdleMin    = o.adminIdleMin;
    c.sharing         = o.publicSharing ? vsconfig::Sharing::Public : vsconfig::Sharing::Local;
    c.updateSrvHour = o.updateSrvHour; c.updateSrvDay = o.updateSrvDay;
    c.updateAllHour = o.updateAllHour; c.updateAllDay = o.updateAllDay;
    c.freq = o.freq; c.rate = o.rate;
    c.lockFreq = o.lockFreq; c.lockRate = o.lockRate;
    c.gain = o.gain; c.demodMode = o.mode;
    c.users = o.users;
    c.maxBw = o.maxBw; c.maxFps = o.maxFps; c.fftRate = o.fftRate;
    c.uncompressed = o.uncompressed;
    c.forceIdleSaver = o.forceIdleSaver;
    c.releaseWhenIdle = o.releaseWhenIdle;
    c.idleGrace = o.idleGrace;
    c.rfNotch = o.rfNotch; c.dabNotch = o.dabNotch; c.zoomSpectrum = o.zoomSpectrum;
    c.port = o.port; c.web = o.web;
    c.mode = o.lockFreq > 0 ? vsconfig::Mode::LockedRange : vsconfig::Mode::SingleUser;
}

}  // namespace

/** ★ Small and local on purpose: the shim has one, but it is a private member of an internal
 *  class, and reaching into that to save nine lines would couple the daemon to its internals. */
static std::string jsonEscape(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20)  o += ' ';
        else o += c;
    }
    return o;
}

namespace { std::string g_configPath; vsconfig::Config g_runtimeConfig;
            /** ★ The WHOLE machine, so the setup page can show a tab per radio. This process runs
             *  exactly one of them (g_myRadioSerial); the rest it only reads and writes on their
             *  behalf, which is why every write re-reads the file first. */
            vsconfig::ServerConfig g_serverConfig; std::string g_myRadioSerial;
            std::atomic<bool> g_restartRequested{false}; }


/** ★★★ RENAME A DONGLE, THE ONE DESTRUCTIVE THING THIS PROGRAM DOES.
 *
 *  Two RTL dongles of the same model are indistinguishable — they ship with the same serial — so
 *  anything that remembers per-radio settings remembers them against the WRONG radio as soon as a
 *  second one appears. Giving each a unique serial is the fix, and it is a normal step for anyone
 *  running several (OpenWebRX needs it too). There was simply nowhere to do it: Stuart had to boot
 *  a Windows PC and use SDR Console (2026-08-07). A receiver that sends you to another operating
 *  system to finish setting it up is not finished.
 *
 *  ★★ A BAD WRITE BRICKS THE DONGLE. Not "resets" — bricks. So, in order, and no step is optional:
 *      1. refuse if anything else has the radio, rather than fighting it for the chip
 *      2. read and fully PARSE the existing image; refuse anything not understood completely
 *      3. save a byte-for-byte BACKUP to disk and print where it went
 *      4. show old → new and make the operator type the new serial back
 *      5. write, then reopen and READ BACK, and compare against what we meant to write
 *  The rebuild itself is covered by test-rtl-eeprom against a real dumped image, so by the time we
 *  get here the bytes are the part we are least worried about.
 */
static int setRtlSerial(int index, const std::string& newSerial) {
    std::string err;
    if (!vibe::rtlSerialAcceptable(newSerial, err)) {
        std::fprintf(stderr, "VibeServer: %s\n", err.c_str());
        return 1;
    }
    const uint32_t n = rtlsdr_get_device_count();
    if (index < 0 || (uint32_t)index >= n) {
        std::fprintf(stderr, "VibeServer: there is no RTL-SDR %d (%u found)\n", index, n);
        return 1;
    }
    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_open(&dev, (uint32_t)index) != 0 || !dev) {
        std::fprintf(stderr,
            "VibeServer: could not open that dongle — something else is using it.\n"
            "            Stop the server first:  sudo systemctl stop vibeserver\n");
        return 1;
    }
    uint8_t buf[vibe::RTL_EEPROM_SIZE] = {0};
    if (rtlsdr_read_eeprom(dev, buf, 0, sizeof buf) < 0) {
        std::fprintf(stderr, "VibeServer: could not read the dongle's memory — nothing was changed.\n");
        rtlsdr_close(dev); return 1;
    }
    vibe::RtlEeprom cur;
    if (!vibe::rtlEepromParse(buf, sizeof buf, cur, err)) {
        std::fprintf(stderr,
            "VibeServer: %s\n"
            "            Refusing to write to a chip we do not fully understand — the parts we\n"
            "            could not read are exactly the parts we would destroy. Nothing changed.\n",
            err.c_str());
        rtlsdr_close(dev); return 1;
    }
    std::vector<uint8_t> want;
    if (!vibe::rtlEepromWithSerial(cur, newSerial, want, err)) {
        std::fprintf(stderr, "VibeServer: %s — nothing was changed.\n", err.c_str());
        rtlsdr_close(dev); return 1;
    }

    // ── 3. The backup, before anything is written ───────────────────────────────────────────
    std::string dir = "/var/lib/vibeserver/eeprom";
    ::mkdir("/var/lib/vibeserver", 0755);
    ::mkdir(dir.c_str(), 0755);
    char stamp[32]; const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    const std::string backup = dir + "/" + (cur.serial.empty() ? "unnamed" : cur.serial)
                             + "-" + stamp + ".bin";
    if (FILE* f = std::fopen(backup.c_str(), "wb")) {
        const bool wrote = std::fwrite(buf, 1, sizeof buf, f) == sizeof buf;
        std::fclose(f);
        if (!wrote) {
            std::fprintf(stderr, "VibeServer: the backup did not write fully — stopping here.\n");
            rtlsdr_close(dev); return 1;
        }
    } else {
        // ★ NO BACKUP, NO WRITE. Without it a failed write leaves nothing to restore from, and
        //   this is the operation where that matters most.
        std::fprintf(stderr, "VibeServer: could not save a backup to %s — stopping here.\n"
                             "            (try again with sudo)\n", backup.c_str());
        rtlsdr_close(dev); return 1;
    }

    // ── 4. Say exactly what is about to happen, and make them type it ──────────────────────
    std::printf("\n  Dongle %d:  %s %s\n", index, cur.manufacturer.c_str(), cur.product.c_str());
    std::printf("  Serial:    %s  ->  %s\n", cur.serial.c_str(), newSerial.c_str());
    std::printf("  Backup:    %s\n\n", backup.c_str());
    std::printf("  This writes to the dongle's memory. If it is interrupted the dongle can be\n"
                "  left unusable. Do not unplug it, and do not do this on a machine that might\n"
                "  lose power.\n\n");
    std::printf("  Type the new serial to confirm, or anything else to cancel: ");
    std::fflush(stdout);
    char typed[128] = {0};
    if (!std::fgets(typed, sizeof typed, stdin)) { rtlsdr_close(dev); return 1; }
    std::string confirm(typed);
    while (!confirm.empty() && (confirm.back() == '\n' || confirm.back() == '\r')) confirm.pop_back();
    if (confirm != newSerial) {
        std::printf("\n  Cancelled. Nothing was changed.\n");
        rtlsdr_close(dev); return 1;
    }

    // ── 5. Write, then prove it ────────────────────────────────────────────────────────────
    if (rtlsdr_write_eeprom(dev, want.data(), 0, (uint16_t)want.size()) < 0) {
        std::fprintf(stderr,
            "\nVibeServer: the write failed. The dongle is probably untouched, but if it now\n"
            "            misbehaves, restore it from:  %s\n", backup.c_str());
        rtlsdr_close(dev); return 1;
    }
    rtlsdr_close(dev);

    // ★★ READ IT BACK FROM A FRESH HANDLE. Verifying from the same open handle would be happy to
    //    hand us a cached copy of what we just sent, which proves nothing about the chip.
    dev = nullptr;
    if (rtlsdr_open(&dev, (uint32_t)index) == 0 && dev) {
        uint8_t check[vibe::RTL_EEPROM_SIZE] = {0};
        const bool readOk = rtlsdr_read_eeprom(dev, check, 0, sizeof check) >= 0;
        rtlsdr_close(dev);
        if (readOk && std::memcmp(check, want.data(), want.size()) == 0) {
            // ★★ SAYING THIS MATTERS, because the write looks like it failed otherwise. The
            //    RTL2832U reads its EEPROM at POWER-ON, so the new serial does not appear until
            //    the device loses power. Measured on the Pi 2026-08-07: after a verified write the
            //    chip read back the new serial while USB still reported the old one. A REBOOT
            //    clears it (the Pi drops port power); unbind/bind and toggling `authorized` do
            //    NOT, because neither removes power — so "re-enumerate it in software" is advice
            //    that quietly does nothing. Anyone checking with lsusb in between sees the old
            //    serial and concludes the write silently failed.
            std::printf("\n  Done, and verified by reading it back.\n\n"
                        "  Unplug the dongle and plug it back in — or reboot — for the new serial\n"
                        "  to take effect. Until then it still reports the old one (%s).\n\n",
                        cur.serial.c_str());
            return 0;
        }
        std::fprintf(stderr,
            "\nVibeServer: the dongle did not read back what we wrote.\n"
            "            Restore it with the backup before using it:  %s\n", backup.c_str());
        return 1;
    }
    std::printf("\n  Written. Unplug the dongle and plug it in again, then check the serial.\n"
                "  Backup, if you need it: %s\n\n", backup.c_str());
    return 0;
}

/** True when this process owns the machine's main port — it serves the landing page and
 *  the setup page for every radio, not just its own. */
static bool g_isPrimaryRadio = true;

int main(int argc, char** argv) {
    // ★ Handled before everything else: it never starts a server, and it must work on a machine
    //   whose config is broken or absent — renaming a dongle is often what you do BEFORE setup.
    // ★★ WHAT THE SERVER ACTUALLY SEES, in the order --radio numbers them. Needed the moment
    //    there is more than one radio: the setup screen, the config file and --radio must all
    //    agree about which one is "the second radio", and until now nothing could show you.
    // ★ IDENTITY COMES FROM THREE DIFFERENT PLACES, which is why this prints it rather than
    //   assuming. RTL dongles carry a USB serial (and ship with duplicates — see
    //   --set-rtl-serial); the Airspy HF+ carries one too; but an SDRplay RSP presents NO USB
    //   serial at all and is identified by a serial its own API hands out.
    for (int i = 1; i < argc; i++) {
        // ★★ THE SUPERVISOR ASKS US, rather than parsing JSON in shell. Which radio is primary and
        //    which port each takes are rules with real subtleties (a disabled radio must not move
        //    an earlier one's port), and a second implementation of them in bash would drift from
        //    this one the first time either changed. One serial per line; the primary is NOT
        //    listed, because it is vibeserver.service and starting it twice would fight for a USB
        //    device with itself.
        if (std::string(argv[i]) == "--list-secondary-radios") {
            const char* e = getenv("VIBESERVER_CONFIG");
            const std::string path = (e && *e) ? e : vsconfig::defaultPath();
            vsconfig::ServerConfig srv; std::string err;
            if (!vsconfig::loadServer(path, srv, err)) return 0;   // nothing configured yet
            const int primary = vsconfig::primaryRadio(srv);
            for (size_t k = 0; k < srv.radios.size(); k++) {
                const auto& r = srv.radios[k];
                if ((int)k == primary) continue;
                if (!r.enabled || !r.configured) continue;
                if (r.serial.empty()) continue;   // ★ cannot name a unit after nothing
                std::printf("%s\n", r.serial.c_str());
            }
            return 0;
        }
        if (std::string(argv[i]) == "--list-radios") {
            const auto rs = vibe::detectRadios();
            if (rs.empty()) { std::printf("No radios found.\n"); return 1; }
            std::printf("\n  #  driver    radio\n");
            for (const auto& r : rs)
                std::printf("  %d  %-9s %s%s%s\n", r.index, r.driver.c_str(), r.name.c_str(),
                            r.serial.empty() ? "" : "  (serial ",
                            r.serial.empty() ? "" : (r.serial + ")").c_str());
            if (vibe::serialsCollide(rs))
                std::printf("\n  ! Two radios report the same serial, so they cannot be told apart.\n"
                            "    Give one a new one:  vibeserver --set-rtl-serial <#> <serial>\n");
            std::printf("\n  Pick one with:  vibeserver --radio <#>\n\n");
            return 0;
        }
        if (std::string(argv[i]) == "--set-rtl-serial") {
            if (i + 2 >= argc) {
                std::fprintf(stderr, "usage: vibeserver --set-rtl-serial <dongle-number> <new-serial>\n");
                return 1;
            }
            return setRtlSerial(std::atoi(argv[i + 1]), argv[i + 2]);
        }
    }

    // ★★★ LINE-BUFFER STDOUT, ALWAYS. Under systemd stdout is a PIPE, so libc block-buffers it —
    //     and every status message this server prints sat in a 4 KB buffer until the process
    //     EXITED. `journalctl -u vibeserver` showed nothing while it ran, then dumped the whole
    //     boot's worth of messages at shutdown, timestamped with the moment it died.
    // ★★ That is not a cosmetic problem: it makes the log useless for the one job it has. I lost
    //    time to it reading "no country data yet — downloading" and "country data ready" as the
    //    CURRENT boot re-downloading 50 MB, when both lines were half an hour old and belonged to
    //    the previous process. A log that lies about WHEN is worse than no log.
    // ★ Must be before any printf, hence the first line of main.
    setvbuf(stdout, nullptr, _IOLBF, 0);

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
        // ★★★ READ THE MACHINE, THEN TAKE THIS PROCESS'S RADIO OUT OF IT.
        //
        //     loadServer() understands BOTH file shapes: today's single-radio file becomes a
        //     one-radio machine (enabled and configured, so a working receiver is not taken off
        //     the air by gaining gates it never had), and the new shape is read as written.
        //
        // ★★ WHICH RADIO IS THIS PROCESS? `--radio` decides when given — that is how the
        //    supervisor starts one process per radio. Otherwise take the first that is both
        //    ENABLED and CONFIGURED, which for every existing install is the only one there is.
        // ★★★ WHICH RADIO ARE WE? ASK ARGV FIRST. The config is deliberately read BEFORE the flags
        //     (file supplies defaults, flags override) — but choosing which radio's settings to
        //     take out of the file is not a default, it decides which file entry we are. Without
        //     this pre-scan every process fell back to "the first ready radio" and therefore to
        //     the PRIMARY's port, so radios two and three both tried to bind 48040 and died with
        //     "port already in use" (measured 2026-08-08).
        std::string wantSerial;
        for (int k = 1; k + 1 < argc; k++)
            if (std::string(argv[k]) == "--radio") { wantSerial = argv[k + 1]; break; }

        vsconfig::ServerConfig srv;
        if (vsconfig::loadServer(g_configPath, srv, err)) {
            const vsconfig::RadioConfig* mine = nullptr;
            if (!wantSerial.empty()) {
                for (const auto& r : srv.radios) if (r.serial == wantSerial) { mine = &r; break; }
            }
            if (!mine) {
                for (const auto& r : srv.radios) if (r.enabled && r.configured) { mine = &r; break; }
            }
            // ★ A machine with radios but none ready still runs: it serves the setup page, which
            //   is exactly where the owner goes to make one ready. Refusing to start would leave
            //   them with no way in.
            if (mine) {
                cfg = vsconfig::effectiveFor(srv, *mine);
                applyConfig(cfg, o);
                // ★★ THE PORT COMES FROM THE MACHINE, not from this radio in isolation: which port
                //    a radio answers on depends on whether it is the primary, and that is a
                //    property of the whole list. Computed here rather than stored, so it cannot
                //    drift from what the supervisor and the landing page believe.
                const size_t idx = (size_t)(mine - &srv.radios[0]);
                if (!o.portGiven) o.port = vsconfig::portForRadio(srv, idx);
                g_isPrimaryRadio = ((int)idx == vsconfig::primaryRadio(srv));
                g_myRadioSerial = mine->serial;
            }
            g_serverConfig = srv;
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
            // ★★ THE WHOLE MACHINE, with THIS radio's live values folded in. The page needs every
            //    radio to draw its tabs, but it must also show what the radio in front of you is
            //    ACTUALLY running — those differ the moment anyone passes a flag, and a settings
            //    page showing the file while the radio obeys something else is a page that lies.
            vsconfig::ServerConfig out = g_serverConfig;
            for (auto& r : out.radios) {
                if (r.serial.empty() || r.serial != g_myRadioSerial) continue;
                const auto& c = g_runtimeConfig;
                r.mode = c.mode; r.freq = c.freq; r.rate = c.rate;
                r.lockFreq = c.lockFreq; r.lockRate = c.lockRate;
                r.gain = c.gain; r.lnaState = c.lnaState; r.ifGr = c.ifGr; r.ifAgc = c.ifAgc;
                r.demodMode = c.demodMode; r.landingFreq = c.landingFreq;
                r.users = c.users; r.maxBw = c.maxBw; r.maxFps = c.maxFps; r.fftRate = c.fftRate;
                r.uncompressed = c.uncompressed;
                r.forceIdleSaver = c.forceIdleSaver; r.releaseWhenIdle = c.releaseWhenIdle;
                r.idleGrace = c.idleGrace;
                r.rfNotch = c.rfNotch; r.dabNotch = c.dabNotch; r.zoomSpectrum = c.zoomSpectrum;
                break;
            }
            return vsconfig::toJson(out);
        },
        [](const std::string& json, std::string& err) -> bool {
            // ★★★ RE-READ BEFORE WRITING, ALWAYS. There are now several writers: this process,
            //     every OTHER radio's process (each persists its own gain when an admin changes
            //     it), and the TUI. Writing back a copy loaded at startup would silently revert
            //     whatever another radio had saved in the meantime — the same read-modify-write
            //     race the TUI already learned about, multiplied by the number of radios.
            vsconfig::ServerConfig next;
            std::string ignored;
            if (!vsconfig::loadServer(g_configPath, next, ignored)) next = g_serverConfig;
            if (!vsconfig::fromJson(json, next, err)) return false;

            // ★★ A RESTART IS ASKED FOR, NOT ASSUMED. The setup page saves a tab at a time — and a
            //    tab save that bounced every listener off the OTHER radios would make the page
            //    unusable. "Save and reboot server" sends restart=1; per-tab saves do not, and the
            //    page says the change applies on the next restart.
            bool wantRestart = json.find("\"restart\":true") != std::string::npos
                            || json.find("\"restart\": true") != std::string::npos;
            if (wantRestart) next.configured = true;

            if (!vsconfig::saveServer(g_configPath, next, err)) return false;
            g_serverConfig = next;

            // Keep this process's own view in step, so the page does not show stale values back.
            for (const auto& r : next.radios)
                if (!r.serial.empty() && r.serial == g_myRadioSerial) {
                    g_runtimeConfig = vsconfig::effectiveFor(next, r);
                    break;
                }
            if (next.configured) LocalSdrShim::setConfigured(true);
            if (wantRestart) g_restartRequested.store(true);
            return true;
        });

    // ── Which radios this machine offers ────────────────────────────────────────────────────
    // ★★ ANSWERED FROM THE FILE, re-read each time, because the OTHER radios are separate
    //    processes and this one cannot see their live state. What it can state truthfully is what
    //    the owner configured and which port each answers on — enough for the landing page to
    //    offer them, and honest about being a directory rather than a status board.
    LocalSdrShim::setRadiosHandler([]() -> std::string {
        vsconfig::ServerConfig srv; std::string err;
        if (!vsconfig::loadServer(g_configPath, srv, err)) srv = g_serverConfig;
        const int primary = vsconfig::primaryRadio(srv);
        std::string j = "{\"name\":\"" + jsonEscape(srv.name) + "\",\"radios\":[";
        bool first = true;
        for (size_t i = 0; i < srv.radios.size(); i++) {
            const auto& r = srv.radios[i];
            if (!r.enabled || !r.configured) continue;   // ★ both gates, same as the supervisor
            if (!first) j += ",";
            first = false;
            j += "{\"serial\":\"" + jsonEscape(r.serial) + "\"";
            j += ",\"label\":\"" + jsonEscape(r.label.empty() ? r.driver : r.label) + "\"";
            j += ",\"driver\":\"" + jsonEscape(r.driver) + "\"";
            j += ",\"port\":" + std::to_string(vsconfig::portForRadio(srv, i));
            j += ",\"primary\":" + std::string((int)i == primary ? "true" : "false");
            j += ",\"users\":" + std::to_string(r.users);
            j += ",\"mine\":" + std::string(r.serial == g_myRadioSerial ? "true" : "false");
            // What the listener actually wants to know: where it is pointed.
            const double centre = r.lockFreq > 0 ? r.lockFreq : r.freq;
            j += ",\"centreHz\":" + std::to_string((long long)centre);
            j += ",\"spanHz\":" + std::to_string((long long)(r.lockRate > 0 ? r.lockRate : r.rate));
            j += ",\"mode\":\"" + jsonEscape(r.demodMode) + "\"";
            j += ",\"locked\":" + std::string(r.mode == vsconfig::Mode::LockedRange ? "true" : "false");
            j += "}";
        }
        return j + "]}";
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
    LocalSdrShim::setAdminIdleMinutes(o.adminIdleMin);
    LocalSdrShim::setPublicSharing(o.publicSharing);
    LocalSdrShim::setUpdateSchedule(o.updateSrvHour, o.updateSrvDay,
                                    o.updateAllHour, o.updateAllDay);
    // ★ Linux offers the lot: it has systemd and apt, and a reboot comes back on its own.
    //   macOS and Android must NOT — see setMaintenanceActions in local_sdr_shim.h.
    LocalSdrShim::setMaintenanceActions("restart,reboot,shutdown,update-check,update,update-all");
    LocalSdrShim::setVibeServerForceIdleSaver(o.forceIdleSaver);
    LocalSdrShim::setVibeServerReleaseWhenIdle(o.releaseWhenIdle);
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
                                              "(expected 4 or 6 characters, e.g. IO81jm)\n", o.rxGrid.c_str());
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
    } else if (o.radioGiven) {
        // ★ Resolve a serial to today's index. Done HERE, at start, so a radio that has moved in
        //   the list since the config was written still lands on the right hardware.
        // ★★★ ONE ENUMERATION, ONE ANSWER. Resolving a serial to a flat index and then decomposing
        //     that index against freshly counted per-driver totals is racy: the counts move. With
        //     three radios starting together, the first process claimed the RSP, SDRplay then
        //     reported one device fewer, and the other two were routed into the AIRSPY branch —
        //     both failing with "no Airspy HF+ at that index", including the RTL dongle
        //     (measured 2026-08-08). So take the driver and its own index from the SAME lookup.
        if (!o.radioSerial.empty()) {
            std::string drv; int drvIdx = -1;
            for (const auto& r : vibe::detectRadios())
                if (r.serial == o.radioSerial) { drv = r.driver; drvIdx = r.driverIndex; break; }
            if (drvIdx < 0) {
                std::fprintf(stderr, "VibeServer: no radio with serial %s is attached\n",
                             o.radioSerial.c_str());
                return 1;
            }
            if (drv == "airspyhf")
                port = shim.startAirspyHf(drvIdx, o.freq, o.rate, o.gain,
                                          o.fftSize, o.fftRate, o.mode, err);
            else if (drv == "sdrplay")
                port = shim.startSdrplay(drvIdx, o.freq, o.rate, o.gain,
                                         o.fftSize, o.fftRate, o.mode, err);
            else
                port = shim.start(-(drvIdx + 1), 0, 0, o.freq, o.rate, o.gain,
                                  o.fftSize, o.fftRate, o.mode, err);
            std::printf("VibeServer: using %s %d (serial %s)\n",
                        drv.c_str(), drvIdx, o.radioSerial.c_str());
        } else {
        // ★★★ PICK A RADIO OUT OF THE FLAT LIST — the SAME list vs_device_count() and
        //     vs_device_name() publish, so "the third radio" means the same thing to the setup
        //     screen, the config API and this. Until now `--device N` reached ONLY the dongle
        //     path (its error even says "RTL-SDR index N out of range"), and there was no way at
        //     all to name an RSP or an Airspy: you got whichever the discovery chain preferred.
        //     With three radios on one machine that is not a preference, it is a lottery.
        const int nRtl = (int)rtlsdr_get_device_count();
        const int nRsp = vibe::SdrplaySource::deviceCount();
        if (o.radio >= nRtl + nRsp) {
            std::printf("VibeServer: using Airspy HF+ %d\n", o.radio - nRtl - nRsp);
            port = shim.startAirspyHf(o.radio - nRtl - nRsp, o.freq, o.rate, o.gain,
                                      o.fftSize, o.fftRate, o.mode, err);
        } else if (o.radio >= nRtl) {
            std::printf("VibeServer: using SDRplay RSP %d\n", o.radio - nRtl);
            port = shim.startSdrplay(o.radio - nRtl, o.freq, o.rate, o.gain,
                                     o.fftSize, o.fftRate, o.mode, err);
        } else {
            std::printf("VibeServer: using RTL-SDR %d\n", o.radio);
            port = shim.start(-(o.radio + 1), 0, 0, o.freq, o.rate, o.gain,
                              o.fftSize, o.fftRate, o.mode, err);
        }
        }
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

    // ★★★ STATE THE BIAS-T, EVERY START, WHETHER OR NOT WE WANT IT ON.
    //
    //     A dongle's bias-T is a GPIO that holds its setting for as long as the device has power,
    //     so it outlives the program that set it. A receiver that never states a preference
    //     therefore INHERITS one — and on 2026-08-08 a V4 came up with its red light on and 4.5 V
    //     going out to the antenna, left there by a Windows tool used hours earlier on another
    //     machine. Nothing in VibeServer had ever turned it on, and nothing in VibeServer would
    //     ever have turned it off.
    //
    // ★★ WORSE THAN A WRONG SETTING: it is current out of the same USB budget, and DC into
    //    whatever is connected. The owner could not even SEE it — the server reports whether a
    //    radio HAS a bias-T, never whether it is on.
    // ★ Same principle as starting at minimum gain: assert a known safe state rather than
    //   discovering one. Off unless this radio's config asks for it.
    LocalSdrShim::instance().setBiasTee(g_runtimeConfig.biasT);
    if (g_runtimeConfig.biasT)
        std::printf("  bias-T: ON — this radio is putting DC on its feedline (from your settings)\n");

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
    // ★ Beside the spectrogram, and for the same reason: this is state the SERVER writes and must
    //   keep across a restart. A ban that evaporates on reboot is not a ban — and this Pi reboots.
    LocalSdrShim::instance().setBanListPath("/var/lib/vibeserver/bans.jsonl");
    // ★ Beside the bans, and for the same reason: this is history the owner needs ACROSS
    //   restarts, and an update restarts the server.
    LocalSdrShim::instance().setConnLogPath("/var/lib/vibeserver/connections.jsonl");

    // ── ★★ WHERE LISTENERS ARE CONNECTING FROM ────────────────────────────────────────────────
    // Flags beside the listener list, and the top-countries chart. See vibeserver/geoip.cpp for
    // why this is the RIRs' OWN published allocation data and not a geolocation API: no account,
    // no licence, and — the part that matters — no visitor's address ever leaves this machine.
    geoip::setDir("/var/lib/vibeserver");
    if (!geoip::load())
        std::printf("VibeServer: no country data yet — downloading in the background.\n");
    LocalSdrShim::setGeoIpHandler([](const std::string& ip) { return geoip::lookup(ip); });

    // ── ★★ WHICH NETWORK an address belongs to — for the listener list and for ASN BANS ───────
    // ★★★ THIS IS WHAT MAKES BLOCKING WORK AGAINST ABUSE. A single address is a treadmill: ban
    //     .7 and the next request is .8. Even a /24 loses to a proxy pool spread across a
    //     provider. Blocking the ASN blocks the whole network in one decision — which is exactly
    //     why it also needs care, and why the page names the network you are about to block.
    // ★ NOT from the registry files. Their opaque-id is a per-holder UUID with no name, and their
    //   `asn` records say which AS NUMBERS were allocated to whom — not which AS announces a
    //   given prefix. That only exists in BGP. See asndb.cpp.
    asndb::setDir("/var/lib/vibeserver");
    asndb::load();
    LocalSdrShim::setAsnHandler([](const std::string& ip, uint32_t& asn, std::string& name) {
        return asndb::lookup(ip, asn, name);
    });
    // ★ OFF THE STARTUP PATH. It is ~50 MB across five registries; doing it inline would leave
    //   the radio silent for a minute on every boot, and a receiver that is slow to start reads
    //   as a broken one. Refreshed at most weekly — allocations move slowly.
    // ★★ ONE background thread for BOTH datasets, in sequence. Two threads would have the Pi
    //    downloading and parsing ~90 MB at once while the DSP is trying to keep up — and this
    //    machine has four cores and a radio to run. Sequential costs a minute and disturbs
    //    nothing.
    if (geoip::stale(7) || asndb::stale(7)) {
        std::thread([]{
            std::string err;
            if (geoip::stale(7)) {
                if (geoip::refresh(err))
                    std::printf("VibeServer: country data ready — %d ranges.\n", geoip::count());
                else
                    std::printf("VibeServer: country data unavailable (%s) — listeners will show "
                                "no flag, which is the honest answer.\n", err.c_str());
            }
            if (asndb::stale(7)) {
                if (asndb::refresh(err))
                    std::printf("VibeServer: network data ready — %d ranges.\n", asndb::count());
                else
                    std::printf("VibeServer: network data unavailable (%s) — network blocking "
                                "will match nobody until it downloads.\n", err.c_str());
            }
        }).detach();
    }

    // ── ★★★ THE MAINTENANCE ACTIONS — the four buttons that exist INSTEAD of a web terminal ──
    //
    // See local_sdr_shim.cpp's /vibeserver/admin/ router for the full argument. In short: the
    // need Stuart described ("send a reboot command from my iPhone... loft or up the allotment")
    // is bounded — reboot, restart, update — and a terminal is an unbounded hole for a bounded
    // need, behind a password whose day job is guarding gain sliders, on a receiver that is
    // publicly listed and served over plain HTTP.
    //
    // ★★★ THE COMMANDS ARE LITERALS IN THIS FILE. Not templated, not built from any part of the
    //     request, not read from config. `action` selects WHICH fixed string runs and can do
    //     nothing else — so there is no argument to escape, and no injection to get wrong. The
    //     moment one of these takes a parameter from the network, that property is gone.
    //
    // ★★ FIRE AND FORGET, ON A DETACHED THREAD. `reboot` never returns, and blocking the caller
    //    would leave the admin page waiting on a socket the kernel is about to take away — which
    //    renders as "the reboot failed" for the one action that most obviously worked.
    // ★★ THE OUTPUT OF THE LAST MAINTENANCE ACTION. The helper tees everything it prints into
    //    this file; the admin page polls it so "Install updates" shows apt working rather than a
    //    button that goes silent for two minutes.
    LocalSdrShim::setAdminLogHandler([](std::string& text, bool& running, int& exitCode) {
        text.clear(); running = false; exitCode = 0;
        FILE* f = fopen("/var/lib/vibeserver/maintenance.log", "rb");
        if (!f) return;
        // ★ Tail, not the whole file: an apt run can print a lot, and this is polled every
        //   second. The page only ever shows the end of it anyway.
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        const long kMax = 16 * 1024;
        if (len > kMax) { fseek(f, len - kMax, SEEK_SET); len = kMax; } else fseek(f, 0, SEEK_SET);
        text.resize((size_t)len);
        const size_t got = fread(&text[0], 1, (size_t)len, f);
        text.resize(got);
        fclose(f);
        // ★★ RUNNING IS THE ABSENCE OF THE END MARKER, which the helper writes from an EXIT trap
        //    so it appears even when the action fails. Deciding this any other way (asking
        //    systemd, timing out) would leave a crashed action looking like a busy one.
        const std::string kDone = "__VIBESERVER_DONE__";
        const size_t at = text.rfind(kDone);
        if (at == std::string::npos) { running = !text.empty(); return; }
        exitCode = atoi(text.c_str() + at + kDone.size());
        text.erase(at);                       // the marker is plumbing, not something to show
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    });

    LocalSdrShim::setAdminActionHandler([](const std::string& action, std::string& err) -> bool {
        // ★★★ WRITE A REQUEST; DO NOT TRY TO BECOME ROOT. The unit sets NoNewPrivileges=yes, so
        //     sudo can never work here however correct the sudoers file is — sudo says so itself:
        //     "The 'no new privileges' flag is set, which prevents sudo from running as root."
        //     The choice was to weaken the sandbox or to stop asking the daemon to elevate. This
        //     is the second: systemd watches for this file (vibeserver-maintenance.path) and runs
        //     a ROOT helper that owns the list of permitted actions.
        //     ★★ It is also a tighter boundary than sudo was. The whitelist now lives in a
        //        root-owned script this process cannot write to, so a compromised daemon can ask
        //        for one of five things by name and nothing else. Same argument as refusing a web
        //        terminal, one layer down.
        //     ★ Validated HERE TOO, so a typo never reaches the helper and the admin page gets an
        //       immediate answer rather than silence.
        static const char* kActions[] = { "reboot", "shutdown", "restart",
                                          "update-check", "update", "update-all" };
        bool known = false;
        for (const char* a : kActions) if (action == a) { known = true; break; }
        if (!known) { err = "unknown action: " + action; return false; }

        // ★ The helper is what makes any of this possible; if it is not installed, say that
        //   plainly instead of writing a request nothing will ever read.
        if (::access("/usr/lib/vibeserver/vibeserver-maintenance", X_OK) != 0) {
            err = "maintenance actions are not available on this server "
                  "(the helper is not installed — reinstalling the package restores it)";
            return false;
        }

        const std::string path = "/var/lib/vibeserver/maintenance.request";
        // ★★ Write to a temp file and rename. systemd's PathExists= fires the moment the name
        //    appears, so a partially-written file could be read before the action is complete —
        //    rename is atomic and cannot be seen half-done.
        const std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) {
            err = "could not write the maintenance request (is /var/lib/vibeserver writable?)";
            return false;
        }
        fputs(action.c_str(), f);
        fclose(f);
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            remove(tmp.c_str());
            err = "could not submit the maintenance request";
            return false;
        }
        std::printf("VibeServer: maintenance '%s' requested\n", action.c_str());
        return true;
    });

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // ★ The DSP path only raises a FLAG; the write happens here, off that thread.
        LocalSdrShim::instance().saveSpectrogramIfDue();
        LocalSdrShim::instance().saveConnLogIfDue();

        // ── ★★★ SCHEDULED UPDATES ────────────────────────────────────────────────────────
        // ★★ Two independent schedules. They ask the SAME helper the admin page's buttons ask,
        //    by writing the same request file — one path to privilege, one action whitelist.
        // ★★★ THE GUARD IS THE DATE OF THE LAST RUN, not a countdown. A countdown restarts with
        //     the process, and this process is restarted BY the very update it performs — which
        //     would loop.
        {
            static int lastSrvYday = -1, lastAllYday = -1;
            const std::time_t now = std::time(nullptr);
            std::tm lt{};
            localtime_r(&now, &lt);
            auto due = [&](int hour, int day, int& lastYday) {
                if (hour < 0) return false;
                if (day >= 0 && lt.tm_wday != day) return false;
                if (lt.tm_hour != hour || lastYday == lt.tm_yday) return false;
                lastYday = lt.tm_yday;
                return true;
            };
            // ★ VibeServer first when both land on the same hour: it is the small, low-risk one,
            //   and if the system upgrade then wants a reboot we have already taken ours.
            std::string err;
            if (due(o.updateSrvHour, o.updateSrvDay, lastSrvYday)) {
                if (LocalSdrShim::instance().adminAction("update", err))
                     std::printf("VibeServer: scheduled VibeServer update started\n");
                else std::printf("VibeServer: scheduled update failed to start — %s\n", err.c_str());
            }
            if (due(o.updateAllHour, o.updateAllDay, lastAllYday)) {
                if (LocalSdrShim::instance().adminAction("update-all", err))
                     std::printf("VibeServer: scheduled system update started\n");
                else std::printf("VibeServer: scheduled system update failed to start — %s\n", err.c_str());
            }
        }
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
