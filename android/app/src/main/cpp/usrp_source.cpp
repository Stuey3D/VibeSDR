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
#include <uhd/property_tree.hpp>
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
                if (k > 0) f.serial = usrpcaps::cleanSerial(std::string((const char*)buf, (size_t)k));   // ★ "30AA038 ?" → "30AA038"
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
    // ★ The owner's override folder (a clone's own FPGA image, uploaded from the Setup page) — usrpcaps::ownerImageDir:
    //   /etc/vibeserver/uhd-images on Linux, Application Support on the Mac.
    d.push_back(usrpcaps::ownerImageDir());
#if defined(__APPLE__)
    char exe[PATH_MAX] = {0}; uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        std::string dir(exe); dir = dir.substr(0, dir.find_last_of('/'));
        d.push_back(dir + "/../Resources/uhd-images");   // VibeServer.app/Contents/MacOS → Contents/Resources
        d.push_back(dir + "/uhd-images");                // a bare build-tree binary
    }
#endif
    d.push_back(VIBE_UHD_IMAGES_DIR);                    // what the package ships
#if !defined(__APPLE__)
    // ★★ Both Linux homes, whatever was compiled in: the package's /usr/share, and a source install's /usr/local/share.
    //    A binary that looked only in /usr/local/share failed on every packaged install (2026-11-03, the first B210).
    for (const char* alt : { "/usr/share/vibeserver/uhd-images", "/usr/local/share/vibeserver/uhd-images" })
        if (std::find(d.begin(), d.end(), std::string(alt)) == d.end()) d.push_back(alt);
#endif
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
    // ★ V12 the owner's options (setOptions) — what was asked, what the radio can do, what was refused
    usrpcaps::Options           opts;
    usrpcaps::OptionCaps        oc;
    std::string                 note;               // refusals, in words; "" = everything asked for is in force
    bool                        dcOn = false, iqOn = false;
    // ★ sensor reads are USB control transfers (the GPSDO's go through its UART): cached, not read per hwinfo
    mutable double              sensAt = -1e9;
    mutable int                 refL = -1, gpsL = -1;
};

UsrpSource::UsrpSource() : impl_(new Impl) {}

std::string UsrpSource::shippedImageDir() {
    // ★ The images the PACKAGE ships — the search list without the test variable and the owner's override folder.
    const std::string owner = usrpcaps::ownerImageDir();
    const char* env = std::getenv("VIBE_UHD_IMAGES_DIR");
    for (const auto& d : imageDirs()) {
        if (d == owner || (env && d == env)) continue;
        if (usrpcaps::fileExists(d + "/" + usrpcaps::firmwareFile())) return d;
    }
    return "";
}
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
    /* ★ NO setenv("UHD_IMAGES_DIR") here, deliberately: both images are passed by ABSOLUTE path below (UHD takes an
     *   existing path as-is, before any search), and setenv in a running multi-threaded server races every getenv.
     *   The directory UHD would search otherwise is compiled into libuhd (build-uhd.sh) as ours. */
    std::string product;   // ★ outside the try: a failed FPGA load names the file the board needs (cloneFpgaAdvice)
    try {
        /* ★★ find() BY SERIAL, with our firmware: it loads the FX3 firmware if the radio has none yet, waits for the
         *    re-enumeration, and reports the PRODUCT (B200 / B210 / B200mini / B205mini) from the EEPROM — which is
         *    what picks the FPGA image. A radio with no readable serial is found by type alone (the first one). */
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        hint["fw"] = fw;
        /* ★★ Matched by the CLEANED serial, never by handing UHD ours: UHD reads the descriptor itself and a clone's may
         *    carry junk after the serial ("30AA038 ?"), which an exact hint would never match. Every B2xx is found, then
         *    the one whose cleaned serial is ours is taken; with no serial (or a single radio) the first. */
        const uhd::device_addrs_t found = uhd::device::find(hint, uhd::device::USRP);
        if (found.empty()) { err = usrpcaps::busyAdvice("", /*foundNone=*/true); return false; }
        uhd::device_addr_t addr = found.front();
        if (!f.serial.empty())
            for (const auto& a : found)
                if (a.has_key("serial") && usrpcaps::cleanSerial(a["serial"]) == f.serial) { addr = a; break; }
        product = addr.has_key("product") ? addr["product"] : "";
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
        serial_ = addr.has_key("serial") ? usrpcaps::cleanSerial(addr["serial"]) : f.serial;
        if (serial_.empty()) serial_ = f.serial;
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
        /* ★★ V12 THE RATE GATE (vibe_usrp_caps.h ratesFor): on USB 3, the rates above 16 MS/s that THIS machine's saved
         *  benchmark passed — and the rate asked for, kept offered whatever the benchmark says (never hide one in use). */
        rates_ = usrpcaps::ratesFor(usbSpeed_, usrpcaps::gateHzFor(usrpcaps::rateGate()), sampleRateHz);
        // ★ AGC: present on a B2xx, but asked, not assumed — a refusal simply means no AUTO is offered.
        hasAgc_ = false;
        try { u.set_rx_agc(false, c); hasAgc_ = true; } catch (const std::exception&) { hasAgc_ = false; }
        /* ★★ V12 WHAT THIS RADIO OFFERS THE OWNER, asked of the radio and never assumed (AGENTS.md: no dead control).
         *    · clock / time sources: get_clock_sources / get_time_sources — and "gpsdo" kept only when a GPSDO is FITTED
         *      (the gps_locked sensor exists; UHD lists the source on every full-size B2xx, board or no board);
         *    · the DC-offset / IQ-balance switches: the property UHD itself sets (multi_usrp::set_rx_dc_offset looks for
         *      it and only WARNS when it is missing, so the call alone cannot tell us);
         *    · the analogue bandwidth: get_rx_bandwidth_range, offered only when it is a range worth having. */
        {
            usrpcaps::OptionCaps oc;
            try {
                const auto sn = u.get_mboard_sensor_names(0);
                oc.gpsdo = usrpcaps::hasGpsdo(sn);
                for (const auto& n : sn) if (n == "ref_locked") oc.refSensor = true;
            } catch (const std::exception&) {}
            try { oc.clockSources = usrpcaps::sourceChoices(u.get_clock_sources(0), oc.gpsdo); } catch (const std::exception&) {}
            try { oc.timeSources  = usrpcaps::sourceChoices(u.get_time_sources(0), oc.gpsdo); } catch (const std::exception&) {}
            try {
                const uhd::meta_range_t br = u.get_rx_bandwidth_range(c);
                oc.bwLo = br.start(); oc.bwHi = br.stop();
                oc.bwAdjustable = usrpcaps::bwRangeUseful(oc.bwLo, oc.bwHi);
            } catch (const std::exception&) {}
            try {
                const auto tree = u.get_tree();
                const auto spec = u.get_rx_subdev_spec(0).at(c);
                const uhd::fs_path rf = uhd::fs_path("/mboards/0/dboards") / spec.db_name / "rx_frontends" / spec.sd_name;
                const uhd::fs_path fe = uhd::fs_path("/mboards/0/rx_frontends") / spec.db_name;
                oc.dcOffset  = tree->exists(rf / "dc_offset" / "enable") || tree->exists(fe / "dc_offset" / "enable");
                oc.iqBalance = tree->exists(rf / "iq_balance" / "enable") || tree->exists(fe / "iq_balance" / "enable");
            } catch (const std::exception&) {}
            impl_->oc = oc;
        }
        // ★ The reference first (before any tuning), then the corrections; the bandwidth rides setSampleRate below.
        applyOptionsLocked(/*atOpen=*/true);
    } catch (const std::exception& e) {
        /* ★★ A CLONE'S FPGA LOAD FAILING is said as what to do (upload its maker's image, and which file), not as
         *    UHD's internals; anything else keeps UHD's own words. Either way it reaches the radio's status. */
        std::string advice = usrpcaps::cloneFpgaAdvice(e.what(), product);
        if (advice.empty()) advice = usrpcaps::busyAdvice(e.what(), false);
        err = advice.empty() ? std::string("USRP: ") + e.what() : advice;
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
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
        const auto& oc = impl_->oc;
        std::string cs, ts;
        for (const auto& x : oc.clockSources) cs += (cs.empty() ? "" : "/") + x;
        for (const auto& x : oc.timeSources)  ts += (ts.empty() ? "" : "/") + x;
        std::fprintf(stderr, "VibeServer: USRP options — clock %s [%s], time %s [%s], GPSDO %s, DC offset %s, IQ balance %s, "
                             "analogue bandwidth %s (%.3f-%.3f MHz)\n",
                     impl_->opts.clock.empty() ? "internal" : impl_->opts.clock.c_str(), cs.empty() ? "no choice" : cs.c_str(),
                     impl_->opts.time.empty() ? "internal" : impl_->opts.time.c_str(), ts.empty() ? "no choice" : ts.c_str(),
                     oc.gpsdo ? "fitted" : "none", oc.dcOffset ? (impl_->dcOn ? "auto" : "off") : "n/a",
                     oc.iqBalance ? (impl_->iqOn ? "auto" : "off") : "n/a",
                     !oc.bwAdjustable ? "fixed by the radio" : impl_->opts.rxBwHz > 0 ? "fixed" : "AUTO", oc.bwLo / 1e6, oc.bwHi / 1e6);
        if (!impl_->note.empty()) std::fprintf(stderr, "VibeServer: USRP: %s\n", impl_->note.c_str());
    }
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
        /* ★★ ~5 ms A HAND-OFF, SIZED TO THE RATE (V12, 2026-11-03 — the wide rates). One packet's worth (~2000 samples)
         *  per recv was 15 000 hand-offs a second at 30.72 MS/s, each a queue push and a whole DSP-loop iteration (its
         *  lock, its ticks, the capture cleaner's call) — overhead that grows with the rate and buys nothing. recv() fills
         *  the buffer across packets (one_packet false), so a bigger buffer is simply fewer, larger hand-offs. Bounded:
         *  at most 262144 samples (2 MB), never fewer than one packet. */
        const size_t pkt = std::max<size_t>(rx->get_max_num_samps(), 1024);
        const size_t want = impl_->rate > 0 ? (size_t)(impl_->rate * 0.005) : pkt;
        const size_t maxN = std::max(pkt, std::min<size_t>(want, 262144));
        impl_->buf.assign(maxN * 2, 0.0f);
        uhd::rx_metadata_t md;
        while (impl_->run.load(std::memory_order_relaxed)) {
            /* ★★★ THE BUFFER ITSELF, NEVER &pointer (the first real B210, 2026-11-03 — found by the memory-check build:
             *     "SEGV … usrp_source.cpp:353", every start, within a second). recv() takes uhd::ref_vector<void*>, and its
             *     one-pointer constructor treats ITS ARGUMENT as the channel's buffer: given `&bp` (a void**) it took the
             *     address of the local `bp` as the buffer and wrote each block of samples over this thread's STACK —
             *     smashing the locals (the streamer's shared_ptr among them) and the heap beside them. The float*
             *     goes straight in. */
            float* const bp = impl_->buf.data();
            size_t n = 0;
            try { n = rx->recv(bp, maxN, md, 0.2); }
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
            uhd::rx_metadata_t md; float* const bp = impl_->buf.data();   // ★ the buffer itself — see the loop above
            const size_t maxN = impl_->buf.size() / 2;
            for (int i = 0; i < 50 && maxN && impl_->rx->recv(bp, maxN, md, 0.01) > 0; i++) {}
        } catch (const std::exception&) {}
    }
}

void UsrpSource::close() {
    stop();
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    impl_->rx.reset();
    impl_->usrp.reset();
    impl_->oc = usrpcaps::OptionCaps{};   // ★ capabilities belong to an OPEN radio; the owner's options are kept
    impl_->refL = impl_->gpsL = -1; impl_->sensAt = -1e9;
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
            /* ★ The analogue filter follows the rate (AUTO): the AD9361's RX bandwidth set to the span, so nothing
             *   outside what is drawn folds in. UHD clips it to the part's own range (about 200 kHz – 56 MHz).
             * ★ V12: or the owner's fixed figure, when this radio's range allows it (usrpcaps::bwFor). */
            try { impl_->usrp->set_rx_bandwidth(usrpcaps::bwFor(impl_->opts, impl_->rate, impl_->oc), c); } catch (const std::exception&) {}
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
                try { impl_->usrp->set_rx_bandwidth(usrpcaps::bwFor(impl_->opts, impl_->rate, impl_->oc), c); } catch (const std::exception&) {}
                impl_->usrp->set_rx_freq(uhd::tune_request_t(impl_->centre), c);
                if (agcOn_ && hasAgc_) impl_->usrp->set_rx_agc(true, c);
                else { if (hasAgc_) impl_->usrp->set_rx_agc(false, c); impl_->usrp->set_rx_gain(gainTenth_ / 10.0, c); }
                // ★ The owner's corrections, on the new channel too (only where the radio has the switch).
                try { if (impl_->oc.dcOffset)  impl_->usrp->set_rx_dc_offset(impl_->opts.dcOffset, c); } catch (const std::exception&) {}
                try { if (impl_->oc.iqBalance) impl_->usrp->set_rx_iq_balance(impl_->opts.iqBalance, c); } catch (const std::exception&) {}
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

// ── ★ V12 the owner's options ─────────────────────────────────────────────────────────────────────────────────
void UsrpSource::applyOptionsLocked(bool atOpen) {
    if (!impl_->usrp) return;
    auto& u = *impl_->usrp;
    const auto& o  = impl_->opts;
    const auto& oc = impl_->oc;
    const size_t c = impl_->port.chan;
    std::string notes;
    auto addNote = [&](const std::string& s) { if (s.empty()) return; if (!notes.empty()) notes += "; "; notes += s; };

    /* ★★ THE REFERENCE. Refused in words when the radio did not list it (refuseSource); a mini cannot take external
     *    for both (refuseCombination). Only touched where the radio offers a choice at all — a radio with one source
     *    is never written to. Time first unless it is going external, so a mini never passes through the one pairing
     *    UHD refuses. */
    std::string clk = o.clock.empty() ? "internal" : o.clock;
    std::string tim = o.time.empty() ? "internal" : o.time;
    { const std::string r = usrpcaps::refuseSource(o.clock, oc.clockSources, "reference clock"); if (!r.empty()) { addNote(r); clk = "internal"; } }
    { const std::string r = usrpcaps::refuseSource(o.time, oc.timeSources, "time source"); if (!r.empty()) { addNote(r); tim = "internal"; } }
    { const std::string r = usrpcaps::refuseCombination(model_, clk, tim); if (!r.empty()) { addNote(r); tim = "internal"; } }
    bool clockMoved = false;
    auto setClock = [&] {
        if (oc.clockSources.empty()) return;
        try {
            if (u.get_clock_source(0) != clk) { u.set_clock_source(clk, 0); clockMoved = true; }
        } catch (const std::exception& e) {
            addNote(std::string("the radio refused ") + usrpcaps::sourceWords(clk) + " as its reference clock (" + e.what() + ")");
            try { u.set_clock_source("internal", 0); } catch (const std::exception&) {}
        }
    };
    auto setTime = [&] {
        if (oc.timeSources.empty()) return;
        try {
            if (u.get_time_source(0) != tim) u.set_time_source(tim, 0);
        } catch (const std::exception& e) {
            addNote(std::string("the radio refused ") + usrpcaps::sourceWords(tim) + " as its time source (" + e.what() + ")");
            try { u.set_time_source("internal", 0); } catch (const std::exception&) {}
        }
    };
    if (tim == "external") { setClock(); setTime(); } else { setTime(); setClock(); }

    /* ★ The AD9361's automatic corrections — where the radio has the switch. A switch-off asked of a radio without
     *   one is said; leaving them at UHD's default (on) needs no words. */
    impl_->dcOn = impl_->iqOn = false;
    if (oc.dcOffset) { try { u.set_rx_dc_offset(o.dcOffset, c); impl_->dcOn = o.dcOffset; } catch (const std::exception& e) { addNote(std::string("DC offset: ") + e.what()); } }
    else if (!o.dcOffset) addNote("this radio has no automatic DC-offset switch");
    if (oc.iqBalance) { try { u.set_rx_iq_balance(o.iqBalance, c); impl_->iqOn = o.iqBalance; } catch (const std::exception& e) { addNote(std::string("IQ balance: ") + e.what()); } }
    else if (!o.iqBalance) addNote("this radio has no automatic IQ-balance switch");

    // ★ The analogue bandwidth. At open the rate is not set yet — setSampleRate applies it there.
    addNote(usrpcaps::refuseBw(o.rxBwHz, oc));
    if (!atOpen && impl_->rate > 0) {
        try { u.set_rx_bandwidth(usrpcaps::bwFor(o, impl_->rate, oc), c); } catch (const std::exception& e) { addNote(std::string("bandwidth: ") + e.what()); }
    }
    // ★ A new reference moves the LO's synthesiser: re-tune so it relocks against it (open tunes after this anyway).
    if (clockMoved && !atOpen && impl_->centre > 0) {
        try { u.set_rx_freq(uhd::tune_request_t(impl_->centre), c); } catch (const std::exception&) {}
    }
    impl_->note = notes;
    impl_->sensAt = -1e9;   // ★ the lock state changes with the reference: read it afresh
}

void UsrpSource::setOptions(const usrpcaps::Options& o) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    const bool same = impl_->opts == o;
    impl_->opts = o;
    if (!impl_->usrp || same) return;
    applyOptionsLocked(/*atOpen=*/false);
    const std::string bw = o.rxBwHz > 0 ? std::to_string((long long)o.rxBwHz) + " Hz" : std::string("AUTO");
    std::fprintf(stderr, "VibeServer: USRP options applied — clock %s, time %s, DC offset %s, IQ balance %s, bandwidth %s%s%s\n",
                 o.clock.empty() ? "internal" : o.clock.c_str(), o.time.empty() ? "internal" : o.time.c_str(),
                 o.dcOffset ? "auto" : "off", o.iqBalance ? "auto" : "off", bw.c_str(),
                 impl_->note.empty() ? "" : " — ", impl_->note.c_str());
}

usrpcaps::Options UsrpSource::options() const {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    return impl_->opts;
}

usrpcaps::OptionCaps UsrpSource::optionCaps() const {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    return impl_->oc;
}

void UsrpSource::fillOptionView(usrpcaps::CapsView& v) const {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    v.oc = impl_->oc;
    v.optionsNote = impl_->note;
    v.dcOffsetOn = impl_->dcOn; v.iqBalanceOn = impl_->iqOn;
    v.rxBwAuto = !(impl_->opts.rxBwHz > 0 && usrpcaps::refuseBw(impl_->opts.rxBwHz, impl_->oc).empty());
    if (!impl_->usrp) return;
    auto& u = *impl_->usrp;
    const size_t c = impl_->port.chan;
    try { if (!impl_->oc.clockSources.empty()) v.clock = u.get_clock_source(0); } catch (const std::exception&) {}
    try { if (!impl_->oc.timeSources.empty())  v.time  = u.get_time_source(0); }  catch (const std::exception&) {}
    try { v.rxBwHz = u.get_rx_bandwidth(c); } catch (const std::exception&) {}
    /* ★★ THE LOCK STATE — ref_locked only when the reference is not the internal one (on an internal reference the
     *    sensor reports the external-lock PLL, which means nothing there), gps_locked only with a GPSDO fitted.
     *    Cached ~3 s: each read is a USB control transfer, the GPSDO's a UART query. */
    const double now = nowSecsMono();
    if (now - impl_->sensAt > 3.0) {
        impl_->sensAt = now;
        impl_->refL = impl_->gpsL = -1;
        std::string clk; try { clk = u.get_clock_source(0); } catch (const std::exception&) {}
        if (impl_->oc.refSensor && !clk.empty() && clk != "internal")
            try { impl_->refL = u.get_mboard_sensor("ref_locked", 0).to_bool() ? 1 : 0; } catch (const std::exception&) {}
        if (impl_->oc.gpsdo)
            try { impl_->gpsL = u.get_mboard_sensor("gps_locked", 0).to_bool() ? 1 : 0; } catch (const std::exception&) {}
    }
    v.refLocked = impl_->refL; v.gpsLocked = impl_->gpsL;
}

}  // namespace vibe
#endif  // VIBE_HAVE_USRP
