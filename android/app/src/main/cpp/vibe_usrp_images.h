// vibe_usrp_images.h — the owner's USRP image folder: list, write (atomically), remove. Header-only, no UHD, so it is
// tested on its own (vibeserver/test-usrp-caps.cpp). V12, 2026-11-02 — docs/v12/USRP.md "Board's own image".
//
// ★★★ THE ONE DOOR FOR A CLONE THAT NEEDS ITS MAKER'S FPGA IMAGE. We ship no third-party image; the owner uploads the
//     one their seller provides from the Setup page, and it is written into usrpcaps::ownerImageDir() — the folder
//     usrp_source.cpp's image search tries FIRST, per file — under the exact name UHD loads.
//  ★★ NEVER A PATH FROM THE CLIENT. The name is one of usrpcaps::ownerImageNames() (a closed list) or nothing is
//     written; the folder is ours. The bytes go to a temporary file opened O_EXCL | O_NOFOLLOW beside the target, are
//     size- and head-checked, fsync'd, and renamed over the target — a half-uploaded image is never what UHD finds.
#pragma once
#include "vibe_usrp_caps.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace vibe {
namespace usrpimages {

struct Entry { std::string name; long long bytes = 0; long long mtime = 0; };

/** What is in the owner's folder — only files under UHD's names (anything else there is not ours to show). */
inline std::vector<Entry> list(const std::string& dir) {
    std::vector<Entry> out;
    for (const auto& n : usrpcaps::ownerImageNames()) {
        struct stat st{};
        const std::string p = dir + "/" + n;
        if (::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back({n, (long long)st.st_size, (long long)st.st_mtime});
    }
    return out;
}

inline std::string listJson(const std::string& dir) {
    std::string j = "[";
    bool first = true;
    for (const auto& e : list(dir)) {
        if (!first) j += ',';
        first = false;
        j += "{\"name\":" + usrpcaps::jsonQ(e.name) + ",\"bytes\":" + std::to_string(e.bytes) + ",\"mtime\":" + std::to_string(e.mtime) + "}";
    }
    return j + "]";
}

/** ★ Remove one of OUR names; a name outside the list is refused, a missing file is not an error. */
inline bool remove(const std::string& dir, const std::string& name, std::string& err) {
    if (!usrpcaps::ownerImageName(name)) { err = "that is not one of the file names UHD loads for a B2xx"; return false; }
    const std::string p = dir + "/" + name;
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) return true;
    if (!S_ISREG(st.st_mode)) { err = "that is not a plain file — left alone"; return false; }
    if (::unlink(p.c_str()) != 0) { err = std::string("could not remove it: ") + std::strerror(errno); return false; }
    return true;
}

/** Writes one upload. begin() → write()… → finish(); any refusal or a destructor without finish() leaves the old
 *  file (if any) exactly as it was and no temporary behind. */
class Writer {
public:
    ~Writer() { abort(); }
    bool begin(const std::string& dir, const std::string& name, size_t total, std::string& err) {
        abort();
        if (dir.empty()) { err = "this build has no owner image folder"; return false; }
        err = usrpcaps::refuseImageSize(name, total);
        if (!err.empty()) return false;
        // ★ The folder is created if it is missing (0755: UHD runs as the same user that writes it). A symlink in its
        //   place is refused — lstat, not stat — so the upload cannot be steered somewhere else.
        struct stat st{};
        if (::lstat(dir.c_str(), &st) != 0) {
            if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) { err = std::string("could not create ") + dir + ": " + std::strerror(errno); return false; }
            if (::lstat(dir.c_str(), &st) != 0) { err = std::string("could not create ") + dir; return false; }
        }
        if (!S_ISDIR(st.st_mode)) { err = dir + " is not a plain folder — refusing to write into it"; return false; }
        dir_ = dir; name_ = name; total_ = total; got_ = 0; headChecked_ = false;
        char suffix[48];
        std::snprintf(suffix, sizeof suffix, ".upload-%d-%ld", (int)::getpid(), (long)std::time(nullptr));
        tmp_ = dir + "/." + name + suffix;
        fd_ = ::open(tmp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        if (fd_ < 0) { err = std::string("could not write into ") + dir + ": " + std::strerror(errno); tmp_.clear(); return false; }
        return true;
    }
    bool write(const unsigned char* b, size_t n, std::string& err) {
        if (fd_ < 0) { err = "no upload in progress"; return false; }
        if (got_ + n > total_) { err = "more bytes than were announced"; abort(); return false; }
        if (!headChecked_) {
            head_.insert(head_.end(), b, b + std::min(n, usrpcaps::kHeadBytes - std::min(head_.size(), usrpcaps::kHeadBytes)));
            if (head_.size() >= usrpcaps::kHeadBytes || got_ + n == total_) {
                err = usrpcaps::refuseImageHead(name_, head_.data(), head_.size());
                if (!err.empty()) { abort(); return false; }
                headChecked_ = true;
            }
        }
        size_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(fd_, b + off, n - off);
            if (w < 0) { if (errno == EINTR) continue; err = std::string("write failed: ") + std::strerror(errno); abort(); return false; }
            off += (size_t)w;
        }
        got_ += n;
        return true;
    }
    bool finish(std::string& err) {
        if (fd_ < 0) { err = "no upload in progress"; return false; }
        if (got_ != total_) { err = "the upload ended early (" + std::to_string(got_) + " of " + std::to_string(total_) + " bytes)"; abort(); return false; }
        if (::fsync(fd_) != 0) { err = std::string("could not save it: ") + std::strerror(errno); abort(); return false; }
        ::close(fd_); fd_ = -1;
        if (::rename(tmp_.c_str(), (dir_ + "/" + name_).c_str()) != 0) { err = std::string("could not save it: ") + std::strerror(errno); abort(); return false; }
        tmp_.clear();
        return true;
    }
    void abort() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (!tmp_.empty()) { ::unlink(tmp_.c_str()); tmp_.clear(); }
        head_.clear();
    }
private:
    int fd_ = -1;
    std::string dir_, name_, tmp_;
    size_t total_ = 0, got_ = 0;
    bool headChecked_ = false;
    std::vector<unsigned char> head_;
};

/** 1 = the two files are byte-for-byte the same, 0 = they differ, -1 = one cannot be read. Streamed, never held whole. */
inline int sameFile(const std::string& a, const std::string& b) {
    FILE* fa = std::fopen(a.c_str(), "rb");
    if (!fa) return -1;
    FILE* fb = std::fopen(b.c_str(), "rb");
    if (!fb) { std::fclose(fa); return -1; }
    std::vector<unsigned char> ba(64 * 1024), bb(64 * 1024);
    int r = 1;
    for (;;) {
        const size_t na = std::fread(ba.data(), 1, ba.size(), fa), nb = std::fread(bb.data(), 1, bb.size(), fb);
        if (na != nb || std::memcmp(ba.data(), bb.data(), na) != 0) { r = 0; break; }
        if (na == 0) break;
    }
    std::fclose(fa); std::fclose(fb);
    return r;
}

/* ★★★ THIS COMPUTER'S OWN UHD IMAGES — ONLY ON THE OWNER'S ACTION, NEVER AT OPEN. An owner who already runs the board
 *     with another program (a distribution's UHD through SoapyUHD, with the clone's image in the system images folder —
 *     a tester's OpenWebRX+ set-up) has the right file on disk already. The Setup page lists what is there and the
 *     owner chooses; VibeServer COPIES it into its own override folder. It is never searched silently at open: an old
 *     genuine-board image in a system folder can carry another UHD release's FPGA compatibility number, and would
 *     quietly replace the one that matches UHD 4.9.
 *  ★ `prefix` is for the test (a fake root); "" on a real machine. UHD_IMAGES_DIR is read from OUR environment — the
 *    service's — so a variable set only in someone's shell is not seen, and the page says where it looked. */
inline std::vector<std::string> systemDirs(const std::string& prefix = "") {
    std::vector<std::string> out;
    auto add = [&](const std::string& d) {
        if (d.empty()) return;
        for (const auto& x : out) if (x == d) return;
        struct stat st{};
        if (::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(d);
    };
    if (const char* e = std::getenv("UHD_IMAGES_DIR")) {
        std::string cur;
        for (const char ch : std::string(e) + ":") { if (ch == ':' || ch == ';') { if (!cur.empty()) add(prefix + cur); cur.clear(); } else cur += ch; }
    }
    for (const char* base : {"/usr/share/uhd", "/usr/local/share/uhd", "/opt/homebrew/share/uhd"}) {
        const std::string root = prefix + base;
        add(root + "/images");
        // ★ /usr/share/uhd/<version>/images — some packages keep one folder per UHD release
        if (DIR* d = ::opendir(root.c_str())) {
            std::vector<std::string> subs;
            while (struct dirent* de = ::readdir(d)) {
                const std::string n = de->d_name;
                if (n == "." || n == ".." || n == "images") continue;
                subs.push_back(root + "/" + n + "/images");
            }
            ::closedir(d);
            std::sort(subs.begin(), subs.end());
            for (const auto& x : subs) add(x);
        }
    }
    return out;
}

struct SystemFile { size_t dirIndex = 0; std::string dir, name; long long bytes = 0, mtime = 0; int sameAsShipped = -1; };
/** The B2xx files (UHD's names only) in `dirs`, each compared with the copy VibeServer ships (`shippedDir`). */
inline std::vector<SystemFile> scanSystem(const std::vector<std::string>& dirs, const std::string& shippedDir) {
    std::vector<SystemFile> out;
    for (size_t i = 0; i < dirs.size(); i++)
        for (const auto& n : usrpcaps::ownerImageNames()) {
            struct stat st{};
            const std::string p = dirs[i] + "/" + n;
            if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            SystemFile f;
            f.dirIndex = i; f.dir = dirs[i]; f.name = n; f.bytes = (long long)st.st_size; f.mtime = (long long)st.st_mtime;
            f.sameAsShipped = shippedDir.empty() ? -1 : sameFile(p, shippedDir + "/" + n);
            out.push_back(f);
        }
    return out;
}
inline std::string scanJson(const std::vector<std::string>& dirs, const std::vector<SystemFile>& files) {
    std::string j = ",\"systemDirs\":[";
    for (size_t i = 0; i < dirs.size(); i++) { if (i) j += ','; j += usrpcaps::jsonQ(dirs[i]); }
    j += "],\"system\":[";
    for (size_t i = 0; i < files.size(); i++) {
        const auto& f = files[i];
        if (i) j += ',';
        j += "{\"from\":" + std::to_string(f.dirIndex) + ",\"dir\":" + usrpcaps::jsonQ(f.dir) + ",\"name\":" + usrpcaps::jsonQ(f.name)
           + ",\"bytes\":" + std::to_string(f.bytes) + ",\"mtime\":" + std::to_string(f.mtime)
           + ",\"same\":" + (f.sameAsShipped < 0 ? "null" : f.sameAsShipped ? "true" : "false") + "}";
    }
    return j + "]";
}

/** Copy one file into the owner's folder through the same Writer as an upload (size + head checks, atomic). */
inline bool copyInto(const std::string& src, const std::string& dstDir, const std::string& name, std::string& err) {
    struct stat st{};
    if (::stat(src.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { err = "that file is no longer there"; return false; }
    FILE* f = std::fopen(src.c_str(), "rb");
    if (!f) { err = std::string("could not read ") + src + ": " + std::strerror(errno); return false; }
    Writer w;
    bool ok = w.begin(dstDir, name, (size_t)st.st_size, err);
    std::vector<unsigned char> b(64 * 1024);
    while (ok) {
        const size_t n = std::fread(b.data(), 1, b.size(), f);
        if (n == 0) break;
        ok = w.write(b.data(), n, err);
    }
    std::fclose(f);
    return ok && w.finish(err);
}
/** ★★ "Use this computer's UHD images": the chosen file from system folder `from` — and, with an FPGA image, the FX3
 *  firmware from the SAME folder when it is there and differs from ours (a clone may need both). Returns what was
 *  copied; `err` says what was not. */
inline std::vector<std::string> adopt(const std::vector<std::string>& dirs, size_t from, const std::string& name,
                                      const std::string& dstDir, const std::string& shippedDir, std::string& err) {
    std::vector<std::string> done;
    if (from >= dirs.size()) { err = "that folder is not one of the ones listed"; return done; }
    if (!usrpcaps::ownerImageName(name)) { err = "that is not one of the file names UHD loads for a B2xx"; return done; }
    if (!copyInto(dirs[from] + "/" + name, dstDir, name, err)) return done;
    done.push_back(name);
    const std::string fw = usrpcaps::firmwareFile();
    if (name != fw) {
        const std::string src = dirs[from] + "/" + fw;
        struct stat st{};
        if (::stat(src.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (shippedDir.empty() || sameFile(src, shippedDir + "/" + fw) == 0)) {
            std::string e2;
            if (copyInto(src, dstDir, fw, e2)) done.push_back(fw);
            else err = "the FPGA image was copied, but the firmware beside it was not: " + e2;
        }
    }
    return done;
}

}  // namespace usrpimages
}  // namespace vibe
