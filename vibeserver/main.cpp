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
    std::string tcpHost = "127.0.0.1";   // rtl_tcp source
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
    bool        web     = true;
};

void usage() {
    std::printf(
        "VibeServer (standalone)\n\n"
        "  --tcp HOST:PORT   rtl_tcp IQ source            (default 127.0.0.1:1234)\n"
        "  --freq HZ         initial centre frequency     (default 9410000)\n"
        "  --rate HZ         capture sample rate          (default 2400000)\n"
        "  --gain TENTHS_DB  tuner gain, <0 = auto        (default auto)\n"
        "  --mode MODE       am|lsb|usb|nfm|wfm|cw        (default am)\n"
        "  --fft N           FFT size                     (default 4096)\n"
        "  --fps N           spectrum frames/sec          (default 15)\n"
        "  --pin SECRET      require the PIN/HMAC handshake (default: open)\n"
        "  --max-bw HZ       server-enforced bandwidth ceiling\n"
        "  --max-fps N       server-enforced spectrum-rate ceiling\n"
        "  --lock-rate HZ    pin the capture rate (clients cannot change it)\n"
        "  --no-web          do not serve the browser client at GET /\n"
        "  -h, --help\n\n"
        "IQ comes from rtl_tcp — there is no USB path in this build (see the file header).\n"
        "Start one first, e.g.:  rtl_tcp -a 127.0.0.1 -f 9410000 -s 2400000\n");
}

bool parse(int argc, char** argv, Opts& o) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", argv[i]); std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return false; }
        else if (a == "--tcp") {
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
        else if (a == "--pin")       o.pin      = need(i);
        else if (a == "--max-bw")    o.maxBw    = std::atof(need(i));
        else if (a == "--max-fps")   o.maxFps   = std::atof(need(i));
        else if (a == "--lock-rate") o.lockRate = std::atof(need(i));
        else if (a == "--no-web")    o.web      = false;
        else { std::fprintf(stderr, "unknown option: %s\n\n", a.c_str()); usage(); std::exit(2); }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
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
    LocalSdrShim::setVibeServerAuth(o.pin);     // empty = open
    LocalSdrShim::setVibeServerLimits(o.maxBw, o.maxFps);
    LocalSdrShim::setVibeServerLockedRate(o.lockRate);
    LocalSdrShim::setVibeServerWebEnabled(o.web);

    // The shim is a singleton — one radio, one pipeline, one server per process.
    LocalSdrShim& shim = LocalSdrShim::instance();
    std::string err;
    const int port = shim.startTcp(o.tcpHost, o.tcpPort, o.freq, o.rate, o.gain,
                                   o.fftSize, o.fftRate, o.mode, err);
    if (port <= 0) {
        std::fprintf(stderr, "VibeServer: failed to start — %s\n",
                     err.empty() ? "(no reason given)" : err.c_str());
        std::fprintf(stderr, "Is there an rtl_tcp listening on %s:%d?\n", o.tcpHost.c_str(), o.tcpPort);
        return 1;
    }

    std::printf("VibeServer listening on port %d  (IQ from rtl_tcp %s:%d)\n",
                port, o.tcpHost.c_str(), o.tcpPort);
    std::printf("  clients: http://<this-machine>:%d/    auth: %s\n",
                port, o.pin.empty() ? "OPEN (no PIN)" : "PIN required");
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
