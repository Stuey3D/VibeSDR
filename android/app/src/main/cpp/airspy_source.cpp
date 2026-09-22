// See airspy_source.h — Airspy R2 / Mini, on the vendored (unmodified) libairspy.
#include "airspy_source.h"

#ifdef VIBE_HAVE_AIRSPY
#include "airspy.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define ASLOG(...) __android_log_print(ANDROID_LOG_INFO, "VibeLocalSDR", __VA_ARGS__)
#else
#define ASLOG(...) do { printf("[VibeLocalSDR] " __VA_ARGS__); printf("\n"); } while (0)
#endif

namespace vibe {

namespace {
/** ★ 0-21 on BOTH preset curves (airspy_set_linearity_gain / _sensitivity_gain). The slider
 *  speaks tenths of a dB to match every other radio here, so one preset step is 10. */
constexpr int kPresets = 22;
int presetFromTenth(int tenthDb) {
    int p = (tenthDb + 5) / 10;
    return p < 0 ? 0 : (p > kPresets - 1 ? kPresets - 1 : p);
}
}  // namespace

AirspySource::AirspySource() = default;
AirspySource::~AirspySource() { close(); }

int AirspySource::deviceCount() {
    const int n = airspy_list_devices(nullptr, 0);
    return n > 0 ? n : 0;
}

std::string AirspySource::deviceName(int index) {
    const int n = airspy_list_devices(nullptr, 0);
    if (index < 0 || index >= n) return "";
    std::vector<uint64_t> serials((size_t)n, 0);
    if (airspy_list_devices(serials.data(), n) < 0) return "";
    char buf[64];
    snprintf(buf, sizeof buf, "Airspy (%016llX)", (unsigned long long)serials[(size_t)index]);
    return buf;
}

/** ★ 24-1800 MHz. An R2/Mini has no direct-sampling branch, so below 24 MHz there is nothing to
 *  switch out and nothing to hear without an upconverter — the app must not offer it. */
bool AirspySource::tuneRangeContains(double hz) { return hz >= 24e6 && hz <= 1800e6; }

std::vector<int> AirspySource::gainListTenthDb() {
    std::vector<int> g;
    g.reserve(kPresets);
    for (int i = 0; i < kPresets; i++) g.push_back(i * 10);
    return g;
}

namespace {
/** AIRSPY_SAMPLE_FLOAT32_IQ: interleaved float I,Q — the engine's own format, no conversion. */
int airspyRxCallback(airspy_transfer* t) {
    if (!t || t->sample_count <= 0) return 0;
    auto* self = static_cast<AirspySource*>(t->ctx);
    if (self) self->deliver(static_cast<const float*>(t->samples), t->sample_count);
    return 0;
}
}  // namespace

bool AirspySource::open(int index, double sampleRateHz, double centreHz, int gainTenthDb,
                        std::string& err) {
    close();
    const int n = airspy_list_devices(nullptr, 0);
    if (n <= 0 || index < 0 || index >= n) { err = "no Airspy found"; return false; }
    std::vector<uint64_t> serials((size_t)n, 0);
    if (airspy_list_devices(serials.data(), n) < 0) { err = "could not list Airspy devices"; return false; }
    const int rc = airspy_open_sn(&dev_, serials[(size_t)index]);
    if (rc != AIRSPY_SUCCESS || !dev_) { err = std::string("airspy_open_sn: ") + airspy_error_name((airspy_error)rc); dev_ = nullptr; return false; }
    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
}

bool AirspySource::openFd(int fd, double sampleRateHz, double centreHz, int gainTenthDb,
                          std::string& err) {
    close();
    // ★ libusb takes ownership of the fd (upstream airspy_open_fd → libusb_wrap_sys_device).
    const int rc = airspy_open_fd(&dev_, fd);
    if (rc != AIRSPY_SUCCESS || !dev_) { err = std::string("airspy_open_fd: ") + airspy_error_name((airspy_error)rc); dev_ = nullptr; return false; }
    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
}

bool AirspySource::finishOpen(double sampleRateHz, double centreHz, int gainTenthDb,
                              std::string& err) {
    // ── What this actually is: board id names the model, the serial tells two apart ──
    uint8_t board = 0;
    if (airspy_board_id_read(dev_, &board) == AIRSPY_SUCCESS)
        model_ = std::string("Airspy ") + airspy_board_id_name((airspy_board_id)board);
    airspy_read_partid_serialno_t ps{};
    if (airspy_board_partid_serialno_read(dev_, &ps) == AIRSPY_SUCCESS) {
        char b[32];
        snprintf(b, sizeof b, "%08X%08X", ps.serial_no[2], ps.serial_no[3]);
        serial_ = b;
    }
    // ── The rates THIS radio has (a Mini and an R2 differ) ──
    uint32_t count = 0;
    airspy_get_samplerates(dev_, &count, 0);
    if (count > 0 && count < 64) {
        rates_.assign(count, 0);
        airspy_get_samplerates(dev_, rates_.data(), count);
        std::sort(rates_.begin(), rates_.end());
    }
    if (airspy_set_sample_type(dev_, AIRSPY_SAMPLE_FLOAT32_IQ) != AIRSPY_SUCCESS) {
        err = "airspy_set_sample_type failed"; close(); return false;
    }
    if (!setSampleRate(sampleRateHz)) { err = "airspy_set_samplerate failed"; close(); return false; }
    open_ = true;
    setFrequency(centreHz);
    gainTenth_ = gainTenthDb;
    if (gainTenthDb < 0) { setLnaAgc(true); setMixerAgc(true); } else applyGain();
    ASLOG("Airspy: %s serial %s, %zu rate(s), %.3f MS/s at %.3f MHz",
          model_.c_str(), serial_.c_str(), rates_.size(), sampleRateHz / 1e6, centreHz / 1e6);
    return true;
}

void AirspySource::close() {
    stop();
    if (dev_) { airspy_close(dev_); dev_ = nullptr; }
    open_ = false;
}

bool AirspySource::start(std::string& err) {
    if (!dev_) { err = "Airspy not open"; return false; }
    if (streaming_) return true;
    const int rc = airspy_start_rx(dev_, &airspyRxCallback, this);
    if (rc != AIRSPY_SUCCESS) { err = std::string("airspy_start_rx: ") + airspy_error_name((airspy_error)rc); return false; }
    streaming_ = true;
    return true;
}

void AirspySource::stop() {
    if (dev_ && streaming_) airspy_stop_rx(dev_);
    streaming_ = false;
}

void AirspySource::setFrequency(double hz) {
    if (!dev_) return;
    centreHz_ = hz;
    airspy_set_freq(dev_, (uint32_t)llround(hz));
}

uint32_t AirspySource::nearestRate(double hz) const {
    if (rates_.empty()) return (uint32_t)llround(hz);
    uint32_t best = rates_.front();
    double bestD = 1e18;
    for (uint32_t r : rates_) {
        const double d = std::fabs((double)r - hz);
        if (d < bestD) { bestD = d; best = r; }
    }
    return best;
}

bool AirspySource::setSampleRate(double hz) {
    if (!dev_) return false;
    const uint32_t want = nearestRate(hz);
    if (airspy_set_samplerate(dev_, want) != AIRSPY_SUCCESS) return false;
    ASLOG("Airspy: sample rate %.3f MS/s", want / 1e6);
    return true;
}

void AirspySource::setGainTenthDb(int tenthDb) {
    gainTenth_ = tenthDb;
    if (!dev_) return;
    if (tenthDb < 0) {   // ★ the nearest thing this radio has to "auto" — its own stage AGCs
        setLnaAgc(true);
        setMixerAgc(true);
        return;
    }
    setLnaAgc(false);
    setMixerAgc(false);
    lna_ = mixer_ = vga_ = -1;   // back to a preset curve
    applyGain();
}

void AirspySource::applyGain() {
    if (!dev_ || gainTenth_ < 0) return;
    const uint8_t p = (uint8_t)presetFromTenth(gainTenth_);
    if (sensitivity_) airspy_set_sensitivity_gain(dev_, p);
    else              airspy_set_linearity_gain(dev_, p);
    ASLOG("Airspy: %s gain preset %u of %d", sensitivity_ ? "sensitivity" : "linearity", p, kPresets - 1);
}

void AirspySource::setSensitivityCurve(bool sensitivity) {
    sensitivity_ = sensitivity;
    applyGain();
}

void AirspySource::setLnaGain(int v) {
    lna_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    if (dev_) { setLnaAgc(false); airspy_set_lna_gain(dev_, (uint8_t)lna_); }
}
void AirspySource::setMixerGain(int v) {
    mixer_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    if (dev_) { setMixerAgc(false); airspy_set_mixer_gain(dev_, (uint8_t)mixer_); }
}
void AirspySource::setVgaGain(int v) {
    vga_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    if (dev_) airspy_set_vga_gain(dev_, (uint8_t)vga_);
}
void AirspySource::setLnaAgc(bool on)   { lnaAgc_ = on;   if (dev_) airspy_set_lna_agc(dev_, on ? 1 : 0); }
void AirspySource::setMixerAgc(bool on) { mixerAgc_ = on; if (dev_) airspy_set_mixer_agc(dev_, on ? 1 : 0); }
void AirspySource::setBiasTee(bool on)  { bias_ = on;     if (dev_) airspy_set_rf_bias(dev_, on ? 1 : 0); }
void AirspySource::setPacking(bool on)  { packing_ = on;  if (dev_) airspy_set_packing(dev_, on ? 1 : 0); }

} // namespace vibe

#else   // !VIBE_HAVE_AIRSPY — the whole driver compiled out (no libairspy on this platform)
namespace vibe {
AirspySource::AirspySource() = default;
AirspySource::~AirspySource() = default;
int AirspySource::deviceCount() { return 0; }
std::string AirspySource::deviceName(int) { return ""; }
bool AirspySource::tuneRangeContains(double) { return false; }
std::vector<int> AirspySource::gainListTenthDb() { return {}; }
bool AirspySource::open(int, double, double, int, std::string& err) { err = "built without Airspy support"; return false; }
bool AirspySource::openFd(int, double, double, int, std::string& err) { err = "built without Airspy support"; return false; }
bool AirspySource::finishOpen(double, double, int, std::string& err) { err = "built without Airspy support"; return false; }
void AirspySource::close() {}
bool AirspySource::start(std::string& err) { err = "built without Airspy support"; return false; }
void AirspySource::stop() {}
void AirspySource::setFrequency(double) {}
uint32_t AirspySource::nearestRate(double hz) const { return (uint32_t)hz; }
bool AirspySource::setSampleRate(double) { return false; }
void AirspySource::setGainTenthDb(int) {}
void AirspySource::applyGain() {}
void AirspySource::setSensitivityCurve(bool) {}
void AirspySource::setLnaGain(int) {}
void AirspySource::setMixerGain(int) {}
void AirspySource::setVgaGain(int) {}
void AirspySource::setLnaAgc(bool) {}
void AirspySource::setMixerAgc(bool) {}
void AirspySource::setBiasTee(bool) {}
void AirspySource::setPacking(bool) {}
} // namespace vibe
#endif
