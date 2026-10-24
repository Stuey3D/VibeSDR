// adsb_data_curl.h — the ADS-B aircraft data's transport for the desktop servers (Linux daemon, Mac Full and
// Simple modes): curl, as the map pack, EiBi and geoip already use. The policy (when, what, the swap) lives in
// vibe_adsb_data_update.h; this is a conditional GET and nothing more.
//
// ★ -L (codeload and raw.githubusercontent may redirect), no --max-time (a slow link is legitimately slow), a
//   STALLED transfer cut off instead (under 1 KB/s for two minutes). -D writes the headers so the ETag and
//   Last-Modified of the FINAL response (after redirects) are what we keep.
// ★ Android's transport is OkHttp (VibeMapGL.fetchIf through JNI) — Android has no curl.
#pragma once
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "proc.h"
#include "vibe_adsb_data_update.h"

namespace vibeadsbdata {

/** Status, ETag and Last-Modified of the LAST response in a curl -D header dump (one block per redirect). */
inline FetchResult parseCurlHeaders(const std::string& h) {
    FetchResult r;
    std::istringstream in(h);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {
            const size_t sp = line.find(' ');
            r.status = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
            r.etag.clear(); r.lastModified.clear();
            continue;
        }
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        for (char& ch : k) ch = char(std::tolower((unsigned char)ch));
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "etag") r.etag = v;
        else if (k == "last-modified") r.lastModified = v;
    }
    return r;
}

inline void installCurlFetcher(const std::string& userAgent) {
    Updater::get().setUserAgent(userAgent);
    Updater::get().setFetcher([userAgent](const std::string& url, const std::string& dest,
                                          const std::string& inm, const std::string& ims) {
        const std::string hdr = dest + ".headers";
        std::vector<std::string> a = {"curl", "-sS", "-L", "--retry", "2", "--connect-timeout", "30",
                                      "--speed-limit", "1024", "--speed-time", "120", "-A", userAgent,
                                      "-D", hdr, "-o", dest};
        if (!inm.empty()) { a.push_back("-H"); a.push_back("If-None-Match: " + inm); }
        if (!ims.empty()) { a.push_back("-H"); a.push_back("If-Modified-Since: " + ims); }
        a.push_back(url);
        const int rc = vibeproc::run(a);
        std::ifstream f(hdr); std::stringstream ss; ss << f.rdbuf();
        ::unlink(hdr.c_str());
        FetchResult r = parseCurlHeaders(ss.str());
        if (rc != 0) { r.status = 0; r.error = "curl exited " + std::to_string(rc); }
        else if (r.status != 200 && r.status != 304) { r.error = "HTTP " + std::to_string(r.status); r.status = 0; }
        if (r.status != 200) ::unlink(dest.c_str());
        return r;
    });
}

} // namespace vibeadsbdata
