// win_usb.cpp — see win_usb.h. Windows only.
#ifdef _WIN32
#include "win_usb.h"

#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winsvc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libusb.h>
#include "rtl_known_devices.h"   // ★ GENERATED from the librtlsdr we link (windows/build-deps.sh)

namespace vibewin {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

namespace {

std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }

/** A REG_SZ or REG_MULTI_SZ registry property, multi-strings joined with ';'. */
std::string regProp(HDEVINFO set, SP_DEVINFO_DATA& d, DWORD prop) {
    DWORD type = 0, need = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, nullptr, 0, &need);
    if (!need) return {};
    std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, (PBYTE)buf.data(), need, nullptr)) return {};
    if (type == REG_MULTI_SZ) {
        std::string out;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) { if (!out.empty()) out += ';'; out += narrow(p); }
        return out;
    }
    return narrow(buf.data());
}
/** A string device property (DEVPKEY_*). */
std::string devProp(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key) {
    DEVPROPTYPE t = 0; DWORD need = 0;
    SetupDiGetDevicePropertyW(set, &d, &key, &t, nullptr, 0, &need, 0);
    if (!need) return {};
    std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDevicePropertyW(set, &d, &key, &t, (PBYTE)buf.data(), need, nullptr, 0)) return {};
    return t == DEVPROP_TYPE_STRING ? narrow(buf.data()) : std::string();
}

unsigned hexAfter(const std::string& id, const char* tag) {
    const size_t p = id.find(tag);
    if (p == std::string::npos) return 0xFFFFFFFFu;
    return (unsigned)strtoul(id.substr(p + strlen(tag), 4).c_str(), nullptr, 16);
}

}  // namespace

bool identifyRadio(unsigned vid, unsigned pid, Kind& kind, std::string& model) {
    for (const auto& k : kVibeRtlKnown)
        if (k.vid == vid && k.pid == pid) { kind = Kind::Rtl; model = k.name; return true; }
    if (vid == 0x03EB && pid == 0x800C) { kind = Kind::AirspyHf; model = "Airspy HF+"; return true; }
    if (vid == 0x1D50 && pid == 0x60A1) { kind = Kind::Airspy;   model = "Airspy R2 / Mini"; return true; }
    if (vid == 0x1D50 && (pid == 0x6089 || pid == 0x604B || pid == 0xCC15)) { kind = Kind::HackRf; model = "HackRF"; return true; }
    if (vid == 0x1DF7) { kind = Kind::Sdrplay; model = "SDRplay RSP"; return true; }
    return false;
}

namespace {

Driver classifyDriver(const std::string& service, const std::string& cls) {
    const std::string s = upper(service);
    if (s.empty())         return Driver::None;
    if (s == "WINUSB")     return Driver::WinUsb;
    if (s == "LIBUSBK")    return Driver::LibusbK;
    if (s == "LIBUSB0")    return Driver::Libusb0;
    if (s == "USBDK")      return Driver::UsbDk;
    if (s == "USBCCGP")    return Driver::Composite;
    // ★ The Realtek TV drivers' service names are not documented ([unverified] in the plan): anything in the
    //   MEDIA class, or named like one, is "Windows is using it as a TV tuner". The report prints the real name.
    if (upper(cls) == "MEDIA" || s.find("RTL") != std::string::npos || s.find("BDA") != std::string::npos)
        return Driver::TvTuner;
    return Driver::Other;
}

bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string fileVersion(const std::wstring& p) {
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(p.c_str(), &h);
    if (!n) return {};
    std::vector<char> b(n);
    if (!GetFileVersionInfoW(p.c_str(), 0, n, b.data())) return {};
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(b.data(), L"\\", (void**)&fi, &len) || !fi) return {};
    char v[48];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}

}  // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Rtl: return "RTL-SDR"; case Kind::AirspyHf: return "Airspy HF+"; case Kind::Airspy: return "Airspy";
        case Kind::HackRf: return "HackRF"; case Kind::Sdrplay: return "SDRplay"; default: return "other";
    }
}
const char* driverName(Driver d) {
    switch (d) {
        case Driver::None: return "no driver"; case Driver::WinUsb: return "WinUSB"; case Driver::LibusbK: return "libusbK";
        case Driver::Libusb0: return "libusb0 (libusb-win32)"; case Driver::UsbDk: return "UsbDk (borrowed)";
        case Driver::TvTuner: return "TV tuner (DVB-T/BDA)"; case Driver::Composite: return "USB composite parent (usbccgp)";
        default: return "another driver";
    }
}

std::vector<UsbNode> scan() {
    std::vector<UsbNode> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{}; d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t idw[MAX_DEVICE_ID_LEN + 1] = {0};
        if (!SetupDiGetDeviceInstanceIdW(set, &d, idw, MAX_DEVICE_ID_LEN, nullptr)) continue;
        UsbNode n;
        n.instanceId = narrow(idw);
        const std::string uid = upper(n.instanceId);
        n.vid = hexAfter(uid, "VID_"); n.pid = hexAfter(uid, "PID_");
        if (n.vid > 0xFFFF || n.pid > 0xFFFF) continue;
        if (!identifyRadio(n.vid, n.pid, n.kind, n.model)) continue;
        const unsigned mi = hexAfter(uid, "&MI_");
        n.mi = mi <= 0xFF ? (int)mi : -1;
        n.hardwareIds  = regProp(set, d, SPDRP_HARDWAREID);
        n.service      = regProp(set, d, SPDRP_SERVICE);
        n.className    = regProp(set, d, SPDRP_CLASS);
        n.deviceDesc   = regProp(set, d, SPDRP_DEVICEDESC);
        n.location     = regProp(set, d, SPDRP_LOCATION_INFORMATION);
        n.upperFilters = regProp(set, d, SPDRP_UPPERFILTERS);
        n.lowerFilters = regProp(set, d, SPDRP_LOWERFILTERS);
        n.driverDesc   = devProp(set, d, DEVPKEY_Device_DriverDesc);
        n.infPath      = devProp(set, d, DEVPKEY_Device_DriverInfPath);
        n.provider     = devProp(set, d, DEVPKEY_Device_DriverProvider);
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, d.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) {
            n.hasProblem = true; n.problem = prob;
        }
        n.driver = classifyDriver(n.service, n.className);
        n.compositeParent = n.mi < 0 && n.driver == Driver::Composite;
        out.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(set);
    // ★ The node a radio library opens: interface 0 of a composite device, or the device itself.
    for (auto& n : out) n.sdrNode = (n.mi == 0) || (n.mi < 0 && !n.compositeParent);
    return out;
}

std::string verdict(const UsbNode& n, bool usbdkUsable) {
    const std::string what = std::string(kindName(n.kind)) + " found";
    if (n.compositeParent) return what + " — composite parent; its interface 0 carries the radio";
    if (!n.sdrNode) return what + " — interface " + std::to_string(n.mi) + " (not the radio; left alone)";
    switch (n.driver) {
        case Driver::WinUsb:  return what + " — using WinUSB (set up by Zadig, SDR# or this probe). Ready.";
        case Driver::LibusbK: return what + " — using libusbK. Ready.";
        case Driver::Libusb0: return what + " — using libusb-win32 (libusb0). Ready.";
        case Driver::UsbDk:   return what + " — borrowed via UsbDk right now. Ready.";
        case Driver::None:
            return what + (usbdkUsable ? " — no driver; VibeServer will BORROW it via UsbDk. Ready."
                                       : " — no driver yet: needs one-time permission (vibe-usbprobe bind), or UsbDk");
        case Driver::TvTuner:
            return what + " — Windows is using it as a TV tuner (" + n.service + ")" +
                   (usbdkUsable ? "; VibeServer will BORROW it via UsbDk and give it back. Ready."
                                : "; needs UsbDk (borrow) or vibe-usbprobe bind (switch to WinUSB)");
        default:
            if (n.kind != Kind::Rtl && n.driver == Driver::Other)
                return what + " — on " + n.service + " (" + n.driverDesc + ")";
            return what + " — on another driver (" + n.service + ")" +
                   (usbdkUsable ? "; UsbDk can borrow it" : "; needs UsbDk or vibe-usbprobe bind");
    }
}

UsbDkState usbdk() {
    UsbDkState s;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE sv = OpenServiceW(scm, L"UsbDk", SERVICE_QUERY_STATUS)) {
            s.serviceInstalled = true;
            SERVICE_STATUS ss{};
            if (QueryServiceStatus(sv, &ss)) s.serviceRunning = ss.dwCurrentState == SERVICE_RUNNING;
            CloseServiceHandle(sv);
        }
        CloseServiceHandle(scm);
    }
    // ★ The filter is registered on the USB class key (that is how it sees every hub).
    wchar_t buf[2048] = {0}; DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\Class\\{36FC9E60-C465-11CF-8056-444553540000}",
                     L"UpperFilters", RRF_RT_REG_MULTI_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) if (!_wcsicmp(p, L"UsbDk")) s.hubFilter = true;
    }
    // ★★ libusb loads UsbDkHelper.dll from System32 ONLY (load_system_library) — a copy anywhere else is
    //    reported, because "installed but not where libusb looks" is a different fix from "not installed".
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring inSys = std::wstring(sys) + L"\\UsbDkHelper.dll";
    if (fileExists(inSys)) { s.helperPath = narrow(inSys); s.helperVersion = fileVersion(inSys); }
    wchar_t pf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        const std::wstring alt = std::wstring(pf) + L"\\UsbDk Runtime Library\\UsbDkHelper.dll";
        if (fileExists(alt)) { s.helperElsewhere = narrow(alt); if (s.helperVersion.empty()) s.helperVersion = fileVersion(alt); }
    }
    return s;
}

Backend chooseBackend(const std::vector<UsbNode>& nodes, const UsbDkState& dk, std::string& why) {
    if (const char* e = std::getenv("VIBE_USB_BACKEND"); e && *e) {
        if (!_stricmp(e, "usbdk")) { why = "VIBE_USB_BACKEND=usbdk"; return Backend::UsbDk; }
        if (!_stricmp(e, "winusb")) { why = "VIBE_USB_BACKEND=winusb"; return Backend::WinUsb; }
    }
    bool anyRtl = false, allReady = true;
    for (const auto& n : nodes) {
        if (n.kind != Kind::Rtl || !n.sdrNode) continue;
        anyRtl = true;
        if (n.driver != Driver::WinUsb && n.driver != Driver::LibusbK && n.driver != Driver::Libusb0) allReady = false;
    }
    if (anyRtl && allReady) { why = "every RTL-SDR is already on a libusb driver — using it as it is"; return Backend::WinUsb; }
    if (dk.usable()) {
        why = anyRtl ? "UsbDk is installed — borrowing the RTL-SDR(s) while VibeServer runs"
                     : "UsbDk is installed (no RTL-SDR plugged in yet)";
        return Backend::UsbDk;
    }
    why = anyRtl ? "UsbDk is not installed — an RTL-SDR on the TV driver needs vibe-usbprobe bind first"
                 : "no RTL-SDR plugged in; UsbDk not installed";
    return Backend::WinUsb;
}

void applyUsbBackend() {
    const auto nodes = scan();
    const auto dk = usbdk();
    for (const auto& n : nodes)
        if (n.sdrNode) std::printf("VibeServer: USB  %s (%04x:%04x) — %s\n", n.model.c_str(), n.vid, n.pid,
                                   verdict(n, dk.usable()).c_str());
    std::string why;
    const Backend b = chooseBackend(nodes, dk, why);
    if (b == Backend::UsbDk) {
        // ★★ Proved BEFORE it becomes the default: a context made with the option must initialise, or every
        //    radio library would fail to open anything and the owner would see "no radio" for a driver fault.
        libusb_init_option opt{}; opt.option = LIBUSB_OPTION_USE_USBDK;
        libusb_context* ctx = nullptr;
        if (libusb_init_context(&ctx, &opt, 1) == 0) {
            libusb_exit(ctx);
            libusb_set_option(nullptr, LIBUSB_OPTION_USE_USBDK);
            std::printf("VibeServer: USB path = UsbDk (borrow) — %s\n", why.c_str());
            return;
        }
        std::printf("VibeServer: USB path = WinUSB — UsbDk is installed but libusb could not start it "
                    "(UsbDkHelper.dll at %s)\n", dk.helperPath.empty() ? "?" : dk.helperPath.c_str());
        return;
    }
    std::printf("VibeServer: USB path = WinUSB — %s\n", why.c_str());
}

}  // namespace vibewin
#endif
