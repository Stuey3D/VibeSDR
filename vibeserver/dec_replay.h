// dec_replay.h — feed recorded decoder audio (and channel IQ) back through a real DecoderHost: tool-dec-replay.cpp and
// test-dec-dump.cpp share it. The capture side is android/app/src/main/cpp/vibe_dec_dump.h (VIBE_DEC_AUDIO_DUMP).
// docs/v12/APRS-ON-AIR-2.md.
#pragma once
#include "vibe_decoder_host.h"

#include <chrono>
#include <complex>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace decreplay {

/** A decoder socket that keeps every text frame (APRS, AIS … send JSON) and counts the binary ones. */
struct Peer : vibe::DecoderPeer {
    std::mutex m;
    std::vector<std::string> txt;
    size_t bins = 0;
    bool echo = false;
    bool open() const override { return true; }
    void binary(const uint8_t*, size_t) override { std::lock_guard<std::mutex> lk(m); ++bins; }
    void text(const std::string& s) override {
        std::lock_guard<std::mutex> lk(m);
        txt.push_back(s);
        if (echo && s.find("\"aprs_list\"") == std::string::npos) std::printf("%s\n", s.c_str());
    }
};

/** What a capture's .txt says: the ext, the attach message, the channel IQ's rate (0 = none). */
struct Sidecar { std::string ext, params; double iqRate = 0; bool ok = false; };
inline Sidecar readSidecar(const std::string& path) {
    Sidecar s;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("# ext ", 0) == 0) { s.ext = line.substr(6); s.ok = true; }
        else if (line.rfind("# params ", 0) == 0) s.params = line.substr(9);
        else if (line.rfind("# channel IQ at ", 0) == 0 && s.iqRate == 0) s.iqRate = std::atof(line.c_str() + 16);
    }
    return s;
}
inline bool readCf32(const std::string& path, std::vector<std::complex<float>>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n > 0 ? n : 0) / sizeof(std::complex<float>));
    const size_t got = out.empty() ? 0 : std::fread(out.data(), sizeof(std::complex<float>), out.size(), f);
    out.resize(got);
    std::fclose(f);
    return true;
}

/** Run `audio` (48 kHz mono) and, interleaved by time, `iq` at `iqRate` through a fresh DecoderHost started with
 *  `ext` / `msg`. Paced on the host's own queues (never past what it would drop). The peer holds what came out. */
inline bool replay(const std::string& ext, const std::string& msg, const std::vector<float>& audio,
                   const std::vector<std::complex<float>>& iq, double iqRate, Peer& peer, std::string* why = nullptr) {
    vibe::DecoderHost::Env env;
    env.dialHz = [] { return 144.8e6; };
    env.log = [](const std::string& t) { std::fprintf(stderr, "[host] %s\n", t.c_str()); };
    vibe::DecoderHost host(env, "replay");
    auto p = std::shared_ptr<vibe::DecoderPeer>(&peer, [](vibe::DecoderPeer*) {});
    host.addPeer(p);
    if (host.start(ext, msg) != vibe::DecoderHost::Start::Ok) { if (why) *why = "the host refused '" + ext + "'"; return false; }
    const size_t blk = 960;   // 20 ms, as the server's audio blocks
    size_t qi = 0;
    for (size_t o = 0; o < audio.size(); o += blk) {
        const size_t n = std::min(blk, audio.size() - o);
        // the IQ that belongs before the end of this audio block
        if (iqRate > 0 && !iq.empty()) {
            const size_t until = std::min(iq.size(), size_t(double(o + n) / 48000.0 * iqRate));
            while (qi < until) {
                const size_t k = std::min<size_t>(until - qi, size_t(iqRate / 50) + 1);
                while (host.queuedChannelIqSec() > 0.5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                host.feedChannelIq(iq.data() + qi, int(k), iqRate);
                qi += k;
            }
        }
        while (host.queued() > 24000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        host.feed(audio.data() + o, int(n), 1);
    }
    for (int i = 0; i < 2000 && (host.queued() > 0 || host.queuedChannelIqSec() > 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the last block's decode
    host.shutdown();
    if (host.droppedSamples() && why) *why = "the host dropped " + std::to_string(host.droppedSamples()) + " samples";
    return true;
}

/** The "raw" (TNC2) of every aprs_packet in what a peer got, in order. */
inline std::vector<std::string> aprsRaws(const Peer& peer) {
    std::vector<std::string> out;
    for (const std::string& t : peer.txt) {
        if (t.find("\"type\":\"aprs_packet\"") == std::string::npos) continue;
        const size_t a = t.find("\"raw\":\"");
        if (a == std::string::npos) { out.push_back(t); continue; }
        size_t e = a + 7;
        while (e < t.size() && !(t[e] == '"' && t[e - 1] != '\\')) ++e;
        out.push_back(t.substr(a + 7, e - a - 7));
    }
    return out;
}

} // namespace decreplay
