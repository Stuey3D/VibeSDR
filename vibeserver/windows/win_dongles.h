// win_dongles.h — vibeserver.exe's side of "Switch this TV stick to SDR" and "Start when Windows starts"
// (Windows only): the watch on the USB bus, the offer, the one-prompt helper, and the setup page's routes.
//
// ★★★ THE FLOW (Stuart, 2026-10-26):
//   1. At start and whenever a USB device arrives, the server lists the RTL-SDRs (win_switch.h). A stick Windows
//      uses as a TV tuner — or has no driver for — is OFFERED: a console line, and a card in the setup page with
//      SWITCH and NOT NOW. A stick that already works is used silently and never mentioned.
//   2. SWITCH launches `vibeserver.exe --winusb-bind <instance id> --reply <pipe>` through ShellExecuteEx "runas":
//      exactly ONE UAC prompt. The helper re-checks the device itself, puts Windows' in-box winusb.inf on it
//      (win_bind.h), and reports every step back over a named pipe that only this process created. The server itself
//      never runs elevated.
//   3. The server then finds the dongle with librtlsdr and adds it as a radio straight away — no restart of Windows,
//      no replug — unless Windows itself says a restart is needed, which the page then says in plain words.
//   4. GIVE BACK (one prompt) stops that radio first, then uninstalls the node so Windows picks its TV driver again.
// ★★ NOTHING IS REMEMBERED THAT COULD SWITCH A DONGLE BEHIND ANYONE'S BACK: no list of "bound" devices, no rule
//    applied on arrival. Windows itself remembers WinUSB for that device instance across replugs (that is per-device,
//    by design), and the server simply finds it usable next time.
#pragma once
#ifdef _WIN32
#include <functional>
#include <string>
#include <vector>
#include "win_usb.h"

namespace vibewin {

struct DongleHooks {
    int port = 48000;
    /** librtlsdr's serials for every RTL-SDR it can open right now. */
    std::function<std::vector<std::string>()> rtlSerials;
    /** After a successful switch: `fresh` are serials librtlsdr can now open that it could not before. Returns what
     *  to tell the owner ("added as a radio — set it up in its tab"). */
    std::function<std::string(const std::vector<std::string>& fresh)> afterSwitch;
    /** Before a give-back: stop serving the radio with this serial (may be ""), so Windows can remove it cleanly. */
    std::function<void(const std::string& serial)> releaseForGiveBack;
};

/** The front door, once: scan, print the offers, and watch for USB arrivals (CM_Register_Notification). */
void startDongleWatch(const DongleHooks& hooks);

/** /vibeserver/windows/* — hello (open), state / switch / giveback / notnow / autostart (admin). True if answered. */
bool handleWindowsHttp(const std::string& method, const std::string& path, const std::string& body, bool adminOk,
                       int& code, std::string& status, std::string& out);

/** `vibeserver.exe --winusb-bind | --winusb-giveback | --autostart-set …` — the elevated helper. */
int helperMain(int argc, char** argv);

/** The device list the server works from: SetupAPI, or VIBE_WIN_FAKE_USB's file (tests). Never used by the helper. */
std::vector<UsbNode> serverNodes();

}  // namespace vibewin
#endif
