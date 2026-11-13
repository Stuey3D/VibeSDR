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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
/* ★★ A SERIAL WE CAN USE (2026-11-02, the first real B210, a tester's clone: "Ettus USRP B200/B210 (31A0B7C ?), serial
 *  31A0B7C ?" and ADD AS A NEW RADIO refused "that is not a usable serial"). libusb's ASCII descriptor read turns any byte
 *  it cannot show into '?', and this board's descriptor carries "31A0B7C", a space and such a byte. The serial is its
 *  leading run of the characters a config serial may hold ([A-Za-z0-9._:-], vsconfig::validSerial) — cut at the first
 *  other one, so "31A0B7C ?" is "31A0B7C" everywhere (the Setup page, the config, matching the radio when it opens). */
inline std::string cleanSerial(const std::string& raw) {
    std::string out;
    size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++;   // leading blanks
    for (; i < raw.size() && out.size() < 64; i++) {
        const unsigned char c = (unsigned char)raw[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) break;
        out.push_back((char)c);
    }
    return out;
}

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
/** ★★★ THE WIDE RATES, BEHIND THE BENCHMARK (V12, 2026-11-03 — a tester's B210 on an 11th-gen i7, wanting 82–108 MHz,
 *  the whole FM band with the Dutch pirate end, in ONE locked-range capture: 26 MHz of band → 30.72 MS/s).
 *  ★ What the B2xx does (UHD's B200 manual and b200_impl.cpp): the AD9361's master clock (MCR) runs up to 61.44 MHz with
 *    one channel streaming (30.72 MHz with two); with no MCR given — we give none — UHD picks the highest MCR that is
 *    the rate times a power of two, and the FPGA's halfbands decimate to the rate. So 20 → MCR 40 (÷2), 25 → 50 (÷2),
 *    28 → 56 (÷2), 30.72 → 61.44 (÷2), 32 → 32 (÷1, the AD9361's own filters only). All five are clean.
 *  ★ The link: sc16 is 4 bytes a sample — 30.72 MS/s is 123 MB/s and 32 MS/s 128 MB/s, inside USB 3's ~500 MB/s and
 *    inside what Ettus quotes for one channel over USB 3 (56 MHz of bandwidth, 61.44 MS/s on a good controller). 32 is
 *    the top offered: past it the server's FFT cap (32768 bins) leaves under 1 kHz of resolution per bin and a listener's
 *    channel the same, and nothing here has been measured that fast.
 *  ★★ OFFERED ONLY WHEN THIS MACHINE'S BENCHMARK SAYS ITS FRONT END KEEPS UP AT THAT RATE WITH HEADROOM — `gateHz`, the
 *     highest rate the saved benchmark passed (vibe_benchmark_wide.h; the locked-range verdict for a locked radio, the
 *     full-radio one otherwise). 0 = never measured: none of these. USB 3 only. ★ AND NEVER HIDING ONE IN USE: `keepHz`
 *     (the radio's saved rate or locked rate) stays offered even if a later benchmark says no — the default is set by the
 *     benchmark, the owner's choice is never taken away. */
inline const std::vector<uint32_t>& gatedRates() {
    /* ★★ V12 beta74 (Stuart, 2026-11-13: "on the B210 do we go up to 56MSPS" … "the dude is running an 11th gen i7"):
     *  40, 50 and 56 MS/s — the AD9361's single-channel top. Like the rest of this list they are offered ONLY where the
     *  benchmark measured this machine carrying them (ratesFor), so a smaller host never sees them. 56 MS/s of sc16 is
     *  ~224 MB/s over USB 3; UHD chooses the B2xx master clock for the rate asked (none is set here). */
    static const std::vector<uint32_t> g = { 20000000u, 25000000u, 28000000u, 30720000u, 32000000u,
                                             40000000u, 50000000u, 56000000u };
    return g;
}
inline std::vector<uint32_t> ratesFor(int usbSpeed, double gateHz = 0, double keepHz = 0) {
    /* ★★ 2.048 MS/s IS LISTED (V12, 2026-11-04): DAB's own rate, an exact 32.768 MHz / 16 on the AD9361. Without it DAB
     *  asked for 2.048, got the nearest (2.0), failed its readback and fell to 2.4 — two re-rates on entry, and two more
     *  on every block change after (see the DAB entry in local_sdr_shim.cpp). */
    static const uint32_t all[] = { 1000000u, 2000000u, 2048000u, 2400000u, 3200000u, 4000000u, 5000000u,
                                    8000000u, 10000000u, 12500000u, 16000000u };
    // ★ Unknown speed (0) is treated as USB 2 — the conservative reading, never the optimistic one.
    const uint32_t top = usbSpeed >= 3 ? 16000000u : 8000000u;
    std::vector<uint32_t> out;
    for (uint32_t r : all) if (r <= top) out.push_back(r);
    if (usbSpeed >= 3)
        for (uint32_t r : gatedRates())
            if ((gateHz > 0 && r <= gateHz + 0.5) || (keepHz > 0 && std::fabs((double)r - keepHz) < 1.0)) out.push_back(r);
    return out;
}
/** ★ The gate's verdict, set once from the saved benchmark (main.cpp, at start-up and after a run) and read when a radio
 *  opens (usrp_source.cpp). Two figures: the LOCKED-RANGE front end (the channeliser + its listeners) and the FULL-RADIO
 *  one (the shared pipeline); a radio is judged by the one it runs. 0 = no verdict (never measured, or too slow). */
struct RateGate { double lockedHz = 0, fullHz = 0; bool locked = false; };
inline RateGate& rateGate() { static RateGate g; return g; }
/** The figure THIS radio is judged by: the locked-range one for a locked-range radio, the full-radio one otherwise. */
inline double gateHzFor(const RateGate& g) { return g.locked ? g.lockedHz : g.fullHz; }
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
/* ★★ "RX2 (A)", NOT "A: RX2". The owner's per-band aerial list (vibe_bands.h parseAntennaList) splits a rule at its
 *    LAST colon — "0-400MHz A: RX2" would read as the band "0-400MHz A" — so a label must never contain one. The
 *    socket first, the channel in brackets, the way the case reads. */
inline std::string portLabel(size_t chan, size_t nChans, const std::string& socket) {
    if (nChans <= 1) return socket;
    return socket + " (" + std::string(1, (char)('A' + (chan < 26 ? chan : 25))) + ")";
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

// ── the BOARD'S OWN images: the owner's override folder (V12, 2026-11-02 — a tester's clone) ────────────────────
/* ★★★ A CLONE THAT IS NOT ETTUS'S DESIGN NEEDS ITS MAKER'S FPGA IMAGE (the Artix-7 "LibreSDR B210/B220 mini" boards;
 *     a tester's B210 clone: "this unit requires non-official firmware, as it is not an official Ettus product").
 *     We do NOT ship such images (no stated licence, no source we can point to) — the OWNER uploads the one their
 *     seller provides, from the Setup page, into the override folder resolveImage searches first.
 *  ★★ THE NAME IS OURS, NEVER THE CLIENT'S: the owner picks which of UHD's own file names it is (a closed list), and
 *     the bytes are written under exactly that name — so no path, no "..", nothing else in the folder can be named. */
inline const std::vector<std::string>& ownerImageNames() {
    static const std::vector<std::string> n = {
        "usrp_b210_fpga.bin", "usrp_b200_fpga.bin", "usrp_b200mini_fpga.bin", "usrp_b205mini_fpga.bin", "usrp_b200_fw.hex" };
    return n;
}
inline bool ownerImageName(const std::string& n) {
    for (const auto& x : ownerImageNames()) if (x == n) return true;
    return false;
}
/** The owner's override folder. ★ VIBE_UHD_OVERRIDE_DIR first (tests). Linux: /etc/vibeserver/uhd-images —
 *  /etc/vibeserver belongs to the daemon and is in both units' ReadWritePaths. The Mac: Application Support. */
inline std::string ownerImageDir() {
    if (const char* e = std::getenv("VIBE_UHD_OVERRIDE_DIR")) if (*e) return e;
#if defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) if (*h) return std::string(h) + "/Library/Application Support/VibeServer/uhd-images";
    return "";
#else
    return "/etc/vibeserver/uhd-images";
#endif
}
constexpr size_t kFpgaMin = 1024 * 1024, kFpgaMax = 16 * 1024 * 1024;    // a B2xx bitstream is 2-10 MB
constexpr size_t kHeadBytes = 256;                                        // how much of the start is judged
constexpr size_t kFwMin = 16 * 1024, kFwMax = 2 * 1024 * 1024;           // the FX3 firmware hex is ~0.5 MB
/** The size the upload may be, before a byte is read: "" = fine, else the refusal in words. */
inline std::string refuseImageSize(const std::string& name, size_t bytes) {
    if (!ownerImageName(name)) return "that is not one of the file names UHD loads for a B2xx";
    const bool fw = name == "usrp_b200_fw.hex";
    const size_t lo = fw ? kFwMin : kFpgaMin, hi = fw ? kFwMax : kFpgaMax;
    if (bytes < lo) return std::string("that file is too small to be ") + (fw ? "the FX3 firmware" : "an FPGA image") + " (" + std::to_string(bytes) + " bytes)";
    if (bytes > hi) return std::string("that file is too large to be ") + (fw ? "the FX3 firmware" : "an FPGA image") + " (" + std::to_string(bytes) + " bytes)";
    return "";
}
/* ★★★ WHAT A FILE IS, FROM ITS FIRST BYTES (a tester's clone image, 2026-11-02, and Ettus's own):
 *   · a Xilinx bitstream carries the SYNC WORD AA 99 55 66 after a run of FF padding, early in the file;
 *   · a 7-SERIES one (Artix-7 / Kintex-7 — the clone boards) has the BUS-WIDTH DETECT pattern 00 00 00 BB 11 22 00 44
 *     before the sync word; a SPARTAN-6 one (a genuine B200 / B210 / B200mini / B205mini) has not;
 *   · UHD loads the FX3 firmware as INTEL HEX (text, every record starting ':'); a Cypress boot image (.img, "CY" at
 *     the front) is for flashing the board's EEPROM with Cypress's tools, and UHD cannot load one. */
enum class ImageKind { Unknown, Spartan6, Series7, IntelHex, Fx3Img };
inline ImageKind classifyImage(const unsigned char* b, size_t n) {
    if (n >= 2 && b[0] == 'C' && b[1] == 'Y') return ImageKind::Fx3Img;
    size_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) i++;
    if (i < n && b[i] == ':') return ImageKind::IntelHex;
    const size_t lim = n < kHeadBytes ? n : kHeadBytes;
    bool busWidth = false;
    for (size_t k = 0; k + 4 <= lim; k++) {
        if (k + 8 <= lim && b[k] == 0x00 && b[k + 1] == 0x00 && b[k + 2] == 0x00 && b[k + 3] == 0xBB
            && b[k + 4] == 0x11 && b[k + 5] == 0x22 && b[k + 6] == 0x00 && b[k + 7] == 0x44) busWidth = true;
        if (b[k] == 0xAA && b[k + 1] == 0x99 && b[k + 2] == 0x55 && b[k + 3] == 0x66)
            return busWidth ? ImageKind::Series7 : ImageKind::Spartan6;
    }
    return ImageKind::Unknown;
}
inline const char* imageKindWords(ImageKind k) {
    switch (k) {
        case ImageKind::Series7:  return "7-series (Kintex/Artix) image \xe2\x80\x94 for clone boards";
        case ImageKind::Spartan6: return "Spartan-6 image \xe2\x80\x94 a genuine B2xx's";
        case ImageKind::IntelHex: return "FX3 firmware (Intel HEX)";
        case ImageKind::Fx3Img:   return "FX3 boot image (.img)";
        default:                  return "not recognised";
    }
}
/** The first bytes of an upload: "" = acceptable, else the refusal in words. ★ It cannot PROVE an FPGA image is the
 *  right one — only the radio can — but only a plausible Xilinx bitstream goes in as an FPGA image and only Intel HEX
 *  as the firmware, so the usual mistakes (the zip it came in, a web page saved instead of the file, the firmware
 *  under the FPGA's name, an EEPROM .img) are refused before the radio meets them. */
inline std::string refuseImageHead(const std::string& name, const unsigned char* b, size_t n) {
    if (n >= 2 && b[0] == 'P' && b[1] == 'K') return "that is a zip archive \xe2\x80\x94 unzip it and upload the file inside";
    if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) return "that is a gzip archive \xe2\x80\x94 unpack it and upload the file inside";
    const ImageKind k = classifyImage(b, n);
    if (name == "usrp_b200_fw.hex") {
        if (k == ImageKind::IntelHex) return "";
        if (k == ImageKind::Fx3Img) return "that is an FX3 boot image (.img), for flashing the board's EEPROM with Cypress's tools \xe2\x80\x94 UHD loads the firmware only as Intel HEX (.hex); most clones need Ettus's firmware anyway, which VibeServer already has";
        return "that is not FX3 firmware: UHD loads it as an Intel HEX text file, which starts with ':'";
    }
    if (k == ImageKind::Spartan6 || k == ImageKind::Series7) return "";
    if (k == ImageKind::IntelHex || k == ImageKind::Fx3Img) return "that is FX3 firmware, not an FPGA image \xe2\x80\x94 choose usrp_b200_fw.hex as its name, or pick the FPGA .bin";
    size_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n' || b[i] == 0xEF || b[i] == 0xBB || b[i] == 0xBF)) i++;
    if (i < n && b[i] == '<') return "that is a web page or XML, not an FPGA image \xe2\x80\x94 save the file itself, not the page it is on";
    return "that is not a Xilinx FPGA bitstream (no sync word AA995566 near its start) \xe2\x80\x94 an FPGA image for a B2xx is a raw .bin";
}
/** ★★ THE CLONE'S FAILURE, SAID AS WHAT TO DO. A board whose FPGA is not the part Ettus's image is built for fails
 *  the FPGA load (UHD: "fx3 is in state 5", or a timeout / failure loading or initialising the FPGA). The owner
 *  needs one sentence, not UHD's internals: which file to upload, and where. "" = not that failure. */
inline std::string cloneFpgaAdvice(const std::string& uhdMessage, const std::string& product) {
    std::string m = uhdMessage;
    for (auto& ch : m) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    const bool fx3State = m.find("fx3 is in state") != std::string::npos;
    const bool fpgaLoad = m.find("fpga") != std::string::npos &&
        (m.find("load") != std::string::npos || m.find("timed out") != std::string::npos || m.find("timeout") != std::string::npos
         || m.find("failed to init") != std::string::npos || m.find("state") != std::string::npos);
    if (!fx3State && !fpgaLoad) return "";
    const char* f = fpgaFileFor(product);
    const std::string file = f ? f : "usrp_b210_fpga.bin";
    return "This board needs its maker's FPGA image \xe2\x80\x94 upload " + file +
           " in Setup \xe2\x86\x92 this radio (Board's own firmware / FPGA image). UHD said: " + uhdMessage;
}

/** ★★ ANOTHER PROGRAM HAS THE RADIO — said plainly, with what to do. A B2xx held by another process (OpenWebRX+'s
 *  owrx_connector through SoapyUHD, a SoapyRemote server, GNU Radio, uhd_fft) cannot be claimed: UHD's find comes back
 *  empty, or the claim fails with libusb's BUSY / ACCESS. `foundNone` = find() answered nothing at all. "" = not that. */
inline std::string busyAdvice(const std::string& uhdMessage, bool foundNone) {
    std::string m = uhdMessage;
    for (auto& ch : m) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    const bool busy = foundNone || m.find("busy") != std::string::npos || m.find("claim") != std::string::npos
                   || m.find("resource temporarily unavailable") != std::string::npos || m.find("access denied") != std::string::npos
                   || m.find("libusb_error_access") != std::string::npos;
    if (!busy) return "";
    return "Another program is using this USRP \xe2\x80\x94 stop it first (OpenWebRX+ / owrx_connector, a SoapyRemote "
           "server, GNU Radio or uhd_fft), and VibeServer takes the radio within seconds."
           + (uhdMessage.empty() ? std::string() : " UHD said: " + uhdMessage);
}

// ── the owner's radio options (V12, 2026-11-02: "can you add the missing options too please" — Stuart) ────────
/* ★★★ FOUR OWNER SETTINGS, PER RADIO, AND EACH OFFERED ONLY WHERE THE RADIO SAYS SO (AGENTS.md: no dead control):
 *   · the REFERENCE CLOCK and the TIME SOURCE — internal, external (10 MHz + PPS on the SMA sockets), GPSDO;
 *   · the AD9361's automatic DC-OFFSET and IQ-BALANCE correction (UHD's default: both on);
 *   · the ANALOGUE RX BANDWIDTH — AUTO (follows the sample rate) or a fixed figure inside the radio's own range.
 *  They are the OWNER's (Setup page, config.json per radio); no listener message reaches them. usrp_source.cpp asks
 *  the radio what it has and applies them; every decision about what to offer and what to refuse is here, so it is
 *  tested from fake capability lists (vibeserver/test-usrp-caps.cpp). */
struct Options {
    std::string clock;            // "" = UHD's default (internal) | "internal" | "external" | "gpsdo"
    std::string time;             // the same names
    bool   dcOffset  = true;      // ★ UHD's default, kept: an owner switches it OFF, never has to switch it on
    bool   iqBalance = true;
    double rxBwHz    = 0;         // 0 = AUTO (the span); otherwise a fixed analogue bandwidth, Hz
    bool operator==(const Options& o) const {
        return clock == o.clock && time == o.time && dcOffset == o.dcOffset && iqBalance == o.iqBalance && rxBwHz == o.rxBwHz;
    }
};
/** What the OPEN radio reported it can do — filled by usrp_source.cpp from UHD, by the test from fake lists. */
struct OptionCaps {
    std::vector<std::string> clockSources, timeSources;   // ★ already shaped by sourceChoices — empty = no choice
    bool   gpsdo = false;          // a GPSDO is FITTED (the gps_locked sensor exists), not merely supported
    bool   refSensor = false;      // the radio has a ref_locked sensor
    bool   dcOffset = false, iqBalance = false;           // the radio accepts the automatic switch
    double bwLo = 0, bwHi = 0;     // get_rx_bandwidth_range
    bool   bwAdjustable = false;   // ★ bwRangeUseful(bwLo, bwHi)
};
inline bool knownSource(const std::string& s) { return s.empty() || s == "internal" || s == "external" || s == "gpsdo"; }
/** ★ "A GPSDO is fitted" = the radio publishes a gps_locked sensor. UHD lists "gpsdo" as a clock source on every
 *  full-size B200/B210 whether or not the board carries one, so the source list alone cannot be trusted for it. */
inline bool hasGpsdo(const std::vector<std::string>& sensorNames) {
    for (const auto& n : sensorNames) if (n == "gps_locked") return true;
    return false;
}
/** The choices the owner is OFFERED, from what the radio REPORTED (get_clock_sources / get_time_sources): only the
 *  three VibeServer knows how to describe, in a fixed order, "gpsdo" only with a GPSDO fitted — and EMPTY when that
 *  leaves fewer than two, so no selector is drawn for a radio with nothing to choose. ("none", "mimo" and any later
 *  UHD name are dropped: nothing an owner of a receiver needs.) */
inline std::vector<std::string> sourceChoices(const std::vector<std::string>& reported, bool gpsdoFitted) {
    std::vector<std::string> out;
    for (const char* want : {"internal", "external", "gpsdo"}) {
        if (std::string(want) == "gpsdo" && !gpsdoFitted) continue;
        for (const auto& r : reported) if (r == want) { out.push_back(want); break; }
    }
    if (out.size() < 2) out.clear();
    return out;
}
inline const char* sourceWords(const std::string& s) {
    return s == "external" ? "external (10 MHz / PPS on the SMA sockets)" : s == "gpsdo" ? "the GPSDO" : "internal";
}
/** "" = fine; otherwise the refusal, in words. ★ An empty / "internal" choice is always fine: it is what every
 *  radio does with no setting at all, so it can never be refused. */
inline std::string refuseSource(const std::string& want, const std::vector<std::string>& choices, const char* what) {
    if (want.empty() || want == "internal") return "";
    if (!knownSource(want)) return std::string("'") + want + "' is not a " + what + " VibeServer knows";
    for (const auto& c : choices) if (c == want) return "";
    if (want == "gpsdo") return std::string("this radio has no GPSDO fitted, so the ") + what + " stays internal";
    return std::string("this radio does not offer ") + sourceWords(want) + " as its " + what + ", so it stays internal";
}
/** ★ A B200mini / B205mini routes ONE SMA to either the 10 MHz or the PPS — UHD refuses external for both. Said
 *  before the radio is asked, in words, and the time source is the one given up (the clock is what tuning needs). */
inline std::string refuseCombination(const std::string& model, const std::string& clock, const std::string& time) {
    const bool mini = model.find("mini") != std::string::npos;
    if (mini && clock == "external" && time == "external")
        return "a " + model + " has one reference socket: it can take the external 10 MHz or the PPS, not both — the time source stays internal";
    return "";
}
/** ★ "Non-trivial": a range at least 1 MHz wide. Anything narrower is a radio whose filter cannot really be moved,
 *  and a control over it would do nothing an owner could hear. */
inline bool bwRangeUseful(double lo, double hi) { return hi > lo && lo >= 0 && (hi - lo) >= 1e6; }
/** The fixed bandwidths offered, inside the radio's range — the rates this server runs at plus the AD9361's
 *  common filter widths. An empty list when the range is not useful. */
inline std::vector<uint32_t> bwChoices(double lo, double hi) {
    std::vector<uint32_t> out;
    if (!bwRangeUseful(lo, hi)) return out;
    static const uint32_t ladder[] = { 200000u, 500000u, 1000000u, 1500000u, 2000000u, 2500000u, 3000000u, 4000000u,
                                       5000000u, 6000000u, 8000000u, 10000000u, 12000000u, 16000000u, 20000000u,
                                       28000000u, 40000000u, 56000000u };
    for (uint32_t b : ladder) if (b >= lo - 1 && b <= hi + 1) out.push_back(b);
    return out;
}
inline std::string refuseBw(double want, const OptionCaps& c) {
    if (want <= 0) return "";
    if (!c.bwAdjustable) return "this radio's analogue bandwidth cannot be set, so it follows the sample rate";
    if (want < c.bwLo - 1 || want > c.bwHi + 1) {
        char b[160];
        std::snprintf(b, sizeof b, "%.3f MHz is outside this radio's analogue bandwidth (%.3f-%.3f MHz), so it follows the sample rate",
                      want / 1e6, c.bwLo / 1e6, c.bwHi / 1e6);
        return b;
    }
    return "";
}
/** The analogue bandwidth to set: the owner's fixed figure when it is allowed, otherwise the span (AUTO). */
inline double bwFor(const Options& o, double rate, const OptionCaps& c) {
    return (o.rxBwHz > 0 && refuseBw(o.rxBwHz, c).empty()) ? o.rxBwHz : rate;
}
/** ★★ THE SAVE'S CHECK, with no radio to ask (the Setup page may save a radio that is not running): only the SHAPE
 *  — names VibeServer knows, a bandwidth that is 0 or a real AD9361 figure. Whether THIS radio has the choice is
 *  decided at open, against what it reports, and a refusal is then said in its status (optionsNote). */
inline std::string validateSaved(const Options& o) {
    if (!knownSource(o.clock)) return "the reference clock must be internal, external or gpsdo";
    if (!knownSource(o.time))  return "the time source must be internal, external or gpsdo";
    if (o.rxBwHz != 0 && !(o.rxBwHz >= 100e3 && o.rxBwHz <= 61.44e6))
        return "the analogue bandwidth must be AUTO or between 0.1 and 61.44 MHz";
    return "";
}

// ── the radio object in hwinfo ────────────────────────────────────────────────────────────────────────────────
/** Everything radioCapsJson says about a USRP, as plain data — filled from UsrpSource by the shim, and from a stub
 *  by vibeserver/test-usrp-caps.cpp, so the wire shape is tested without a radio. */
struct CapsView {
    std::string model, serial;            // model as UHD reports it ("B210"); "" = unknown
    bool hasAgc = false, agcOn = false;
    int usbSpeed = 0;
    double fminHz = 0, fmaxHz = 0;
    std::vector<uint32_t> rates;
    std::vector<std::string> antennas;    // empty = one choice = no selector
    std::string antenna;
    bool antennaLocked = false;
    // ★ The owner's options: what the radio CAN do (oc) and what it IS doing now — read back from the radio.
    OptionCaps oc;
    std::string clock, time;              // the radio's own answer (get_clock_source / get_time_source)
    int  refLocked = -1, gpsLocked = -1;  // -1 = no such sensor (or not meaningful: an internal reference)
    bool dcOffsetOn = false, iqBalanceOn = false;
    double rxBwHz = 0;                    // the analogue bandwidth in force
    bool rxBwAuto = true;
    std::string optionsNote;              // a saved choice the radio refused, in words ("" = none)
    // ★ V12 the rate gate (ratesFor): the highest wide rate this machine's benchmark passed, and which path judged it
    double gateHz = 0; bool gateLocked = false;
};
inline std::string jsonQ(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned char)c); o += b; }
        else o += c;
    }
    return o + "\"";
}
/** The `,"radio":{…}` fragment radioCapsJson appends. ★★ What it HAS and what it has NOT, said explicitly so no
 *  client draws a dead control (AGENTS.md): no bias-T, no direct sampling, no tuner IF filter. The gain list is
 *  hwinfo's generic `gains` (getTunerGains), not repeated here. */
inline std::string capsJson(const CapsView& c) {
    std::string j = ",\"radio\":{\"driver\":\"usrp\",\"model\":" + jsonQ("USRP " + (c.model.empty() ? std::string("B2xx") : c.model));
    j += ",\"serial\":" + jsonQ(c.serial);
    j += std::string(",\"hwAgc\":") + (c.hasAgc ? "true" : "false");
    j += std::string(",\"agcOn\":") + (c.hasAgc && c.agcOn ? "true" : "false");
    j += ",\"usb\":" + std::to_string(c.usbSpeed);
    char rng[96];
    std::snprintf(rng, sizeof rng, ",\"ranges\":[[%.0f,%.0f]]", c.fminHz, c.fmaxHz);
    j += rng;
    j += ",\"rates\":[";
    for (size_t i = 0; i < c.rates.size(); ++i) { if (i) j += ','; j += std::to_string(c.rates[i]); }
    j += "],\"antennas\":[";
    for (size_t i = 0; i < c.antennas.size(); ++i) { if (i) j += ','; j += jsonQ(c.antennas[i]); }
    j += "]";
    if (!c.antennas.empty()) j += ",\"antenna\":" + jsonQ(c.antenna);
    j += std::string(",\"antennaLocked\":") + (c.antennaLocked ? "true" : "false");
    /* ★★ V12 THE RATE GATE, said (2026-11-03): on USB 3 the rates above 16 MS/s are offered only up to what THIS
     *  machine's benchmark kept up at with headroom (`maxHz`, 0 = not measured, or too slow), judged by the locked-range
     *  path for a locked-range radio and the full-radio one otherwise. The Setup page says it in words. */
    if (c.usbSpeed >= 3) {
        char g[128];
        std::snprintf(g, sizeof g, ",\"rateGate\":{\"maxHz\":%.0f,\"by\":\"%s\"}", c.gateHz, c.gateLocked ? "locked" : "full");
        j += g;
    }
    /* ★★ THE OWNER'S OPTIONS — each key only when the radio reported the capability, so a client can draw exactly
     *    what exists and nothing else. Capability and state travel together (the bias-T lesson). */
    auto list = [&](const char* k, const std::vector<std::string>& v) {
        j += std::string(",\"") + k + "\":[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) j += ','; j += jsonQ(v[i]); }
        j += "]";
    };
    list("clockSources", c.oc.clockSources);
    list("timeSources", c.oc.timeSources);
    if (!c.clock.empty()) j += ",\"clock\":" + jsonQ(c.clock);
    if (!c.time.empty())  j += ",\"time\":" + jsonQ(c.time);
    if (c.refLocked >= 0) j += std::string(",\"refLocked\":") + (c.refLocked ? "true" : "false");
    j += std::string(",\"gpsdo\":") + (c.oc.gpsdo ? "true" : "false");
    if (c.oc.gpsdo && c.gpsLocked >= 0) j += std::string(",\"gpsLocked\":") + (c.gpsLocked ? "true" : "false");
    j += std::string(",\"dcOffset\":") + (c.oc.dcOffset ? "true" : "false");
    if (c.oc.dcOffset) j += std::string(",\"dcOffsetOn\":") + (c.dcOffsetOn ? "true" : "false");
    j += std::string(",\"iqBalance\":") + (c.oc.iqBalance ? "true" : "false");
    if (c.oc.iqBalance) j += std::string(",\"iqBalanceOn\":") + (c.iqBalanceOn ? "true" : "false");
    if (c.oc.bwAdjustable) {
        char b[96];
        std::snprintf(b, sizeof b, ",\"rxBwRange\":[%.0f,%.0f]", c.oc.bwLo, c.oc.bwHi);
        j += b;
        j += ",\"rxBwChoices\":[";
        const auto bc = bwChoices(c.oc.bwLo, c.oc.bwHi);
        for (size_t i = 0; i < bc.size(); ++i) { if (i) j += ','; j += std::to_string(bc[i]); }
        j += "]";
        j += std::string(",\"rxBwAuto\":") + (c.rxBwAuto ? "true" : "false");
    }
    if (c.rxBwHz > 0) { char b[48]; std::snprintf(b, sizeof b, ",\"rxBwHz\":%.0f", c.rxBwHz); j += b; }
    if (!c.optionsNote.empty()) j += ",\"optionsNote\":" + jsonQ(c.optionsNote);
    j += ",\"hasBiasT\":false,\"noDirectSampling\":true,\"noTunerBw\":true,\"experimental\":true}";
    return j;
}

}  // namespace usrpcaps
}  // namespace vibe
