// Implementation of the flat C API — a thin translation layer over LocalSdrShim, no logic of its
// own. Anything that looks like a decision belongs in the shim, where every host shares it.
#include "vibeserver_api.h"
#include "local_sdr_shim.h"
#include "sdrplay_source.h"
#include "airspyhf_source.h"

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

// ★ DEVICES ARE ONE FLAT LIST: dongles first, then any SDRplay RSPs. The operator picks a
// receiver, not a driver — which of the two APIs it happens to speak is our problem, not
// theirs. Indices above the dongle count route to the RSP path (2026-07-26).
static int rtlCount() {
#ifdef VIBE_HAVE_LIBRTLSDR
    return (int)rtlsdr_get_device_count();
#else
    return 0;
#endif
}

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
    cfg->adminPassword  = "";
    cfg->sessionLimitMin = 0;
    cfg->port           = 0;
    cfg->maxBandwidthHz = 0;
    cfg->maxFftRate     = 0;
    cfg->lockedRate     = 0;
    cfg->serveWebClient = true;
    cfg->forceIdleSaver = false;
    cfg->uncompressedAudio = VS_UNCOMP_OFF;
}

int vs_start(const VsConfig* cfg, char* errOut, int errCap) {
    if (!cfg) { copyStr(errOut, errCap, "no configuration given"); return -1; }

    // Policy first — the shim reads all of this once, at start.
    LocalSdrShim::setServeOnLan(true);
    LocalSdrShim::setVibeServerPort(cfg->port);
    LocalSdrShim::setVibeServerAuth(cfg->pin ? cfg->pin : "");
    LocalSdrShim::setVibeServerAdminSecret(cfg->adminPassword ? cfg->adminPassword : "");
    LocalSdrShim::setVibeServerSessionLimit(cfg->sessionLimitMin);
    LocalSdrShim::setVibeServerLimits(cfg->maxBandwidthHz, cfg->maxFftRate);
    LocalSdrShim::setVibeServerLockedRate(cfg->lockedRate);
    LocalSdrShim::setVibeServerWebEnabled(cfg->serveWebClient);
    LocalSdrShim::setVibeServerForceIdleSaver(cfg->forceIdleSaver);
    LocalSdrShim::setVibeServerUncompressedAudio(cfg->uncompressedAudio);
    if (cfg->locationJson && *cfg->locationJson)
        LocalSdrShim::setLocationJson(cfg->locationJson);

    std::string err;
    // ★ Route to whichever driver owns this index. See vs_device_count for the flat list.
    const int nRtl = rtlCount();
    const int nRsp = vibe::SdrplaySource::deviceCount();
    if (cfg->deviceIndex >= nRtl + nRsp) {
        const int port3 = LocalSdrShim::instance().startAirspyHf(
            cfg->deviceIndex - nRtl - nRsp, cfg->centreHz, cfg->sampleRate, cfg->gainTenthDb,
            cfg->fftSize, cfg->fftRate, cfg->mode ? cfg->mode : "wfm", err);
        if (port3 <= 0) { copyStr(errOut, errCap, err.empty() ? "could not start" : err); g_port = 0; return -1; }
        g_port = port3;
        return port3;
    }
    if (cfg->deviceIndex >= nRtl) {
        const int port2 = LocalSdrShim::instance().startSdrplay(
            cfg->deviceIndex - nRtl, cfg->centreHz, cfg->sampleRate, cfg->gainTenthDb,
            cfg->fftSize, cfg->fftRate, cfg->mode ? cfg->mode : "wfm", err);
        if (port2 <= 0) { copyStr(errOut, errCap, err.empty() ? "could not start" : err); g_port = 0; return -1; }
        g_port = port2;
        return port2;
    }
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
    out->deviceLost       = s.deviceLost;
    out->port             = g_port;
}

void vs_summon(void) { LocalSdrShim::instance().summonClient(); }

void vs_set_stations(const char* json) {
    LocalSdrShim::instance().setStationsJson(json ? json : "");
}

void vs_sdrplay_retry(void) { vibe::SdrplaySource::retryApi(); }

int vs_sdrplay_api_stuck(void) { return vibe::SdrplaySource::apiUnresponsive() ? 1 : 0; }

// ★ ONE FLAT LIST, now three drivers deep: dongles, then RSPs, then Airspy HF+. The operator
// picks a RECEIVER; which of three APIs it happens to speak is our problem. Order is fixed so
// an index means the same thing on the next launch.
int vs_device_count(void) {
    return rtlCount() + vibe::SdrplaySource::deviceCount()
                      + vibe::AirspyHfSource::deviceCount();
}

const char* vs_device_name(int index) {
    const int nRtl = rtlCount();
    const int nRsp = vibe::SdrplaySource::deviceCount();
    if (index >= nRtl + nRsp) {
        g_deviceName = vibe::AirspyHfSource::deviceName(index - nRtl - nRsp);
        return g_deviceName.c_str();
    }
    if (index >= nRtl) {
        g_deviceName = vibe::SdrplaySource::deviceName(index - nRtl);
        return g_deviceName.c_str();
    }
#ifdef VIBE_HAVE_LIBRTLSDR
    if (index < 0 || (uint32_t)index >= rtlsdr_get_device_count()) { g_deviceName.clear(); return ""; }
    // ★ THE USB DESCRIPTOR, NOT librtlsdr's GUESS. rtlsdr_get_device_name() reports the TUNER
    // chip's generic name — "Generic RTL2832U OEM" — which tells a user nothing and is identical
    // across wildly different dongles. The USB strings carry what is written on the box:
    // manufacturer "RTLSDRBlog", product "Blog V4". Knowing it is a V4 rather than a V3 is the
    // difference between a setup a user can complete and one they have to guess at, because the
    // two need OPPOSITE HF settings (see the profiles section of the multi-radio brief).
    char mfr[256] = {0}, prd[256] = {0}, ser[256] = {0};
    if (rtlsdr_get_device_usb_strings((uint32_t)index, mfr, prd, ser) == 0 && prd[0]) {
        g_deviceName = mfr[0] ? (std::string(mfr) + " " + prd) : std::string(prd);
        return g_deviceName.c_str();
    }
    const char* n = rtlsdr_get_device_name((uint32_t)index);   // fallback: better than nothing
    g_deviceName = n ? n : "";
    return g_deviceName.c_str();
#else
    (void)index; return "";
#endif
}
