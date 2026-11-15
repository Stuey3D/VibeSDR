// ★★★ WINDOWS: DOES A LIVE CONNECTION SURVIVE BEING HANDED TO ANOTHER PROCESS? (the front door's whole design)
//
// The POSIX test (vibeserver/test-fd-passing.cpp) forks; Windows cannot, so this starts ITSELF again as the
// "radio" and hands it a real accepted TCP connection through fd_passing.cpp's WSADuplicateSocketW-over-a-named-pipe path.
// ★★ It tests the CONNECTION, not the handle, exactly as the POSIX one does: the request the client sent
//    BEFORE the hand-off (which the front door only MSG_PEEKs) must still be readable by the radio, and the
//    CLIENT must receive what the RADIO wrote — after the front door has closed its own copy.
#include "fd_passing.h"
#include "vibe_sock.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void ok(bool c, const char* what, const std::string& extra = "") {
    std::printf("   %s %s %s\n", c ? "ok  " : "FAIL", what, c ? "" : extra.c_str());
    if (!c) failures++;
}

static int radio(const std::string& path) {
    std::string err;
    const int lfd = vibe::fdListen(path, err);
    if (lfd < 0) { std::printf("radio: fdListen: %s\n", err.c_str()); return 3; }
    const int fd = vibe::fdAccept(lfd, 10000);
    vibe::fdListenClose(lfd);
    if (fd < 0) { std::printf("radio: nothing handed over\n"); return 4; }
    char req[64] = {0};
    const int n = recv((SOCKET)fd, req, sizeof req - 1, 0);
    const std::string reply = std::string("radio saw: ") + (n > 0 ? std::string(req, (size_t)n) : "(nothing)");
    send((SOCKET)fd, reply.data(), (int)reply.size(), 0);
    shutdown((SOCKET)fd, SD_SEND);
    char sink[16]; recv((SOCKET)fd, sink, sizeof sink, 0);
    vibesock::close(fd);
    return 0;
}

int main(int argc, char** argv) {
    vibe_win_startup();
    if (argc == 3 && !std::strcmp(argv[1], "--radio")) return radio(argv[2]);

    std::printf("\nHanding a live TCP connection to another process (Windows: WSADuplicateSocketW over a named pipe)\n");
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "vibe-fdtest-" + std::to_string(GetCurrentProcessId()) + ".sock";

    // The "front door": a listener, a client, an accepted connection with a request already sent.
    const int lis = (int)socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind((SOCKET)lis, (sockaddr*)&a, sizeof a); listen((SOCKET)lis, 1);
    int al = sizeof a; getsockname((SOCKET)lis, (sockaddr*)&a, &al);
    const int cli = (int)socket(AF_INET, SOCK_STREAM, 0);
    ok(connect((SOCKET)cli, (sockaddr*)&a, sizeof a) == 0, "client connects");
    const int acc = (int)accept((SOCKET)lis, nullptr, nullptr);
    ok(acc >= 0, "front door accepts");
    send((SOCKET)cli, "GET /radio/1", 12, 0);
    char peek[32] = {0};
    Sleep(100);
    const int pk = recv((SOCKET)acc, peek, sizeof peek, MSG_PEEK);
    ok(pk == 12, "front door PEEKS the request without consuming it");

    // The "radio": this program again.
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string cmd = std::string("\"") + self + "\" --radio \"" + path + "\"";
    STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    ok(CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0, "radio process starts");

    std::string err;
    bool sent = false;
    for (int i = 0; i < 50 && !sent; ++i) { sent = vibe::sendFdTo(path, acc, err); if (!sent) Sleep(100); }
    ok(sent, "connection handed over and confirmed", err);
    vibesock::close(acc);                    // ★ the front door drops ITS copy; the connection must live on
    char got[128] = {0};
    int total = 0, r;
    while ((r = recv((SOCKET)cli, got + total, (int)sizeof got - 1 - total, 0)) > 0) total += r;
    ok(std::string(got) == "radio saw: GET /radio/1", "client hears the radio, which read the peeked request", got);
    vibesock::close(cli);                    // ★ the listener hangs up; the radio sees EOF and finishes
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
    ok(code == 0, "radio process exits cleanly", "exit " + std::to_string(code));
    vibesock::close(lis);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
