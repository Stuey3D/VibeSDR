// mapgl_curl.h — the High Detail Maps downloader for the desktop servers (Linux daemon + Mac app).
//
// ★ ONE DOWNLOADER, TWO READERS. main.cpp (Linux, and Mac Full mode) had it inline; Mac Simple mode
//   (vibeserver_api.cpp) never installed one, so its admin page said the detail pack could not be
//   downloaded (2026-09-28). Both now call installCurlDownloader() — the same curl, the same limits.
// ★ curl, as geoip/eibi do: the daemon has no TLS stack of its own. -L because a GitHub release asset
//   is a redirect. No --max-time (169 MB on a slow link is legitimately long); a STALLED transfer is
//   cut off instead: under 1 KB/s for two minutes. Progress is the size of the .part file, polled.
// ★ Android installs its own (JNI + OkHttp) — see vibe_localsdr_jni.cpp.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <sys/stat.h>
#include "proc.h"
#include "vibe_mapgl.h"

namespace vibemapgl {
inline void installCurlDownloader() {
    setDownloader([](const std::string& url, const std::string& dest,
                     std::function<void(int64_t, int64_t)> progress) -> bool {
        std::atomic<bool> done{false};
        std::thread poll([&]() {
            while (!done.load()) {
                struct stat sb{};
                if (::stat(dest.c_str(), &sb) == 0) progress((int64_t)sb.st_size, DETAIL_BYTES);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        const int rc = vibeproc::run({"curl", "-fsSL", "--retry", "3", "--connect-timeout", "30",
                                      "--speed-limit", "1024", "--speed-time", "120",
                                      "-o", dest, url});
        done = true;
        poll.join();
        struct stat sb{};
        if (::stat(dest.c_str(), &sb) == 0) progress((int64_t)sb.st_size, DETAIL_BYTES);
        if (rc != 0) {
            std::fprintf(stderr, "VibeServer: High Detail Maps download: curl exited %d\n", rc);
            return false;
        }
        return true;
    });
}
}  // namespace vibemapgl
