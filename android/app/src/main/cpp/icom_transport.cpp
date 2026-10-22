// icom_transport.cpp — see icom_transport.h. ★ Read §3.3 of docs/ICOM-SUPPORT-PLAN.md before
// changing the ORDER of anything in open(): the order is the safety property.
#include "icom_transport.h"
#include "icom_models.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#if __has_include(<libusb.h>)
#  include <libusb.h>
#  define VIBE_ICOM_LIBUSB 1
#endif
#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#  define VIBE_ICOM_TTY 1
#endif

namespace icom {

namespace {

constexpr uint16_t kSilabsVid = 0x10c4;
constexpr uint16_t kCp2102Pid = 0xea60;   // CP2102/CP2109 — the IC-7300's
constexpr uint16_t kCp2105Pid = 0xea70;   // dual CP2105 — some later Icoms [unverified]
constexpr uint16_t kIcomVid   = 0x0c26;   // native-USB Icoms [unverified per model]

#if VIBE_ICOM_LIBUSB
std::string portPath(libusb_device* d) {
    uint8_t ports[8]; const int n = libusb_get_port_numbers(d, ports, 8);
    std::string s = "usb:" + std::to_string(libusb_get_bus_number(d)) + "-";
    for (int i = 0; i < n; i++) { if (i) s += "."; s += std::to_string(ports[i]); }
    return s;
}
/** The device's serial WITHOUT touching it where we can (Linux sysfs), else a bare open. */
std::string readSerial(libusb_device* d, const libusb_device_descriptor& dd) {
#if defined(__linux__)
    const std::string p = "/sys/bus/usb/devices/" + portPath(d).substr(4) + "/serial";   // "usb:1-1.2" → 1-1.2
    if (FILE* f = std::fopen(p.c_str(), "r")) {
        char buf[128] = {0};
        const bool got = std::fgets(buf, sizeof buf, f) != nullptr;
        std::fclose(f);
        if (got) { std::string s(buf); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
    }
#endif
    if (!dd.iSerialNumber) return "";
    libusb_device_handle* h = nullptr;
    if (libusb_open(d, &h) != 0 || !h) return "";
    unsigned char buf[128] = {0};
    const int n = libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, buf, sizeof buf - 1);
    libusb_close(h);
    return n > 0 ? std::string((const char*)buf, (size_t)n) : std::string();
}
#endif

// ── libusb: CP210x or CDC-ACM ──────────────────────────────────────────────────────────────
#if VIBE_ICOM_LIBUSB
class UsbTransport final : public CivTransport {
public:
    UsbTransport(std::string spec, int baud) : spec_(std::move(spec)), baud_(baud) {}
    ~UsbTransport() override { close(); }

    bool open(std::string& err) override {
        if (libusb_init(&ctx_) != 0) { err = "libusb could not start"; ctx_ = nullptr; return false; }
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx_, &list);
        libusb_device* pick = nullptr;
        for (ssize_t i = 0; i < n && !pick; i++) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            const bool cp = dd.idVendor == kSilabsVid && (dd.idProduct == kCp2102Pid || dd.idProduct == kCp2105Pid);
            const bool acm = dd.idVendor == kIcomVid;
            if (!cp && !acm) continue;
            const std::string path = portPath(list[i]);
            if (spec_ == "usb") {
                // ★ "usb" = the first device that SAYS it is an Icom. A plain CP2102 is never guessed at.
                if (cp && !modelBySerial(readSerial(list[i], dd).c_str())) continue;
            } else if (spec_ != path) continue;
            pick = list[i]; cdc_ = acm; path_ = path;
        }
        if (pick) libusb_ref_device(pick);
        if (list) libusb_free_device_list(list, 1);
        if (!pick) {
            err = spec_ == "usb" ? "no Icom radio found on USB (is it switched on and plugged in?)"
                                 : "no Icom radio at " + spec_;
            return false;
        }
        const int rc = libusb_open(pick, &h_);
        libusb_unref_device(pick);
        if (rc != 0 || !h_) { err = std::string("could not open the radio's USB port: ") + libusb_error_name(rc); h_ = nullptr; return false; }
        if (!findEndpoints(err)) { close(); return false; }
        // ★ The kernel driver (cp210x / cdc_acm) gives way for this session and comes back on close.
        //   It has NOT opened a tty unless another program holds one — see the plan's exclusive-port note.
        libusb_set_auto_detach_kernel_driver(h_, 1);
        for (int i = 0; i < nIfc_; i++) {
            const int c = libusb_claim_interface(h_, ifc_[i]);
            if (c != 0) {
                err = c == LIBUSB_ERROR_BUSY
                    ? "another program is using the radio's USB port — close wfview, wfweb, WSJT-X or any logger that talks to it"
                    : std::string("could not claim the radio's USB port: ") + libusb_error_name(c);
                close(); return false;
            }
            claimed_[i] = true;
        }
        if (!(cdc_ ? setupCdc(err) : setupCp210x(err))) { close(); return false; }
        return true;
    }

    void close() override {
        if (h_) {
            // ★ Lines low on the way out too, then the UART off: nothing is left asserted for whoever
            //   opens it next (the kernel driver re-binds on release).
            if (!cdc_) { mhsLow(); vendor(0x00, 0x0000, nullptr, 0); }
            else       { ctrlLines0(); }
            for (int i = nIfc_ - 1; i >= 0; i--) if (claimed_[i]) { libusb_release_interface(h_, ifc_[i]); claimed_[i] = false; }
            libusb_close(h_); h_ = nullptr;
        }
        if (ctx_) { libusb_exit(ctx_); ctx_ = nullptr; }
    }

    int read(uint8_t* b, int max, int timeoutMs) override {
        if (!h_) return -1;
        int got = 0;
        const int rc = libusb_bulk_transfer(h_, epIn_, b, max, &got, (unsigned)(timeoutMs > 0 ? timeoutMs : 1));
        if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) return got;
        if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE) return -1;
        return got;
    }
    bool write(const uint8_t* b, int n) override {
        if (!h_) return false;
        int sent = 0;
        const int rc = libusb_bulk_transfer(h_, epOut_, const_cast<uint8_t*>(b), n, &sent, 500);
        return rc == 0 && sent == n;
    }
    const char* kind() const override { return cdc_ ? "cdc-acm" : "cp210x"; }
    bool linesHeldLow() const override { return true; }
    const std::string& path() const { return path_; }

private:
    std::string spec_, path_;
    int baud_;
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* h_ = nullptr;
    bool cdc_ = false;
    int ifc_[2] = {0, 0}; bool claimed_[2] = {false, false}; int nIfc_ = 0;
    int ctrlIfc_ = 0;
    unsigned char epIn_ = 0x81, epOut_ = 0x01;

    bool findEndpoints(std::string& err) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(h_), &cfg) != 0 || !cfg) { err = "the radio's USB descriptor could not be read"; return false; }
        bool found = false;
        for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
            const libusb_interface_descriptor& id = cfg->interface[i].altsetting[0];
            if (cdc_ && id.bInterfaceClass == 0x02) { ctrlIfc_ = id.bInterfaceNumber; continue; }
            if (cdc_ && id.bInterfaceClass != 0x0A) continue;          // CDC data interface only
            unsigned char in = 0, out = 0;
            for (int e = 0; e < id.bNumEndpoints; e++) {
                const auto& ep = id.endpoint[e];
                if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep.bEndpointAddress & 0x80) in = ep.bEndpointAddress; else out = ep.bEndpointAddress;
            }
            if (in && out) { epIn_ = in; epOut_ = out; ifc_[nIfc_++] = id.bInterfaceNumber; found = true; }
        }
        libusb_free_config_descriptor(cfg);
        if (!found) { err = "the radio's USB port has no serial endpoints"; return false; }
        if (cdc_ && ctrlIfc_ != ifc_[0]) ifc_[nIfc_++] = ctrlIfc_;
        return true;
    }

    // CP210x vendor requests (Silicon Labs AN571). bmRequestType 0x41: host→device, vendor, interface.
    int vendor(uint8_t req, uint16_t value, unsigned char* data, uint16_t len) {
        return libusb_control_transfer(h_, 0x41, req, value, (uint16_t)ifc_[0], data, len, 500);
    }
    /** ★★★ DTR LOW, RTS LOW — with both MASK bits set, so the write actually applies to both. */
    bool mhsLow() { return vendor(0x07 /*SET_MHS*/, 0x0300, nullptr, 0) >= 0; }
    bool setupCp210x(std::string& err) {
        // ★★★ THE ORDER IS THE POINT. Lines low and flow control "held inactive" BEFORE the UART is
        //     enabled, so the enable cannot raise them; then again after each setting, belt and braces.
        unsigned char flow[16] = {0};   // ulControlHandshake=0 (DTR held inactive), ulFlowReplace=0 (RTS held inactive), no handshake
        bool okAll = mhsLow();
        okAll &= vendor(0x13 /*SET_FLOW*/, 0, flow, sizeof flow) >= 0;
        okAll &= vendor(0x00 /*IFC_ENABLE*/, 0x0001, nullptr, 0) >= 0;
        okAll &= mhsLow();
        unsigned char rate[4] = {(unsigned char)(baud_ & 0xFF), (unsigned char)((baud_ >> 8) & 0xFF),
                                 (unsigned char)((baud_ >> 16) & 0xFF), (unsigned char)((baud_ >> 24) & 0xFF)};
        okAll &= vendor(0x1E /*SET_BAUDRATE*/, 0, rate, 4) >= 0;
        okAll &= vendor(0x03 /*SET_LINE_CTL*/, 0x0800, nullptr, 0) >= 0;   // 8 data, no parity, 1 stop
        okAll &= vendor(0x13, 0, flow, sizeof flow) >= 0;
        okAll &= mhsLow();
        vendor(0x12 /*PURGE*/, 0x000F, nullptr, 0);
        if (!okAll) err = "the radio's USB serial chip refused its settings";
        return okAll;
    }
    // CDC-ACM class requests. bmRequestType 0x21: host→device, class, interface.
    bool ctrlLines0() { return libusb_control_transfer(h_, 0x21, 0x22 /*SET_CONTROL_LINE_STATE*/, 0x0000, (uint16_t)ctrlIfc_, nullptr, 0, 500) >= 0; }
    bool setupCdc(std::string& err) {
        bool okAll = ctrlLines0();      // ★ DTR=0 RTS=0 before anything else
        unsigned char lc[7] = {(unsigned char)(baud_ & 0xFF), (unsigned char)((baud_ >> 8) & 0xFF),
                               (unsigned char)((baud_ >> 16) & 0xFF), (unsigned char)((baud_ >> 24) & 0xFF), 0, 0, 8};
        okAll &= libusb_control_transfer(h_, 0x21, 0x20 /*SET_LINE_CODING*/, 0, (uint16_t)ctrlIfc_, lc, 7, 500) >= 0;
        okAll &= ctrlLines0();
        if (!okAll) err = "the radio's USB port refused its settings";
        return okAll;
    }
};
#endif

// ── termios: the owner's opt-in fallback ───────────────────────────────────────────────────
#if VIBE_ICOM_TTY
speed_t speedFor(int baud) {
    switch (baud) {
        case 4800: return B4800; case 9600: return B9600; case 19200: return B19200;
        case 38400: return B38400; case 57600: return B57600; case 115200: return B115200;
    }
    return B115200;
}
class TtyTransport final : public CivTransport {
public:
    TtyTransport(std::string path, int baud) : path_(std::move(path)), baud_(baud) {}
    ~TtyTransport() override { close(); }
    bool open(std::string& err) override {
        // ★★★ THE KERNEL RAISES DTR AND RTS INSIDE THIS open() — nothing in user space can get ahead
        //     of it. The very next syscall drops them, which shortens the glitch; it cannot remove it.
        //     That is why this path is behind the owner's switch (see the header).
        fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) { err = "could not open " + path_ + ": " + std::strerror(errno); return false; }
        int bits = TIOCM_DTR | TIOCM_RTS;
        ioctl(fd_, TIOCMBIC, &bits);
        termios t{};
        if (tcgetattr(fd_, &t) != 0) { err = "not a serial port: " + path_; close(); return false; }
        cfmakeraw(&t);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(CRTSCTS | HUPCL);        // ★ no hardware flow (it drives RTS), no raise-on-close
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        cfsetispeed(&t, speedFor(baud_)); cfsetospeed(&t, speedFor(baud_));
        tcsetattr(fd_, TCSANOW, &t);
        ioctl(fd_, TIOCMBIC, &bits);
        tcflush(fd_, TCIOFLUSH);
        return true;
    }
    void close() override {
        if (fd_ >= 0) { int bits = TIOCM_DTR | TIOCM_RTS; ioctl(fd_, TIOCMBIC, &bits); ::close(fd_); fd_ = -1; }
    }
    int read(uint8_t* b, int max, int timeoutMs) override {
        if (fd_ < 0) return -1;
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeoutMs);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (r == 0) return 0;
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return -1;
        const ssize_t n = ::read(fd_, b, (size_t)max);
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        return (int)n;
    }
    bool write(const uint8_t* b, int n) override {
        if (fd_ < 0) return false;
        int off = 0;
        for (int tries = 0; off < n && tries < 50; tries++) {
            const ssize_t w = ::write(fd_, b + off, (size_t)(n - off));
            if (w > 0) off += (int)w;
            else if (w < 0 && errno != EAGAIN && errno != EINTR) return false;
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return off == n;
    }
    const char* kind() const override { return "tty"; }
    bool linesHeldLow() const override { return false; }
private:
    std::string path_;
    int baud_;
    int fd_ = -1;
};
#endif

// ── memory: tests and the fake radio ───────────────────────────────────────────────────────
class MemoryTransport final : public CivTransport {
public:
    explicit MemoryTransport(CivPeer* p) : peer_(p) {}
    bool open(std::string&) override { open_ = true; return true; }
    void close() override { open_ = false; }
    int read(uint8_t* b, int max, int timeoutMs) override {
        if (!open_ || !peer_ || peer_->gone()) return -1;
        return peer_->toController(b, max, timeoutMs);
    }
    bool write(const uint8_t* b, int n) override {
        if (!open_ || !peer_ || peer_->gone()) return false;
        peer_->fromController(b, n); return true;
    }
    const char* kind() const override { return "memory"; }
    bool linesHeldLow() const override { return true; }
private:
    CivPeer* peer_;
    bool open_ = false;
};

}  // namespace

std::vector<UsbIcom> listUsbIcoms() {
    std::vector<UsbIcom> out;
#if VIBE_ICOM_LIBUSB
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        const bool cp = dd.idVendor == kSilabsVid && (dd.idProduct == kCp2102Pid || dd.idProduct == kCp2105Pid);
        const bool acm = dd.idVendor == kIcomVid;
        if (!cp && !acm) continue;
        UsbIcom u;
        u.path = portPath(list[i]); u.vid = dd.idVendor; u.pid = dd.idProduct; u.cdcAcm = acm;
        u.serial = readSerial(list[i], dd);
        if (const Model* m = modelBySerial(u.serial.c_str())) u.model = m->name;
        // ★ A CP210x that does not name an Icom is not ours to list (someone's GPS, a programming lead).
        if (cp && u.model.empty()) continue;
        out.push_back(u);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
#endif
    return out;
}

std::unique_ptr<CivTransport> makeTransport(const std::string& spec, int baud, bool ttyOk, std::string& err) {
    if (spec.empty() || spec == "usb" || spec.rfind("usb:", 0) == 0) {
#if VIBE_ICOM_LIBUSB
        return std::unique_ptr<CivTransport>(new UsbTransport(spec.empty() ? "usb" : spec, baud));
#else
        err = "this build has no USB support"; return nullptr;
#endif
    }
    if (spec[0] == '/') {
#if VIBE_ICOM_TTY
        if (!ttyOk) {
            err = "the radio is set to a serial device (" + spec + "), and opening a serial device raises its DTR and RTS lines — "
                  "which KEYS THE TRANSMITTER if the radio's USB SEND or USB Keying is set to DTR or RTS. Use the radio's USB "
                  "port directly (icomPort=usb), or, if those menu items are OFF, switch on \"Allow a serial device\" in setup";
            return nullptr;
        }
        return std::unique_ptr<CivTransport>(new TtyTransport(spec, baud));
#else
        err = "no serial support on this platform"; return nullptr;
#endif
    }
    err = "icomPort must be usb, usb:<bus>-<port>, or a /dev path";
    return nullptr;
}

std::unique_ptr<CivTransport> makeMemoryTransport(CivPeer* peer) {
    return std::unique_ptr<CivTransport>(new MemoryTransport(peer));
}

std::string transportUsbPath(const CivTransport* t) {
#if VIBE_ICOM_LIBUSB
    if (auto u = dynamic_cast<const UsbTransport*>(t)) return u->path().rfind("usb:", 0) == 0 ? u->path().substr(4) : u->path();
#endif
    (void)t;
    return "";
}

}  // namespace icom
