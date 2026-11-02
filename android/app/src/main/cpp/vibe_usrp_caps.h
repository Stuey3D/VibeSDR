// vibe_usrp_caps.h — the USRP B2xx's POLICY, header-only and driver-free so it is tested without a radio
// (vibeserver/test-usrp-caps.cpp). V12, 2026-11-02 — docs/v12/USRP.md.
//
// What lives here, and why not in usrp_source.cpp: everything that is a DECISION rather than a UHD call —
//   · which USB IDs are a B2xx, and what a bare PID can honestly be called before the radio is opened;
//   · the sample rates offered, given the USB link speed and the host, and the default for a new radio;
//   · the RX ports as the client sees them (channel + socket) and back again;
//   · which image FILE is loaded — an owner's override first, then the one the package ships.
// usrp_source.cpp asks these and does the hardware writes itself.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace vibe {
namespace usrpcaps {

// ── USB identity ──────────────────────────────────────────────────────────────────────────────────────────────
// ★ UHD 4.9.0.1 host/lib/usrp/b200/b200_iface.hpp — the IDs its own b200 driver searches for, restated so the
//   descriptor walk (radios.cpp, usrp_source.cpp) and UHD agree on what "a B2xx" is.
constexpr uint16_t kVidEttus = 0x2500;
constexpr uint16_t kVidNi    = 0x3923;
struct UsbId { uint16_t vid, pid; const char* name; };
inline const std::vector<UsbId>& usbIds() {
    /* ★★ 0x0020 IS "B200 OR B210" — the PID cannot tell them apart (UHD reads the product from the EEPROM, after
     *    the firmware is loaded). So the pre-open name says "B200/B210", never a guess; the opened radio says what
     *    it is (UsrpSource::model()). AGENTS.md: no inferred hardware readouts. */
    static const std::vector<UsbId> ids = {
        {kVidEttus, 0x0020, "B200/B210"},
        {kVidEttus, 0x0021, "B200mini"},
        {kVidEttus, 0x0022, "B205mini"},
        {kVidEttus, 0x0023, "B206mini"},
        {kVidNi,    0x7813, "B200"},      // NI-branded
        {kVidNi,    0x7814, "B210"},      // NI-branded
    };
    return ids;
}
/** The pre-open name for a VID:PID, or nullptr when it is not a B2xx. */
inline const char* nameForUsbId(uint16_t vid, uint16_t pid) {
    for (const auto& u : usbIds()) if (u.vid == vid && u.pid == pid) return u.name;
    return nullptr;
}
/** "Ettus USRP B210 (30AD2C1)" — model from the PID (or the opened radio), serial in brackets when there is one. */
inline std::string displayName(const std::string& model, const std::string& serial) {
    std::string n = "Ettus USRP " + (model.empty() ? std::string("B2xx") : model);
    if (!serial.empty()) n += " (" + serial + ")";
    return n;
}

// ── sample rates ──────────────────────────────────────────────────────────────────────────────────────────────
/* ★★★ A CHOSEN LIST, NOT AN ENUMERATED ONE. UHD takes a continuous rate on a B2xx (it re-picks the AD9361's master
 *     clock to suit), so there is nothing to read back — these are the rates worth offering, all ones the rest of
 *     the engine already runs at for other radios.
 *  ★★ THE LINK DECIDES THE TOP. UHD's own figures (b200_impl.hpp): USB 2 carries 53.248 MB/s, USB 3 500 MB/s. At
 *     sc16 over the wire a complex sample is 4 bytes, so USB 2's ceiling is ~13 MS/s on paper; Ettus's guidance is to
 *     expect overflows well before it on a shared bus, so USB 2 tops out at 8 MS/s here. USB 3 tops out at 16 MS/s —
 *     not because the radio stops there (it does 61.44) but because no host this ships for runs our DSP that fast
 *     with audio to spare.
 *  ★★ THE HOST DECIDES THE DEFAULT, NEVER THE LIST (feedback: "benchmark sets the default, never hides" — the owner
 *     can always pick a higher one). arm64 (a Pi 4 / Pi 5 class board) starts at 2.4 MS/s, the rate every other
 *     radio here is tuned around; amd64 and Apple silicon start at 4 MS/s. Thread priority stays Network > Audio >
 *     Spectrum > Decoders whatever the rate: a too-high rate costs waterfall smoothness and decoders first. */
enum class Host { Arm64, X86_64, Apple };
inline Host thisHost() {
#if defined(__APPLE__)
    return Host::Apple;
#elif defined(__aarch64__)
    return Host::Arm64;
#else
    return Host::X86_64;
#endif
}
inline std::vector<uint32_t> ratesFor(int usbSpeed) {
    static const uint32_t all[] = { 1000000u, 2000000u, 2400000u, 3200000u, 4000000u, 5000000u,
                                    8000000u, 10000000u, 12500000u, 16000000u };
    // ★ Unknown speed (0) is treated as USB 2 — the conservative reading, never the optimistic one.
    const uint32_t top = usbSpeed >= 3 ? 16000000u : 8000000u;
    std::vector<uint32_t> out;
    for (uint32_t r : all) if (r <= top) out.push_back(r);
    return out;
}
inline uint32_t defaultRate(Host h) { return h == Host::Arm64 ? 2400000u : 4000000u; }
/** Nearest offered rate to `hz`; `hz` <= 0 = the host's default (a fresh radio with no saved rate). */
inline uint32_t nearestRate(const std::vector<uint32_t>& rates, double hz, Host h) {
    if (rates.empty()) return 0;
    if (hz <= 0) hz = defaultRate(h);
    uint32_t best = rates.front(); double bd = 1e300;
    for (uint32_t r : rates) { const double d = r > hz ? r - hz : hz - r; if (d < bd) { bd = d; best = r; } }
    return best;
}

// ── gain ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** The slider's list from the DEVICE's range (get_rx_gain_range): start..stop every `step` dB, in tenths.
 *  ★ A step below 1 dB is listed at 1 dB — the AD9361 does 1 dB in this mode, and a 0.25 dB list would be 300
 *    slider positions for no audible difference. Bad input (stop < start, step <= 0) = an empty list. */
inline std::vector<int> gainList(double startDb, double stopDb, double stepDb) {
    std::vector<int> out;
    if (!(stopDb >= startDb)) return out;
    if (!(stepDb >= 1.0)) stepDb = 1.0;
    for (double g = startDb; g <= stopDb + 1e-6 && out.size() < 512; g += stepDb) out.push_back((int)(g * 10 + (g >= 0 ? 0.5 : -0.5)));
    return out;
}
/** Nearest list entry to a requested tenths value. */
inline int nearestGain(const std::vector<int>& list, int tenth) {
    if (list.empty()) return 0;
    int best = list.front();
    for (int g : list) if ((g > tenth ? g - tenth : tenth - g) < (best > tenth ? best - tenth : tenth - best)) best = g;
    return best;
}

// ── RX ports ──────────────────────────────────────────────────────────────────────────────────────────────────
/* ★★ ONE LIST OF (channel, socket), labelled as the case is. A B210 has channels A and B, each with TX/RX and RX2;
 *    a B200 / B200mini has one channel. RX2 first: it is the receive-only socket, and the default UHD itself uses.
 *  ★★★ A RADIO WITH ONE CHOICE PUBLISHES NONE (empty list), so the client draws no selector — "a control whose
 *      every use is a no-op" (AGENTS.md). */
struct Port { size_t chan; std::string socket; };
inline std::string portLabel(size_t chan, size_t nChans, const std::string& socket) {
    if (nChans <= 1) return socket;
    return std::string(1, (char)('A' + (chan < 26 ? chan : 25))) + ": " + socket;
}
/** `sockets[c]` = get_rx_antennas(c) for every channel c. */
inline std::vector<Port> ports(const std::vector<std::vector<std::string>>& sockets) {
    std::vector<Port> out;
    for (size_t c = 0; c < sockets.size(); c++) {
        std::vector<std::string> s = sockets[c];
        // RX2 to the front, keeping UHD's order otherwise
        for (size_t i = 0; i < s.size(); i++) if (s[i] == "RX2" && i) { s.erase(s.begin() + (long)i); s.insert(s.begin(), "RX2"); break; }
        // ★ Never "CAL" or "LOCAL" style internal paths, should a later UHD list them — only real sockets.
        for (const auto& a : s) if (a == "RX2" || a == "TX/RX") out.push_back({c, a});
    }
    return out;
}
inline std::vector<std::string> labels(const std::vector<Port>& ps, size_t nChans) {
    std::vector<std::string> out;
    if (ps.size() < 2) return out;                     // ★ one choice = no selector
    for (const auto& p : ps) out.push_back(portLabel(p.chan, nChans, p.socket));
    return out;
}
/** The port a label names; false = not one of this radio's (refused, never guessed). Case-insensitive. */
inline bool portFor(const std::vector<Port>& ps, size_t nChans, const std::string& label, Port& out) {
    auto low = [](std::string s) { for (auto& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); return s; };
    const std::string want = low(label);
    for (const auto& p : ps) if (low(portLabel(p.chan, nChans, p.socket)) == want) { out = p; return true; }
    return false;
}

// ── images ────────────────────────────────────────────────────────────────────────────────────────────────────
/** UHD's own file names for the B2xx images (b200_iface.hpp). The FPGA one depends on the product UHD reports. */
inline const char* firmwareFile() { return "usrp_b200_fw.hex"; }
inline const char* fpgaFileFor(const std::string& product) {
    if (product == "B200")     return "usrp_b200_fpga.bin";
    if (product == "B210")     return "usrp_b210_fpga.bin";
    if (product == "B200mini") return "usrp_b200mini_fpga.bin";
    if (product == "B205mini" || product == "B206mini") return "usrp_b205mini_fpga.bin";   // ★ UHD maps B206 to it too
    return nullptr;
}
inline bool fileExists(const std::string& p) { struct stat st{}; return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
/* ★★★ WHERE OUR IMAGES ARE, and the ONE door for a clone. The search, in order:
 *     1. the owner's override directory — empty on every install until an owner puts a file there. It is how a clone
 *        that needs a THIRD-PARTY FPGA image (the Artix-7 "LibreSDR B210/B220 mini" boards) is run, and the only
 *        way: we do not ship such images (no stated licence, no source we can point to). Linux
 *        /etc/vibeserver/uhd-images, the Mac ~/Library/Application Support/VibeServer/uhd-images.
 *     2. the images the package ships (Linux /usr/share/vibeserver/uhd-images, the Mac app's Resources/uhd-images).
 *   Per FILE, not per directory: an override holding only usrp_b210_fpga.bin still takes our firmware.
 *   "" = not found anywhere (a broken install — the caller says so in words, never "download the images"). */
inline std::string resolveImage(const std::string& file, const std::vector<std::string>& dirs) {
    for (const auto& d : dirs) {
        if (d.empty()) continue;
        const std::string p = d + (d.back() == '/' ? "" : "/") + file;
        if (fileExists(p)) return p;
    }
    return "";
}

}  // namespace usrpcaps
}  // namespace vibe
