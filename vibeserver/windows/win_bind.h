// win_bind.h — put Microsoft's own in-box WinUSB on ONE RTL-SDR, and give it back (Windows only).
//
// ★★★ STUART'S RULES (2026-10-26), BINDING ON THIS FILE:
//     "no manual Zadig messing around, ONE admin prompt to cleanly replace the driver"
//     "Anything that requires a user to lower their security is a big no go."
//   So the driver is %WINDIR%\INF\winusb.inf — Windows' own, Microsoft-signed, already on every PC since Windows 8 —
//   selected for this one device instance by SetupAPI and installed with DiInstallDevice. There is NO INF of ours,
//   NO catalog, NO certificate (Zadig/libwdi generate and trust one — exactly what we must not do), and nothing
//   about Windows' signing, Memory Integrity, Smart App Control or Secure Boot is touched.
// ★★ If none of the ways below can reach "WinUsb Device" for a stick on the TV driver, the result is FAIL with the
//    reason — there is no fallback that bends a rule. That outcome is the Windows no-go finding.
// ★ Shared by vibeserver.exe's elevated helper (--winusb-bind / --winusb-giveback) and vibe-usbprobe.exe.
#pragma once
#ifdef _WIN32
#include <functional>
#include <string>

namespace vibewin {

using LogFn = std::function<void(const std::string&)>;

struct BindResult {
    bool        ok = false;
    bool        reboot = false;      // Windows said the change completes only after a restart
    std::string method;              // which way reached "WinUsb Device"
    std::string error;               // plain words when !ok
};

/** Needs administrator rights. Every step is reported through `log`. ★ The caller has already checked that
 *  `instanceId` is an RTL-SDR's radio node in an offerable state (win_switch.h: helperMayBind). */
BindResult bindWinUsb(const std::string& instanceId, const LogFn& log);

/** Needs administrator rights. Uninstalls THIS device node (its driver choice with it) and rescans, so Windows
 *  chooses again — the TV driver if its package is installed, otherwise none. winusb.inf matches no RTL hardware
 *  id, so Windows never picks it again by itself. */
BindResult giveBack(const std::string& instanceId, const LogFn& log);

}  // namespace vibewin
#endif
