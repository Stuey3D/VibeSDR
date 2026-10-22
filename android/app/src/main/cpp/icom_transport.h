// icom_transport.h — how CI-V bytes reach an Icom: libusb to the radio's own USB serial chip (the
// default), a termios tty (an owner opt-in fallback), or memory (tests and the fake radio).
//
// ★★★ THE MODEM LINES ARE A TRANSMIT PATH (plan §3.3). The IC-7300 menu has USB SEND and USB
//     Keying (CW/RTTY) = OFF/DTR/RTS, and digital-mode owners routinely set USB SEND = RTS. In that
//     state ASSERTING RTS KEYS THE TRANSMITTER — and Linux asserts DTR and RTS when a tty is
//     opened (cp210x dtr_rts() on open, before user space can intervene; macOS does the same).
//     So the default transport never opens a tty at all:
//       • CP210x (the 7300's CP2102, 10c4:ea60) over libusb — vendor requests SET_FLOW "held
//         inactive" and SET_MHS both-low are the FIRST things sent, before the UART is even
//         enabled, and again after every setting. DTR and RTS are never asserted, by us or by a
//         kernel driver (libusb detaches cp210x from that one interface for the session).
//       • CDC-ACM (native-USB Icoms, VID 0c26 — 705/9700/MK2 [unverified]) over libusb —
//         SET_CONTROL_LINE_STATE(0) first, then the line coding, then (0) again.
//       • The tty is a FALLBACK the owner must switch on (`icomTtyOk`), because no ordering of
//         open/ioctl can stop the kernel's own raise on open. It clears the lines as the very next
//         syscall and turns HUPCL off, and the setup page says plainly what the risk is. ★ On macOS
//         this is the likely path (a system driver owns the CP210x and libusb cannot claim it
//         without root) — see the plan's "Open for Stuart".
//   ★ linesHeldLow() is what the setup page reports: true only for the libusb paths.
//
// ★ No transport ever writes on its own. IcomSource::sendFrame — after civ::mayTransmit — is the
//   only caller of write(), and there is no other way to reach a radio from this codebase.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace icom {

class CivTransport {
public:
    virtual ~CivTransport() = default;
    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;
    /** Up to `max` bytes, waiting at most `timeoutMs`. 0 = nothing yet; -1 = the device is GONE. */
    virtual int  read(uint8_t* b, int max, int timeoutMs) = 0;
    /** ★ PRIVATE TO IcomSource::sendFrame. Whole frames only, already allow-listed. */
    virtual bool write(const uint8_t* b, int n) = 0;
    virtual const char* kind() const = 0;          ///< "cp210x" | "cdc-acm" | "tty" | "memory"
    virtual bool linesHeldLow() const = 0;
    /** ★ ANDROID ONLY (the "fd:" transport): a FRESH UsbManager descriptor for the same radio after it left the
     *  bus and came back — usbfs binds an fd to the device INSTANCE it was opened on, so the old one answers
     *  -ENODEV for ever. Ownership passes here; the next open() adopts it. False (and the caller keeps the fd)
     *  for every other transport. Any thread. */
    virtual bool offerFd(int fd) { (void)fd; return false; }
    /** ★ The "fd:" transport has no usable descriptor and none waiting — only the app can open a new one. */
    virtual bool wantsFd() const { return false; }
};

/** An Icom-looking device on the USB bus, found WITHOUT sending it a byte.
 *  ★★ HOW AN ICOM IS TOLD FROM ANY OTHER CP2102: the radio programs its own model into the chip's
 *     USB serial string ("IC-7300 03001234" — the /dev/serial/by-id/…_IC-7300_03001234-if00 path
 *     owners see). A CP2102 whose serial does not start with a model we know is somebody's GPS or
 *     programming cable, and is never claimed — an owner can still name it in the config
 *     (`icomPort`). On Linux the serial is read from sysfs (no open at all); elsewhere a libusb
 *     open + string descriptor + close, with no claim and nothing written. */
struct UsbIcom {
    std::string path;        ///< "usb:<bus>-<port.port…>" — stable while it stays in the same socket
    uint16_t vid = 0, pid = 0;
    std::string serial;      ///< as the device reports it
    std::string model;       ///< "IC-7300", or "" when only the vendor says Icom
    bool cdcAcm = false;     ///< native USB (VID 0c26) rather than a CP210x
};
std::vector<UsbIcom> listUsbIcoms();

/** "usb" (the first Icom found), "usb:<bus>-<ports>", a /dev path (tty — needs ttyOk), or "fd:<n>".
 *  ★★ "fd:<n>" IS ANDROID'S DOOR (plan Phase 4): an app may not enumerate USB, so UsbManager opens the radio and
 *     hands us the descriptor, which libusb WRAPS (libusb_wrap_sys_device — the way every other radio reaches the
 *     engine on Android). Then the SAME CP210x/CDC code as the "usb" path runs on it: lines held low before the
 *     UART is enabled, and there is no kernel tty on Android to raise DTR/RTS behind our back. The transport takes
 *     OWNERSHIP of <n> (pass a dup). ★ A CP210x whose USB serial does not name an Icom is refused before a single
 *     byte is written to its UART — the app checked the same thing, this is the second look. */
std::unique_ptr<CivTransport> makeTransport(const std::string& spec, int baud, bool ttyOk, std::string& err);

/** The far end of a MemoryTransport: a fake radio, or a test. Called on the transport's thread. */
class CivPeer {
public:
    virtual ~CivPeer() = default;
    virtual void fromController(const uint8_t* b, int n) = 0;
    /** Bytes the radio would have sent by now; may block up to timeoutMs to pace a stream. */
    virtual int  toController(uint8_t* b, int max, int timeoutMs) = 0;
    virtual bool gone() const { return false; }
};
std::unique_ptr<CivTransport> makeMemoryTransport(CivPeer* peer);

/** Where on the bus the transport's device is, for finding its sibling audio codec (Linux sysfs
 *  name like "1-1.2.3"); empty when unknown (tty, memory). */
std::string transportUsbPath(const CivTransport* t);

}  // namespace icom
