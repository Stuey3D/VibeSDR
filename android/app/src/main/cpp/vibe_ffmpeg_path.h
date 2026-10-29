// vibe_ffmpeg_path.h — WHICH ffmpeg a POSIX server runs (DAB+ when the owner picks FFmpeg: vibe_dab_aacdec.h; DRM xHE-AAC:
// vibe_drm_audiodec.h). One answer for both, so the two can never disagree about which binary — or which version — is
// in use. Not for Android (AMediaCodec / MediaCodec) and never in the App Store app.
//
// ★★★ LINUX: THE PACKAGE'S OWN FIRST (V12, 2026-10-29). The .deb carries a minimal LGPL FFmpeg 8.1 at
//     /usr/lib/vibeserver/ffmpeg (sdr-kit/build-ffmpeg-linux.sh; docs/v12/BUNDLED-FFMPEG.md) — the version we tested, with
//     the xHE-AAC decoder a distribution's ffmpeg often lacks (Pi OS bookworm 5.1, Ubuntu 24.04 6.1). A system ffmpeg is
//     used ONLY if the bundled one is missing (a hand build, a stripped install).
//  ★★ THE ARMv6 BUILD (Pi Zero / Pi 1) RUNS ITS OWN: /usr/lib/vibeserver/armv6/ffmpeg. The armhf package's main copy is
//     ARMv7 + NEON and would die "Illegal instruction" there; the ARMv6 vibeserver binary is compiled separately, so it
//     knows at compile time which one is its own (and never falls back to the NEON one).
//  ★ Found relative to the binary first (/usr/bin/vibeserver → ../lib/vibeserver), as vibe_mapdata.h finds its data, so a
//    package unpacked under another prefix still uses its own copy.
// ★ macOS: unchanged — Homebrew's (/opt/homebrew/bin, /usr/local/bin), /usr/bin, then $PATH. A Finder-launched app has no
//   Homebrew on $PATH, which is why the known homes are tried first.
// ★ VIBE_FFMPEG overrides everything (tests, and an owner who wants a particular build).
#pragma once
#if !defined(__ANDROID__) && (defined(__unix__) || defined(__APPLE__))
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

namespace vibe {

inline std::vector<std::string> ffmpegCandidates() {
    std::vector<std::string> c;
#if defined(__linux__)
#if defined(__ARM_ARCH) && (__ARM_ARCH < 7) && !defined(__aarch64__)
    static constexpr const char* kOwn = "lib/vibeserver/armv6/ffmpeg";   // ★ the ARMv6 binary's own (see above)
#else
    static constexpr const char* kOwn = "lib/vibeserver/ffmpeg";
#endif
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        std::string dir(buf);
        const size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            dir.resize(slash);                                   // /usr/bin
            const size_t up = dir.rfind('/');
            if (up != std::string::npos) c.push_back(dir.substr(0, up + 1) + kOwn);   // /usr/lib/vibeserver/ffmpeg
        }
    }
    c.push_back(std::string("/usr/") + kOwn);
    c.push_back(std::string("/usr/local/") + kOwn);
    c.push_back("/usr/local/bin/ffmpeg");
    c.push_back("/usr/bin/ffmpeg");
#else
    c.push_back("/opt/homebrew/bin/ffmpeg");
    c.push_back("/usr/local/bin/ffmpeg");
    c.push_back("/usr/bin/ffmpeg");
#endif
    return c;
}

/** The ffmpeg this server runs, resolved ONCE. "ffmpeg" (= $PATH) when none of the known homes has one. */
inline const char* ffmpegPath() {
    static const std::string path = [] {
        if (const char* e = std::getenv("VIBE_FFMPEG"); e && *e) return std::string(e);
        for (const std::string& c : ffmpegCandidates())
            if (::access(c.c_str(), X_OK) == 0) return c;
        return std::string("ffmpeg");
    }();
    return path.c_str();
}

/** Is the ffmpeg in use the one the package carries? (For the log line and the DRM/DAB panels' backend names.) */
inline bool ffmpegIsBundled() {
    const std::string p = ffmpegPath();
    return p.find("/lib/vibeserver/") != std::string::npos;
}

}  // namespace vibe
#endif
