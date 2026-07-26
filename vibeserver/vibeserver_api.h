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
    // Serve RAW audio to a client that cannot decode Opus? ~187 KB/s each, ~20x
    // the compressed stream, out of the OWNER's uplink. Default (zero-init) = false.
    bool   allowUncompressedAudio;
    /** ★ RECEIVER LOCATION as the JSON served at GET /location:
     *    {"name":"…","iso":"GB","lat":52.24,"lon":-0.90,"label":"Northampton","grid":"IO92ng"}
     *  NULL/"" = unknown, and everything that depends on it degrades honestly rather than
     *  guessing. It is the SERVER's position, never the listener's: distances, map centring
     *  and the ITU REGION (80m is 3.5-3.8 in R1 but 3.5-4.0 in R2) all follow the ANTENNA.
     *  ★ It also decides the RDS country. The flag logic refuses to invent one — it either
     *  has an Extended Country Code from the air, or it validates the PI's country nibble
     *  against the RECEIVER's country — so with no location set, every station's country
     *  and flag stayed blank (found on air 2026-07-26). */
    const char* locationJson;
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
    bool   deviceLost;     // radio unplugged or failed; server still up, nothing to serve
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

/**
 * Hand the web client its searchable station list (GET /stations) — the EiBi schedule as a JSON
 * array of {name, frequency, mode, group, comment, flag, itu, source}.
 *
 * ★ The SERVER must supply this because a browser cannot fetch eibispace.de itself (no CORS
 * headers) — served same-origin from the shim, the problem disappears. On the phone the app owned
 * the download; a standalone VibeServer has no app, so the host GUI downloads and calls this. Pass
 * "" or "[]" to clear. Held until replaced; survives start/stop.
 */
void vs_set_stations(const char* json);

#ifdef __cplusplus
}
#endif
#endif  // VIBESERVER_API_H
