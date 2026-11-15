// win_autostart.cpp — see win_autostart.h. Windows only.
#ifdef _WIN32
#include "win_autostart.h"
#include "win_usb.h"   // narrow / widen
#include "proc.h"

#include <aclapi.h>
#include <sddl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>

namespace vibewin {

// ── pure ─────────────────────────────────────────────────────────────────────────────────────────────────────────
std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break; case '\'': o += "&apos;"; break; default: o += c;
        }
    }
    return o;
}

std::string taskXml(const std::string& exe) {
    const size_t cut = exe.find_last_of("\\/");
    const std::string dir = cut == std::string::npos ? std::string(".") : exe.substr(0, cut);
    // ★★ S-1-5-19 = NT AUTHORITY\LOCAL SERVICE — Windows' own restricted service account (no password, no profile
    //    of a person, network identity anonymous). LeastPrivilege: never elevated.
    // ★ The task's own security descriptor lets any signed-in account READ it (so the un-elevated setup page can
    //   say "starts at boot"); only administrators and SYSTEM can change or delete it.
    // ★ RestartOnFailure is a second net under the keeper (--autostart boot), which restarts the server itself.
    return
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        "<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo>\n"
        "    <Author>VibeServer</Author>\n"
        "    <Description>Starts VibeServer when Windows starts. Turned on and off from VibeServer's setup page.</Description>\n"
        "    <SecurityDescriptor>D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GRGX;;;AU)</SecurityDescriptor>\n"
        "  </RegistrationInfo>\n"
        "  <Triggers>\n"
        "    <BootTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></BootTrigger>\n"
        "  </Triggers>\n"
        "  <Principals>\n"
        "    <Principal id=\"Author\"><UserId>S-1-5-19</UserId><LogonType>ServiceAccount</LogonType>"
        "<RunLevel>LeastPrivilege</RunLevel></Principal>\n"
        "  </Principals>\n"
        "  <Settings>\n"
        "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        "    <AllowHardTerminate>true</AllowHardTerminate>\n"
        "    <StartWhenAvailable>true</StartWhenAvailable>\n"
        "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
        "    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\n"
        "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
        "    <Enabled>true</Enabled>\n"
        "    <Hidden>false</Hidden>\n"
        "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
        "    <WakeToRun>false</WakeToRun>\n"
        "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
        "    <Priority>4</Priority>\n"
        "    <RestartOnFailure><Interval>PT1M</Interval><Count>999</Count></RestartOnFailure>\n"
        "  </Settings>\n"
        "  <Actions Context=\"Author\">\n"
        "    <Exec>\n"
        "      <Command>\"" + xmlEsc(exe) + "\"</Command>\n"
        "      <Arguments>--autostart boot</Arguments>\n"
        "      <WorkingDirectory>" + xmlEsc(dir) + "</WorkingDirectory>\n"
        "    </Exec>\n"
        "  </Actions>\n"
        "</Task>\n";
}

// ★ /f replaces an existing task of that name: create is idempotent. Delete is made idempotent by the caller (query
//   first: "not there" is success).
std::vector<std::string> schtasksCreateArgs(const std::string& xmlPath) { return { "/Create", "/TN", kTaskName, "/XML", xmlPath, "/F" }; }
std::vector<std::string> schtasksDeleteArgs() { return { "/Delete", "/TN", kTaskName, "/F" }; }
std::vector<std::string> schtasksQueryArgs()  { return { "/Query", "/TN", kTaskName, "/FO", "LIST" }; }

std::string runValue(const std::string& exe) { return "\"" + exe + "\" --autostart signin"; }

bool exePathServiceReadable(const std::string& exe, const std::string& usersDir, std::string& why) {
    auto lower = [](std::string s) { for (char& c : s) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; } return s; };
    std::string e = lower(exe), u = lower(usersDir);
    while (!u.empty() && u.back() == '\\') u.pop_back();
    if (!u.empty() && e.size() > u.size() && e.compare(0, u.size(), u) == 0 && e[u.size()] == '\\') {
        // ★ C:\Users\Public is shared on purpose; every other folder under C:\Users belongs to one person.
        const std::string rest = e.substr(u.size() + 1);
        if (rest.rfind("public\\", 0) != 0) {
            why = "VibeServer is in a personal folder (" + exe.substr(0, exe.find_last_of("\\/")) + "), which Windows' "
                  "background account cannot read. Move the VibeServer folder somewhere like C:\\VibeServer and run it "
                  "from there to start at boot — or use \"start when you sign in\".";
            return false;
        }
    }
    return true;
}

// ── Windows ──────────────────────────────────────────────────────────────────────────────────────────────────────
std::string selfExe() {
    wchar_t w[32768];
    const DWORD n = GetModuleFileNameW(nullptr, w, 32768);
    return n ? narrow(std::wstring(w, n)) : std::string();
}

namespace {

/** ★ The task is there only if schtasks says so AND names it: an exit code alone is not enough (Wine's schtasks, the
 *  test rig, answers 0 for a task that does not exist). Windows prints "TaskName: \VibeServer" in the LIST form. */
bool taskPresent();

std::string sysDir() { wchar_t s[MAX_PATH] = L"C:\\Windows\\System32"; GetSystemDirectoryW(s, MAX_PATH); return narrow(s); }
std::string schtasksExe() { return sysDir() + "\\schtasks.exe"; }

std::string dataDir() {
    char pd[MAX_PATH] = "C:\\ProgramData";
    if (const char* p = std::getenv("ProgramData"); p && *p) snprintf(pd, sizeof pd, "%s", p);
    return std::string(pd) + "\\VibeServer";
}

int schtasks(std::vector<std::string> args, std::string* out = nullptr) {
    args.insert(args.begin(), schtasksExe());
    return vibeproc::run(args, out);
}

bool taskPresent() {
    std::string out;
    if (schtasks(schtasksQueryArgs(), &out) != 0) return false;
    return out.find(std::string("\\") + kTaskName) != std::string::npos;
}

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/** Grant (or revoke) modify on our data folder, inherited by everything in it — existing files included. */
bool folderGrant(const std::string& sid, bool grant, std::string& err) {
    PSID psid = nullptr;
    if (!ConvertStringSidToSidW(widen(sid).c_str(), &psid)) { err = "bad SID " + sid; return false; }
    const std::wstring dir = widen(dataDir());
    CreateDirectoryW(dir.c_str(), nullptr);
    PACL oldAcl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD e = GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl, nullptr, &sd);
    if (e != ERROR_SUCCESS) { LocalFree(psid); err = "could not read the folder's permissions (" + std::to_string(e) + ")"; return false; }
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | 0x00010000L;   // "Modify" (0x10000 = DELETE, which the compat layer #undefs)
    ea.grfAccessMode = grant ? GRANT_ACCESS : REVOKE_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = (LPWSTR)psid;
    PACL newAcl = nullptr;
    e = SetEntriesInAclW(1, &ea, oldAcl, &newAcl);
    if (e == ERROR_SUCCESS)
        e = SetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
    if (newAcl) LocalFree(newAcl);
    if (sd) LocalFree(sd);
    LocalFree(psid);
    if (e != ERROR_SUCCESS) { err = "could not change the folder's permissions (" + std::to_string(e) + ")"; return false; }
    return true;
}

}  // namespace

std::string currentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    std::string out;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<char> b(need ? need : 1);
    if (need && GetTokenInformation(tok, TokenUser, b.data(), need, &need)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid, &s)) { out = narrow(s); LocalFree(s); }
    }
    CloseHandle(tok);
    return out;
}

AutostartState queryAutostart() {
    AutostartState st;
    st.boot = taskPresent();
    wchar_t v[4096] = {0}; DWORD sz = sizeof v - sizeof(wchar_t);
    st.signin = RegGetValueW(HKEY_CURRENT_USER, kRunKey, widen(kRunValue).c_str(), RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS;
    DWORD session = 1;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    st.background = session == 0;
    char users[MAX_PATH] = "C:\\Users";
    if (const char* p = std::getenv("SystemDrive"); p && *p) snprintf(users, sizeof users, "%s\\Users", p);
    st.exeOk = exePathServiceReadable(selfExe(), users, st.exeWhy);
    return st;
}

bool setSignIn(bool on, std::string& err) {
    HKEY k = nullptr;
    LONG e = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) { err = "could not open the sign-in list (" + std::to_string(e) + ")"; return false; }
    if (on) {
        const std::wstring v = widen(runValue(selfExe()));
        e = RegSetValueExW(k, widen(kRunValue).c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        e = RegDeleteValueW(k, widen(kRunValue).c_str());
        if (e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS;   // ★ already off: idempotent
    }
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) { err = "could not change the sign-in list (" + std::to_string(e) + ")"; return false; }
    return true;
}

bool helperSetBoot(bool on, const std::string& grantSid, const AutoLogFn& log, std::string& err) {
    if (on) {
        const std::string exe = selfExe();
        char users[MAX_PATH] = "C:\\Users";
        if (const char* p = std::getenv("SystemDrive"); p && *p) snprintf(users, sizeof users, "%s\\Users", p);
        if (!exePathServiceReadable(exe, users, err)) return false;
        // ★ The folder first: a task that starts before its data folder is writable fails at the first config save.
        if (!folderGrant("S-1-5-19", true, err)) return false;
        log("  " + dataDir() + ": LOCAL SERVICE may now read and write VibeServer's own settings folder");
        if (!grantSid.empty()) {
            if (!folderGrant(grantSid, true, err)) return false;
            log("  … and so may your own account, so running VibeServer by hand still works");
        }
        // ★ schtasks wants the XML as UTF-16 (it says so in the declaration). Written to our own folder, then deleted.
        const std::string xmlPath = dataDir() + "\\vibeserver-task.xml";
        const std::wstring xml = widen(taskXml(exe));
        FILE* f = _wfopen(widen(xmlPath).c_str(), L"wb");
        if (!f) { err = "could not write " + xmlPath; return false; }
        const unsigned char bom[2] = { 0xFF, 0xFE };
        fwrite(bom, 1, 2, f);
        fwrite(xml.data(), sizeof(wchar_t), xml.size(), f);
        fclose(f);
        std::string out;
        const int rc = schtasks(schtasksCreateArgs(xmlPath), &out);
        DeleteFileW(widen(xmlPath).c_str());
        if (rc != 0) { err = "Windows' Task Scheduler refused the task (schtasks " + std::to_string(rc) + ")"; return false; }
        log("  Scheduled Task \"" + std::string(kTaskName) + "\": at system startup, as LOCAL SERVICE, restarted if it stops");
        return true;
    }
    if (taskPresent()) {
        const int rc = schtasks(schtasksDeleteArgs());
        if (rc != 0) { err = "Windows' Task Scheduler would not remove the task (schtasks " + std::to_string(rc) + ")"; return false; }
        log("  Scheduled Task \"" + std::string(kTaskName) + "\" removed");
    } else log("  no Scheduled Task to remove — already off");
    std::string e2;
    if (folderGrant("S-1-5-19", false, e2)) log("  LOCAL SERVICE's access to " + dataDir() + " withdrawn");
    else log("  (could not withdraw LOCAL SERVICE's folder access: " + e2 + ")");
    return true;
}

// ── the keeper ───────────────────────────────────────────────────────────────────────────────────────────────────
int keeperMain(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[2] : "signin";
    // ★ Started from the sign-in list a console program gets a window; the keeper needs none (a flash, then gone).
    FreeConsole();
    const std::string dir = dataDir();
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    const std::string logPath = dir + "\\vibeserver.log";
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    const std::wstring exe = widen(selfExe());
    int backoff = 5;
    for (;;) {
        // ★ One log, kept small: past 5 MB the old one becomes vibeserver.log.old.
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(widen(logPath).c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 5u * 1024 * 1024)
            MoveFileExW(widen(logPath).c_str(), widen(logPath + ".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
        HANDLE log = CreateFileW(widen(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            char line[256]; const time_t t = time(nullptr); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
            const int n = snprintf(line, sizeof line, "\r\n== %s keeper (%s): starting VibeServer\r\n", ts, mode.c_str());
            DWORD w = 0; WriteFile(log, line, (DWORD)n, &w, nullptr);
        }
        std::wstring cmd = L"\"" + exe + L"\" --serve";
        STARTUPINFOW si{}; si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = nul; si.hStdOutput = log; si.hStdError = log;
        PROCESS_INFORMATION pi{};
        const auto t0 = std::chrono::steady_clock::now();
        if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
                           nullptr, nullptr, &si, &pi)) {
            if (job) AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            // ★★ WAIT FOR THE JOB TO EMPTY, NOT FOR THIS PID: a settings save makes the server start a fresh copy of
            //    itself and exit (vibeproc::restartSelf) — that copy is in this job too, so the keeper keeps watching.
            for (;;) {
                if (!job) { WaitForSingleObject(pi.hProcess, INFINITE); break; }
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai{};
                if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof ai, nullptr)) break;
                if (ai.ActiveProcesses == 0) break;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            CloseHandle(pi.hProcess);
        }
        const long lived = (long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        backoff = lived > 300 ? 5 : (backoff * 2 > 300 ? 300 : backoff * 2);
        if (log != INVALID_HANDLE_VALUE) {
            char line[160];
            const int n = snprintf(line, sizeof line, "== keeper: VibeServer stopped after %ld s; starting it again in %d s\r\n", lived, backoff);
            DWORD w = 0; WriteFile(log, line, (DWORD)n, &w, nullptr);
            CloseHandle(log);
        }
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }
}

}  // namespace vibewin
#endif
