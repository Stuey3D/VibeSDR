// Host harness: runs the REAL android/app/src/main/cpp/rtl_tcp_server.cpp over the fake librtlsdr.
// Usage: harness <port>   (FAKE_RADIO=rtl|airspy). Prints START OK/START REFUSED, then serves until stdin closes.
#include "rtl_tcp_server.h"
#include <cstdio>
#include <cstdlib>
#include <string>
int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 12345;
    std::string err;
    int bound = vibe::RtlTcpServer::instance().start(/*fd*/ 0, 0, 0, 2048000, 100000000, -1, port, 0, err);
    if (bound < 0) { std::printf("START REFUSED: %s\n", err.c_str()); std::fflush(stdout); return 3; }
    std::printf("START OK %d\n", bound); std::fflush(stdout);
    int c; while ((c = std::getchar()) != EOF) {}
    vibe::RtlTcpServer::instance().stop();
    return 0;
}
