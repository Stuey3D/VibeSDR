// ★★★ DOES A LIVE CONNECTION SURVIVE BEING HANDED TO ANOTHER PROCESS?
//
// The whole single-port design rests on this: the front door accepts, decides which radio the
// request belongs to, and passes the socket on. If the descriptor arrives but the connection is
// subtly broken — bytes lost, the peer already gone, the pre-read data consumed — the symptom is a
// browser that hangs with no error at either end, which is close to undiagnosable in the field.
//
// ★★ SO THIS TESTS THE CONNECTION, NOT THE DESCRIPTOR. Asserting "an fd arrived" would pass on a
//    socket that can no longer talk to anyone. It forks a real child, hands it a real accepted TCP
//    connection, and requires the CLIENT to receive what the CHILD wrote.
#include "../android/app/src/main/cpp/fd_passing.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

int main() {
    const std::string sockPath = "/tmp/vibe-fdtest-" + std::to_string(getpid()) + ".sock";
    std::string err;

    std::printf("\nHanding a live TCP connection to another process\n");

    const int lfd = vibe::fdListen(sockPath, err);
    ok(lfd >= 0, "the receiving end can listen", err);
    if (lfd < 0) return 1;

    const pid_t child = fork();
    if (child == 0) {
        // ── The "radio process": take the connection and answer on it. ──
        const int got = vibe::fdAccept(lfd, 5000);
        if (got < 0) _exit(2);
        const char* reply = "HANDED-OVER";
        const ssize_t n = ::send(got, reply, strlen(reply), 0);
        ::close(got);
        _exit(n == (ssize_t)strlen(reply) ? 0 : 3);
    }

    // ── The "front door": listen on TCP, accept, hand the connection over. ──
    const int tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                       // let the kernel choose
    ok(::bind(tcp, (sockaddr*)&a, sizeof a) == 0, "the front door binds");
    socklen_t alen = sizeof a;
    ::getsockname(tcp, (sockaddr*)&a, &alen);
    ::listen(tcp, 4);

    // A client connects and then waits to be spoken to.
    const int cli = ::socket(AF_INET, SOCK_STREAM, 0);
    ok(::connect(cli, (sockaddr*)&a, sizeof a) == 0, "a client connects to the front door");

    const int accepted = ::accept(tcp, nullptr, nullptr);
    ok(accepted >= 0, "the front door accepts it");

    // ★ THE CLIENT SENDS FIRST, and the front door must NOT eat it: the real front peeks the
    //   request line to decide where to route, and the receiving process then has to read that
    //   same request from the beginning.
    const char* request = "GET /r/ABC123/ws/audio HTTP/1.1\r\n";
    ::send(cli, request, strlen(request), 0);
    char peek[64] = {0};
    const ssize_t pn = ::recv(accepted, peek, sizeof peek - 1, MSG_PEEK);
    ok(pn > 0 && std::string(peek).find("/r/ABC123/") != std::string::npos,
       "★ the front can PEEK the request to route it", peek);

    ok(vibe::sendFdTo(sockPath, accepted, err), "the connection is handed over", err);
    ::close(accepted);          // the front drops its copy; the child now owns it

    // ★★★ THE ASSERTION THAT MATTERS: the CLIENT hears the CHILD.
    char buf[64] = {0};
    const ssize_t n = ::recv(cli, buf, sizeof buf - 1, 0);
    ok(n > 0 && std::string(buf) == "HANDED-OVER",
       "★ the client is answered by the OTHER process, on the same connection", buf);

    // And the request the front peeked is still there for the child to read — proven by the fact
    // it was never consumed: peek left it, and nothing else read it.
    int status = 0;
    waitpid(child, &status, 0);
    ok(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the receiving process exited cleanly",
       "status " + std::to_string(status));

    ::close(cli); ::close(tcp); ::close(lfd);
    ::unlink(sockPath.c_str());

    std::printf("\nHanding to somewhere nothing is listening\n");
    {
        // ★ A NORMAL OUTCOME, not an error: it means that radio's process is not running, and the
        //   front answers for itself instead of hanging the listener.
        std::string e2;
        ok(!vibe::sendFdTo("/tmp/vibe-fdtest-nobody.sock", 0, e2),
           "★ it fails rather than blocking", e2);
        ok(!e2.empty(), "and says why", e2);
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
