// vibe-usbprobe.exe — Milestone 1 of the Windows port: ANSWER THE DRIVER QUESTION ON A REAL PC.
//
// ★★★ WHAT IT IS FOR (docs/WINDOWS-PORT-PLAN.md §5, docs/WINDOWS-M1-TEST.md). Since test build 2 the USER flow is
//     in vibeserver.exe itself — the setup page offers a TV stick a switch to SDR, one UAC prompt (Stuart,
//     2026-10-26). This probe is the DIAGNOSTIC beside it: it reports what Windows has on each dongle, streams in
//     counter mode to prove a route is clean, and its `bind` / `giveback` run the very same code as the server's
//     SWITCH / Give back (win_bind.cpp) with every step printed. UsbDk is tested only with --usbdk (optional).
// ★★ Every command prints a plain-text report and APPENDS it to vibe-usbprobe.log beside the .exe, so the
//    whole session can be pasted back in one go.
// ★ Commands: list · usbdk · report · stream · cycle · bind · giveback   (help: no arguments)
#ifdef _WIN32
#include "win_usb.h"
#include "win_bind.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>   // ★ for the types; the two functions are loaded from newdev.dll (see diFns)
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>
#include <libusb.h>
#include <rtl-sdr.h>

using namespace vibewin;
using Clock = std::chrono::steady_clock;

namespace {

FILE* g_log = nullptr;         // vibe-usbprobe.log beside the .exe (append)
FILE* g_relay = nullptr;       // --log <file>: the elevated copy's output, read back by the one that asked

void say(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::fputs(buf, stdout);
    if (g_log) { std::fputs(buf, g_log); std::fflush(g_log); }
    if (g_relay) { std::fputs(buf, g_relay); std::fflush(g_relay); }
}

std::wstring exeDir() {
    wchar_t p[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    std::wstring s(p, n);
    const size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : s.substr(0, cut);
}

bool isAdmin() { return IsUserAnAdmin() != FALSE; }

std::string regStr(HKEY root, const wchar_t* key, const wchar_t* val) {
    wchar_t b[512] = {0}; DWORD sz = sizeof b;
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, nullptr, b, &sz) != ERROR_SUCCESS) return {};
    return narrow(b);
}
long regDword(HKEY root, const wchar_t* key, const wchar_t* val) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(root, key, val, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return -1;
    return (long)v;
}

/** The machine facts the plan's [unverified] rows depend on. */
void sayMachine() {
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
    if (auto f = (RtlGetVersion_t)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::string disp = regStr(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    const long ubr = regDword(HKEY_LOCAL_MACHINE, cv, L"UBR");
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" : "other";
    say("Windows %lu.%lu build %lu.%ld (%s), %s, %s\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr,
        disp.empty() ? "?" : disp.c_str(), arch, isAdmin() ? "running as administrator" : "running as a normal user");
    const long hvci = regDword(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled");
    const long sb = regDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled");
    const long sac = regDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", L"VerifiedAndReputablePolicyState");
    say("Memory Integrity (HVCI): %s · Secure Boot: %s · Smart App Control: %s\n",
        hvci == 1 ? "ON" : hvci == 0 ? "off" : "not configured",
        sb == 1 ? "on" : sb == 0 ? "off" : "unknown",
        sac == 1 ? "ON (enforcing)" : sac == 2 ? "evaluation" : sac == 0 ? "off" : "unknown");
}

void sayUsbDk(const UsbDkState& dk) {
    say("UsbDk: service %s%s · hub filter %s · UsbDkHelper.dll %s%s%s\n",
        dk.serviceInstalled ? "installed" : "NOT installed",
        dk.serviceInstalled ? (dk.serviceRunning ? " (running)" : " (not running — it starts on demand)") : "",
        dk.hubFilter ? "registered" : "not registered",
        dk.helperPath.empty() ? "not in System32" : dk.helperPath.c_str(),
        dk.helperVersion.empty() ? "" : (" v" + dk.helperVersion).c_str(),
        dk.helperElsewhere.empty() ? "" : ("; also at " + dk.helperElsewhere).c_str());
    if (dk.helperPath.empty() && !dk.helperElsewhere.empty())
        say("  ★ libusb looks for UsbDkHelper.dll in System32 only — the copy above will not be found.\n");
}

void sayNodes(const std::vector<UsbNode>& nodes, const UsbDkState& dk) {
    if (nodes.empty()) { say("No radio we know is plugged in (RTL-SDR, Airspy, HackRF, SDRplay).\n"); return; }
    int i = 0;
    for (const auto& n : nodes) {
        say("\n[%d] %s %04x:%04x  %s%s\n", i++, n.model.c_str(), n.vid, n.pid,
            n.compositeParent ? "(composite parent)" : n.mi >= 0 ? ("(interface " + std::to_string(n.mi) + ")").c_str() : "(whole device)",
            n.sdrNode ? "  ← the radio's node" : "");
        say("    instance:  %s\n", n.instanceId.c_str());
        say("    driver:    %s  [service \"%s\", class \"%s\"]\n", driverName(n.driver),
            n.service.c_str(), n.className.c_str());
        if (!n.driverDesc.empty() || !n.infPath.empty())
            say("    package:   %s · %s · %s\n", n.driverDesc.c_str(), n.provider.c_str(), n.infPath.c_str());
        if (!n.upperFilters.empty() || !n.lowerFilters.empty())
            say("    filters:   upper \"%s\" lower \"%s\"\n", n.upperFilters.c_str(), n.lowerFilters.c_str());
        if (n.hasProblem)
            say("    problem:   code %u%s\n", n.problem, n.problem == CM_PROB_FAILED_INSTALL || n.problem == 28 ? " (no driver installed)" : "");
        say("    VibeServer: %s\n", verdict(n, dk.usable()).c_str());
    }
}

/** What libusb itself can reach, with one backend or the other — "would VibeServer see it". */
void sayLibusbView(bool useUsbDk) {
    libusb_context* ctx = nullptr;
    libusb_init_option opt{}; opt.option = LIBUSB_OPTION_USE_USBDK;
    const int r = libusb_init_context(&ctx, useUsbDk ? &opt : nullptr, useUsbDk ? 1 : 0);
    if (r != 0) { say("libusb (%s): could not start — %s\n", useUsbDk ? "UsbDk" : "WinUSB", libusb_error_name(r)); return; }
    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    int shown = 0;
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        Kind k; std::string model;
        if (!identifyRadio(dd.idVendor, dd.idProduct, k, model)) continue;
        libusb_device_handle* h = nullptr;
        const int o = libusb_open(list[i], &h);
        say("libusb (%s): %04x:%04x on bus %u port %u — open %s\n", useUsbDk ? "UsbDk" : "WinUSB", dd.idVendor,
            dd.idProduct, libusb_get_bus_number(list[i]), libusb_get_port_number(list[i]),
            o == 0 ? "OK" : libusb_error_name(o));
        if (h) libusb_close(h);
        ++shown;
    }
    if (!shown) say("libusb (%s): no radio it can see\n", useUsbDk ? "UsbDk" : "WinUSB");
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

// ── stream / cycle: librtlsdr in the RTL2832's counter mode ─────────────────────────────────────
// ★★ The chip's TEST MODE replaces the samples with a free-running 8-bit counter, so every lost byte shows
//    as a jump in the sequence — an exact drop count, not an estimate from the byte rate. Then a few
//    seconds of real, tuned samples prove the tuner itself (and, on a V4, HF through its upconverter).

struct StreamStats {
    std::atomic<uint64_t> bytes{0}, buffers{0}, shortBufs{0}, jumps{0};
    uint8_t  next = 0;
    bool     primed = false;
    uint32_t bufLen = 0;
};

void onSamples(unsigned char* buf, uint32_t len, void* ctx) {
    auto* st = (StreamStats*)ctx;
    if (st->bufLen && len != st->bufLen) st->shortBufs++;
    for (uint32_t i = 0; i < len; ++i) {
        if (st->primed && buf[i] != st->next) st->jumps++;
        st->next = (uint8_t)(buf[i] + 1);
        st->primed = true;
    }
    st->bytes += len;
    st->buffers++;
}

struct Opts {
    int index = 0;
    double rate = 2400000, freq = 100e6, seconds = 60;
    bool usbdk = false, hf = false, biasT = false, quiet = false;
    int cycles = 10;
};

bool setBackend(bool usbdk) {
    if (!usbdk) return true;
    // ★ librtlsdr makes its OWN libusb context inside rtlsdr_open; a NULL-context option is libusb's default
    //   for every context made after it (libusb ≥ 1.0.27), which is exactly what reaches that one.
    const int r = libusb_set_option(nullptr, LIBUSB_OPTION_USE_USBDK);
    if (r != 0) { say("!! libusb would not take the UsbDk option: %s\n", libusb_error_name(r)); return false; }
    return true;
}

/** One counter-mode run on an open device. Returns true when it was clean. */
bool counterRun(rtlsdr_dev_t* dev, const Opts& o, double seconds, bool perSecond) {
    rtlsdr_set_sample_rate(dev, (uint32_t)o.rate);
    rtlsdr_set_testmode(dev, 1);
    rtlsdr_reset_buffer(dev);
    StreamStats st; st.bufLen = 16 * 16384;
    std::atomic<bool> done{false};
    const auto t0 = Clock::now();
    std::thread stopper([&] {
        uint64_t lastBytes = 0;
        for (int s = 1; s <= (int)seconds && !done; ++s) {
            std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
            const uint64_t b = st.bytes.load();
            if (perSecond)
                say("  %3ds  %.3f MS/s  jumps %llu  short buffers %llu\n", s, (b - lastBytes) / 2.0 / 1e6,
                    (unsigned long long)st.jumps.load(), (unsigned long long)st.shortBufs.load());
            lastBytes = b;
        }
        rtlsdr_cancel_async(dev);
    });
    const int r = rtlsdr_read_async(dev, onSamples, &st, 15, st.bufLen);
    done = true;
    stopper.join();
    rtlsdr_set_testmode(dev, 0);
    const double el = std::chrono::duration<double>(Clock::now() - t0).count();
    const double got = st.bytes.load() / 2.0, want = o.rate * el;
    const double pct = want > 0 ? 100.0 * got / want : 0;
    const bool clean = r == 0 && st.jumps.load() == 0 && pct > 99.0;
    say("  counter run: %.1f s, %.2f M samples (%.2f%% of %.2f MS/s), %llu jumps (lost data), %llu short buffers, "
        "read_async=%d → %s\n", el, got / 1e6, pct, o.rate / 1e6, (unsigned long long)st.jumps.load(),
        (unsigned long long)st.shortBufs.load(), r, clean ? "CLEAN" : "NOT CLEAN");
    return clean;
}

/** A second of real samples at `freq`: is the tuner delivering signal (not a flat line)? */
void tunedCheck(rtlsdr_dev_t* dev, double freq, const char* label) {
    rtlsdr_set_tuner_gain_mode(dev, 0);
    const int fr = rtlsdr_set_center_freq(dev, (uint32_t)freq);
    rtlsdr_reset_buffer(dev);
    std::vector<uint8_t> b(262144);
    int n = 0;
    const int r = rtlsdr_read_sync(dev, b.data(), (int)b.size(), &n);
    double sum = 0, sum2 = 0; int lo = 255, hi = 0;
    for (int i = 0; i < n; ++i) { sum += b[i]; sum2 += (double)b[i] * b[i]; lo = std::min<int>(lo, b[i]); hi = std::max<int>(hi, b[i]); }
    const double mean = n ? sum / n : 0, var = n ? sum2 / n - mean * mean : 0;
    say("  %s %.3f MHz: set_center_freq=%d read_sync=%d, %d bytes, mean %.1f, spread %.1f (min %d max %d) → %s\n",
        label, freq / 1e6, fr, r, n, mean, var > 0 ? std::sqrt(var) : 0.0, lo, hi,
        (r == 0 && n > 0 && var > 0.5) ? "samples arriving" : "NO SIGNAL / NO SAMPLES");
}

int cmdStream(const Opts& o) {
    if (!setBackend(o.usbdk)) return 2;
    say("== stream: device %d, %.2f MS/s for %.0f s, backend %s\n", o.index, o.rate / 1e6, o.seconds, o.usbdk ? "UsbDk" : "WinUSB");
    const uint32_t cnt = rtlsdr_get_device_count();
    say("librtlsdr sees %u RTL-SDR(s)\n", cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        char m[256] = {0}, p[256] = {0}, sn[256] = {0};
        rtlsdr_get_device_usb_strings(i, m, p, sn);
        say("  #%u %s · %s · %s · serial %s\n", i, rtlsdr_get_device_name(i), m, p, sn);
    }
    rtlsdr_dev_t* dev = nullptr;
    const auto t0 = Clock::now();
    const int r = rtlsdr_open(&dev, (uint32_t)o.index);
    const double openMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (r != 0 || !dev) {
        say("!! rtlsdr_open(%d) failed: %d after %.0f ms — %s\n", o.index, r, openMs,
            o.usbdk ? "UsbDk could not borrow it (see the UsbDk line from `usbdk`)"
                    : "the dongle is not on a libusb driver (try --usbdk, or `bind`)");
        return 1;
    }
    const int tuner = rtlsdr_get_tuner_type(dev);
    say("opened in %.0f ms · tuner type %d%s\n", openMs, tuner, tuner == RTLSDR_TUNER_R828D ? " (R828D — a V4)" : "");
    const bool clean = counterRun(dev, o, o.seconds, true);
    tunedCheck(dev, o.freq, "tuned");
    if (o.hf) tunedCheck(dev, 7.1e6, "HF");
    if (o.biasT) {
        say("  bias-T ON for 3 s (measure it now if you want to) …\n");
        const int on = rtlsdr_set_bias_tee(dev, 1);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const int off = rtlsdr_set_bias_tee(dev, 0);
        say("  bias-T on=%d off=%d (0 = accepted)\n", on, off);
    }
    rtlsdr_close(dev);
    say("closed. RESULT: %s\n", clean ? "PASS — clean at this rate" : "FAIL — data was lost (see the jumps)");
    return clean ? 0 : 1;
}

int cmdCycle(const Opts& o) {
    if (!setBackend(o.usbdk)) return 2;
    say("== cycle: %d × (open, 5 s counter stream, close), device %d, %.2f MS/s, backend %s\n", o.cycles, o.index,
        o.rate / 1e6, o.usbdk ? "UsbDk" : "WinUSB");
    int clean = 0, failedOpen = 0, retried = 0;
    for (int c = 1; c <= o.cycles; ++c) {
        rtlsdr_dev_t* dev = nullptr;
        int r = -1, tries = 0;
        const auto t0 = Clock::now();
        // ★★ THE RE-BIND RACE (plan §2.3): after a close, Windows may hand the dongle back to its own driver
        //    and re-enumerate it; an immediate reopen can lose. Up to 5 tries a second apart, and COUNTED.
        for (; tries < 5; ++tries) {
            r = rtlsdr_open(&dev, (uint32_t)o.index);
            if (r == 0 && dev) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (r != 0 || !dev) { say("cycle %d: OPEN FAILED (%d) after %d tries\n", c, r, tries); ++failedOpen; continue; }
        if (tries) ++retried;
        say("cycle %d: opened in %.0f ms%s\n", c, ms, tries ? (" after " + std::to_string(tries) + " retries").c_str() : "");
        if (counterRun(dev, o, 5, false)) ++clean;
        rtlsdr_close(dev);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const bool pass = clean == o.cycles && failedOpen == 0;
    say("cycle RESULT: %d/%d clean, %d failed to open, %d needed a retry → %s\n", clean, o.cycles, failedOpen, retried,
        pass ? (retried ? "PASS (with retries — note it)" : "PASS") : "FAIL");
    return pass ? 0 : 1;
}

// ── bind / giveback: Windows' own WinUSB on one dongle, and back ─────────────────────────────────
// ★★★ NO DRIVER OF OURS, NO CERTIFICATE. The driver is winusb.inf, Microsoft's, already on every Windows
//     since 8 — the "Update driver → Let me pick → Universal Serial Bus devices → WinUsb Device" path in
//     Device Manager, done in code on ONE device (plan §2.4). Nothing is planted in the certificate store.
// ★★ What M1 has to find out is whether Windows lets code pick that driver for a node in another class
//    (state a2, the TV driver) or with no driver (a1). So every attempt is reported as it is made.

std::wstring stateFile() {
    wchar_t pd[MAX_PATH] = L"C:\\ProgramData";
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    const std::wstring dir = std::wstring(pd) + L"\\VibeServer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\usbprobe-bound.txt";
}

const UsbNode* pickRtl(const std::vector<UsbNode>& nodes, int index) {
    int k = 0;
    for (const auto& n : nodes) if (n.kind == Kind::Rtl && n.sdrNode) { if (k == index) return &n; ++k; }
    return nullptr;
}

// ★★ The SAME code vibeserver.exe's SWITCH runs (win_bind.cpp) — the probe only adds the report around it.
int cmdBind(const Opts& o) {
    say("== bind: Windows' own WinUSB onto RTL-SDR #%d\n", o.index);
    const auto nodes = scan();
    const UsbNode* n = pickRtl(nodes, o.index);
    if (!n) { say("!! no RTL-SDR #%d plugged in (see `list`)\n", o.index); return 1; }
    say("target: %s\n  now: %s [service \"%s\", %s]\n", n->instanceId.c_str(), driverName(n->driver), n->service.c_str(), n->infPath.c_str());
    if (n->driver == Driver::WinUsb || n->driver == Driver::LibusbK || n->driver == Driver::Libusb0) {
        say("Already on a libusb driver — nothing to do. Ready.\n"); return 0;
    }
    // ★ Remember what was there, so `giveback` can say what should come back.
    if (FILE* f = _wfopen(stateFile().c_str(), L"w")) {
        std::fprintf(f, "%s\n%s\n%s\n%s\n", n->instanceId.c_str(), n->service.c_str(), n->infPath.c_str(), n->driverDesc.c_str());
        std::fclose(f);
    }
    const BindResult r = bindWinUsb(n->instanceId, [](const std::string& l) { say("%s\n", l.c_str()); });
    sayLibusbView(false);
    say("RESULT: %s%s\n", r.ok ? ("bound by way " + r.method + " (check the libusb line above says open OK, then run `stream`)").c_str()
                              : ("FAIL — " + r.error).c_str(), r.reboot ? " — RESTART WINDOWS to finish" : "");
    if (!r.ok) say("        Record this. Device Manager's manual path (test sheet, DIAGNOSTIC only) tells us whether\n"
                   "        Windows offers WinUsb Device for this dongle at all.\n");
    return r.ok ? 0 : 1;
}

int cmdGiveback(const Opts& o) {
    say("== giveback: the dongle back to Windows' own choice of driver\n");
    std::string inst, origSvc, origInf, origDesc;
    if (FILE* f = _wfopen(stateFile().c_str(), L"r")) {
        char b[1024];
        auto line = [&](std::string& out) { if (fgets(b, sizeof b, f)) { out = b; while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back(); } };
        line(inst); line(origSvc); line(origInf); line(origDesc);
        std::fclose(f);
    }
    if (inst.empty()) {
        const auto nodes = scan();
        if (const UsbNode* n = pickRtl(nodes, o.index)) inst = n->instanceId;
    }
    if (inst.empty()) { say("!! nothing recorded and no RTL-SDR #%d plugged in\n", o.index); return 1; }
    say("device: %s\n  before bind it had: service \"%s\" %s %s\n", inst.c_str(), origSvc.c_str(), origInf.c_str(), origDesc.c_str());
    const BindResult r = giveBack(inst, [](const std::string& l) { say("%s\n", l.c_str()); });
    sayNodes(scan(), usbdk());
    if (r.ok) DeleteFileW(stateFile().c_str());
    say("RESULT: %s\n", r.ok ? "given back (compare the driver above with what it had before the bind)" : ("FAIL — " + r.error).c_str());
    return r.ok ? 0 : 1;
}

/** bind/giveback need administrator rights: ask for them ONCE (the UAC prompt), run the same command
 *  elevated, and show its output here. */
int elevated(int argc, char** argv) {
    wchar_t self[MAX_PATH * 4]; GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring relay = std::wstring(tmp) + L"vibe-usbprobe-elevated.txt";
    DeleteFileW(relay.c_str());
    std::wstring args;
    for (int i = 1; i < argc; ++i) args += L"\"" + widen(argv[i]) + L"\" ";
    args += L"--relay \"" + relay + L"\"";
    say("This step changes a driver, so Windows will ask for permission (UAC) …\n");
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = L"runas"; sei.lpFile = self;
    sei.lpParameters = args.c_str(); sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) { say("!! not elevated (permission refused?) — error %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
    if (FILE* f = _wfopen(relay.c_str(), L"r")) {
        char b[4096];
        while (fgets(b, sizeof b, f)) { std::fputs(b, stdout); if (g_log) std::fputs(b, g_log); }
        std::fclose(f);
        DeleteFileW(relay.c_str());
    }
    return (int)code;
}

void usage() {
    say("vibe-usbprobe — VibeServer for Windows, Milestone 1 driver test (docs/WINDOWS-M1-TEST.md)\n\n"
        "  list                     every radio Windows sees, its driver, and what VibeServer would do\n"
        "  usbdk                    is UsbDk installed, and can libusb use it?\n"
        "  report                   list + usbdk + what libusb can open, both ways (start here)\n"
        "  stream [options]         open RTL-SDR, count lost data in the chip's counter mode, then real samples\n"
        "  cycle  [options]         open / stream 5 s / close, N times — the hand-back and re-borrow test\n"
        "  bind   [--index N]       put Windows' own WinUSB on that one dongle (asks for admin once)\n"
        "  giveback [--index N]     undo `bind`: Windows picks the dongle's driver again (asks for admin)\n\n"
        "  options: --index N (default 0) · --usbdk (borrow through UsbDk) · --rate HZ (2400000)\n"
        "           --seconds S (60) · --freq HZ (100000000) · --hf (also 7.1 MHz, V4) · --bias-t · --cycles N (10)\n\n"
        "Everything is also appended to vibe-usbprobe.log beside this program.\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string cmd = argc > 1 ? argv[1] : "";
    Opts o;
    std::string relay;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--index") o.index = atoi(next());
        else if (a == "--usbdk") o.usbdk = true;
        else if (a == "--rate") o.rate = atof(next());
        else if (a == "--seconds") o.seconds = atof(next());
        else if (a == "--freq") o.freq = atof(next());
        else if (a == "--hf") o.hf = true;
        else if (a == "--bias-t") o.biasT = true;
        else if (a == "--cycles") o.cycles = atoi(next());
        else if (a == "--relay") relay = next();
    }
    if (!relay.empty()) g_relay = _wfopen(widen(relay).c_str(), L"w");
    else g_log = _wfopen((exeDir() + L"\\vibe-usbprobe.log").c_str(), L"a");
    {
        const std::time_t t = std::time(nullptr);
        char ts[64]; std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::string all; for (int i = 1; i < argc; ++i) { all += ' '; all += argv[i]; }
        say("\n──── vibe-usbprobe %s · %s ·%s\n", VIBESERVER_VERSION_STR, ts, all.c_str());
    }
    int rc = 0;
    if (cmd == "list" || cmd == "report" || cmd == "usbdk") {
        sayMachine();
        const UsbDkState dk = usbdk();
        sayUsbDk(dk);
        if (cmd != "usbdk") sayNodes(scan(), dk);
        if (cmd != "list") {
            say("\n");
            sayLibusbView(false);
            if (dk.usable()) sayLibusbView(true);
            else say("libusb (UsbDk): not tried — UsbDk is not installed where libusb looks\n");
            std::string why;
            const Backend b = chooseBackend(scan(), dk, why);
            say("\nVibeServer would use: %s — %s\n", b == Backend::UsbDk ? "UsbDk (borrow)" : "WinUSB", why.c_str());
        }
    } else if (cmd == "stream") {
        rc = cmdStream(o);
    } else if (cmd == "cycle") {
        rc = cmdCycle(o);
    } else if (cmd == "bind" || cmd == "giveback") {
        if (!isAdmin() && relay.empty()) rc = elevated(argc, argv);
        else rc = cmd == "bind" ? cmdBind(o) : cmdGiveback(o);
    } else {
        usage();
        rc = cmd.empty() || cmd == "help" || cmd == "--help" ? 0 : 2;
    }
    if (g_log) std::fclose(g_log);
    if (g_relay) std::fclose(g_relay);
    return rc;
}
#endif
