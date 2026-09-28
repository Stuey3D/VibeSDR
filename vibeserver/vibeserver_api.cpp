// Implementation of the flat C API — a thin translation layer over LocalSdrShim, no logic of its
// own. Anything that looks like a decision belongs in the shim, where every host shares it.
#include "vibeserver_api.h"
#include "local_sdr_shim.h"
#include "geoip.h"
#include "asndb.h"
#include <sys/stat.h>
#include <thread>
#include "sdrplay_source.h"
#include "airspyhf_source.h"
#include "hackrf_source.h"
#include "airspy_source.h"
#include "radios.h"
#include "directory.h"
#include "mapgl_curl.h"

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
    LocalSdrShim::setVibeServerUncompressedAudio(cfg->uncompressedAudio);
    if (cfg->locationJson && *cfg->locationJson)
        LocalSdrShim::setLocationJson(cfg->locationJson);

    // ★★★ A GUI APP IS CONFIGURED BY DEFINITION — its settings pane IS the setup.
    //
    // `configured` defaults to FALSE and only the Linux daemon ever set it, so on macOS the shim
    // believed it had never been set up and served the browser SETUP WIZARD at GET / instead of
    // the receiver. Pressing "Start serving" therefore opened a page demanding the admin password
    // before anything would work — on an app whose whole promise is plug a radio in and press
    // start (Stuart, 2026-08-07: "simple mode is broken").
    //
    // ★★ The setup page exists for a HEADLESS Linux box, where a browser is the only way in and
    //    the server genuinely has a radio and a password but no policy yet. A Mac has a window.
    //    There is no unconfigured state to be in, so there is nothing for that page to ask.
    // ★ Not gated on Simple/Full: even in Full the questions are answered in the GUI, not a
    //   browser wizard.
    // ★★★ WHICH SURFACE OWNS SETUP, and it depends on the mode.
    //
    //   SIMPLE — the GUI owns it. A handful of controls in the app's own window, and the browser
    //     wizard is never served: plug a radio in, press start. Two ways to configure one server
    //     is how they drift apart, so in Simple there is exactly one.
    //   FULL — the WEB PAGE owns it. The full option set already exists there, is already
    //     maintained, and is already shared by Linux, macOS and Android. Re-implementing it in
    //     SwiftUI and again in Kotlin would be three copies of one set of rules — and the GUI is
    //     a thin strip that is a lot to scroll before we add anything (Stuart, 2026-08-07).
    //
    // ★ `configured` is always true on a GUI host either way: there is no "never been set up"
    //   state to be in when the app has a window. What changes is where the DETAIL lives.
    LocalSdrShim::setConfigured(true);
    LocalSdrShim::setNativeSetup(!cfg->fullMode);

    // ── ★★★ THE ADMIN FEATURES ON macOS ──────────────────────────────────────────────────────
    // The admin page, the ban list, the connection log and the idle re-lock all live in the
    // SHARED shim, so they arrive here by recompiling. What has to be registered per platform is
    // where state is kept and what the machine can actually do.
    {
        // ★ Application Support, not /var/lib: a Mac app must keep its state where the OS expects
        //   it, and a sandboxed or notarised app cannot write outside its container anyway.
        std::string dir;
        if (const char* home = getenv("HOME")) {
            dir = std::string(home) + "/Library/Application Support/VibeServer";
            // 0700: the ban list is policy and the connection log holds visitors' addresses.
            ::mkdir((std::string(home) + "/Library/Application Support").c_str(), 0755);
            ::mkdir(dir.c_str(), 0700);
        }
        if (!dir.empty()) {
            LocalSdrShim::instance().setBanListPath(dir + "/bans.jsonl");
            LocalSdrShim::instance().setConnLogPath(dir + "/connections.jsonl");
            LocalSdrShim::instance().setSpectrogramPath(dir + "/spectrogram.bin");
            geoip::setDir(dir);
            asndb::setDir(dir);
            // ★ The GPU map's High Detail pack: where it lives and how it is fetched — Simple mode had
            //   neither, so its admin page could only say the download was unavailable (2026-09-28).
            vibemapgl::setDataDir(dir + "/mapgl");
            vibemapgl::installCurlDownloader();
        }
        geoip::load();
        asndb::load();
        LocalSdrShim::setGeoIpHandler([](const std::string& ip) { return geoip::lookup(ip); });
        LocalSdrShim::setAsnHandler([](const std::string& ip, uint32_t& asn, std::string& name) {
            return asndb::lookup(ip, asn, name);
        });
        // ★ One background thread for both, off the startup path — the same reasoning as the
        //   daemon: ~90 MB of downloading and parsing must not delay the radio coming up.
        if (geoip::stale(7) || asndb::stale(7)) {
            std::thread([]{
                std::string e;
                if (geoip::stale(7)) geoip::refresh(e);
                if (asndb::stale(7)) asndb::refresh(e);
            }).detach();
        }

        // ★★★ NO MAINTENANCE ACTIONS ON macOS, AND THAT IS DELIBERATE (Stuart, 2026-08-07):
        //     a reboot stops at the FileVault login and needs someone PHYSICALLY PRESENT to
        //     continue, so a remote reboot would take the receiver off the air until somebody
        //     walks to it. There is no apt to update through either — a Mac app updates itself.
        //     ★ Empty means the admin page draws no maintenance section at all, rather than
        //       buttons that would strand the machine.
        LocalSdrShim::setMaintenanceActions("");
    }

    std::string err;
    // ★ Route to whichever driver owns this index. See vs_device_count for the flat list.
    const int nRtl = rtlCount();
    const int nRsp = vibe::SdrplaySource::deviceCount();
    const int nAhf = vibe::AirspyHfSource::deviceCount();
    const int nHrf = vibe::HackRfSource::deviceCount();
    const char* mode = cfg->mode ? cfg->mode : "wfm";
    /* ★★★ THE SAME ORDER AS detectRadios(): RTL, RSP, HF+, HackRF, Airspy R2/Mini. Until
     *     2026-09-28 this stopped at the HF+, so on a Mac a HackRF or an R2 was not in the list at
     *     all — and anything past the HF+ range would have been handed to startAirspyHf with an
     *     index it does not have. Each branch names its driver; nothing is "the rest". */
    if (cfg->deviceIndex >= nRtl + nRsp + nAhf + nHrf) {
        const int p = LocalSdrShim::instance().startAirspy(
            cfg->deviceIndex - nRtl - nRsp - nAhf - nHrf, cfg->centreHz, cfg->sampleRate,
            cfg->gainTenthDb, cfg->fftSize, cfg->fftRate, mode, err);
        if (p <= 0) { copyStr(errOut, errCap, err.empty() ? "could not start" : err); g_port = 0; return -1; }
        g_port = p;
        return p;
    }
    if (cfg->deviceIndex >= nRtl + nRsp + nAhf) {
        const int p = LocalSdrShim::instance().startHackRf(
            cfg->deviceIndex - nRtl - nRsp - nAhf, cfg->centreHz, cfg->sampleRate,
            cfg->gainTenthDb, cfg->fftSize, cfg->fftRate, mode, err);
        if (p <= 0) { copyStr(errOut, errCap, err.empty() ? "could not start" : err); g_port = 0; return -1; }
        g_port = p;
        return p;
    }
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

/* ── The public directory ─────────────────────────────────────────────────────────────────────
 * ★ A translation layer and nothing more, as the note at the top of this file requires: every
 *   decision about WHEN to list, for how long, and what to call it belongs to the host's UI, and
 *   every decision about HOW to list belongs to vibedir. */
void vs_directory_state_dir(const char* dir) {
    vibedir::setStateDir(dir ? dir : "");
}

void vs_directory_apply(bool listed, const char* name, const char* locator,
                        const char* publicUrl, long long shareForSec) {
    vibedir::Settings d;
    d.listed      = listed;
    d.name        = name ? name : "";
    d.locator     = locator ? locator : "";
    d.publicUrl   = publicUrl ? publicUrl : "";
    d.shareForSec = shareForSec;
    // ★★★ THE PORT WE ACTUALLY BOUND. Not the one that was asked for — vs_start falls through to
    //     the next free port in the range, so the requested number and the real one differ often
    //     enough to matter. A listing that names a port nobody is listening on is the one failure
    //     that looks completely healthy from this side: the entry appears, the map pin lands, and
    //     every visitor gets a connection refused. main.cpp carries the same warning about mDNS.
    d.port        = g_port;
    vibedir::apply(d);
}

const char* vs_directory_status(void) {
    // ★ Held in a static so the caller has something with a lifetime; overwritten on each call,
    //   which is what the header promises.
    static std::string s_status;
    s_status = vibedir::statusJson();
    return s_status.c_str();
}

void vs_sdrplay_retry(void) { vibe::SdrplaySource::retryApi(); }

int vs_sdrplay_api_stuck(void) { return vibe::SdrplaySource::apiUnresponsive() ? 1 : 0; }

// ★ ONE FLAT LIST, now five drivers deep: dongles, then RSPs, then Airspy HF+, then HackRF, then
// Airspy R2/Mini — the order detectRadios() uses, so the Mac app, the TUI and `--radio N` all mean
// the same receiver by the same number. The operator picks a RECEIVER; which API it happens to
// speak is our problem. Order is fixed so an index means the same thing on the next launch.
int vs_device_count(void) {
    return rtlCount() + vibe::SdrplaySource::deviceCount()
                      + vibe::AirspyHfSource::deviceCount()
                      + vibe::HackRfSource::deviceCount()
                      + vibe::AirspySource::deviceCount();
}

const char* vs_device_name(int index) {
    const int nRtl = rtlCount();
    const int nRsp = vibe::SdrplaySource::deviceCount();
    const int nAhf = vibe::AirspyHfSource::deviceCount();
    const int nHrf = vibe::HackRfSource::deviceCount();
    if (index >= nRtl + nRsp + nAhf + nHrf) {
        g_deviceName = vibe::AirspySource::deviceName(index - nRtl - nRsp - nAhf - nHrf);
        return g_deviceName.c_str();
    }
    if (index >= nRtl + nRsp + nAhf) {
        g_deviceName = vibe::HackRfSource::deviceName(index - nRtl - nRsp - nAhf);
        return g_deviceName.c_str();
    }
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

// ── Full mode's radio list: identity, not just a name ────────────────────────
//
// ★★★ THE FRONT DOOR FORKS BY SERIAL, so a host writing a `radios[]` config needs serials. A flat
//     index is not an identity: unplug one dongle and every index below it moves, which would
//     silently point a radio's saved settings at different hardware.
//
// ★★ ONE ENUMERATION, ONE ANSWER. Every accessor below reads this ONE cached pass rather than
//    re-detecting per call. Re-detecting per call would let the list move BETWEEN the serial
//    lookup and the driver lookup — the exact shape of the 2026-08-08 bug where three radios
//    starting at once sent two of themselves into the Airspy branch. It is also much cheaper:
//    probing the SDRplay API is slow and can block.
namespace {
std::vector<vibe::DetectedRadio> g_radios;
std::string g_radioStr;                    // backing store for the returned pointers
const vibe::DetectedRadio* radioAt(int i) {
    return (i >= 0 && i < (int)g_radios.size()) ? &g_radios[(size_t)i] : nullptr;
}
}  // namespace

void vs_radios_refresh(void) { g_radios = vibe::detectRadios(); }

int vs_radio_count(void) {
    // ★ Refresh on first use, so a host that only ever calls the accessors still gets an answer
    //   rather than a confusing zero. An explicit refresh is still the way to RE-scan.
    if (g_radios.empty()) vs_radios_refresh();
    return (int)g_radios.size();
}

const char* vs_radio_serial(int index) {
    const auto* r = radioAt(index);
    g_radioStr = r ? r->serial : std::string();
    return g_radioStr.c_str();
}

const char* vs_radio_driver(int index) {
    const auto* r = radioAt(index);
    g_radioStr = r ? r->driver : std::string();
    return g_radioStr.c_str();
}

const char* vs_radio_name(int index) {
    const auto* r = radioAt(index);
    g_radioStr = r ? r->name : std::string();
    return g_radioStr.c_str();
}

int vs_radio_serials_collide(void) {
    if (g_radios.empty()) vs_radios_refresh();
    return vibe::serialsCollide(g_radios) ? 1 : 0;
}
