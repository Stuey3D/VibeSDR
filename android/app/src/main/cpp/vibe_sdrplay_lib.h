// WHICH SDRplay API LIBRARY, AND WHOSE SERVICE — the one fact behind the bundled OpenRSP driver
// (docs/OPENRSP-BUNDLE-PLAN.md §4, §5.2), as pure functions so the decision is tested rather than hoped.
//
// ★★★ ONE QUESTION, SEVERAL READERS. "Is a SYSTEM SDRplay stack installed?" is asked by
//     sdrplay_source.cpp (which library comes first), by linux/vibeserver-rsp.service
//     (ConditionPathExists) and by the Mac app's supervisor (VibeServerApp.swift, through
//     vs_sdrplay_system_stack()). If any two disagreed, the server would be a client of a service
//     that is not running — or two services would fight over one receiver's firmware and claim.
//     The Linux unit file cannot include a header, so its path is spelled out beside the one here.
// ★★ AN INSTALLED STACK WINS, on every platform that has one: it already owns the receivers for every
//    other program on the machine (OpenWebRX, SDR++, SDRconnect), and its owner installed it on purpose.
//    Taking it over silently would change other programs' driver under them.
//
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-sdrplay-lib.cpp
#pragma once
#include <string>
#include <vector>
#include <sys/stat.h>
#include <glob.h>

namespace vibe {
namespace sdrplaylib {

/** ★ `root` is "" in the product and a scratch directory in the test — nothing else may set it. */
inline bool pathExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

/** Linux: SDRplay's installer and OpenRSP's own sdrplay-api-open package both put their service here.
 *  ★★ KEEP IN STEP with ConditionPathExists=! in vibeserver/linux/vibeserver-rsp.service. */
inline constexpr const char* kLinuxSystemService = "/opt/sdrplay_api/sdrplay_apiService";

inline bool linuxSystemStack(const std::string& root = "") {
    return pathExists(root + kLinuxSystemService);
}

/** macOS: SDRplay's installer registers a launch daemon and keeps the service in a VERSIONED
 *  directory (3.15.1 on this Mac). Either is the fact: a daemon plist with its binary removed is a
 *  half-uninstalled stack that will still be started at boot, and a binary without the plist is a
 *  manual install somebody runs by hand — in both cases the receivers are theirs, not ours. */
inline constexpr const char* kMacSystemDaemon = "/Library/LaunchDaemons/com.sdrplay.service.plist";
inline constexpr const char* kMacSystemServiceGlob = "/Library/SDRplayAPI/*/bin/sdrplay_apiService";

inline bool macSystemStack(const std::string& root = "") {
    if (pathExists(root + kMacSystemDaemon)) return true;
#if defined(__ANDROID__)
    // ★ bionic has no glob() below API 28, and there is no Mac stack on a phone to find.
    return false;
#else
    glob_t g{};
    const bool any = ::glob((root + kMacSystemServiceGlob).c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0;
    ::globfree(&g);
    return any;
#endif
}

/** ★ The bundle's Contents directory for an executable's REAL path
 *  ("/Applications/VibeServer.app/Contents/MacOS/vibeserver-engine" → "/Applications/VibeServer.app/Contents"),
 *  or "" when the executable is not inside a bundle — the command-line `vibeserver` in a build tree, which
 *  has no bundled driver and must not invent one. The LAST "/Contents/MacOS/" wins, so an app nested in a
 *  folder that happens to be called Contents still resolves to its own bundle. */
inline std::string bundleContentsOf(const std::string& exeRealPath) {
    static const std::string kMid = "/Contents/MacOS/";
    const size_t at = exeRealPath.rfind(kMid);
    if (at == std::string::npos || at == 0) return {};
    if (exeRealPath.find('/', at + kMid.size()) != std::string::npos) return {};   // MacOS/<dir>/exe is not ours
    return exeRealPath.substr(0, at + 9);   // through "/Contents"
}

/** ★★★ THE ROOT-OWNED CHECK, REPLACED FOR OUR OWN BUNDLE. sdrplay_source.cpp loads a library only if root
 *  owns it — because the Mac app runs with disable-library-validation and nothing else checks what dlopen
 *  runs. An app in /Applications is owned by its USER, so that check cannot pass for the bundled driver.
 *  What it guarded against was a file PLANTED in a user-writable search path; a library whose REAL path (every
 *  symlink resolved) lies inside our own signed bundle's Frameworks directory is not that, so it is accepted
 *  — and only that. `real` must already be realpath()'d: no "..", no symlink can lead out again. */
inline bool insideBundleFrameworks(const std::string& real, const std::string& contents) {
    if (contents.empty() || real.empty()) return false;
    const std::string fw = contents + "/Frameworks/";
    if (real.compare(0, fw.size(), fw) != 0) return false;
    if (real.find("/../") != std::string::npos || real.find("/./") != std::string::npos) return false;
    return real.size() > fw.size() && real.find('/', fw.size()) == std::string::npos;   // directly in Frameworks/
}

/** The library order: an installed system stack first (its service owns the receivers, and the bundled
 *  library speaks the same protocol to either service, so it stays as a fallback); otherwise ours first.
 *  An empty `bundled` (no bundled driver in this build or this bundle) leaves the system list alone. */
inline std::vector<std::string> candidateOrder(bool systemStack, const std::string& bundled,
                                               const std::vector<std::string>& system) {
    std::vector<std::string> out;
    if (!systemStack && !bundled.empty()) out.push_back(bundled);
    out.insert(out.end(), system.begin(), system.end());
    if (systemStack && !bundled.empty()) out.push_back(bundled);
    return out;
}

/** ★★ SHOULD THE MAC APP RUN ITS OWN RSP SERVICE? Only when the bundle carries one AND no system stack is
 *  installed. Two services on one machine would race to upload firmware and to claim the receiver. */
inline bool macRunBundledService(bool bundleHasService, bool systemStack) {
    return bundleHasService && !systemStack;
}

/** ★★ WHICH ANTENNA PORT A NAME MEANS. The exact name always wins. Otherwise a name that is the START or the END of
 *  exactly ONE port's name, at a word boundary, is that port: "Hi-Z" → "Tuner 1 Hi-Z" and "Tuner 2" → "Tuner 2 50Ω"
 *  on a Duo, so a per-band rule written for an RSP2 ("hf:Hi-Z") still means something there. Ambiguous ("Tuner 1",
 *  "50Ω") or unknown → "", and the caller ignores it, as it always ignored an unknown name. */
inline std::string resolvePort(const std::vector<std::string>& ports, const std::string& name) {
    if (name.empty()) return {};
    for (const auto& p : ports) if (p == name) return p;
    std::string hit;
    int n = 0;
    for (const auto& p : ports) {
        if (p.size() <= name.size()) continue;
        const bool head = p.compare(0, name.size(), name) == 0 && p[name.size()] == ' ';
        const bool tail = p.compare(p.size() - name.size(), name.size(), name) == 0 && p[p.size() - name.size() - 1] == ' ';
        if (head || tail) { hit = p; ++n; }
    }
    return n == 1 ? hit : std::string();
}

/** Android: the embedded service needs ASharedMemory_create (API 26). Below that the RSP is not offered at
 *  all — "a control that only works on one radio should not be there" applies to an OS version too. */
inline constexpr int kAndroidMinApi = 26;
inline bool androidRspSupported(int apiLevel) { return apiLevel >= kAndroidMinApi; }

}  // namespace sdrplaylib
}  // namespace vibe
