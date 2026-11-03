// test-asan-harness.cpp — the memory-check build's flags catch the shape of the USRP clone's crash (2026-11-03).
// Built by test-asan-harness.sh with EXACTLY the flags VIBESERVER_ASAN gives vibeserver (and linux/asan_options.c):
// a thread named 'vibe-usrp' callocs 56 bytes (the calloc(1, 56) the tester's box faulted in) and writes 8 past the end.
// Without ASan this corrupts the next chunk's header silently and a LATER calloc dies; with it, the WRITE is the report.
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/prctl.h>

__attribute__((noinline)) static void writePastTheEnd(char* p, size_t n) { std::memset(p, 0x41, n + 8); }

int main() {
    std::thread t([] {
        prctl(PR_SET_NAME, "vibe-usrp", 0, 0, 0);
        char* p = static_cast<char*>(std::calloc(1, 56));
        writePastTheEnd(p, 56);
        std::free(p);
    });
    t.join();
    return 0;
}
