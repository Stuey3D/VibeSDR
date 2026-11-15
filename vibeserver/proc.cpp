#include "proc.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
/* ★★★ WINDOWS: CreateProcessW, STILL NEVER A SHELL (docs/WINDOWS-PORT-PLAN.md §4.2).
 *  The no-shell design ports as it is: argv is quoted into ONE command line by the rules the C runtime
 *  uses to split it again (CommandLineToArgvW's), so an argument containing spaces or quotes arrives
 *  as the same single argument — the property execvp gave us, kept on purpose.
 *  ★★ EVERY CHILD JOINS ONE JOB OBJECT with KILL_ON_JOB_CLOSE. When this process ends — normally,
 *     by Ctrl-C, by a crash or by Task Manager — Windows closes the job's last handle and kills every
 *     child with it. That is the guarantee PR_SET_PDEATHSIG gives on Linux, without its race, and it
 *     is why a radio process can never outlive the front door here (the "orphaned engine" class).
 *  ★ Paths and arguments are UTF-8 in the core and converted to UTF-16 at this boundary. */
#include <map>
#include <mutex>
#include <io.h>
#include <fcntl.h>

namespace vibeproc {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/** ★ One argument, quoted so CommandLineToArgvW / the CRT give back exactly these characters:
 *  backslashes are literal EXCEPT before a quote, where 2n+1 of them make n backslashes and a quote. */
void appendQuoted(std::wstring& cmd, const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) { cmd += a; return; }
    cmd += L'"';
    for (size_t i = 0; ; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { cmd.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { cmd.append(bs * 2 + 1, L'\\'); cmd += L'"'; }
        else { cmd.append(bs, L'\\'); cmd += a[i]; }
    }
    cmd += L'"';
}

HANDLE theJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
        return j;
    }();
    return job;
}

std::mutex g_mtx;
std::map<pid_t, HANDLE> g_procs;   // ★ the handle is held until reaped, so the pid cannot be reused under us

HANDLE nulOut() {
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

/** Start argv with the given std handles (inheritable; INVALID = the parent's own).
 *  ★ `own` = one of our own radio processes: it keeps our console (its log lines interleave with ours,
 *    as on POSIX) in a process group of its own, so CTRL_BREAK reaches it alone and Ctrl-C reaches only
 *    us — the front door then stops it in order. A helper tool (curl, …) gets no console window at all. */
pid_t start(const std::vector<std::string>& argv, HANDLE out, HANDLE err, bool own = false) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmd += L' '; appendQuoted(cmd, widen(argv[i])); }
    STARTUPINFOW si{}; si.cb = sizeof si;
    const bool redirect = out != INVALID_HANDLE_VALUE || err != INVALID_HANDLE_VALUE;
    if (redirect) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out != INVALID_HANDLE_VALUE ? out : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = err != INVALID_HANDLE_VALUE ? err : GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    // ★ CREATE_SUSPENDED → join the job → resume: a child that ran first could spawn a grandchild
    //   outside the job before we put it in.
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, redirect ? TRUE : FALSE,
                        CREATE_SUSPENDED | (own ? CREATE_NEW_PROCESS_GROUP : CREATE_NO_WINDOW),
                        nullptr, nullptr, &si, &pi)) {
        errno = ENOENT;
        return -1;
    }
    if (HANDLE j = theJob()) AssignProcessToJobObject(j, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_procs[(pid_t)pi.dwProcessId] = pi.hProcess;
    return (pid_t)pi.dwProcessId;
}

HANDLE handleOf(pid_t pid) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_procs.find(pid);
    return it == g_procs.end() ? nullptr : it->second;
}

}  // namespace

int waitChild(pid_t pid, int timeoutMs) {
    HANDLE h = handleOf(pid);
    if (!h) return -1;
    if (WaitForSingleObject(h, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs) != WAIT_OBJECT_0) return -2;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_procs.erase(pid);
    }
    CloseHandle(h);
    return (int)code;
}

void killChild(pid_t pid) {
    if (HANDLE h = handleOf(pid)) TerminateProcess(h, 1);
}

pid_t spawnInherit(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    return start(argv, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, true);
}

void askToStop(pid_t pid) {
    // ★ The radio's process-group id is its pid (CREATE_NEW_PROCESS_GROUP). Fails harmlessly with no console.
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)pid);
}

void restartSelf() {
    /* ★★ Windows has no exec that keeps the process: start a fresh copy with OUR command line, exactly as we
     *    were started (GetCommandLineW is the original, quoting and all), and leave. The new copy is not in
     *    the Job Object (only our children are), so it survives our exit; the radios we started do not —
     *    which is what the POSIX path's reapRadios() + execv achieves. */
    wchar_t self[32768];
    if (!GetModuleFileNameW(nullptr, self, 32768)) return;
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof si.StartupInfo;
    PROCESS_INFORMATION pi{};
    /* ★★ STARTED BY THE KEEPER (start at boot / at sign-in) our output goes to a LOG FILE, not a console — and a
     *    fresh copy started with nothing would write into the void from then on. So pass the same stdout/stderr on,
     *    and ONLY those (an explicit handle list): an inherited copy of our listening socket would keep the port busy
     *    and the new copy could not bind it. With a console (a terminal run) nothing changes. */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    std::vector<HANDLE> pass;
    DWORD hf = 0;
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) == FILE_TYPE_DISK && GetHandleInformation(out, &hf) && (hf & HANDLE_FLAG_INHERIT)) {
        pass.push_back(out);
        if (err && err != out && err != INVALID_HANDLE_VALUE && GetHandleInformation(err, &hf) && (hf & HANDLE_FLAG_INHERIT)) pass.push_back(err);
    }
    std::vector<char> attrBuf;
    BOOL inherit = FALSE; DWORD flags = 0;
    if (!pass.empty()) {
        SIZE_T sz = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
        attrBuf.resize(sz);
        auto* al = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
        if (InitializeProcThreadAttributeList(al, 1, 0, &sz) &&
            UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, pass.data(), pass.size() * sizeof(HANDLE), nullptr, nullptr)) {
            si.lpAttributeList = al;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = out; si.StartupInfo.hStdError = pass.size() > 1 ? err : out;
            si.StartupInfo.hStdInput = nullptr;
            inherit = TRUE; flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
        }
    }
    const BOOL started = CreateProcessW(self, &cmd[0], nullptr, nullptr, inherit, flags, nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!started) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fflush(nullptr);
    ExitProcess(0);
}

int run(const std::vector<std::string>& argv, std::string* out) {
    if (argv.empty()) return -1;
    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE nul = nulOut();
    if (out) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) { CloseHandle(nul); return -1; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    const pid_t pid = start(argv, out ? wr : nul, nul);
    if (wr) CloseHandle(wr);
    CloseHandle(nul);
    if (pid < 0) { if (rd) CloseHandle(rd); return -1; }
    if (out && rd) {
        char buf[4096]; DWORD n = 0;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) out->append(buf, n);
        CloseHandle(rd);
    }
    return waitChild(pid, -1);
}

int runToFile(const std::vector<std::string>& argv, const std::string& path) {
    if (argv.empty() || path.empty()) return -1;
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    HANDLE nul = nulOut();
    const pid_t pid = start(argv, f, nul);
    CloseHandle(f);
    CloseHandle(nul);
    return pid < 0 ? -1 : waitChild(pid, -1);
}

Child spawn(const std::vector<std::string>& argv, bool mergeStderr) {
    Child c;
    if (argv.empty()) return c;
    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    if (!CreatePipe(&rd, &wr, &sa, 0)) return c;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = mergeStderr ? INVALID_HANDLE_VALUE : nulOut();
    const pid_t pid = start(argv, wr, mergeStderr ? wr : nul);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (pid < 0) { CloseHandle(rd); return c; }
    // ★ Text mode: the readers parse lines, and a Windows tool ends them with CRLF.
    const int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_TEXT);
    c.out = fd >= 0 ? _fdopen(fd, "r") : nullptr;
    if (!c.out) { if (fd >= 0) _close(fd); else CloseHandle(rd); killChild(pid); waitChild(pid, -1); return c; }
    c.pid = pid;
    return c;
}

int reap(Child& c) {
    if (c.out) { fclose(c.out); c.out = nullptr; }
    if (c.pid <= 0) return -1;
    const pid_t pid = c.pid;
    c.pid = -1;
    return waitChild(pid, -1);
}

}  // namespace vibeproc

#else  // ── POSIX ──────────────────────────────────────────────────────────────────────────────
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vibeproc {
namespace {

/** argv as execvp wants it. ★ execvp does not modify these, but its prototype predates const, so
 *  the cast is required and is safe. */
std::vector<char*> cArgv(const std::vector<std::string>& argv) {
    std::vector<char*> v;
    v.reserve(argv.size() + 1);
    for (const auto& s : argv) v.push_back(const_cast<char*>(s.c_str()));
    v.push_back(nullptr);
    return v;
}

/** Everything the child does between fork and exec must be async-signal-safe: no allocation, no
 *  locks, no C++ runtime. The argv array is built by the PARENT above for exactly that reason. */
[[noreturn]] void childExec(std::vector<char*>& cv, int outFd, int errFd) {
    if (outFd >= 0 && dup2(outFd, STDOUT_FILENO) < 0) _exit(127);
    if (errFd >= 0) {
        if (dup2(errFd, STDERR_FILENO) < 0) _exit(127);
    } else {
        // ★ No caller wants curl's progress chatter on the daemon's own stderr. Losing /dev/null
        //   is not worth failing the exec over — the child simply keeps the parent's stderr.
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
    }
    // ★ Anything above STDERR belongs to the parent — a leaked descriptor keeps a socket or a
    //   pipe open in a child that knows nothing about it, and readers then never see EOF.
    for (int fd = STDERR_FILENO + 1; fd < 256; ++fd) close(fd);
    execvp(cv[0], cv.data());
    _exit(127);                                  // exec failed: 127 is the shell's own convention
}

int waitFor(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;           // ★ EINTR is not a failure, it is a signal
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

}  // namespace

int run(const std::vector<std::string>& argv, std::string* out) {
    if (argv.empty()) return -1;                 // ★ never a shell, not even by accident
    int fds[2] = {-1, -1};
    if (out && pipe(fds) != 0) return -1;

    auto cv = cArgv(argv);
    const pid_t pid = fork();
    if (pid < 0) { if (fds[0] >= 0) { close(fds[0]); close(fds[1]); } return -1; }
    if (pid == 0) {
        if (fds[0] >= 0) close(fds[0]);
        childExec(cv, fds[1], -1);
    }
    if (fds[1] >= 0) close(fds[1]);

    if (out && fds[0] >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0) out->append(buf, (size_t)n);
        close(fds[0]);
    }
    return waitFor(pid);
}

int runToFile(const std::vector<std::string>& argv, const std::string& path) {
    if (argv.empty() || path.empty()) return -1;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    auto cv = cArgv(argv);
    const pid_t pid = fork();
    if (pid < 0) { close(fd); return -1; }
    if (pid == 0) childExec(cv, fd, -1);
    close(fd);
    return waitFor(pid);
}

Child spawn(const std::vector<std::string>& argv, bool mergeStderr) {
    Child c;
    if (argv.empty()) return c;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return c;

    auto cv = cArgv(argv);
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return c; }
    if (pid == 0) {
        close(fds[0]);
        childExec(cv, fds[1], mergeStderr ? fds[1] : -1);
    }
    close(fds[1]);
    c.out = fdopen(fds[0], "r");
    if (!c.out) { close(fds[0]); kill(pid, SIGKILL); waitFor(pid); return c; }
    c.pid = pid;
    return c;
}

int reap(Child& c) {
    if (c.out) { fclose(c.out); c.out = nullptr; }
    if (c.pid <= 0) return -1;
    const pid_t pid = c.pid;
    c.pid = -1;
    return waitFor(pid);
}

}  // namespace vibeproc

#endif  // _WIN32
