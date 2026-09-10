#include "proc.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
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
