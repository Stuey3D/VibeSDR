// A flat C API over the C++ core, so Swift can drive it without C++ interop.
//
// Swift can import C headers directly and cheaply; C++ interop is still fiddly (name mangling,
// std::string, exceptions). One narrow C surface keeps the GUI ignorant of the core's internals
// and — more importantly — keeps the core ignorant of the GUI. The CLI, the menu-bar app and a
// future Pi daemon all call THIS, so behaviour cannot drift between them.
//
// Nothing here is macOS-specific.
#ifndef VIBESERVER_API_H
#define VIBESERVER_API_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Everything set before a start. Zero-initialise, then override what you care about. */
typedef struct {
    int    deviceIndex;    // RTL-SDR index (0 = first)
    double centreHz;       // initial centre frequency
    double sampleRate;     // capture rate, Hz
    int    gainTenthDb;    // < 0 = automatic
    int    fftSize;
    double fftRate;        // spectrum frames per second
    const char* mode;      // "am" | "lsb" | "usb" | "nfm" | "wfm" | "cw…"
    const char* pin;       // NULL/"" = open access. The host machine is never asked (loopback).
    int    port;           // 0 = first free in 48000-48049; otherwise this port or fail
    double maxBandwidthHz; // 0 = uncapped   ─┐ operator ceilings
    double maxFftRate;     // 0 = default    ─┘
    double lockedRate;     // 0 = client may change the capture rate
    bool   serveWebClient; // serve the browser client at GET /
    bool   forceIdleSaver; // listeners may NOT switch off idle power-saving (solar/cellular hosts)
} VsConfig;

/** Live status for a status view. Deliberately the same numbers the CLI prints. */
typedef struct {
    bool   running;
    bool   clientConnected;
    char   clientAddr[64];
    double specBytesPerSec;
    double audioBytesPerSec;
    double fftRate;
    double bandwidthHz;
    double sampleRate;
    bool   pinEnabled;
    int    port;           // the port actually bound
} VsStatus;

/** Fill `cfg` with the defaults the CLI uses, so a caller overrides only what it means to. */
void vs_default_config(VsConfig* cfg);

/**
 * Start serving. Returns the bound port (> 0), or -1 with a human-readable reason written to
 * `errOut` — a message fit to show a user, not a code to look up.
 */
int  vs_start(const VsConfig* cfg, char* errOut, int errCap);

/** Stop serving and release the radio. Safe to call when not running. */
void vs_stop(void);

bool vs_is_running(void);

/** Snapshot the current status. Cheap enough to poll once a second. */
void vs_status(VsStatus* out);

/** How many RTL-SDR devices are attached right now (for a device picker / "plug one in" state). */
/** Ask the connected client to make itself known (flash + focus). No-op if nobody is listening. */
void vs_summon(void);

int  vs_device_count(void);

/** Display name of device `index`, or "" if there is no such device. Valid until the next call. */
const char* vs_device_name(int index);

#ifdef __cplusplus
}
#endif
#endif  // VIBESERVER_API_H
