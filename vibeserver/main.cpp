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
#include <fstream>
#include <unistd.h>
#include "airspyhf_source.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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
        else { std::fprintf(stderr, "unknown option: %s\n\n", a.c_str()); usage(); std::exit(2); }
    }
    return true;
}

}  // namespace

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
    Opts o;
    if (!parse(argc, argv, o)) return 0;

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
        std::string j; auto add = [&](const std::string& k, const std::string& v, bool quote) {
            if (v.empty()) return;
            if (!j.empty()) j += ",";
            j += "\"" + k + "\":" + (quote ? "\"" + v + "\"" : v);
        };
        add("name",  o.rxName,  true);
        add("iso",   o.rxIso,   true);
        add("label", o.rxPlace, true);
        add("lat",   o.rxLat,   false);
        add("lon",   o.rxLon,   false);
        add("grid",  o.rxGrid,  true);
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
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!shim.isRunning()) {
            std::fprintf(stderr, "VibeServer: capture stopped unexpectedly.\n");
            break;
        }
    }

    std::printf("\nStopping…\n");
    shim.stop();
    return 0;
}
