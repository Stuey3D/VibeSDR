// test-win-switch — the TV-stick switch's rules and start-at-boot's pure parts, with made-up devices (Windows build,
// run under Wine by windows/test/run-tests-wine.sh). No USB, no admin, nothing installed.
//
// ★★★ What it guards (Stuart, 2026-10-26): ONLY a TV stick (or a stick with no driver) is ever OFFERED; a stick that
//     already works is never offered anything; the elevated helper refuses any device that is not, right now, an
//     RTL-SDR in an offerable state, and refuses any extra argument; Give back only undoes what WE did; turning
//     start-at-boot on or off is idempotent and runs as LOCAL SERVICE, never SYSTEM.
#include "win_switch.h"
#include "win_autostart.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vibewin;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char* kTv      = "USB\\VID_0BDA&PID_2838&MI_00\\7&2C1A0B3&0&0000|RTL2832UBDA|MEDIA|0|oem12.inf|0|USB\\VID_0BDA&PID_2838\\00000001";
static const char* kNoDrv   = "USB\\VID_0BDA&PID_2838&MI_00\\7&AA&0&0000||USB|28||0|USB\\VID_0BDA&PID_2838\\00000002";
static const char* kZadig   = "USB\\VID_0BDA&PID_2838&MI_00\\7&BB&0&0000|WinUSB|USBDevice|0|oem40.inf|0|USB\\VID_0BDA&PID_2838\\00000003";
static const char* kOurs    = "USB\\VID_0BDA&PID_2838&MI_00\\7&CC&0&0000|WinUSB|USBDevice|0|winusb.inf|1|USB\\VID_0BDA&PID_2838\\00000004";
static const char* kFault   = "USB\\VID_0BDA&PID_2838&MI_00\\7&DD&0&0000||USB|43||0|USB\\VID_0BDA&PID_2838\\00000005";
static const char* kTvFail  = "USB\\VID_0BDA&PID_2838&MI_00\\7&EE&0&0000|RTL2832UBDA|MEDIA|10|oem12.inf|0|USB\\VID_0BDA&PID_2838\\00000006";
static const char* kOther   = "USB\\VID_0BDA&PID_2838&MI_00\\7&FF&0&0000|SomeVendorDrv|Sample|0|oem77.inf|0|USB\\VID_0BDA&PID_2838\\00000007";
static const char* kParent  = "USB\\VID_0BDA&PID_2838\\00000001|usbccgp|USB|0|usb.inf|0";
static const char* kIr      = "USB\\VID_0BDA&PID_2838&MI_01\\7&2C1A0B3&0&0001|HidUsb|HIDClass|0|input.inf|0|USB\\VID_0BDA&PID_2838\\00000001";
static const char* kMouse   = "USB\\VID_046D&PID_C077\\5&1234&0&2|HidUsb|HIDClass|0|input.inf|0";
static const char* kHfPlus  = "USB\\VID_03EB&PID_800C\\0123456789|WinUSB|USBDevice|0|oem3.inf|0";
static const char* kWinUsbV4L = "USB\\VID_0BDA&PID_2838&MI_00\\8&11&0&0000|RTL2832UBDA|MEDIA|0|oem12.inf|0|USB\\VID_0BDA&PID_2838\\V4L00001";

static std::vector<UsbNode> nodes(std::vector<const char*> lines) {
    std::string text = "# fake list\n";
    for (auto* l : lines) { text += l; text += "\n"; }
    std::vector<UsbNode> out; std::string err;
    if (!parseFakeDevices(text, out, err)) std::printf("parse error: %s\n", err.c_str());
    return out;
}
static Dongle one(const char* line) { auto d = rtlDongles(nodes({ line })); return d.empty() ? Dongle{} : d[0]; }

static int helperArgs(std::vector<const char*> a, HelperArgs& out) {
    std::vector<char*> v; v.push_back((char*)"vibeserver.exe");
    for (auto* s : a) v.push_back((char*)s);
    out = parseHelperArgs((int)v.size(), v.data());
    return (int)out.op;
}

int main() {
    // ── classification ────────────────────────────────────────────────────────────────────────────────────────────
    CHECK(one(kTv).state == DongleState::TvStick);
    CHECK(one(kNoDrv).state == DongleState::NoDriver);
    CHECK(one(kZadig).state == DongleState::Usable && !one(kZadig).switchedByUs);
    CHECK(one(kOurs).state == DongleState::Usable && one(kOurs).switchedByUs);
    CHECK(one(kFault).state == DongleState::Other);              // code 43 with no driver: a fault, not offered
    CHECK(one(kTvFail).state == DongleState::TvStick);           // TV driver failing to start: still a TV stick
    CHECK(one(kOther).state == DongleState::Other);
    CHECK(one(kWinUsbV4L).state == DongleState::TvStick);
    CHECK(one(kTv).serial == "00000001");
    CHECK(one(kTv).name.find("RTL2832U") != std::string::npos || !one(kTv).name.empty());
    {   // WinUSB but with a problem code: usable driver, not working — explained, not offered, not "ready"
        auto d = one("USB\\VID_0BDA&PID_2838&MI_00\\7&AB&0&0000|WinUSB|USBDevice|10|winusb.inf|1");
        CHECK(d.state == DongleState::Other);
    }
    // only RTL radio nodes are listed: no parent, no IR interface, no mouse, no Airspy
    {
        auto ds = rtlDongles(nodes({ kParent, kTv, kIr, kMouse, kHfPlus }));
        CHECK(ds.size() == 1 && ds[0].state == DongleState::TvStick);
    }
    // a non-composite RTL (some dongles enumerate without MI_xx)
    CHECK(one("USB\\VID_0BDA&PID_2832\\00000009|RTL2832UBDA|MEDIA|0|oem12.inf|0").state == DongleState::TvStick);

    // ── the offer: shown ONLY for TV stick / no driver ──────────────────────────────────────────────────────────────
    CHECK(offerText(one(kTv)) == "Windows is using this dongle as a TV tuner (" + one(kTv).name +
          ", serial 00000001). Switch it to SDR so VibeServer can use it? You can switch it back any time.");
    CHECK(!offerText(one(kNoDrv)).empty());
    CHECK(offerText(one(kZadig)).empty());
    CHECK(offerText(one(kOurs)).empty());
    CHECK(offerText(one(kFault)).empty());
    CHECK(offerText(one(kOther)).empty());
    CHECK(consoleOffer(one(kZadig), 48000).empty());
    CHECK(consoleOffer(one(kTv), 48000).find("http://localhost:48000/setup") != std::string::npos);
    for (const auto& d : rtlDongles(nodes({ kTv, kNoDrv, kZadig, kOurs, kFault, kTvFail, kOther }))) {
        CHECK(offerable(d.state) == !offerText(d).empty());
        CHECK(offerText(d).find("Zadig") == std::string::npos);
    }
    {
        const std::string j = donglesJson(rtlDongles(nodes({ kTv, kZadig })));
        CHECK(j.find("\"state\":\"tv\",\"offer\":true") != std::string::npos);
        CHECK(j.find("\"state\":\"usable\",\"offer\":false") != std::string::npos);
        CHECK(j.find("USB\\\\VID_0BDA") != std::string::npos);   // backslashes escaped
    }

    // ── the helper's command line ───────────────────────────────────────────────────────────────────────────────────
    const char* id = "USB\\VID_0BDA&PID_2838&MI_00\\7&2C1A0B3&0&0000";
    const char* pipe = "\\\\.\\pipe\\vibeserver-helper-1234-abcdef0123456789";
    HelperArgs h;
    CHECK(helperArgs({ "--winusb-bind", id, "--reply", pipe }, h) == (int)HelperOp::Bind && h.instanceId == id);
    CHECK(helperArgs({ "--winusb-giveback", id, "--reply", pipe }, h) == (int)HelperOp::GiveBack);
    CHECK(helperArgs({ "--autostart-set", "boot", "--grant-sid", "S-1-5-21-1-2-3-1001", "--reply", pipe }, h) == (int)HelperOp::AutostartBoot);
    CHECK(helperArgs({ "--autostart-set", "off", "--reply", pipe }, h) == (int)HelperOp::AutostartOff);
    // refusals
    CHECK(helperArgs({ "--winusb-bind", id }, h) == 0 && !h.error.empty());                                   // no reply pipe
    CHECK(helperArgs({ "--winusb-bind", id, "--reply", "\\\\.\\pipe\\something-else" }, h) == 0);             // not our pipe
    CHECK(helperArgs({ "--winusb-bind", id, "--reply", pipe, "--serve" }, h) == 0);                           // extra argument
    CHECK(helperArgs({ "--winusb-bind", id, "--winusb-giveback", id, "--reply", pipe }, h) == 0);             // two operations
    CHECK(helperArgs({ "--winusb-bind", "USB\\VID_0BDA&PID_2838&MI_00\\7\" & calc", "--reply", pipe }, h) == 0); // quote/space
    CHECK(helperArgs({ "--winusb-bind", "PCI\\VEN_8086&DEV_1234\\3&1", "--reply", pipe }, h) == 0);           // not USB
    CHECK(helperArgs({ "--winusb-bind", "USB\\VID_0BDA&PID_2838&MI_00\\..\\x", "--reply", pipe }, h) == 0);   // backslash in id part
    CHECK(helperArgs({ "--autostart-set", "always", "--reply", pipe }, h) == 0);
    CHECK(helperArgs({ "--autostart-set", "off", "--grant-sid", "S-1-5-21-1", "--reply", pipe }, h) == 0);  // sid only with boot
    CHECK(helperArgs({ "--autostart-set", "boot", "--grant-sid", "S-1-5-21-1;rm", "--reply", pipe }, h) == 0);
    CHECK(isHelperInvocation(3, (char**)(const char*[]){ "x", "--winusb-bind", id }));
    CHECK(!isHelperInvocation(2, (char**)(const char*[]){ "x", "--serve" }));
    // the server's side builds exactly what the helper accepts
    {
        HelperArgs a; a.op = HelperOp::Bind; a.instanceId = id; a.replyPipe = pipe;
        CHECK(helperCommandLine(a) == std::string("--winusb-bind \"") + id + "\" --reply \"" + pipe + "\"");
        a.instanceId = "USB\\VID_0BDA&PID_2838\\x\" --serve"; CHECK(helperCommandLine(a).empty());
        a.instanceId = id; a.replyPipe = "\\\\.\\pipe\\evil"; CHECK(helperCommandLine(a).empty());
        HelperArgs b; b.op = HelperOp::AutostartBoot; b.replyPipe = pipe; b.grantSid = "S-1-5-21-9-8-7-1001";
        CHECK(helperCommandLine(b) == std::string("--autostart-set boot --grant-sid S-1-5-21-9-8-7-1001 --reply \"") + pipe + "\"");
    }

    // ── the helper's refusals against the REAL list it scanned ─────────────────────────────────────────────────────
    {
        const auto real = nodes({ kParent, kTv, kIr, kNoDrv, kZadig, kOurs, kFault, kMouse, kHfPlus });
        std::string why;
        CHECK(helperMayBind(real, one(kTv).instanceId, why));
        CHECK(helperMayBind(real, one(kNoDrv).instanceId, why));
        CHECK(!helperMayBind(real, one(kZadig).instanceId, why) && why.find("already usable") != std::string::npos);
        CHECK(!helperMayBind(real, one(kOurs).instanceId, why));
        CHECK(!helperMayBind(real, one(kFault).instanceId, why));
        CHECK(!helperMayBind(real, "USB\\VID_046D&PID_C077\\5&1234&0&2", why) && why.find("not an RTL-SDR") != std::string::npos);
        CHECK(!helperMayBind(real, "USB\\VID_03EB&PID_800C\\0123456789", why));
        CHECK(!helperMayBind(real, "USB\\VID_0BDA&PID_2838\\00000001", why));        // the composite parent
        CHECK(!helperMayBind(real, "USB\\VID_0BDA&PID_2838&MI_01\\7&2C1A0B3&0&0001", why));  // the IR interface
        CHECK(!helperMayBind(real, "USB\\VID_0BDA&PID_2838&MI_00\\9&99&0&0000", why) && why.find("not plugged in") != std::string::npos);
        // give back: only what WE switched
        CHECK(helperMayGiveBack(real, one(kOurs).instanceId, why));
        CHECK(!helperMayGiveBack(real, one(kZadig).instanceId, why) && why.find("did not switch") != std::string::npos);
        CHECK(!helperMayGiveBack(real, one(kTv).instanceId, why));
        CHECK(!helperMayGiveBack(real, "USB\\VID_046D&PID_C077\\5&1234&0&2", why));
        // case-insensitive ids (Windows' are upper case; a page may send either)
        std::string lc = one(kTv).instanceId; for (char& c : lc) c = (char)tolower((unsigned char)c);
        CHECK(helperMayBind(real, lc, why));
    }

    // ── fake list parsing ───────────────────────────────────────────────────────────────────────────────────────────
    {
        std::vector<UsbNode> out; std::string err;
        CHECK(!parseFakeDevices("USB\\VID_0BDA&PID_2838&MI_00\\7&1|x|y\n", out, err));
        CHECK(!parseFakeDevices("USB\\VID_0BDA&PID_2838&MI_00\\7&1|x|y|abc|z|0\n", out, err));
        CHECK(parseFakeDevices("\n# only a comment\n", out, err) && out.empty());
    }
    CHECK(jsonField("{\"id\":\"USB\\\\VID_0BDA&PID_2838\\\\x\",\"mode\":\"boot\"}", "id") == "USB\\VID_0BDA&PID_2838\\x");
    CHECK(jsonField("{\"mode\":\"boot\"}", "mode") == "boot");
    CHECK(jsonField("{\"mode\":\"boot\"}", "id").empty());
    CHECK(validSidString("S-1-5-19") && !validSidString("S-1-") && !validSidString("S-1-5-") && !validSidString("X-1-5-19"));

    // ── start at boot: the pure parts ───────────────────────────────────────────────────────────────────────────────
    {
        const std::string x = taskXml("C:\\VibeServer\\vibeserver.exe");
        CHECK(x.find("<UserId>S-1-5-19</UserId>") != std::string::npos);            // LOCAL SERVICE
        CHECK(x.find("S-1-5-18") == std::string::npos);                             // never SYSTEM
        CHECK(x.find("<LogonType>ServiceAccount</LogonType>") != std::string::npos);
        CHECK(x.find("<RunLevel>LeastPrivilege</RunLevel>") != std::string::npos);
        CHECK(x.find("<BootTrigger>") != std::string::npos);
        CHECK(x.find("<RestartOnFailure>") != std::string::npos);
        CHECK(x.find("<Command>\"C:\\VibeServer\\vibeserver.exe\"</Command>") != std::string::npos);
        CHECK(x.find("<Arguments>--autostart boot</Arguments>") != std::string::npos);
        CHECK(x.find("<WorkingDirectory>C:\\VibeServer</WorkingDirectory>") != std::string::npos);
        CHECK(x.find("<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>") != std::string::npos);  // never stopped after 72 h
        CHECK(x.find("<DisallowStartIfOnBatteries>false") != std::string::npos);
        const std::string e = taskXml("C:\\R&D <x>\\vibeserver.exe");
        CHECK(e.find("C:\\R&amp;D &lt;x&gt;\\vibeserver.exe") != std::string::npos && e.find("R&D") == std::string::npos);
        // idempotence is in the arguments: /F replaces on create; delete is preceded by a query (absent = done)
        const auto c = schtasksCreateArgs("C:\\ProgramData\\VibeServer\\t.xml");
        CHECK(c.size() == 6 && c[0] == "/Create" && c[2] == "VibeServer" && c[3] == "/XML" && c[5] == "/F");
        const auto d = schtasksDeleteArgs();
        CHECK(d.size() == 4 && d[0] == "/Delete" && d[2] == "VibeServer" && d[3] == "/F");
        CHECK(schtasksQueryArgs().size() == 3 && schtasksQueryArgs()[0] == "/Query");
        CHECK(runValue("C:\\VibeServer\\vibeserver.exe") == "\"C:\\VibeServer\\vibeserver.exe\" --autostart signin");
        std::string why;
        CHECK(exePathServiceReadable("C:\\VibeServer\\vibeserver.exe", "C:\\Users", why));
        CHECK(!exePathServiceReadable("C:\\Users\\stuart\\Downloads\\vs\\vibeserver.exe", "C:\\Users", why) && !why.empty());
        CHECK(!exePathServiceReadable("c:/users/stuart/Desktop/vibeserver.exe", "C:\\Users\\", why));
        CHECK(exePathServiceReadable("C:\\Users\\Public\\VibeServer\\vibeserver.exe", "C:\\Users", why));
        CHECK(exePathServiceReadable("C:\\UsersData\\vibeserver.exe", "C:\\Users", why));
        CHECK(exePathServiceReadable("D:\\Radio\\vibeserver.exe", "C:\\Users", why));
    }

    std::printf("test-win-switch: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
