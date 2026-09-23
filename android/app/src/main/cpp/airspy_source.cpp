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
    if (airspy_board_id_read(dev_, &board) == AIRSPY_SUCCESS) {
        /* ★★ THE BOARD ALREADY SAYS "AIRSPY". libairspy's board_id_name returns "AIRSPY MINI" or
         *  "AIRSPY R2", so prefixing our own "Airspy " produced "Airspy AIRSPY MINI" — which is
         *  what the panel heading showed our tester ("Airspy AIRSPY Controls", 2026-09-22).
         *  ★ Prefix only when the board's own name does NOT already carry it, so an unknown future
         *    board that reports something else is still identified as an Airspy. */
        std::string bn = airspy_board_id_name((airspy_board_id)board);
        const bool saysAirspy = bn.size() >= 6
            && (bn.compare(0, 6, "AIRSPY") == 0 || bn.compare(0, 6, "Airspy") == 0);
        model_ = saysAirspy ? bn : ("Airspy " + bn);
        /* ★ And in the case the library uses for a product name rather than shouting it: the model
         *  is shown as a heading beside "Controls", not as a log line. */
        if (saysAirspy && bn.size() > 6) {
            for (size_t i = 1; i < model_.size(); i++)
                if (model_[i - 1] != ' ' && model_[i] >= 'A' && model_[i] <= 'Z')
                    model_[i] = (char)(model_[i] - 'A' + 'a');
        }
    }
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
    /* ★ RECORDED, NOT APPLIED. Nothing is written to the tuner here any more: applyAll() does it
     *  once the stream is running, which is the only time this radio reliably takes it. */
    centreHz_  = centreHz;
    gainTenth_ = gainTenthDb;
    if (gainTenthDb < 0) { lnaAgc_ = mixerAgc_ = true; mode_ = GainFree; }
    else                 { presetTenth_[1] = gainTenthDb; mode_ = GainLinear; }
    ASLOG("Airspy: %s serial %s, %zu rate(s), %.3f MS/s at %.3f MHz",
          model_.c_str(), serial_.c_str(), rates_.size(), sampleRateHz / 1e6, centreHz / 1e6);
    return true;
}

void AirspySource::close() {
    stop();
    if (dev_) { airspy_close(dev_); dev_ = nullptr; }
    open_ = false;
}

/** ★★★ EVERY SETTING, RE-STATED, AFTER THE STREAM IS RUNNING.
 *
 *  Three of the four faults our first Airspy tester reported are one fault (Onfliner, 2026-09-22,
 *  an Airspy Mini — the first one this driver has ever met):
 *    · "Gain control isn't working at all and sliders reset after some time"
 *    · "If you go back to the main menu and reconnect, the frequency stays at whatever it was but
 *       the audio sounds as if you're tuned to 100.0. Changing the frequency and back fixes it"
 *    · "Bias-T is not working"
 *  Every one of those settings was written BEFORE airspy_start_rx. The frequency symptom is the
 *  proof and it is unambiguous: the UI and the DSP agree on the old frequency and the AUDIO is
 *  somewhere else entirely, which can only mean the tuner never took the value we sent it — and
 *  the cure he found, retuning and coming back, is simply the first write that lands AFTER the
 *  stream is up.
 *  ★★ THIS IS WHAT EVERY OTHER AIRSPY CLIENT DOES. SDR++ and gr-osmosdr both start the transfer
 *     and then set frequency and gains; our open() set them all first, when nothing was running.
 *  ★★ SO THE STATE LIVES IN THIS OBJECT AND THE DEVICE IS TOLD ABOUT IT, rather than the device
 *     being the record. That also makes a rate change safe (see setSampleRate, which must stop
 *     the stream) and a reconnect honest: whatever the user last chose is re-stated, once,
 *     from one place. ★ One function, so a setting added later cannot be forgotten by half of a
 *     pair of call sites — the hand-maintained-list fault this project keeps paying for. */
void AirspySource::applyAll() {
    if (!dev_) return;
    airspy_set_packing(dev_, packing_ ? 1 : 0);
    airspy_set_rf_bias(dev_, bias_ ? 1 : 0);
    airspy_set_freq(dev_, (uint32_t)llround(centreHz_));
    applyGainMode();
    ASLOG("Airspy: settings re-stated on the live stream — %.3f MHz, bias-T %s, packing %s",
          centreHz_ / 1e6, bias_ ? "on" : "off", packing_ ? "on" : "off");
}

bool AirspySource::start(std::string& err) {
    if (!dev_) { err = "Airspy not open"; return false; }
    if (streaming_) return true;
    const int rc = airspy_start_rx(dev_, &airspyRxCallback, this);
    if (rc != AIRSPY_SUCCESS) { err = std::string("airspy_start_rx: ") + airspy_error_name((airspy_error)rc); return false; }
    streaming_ = true;
    // ★★★ AFTER the stream, never before — see applyAll().
    applyAll();
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
    /* ★★★ THE STREAM HAS TO STOP FIRST — "the sample rate won't change, it's stuck at 3.0M"
     *  (Onfliner's Mini, 2026-09-22). airspy_set_samplerate reconfigures the USB transfer geometry
     *  and libairspy will not have it while a transfer is running: the call fails, we returned
     *  false, and the radio stayed on whatever rate open() had chosen. The rate PICKER worked
     *  perfectly, which is what made it look like the picker.
     *  ★★ Stop, set, start again — and start() re-states every setting, so the frequency and the
     *     gains survive the restart rather than coming back at the device's defaults.
     *  ★ A failed set leaves the stream STOPPED only if the restart also fails, and then the
     *    caller is told: coming back at the old rate silently would be a radio that says it
     *    changed and did not. */
    const bool wasStreaming = streaming_;
    if (wasStreaming) stop();
    const bool ok = airspy_set_samplerate(dev_, want) == AIRSPY_SUCCESS;
    if (ok) rateHz_ = (double)want;
    if (wasStreaming) { std::string e; if (!start(e)) ASLOG("Airspy: restart after rate change failed: %s", e.c_str()); }
    if (!ok) return false;
    ASLOG("Airspy: sample rate %.3f MS/s", want / 1e6);
    return true;
}

void AirspySource::setGainTenthDb(int tenthDb) {
    gainTenth_ = tenthDb;
    /* ★★ "AUTO" ON THIS RADIO IS FREE MODE WITH BOTH STAGE AGCs ON — it has no whole-device
     *  automatic gain, and the two stage AGCs are the nearest thing it has. Naming it as a MODE
     *  rather than a magic negative number is the point of the three-way control. */
    if (tenthDb < 0) {
        lnaAgc_ = mixerAgc_ = true;
        mode_   = GainFree;
        applyGainMode();
        return;
    }
    /* ★ The slider drives whichever preset curve is selected, and is REMEMBERED against it. Moving
     *  it while in Free mode is the app asking for a preset, so the mode follows the control the
     *  user actually touched rather than the two disagreeing. */
    if (mode_ == GainFree) mode_ = GainLinear;
    presetTenth_[mode_ == GainSensitive ? 0 : 1] = tenthDb;
    applyGainMode();
}

/** ★★★ THE WHOLE GAIN PATH, IN ONE PLACE, matching SDR++ clause by clause — see the GainMode note
 *  in the header. The two preset modes force BOTH stage AGCs off before setting their curve,
 *  because a curve sets all three stages and an AGC still running would immediately overwrite two
 *  of them; Free honours the AGC switches, and the VGA is always manual because it has none. */
void AirspySource::applyGainMode() {
    if (!dev_) return;
    if (mode_ == GainFree) {
        airspy_set_lna_agc(dev_, lnaAgc_ ? 1 : 0);
        if (!lnaAgc_)   airspy_set_lna_gain(dev_,   (uint8_t)(lna_   < 0 ? 0 : lna_));
        airspy_set_mixer_agc(dev_, mixerAgc_ ? 1 : 0);
        if (!mixerAgc_) airspy_set_mixer_gain(dev_, (uint8_t)(mixer_ < 0 ? 0 : mixer_));
        airspy_set_vga_gain(dev_, (uint8_t)(vga_ < 0 ? 0 : vga_));
        ASLOG("Airspy: free gain — LNA %d%s, mixer %d%s, VGA %d",
              lna_ < 0 ? 0 : lna_, lnaAgc_ ? " (AGC)" : "",
              mixer_ < 0 ? 0 : mixer_, mixerAgc_ ? " (AGC)" : "", vga_ < 0 ? 0 : vga_);
        return;
    }
    airspy_set_lna_agc(dev_, 0);
    airspy_set_mixer_agc(dev_, 0);
    const int tenth = presetTenth_[mode_ == GainSensitive ? 0 : 1];
    if (tenth < 0) return;              // no position chosen yet — leave the radio as it opened
    const uint8_t p = (uint8_t)presetFromTenth(tenth);
    if (mode_ == GainSensitive) airspy_set_sensitivity_gain(dev_, p);
    else                        airspy_set_linearity_gain(dev_, p);
    ASLOG("Airspy: %s gain preset %u of %d",
          mode_ == GainSensitive ? "sensitivity" : "linearity", p, kPresets - 1);
}

void AirspySource::applyGain() { applyGainMode(); }

void AirspySource::setGainMode(int mode) {
    mode_ = (mode == GainSensitive || mode == GainFree) ? mode : GainLinear;
    /* ★ The slider must report the position THIS mode was left at, not the one the other curve
     *  was on — see the per-mode gains in the header. */
    if (mode_ != GainFree) gainTenth_ = presetTenth_[mode_ == GainSensitive ? 0 : 1];
    applyGainMode();
}

void AirspySource::setSensitivityCurve(bool sensitivity) {
    setGainMode(sensitivity ? GainSensitive : GainLinear);
}

/* ★★ A STAGE IS A FREE-MODE CONTROL. Moving one used to leave the preset curve silently; now it
 *  selects the mode it belongs to, so the panel and the radio cannot disagree about which of the
 *  three is in force. */
void AirspySource::setLnaGain(int v) {
    lna_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    mode_ = GainFree; lnaAgc_ = false;
    applyGainMode();
}
void AirspySource::setMixerGain(int v) {
    mixer_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    mode_ = GainFree; mixerAgc_ = false;
    applyGainMode();
}
void AirspySource::setVgaGain(int v) {
    vga_ = v < 0 ? 0 : (v > 15 ? 15 : v);
    mode_ = GainFree;
    applyGainMode();
}
/* ★ The stage AGCs exist only in Free mode — a preset curve sets all three stages itself, so an
 *  AGC left running there would overwrite two of them the moment it moved. Switching one on
 *  therefore selects Free, exactly as moving a stage does. */
void AirspySource::setLnaAgc(bool on)   { lnaAgc_ = on;   mode_ = GainFree; applyGainMode(); }
void AirspySource::setMixerAgc(bool on) { mixerAgc_ = on; mode_ = GainFree; applyGainMode(); }
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
/* ★ THE STUB SIDE NEEDS EVERY NEW MEMBER TOO. iOS compiles this branch, and a member added to
 *  the class but not defined here is an undefined symbol at LINK time — which the Xcode Cloud
 *  build reports long after the trigger has returned a run id. setGainMode arrived with the three
 *  gain modes and was missed; the archive check in build_ios.sh caught it. */
void AirspySource::setGainMode(int) {}
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
