// win_bind.cpp — see win_bind.h. Windows only.
#ifdef _WIN32
#include "win_bind.h"
#include "win_usb.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>   // ★ for the types; DiInstallDevice / DiUninstallDevice are loaded from newdev.dll (diFn)
#include <chrono>
#include <thread>
#include <vector>

namespace vibewin {
namespace {

// ★ GUID_DEVCLASS_USBDEVICE — "Universal Serial Bus devices", the class winusb.inf installs into.
const wchar_t* const kUsbDeviceClass = L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}";

std::string err(DWORD e) { return "error " + std::to_string(e); }

typedef BOOL (WINAPI *DiInstall_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, void*, DWORD, PBOOL);
typedef BOOL (WINAPI *DiUninstall_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
/** ★ newdev.dll at run time: llvm-mingw ships no import library for it on ARM64, and loading it by name from
 *  System32 only keeps both builds the same (and keeps a planted newdev.dll in the folder out). */
template <class F> F diFn(const char* name) {
    static HMODULE m = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? (F)(void*)GetProcAddress(m, name) : nullptr;
}

struct Dev {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA d{};
    ~Dev() { if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set); }
    bool open(const std::string& inst, const LogFn& log) {
        set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        d = SP_DEVINFO_DATA{}; d.cbSize = sizeof d;
        if (set == INVALID_HANDLE_VALUE) { log("  !! SetupDiCreateDeviceInfoList: " + err(GetLastError())); return false; }
        if (!SetupDiOpenDeviceInfoW(set, widen(inst).c_str(), nullptr, 0, &d)) {
            log("  !! that device is not present (SetupDiOpenDeviceInfo: " + err(GetLastError()) + ")");
            return false;
        }
        return true;
    }
};

std::wstring winusbInf() {
    wchar_t win[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(win, MAX_PATH);
    return std::wstring(win) + L"\\INF\\winusb.inf";
}

/** Look through a driver list for Windows' "WinUsb Device" (winusb.inf) — and nothing else. */
bool findWinUsb(Dev& v, DWORD type, SP_DRVINFO_DATA_W& out, const std::string& how, const LogFn& log) {
    int listed = 0;
    for (DWORD i = 0; ; ++i) {
        SP_DRVINFO_DATA_W drv{}; drv.cbSize = sizeof drv;
        if (!SetupDiEnumDriverInfoW(v.set, &v.d, type, i, &drv)) break;
        ++listed;
        std::vector<char> det(8192);
        auto* dd = (SP_DRVINFO_DETAIL_DATA_W*)det.data(); dd->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        std::string inf;
        if (SetupDiGetDriverInfoDetailW(v.set, &v.d, &drv, dd, (DWORD)det.size(), nullptr)) inf = narrow(dd->InfFileName);
        std::string lower = inf; for (char& c : lower) c = (char)tolower((unsigned char)c);
        // ★★★ ONLY THE IN-BOX FILE. A same-named oemNN.inf (Zadig's) would also say "WinUsb Device"; requiring the
        //     path to be %WINDIR%\INF\winusb.inf is what keeps every bind on Microsoft's own signed package.
        const std::string want = [] { std::string w = narrow(winusbInf()); for (char& c : w) c = (char)tolower((unsigned char)c); return w; }();
        if (lower == want) {
            log("  " + how + ": found \"" + narrow(drv.Description) + "\" by " + narrow(drv.MfgName) + " in " + inf);
            out = drv;
            return true;
        }
    }
    log("  " + how + ": " + std::to_string(listed) + " driver(s) listed, none of them Windows' WinUsb Device");
    return false;
}

/** Build one driver list from winusb.inf alone. */
bool listFromWinusbInf(Dev& v, DWORD type, SP_DRVINFO_DATA_W& drv, const std::string& how, const LogFn& log) {
    SP_DEVINSTALL_PARAMS_W ip{}; ip.cbSize = sizeof ip;
    SetupDiGetDeviceInstallParamsW(v.set, &v.d, &ip);
    ip.Flags |= DI_ENUMSINGLEINF;
    ip.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;   // ★ winusb.inf's model is Exclude-From-Select for PnP devices
    const std::wstring inf = winusbInf();
    wcsncpy(ip.DriverPath, inf.c_str(), MAX_PATH - 1);
    if (!SetupDiSetDeviceInstallParamsW(v.set, &v.d, &ip)) { log("  " + how + ": install params " + err(GetLastError())); return false; }
    if (!SetupDiBuildDriverInfoList(v.set, &v.d, type)) { log("  " + how + ": list failed, " + err(GetLastError())); return false; }
    if (findWinUsb(v, type, drv, how, log)) return true;
    SetupDiDestroyDriverInfoList(v.set, &v.d, type);
    return false;
}

void restartDevice(Dev& v) {
    SP_PROPCHANGE_PARAMS pc{};
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = DICS_PROPCHANGE; pc.Scope = DICS_FLAG_GLOBAL;
    if (SetupDiSetClassInstallParamsW(v.set, &v.d, &pc.ClassInstallHeader, sizeof pc))
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, v.set, &v.d);
}

bool rescanAll(const LogFn& log) {
    DEVINST root = 0;
    if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) { log("  rescan: no root"); return false; }
    const bool ok = CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS) == CR_SUCCESS;
    log(std::string("  rescan: ") + (ok ? "done" : "failed"));
    return ok;
}

const UsbNode* findNode(const std::vector<UsbNode>& nodes, const std::string& inst) {
    for (const auto& n : nodes) if (!_stricmp(n.instanceId.c_str(), inst.c_str())) return &n;
    return nullptr;
}

}  // namespace

BindResult bindWinUsb(const std::string& inst, const LogFn& log) {
    BindResult r;
    log("bind: Windows' own WinUSB (winusb.inf) onto " + inst);
    Dev v;
    if (!v.open(inst, log)) { r.error = "the dongle is not plugged in any more"; return r; }

    // 1. ★★ The interface GUID FIRST: winusb.sys registers a device interface only if DeviceInterfaceGUIDs is
    //       there when it starts, and libusb finds a WinUSB device only through that interface. It is in the
    //       device's own hardware key, which an uninstall (Give back) deletes with the node.
    HKEY k = SetupDiCreateDevRegKeyW(v.set, &v.d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (k != INVALID_HANDLE_VALUE) {
        std::wstring val = std::wstring(kVibeIfaceGuid) + L'\0' + L'\0';
        const LONG e = RegSetValueExW(k, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*)val.data(), (DWORD)(val.size() * sizeof(wchar_t)));
        RegCloseKey(k);
        log(std::string("  DeviceInterfaceGUIDs ") + (e == ERROR_SUCCESS ? "written" : ("NOT written, " + err((DWORD)e))));
    } else log("  !! could not open the device's registry key (" + err(GetLastError()) + ")");

    // 2. Reach "WinUsb Device" — best first, every attempt reported.
    //    A  winusb.inf as a CLASS list ("Have Disk"). SetupAPI lists only drivers of the DEVICE's own class
    //       (SetupDiBuildDriverInfoList remarks), so this finds it for a stick with NO driver (state a1, no class
    //       yet) or one already in "Universal Serial Bus devices".
    //    B  The device moved to "Universal Serial Bus devices" first, then A again — the step Device Manager's
    //       "Let me pick → Universal Serial Bus devices" takes for a stick in the TV (MEDIA) class (state a2).
    //       SetupDiSetSelectedDriver/DiInstallDevice would change the class anyway once the driver is chosen; this
    //       only does it early enough for the list to contain it. Undone by Give back (uninstall + rescan).
    //    C  winusb.inf as a COMPATIBLE list — reached only if Windows matches the stick's ids to it (it should
    //       not: winusb.inf lists only USB\MS_COMP_WINUSB). Kept because it costs nothing and its answer is data.
    //  ✗  UpdateDriverForPlugAndPlayDevices is NOT used: it installs an INF for every device matching a HARDWARE
    //     ID that the INF itself lists. winusb.inf lists no RTL id, so it could only work with an INF of our own —
    //     the custom INF + certificate route this file exists to avoid. It is also not device-specific.
    SP_DRVINFO_DATA_W drv{}; drv.cbSize = sizeof drv;
    bool classChanged = false;
    if (listFromWinusbInf(v, SPDIT_CLASSDRIVER, drv, "A: winusb.inf as a class list", log)) r.method = "A (class list)";
    if (r.method.empty()) {
        wchar_t cur[64] = {0}; DWORD ty = 0;
        SetupDiGetDeviceRegistryPropertyW(v.set, &v.d, SPDRP_CLASSGUID, &ty, (PBYTE)cur, sizeof cur - sizeof(wchar_t), nullptr);
        if (_wcsicmp(cur, kUsbDeviceClass) != 0) {
            const std::wstring cls = kUsbDeviceClass;
            if (SetupDiSetDeviceRegistryPropertyW(v.set, &v.d, SPDRP_CLASSGUID, (const BYTE*)cls.c_str(),
                                                  (DWORD)((cls.size() + 1) * sizeof(wchar_t)))) {
                classChanged = true;
                log("  B: device class " + (cur[0] ? narrow(cur) : std::string("(none)")) + " → Universal Serial Bus devices");
                if (listFromWinusbInf(v, SPDIT_CLASSDRIVER, drv, "B: winusb.inf as a class list, after the class change", log))
                    r.method = "B (class change + class list)";
            } else log("  B: Windows refused the class change (" + err(GetLastError()) + ")");
        } else log("  B: already in Universal Serial Bus devices — nothing to change");
    }
    if (r.method.empty() && listFromWinusbInf(v, SPDIT_COMPATDRIVER, drv, "C: winusb.inf as a compatible list", log))
        r.method = "C (compatible list)";

    if (r.method.empty()) {
        r.error = "Windows offered no way to pick its own WinUsb Device for this dongle";
        log("  RESULT: FAIL — " + r.error);
        if (classChanged) {
            // ★ Leave nothing half-done: the class change dropped its TV driver key; an uninstall + rescan lets
            //   Windows put back exactly what it would choose itself.
            log("  undoing the class change (uninstall + rescan, so Windows picks its own driver again)");
            BOOL rb = FALSE;
            if (auto un = diFn<DiUninstall_t>("DiUninstallDevice")) un(nullptr, v.set, &v.d, 0, &rb);
            rescanAll(log);
        }
        return r;
    }

    // 3. Install it on this one node.
    if (!SetupDiSetSelectedDriverW(v.set, &v.d, &drv)) log("  SetupDiSetSelectedDriver: " + err(GetLastError()));
    BOOL reboot = FALSE;
    auto diInstall = diFn<DiInstall_t>("DiInstallDevice");
    const BOOL ok = diInstall && diInstall(nullptr, v.set, &v.d, &drv, DIIDFLAG_NOFINISHINSTALLUI, &reboot);
    const DWORD e = ok ? 0 : GetLastError();
    log(std::string("  DiInstallDevice: ") + (ok ? "OK" : err(e)) + (reboot ? " — Windows asks for a RESTART" : ""));
    if (!ok) {
        r.error = diInstall ? "Windows refused to install its WinUsb Device (" + err(e) + ")" : "newdev.dll has no DiInstallDevice";
        if (classChanged) { BOOL rb = FALSE; if (auto un = diFn<DiUninstall_t>("DiUninstallDevice")) un(nullptr, v.set, &v.d, 0, &rb); rescanAll(log); }
        return r;
    }
    restartDevice(v);
    r.reboot = reboot != FALSE;
    // 4. Check, from Windows' own record, that the node is now WinUSB with our interface.
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto nodes = scan();
        if (const UsbNode* n = findNode(nodes, inst)) {
            if (n->driver == Driver::WinUsb) {
                log("  now: WinUSB [service \"" + n->service + "\", " + n->infPath + "]" + (n->ourInterface ? ", VibeServer's interface" : ""));
                r.ok = true;
                return r;
            }
            if (i == 9) log("  now: " + std::string(driverName(n->driver)) + " [service \"" + n->service + "\"]");
        }
    }
    // ★ Installed but not yet started is what a pending restart looks like.
    r.ok = r.reboot;
    if (!r.ok) r.error = "Windows installed WinUSB but the dongle did not start on it — unplug it and plug it back in";
    return r;
}

BindResult giveBack(const std::string& inst, const LogFn& log) {
    BindResult r;
    log("give back: " + inst + " returns to Windows' own choice of driver");
    Dev v;
    if (!v.open(inst, log)) { r.error = "the dongle is not plugged in — plug it in, then give it back"; return r; }
    BOOL reboot = FALSE;
    auto diUninstall = diFn<DiUninstall_t>("DiUninstallDevice");
    const BOOL ok = diUninstall && diUninstall(nullptr, v.set, &v.d, 0, &reboot);
    const DWORD e = ok ? 0 : GetLastError();
    log(std::string("  DiUninstallDevice: ") + (ok ? "OK" : err(e)) + (reboot ? " — Windows asks for a RESTART" : ""));
    if (!ok) { r.error = "Windows refused to uninstall the device (" + err(e) + ")"; return r; }
    rescanAll(log);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto nodes = scan();
    if (const UsbNode* n = findNode(nodes, inst))
        log("  now: " + std::string(driverName(n->driver)) + (n->service.empty() ? "" : " [service \"" + n->service + "\"]"));
    r.ok = true; r.reboot = reboot != FALSE; r.method = "DiUninstallDevice + rescan";
    return r;
}

}  // namespace vibewin
#endif
