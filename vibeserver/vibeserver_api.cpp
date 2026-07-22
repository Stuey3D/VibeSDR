// Implementation of the flat C API — a thin translation layer over LocalSdrShim, no logic of its
// own. Anything that looks like a decision belongs in the shim, where every host shares it.
#include "vibeserver_api.h"
#include "local_sdr_shim.h"

#ifdef VIBE_HAVE_LIBRTLSDR
#include <rtl-sdr.h>
#endif

#include <cstring>
#include <string>

using vibe::LocalSdrShim;

namespace {
int g_port = 0;                 // the port we actually bound, for vs_status
std::string g_deviceName;       // backing store for vs_device_name's return

void copyStr(char* dst, int cap, const std::string& src) {
    if (!dst || cap <= 0) return;
    const int n = (int)src.size() < cap - 1 ? (int)src.size() : cap - 1;
    std::memcpy(dst, src.data(), (size_t)n);
    dst[n] = '\0';
}
}  // namespace

void vs_default_config(VsConfig* cfg) {
    if (!cfg) return;
    *cfg = VsConfig{};
    cfg->deviceIndex    = 0;
    cfg->centreHz       = 96'600'000;   // an FM station is the friendliest first-run default:
    cfg->sampleRate     = 2'400'000;    // something audible immediately, so a new user knows it works
    cfg->gainTenthDb    = -1;           // automatic
    cfg->fftSize        = 4096;
    cfg->fftRate        = 15;
    cfg->mode           = "wfm";
    cfg->pin            = "";
    cfg->port           = 0;
    cfg->maxBandwidthHz = 0;
    cfg->maxFftRate     = 0;
    cfg->lockedRate     = 0;
    cfg->serveWebClient = true;
    cfg->forceIdleSaver = false;
}

int vs_start(const VsConfig* cfg, char* errOut, int errCap) {
    if (!cfg) { copyStr(errOut, errCap, "no configuration given"); return -1; }

    // Policy first — the shim reads all of this once, at start.
    LocalSdrShim::setServeOnLan(true);
    LocalSdrShim::setVibeServerPort(cfg->port);
    LocalSdrShim::setVibeServerAuth(cfg->pin ? cfg->pin : "");
    LocalSdrShim::setVibeServerLimits(cfg->maxBandwidthHz, cfg->maxFftRate);
    LocalSdrShim::setVibeServerLockedRate(cfg->lockedRate);
    LocalSdrShim::setVibeServerWebEnabled(cfg->serveWebClient);
    LocalSdrShim::setVibeServerForceIdleSaver(cfg->forceIdleSaver);

    std::string err;
    // Negative fd = "open by device index" on desktop — see local_sdr_shim.cpp.
    const int port = LocalSdrShim::instance().start(
        -(cfg->deviceIndex + 1), 0, 0,
        cfg->centreHz, cfg->sampleRate, cfg->gainTenthDb,
        cfg->fftSize, cfg->fftRate, cfg->mode ? cfg->mode : "wfm", err);

    if (port <= 0) { copyStr(errOut, errCap, err.empty() ? "could not start" : err); g_port = 0; return -1; }
    g_port = port;
    return port;
}

void vs_stop(void) {
    LocalSdrShim::instance().stop();
    g_port = 0;
}

bool vs_is_running(void) { return LocalSdrShim::instance().isRunning(); }

void vs_status(VsStatus* out) {
    if (!out) return;
    *out = VsStatus{};
    const auto s = LocalSdrShim::instance().getVibeServerStatus();
    out->running          = LocalSdrShim::instance().isRunning();
    out->clientConnected  = s.clientConnected;
    copyStr(out->clientAddr, (int)sizeof(out->clientAddr), s.clientAddr);
    out->specBytesPerSec  = s.specBytesPerSec;
    out->audioBytesPerSec = s.audioBytesPerSec;
    out->fftRate          = s.fftRate;
    out->bandwidthHz      = s.bandwidthHz;
    out->sampleRate       = s.sampleRate;
    out->pinEnabled       = s.pinEnabled;
    out->port             = g_port;
}

int vs_device_count(void) {
#ifdef VIBE_HAVE_LIBRTLSDR
    return (int)rtlsdr_get_device_count();
#else
    return 0;
#endif
}

const char* vs_device_name(int index) {
#ifdef VIBE_HAVE_LIBRTLSDR
    if (index < 0 || (uint32_t)index >= rtlsdr_get_device_count()) { g_deviceName.clear(); return ""; }
    const char* n = rtlsdr_get_device_name((uint32_t)index);
    g_deviceName = n ? n : "";
    return g_deviceName.c_str();
#else
    (void)index; return "";
#endif
}
