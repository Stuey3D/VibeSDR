// win_autostart.h — "Start VibeServer when Windows starts" (Windows only).
//
// ★★★ STUART (2026-10-26): "we need a button to start on boot." Two ways, and the setup page says which one is on:
//   • STARTS AT BOOT — a per-machine Scheduled Task, trigger "at system startup", running VibeServer as Windows'
//     own restricted LOCAL SERVICE account (not SYSTEM, not the user): no one needs to be signed in. Created and
//     removed by the same elevated helper as the dongle switch — ONE UAC prompt each way.
//       ★ Why not SYSTEM: nothing here needs it. A WinUSB dongle opens for any account (that is how SDR# runs as a
//         plain user); the server listens on a port above 1024; the data folder is granted to LOCAL SERVICE.
//       ★ "Restarted on failure": the task runs `vibeserver.exe --autostart boot`, a small KEEPER that runs the
//         server in a Job Object and starts it again (with a back-off) whenever the job empties — which covers a
//         crash AND a self-restart after a settings save, which Task Scheduler's own RestartOnFailure does not.
//   • STARTS WHEN YOU SIGN IN — if the owner declines the prompt (or the program sits in a personal folder that the
//     background account cannot read): HKCU\…\Run, no elevation at all, `vibeserver.exe --autostart signin`.
//   Turning it off removes whichever is there, cleanly; nothing is left behind (the folder grant is revoked too).
// ★★ Nothing here lowers Windows' security: no service hardening is relaxed, no firewall rule is opened by us (the
//    rule Windows asked the owner for at the first run covers the program wherever it runs), no account is created.
// ★ The pure parts (task XML, schtasks arguments, Run value, the folder check) are unit-tested; the rest is Windows.
#pragma once
#ifdef _WIN32
#include <functional>
#include <string>
#include <vector>

namespace vibewin {

constexpr const char* kTaskName = "VibeServer";
constexpr const char* kRunValue = "VibeServer";

struct AutostartState {
    bool boot = false;        // the Scheduled Task exists
    bool signin = false;      // the per-user Run value exists
    bool background = false;  // THIS process runs with no desktop (session 0 — the boot task): it cannot ask UAC
    bool exeOk = true;        // the background account can read this program where it is
    std::string exeWhy;       // why not
};

// ── pure ─────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The task's XML (UTF-8 text; written as UTF-16 for schtasks). `exe` is the full path of vibeserver.exe. */
std::string taskXml(const std::string& exe);
/** schtasks.exe arguments (without the program). */
std::vector<std::string> schtasksCreateArgs(const std::string& xmlPath);
std::vector<std::string> schtasksDeleteArgs();
std::vector<std::string> schtasksQueryArgs();
/** The HKCU Run value: "<exe>" --autostart signin */
std::string runValue(const std::string& exe);
/** Can Windows' LOCAL SERVICE account read a program at this path? A personal folder (C:\Users\<name>\…, which is
 *  where Downloads and the Desktop are) is private to its owner — false, with the reason in plain words. */
bool exePathServiceReadable(const std::string& exe, const std::string& usersDir, std::string& why);
/** XML-escape a value placed in the task XML. */
std::string xmlEsc(const std::string& s);

// ── Windows ──────────────────────────────────────────────────────────────────────────────────────────────────────
using AutoLogFn = std::function<void(const std::string&)>;
std::string selfExe();
AutostartState queryAutostart();
/** Un-elevated: the per-user Run value on/off (idempotent). Turning it on also needs the task OFF — the caller
 *  removes the task first; two starters would fight over port 48000. */
bool setSignIn(bool on, std::string& err);
/** ELEVATED (the helper): create/replace the boot task (idempotent) and grant LOCAL SERVICE + `grantSid` modify on
 *  %ProgramData%\VibeServer; or remove the task (absent is success) and revoke the LOCAL SERVICE grant. */
bool helperSetBoot(bool on, const std::string& grantSid, const AutoLogFn& log, std::string& err);
/** The current user's SID string ("S-1-5-21-…"), for --grant-sid. */
std::string currentUserSid();

/** `vibeserver.exe --autostart boot|signin`: the KEEPER. Never returns while it keeps the server running. */
int keeperMain(int argc, char** argv);

}  // namespace vibewin
#endif
