// audio_rec.h — the AUDIO sheet's recorder, natively: the engine's speaker TAP ({"type":"speaker","tap":1} on the
// spectrum socket) streams this process the speaker socket's raw PCM, and this writes it as a WAV to the card through
// the supervisor (POST /handheld/recordings), exactly as the web page does (web/client/src/audio.ts startRecording /
// stopRecording / _recordPcm, handheld.ts saveRecordingToCard). Inventory: docs/v12/R36S-NATIVE-MENU-INVENTORY.md §3.6,
// §3.7, §5.2.
//
// ★ The PCM frame (local_sdr_shim.cpp sendAudioPcm): [0] channels (1|2) · [1] 0 = raw · [2..5] rate, uint32 LE ·
//   then int16 LE samples. The tapped PCM is taken BEFORE the speaker level, so a recording does not follow the volume.
// ★ Always STEREO in the file, as the web's: a WFM stream drops from 2 ch to 1 ch when the pilot unlocks and a WAV
//   header cannot change its channel count half way, so mono is duplicated.
// ★★ NO OOM (memory: feedback_memory_floor_256mb): the web holds the whole recording in RAM as a Blob. Here it streams
//    to a temp file in $HOME as it arrives and the upload streams from that file, so an hour costs disk, not memory.
//    Only the RECORD BUFFER is in RAM, and it is bounded (60 s stereo = 11.5 MB, mono half that).
// ★ Threads: pcm() / start() / stop() run on the UI thread (Engine::pump calls onAudio there). The upload and the
//   list / delete calls run on their own threads and hand their answers back through a locked mailbox.
#pragma once
#include "http.h"
#include "json.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hh {
namespace arec {

/** A held chunk of the record buffer, as it arrived (mono stays mono: half the memory). */
struct Chunk { std::vector<int16_t> pcm; int ch = 1; int frames = 0; };

/** A finished recording waiting to be sent. */
struct Done { std::string path; uint64_t frames = 0; int rate = 48000; long long startUnix = 0; };

/** What a worker thread hands back to the UI thread. */
struct Mailbox {
    std::mutex m;
    std::deque<std::string> toasts;
    bool listReady = false;
    bool listOk = false;
    std::vector<Json> list;          // {name, bytes, mtime}
    int uploading = 0;               // uploads in flight (the list refreshes when one lands)
};
inline Mailbox& mail() { static Mailbox mb; return mb; }

/** The web's formatSize (web/client/src/recordings.ts). */
inline std::string formatSize(double bytes) {
    char b[32];
    if (bytes >= 1e6) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1e6);
    else if (bytes < 1000) std::snprintf(b, sizeof b, "%d B", (int)std::max(0.0, std::round(bytes)));
    else std::snprintf(b, sizeof b, "%d KB", (int)std::lround(bytes / 1e3));
    return b;
}

inline std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

class Recorder {
public:
    // ── the record buffer (§3.7): 0 = off (frees it); otherwise the seconds to hold ──
    void setBuffer(int seconds) {
        bufSec_ = seconds;
        if (seconds <= 0) { rb_.clear(); rbFrames_ = 0; return; }
        trim();
    }
    int bufferSeconds() const { return bufSec_; }
    void clearBuffer() { rb_.clear(); rbFrames_ = 0; }
    double bufferedSeconds() const { return rate_ > 0 ? (double)rbFrames_ / rate_ : 0; }

    // ── recording ──
    bool recording() const { return f_ != nullptr; }
    /** Seconds recorded so far, counting the buffer's prefix (the web's recordedSeconds). */
    double seconds() const { return recording() ? (double)frames_ / std::max(1, rate_) : 0; }
    uint64_t bytes() const { return frames_ * 4 + 44; }
    /** Wants the tap: recording, or the buffer is on. */
    bool wantsTap() const { return recording() || bufSec_ > 0; }

    /** Start: the temp file, then the record buffer's contents as the prefix (oldest first). */
    bool start() {
        if (f_) return true;
        const char* home = std::getenv("HOME");
        char nm[64];
        std::snprintf(nm, sizeof nm, "/.vibe-hhui-rec-%d.pcm", ++seq_);
        path_ = std::string(home && *home ? home : "/tmp") + nm;
        f_ = std::fopen(path_.c_str(), "wb");
        if (!f_) return false;
        frames_ = 0;
        startWall_ = (long long)std::time(nullptr);
        for (auto& c : rb_) writeStereo(c.pcm.data(), c.ch, c.frames);
        startWall_ -= (long long)std::llround((double)frames_ / std::max(1, rate_));   // ★ start = when the held audio began
        return true;
    }
    /** Stop: the file is closed and handed over to send (false = nothing was captured). */
    bool stop(Done& out) {
        if (!f_) return false;
        std::fclose(f_);
        f_ = nullptr;
        out.path = path_; out.frames = frames_; out.rate = rate_; out.startUnix = startWall_;
        if (!frames_) { std::remove(path_.c_str()); return false; }
        return true;
    }

    /** One message on the audio socket (binary = a PCM frame). Returns true when the recording reached its cap. */
    bool pcm(const std::string& d) {
        if (d.size() <= 6) return false;
        const uint8_t* p = (const uint8_t*)d.data();
        const int ch = p[0];
        if (p[1] != 0 || (ch != 1 && ch != 2)) return false;
        const uint32_t sr = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
        if (sr >= 8000 && sr <= 384000 && (!recording() || frames_ == 0)) rate_ = (int)sr;
        const size_t n = (d.size() - 6) / 2;
        const int frames = (int)(n / (size_t)ch);
        if (frames <= 0) return false;
        // int16 LE on the wire; the R36S (and the test container) is little-endian, as is the WAV
        const int16_t* s = (const int16_t*)(const void*)(p + 6);
        if (bufSec_ > 0) {
            Chunk c; c.ch = ch; c.frames = frames; c.pcm.assign(s, s + (size_t)frames * ch);
            rbFrames_ += (uint64_t)frames;
            rb_.push_back(std::move(c));
            trim();
        }
        if (!f_) return false;
        writeStereo(s, ch, frames);
        // ★ the supervisor takes a WAV of up to 2 GB (hRecordingsPost): stop just short of it
        return bytes() > (2LL << 30) - (1LL << 20);
    }

private:
    FILE* f_ = nullptr;
    std::string path_;
    uint64_t frames_ = 0;
    int rate_ = 48000;
    long long startWall_ = 0;
    int seq_ = 0;
    int bufSec_ = 0;
    std::deque<Chunk> rb_;
    uint64_t rbFrames_ = 0;
    std::vector<int16_t> st_;

    void writeStereo(const int16_t* s, int ch, int frames) {
        if (ch == 2) std::fwrite(s, 2, (size_t)frames * 2, f_);
        else {
            st_.resize((size_t)frames * 2);
            for (int i = 0; i < frames; i++) { st_[(size_t)i * 2] = s[i]; st_[(size_t)i * 2 + 1] = s[i]; }
            std::fwrite(st_.data(), 2, st_.size(), f_);
        }
        frames_ += (uint64_t)frames;
    }
    void trim() {
        // whole chunks from the front while what remains still covers the capacity (audio.ts _rbTrim)
        const uint64_t cap = (uint64_t)bufSec_ * (uint64_t)std::max(1, rate_);
        while (!rb_.empty() && rbFrames_ - (uint64_t)rb_.front().frames >= cap) { rbFrames_ -= (uint64_t)rb_.front().frames; rb_.pop_front(); }
    }
};

/** The web's wavBlob header (audio.ts): RIFF / fmt PCM 16-bit / data. */
inline std::string wavHeader(uint64_t frames, int ch, int rate) {
    const uint32_t data = (uint32_t)(frames * (uint64_t)ch * 2);
    std::string h(44, '\0');
    auto u32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (char)(v >> (8 * i) & 255); };
    auto u16 = [&](size_t o, uint16_t v) { h[o] = (char)(v & 255); h[o + 1] = (char)(v >> 8); };
    std::memcpy(&h[0], "RIFF", 4); u32(4, 36 + data); std::memcpy(&h[8], "WAVE", 4);
    std::memcpy(&h[12], "fmt ", 4); u32(16, 16); u16(20, 1); u16(22, (uint16_t)ch); u32(24, (uint32_t)rate);
    u32(28, (uint32_t)(rate * ch * 2)); u16(32, (uint16_t)(ch * 2)); u16(34, 16);
    std::memcpy(&h[36], "data", 4); u32(40, data);
    return h;
}

/** POST the finished recording to the supervisor, STREAMED from the temp file (http.h's httpRequest wants the body
 *  in memory). Off the UI thread; the answer comes back as a toast. The temp file is removed either way. */
inline void upload(int port, Done d, double hz, std::string mode) {
    { std::lock_guard<std::mutex> lk(mail().m); mail().uploading++; }
    std::thread([port, d, hz, mode] {
        std::string toast;
        int status = 0;
        std::string body;
        FILE* f = std::fopen(d.path.c_str(), "rb");
        const int fd = f ? ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0) : -1;
        if (fd >= 0) {
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            timeval tv{20, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (::connect(fd, (sockaddr*)&a, sizeof a) == 0) {
                const std::string wav = wavHeader(d.frames, 2, d.rate);
                const unsigned long long len = 44ull + d.frames * 4ull;
                char q[256];
                std::snprintf(q, sizeof q, "/handheld/recordings?hz=%lld&mode=%s&start=%lld", (long long)std::llround(hz),
                              pctEncode(mode).c_str(), d.startUnix);
                std::string req = std::string("POST ") + q + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                                  "Content-Type: audio/wav\r\nContent-Length: " + std::to_string(len) + "\r\n\r\n" + wav;
                auto sendAll = [&](const char* p, size_t n) {
                    while (n) { const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return false; p += w; n -= (size_t)w; }
                    return true;
                };
                bool ok = sendAll(req.data(), req.size());
                std::vector<char> buf(1 << 16);
                unsigned long long left = d.frames * 4ull;
                while (ok && left) {
                    const size_t want = (size_t)std::min<unsigned long long>(buf.size(), left);
                    const size_t got = std::fread(buf.data(), 1, want, f);
                    if (got == 0) break;   // (short file: the supervisor answers "did not arrive whole")
                    ok = sendAll(buf.data(), got);
                    left -= got;
                }
                ::shutdown(fd, SHUT_WR);
                std::string all;
                char rb[4096];
                for (;;) { const ssize_t r = ::recv(fd, rb, sizeof rb, 0); if (r <= 0) break; all.append(rb, (size_t)r); if (all.size() > 65536) break; }
                if (all.compare(0, 5, "HTTP/") == 0) {
                    status = std::atoi(all.c_str() + all.find(' ') + 1);
                    const size_t he = all.find("\r\n\r\n");
                    if (he != std::string::npos) body = all.substr(he + 4);
                }
            }
            ::close(fd);
        }
        if (f) std::fclose(f);
        std::remove(d.path.c_str());
        const Json j = Json::parse(body);
        if (status == 200 && j["ok"].boolean()) toast = "Saved to the card";   // (the web adds "Recordings/<name>" as a second line — too wide for one toast here)
        else toast = "Recording NOT saved · " + (j["error"].str().empty() ? std::string("the card did not take it") : j["error"].str());
        std::lock_guard<std::mutex> lk(mail().m);
        mail().toasts.push_back(toast);
        mail().uploading--;
    }).detach();
}

/** GET /handheld/recordings, off the UI thread (mail().list when listReady). */
inline void fetchList(int port) {
    std::thread([port] {
        const HttpResult r = httpRequest(port, "GET", "/handheld/recordings", "", 5);
        const Json j = Json::parse(r.body);
        std::lock_guard<std::mutex> lk(mail().m);
        mail().list = j["recordings"].a;
        mail().listOk = r.status == 200;
        mail().listReady = true;
    }).detach();
}

/** DELETE /handheld/recordings/<name>, then the list again. */
inline void remove(int port, const std::string& name) {
    std::thread([port, name] {
        const HttpResult r = httpRequest(port, "DELETE", "/handheld/recordings/" + pctEncode(name), "", 5);
        if (r.status != 200) { std::lock_guard<std::mutex> lk(mail().m); mail().toasts.push_back("Could not delete " + name); }
        fetchList(port);
    }).detach();
}

}  // namespace arec
}  // namespace hh
