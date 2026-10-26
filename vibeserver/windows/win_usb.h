// win_usb.h — which driver Windows has on each radio, and which USB path VibeServer takes (Windows only).
//
// ★★★ STUART'S RULE FOR WINDOWS (2026-10-05): NO ZADIG STEP. An RTL-SDR on the stock DVB-T driver, on no
//     driver at all, or on a Zadig-replaced driver must all simply work. Three ways in, in this order
//     (docs/WINDOWS-PORT-PLAN.md §3):
//       1. already on WinUSB / libusbK / libusb0 (Zadig, SDR#, SDR++ users) → use it as it is, silently;
//       2. ★★★ THE DEFAULT (Stuart, 2026-10-26): a stick Windows is using as a TV tuner (or has no driver
//          for) is OFFERED in the setup page — "Switch it to SDR?" — and SWITCH puts Microsoft's own in-box
//          winusb.inf on that ONE device after ONE UAC prompt (win_switch.h, win_bind.h). No Zadig, no
//          driver or certificate of ours, nothing of Windows' security lowered;
//       3. UsbDk "borrow" — kept, but OFF unless VIBE_USB_BACKEND=usbdk (no longer the default).
// ★★ The scan is SetupAPI only: no admin, and it never opens a device — so it sees a dongle that another
//    program holds, and never says "unplugged" for one that is merely in use (sdr_presence.h's rule).
// ★ Shared by vibeserver.exe (the choice it logs at start-up) and vibe-usbprobe.exe (the report).
#pragma once
#ifdef _WIN32
#include <string>
#include <vector>

namespace vibewin {

enum class Kind { Rtl, AirspyHf, Airspy, HackRf, Sdrplay, Other };
/** What the bound service means for us. */
enum class Driver { None, WinUsb, LibusbK, Libusb0, UsbDk, TvTuner, Composite, Other };

struct UsbNode {
    std::string instanceId;          // USB\VID_0BDA&PID_2838&MI_00\7&2C1A…&0&0000
    std::string hardwareIds;         // all of them, ';'-separated
    unsigned    vid = 0, pid = 0;
    int         mi = -1;             // interface number for a composite child; -1 = the device itself
    bool        compositeParent = false;   // usbccgp: its MI_xx children carry the drivers
    bool        sdrNode = false;     // ★ THE node a radio library opens (MI_00, or a non-composite device)
    std::string service;             // DEVPKEY_Device_Service — the bound driver
    std::string driverDesc, className, infPath, provider, deviceDesc, location;
    std::string upperFilters, lowerFilters;
    unsigned    problem = 0;         // CM_PROB_* (28 = no driver installed)
    bool        hasProblem = false;
    Kind        kind = Kind::Other;
    std::string model;               // "Generic RTL2832U OEM" (librtlsdr's own name for it)
    Driver      driver = Driver::None;
    std::string parentId;            // the composite parent's instance id (for an MI_xx child), else ""
    std::string serial;              // the USB serial Windows put in the instance id, or "" if it made one up
    bool        ourInterface = false;// DeviceInterfaceGUIDs carries VibeServer's GUID: WE put WinUSB here
};

/** ★ VibeServer's WinUSB device-interface GUID — written by the switch, and how a dongle WE switched is told
 *  apart from one Zadig or SDR# set up (only ours are offered a Give back). */
extern const wchar_t* const kVibeIfaceGuid;

/** Is this VID:PID a radio we know (RTL-SDR from librtlsdr's own table, Airspy, HackRF, SDRplay)? */
bool identifyRadio(unsigned vid, unsigned pid, Kind& kind, std::string& model);

/** Every present USB node that belongs to a radio we know, parents and interfaces alike. */
std::vector<UsbNode> scan();

/** What a bound service (and its setup class) means for us. */
Driver classifyDriver(const std::string& service, const std::string& className);

const char* kindName(Kind k);
const char* driverName(Driver d);
/** The one line VibeServer would show for this radio (plan §3.1). */
std::string verdict(const UsbNode& n, bool usbdkUsable);
/** The serial from an instance id's last part, or "" when Windows generated it (it then contains '&'). */
std::string serialFromInstanceId(const std::string& id);

struct UsbDkState {
    bool        serviceInstalled = false;
    bool        serviceRunning   = false;
    bool        hubFilter        = false;  // registered as an UpperFilter on the USB class
    std::string helperPath;                // the UsbDkHelper.dll libusb will load (System32), or ""
    std::string helperElsewhere;           // a copy found somewhere libusb will NOT look
    std::string helperVersion;
    bool usable() const { return serviceInstalled && !helperPath.empty(); }
};
UsbDkState usbdk();

enum class Backend { WinUsb, UsbDk };
/** The process-wide USB path. VIBE_USB_BACKEND=usbdk|winusb overrides; otherwise the rule above. */
Backend chooseBackend(const std::vector<UsbNode>& nodes, const UsbDkState& dk, std::string& why);

/** vibeserver.exe at start-up: scan, log one line per radio, choose, and set libusb's default
 *  (LIBUSB_OPTION_USE_USBDK) before any radio library makes its first context. */
void applyUsbBackend();

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

}  // namespace vibewin
#endif
