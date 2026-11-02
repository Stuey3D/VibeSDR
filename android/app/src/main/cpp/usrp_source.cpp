// Ettus USRP B2xx capture source — EXPERIMENTAL, desktop VibeServer only (Linux amd64/arm64, macOS). See usrp_source.h.
// ★ Compiled ONLY when VIBE_HAVE_USRP (vibeserver/CMakeLists.txt); every other build uses the header's inline stub.
// ★ Clean-room: written from UHD's published API (uhd::usrp::multi_usrp, uhd::rx_streamer, uhd::device::find) and
//   libusb's; UHD is used as a library, nothing is copied from any SDR application.
#include "usrp_source.h"

#if defined(VIBE_HAVE_USRP)
#include "vibe_usrp_caps.h"
#include "vibe_thread.h"

#include <uhd/device.hpp>
#include <uhd/exception.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/log.hpp>

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#endif

#ifndef VIBE_UHD_IMAGES_DIR
#  define VIBE_UHD_IMAGES_DIR "/usr/share/vibeserver/uhd-images"
#endif

namespace vibe {
namespace {

double nowSecsMono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* ★★ THE DESCRIPTOR WALK — our own, not uhd::device::find(). find() LOADS FIRMWARE into every B2xx it meets, claims
 *    interface 0 to do it and then waits for the radio to re-enumerate: seconds, and a USB reset under any process
 *    holding one. detectRadios() runs on every setup-page refresh, so it must be the same cheap, claim-free walk the
 *    other drivers' presence code uses (radios.cpp usbBusCounts). Only open() calls find(). */
struct Found { uint8_t bus = 0, addr = 0; uint16_t vid = 0, pid = 0; std::string serial; int speed = 0; };
std::vector<Found> walk(bool readSerials) {
    std::vector<Found> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (!usrpcaps::nameForUsbId(d.idVendor, d.idProduct)) continue;
        Found f;
        f.bus = libusb_get_bus_number(list[i]); f.addr = libusb_get_device_address(list[i]);
        f.vid = d.idVendor; f.pid = d.idProduct;
        const int sp = libusb_get_device_speed(list[i]);
        f.speed = sp >= LIBUSB_SPEED_SUPER ? 3 : sp == LIBUSB_SPEED_HIGH ? 2 : sp > 0 ? 1 : 0;
        /* ★ The serial is a string descriptor, which needs an OPEN (never a claim). An open fails on a radio another
         *   program holds on some systems — then the serial is "" and sdr_presence.h's bus count still sees it. */
        if (readSerials && d.iSerialNumber) {
            libusb_device_handle* h = nullptr;
            if (libusb_open(list[i], &h) == 0 && h) {
                unsigned char buf[128] = {0};
                const int k = libusb_get_string_descriptor_ascii(h, d.iSerialNumber, buf, sizeof buf - 1);
                if (k > 0) f.serial.assign((const char*)buf, (size_t)k);
                libusb_close(h);
            }
        }
        out.push_back(f);
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    // ★ A stable order (bus, address) so "the second USRP" means the same radio on two calls a second apart.
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        return a.bus != b.bus ? a.bus < b.bus : a.addr < b.addr; });
    return out;
}

/** Where our images are, most specific first — see usrpcaps::resolveImage. */
std::vector<std::string> imageDirs() {
    std::vector<std::string> d;
    if (const char* e = std::getenv("VIBE_UHD_IMAGES_DIR")) if (*e) d.push_back(e);   // tests / a developer
#if defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) d.push_back(std::string(h) + "/Library/Application Support/VibeServer/uhd-images");
    char exe[PATH_MAX] = {0}; uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        std::string dir(exe); dir = dir.substr(0, dir.find_last_of('/'));
        d.push_back(dir + "/../Resources/uhd-images");   // VibeServer.app/Contents/MacOS → Contents/Resources
        d.push_back(dir + "/uhd-images");                // a bare build-tree binary
    }
#else
    d.push_back("/etc/vibeserver/uhd-images");          // the owner's override (a clone's own FPGA image)
#endif
    d.push_back(VIBE_UHD_IMAGES_DIR);                    // what the package ships
    return d;
}

/* ★★ UHD's LOG GOES TO OUR stderr AT WARNING. Its default console level is INFO, which prints a banner and a line per
 *    setting change into the journal; warnings (overflows, a failed image) are what an owner's log needs. Set once,
 *    before the first UHD call. UHD_LOG_CONSOLE_LEVEL in the environment still overrides it for a test. */
std::once_flag g_logOnce;
void quietUhdLog() {
    std::call_once(g_logOnce, [] {
        if (!std::getenv("UHD_LOG_CONSOLE_LEVEL")) uhd::log::set_console_level(uhd::log::warning);
    });
}

}  // namespace

struct UsrpSource::Impl {
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr      rx;
    std::vector<usrpcaps::Port> ports;
    size_t                      nChans = 1;
    usrpcaps::Port              port{0, "RX2"};
    double                      rate = 0, centre = 0;
    std::recursive_mutex        mtx;          // every UHD call on the device
    std::thread                 thr;
    std::atomic<bool>           run{false};
    std::atomic<bool>           paused{false};
    std::atomic<double>         lastRx{0.0};
    std::atomic<uint64_t>       overflows{0};
    std::vector<float>          buf;
};

UsrpSource::UsrpSource() : impl_(new Impl) {}
UsrpSource::~UsrpSource() { close(); delete impl_; }

int UsrpSource::deviceCount() { return (int)walk(false).size(); }

std::string UsrpSource::deviceSerial(int index) {
    const auto l = walk(true);
    return (index >= 0 && index < (int)l.size()) ? l[(size_t)index].serial : std::string();
}

std::string UsrpSource::deviceName(int index) {
    const auto l = walk(true);
    if (index < 0 || index >= (int)l.size()) return "";
    return usrpcaps::displayName(usrpcaps::nameForUsbId(l[(size_t)index].vid, l[(size_t)index].pid), l[(size_t)index].serial);
}

bool UsrpSource::open(int index, double sampleRateHz, double centreHz, int gainTenthDb, std::string& err) {
    close();
    quietUhdLog();
    const auto l = walk(true);
    if (index < 0 || index >= (int)l.size()) { err = "no USRP at that index"; return false; }
    const Found f = l[(size_t)index];
    const auto dirs = imageDirs();
    const std::string fw = usrpcaps::resolveImage(usrpcaps::firmwareFile(), dirs);
    if (fw.empty()) { err = "the USRP firmware (usrp_b200_fw.hex) is missing from this install — reinstall VibeServer"; return false; }
    /* ★ UHD_IMAGES_DIR too, for any lookup UHD makes that we did not foresee — pointed at the directory holding OUR
     *   firmware, so it is never the owner's own UHD install of a different release (whose FPGA compat number would
     *   not match this libuhd). */
    { const std::string d = fw.substr(0, fw.find_last_of('/')); ::setenv("UHD_IMAGES_DIR", d.c_str(), 1); }
    try {
        /* ★★ find() BY SERIAL, with our firmware: it loads the FX3 firmware if the radio has none yet, waits for the
         *    re-enumeration, and reports the PRODUCT (B200 / B210 / B200mini / B205mini) from the EEPROM — which is
         *    what picks the FPGA image. A radio with no readable serial is found by type alone (the first one). */
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        hint["fw"] = fw;
        if (!f.serial.empty()) hint["serial"] = f.serial;
        const uhd::device_addrs_t found = uhd::device::find(hint, uhd::device::USRP);
        if (found.empty()) { err = "the USRP did not answer (is another program using it?)"; return false; }
        uhd::device_addr_t addr = found.front();
        const std::string product = addr.has_key("product") ? addr["product"] : "";
        const char* fpgaName = usrpcaps::fpgaFileFor(product);
        if (!fpgaName) { err = "this USRP reports itself as '" + product + "', which VibeServer has no FPGA image for"; return false; }
        const std::string fpga = usrpcaps::resolveImage(fpgaName, dirs);
        if (fpga.empty()) { err = std::string("the USRP FPGA image (") + fpgaName + ") is missing from this install — reinstall VibeServer"; return false; }
        addr["fw"] = fw;
        addr["fpga"] = fpga;
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        impl_->usrp = uhd::usrp::multi_usrp::make(addr);
        auto& u = *impl_->usrp;
        model_  = product;
        serial_ = addr.has_key("serial") ? addr["serial"] : f.serial;
        impl_->nChans = std::max<size_t>(1, u.get_rx_num_channels());
        std::vector<std::vector<std::string>> socks;
        for (size_t c = 0; c < impl_->nChans; c++) socks.push_back(u.get_rx_antennas(c));
        impl_->ports = usrpcaps::ports(socks);
        if (impl_->ports.empty()) impl_->ports.push_back({0, "RX2"});
        impl_->port = impl_->ports.front();
        antennas_ = usrpcaps::labels(impl_->ports, impl_->nChans);
        const size_t c = impl_->port.chan;
        const uhd::freq_range_t fr = u.get_rx_freq_range(c);
        fmin_ = fr.start(); fmax_ = fr.stop();
        const uhd::gain_range_t gr = u.get_rx_gain_range(c);
        gains_ = usrpcaps::gainList(gr.start(), gr.stop(), gr.step());
        // ★ Link speed AFTER find(): the firmware has re-enumerated the radio by now, so this is the real link.
        usbSpeed_ = 0;
        for (const auto& g : walk(false)) if (g.vid == f.vid && g.pid == f.pid) { usbSpeed_ = std::max(usbSpeed_, g.speed); }
        rates_ = usrpcaps::ratesFor(usbSpeed_);
        // ★ AGC: present on a B2xx, but asked, not assumed — a refusal simply means no AUTO is offered.
        hasAgc_ = false;
        try { u.set_rx_agc(false, c); hasAgc_ = true; } catch (const std::exception&) { hasAgc_ = false; }
        // ★ The AD9361's own DC-offset and IQ-balance tracking (both default on in UHD, set explicitly so it is said).
        try { u.set_rx_dc_offset(true, c); } catch (const std::exception&) {}
        try { u.set_rx_iq_balance(true, c); } catch (const std::exception&) {}
    } catch (const std::exception& e) {
        err = std::string("USRP: ") + e.what();
        impl_->usrp.reset();
        return false;
    }
    open_ = true;
    if (!setSampleRate(sampleRateHz)) { err = "USRP: the sample rate was refused"; close(); return false; }
    setFrequency(centreHz);
    setGainTenthDb(gainTenthDb < 0 && !hasAgc_ ? 0 : gainTenthDb);
    std::fprintf(stderr, "VibeServer: USRP %s (%s) open — USB %d, %.3f MS/s, %s, %zu channel(s), images from %s\n",
                 model_.c_str(), serial_.c_str(), usbSpeed_, impl_->rate / 1e6, impl_->port.socket.c_str(),
                 impl_->nChans, fw.substr(0, fw.find_last_of('/')).c_str());
    if (usbSpeed_ > 0 && usbSpeed_ < 3)
        std::fprintf(stderr, "VibeServer: USRP is on a USB 2 link — rates above 8 MS/s are not offered, and a B210 may "
                             "need its external power supply on USB 2\n");
    return true;
}

bool UsrpSource::rebuildStream(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->usrp) { err = "not open"; return false; }
    try {
        impl_->rx.reset();
        uhd::stream_args_t sa("fc32", "sc16");   // ★ host float, wire int16 — UHD's SIMD converter does the step
        sa.channels = { impl_->port.chan };
        impl_->rx = impl_->usrp->get_rx_stream(sa);
    } catch (const std::exception& e) { err = std::string("USRP stream: ") + e.what(); return false; }
    return true;
}

bool UsrpSource::start(std::string& err) {
    if (!open_) { err = "not open"; return false; }
    if (impl_->run.load()) return true;
    if (!rebuildStream(err)) return false;
    wantStreaming_ = true;
    impl_->run.store(true);
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        try {
            uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
            cmd.stream_now = true;
            impl_->rx->issue_stream_cmd(cmd);
        } catch (const std::exception& e) { impl_->run.store(false); err = std::string("USRP start: ") + e.what(); return false; }
    }
    impl_->thr = std::thread([this] {
        /* ★★★ THE IQ RUNG — the same one every other radio's USB thread takes (vibeIqThread): above spectrum and
         *     decoders, below audio and the network (Network > Audio > Spectrum > Decoders). */
        vibeIqThread("vibe-usrp");
        uhd::rx_streamer::sptr rx;
        { std::lock_guard<std::recursive_mutex> lk(impl_->mtx); rx = impl_->rx; }
        if (!rx) return;
        const size_t maxN = std::max<size_t>(rx->get_max_num_samps(), 1024);
        impl_->buf.assign(maxN * 2, 0.0f);
        uhd::rx_metadata_t md;
        while (impl_->run.load(std::memory_order_relaxed)) {
            void* bp = impl_->buf.data();
            size_t n = 0;
            try { n = rx->recv(&bp, maxN, md, 0.2); }
            catch (const std::exception& e) { std::fprintf(stderr, "VibeServer: USRP recv: %s\n", e.what()); break; }
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) { impl_->overflows.fetch_add(1, std::memory_order_relaxed); continue; }
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) continue;
            if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) continue;
            if (n == 0) continue;
            // ★ Liveness before the drop — this buffer proves the radio is alive whether or not we keep it.
            impl_->lastRx.store(nowSecsMono(), std::memory_order_relaxed);
            if (impl_->paused.load(std::memory_order_relaxed)) continue;
            if (sink_) sink_(impl_->buf.data(), (int)n);
        }
    });
    return true;
}

void UsrpSource::stop() {
    wantStreaming_ = false;
    if (!impl_->run.exchange(false)) { if (impl_->thr.joinable()) impl_->thr.join(); return; }
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        try {
            if (impl_->rx) impl_->rx->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
        } catch (const std::exception&) {}
    }
    if (impl_->thr.joinable()) impl_->thr.join();
    // ★ Drain what is in flight, so a restart does not begin with a stale buffer and an overflow.
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->rx) {
        try {
            uhd::rx_metadata_t md; void* bp = impl_->buf.data();
            const size_t maxN = impl_->buf.size() / 2;
            for (int i = 0; i < 50 && maxN && impl_->rx->recv(&bp, maxN, md, 0.01) > 0; i++) {}
        } catch (const std::exception&) {}
    }
}

void UsrpSource::close() {
    stop();
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    impl_->rx.reset();
    impl_->usrp.reset();
    open_ = false;
}

void UsrpSource::setFrequency(double hz) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->usrp) return;
    if (fmax_ > 0) hz = std::min(std::max(hz, fmin_), fmax_);
    try { impl_->usrp->set_rx_freq(uhd::tune_request_t(hz), impl_->port.chan); impl_->centre = hz; }
    catch (const std::exception& e) { std::fprintf(stderr, "VibeServer: USRP tune %.0f Hz: %s\n", hz, e.what()); }
}

uint32_t UsrpSource::nearestRate(double hz) const { return usrpcaps::nearestRate(rates_, hz, usrpcaps::thisHost()); }

bool UsrpSource::setSampleRate(double hz) {
    const uint32_t r = nearestRate(hz);
    if (!r) return false;
    const bool wasRunning = impl_->run.load();
    if (wasRunning) stop();
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        if (!impl_->usrp) return false;
        try {
            const size_t c = impl_->port.chan;
            impl_->usrp->set_rx_rate((double)r, c);
            impl_->rate = impl_->usrp->get_rx_rate(c);
            /* ★ The analogue filter follows the rate: the AD9361's RX bandwidth set to the span, so nothing outside
             *   what is drawn folds in. UHD clips it to the part's own range (about 200 kHz – 56 MHz). */
            try { impl_->usrp->set_rx_bandwidth((double)r, c); } catch (const std::exception&) {}
            ok = true;
        } catch (const std::exception& e) { std::fprintf(stderr, "VibeServer: USRP rate %u: %s\n", r, e.what()); }
    }
    if (wasRunning) { std::string e; start(e); }
    return ok;
}

void UsrpSource::setGainTenthDb(int tenthDb) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->usrp) { gainTenth_ = tenthDb < 0 ? 0 : tenthDb; return; }
    const size_t c = impl_->port.chan;
    try {
        if (tenthDb < 0 && hasAgc_) {
            impl_->usrp->set_rx_agc(true, c);
            agcOn_ = true;
            return;
        }
        if (hasAgc_ && agcOn_) { impl_->usrp->set_rx_agc(false, c); agcOn_ = false; }
        const int g = usrpcaps::nearestGain(gains_, tenthDb < 0 ? 0 : tenthDb);
        impl_->usrp->set_rx_gain(g / 10.0, c);
        gainTenth_ = g;
    } catch (const std::exception& e) { std::fprintf(stderr, "VibeServer: USRP gain: %s\n", e.what()); }
}

std::string UsrpSource::antenna() const {
    if (antennas_.empty()) return "";
    return usrpcaps::portLabel(impl_->port.chan, impl_->nChans, impl_->port.socket);
}

bool UsrpSource::setAntenna(const std::string& label) {
    usrpcaps::Port want;
    if (!usrpcaps::portFor(impl_->ports, impl_->nChans, label, want)) return false;
    const bool chanMoves = want.chan != impl_->port.chan;
    const bool wasRunning = impl_->run.load();
    if (chanMoves && wasRunning) stop();
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        if (!impl_->usrp) return false;
        try {
            impl_->port = want;
            const size_t c = want.chan;
            impl_->usrp->set_rx_antenna(want.socket, c);
            if (chanMoves) {
                // ★ The other channel starts from nothing: put rate, tuning and gain back on IT.
                impl_->usrp->set_rx_rate(impl_->rate, c);
                try { impl_->usrp->set_rx_bandwidth(impl_->rate, c); } catch (const std::exception&) {}
                impl_->usrp->set_rx_freq(uhd::tune_request_t(impl_->centre), c);
                if (agcOn_ && hasAgc_) impl_->usrp->set_rx_agc(true, c);
                else { if (hasAgc_) impl_->usrp->set_rx_agc(false, c); impl_->usrp->set_rx_gain(gainTenth_ / 10.0, c); }
                try { impl_->usrp->set_rx_dc_offset(true, c); impl_->usrp->set_rx_iq_balance(true, c); } catch (const std::exception&) {}
            }
        } catch (const std::exception& e) { std::fprintf(stderr, "VibeServer: USRP antenna %s: %s\n", label.c_str(), e.what()); }
    }
    if (chanMoves && wasRunning) { std::string e; start(e); }
    return true;
}

uint64_t UsrpSource::takeOverflows() { return impl_->overflows.exchange(0, std::memory_order_relaxed); }

double UsrpSource::secondsSinceLastRx() const {
    const double t = impl_->lastRx.load(std::memory_order_relaxed);
    return t <= 0 ? 1e9 : nowSecsMono() - t;
}

void UsrpSource::setPaused(bool paused) { impl_->paused.store(paused, std::memory_order_relaxed); }

}  // namespace vibe
#endif  // VIBE_HAVE_USRP
