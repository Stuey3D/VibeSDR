// ★★★ THE ETTUS USRP B2xx's POLICY AND WIRE SHAPE, TESTED WITHOUT A RADIO (V12, 2026-11-02 — docs/v12/USRP.md).
//
// No B2xx exists on the build Mac. Every DECISION usrp_source.cpp and the shim make comes from vibe_usrp_caps.h, so
// this drives that header: the USB ids, the rates by USB link and the default by host, the gain list read from a
// range, the RX ports and their labels (and that those labels survive the owner's per-band aerial parser), which
// image file is loaded (owner override first, per file), and the `radio` object hwinfo carries — from a stubbed
// CapsView, so the shape is checked without UHD.
// ★ Built two ways. scripts/run-tests.sh compiles it header-only (no UHD): then UsrpSource is the inline stub, and
//   the stub's promises are checked (zero devices, a refusal in words). The CMake target test-usrp-caps, made only
//   where UHD is linked (amd64 / arm64 / Mac), defines VIBE_HAVE_USRP and links the real source: then "no USRP on
//   this machine" must be a clean answer, not a hang, an exception or a firmware download.
#include "usrp_source.h"
#include "vibe_usrp_caps.h"
#include "vibe_bands.h"
#include "sdr_presence.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
static void touch(const std::string& p) { FILE* f = std::fopen(p.c_str(), "w"); if (f) { std::fputs("x", f); std::fclose(f); } }

using namespace vibe::usrpcaps;

int main() {
    std::printf("== USB identity\n");
    ok(nameForUsbId(0x2500, 0x0020) && std::string(nameForUsbId(0x2500, 0x0020)) == "B200/B210",
       "2500:0020 is \"B200/B210\" — the PID cannot tell them apart, so the name does not guess");
    ok(nameForUsbId(0x2500, 0x0021) && std::string(nameForUsbId(0x2500, 0x0021)) == "B200mini", "2500:0021 B200mini");
    ok(nameForUsbId(0x2500, 0x0022) && std::string(nameForUsbId(0x2500, 0x0022)) == "B205mini", "2500:0022 B205mini");
    ok(nameForUsbId(0x3923, 0x7814) && std::string(nameForUsbId(0x3923, 0x7814)) == "B210", "3923:7814 NI-branded B210");
    ok(!nameForUsbId(0x0bda, 0x2838), "an RTL dongle is not a USRP");
    ok(!nameForUsbId(0x04b4, 0x00f3), "a bare Cypress FX3 bootloader is not claimed as a USRP");
    ok(displayName("B210", "30AD2C1") == "Ettus USRP B210 (30AD2C1)", "display name carries the serial");
    ok(displayName("", "") == "Ettus USRP B2xx", "no model, no serial — still a name, never empty");
    // ★★ 2026-11-02, a tester's B210 clone: the descriptor read "30AA038 ?" and the Setup page refused it.
    ok(cleanSerial("30AA038 ?") == "30AA038", "a clone's serial with a space and a '?' after it is its leading part");
    ok(cleanSerial("30AD2C1") == "30AD2C1", "a clean serial is unchanged");
    ok(cleanSerial("  F5A3B2\x01junk") == "F5A3B2", "leading blanks dropped, cut at the first byte a serial cannot hold");
    ok(cleanSerial("???") == "", "all junk: no serial (found by type alone)");
    ok(cleanSerial(std::string(100, 'A')).size() == 64, "never longer than a config serial may be");

    std::printf("== sample rates\n");
    const auto r2 = ratesFor(2), r3 = ratesFor(3), r0 = ratesFor(0);
    ok(!r2.empty() && r2.back() == 8000000u, "USB 2 tops out at 8 MS/s");
    ok(!r3.empty() && r3.back() == 16000000u, "USB 3 tops out at 16 MS/s");
    ok(r0 == r2, "an UNKNOWN link is treated as USB 2 — the conservative reading");
    ok(r2.front() == 1000000u && r3.front() == 1000000u, "1 MS/s floor on both");
    bool asc = true; for (size_t i = 1; i < r3.size(); i++) asc = asc && r3[i] > r3[i - 1];
    ok(asc, "the list is ascending and has no duplicates");
    ok(defaultRate(Host::Arm64) == 2400000u, "arm64 (Pi 4/5 class) defaults to 2.4 MS/s");
    ok(defaultRate(Host::X86_64) == 4000000u && defaultRate(Host::Apple) == 4000000u, "x86-64 and the Mac default to 4 MS/s");
    ok(nearestRate(r2, 0, Host::Arm64) == 2400000u, "a fresh radio (no saved rate) gets the host default");
    ok(nearestRate(r2, 2.4e6, Host::X86_64) == 2400000u, "an RTL-era saved 2.4 MS/s is kept");
    ok(nearestRate(r2, 20e6, Host::X86_64) == 8000000u, "a saved 20 MS/s on USB 2 snaps to 8");
    ok(nearestRate(r3, 12e6, Host::X86_64) == 12500000u, "12 MS/s on USB 3 snaps to 12.5");
    ok(nearestRate({}, 2e6, Host::Arm64) == 0, "an empty list answers 0, never a guess");

    std::printf("== gain\n");
    const auto g = gainList(0, 76, 1);   // UHD 4.9.0.1's AD9361 RX range (ad9361_ctrl.hpp)
    ok(g.size() == 77 && g.front() == 0 && g.back() == 760, "0-76 dB in 1 dB steps = 77 entries, in tenths");
    ok(gainList(0, 76, 0.25).size() == 77, "a sub-dB step is listed at 1 dB");
    ok(gainList(10, 5, 1).empty() && gainList(0, 10, -1).size() == 11, "stop < start = empty; a bad step = 1 dB");
    ok(nearestGain(g, 305) == 300 || nearestGain(g, 305) == 310, "nearest step to 30.5 dB");
    ok(nearestGain(g, 9999) == 760 && nearestGain(g, -50) == 0, "clamped to the range");

    std::printf("== RX ports\n");
    const auto b200 = ports({{"TX/RX", "RX2"}});
    const auto l200 = labels(b200, 1);
    ok(l200.size() == 2 && l200[0] == "RX2" && l200[1] == "TX/RX", "B200: RX2 first, then TX/RX, no channel letter");
    const auto b210 = ports({{"TX/RX", "RX2"}, {"TX/RX", "RX2"}});
    const auto l210 = labels(b210, 2);
    ok(l210.size() == 4 && l210[0] == "RX2 (A)" && l210[1] == "TX/RX (A)" && l210[2] == "RX2 (B)" && l210[3] == "TX/RX (B)",
       "B210: both channels, socket then (channel)");
    ok(labels(ports({{"RX2"}}), 1).empty(), "ONE choice publishes NO list — no selector is drawn");
    ok(labels(ports({{"RX2", "CAL"}}), 1).empty(), "an internal path is never offered as a socket");
    Port pt;
    ok(portFor(b210, 2, "rx2 (b)", pt) && pt.chan == 1 && pt.socket == "RX2", "a label maps back, case-insensitively");
    ok(!portFor(b210, 2, "RX1", pt) && !portFor(b200, 1, "RX2 (B)", pt), "a port the radio does not have is refused");
    for (const auto& l : l210) ok(l.find(':') == std::string::npos, "no label contains a colon (the aerial parser splits on it)", l);
    {
        // ★ The owner's per-band aerial list, through the SAME parser the server uses (vibe_bands.h).
        const auto rules = vibebands::parseAntennaList("0-400MHz RX2, 400MHz+ TX/RX");
        ok(vibebands::antennaAt(rules, 100e6) == "RX2", "\"0-400MHz RX2\" picks RX2 at 100 MHz", vibebands::antennaAt(rules, 100e6));
        ok(vibebands::antennaAt(rules, 1.2e9) == "TX/RX", "\"400MHz+ TX/RX\" picks TX/RX at 1.2 GHz", vibebands::antennaAt(rules, 1.2e9));
        const auto r210 = vibebands::parseAntennaList("0-400MHz RX2 (A), 400MHz+ RX2 (B)");
        const std::string a = vibebands::antennaAt(r210, 100e6), b = vibebands::antennaAt(r210, 2e9);
        ok(portFor(b210, 2, a, pt) && pt.chan == 0, "a B210 rule \"RX2 (A)\" resolves to channel A", a);
        ok(portFor(b210, 2, b, pt) && pt.chan == 1, "a B210 rule \"RX2 (B)\" resolves to channel B", b);
    }

    std::printf("== images\n");
    ok(std::string(firmwareFile()) == "usrp_b200_fw.hex", "firmware is UHD's usrp_b200_fw.hex");
    ok(std::string(fpgaFileFor("B210")) == "usrp_b210_fpga.bin" && std::string(fpgaFileFor("B200")) == "usrp_b200_fpga.bin",
       "B200 / B210 FPGA names");
    ok(std::string(fpgaFileFor("B200mini")) == "usrp_b200mini_fpga.bin" && std::string(fpgaFileFor("B206mini")) == "usrp_b205mini_fpga.bin",
       "minis — and the B206mini takes the B205mini image, as UHD maps it");
    ok(fpgaFileFor("X310") == nullptr && fpgaFileFor("") == nullptr, "anything else has no image — refused in words, not guessed");
    {
        char tmpl[] = "/tmp/vibe-usrp-test-XXXXXX";
        const char* d = mkdtemp(tmpl);
        ok(d != nullptr, "a scratch directory");
        if (d) {
            const std::string root = d, ovr = root + "/override", ship = root + "/ship";
            mkdir(ovr.c_str(), 0755); mkdir(ship.c_str(), 0755);
            touch(ship + "/usrp_b200_fw.hex"); touch(ship + "/usrp_b210_fpga.bin");
            const std::vector<std::string> dirs = { "", ovr, ship };
            ok(resolveImage("usrp_b210_fpga.bin", dirs) == ship + "/usrp_b210_fpga.bin", "with no override, the shipped image");
            touch(ovr + "/usrp_b210_fpga.bin");   // ★ a clone's third-party FPGA image, put there by its owner
            ok(resolveImage("usrp_b210_fpga.bin", dirs) == ovr + "/usrp_b210_fpga.bin", "the owner's override wins");
            ok(resolveImage("usrp_b200_fw.hex", dirs) == ship + "/usrp_b200_fw.hex",
               "PER FILE: an override of the FPGA alone still takes our firmware");
            ok(resolveImage("usrp_b200mini_fpga.bin", dirs).empty(), "missing everywhere = \"\" (a broken install, said in words)");
            mkdir((ship + "/usrp_b205mini_fpga.bin").c_str(), 0755);
            ok(resolveImage("usrp_b205mini_fpga.bin", dirs).empty(), "a DIRECTORY with the image's name is not an image");
            ok(resolveImage("usrp_b200_fw.hex", { ship + "/" }) == ship + "/usrp_b200_fw.hex", "a trailing slash is not doubled");
            std::string rm = "rm -rf '" + root + "'"; (void)!std::system(rm.c_str());
        }
    }

    std::printf("== hwinfo radio object (stubbed CapsView)\n");
    {
        CapsView c;
        c.model = "B210"; c.serial = "30AD2C1"; c.hasAgc = true; c.agcOn = false; c.usbSpeed = 3;
        c.fminHz = 50e6; c.fmaxHz = 6e9; c.rates = ratesFor(3); c.antennas = l210; c.antenna = "RX2 (A)";
        const std::string j = capsJson(c);
        ok(j.rfind(",\"radio\":{", 0) == 0 && j.back() == '}', "a `,\"radio\":{…}` fragment, as radioCapsJson returns");
        ok(has(j, "\"driver\":\"usrp\"") && has(j, "\"model\":\"USRP B210\"") && has(j, "\"serial\":\"30AD2C1\""), "driver, model, serial");
        ok(has(j, "\"ranges\":[[50000000,6000000000]]"), "the radio's own tuning range", j);
        ok(has(j, "\"rates\":[1000000,") && has(j, ",16000000]"), "the USB 3 rate list");
        ok(has(j, "\"antennas\":[\"RX2 (A)\",\"TX/RX (A)\",\"RX2 (B)\",\"TX/RX (B)\"]") && has(j, "\"antenna\":\"RX2 (A)\""),
           "the ports and the one in use travel together");
        ok(has(j, "\"hwAgc\":true") && has(j, "\"agcOn\":false") && has(j, "\"usb\":3"), "AGC capability + state, USB link");
        ok(has(j, "\"hasBiasT\":false") && has(j, "\"noDirectSampling\":true") && has(j, "\"noTunerBw\":true"),
           "what it has NOT is said, so nothing dead is drawn");
        c.antennas = labels(b200, 1); c.antenna = "RX2"; c.model = "B200mini"; c.hasAgc = false; c.agcOn = true; c.usbSpeed = 2;
        const std::string j2 = capsJson(c);
        ok(has(j2, "\"antennas\":[\"RX2\",\"TX/RX\"]") && has(j2, "\"model\":\"USRP B200mini\""), "B200mini: two plain sockets");
        ok(has(j2, "\"hwAgc\":false") && has(j2, "\"agcOn\":false"), "no AGC capability = agcOn can never read true");
        c.antennas.clear(); c.antenna = "";
        const std::string j3 = capsJson(c);
        ok(has(j3, "\"antennas\":[]") && !has(j3, "\"antenna\":"), "one socket: an EMPTY list and no `antenna` field");
        c.serial = "a\"b\\c";
        ok(has(capsJson(c), "\"serial\":\"a\\\"b\\\\c\""), "device-supplied strings are escaped");
    }

    std::printf("== sysfs claimed counts (fake tree)\n");
    {
        char tmpl[] = "/tmp/vibe-usrp-sysfs-XXXXXX";
        const char* d = mkdtemp(tmpl);
        if (d) {
            const std::string root = d;
            auto dev = [&](const std::string& n, const char* vid, const char* pid, bool claimed) {
                mkdir((root + "/" + n).c_str(), 0755);
                FILE* f = std::fopen((root + "/" + n + "/idVendor").c_str(), "w"); std::fputs(vid, f); std::fclose(f);
                f = std::fopen((root + "/" + n + "/idProduct").c_str(), "w"); std::fputs(pid, f); std::fclose(f);
                mkdir((root + "/" + n + ":1.0").c_str(), 0755);
                if (claimed) (void)!symlink("../../../bus/usb/drivers/usbfs", (root + "/" + n + ":1.0/driver").c_str());
            };
            dev("1-1", "2500", "0020", true);    // a B210 something has claimed
            dev("1-2", "2500", "0021", false);   // a B200mini nobody holds
            dev("1-3", "1d50", "6089", true);    // a HackRF, claimed
            int sp, ahf, hrf, asp, usrp = -1;
            ok(vibe::sysfsClaimedCounts(root, sp, ahf, hrf, asp, &usrp), "the tree reads");
            ok(usrp == 1 && hrf == 1, "one claimed B2xx, one claimed HackRF; the free mini is not counted",
               "usrp=" + std::to_string(usrp) + " hrf=" + std::to_string(hrf));
            int u2 = 7;
            ok(!vibe::sysfsClaimedCounts(root + "/nope", sp, ahf, hrf, asp, &u2) && u2 == 0, "an unreadable root = unknown, count zeroed");
            ok(vibe::sysfsClaimedCounts(root, sp, ahf, hrf, asp), "the four-count callers are unchanged (usrp optional)");
            std::string rm = "rm -rf '" + root + "'"; (void)!std::system(rm.c_str());
        }
    }

    std::printf("== the source, with no radio attached\n");
    {
        vibe::UsrpSource s;
        std::string err;
#if defined(VIBE_HAVE_USRP)
        ok(vibe::UsrpSource::compiledIn(), "this build links UHD");
        const int n = vibe::UsrpSource::deviceCount();
        std::printf("   (B2xx on this machine's bus: %d)\n", n);
        if (n == 0) {
            ok(vibe::UsrpSource::deviceName(0).empty() && vibe::UsrpSource::deviceSerial(0).empty(), "no device = no name, no serial");
            ok(!s.open(0, 2.4e6, 100e6, 0, err) && err == "no USRP at that index", "open says \"no USRP at that index\"", err);
            ok(!s.isOpen() && s.sampleRates().empty() && s.antennas().empty(), "nothing half-open afterwards");
        }
        ok(!s.open(-1, 2.4e6, 100e6, 0, err), "a negative index is refused");
#else
        ok(!vibe::UsrpSource::compiledIn(), "no UHD in this build: the inline stub");
        ok(vibe::UsrpSource::deviceCount() == 0 && vibe::UsrpSource::deviceName(0).empty(), "the stub lists nothing");
        ok(!s.open(0, 2.4e6, 100e6, 0, err) && has(err, "no USRP support"), "and refuses an open in words", err);
#endif
        s.close(); s.stop();
        ok(true, "close/stop on a never-opened source are harmless");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
