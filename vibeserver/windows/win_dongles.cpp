// win_dongles.cpp — see win_dongles.h. Windows only.
#ifdef _WIN32
#include "win_dongles.h"
#include "win_switch.h"
#include "win_bind.h"
#include "win_autostart.h"

#include <cfgmgr32.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>

namespace vibewin {
namespace {

// ── state, all under g_mtx ───────────────────────────────────────────────────────────────────────────────────────
std::mutex g_mtx;
std::condition_variable g_cv;
bool g_rescan = false;
DongleHooks g_hooks;
std::vector<Dongle> g_dongles;
std::set<std::string> g_announced;   // offerable ids already printed to the console (this run only)
std::set<std::string> g_notNow;      // NOT NOW, this run only — ★ nothing is persisted
struct Job {
    bool running = false, done = false, ok = false, declined = false, reboot = false;
    std::string op, name, message;
    std::vector<std::string> log;
    unsigned seq = 0;
} g_job;

std::string lowerHex(size_t n) {
    std::string s; unsigned char b[16] = {0};
    BCryptGenRandom(nullptr, b, (ULONG)sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* h = "0123456789abcdef";
    for (size_t i = 0; i < n && i < sizeof b * 2; ++i) s += h[(b[i / 2] >> ((i & 1) * 4)) & 15];
    return s;
}

std::string readFile(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n); std::fclose(f); }
    return out;
}

/** Rescan and announce new offers (console). Called on the watch thread. */
void rescan() {
    const auto ds = rtlDongles(serverNodes());
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        std::set<std::string> present;
        for (const auto& d : ds) {
            present.insert(d.instanceId);
            if (offerable(d.state) && !g_announced.count(d.instanceId)) {
                g_announced.insert(d.instanceId);
                lines.push_back(consoleOffer(d, g_hooks.port));
            }
        }
        // ★ Unplugged: forget it, so a stick plugged back in is offered again (and NOT NOW is forgotten with it).
        for (auto it = g_announced.begin(); it != g_announced.end();) it = present.count(*it) ? std::next(it) : g_announced.erase(it);
        for (auto it = g_notNow.begin(); it != g_notNow.end();) it = present.count(*it) ? std::next(it) : g_notNow.erase(it);
        g_dongles = ds;
    }
    for (const auto& l : lines) std::printf("%s\n", l.c_str());
}

DWORD CALLBACK onDeviceChange(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_rescan = true;
        g_cv.notify_all();
    }
    return ERROR_SUCCESS;
}

void setJob(const std::function<void(Job&)>& f) { std::lock_guard<std::mutex> lk(g_mtx); f(g_job); }
void jobLog(const std::string& l) { setJob([&](Job& j) { if (j.log.size() < 400) j.log.push_back(l); }); }

/** ★★★ THE ONE PROMPT. Run this same .exe elevated with `a`, read its report from a pipe only we created, and wait.
 *  Returns the helper's exit code; `declined` when the owner said No to Windows' prompt. */
int runElevated(HelperArgs a, bool& declined, bool& reboot, std::string& resultMsg) {
    declined = false; reboot = false;
    a.replyPipe = "\\\\.\\pipe\\vibeserver-helper-" + std::to_string(GetCurrentProcessId()) + "-" + lowerHex(16);
    const std::string params = helperCommandLine(a);
    if (params.empty()) { resultMsg = "internal: the helper's arguments were refused"; return 2; }
    // ★ Inbound only, one instance, local clients only, and FIRST_PIPE_INSTANCE: if anyone already made a pipe of
    //   this (random) name we refuse rather than read theirs.
    HANDLE pipe = CreateNamedPipeW(widen(a.replyPipe).c_str(),
                                   PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { resultMsg = "could not open the reply pipe (" + std::to_string(GetLastError()) + ")"; return 2; }
    const std::wstring self = widen(selfExe()), wparams = widen(params);
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas"; sei.lpFile = self.c_str(); sei.lpParameters = wparams.c_str(); sei.nShow = SW_HIDE;
    jobLog("Windows is asking for permission on this PC's screen …");
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        CloseHandle(pipe);
        if (e == ERROR_CANCELLED) { declined = true; resultMsg = "Windows' permission prompt was declined — nothing was changed."; return 1223; }
        resultMsg = "Windows could not start the permission prompt (error " + std::to_string(e) + ")";
        return 2;
    }
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe, &ov) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected && GetLastError() == ERROR_IO_PENDING) {
        HANDLE hs[2] = { ov.hEvent, sei.hProcess };
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, 120000);
        connected = w == WAIT_OBJECT_0;
        if (!connected) CancelIo(pipe);
    }
    std::string buf;
    if (connected) {
        char b[4096];
        for (;;) {
            DWORD n = 0; ResetEvent(ov.hEvent);
            if (!ReadFile(pipe, b, sizeof b, &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) break;
                if (!GetOverlappedResult(pipe, &ov, &n, TRUE)) break;
            }
            if (n == 0) break;
            buf.append(b, n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("@@RESULT ", 0) == 0) {
                    // @@RESULT ok|fail reboot=0|1 <message>
                    reboot = line.find(" reboot=1") != std::string::npos;
                    const size_t m = line.find(' ', line.find("reboot=") );
                    resultMsg = m == std::string::npos ? std::string() : line.substr(m + 1);
                } else jobLog(line);
            }
        }
    }
    CloseHandle(ov.hEvent);
    CloseHandle(pipe);
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (!connected && resultMsg.empty()) resultMsg = "the permission helper did not report back";
    return (int)code;
}

const Dongle* findDongle(const std::string& id) {
    for (const auto& d : g_dongles) if (!_stricmp(d.instanceId.c_str(), id.c_str())) return &d;
    return nullptr;
}

bool startJob(const std::string& op, const std::string& name, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_job.running) { err = "VibeServer is already waiting on Windows for \"" + g_job.name + "\""; return false; }
    g_job = Job{}; g_job.running = true; g_job.op = op; g_job.name = name;
    static unsigned seq = 0; g_job.seq = ++seq;
    return true;
}

void finishJob(bool ok, bool declined, bool reboot, const std::string& msg) {
    setJob([&](Job& j) { j.running = false; j.done = true; j.ok = ok; j.declined = declined; j.reboot = reboot; j.message = msg; });
    std::printf("VibeServer: %s\n", msg.c_str());
}

void comInit() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); }

bool background() { DWORD s = 1; ProcessIdToSessionId(GetCurrentProcessId(), &s); return s == 0; }
const char* kBackgroundWhy =
    "VibeServer is running in the background (started at boot), where Windows cannot show its permission prompt. "
    "Sign in on the PC, stop the background copy, and run VibeServer from its folder to change this.";

void doSwitch(Dongle d) {
    comInit();
    std::vector<std::string> before;
    if (g_hooks.rtlSerials) before = g_hooks.rtlSerials();
    HelperArgs a; a.op = HelperOp::Bind; a.instanceId = d.instanceId;
    bool declined = false, reboot = false; std::string msg;
    const int rc = runElevated(a, declined, reboot, msg);
    if (rc != 0) { finishJob(false, declined, false, declined ? msg : "Not switched: " + msg); return; }
    if (reboot) {
        finishJob(true, false, true, describe(d) + " is switched to SDR, but Windows says it needs a RESTART of the PC "
                                     "before it can be used. Restart Windows, then start VibeServer again.");
        return;
    }
    // ★★ OPEN IT NOW: wait (≤10 s) for librtlsdr to see the dongle it could not open before, then add it.
    std::vector<std::string> fresh;
    for (int i = 0; i < 10 && fresh.empty() && g_hooks.rtlSerials; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (const auto& s : g_hooks.rtlSerials())
            if (std::find(before.begin(), before.end(), s) == before.end()) fresh.push_back(s);
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx); g_rescan = true; g_cv.notify_all();
    }
    if (fresh.empty()) {
        finishJob(true, false, false, describe(d) + " is switched to SDR, but VibeServer cannot open it yet — unplug it, "
                                      "plug it back in, and press Check again.");
        return;
    }
    const std::string added = g_hooks.afterSwitch ? g_hooks.afterSwitch(fresh) : std::string();
    finishJob(true, false, false, describe(d) + " is now an SDR. " + added);
}

void doGiveBack(Dongle d) {
    comInit();
    if (g_hooks.releaseForGiveBack) g_hooks.releaseForGiveBack(d.serial);
    HelperArgs a; a.op = HelperOp::GiveBack; a.instanceId = d.instanceId;
    bool declined = false, reboot = false; std::string msg;
    const int rc = runElevated(a, declined, reboot, msg);
    { std::lock_guard<std::mutex> lk(g_mtx); g_rescan = true; g_cv.notify_all(); }
    if (rc != 0) {
        // ★ Nothing changed in Windows, so nothing changes here either: the radio goes back on the air.
        if (g_hooks.undoRelease) g_hooks.undoRelease(d.serial);
        finishJob(false, declined, false, declined ? msg : "Not given back: " + msg);
        return;
    }
    finishJob(true, false, reboot, describe(d) + " is back on Windows' own driver" +
              (reboot ? " once the PC restarts (Windows asked for a restart)." : ". Its settings are kept, paused, in case you switch it again."));
}

void doAutostart(std::string mode) {
    comInit();
    const AutostartState st = queryAutostart();
    bool declined = false, reboot = false; std::string msg; int rc = 0;
    if (mode == "boot") {
        HelperArgs a; a.op = HelperOp::AutostartBoot; a.grantSid = currentUserSid();
        if (!validSidString(a.grantSid)) a.grantSid.clear();
        rc = runElevated(a, declined, reboot, msg);
        if (rc == 0) { std::string e; setSignIn(false, e); finishJob(true, false, false, "VibeServer now starts when Windows starts — nobody needs to sign in."); }
        else finishJob(false, declined, false, declined ? "Windows' permission prompt was declined, so VibeServer will not start at boot."
                                                         : "Could not set VibeServer to start at boot: " + msg);
        return;
    }
    if (st.boot) {   // signin or off: the boot task has to go first, and only an administrator can remove it
        HelperArgs a; a.op = HelperOp::AutostartOff;
        rc = runElevated(a, declined, reboot, msg);
        if (rc != 0) { finishJob(false, declined, false, declined ? "Windows' permission prompt was declined — VibeServer still starts at boot."
                                                                   : "Could not remove the start-at-boot task: " + msg); return; }
    }
    std::string e;
    if (!setSignIn(mode == "signin", e)) { finishJob(false, false, false, e); return; }
    finishJob(true, false, false, mode == "signin" ? "VibeServer now starts when you sign in to Windows."
                                                   : "VibeServer no longer starts by itself.");
}

std::string jobJson() {
    std::string j = "{\"running\":" + std::string(g_job.running ? "true" : "false") + ",\"done\":" + (g_job.done ? "true" : "false")
                  + ",\"ok\":" + (g_job.ok ? "true" : "false") + ",\"declined\":" + (g_job.declined ? "true" : "false")
                  + ",\"reboot\":" + (g_job.reboot ? "true" : "false") + ",\"seq\":" + std::to_string(g_job.seq)
                  + ",\"op\":\"" + jsonEsc(g_job.op) + "\",\"name\":\"" + jsonEsc(g_job.name) + "\",\"message\":\"" + jsonEsc(g_job.message)
                  + "\",\"log\":[";
    for (size_t i = 0; i < g_job.log.size(); ++i) j += (i ? ",\"" : "\"") + jsonEsc(g_job.log[i]) + "\"";
    return j + "]}";
}

}  // namespace

std::vector<UsbNode> serverNodes() {
    if (const char* f = std::getenv("VIBE_WIN_FAKE_USB"); f && *f) {
        std::vector<UsbNode> out; std::string err;
        if (!parseFakeDevices(readFile(f), out, err)) std::fprintf(stderr, "VibeServer: VIBE_WIN_FAKE_USB: %s\n", err.c_str());
        return out;
    }
    return scan();
}

void startDongleWatch(const DongleHooks& hooks) {
    { std::lock_guard<std::mutex> lk(g_mtx); g_hooks = hooks; }
    rescan();
    // ★ Arrivals and removals of any USB device; the scan that follows is SetupAPI only — it never opens a device, so
    //   it cannot disturb a radio that is streaming.
    static const GUID kUsbDeviceIface = { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };
    CM_NOTIFY_FILTER f{}; f.cbSize = sizeof f;
    f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    f.u.DeviceInterface.ClassGuid = kUsbDeviceIface;
    HCMNOTIFICATION h = nullptr;
    // ★ Looked up, not imported: Wine (the test rig) exports it as a stub that ABORTS the process when called, so
    //   under Wine the watch falls back to re-reading the list every few seconds. On Windows 8+ it is always there.
    typedef CONFIGRET (WINAPI *Reg_t)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    auto reg = wine ? nullptr : (Reg_t)(void*)GetProcAddress(LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32),
                                                             "CM_Register_Notification");
    const bool notified = reg && reg(&f, nullptr, onDeviceChange, &h) == CR_SUCCESS;
    const bool fake = std::getenv("VIBE_WIN_FAKE_USB") && *std::getenv("VIBE_WIN_FAKE_USB");
    std::thread([notified, fake] {
        for (;;) {
            std::unique_lock<std::mutex> lk(g_mtx);
            // ★ With notifications: wait for one (a 60 s net under them). Without (or with the test file): every 3 s —
            //   a SetupAPI read, nothing more.
            g_cv.wait_for(lk, std::chrono::seconds(notified && !fake ? 60 : 3), [] { return g_rescan; });
            g_rescan = false;
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(700));   // ★ let Windows finish binding the arrival
            rescan();
        }
    }).detach();
}

bool handleWindowsHttp(const std::string& method, const std::string& path, const std::string& body, bool adminOk,
                       int& code, std::string& status, std::string& out) {
    if (path.rfind("/vibeserver/windows/", 0) != 0) return false;
    const std::string what = path.substr(20);
    code = 200; status = "OK";
    auto fail = [&](int c, const std::string& why) { code = c; status = c == 401 ? "Unauthorized" : c == 404 ? "Not Found" : "Conflict";
                                                      out = "{\"error\":\"" + jsonEsc(why) + "\"}"; return true; };
    if (what == "hello") { out = "{\"windows\":true}"; return true; }
    if (!adminOk) return fail(401, "sign in as admin first");
    if (what == "state" && method == "GET") {
        const AutostartState st = queryAutostart();
        std::lock_guard<std::mutex> lk(g_mtx);
        std::string nn = "[";
        for (const auto& id : g_notNow) nn += (nn.size() > 1 ? ",\"" : "\"") + jsonEsc(id) + "\"";
        out = "{\"windows\":true,\"dongles\":" + donglesJson(g_dongles) + ",\"notNow\":" + nn + "]"
            + ",\"job\":" + jobJson()
            + ",\"autostart\":{\"boot\":" + (st.boot ? "true" : "false") + ",\"signin\":" + (st.signin ? "true" : "false")
            + ",\"background\":" + (st.background ? "true" : "false") + ",\"exeOk\":" + (st.exeOk ? "true" : "false")
            + ",\"exeWhy\":\"" + jsonEsc(st.exeWhy) + "\"}}";
        return true;
    }
    if (method != "POST") return fail(404, "no such route");
    if (what == "notnow") {
        const std::string id = jsonField(body, "id");
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!findDongle(id)) return fail(409, "that dongle is not plugged in");
        g_notNow.insert(id);
        out = "{\"ok\":true}";
        return true;
    }
    if (what == "switch" || what == "giveback") {
        if (background()) return fail(409, kBackgroundWhy);
        const std::string id = jsonField(body, "id");
        Dongle d;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            const Dongle* p = findDongle(id);
            if (!p) return fail(409, "that dongle is not plugged in any more");
            d = *p;
        }
        // ★★ The same rules the helper enforces, so the page is refused here first with the same words.
        if (what == "switch" && !offerable(d.state)) return fail(409, d.state == DongleState::Usable ? "that dongle is already usable" : d.why);
        if (what == "giveback" && !d.switchedByUs) return fail(409, "VibeServer did not switch that dongle, so it will not undo it");
        std::string err;
        if (!startJob(what, describe(d), err)) return fail(409, err);
        if (what == "switch") std::thread(doSwitch, d).detach(); else std::thread(doGiveBack, d).detach();
        code = 202; status = "Accepted"; out = "{\"started\":true}";
        return true;
    }
    if (what == "autostart") {
        const std::string mode = jsonField(body, "mode");
        if (mode != "boot" && mode != "signin" && mode != "off") return fail(409, "mode is boot, signin or off");
        const AutostartState st = queryAutostart();
        if (st.background && (mode == "boot" || st.boot)) return fail(409, kBackgroundWhy);
        if (mode == "boot" && !st.exeOk) return fail(409, st.exeWhy);
        // ★ Idempotent: asking for what is already so changes nothing and asks Windows for nothing.
        if ((mode == "boot" && st.boot && !st.signin) || (mode == "signin" && st.signin && !st.boot) || (mode == "off" && !st.boot && !st.signin)) {
            out = "{\"ok\":true,\"unchanged\":true}"; return true;
        }
        if (!st.boot && mode != "boot") {   // no prompt needed: do it now
            std::string e;
            if (!setSignIn(mode == "signin", e)) return fail(409, e);
            out = "{\"ok\":true}"; return true;
        }
        std::string err;
        if (!startJob("autostart", mode == "boot" ? "start at boot" : mode == "signin" ? "start at sign-in" : "do not start by itself", err))
            return fail(409, err);
        std::thread(doAutostart, mode).detach();
        code = 202; status = "Accepted"; out = "{\"started\":true}";
        return true;
    }
    return fail(404, "no such route");
}

// ── the elevated helper ──────────────────────────────────────────────────────────────────────────────────────────
namespace {
HANDLE g_reply = INVALID_HANDLE_VALUE;
FILE* g_hlog = nullptr;
void reply(const std::string& line) {
    const std::string l = line + "\n";
    if (g_reply != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(g_reply, l.data(), (DWORD)l.size(), &w, nullptr); }
    if (g_hlog) { std::fputs(l.c_str(), g_hlog); std::fflush(g_hlog); }
}
bool elevatedNow() {
    HANDLE tok = nullptr; TOKEN_ELEVATION e{}; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}
}  // namespace

int helperMain(int argc, char** argv) {
    const HelperArgs a = parseHelperArgs(argc, argv);
    if (validPipeName(a.replyPipe)) {
        const std::wstring p = widen(a.replyPipe);
        for (int i = 0; i < 50 && g_reply == INVALID_HANDLE_VALUE; ++i) {
            g_reply = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (g_reply == INVALID_HANDLE_VALUE) { WaitNamedPipeW(p.c_str(), 100); }
        }
    }
    // ★ An audit trail beside the settings: what the elevated copy did, every time (no secrets in it).
    {
        char pd[MAX_PATH] = "C:\\ProgramData";
        if (const char* e = std::getenv("ProgramData"); e && *e) snprintf(pd, sizeof pd, "%s", e);
        const std::string dir = std::string(pd) + "\\VibeServer";
        CreateDirectoryW(widen(dir).c_str(), nullptr);
        g_hlog = _wfopen(widen(dir + "\\vibeserver-helper.log").c_str(), L"a");
        char ts[32]; const time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
        if (g_hlog) std::fprintf(g_hlog, "\n== %s helper:", ts);
        for (int i = 1; g_hlog && i < argc; ++i) std::fprintf(g_hlog, " %s", argv[i]);
        if (g_hlog) std::fputs("\n", g_hlog);
    }
    auto done = [&](bool ok, bool reboot, const std::string& msg) {
        reply(std::string("@@RESULT ") + (ok ? "ok" : "fail") + " reboot=" + (reboot ? "1" : "0") + " " + msg);
        if (g_reply != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_reply); CloseHandle(g_reply); }
        if (g_hlog) std::fclose(g_hlog);
        return ok ? 0 : 1;
    };
    if (a.op == HelperOp::None) return done(false, false, "refused: " + a.error);
    if (g_reply == INVALID_HANDLE_VALUE) return done(false, false, "refused: the server that asked is not listening");
    if (!elevatedNow()) return done(false, false, "this step needs Windows' permission (administrator)");
    const LogFn log = [](const std::string& l) { reply(l); };
    std::string why;
    switch (a.op) {
        case HelperOp::Bind: {
            const auto real = scan();   // ★★ never the fake list, never the server's word: Windows', now
            if (!helperMayBind(real, a.instanceId, why)) return done(false, false, "refused: " + why);
            const BindResult r = bindWinUsb(a.instanceId, log);
            return done(r.ok, r.reboot, r.ok ? "switched (" + r.method + ")" : r.error);
        }
        case HelperOp::GiveBack: {
            const auto real = scan();
            if (!helperMayGiveBack(real, a.instanceId, why)) return done(false, false, "refused: " + why);
            const BindResult r = giveBack(a.instanceId, log);
            return done(r.ok, r.reboot, r.ok ? "given back" : r.error);
        }
        case HelperOp::AutostartBoot: case HelperOp::AutostartOff: {
            std::string err;
            const bool ok = helperSetBoot(a.op == HelperOp::AutostartBoot, a.grantSid, log, err);
            return done(ok, false, ok ? "done" : err);
        }
        default: return done(false, false, "refused");
    }
}

}  // namespace vibewin
#endif
