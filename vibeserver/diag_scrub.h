// diag_scrub.h — the daemon's own pass over the diagnostics file before it leaves the machine (2026-10-20).
//
// ★★★ BELT AND BRACES. vibeserver-diagnostics (root) already scrubs every secret it can find — the config's
//     password/PIN/key fields, the directory key, every Wi-Fi password NetworkManager holds — and anything that looks
//     like one. This pass scrubs, a second time, the secrets the DAEMON knows (the admin password, the listener PIN,
//     each radio's PIN, the directory key) as literal strings. It is what stands between a secret and the download on
//     a machine without python3, where the helper falls back to a journal-only file it cannot scrub itself.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace vsdiag {

/** Every occurrence of every secret (4+ characters — shorter would scrub ordinary words; longest first, so a secret
 *  that contains another is removed whole) replaced by "[redacted]". */
inline std::string scrubSecrets(std::string text, std::vector<std::string> secrets) {
    secrets.erase(std::remove_if(secrets.begin(), secrets.end(), [](const std::string& s) { return s.size() < 4; }),
                  secrets.end());
    std::sort(secrets.begin(), secrets.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    static const std::string kRed = "[redacted]";
    for (const auto& s : secrets) {
        size_t at = 0;
        while ((at = text.find(s, at)) != std::string::npos) { text.replace(at, s.size(), kRed); at += kRed.size(); }
    }
    return text;
}

/** "vibeserver-diagnostics-<host>-<YYYYmmdd-HHMMSS>.txt", the host reduced to letters, digits and '-'. */
inline std::string fileName(std::string host, const std::string& stamp) {
    for (char& c : host) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) c = '-';
    if (host.empty()) host = "vibeserver";
    return "vibeserver-diagnostics-" + host + "-" + stamp + ".txt";
}

}  // namespace vsdiag
