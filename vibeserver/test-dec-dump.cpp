// test-dec-dump.cpp — the developer capture of what a decoder is fed (vibe_dec_dump.h, VIBE_DEC_AUDIO_DUMP) and its
// replay (dec_replay.h, tool-dec-replay.cpp). docs/v12/APRS-ON-AIR-2.md.
//
//  1. Unset: a host decoding APRS writes NOTHING (a child process, the variable never set).
//  2. Set: APRS AUTO on synthetic Bell 202 packets (clean and weak) with channel IQ beside it → a .wav that holds exactly
//     the samples the host was fed, a .cf32 of the IQ, a .txt with the ext, the params and a line a second.
//  3. The replay of that capture through a fresh host gives THE SAME PACKETS, in the same order.
//  4. The cap: a 1 MB cap closes the files at 1 MB and says so.
#include "dec_replay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <random>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void check(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) ++g_pass; else ++g_fail; }

static void addr(std::vector<uint8_t>& b, const std::string& call, int ssid, bool last) {
    for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < int(call.size()) ? call[size_t(i)] : ' ') << 1));
    b.push_back(uint8_t(0x60 | (ssid << 1) | (last ? 1 : 0)));
}
static std::vector<uint8_t> ui(int i) {
    std::vector<uint8_t> b; addr(b, "APRS", 0, false); addr(b, "G4ABC", i % 16, false); addr(b, "WIDE1", 1, true);
    b.push_back(3); b.push_back(0xF0);
    char info[96]; std::snprintf(info, sizeof info, "!%02d%02d.%02dN/00%d%02d.%02dW>capture test %d", 50 + i % 5, i % 60, (i * 7) % 100, i % 9, (i * 3) % 60, i % 100, i);
    b.insert(b.end(), info, info + std::strlen(info));
    return b;
}
/** Bell 202 at 48 kHz, continuous phase, NRZI, `flags` of preamble. */
static void modulate(std::vector<float>& out, const std::vector<uint8_t>& frame, double amp, int flags, double& ph) {
    std::vector<uint8_t> f = frame; vibehdlc::appendFcs(f);
    std::vector<int> bits; for (int i = 0; i < flags; ++i) vibehdlc::flagBits(bits);
    vibehdlc::stuffBits(f, bits); vibehdlc::flagBits(bits); vibehdlc::flagBits(bits);
    std::vector<int> lv; int level = 1; vibehdlc::nrzi(bits, lv, level);
    double t = 0;
    for (size_t k = 0; k < lv.size(); ++k)
        for (; t < (k + 1) * 40.0; t += 1) { ph += 2 * M_PI * (lv[k] ? 1200 : 2200) / 48000.0; out.push_back(float(amp * std::sin(ph))); }
}
static std::vector<std::string> listDir(const std::string& d) {
    std::vector<std::string> v;
    if (DIR* dp = opendir(d.c_str())) { while (dirent* e = readdir(dp)) if (e->d_name[0] != '.') v.push_back(e->d_name); closedir(dp); }
    std::sort(v.begin(), v.end());
    return v;
}
static long fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? long(st.st_size) : -1; }

/** Feed `audio` (+ `iq` alongside) into a host running APRS AUTO; the packets' raws out. */
static std::vector<std::string> runHost(const std::vector<float>& audio, const std::vector<std::complex<float>>& iq, double iqRate) {
    decreplay::Peer peer;
    decreplay::replay("aprs", "{\"packet\":\"auto\"}", audio, iq, iqRate, peer);
    return decreplay::aprsRaws(peer);
}

int main() {
    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vibe-decdump-" + std::to_string(getpid());
    const std::string none = tmp + "-unset", dir = tmp + "-on", small = tmp + "-cap";
    mkdir(none.c_str(), 0755); mkdir(dir.c_str(), 0755); mkdir(small.c_str(), 0755);

    // ── the signal: 12 packets (8 clean, 4 at about 9 dB in 3 kHz), 0.4 s gaps; channel IQ = noise at 37.5 kS/s ──
    std::vector<float> audio; double ph = 0;
    std::vector<std::string> sentRaw;
    for (int i = 0; i < 12; ++i) {
        audio.insert(audio.end(), 19200, 0.f);
        modulate(audio, ui(i), i < 8 ? 0.3 : 0.08, 30, ph);
    }
    audio.insert(audio.end(), 19200, 0.f);
    std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 0.03f);
    for (auto& x : audio) x += nd(rng);
    const double iqRate = 37500;
    std::vector<std::complex<float>> iq(size_t(audio.size() / 48000.0 * iqRate));
    for (auto& z : iq) z = { nd(rng), nd(rng) };
    std::printf("── the signal: %.1f s of audio, 12 packets; %.1f s of channel IQ ──\n", audio.size() / 48000.0, iq.size() / iqRate);

    // ── 1. unset: nothing written ──
    std::printf("── 1. VIBE_DEC_AUDIO_DUMP unset ──\n");
    {
        const pid_t pid = fork();
        if (pid == 0) {
            unsetenv("VIBE_DEC_AUDIO_DUMP");
            if (chdir(none.c_str()) != 0) _exit(3);
            const auto got = runHost(audio, iq, iqRate);
            _exit(vibedec::DecDump::enabled() ? 2 : got.size() >= 8 ? 0 : 1);
        }
        int st = 0; waitpid(pid, &st, 0);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a host decodes APRS with the capture off (child exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + ")");
        check(listDir(none).empty(), "…and not one file is written (" + std::to_string(listDir(none).size()) + " in its directory)");
    }

    // ── 2. set: the capture ──
    std::printf("── 2. VIBE_DEC_AUDIO_DUMP=<dir> ──\n");
    setenv("VIBE_DEC_AUDIO_DUMP", dir.c_str(), 1);
    check(vibedec::DecDump::enabled(), "the capture is on");
    const auto live = runHost(audio, iq, iqRate);
    check(live.size() >= 10, "the live host: " + std::to_string(live.size()) + " of 12 packets");
    std::vector<std::string> files = listDir(dir);
    std::string base;
    for (auto& f : files) if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) base = dir + "/" + f.substr(0, f.size() - 4);
    check(files.size() == 3 && !base.empty(), "three files: .wav .cf32 .txt (" + std::to_string(files.size()) + ")");
    std::vector<float> back; int rate = 0;
    const bool rd = vibedec::DecDump::readWav(base + ".wav", back, rate);
    bool same = rd && back.size() == audio.size();
    for (size_t i = 0; same && i < back.size(); ++i) same = back[i] == audio[i];
    check(rd && rate == 48000, "the .wav opens: 48000 Hz float");
    check(same, "…and holds exactly the samples the host was fed (" + std::to_string(back.size()) + " of " + std::to_string(audio.size()) + ")");
    std::vector<std::complex<float>> iqBack;
    decreplay::readCf32(base + ".cf32", iqBack);
    bool iqSame = iqBack.size() == iq.size();
    for (size_t i = 0; iqSame && i < iq.size(); ++i) iqSame = iqBack[i] == iq[i];
    check(iqSame, "the .cf32 holds exactly the channel IQ (" + std::to_string(iqBack.size()) + " of " + std::to_string(iq.size()) + ")");
    const decreplay::Sidecar sc = decreplay::readSidecar(base + ".txt");
    check(sc.ok && sc.ext == "aprs" && sc.params.find("\"packet\":\"auto\"") != std::string::npos && std::fabs(sc.iqRate - iqRate) < 1,
          "the .txt: ext " + sc.ext + ", params " + sc.params + ", IQ at " + std::to_string((int)sc.iqRate));
    int lines = 0; { std::ifstream f(base + ".txt"); std::string l; while (std::getline(f, l)) if (!l.empty() && l[0] != '#') ++lines; }
    check(lines >= 1, "…and its state lines (" + std::to_string(lines) + ")");

    // ── 3. the replay gives the same packets ──
    std::printf("── 3. replay ──\n");
    unsetenv("VIBE_DEC_AUDIO_DUMP");   // (the static was read once: the replay host writes a capture of its own, ignored)
    decreplay::Peer peer;
    std::vector<std::complex<float>> riq; decreplay::readCf32(base + ".cf32", riq);
    decreplay::replay(sc.ext, sc.params, back, riq, sc.iqRate, peer);
    const auto rep = decreplay::aprsRaws(peer);
    check(rep == live, "the replay: " + std::to_string(rep.size()) + " packets, the same as live (" + std::to_string(live.size()) + "), same order");
    if (!live.empty()) std::printf("     first: %s\n", live.front().c_str());

    // ── 4. the cap ──
    std::printf("── 4. the cap ──\n");
    {
        const uint64_t cap = vibedec::DecDump::capBytes();
        check(cap == 512ull * 1024 * 1024, "the default cap is 512 MB (" + std::to_string(cap >> 20) + " MB)");
        // fill the process total to just under the cap, then one attach: it writes what fits and closes
        vibedec::DecDump::written().store(cap - 100000);
        std::string b2;
        {
            vibedec::DecDump d("aprs", "{}", "cap-test");
            b2 = d.base();
            for (int k = 0; k < 100; ++k) d.audio(audio.data(), 960);
        }
        const long sz = fileSize(b2 + ".wav");
        std::string tail; { std::ifstream f(b2 + ".txt"); std::string l; while (std::getline(f, l)) tail = l; }
        check(sz > 44 && sz <= 44 + 100000, "at the cap the .wav stops (" + std::to_string(sz) + " bytes)");
        check(tail.find("cap reached") != std::string::npos, "…and the .txt says so: " + tail);
    }
    // tidy: our own files only
    for (const std::string& d : { none, dir, small }) { for (auto& f : listDir(d)) std::remove((d + "/" + f).c_str()); rmdir(d.c_str()); }
    std::printf("\ntest-dec-dump: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
