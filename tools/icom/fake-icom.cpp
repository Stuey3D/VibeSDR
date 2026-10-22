// fake-icom — a pretend IC-7300 on a pseudo-terminal, for testing VibeServer end to end with no radio.
//
//   fake-icom [--echo] [--no-transceive] [--usb-send N] [--vox] [--baud B]
//       Opens a pty, prints "FAKE_ICOM_PTY <path>" and serves CI-V on it (icom_fake_radio.h): answers
//       reads, accepts the allowed writes, streams a synthetic 475-point scope paced at the baud rate.
//       Point VibeServer at it:  vibeserver --icom <path> --icom-tty-ok --icom-audio tone
//       Commands on stdin (one per line): "knob <hz>"  "tx 1" / "tx 0"  "stats"  "quit".
//       ★ Every frame it receives is judged by the allow-list AND a crude transmit detector; each
//         violation prints "FAKE_ICOM_VIOLATION <hex>" and is counted in "FAKE_ICOM_STATS".
//
//   fake-icom --fixture <sweeps> <out.civhex>
//       Writes a synthetic capture in the civ-capture.py format (test/fixtures/icom/), so the scope
//       test has a file to replay until Saber's real ones arrive.
#include "icom_fake_radio.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

using namespace icom;

static int dumpFixture(int sweeps, const char* path) {
    FakeIcomRadio r;
    FILE* f = std::fopen(path, "w");
    if (!f) { std::perror(path); return 1; }
    std::fprintf(f, "# fake-icom synthetic IC-7300 capture: centre mode, 14.074 MHz, ±25 kHz, %d sweeps\n", sweeps);
    std::fprintf(f, "# format: <ms> <dir> <hex>  — '<' radio→us, '>' us→radio (tools/icom/civ-capture.py)\n");
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    auto send = [&](std::initializer_list<uint8_t> b) {
        std::vector<uint8_t> v(b);
        std::fprintf(f, "%.1f >", ms()); for (uint8_t x : v) std::fprintf(f, " %02X", x); std::fprintf(f, "\n");
        r.fromController(v.data(), (int)v.size());
    };
    send({0xFE, 0xFE, 0x94, 0xE0, 0x19, 0x00, 0xFD});
    send({0xFE, 0xFE, 0x94, 0xE0, 0x27, 0x10, 0x01, 0xFD});
    send({0xFE, 0xFE, 0x94, 0xE0, 0x27, 0x11, 0x01, 0xFD});
    civ::Parser ps;
    uint8_t buf[512];
    while ((int)r.sweepsOut.load() < sweeps + 1) {
        const int n = r.toController(buf, sizeof buf, 20);
        ps.feed(buf, (size_t)n, [&](const civ::Frame& fr) {
            std::fprintf(f, "%.1f < FE FE %02X %02X %02X", ms(), fr.to, fr.from, fr.cmd);
            for (int i = 0; i < fr.len; i++) std::fprintf(f, " %02X", fr.data[i]);
            std::fprintf(f, " FD\n");
        });
    }
    send({0xFE, 0xFE, 0x94, 0xE0, 0x27, 0x11, 0x00, 0xFD});
    std::fclose(f);
    std::printf("wrote %s\n", path);
    return 0;
}

int main(int argc, char** argv) {
    FakeIcomRadio r;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--fixture" && i + 2 < argc) return dumpFixture(std::atoi(argv[i + 1]), argv[i + 2]);
        else if (a == "--echo") r.echo = true;
        else if (a == "--no-transceive") r.transceive = false;
        else if (a == "--usb-send" && i + 1 < argc) r.usbSend = (uint8_t)std::atoi(argv[++i]);
        else if (a == "--vox") r.vox = true;
        else if (a == "--baud" && i + 1 < argc) r.baud = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "usage: see the comment at the top of tools/icom/fake-icom.cpp\n"); return 2; }
    }
    const int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) { std::perror("pty"); return 1; }
    termios t{}; tcgetattr(m, &t); cfmakeraw(&t); tcsetattr(m, TCSANOW, &t);
    const char* slave = ptsname(m);
    // ★ Keep a slave fd open ourselves, so the master does not see a hangup between client opens.
    const int keep = ::open(slave, O_RDWR | O_NOCTTY);
    if (keep >= 0) { termios s{}; tcgetattr(keep, &s); cfmakeraw(&s); tcsetattr(keep, TCSANOW, &s); }
    std::printf("FAKE_ICOM_PTY %s\n", slave);
    std::fflush(stdout);
    std::atomic<bool> run{true};
    std::thread ctl([&] {
        char line[256];
        while (run.load() && std::fgets(line, sizeof line, stdin)) {
            unsigned long long hz = 0; int v = 0;
            if (std::sscanf(line, "knob %llu", &hz) == 1) r.turnKnob(hz);
            else if (std::sscanf(line, "tx %d", &v) == 1) r.tx = v != 0;
            else if (!std::strncmp(line, "quit", 4)) break;
            std::printf("FAKE_ICOM_STATS frames=%llu violations=%llu sweeps=%llu freq=%llu mode=%u data=%u span=%u tx=%d\n",
                        (unsigned long long)r.framesIn.load(), (unsigned long long)r.violations.load(),
                        (unsigned long long)r.sweepsOut.load(), (unsigned long long)r.freq, r.mode, r.dataOn, r.spanHalf * 2, (int)r.tx.load());
            std::fflush(stdout);
        }
        run.store(false);
    });
    uint64_t seenViol = 0;
    uint8_t buf[512];
    while (run.load()) {
        pollfd p{m, POLLIN, 0};
        if (::poll(&p, 1, 2) > 0 && (p.revents & POLLIN)) {
            const ssize_t n = ::read(m, buf, sizeof buf);
            if (n > 0) r.fromController(buf, (int)n);
        }
        const int n = r.toController(buf, sizeof buf, 3);
        if (n > 0) { ssize_t w = ::write(m, buf, (size_t)n); (void)w; }
        if (r.violations.load() != seenViol) {
            seenViol = r.violations.load();
            std::printf("FAKE_ICOM_VIOLATION %s\n", r.lastViolation.c_str());
            std::fflush(stdout);
        }
    }
    std::fflush(stdout);
    std::printf("FAKE_ICOM_STATS frames=%llu violations=%llu sweeps=%llu\n", (unsigned long long)r.framesIn.load(),
                (unsigned long long)r.violations.load(), (unsigned long long)r.sweepsOut.load());
    std::fflush(stdout);
    std::_Exit(0);   // ★ the stdin thread may still be blocked in fgets — do not unwind under it
}
