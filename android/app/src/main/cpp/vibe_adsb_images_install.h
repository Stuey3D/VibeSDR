// vibe_adsb_images_install.h — the admin page's "Install logos / silhouettes (.zip)": an owner uploads a pack they
// already have (a VRS OperatorFlags / Silhouettes zip) and the server unpacks it, SAFELY, into its own folders.
//
// ★★★ HOSTILE INPUT BY DEFAULT. Admin-gated, but a zip is still the classic way to write outside a folder
//     ("zip slip") or fill a disk (a bomb). So:
//     - NO PATH FROM THE ARCHIVE IS EVER USED AS A PATH. A member's name is only CLASSIFIED (which folder it is
//       in) and its BASENAME validated as a key (Images::validKey: letters, digits, '-', '_', ≤ 40) with an image
//       extension; the file is written as `<target folder>/<key>.<ext>`, a name we built. Absolute names, drive
//       letters and any ".." component reject the member outright, even though they could not escape anyway.
//     - LIMITS: 64 MB uploaded, 256 MB unpacked in all, 4 MB a file, 20 000 files; inflation stops AT the cap
//       (vibeinflate::zipRead), so a 1 KB member claiming 4 GB costs nothing. Encrypted and ZIP64 members are
//       refused with a sentence, not misread.
//     - ATOMIC: everything is unpacked into a temporary folder beside the target (old files hard-linked in first,
//       so a pack ADDS to what is there), then swapped in by rename. Listeners never see a half-installed folder,
//       and a failure leaves the old one exactly as it was. Live, no restart: the index rescans on the swap.
// ★★ WE SHIP NONE OF THESE IMAGES (airline logos are trademarks — research §1.4); this only installs the owner's.
#pragma once
#include "vibe_adsb_data.h"
#include "vibe_inflate.h"
#include <cerrno>

namespace vibeadsbdata {

struct ImageInstall {
    std::string error;                    ///< "" = done (even if some members were skipped)
    int logos = 0, silhouettes = 0, skipped = 0;
    std::map<std::string, int> why;       ///< skip reason → count
    std::string summary() const {
        if (!error.empty()) return error;
        std::string s = "Installed " + std::to_string(logos) + (logos == 1 ? " logo" : " logos") + " and " +
                        std::to_string(silhouettes) + (silhouettes == 1 ? " silhouette" : " silhouettes");
        if (skipped) {
            s += " (" + std::to_string(skipped) + " skipped: ";
            bool first = true;
            for (const auto& kv : why) { if (!first) s += ", "; first = false; s += std::to_string(kv.second) + " " + kv.first; }
            s += ")";
        }
        return s + ".";
    }
};

constexpr size_t kImgUploadMax = 64u << 20, kImgTotalMax = 256u << 20, kImgFileMax = 4u << 20, kImgFilesMax = 20000;

/** Which folder a member belongs to from its path: 0 logo, 1 silhouette, -1 unknown, -2 refused (unsafe name). */
inline int classifyMember(const std::string& rawName) {
    std::string n = rawName;
    for (char& c : n) if (c == '\\') c = '/';
    if (n.empty() || n[0] == '/' || (n.size() > 1 && n[1] == ':')) return -2;
    int kind = -1;
    size_t o = 0;
    while (o <= n.size()) {
        size_t e = n.find('/', o); if (e == std::string::npos) e = n.size();
        const std::string part = Images::upper(n.substr(o, e - o));
        if (part == "..") return -2;
        if (e != n.size()) {     // a folder name, not the file
            if (part == "OPERATORFLAGS" || part == "OPFLAGS" || part == "FLAGS" || part == "LOGOS" || part == "OPERATORLOGOS") kind = 0;
            else if (part == "SILHOUETTES" || part == "SILHOUETTE" || part == "TYPES" || part == "TYPE") kind = 1;
        }
        o = e + 1;
    }
    return kind;
}

/** The key + extension a member installs as, or "" when it is not an image we keep. */
inline std::string memberFile(const std::string& rawName) {
    std::string n = rawName;
    for (char& c : n) if (c == '\\') c = '/';
    const std::string base = n.substr(n.rfind('/') == std::string::npos ? 0 : n.rfind('/') + 1);
    const size_t dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = base.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext == "jpeg") ext = "jpg";
    if (ext != "png" && ext != "bmp" && ext != "svg" && ext != "gif" && ext != "jpg") return {};
    const std::string key = base.substr(0, dot);
    if (!Images::validKey(key)) return {};
    return key + "." + ext;
}

/** Content check: the bytes must look like the extension says (an "image" that is an executable is refused). */
inline bool looksLike(const std::string& f, const std::string& b) {
    const std::string ext = f.substr(f.rfind('.') + 1);
    auto starts = [&](const char* m, size_t k) { return b.size() >= k && std::memcmp(b.data(), m, k) == 0; };
    if (ext == "png") return starts("\x89PNG\r\n\x1a\n", 8);
    if (ext == "bmp") return starts("BM", 2);
    if (ext == "gif") return starts("GIF8", 4);
    if (ext == "jpg") return starts("\xFF\xD8\xFF", 3);
    if (ext == "svg") return b.find("<svg") != std::string::npos && b.size() < (1u << 20);
    return false;
}

inline bool rmTree(const std::string& d) {
    DIR* dp = ::opendir(d.c_str());
    if (!dp) return ::rmdir(d.c_str()) == 0 || errno == ENOENT;
    while (dirent* e = ::readdir(dp)) {
        const std::string f = e->d_name;
        if (f == "." || f == "..") continue;
        const std::string p = d + "/" + f;
        struct stat sb{};
        if (::lstat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) rmTree(p); else ::unlink(p.c_str());
    }
    ::closedir(dp);
    return ::rmdir(d.c_str()) == 0;
}

/** Swap `fresh` in as `target` (both directories beside each other). */
inline bool swapIn(const std::string& fresh, const std::string& target, std::string& err) {
    const std::string old = target + ".old-" + std::to_string(::getpid());
    struct stat sb{};
    const bool had = ::stat(target.c_str(), &sb) == 0;
    if (had && ::rename(target.c_str(), old.c_str()) != 0) { err = "could not move the old folder aside: " + std::string(std::strerror(errno)); return false; }
    if (::rename(fresh.c_str(), target.c_str()) != 0) {
        err = "could not install the new folder: " + std::string(std::strerror(errno));
        if (had) ::rename(old.c_str(), target.c_str());
        return false;
    }
    if (had) rmTree(old);
    return true;
}

/** Install a pack. `defaultKind` for members in no recognised folder: 0 logos, 1 silhouettes, -1 = ask (skipped,
 *  and the summary says "not in an OperatorFlags or Silhouettes folder"). `imagesDir` = Images' dir. */
inline ImageInstall installImageZip(const uint8_t* zip, size_t n, int defaultKind, const std::string& imagesDir) {
    ImageInstall r;
    if (imagesDir.empty()) { r.error = "this server has no folder for logos"; return r; }
    if (n > kImgUploadMax) { r.error = "the zip is larger than 64 MB"; return r; }
    std::vector<vibeinflate::ZipEntry> list;
    std::string e = vibeinflate::zipList(zip, n, list, kImgFilesMax);
    if (!e.empty()) { r.error = e; return r; }
    ::mkdir(imagesDir.c_str(), 0755);
    const std::string folder[2] = {imagesDir + "/OperatorFlags", imagesDir + "/Silhouettes"};
    const std::string tmp[2] = {folder[0] + ".new-" + std::to_string(::getpid()), folder[1] + ".new-" + std::to_string(::getpid())};
    bool used[2] = {false, false};
    auto skip = [&](const char* why) { ++r.skipped; ++r.why[why]; };
    auto prepare = [&](int k) -> bool {
        if (used[k]) return true;
        rmTree(tmp[k]);
        if (::mkdir(tmp[k].c_str(), 0755) != 0) { r.error = "cannot create " + tmp[k] + ": " + std::strerror(errno); return false; }
        // ★ the pack ADDS to what is installed: hard-link the existing files in (a copy where links are refused)
        if (DIR* dp = ::opendir(folder[k].c_str())) {
            while (dirent* de = ::readdir(dp)) {
                const std::string f = de->d_name;
                if (f[0] == '.') continue;
                const std::string a = folder[k] + "/" + f, b = tmp[k] + "/" + f;
                if (::link(a.c_str(), b.c_str()) != 0) {
                    FILE* in = std::fopen(a.c_str(), "rb"); FILE* out = in ? std::fopen(b.c_str(), "wb") : nullptr;
                    char buf[16384]; size_t got;
                    while (in && out && (got = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, got, out);
                    if (in) std::fclose(in);
                    if (out) std::fclose(out);
                }
            }
            ::closedir(dp);
        }
        used[k] = true;
        return true;
    };
    size_t total = 0;
    std::string body;
    for (const auto& z : list) {
        if (z.dir()) continue;
        const int cls = classifyMember(z.name);
        if (cls == -2) { skip("with an unsafe path"); continue; }
        const std::string file = memberFile(z.name);
        if (file.empty()) {
            // ★ readme.txt, __MACOSX/._x.png and the like are not failures worth shouting about, but they are counted
            skip("not images named as an operator, registration or type code");
            continue;
        }
        const size_t slash = z.name.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? z.name : z.name.substr(slash + 1);
        if (!base.empty() && base[0] == '.') { skip("hidden files"); continue; }
        const int kind = cls >= 0 ? cls : defaultKind;
        if (kind < 0) { skip("not in an OperatorFlags or Silhouettes folder (choose which they are)"); continue; }
        if (z.flags & 1) { skip("encrypted"); continue; }
        if (z.usize > kImgFileMax) { skip("larger than 4 MB"); continue; }
        if (total + z.usize > kImgTotalMax) { r.error = "the zip unpacks to more than 256 MB — nothing was installed"; break; }
        e = vibeinflate::zipRead(zip, n, z, kImgFileMax, body);
        if (!e.empty()) {
            if (e.find("expands past") != std::string::npos) { r.error = "the zip is malformed (a member expands past its stated size) — nothing was installed"; break; }
            skip("damaged or unsupported"); continue;
        }
        if (!looksLike(file, body)) { skip("whose contents are not the image their name says"); continue; }
        total += body.size();
        if (!prepare(kind)) break;
        // ★★ the name we write is ours: the validated key + extension, inside the temporary folder
        const std::string out = tmp[kind] + "/" + file;
        // one key, one file: a new upload's .png replaces an old .bmp of the same key
        for (const char* x : {"png", "bmp", "svg", "gif", "jpg"}) {
            const std::string other = tmp[kind] + "/" + file.substr(0, file.rfind('.') + 1) + x;
            if (other != out) ::unlink(other.c_str());
        }
        FILE* f = std::fopen(out.c_str(), "wb");
        if (!f || std::fwrite(body.data(), 1, body.size(), f) != body.size()) { if (f) std::fclose(f); r.error = "could not write to " + tmp[kind]; break; }
        std::fclose(f);
        (kind ? r.silhouettes : r.logos)++;
    }
    for (int k = 0; k < 2; ++k) {
        if (!used[k]) continue;
        if (!r.error.empty()) { rmTree(tmp[k]); continue; }
        std::string err;
        if (!swapIn(tmp[k], folder[k], err)) { r.error = err; rmTree(tmp[k]); }
    }
    if (!r.error.empty()) { r.logos = r.silhouettes = 0; }
    Db::images().setDir(imagesDir);          // ★ rescan now: live, no restart
    return r;
}

/** "Remove all" for one folder. */
inline bool removeImages(const std::string& imagesDir, int kind, std::string& err) {
    if (imagesDir.empty()) { err = "this server has no folder for logos"; return false; }
    const std::string d = imagesDir + (kind ? "/Silhouettes" : "/OperatorFlags");
    const std::string gone = d + ".gone-" + std::to_string(::getpid());
    struct stat sb{};
    if (::stat(d.c_str(), &sb) != 0) { Db::images().setDir(imagesDir); return true; }
    if (::rename(d.c_str(), gone.c_str()) != 0) { err = std::string("could not remove: ") + std::strerror(errno); return false; }
    rmTree(gone);
    Db::images().setDir(imagesDir);
    return true;
}

} // namespace vibeadsbdata
