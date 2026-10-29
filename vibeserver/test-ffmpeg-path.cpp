// test-ffmpeg-path.cpp — which ffmpeg a server runs (vibe_ffmpeg_path.h, V12 2026-10-29).
// ★★ /usr/lib/vibeserver/ffmpeg (an owner-placed build; the package no longer ships one) comes first; a system ffmpeg must only ever be the
//    fallback, or a Pi with Debian's 5.1 installed would silently lose xHE-AAC. And the ARMv6 binary must never be handed
//    the NEON copy. This checks the ORDER of the candidates, and that VIBE_FFMPEG overrides them.
#include "vibe_ffmpeg_path.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int fails = 0, checks = 0;
static void check(bool c, const char* what) { ++checks; if (!c) ++fails; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); }
static int indexOf(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return int(i);
    return -1;
}

int main() {
    const auto c = vibe::ffmpegCandidates();
    for (const auto& s : c) std::printf("   candidate: %s\n", s.c_str());
#if defined(__linux__)
#if defined(__ARM_ARCH) && (__ARM_ARCH < 7) && !defined(__aarch64__)
    const std::string own = "/usr/lib/vibeserver/armv6/ffmpeg";
    check(indexOf(c, "/usr/lib/vibeserver/ffmpeg") < 0, "the ARMv6 build never looks at the NEON copy");
#else
    const std::string own = "/usr/lib/vibeserver/ffmpeg";
#endif
    const int iOwn = indexOf(c, own), iSys = indexOf(c, "/usr/bin/ffmpeg");
    check(iOwn >= 0, "the package's own ffmpeg is a candidate");
    check(iSys >= 0 && iOwn < iSys, "…and it comes before the system's /usr/bin/ffmpeg");
    check(!c.empty() && c[0].size() >= 22 && c[0].compare(c[0].size() - own.size() + 4, std::string::npos, own.substr(4)) == 0,
          "the first candidate is the one beside this binary (../lib/vibeserver/…)");
#else
    check(indexOf(c, "/opt/homebrew/bin/ffmpeg") == 0, "macOS: Homebrew's first (unchanged)");
#endif
    setenv("VIBE_FFMPEG", "/nonexistent/test-ffmpeg", 1);
    check(std::string(vibe::ffmpegPath()) == "/nonexistent/test-ffmpeg", "VIBE_FFMPEG overrides every candidate");
    std::printf("\ntest-ffmpeg-path: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
