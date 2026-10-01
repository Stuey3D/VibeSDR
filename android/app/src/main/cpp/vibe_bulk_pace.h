// vibe_bulk_pace.h — ONE shared budget for the bytes nobody hears late (map tiles, map data).
//
// ★★★ WHY. Zooming out of the GPU map fires a burst of PMTiles range reads, and every one of them
//     was answered as fast as the socket would take it. On a receiver reached through the
//     directory, all of it leaves through ONE tunnel connection (cloudflared → the edge, HTTP/2 over
//     one TCP stream) — the same one carrying every listener's AUDIO. A few hundred KB of tiles
//     queued ahead of the audio on a home uplink is a few hundred ms of audio arriving late, which is
//     a stutter at the listener ("zooming out of the map caused a tiny stutter", Stuart, 2026-10-01).
//     On a Pi 2 there is a second road to the same place: its Ethernet hangs off the SAME USB 2.0 bus
//     as the dongle. The thread priority rule (Network > Audio > Spectrum > Decoders) puts these
//     bytes below all four, and a lower nice value cannot do that on its own — the queue they fill
//     is not ours.
// ★★ A TOKEN BUCKET, PROCESS-WIDE. Per-connection pacing would let the twenty connections of one
//    zoom add up to twenty times the rate. A small BURST is allowed so a single click (the style, a
//    glyph range, a tile or two) is answered at once; only a sustained load is held to the rate.
// ★ VIBESERVER_BULK_KBPS overrides the rate (KB/s) for measurement; 0 switches pacing off.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace vibebulk {

struct Bucket {
    double rateBps;      // bytes per second; <= 0 = unpaced
    double burstBytes;
    std::mutex m;
    double tokens;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    Bucket(double rate, double burst) : rateBps(rate), burstBytes(burst), tokens(burst) {}

    /** How long the caller must wait before sending `bytes`, with the bytes already charged.
     *  Charging up front (even into debt) is what makes concurrent senders queue fairly rather than
     *  all seeing the same full bucket at once. */
    double charge(size_t bytes, std::chrono::steady_clock::time_point now) {
        if (rateBps <= 0) return 0;
        std::lock_guard<std::mutex> lk(m);
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        tokens = std::min(burstBytes, tokens + std::max(0.0, dt) * rateBps);
        tokens -= (double)bytes;
        return tokens >= 0 ? 0.0 : -tokens / rateBps;
    }
};

/** 512 KB/s with a 128 KB burst: a zoom's first tiles arrive at once, a pan across a continent
 *  costs a second or two more, and the uplink keeps most of its room for the audio. */
inline Bucket& bucket() {
    static Bucket b([] {
        double kbps = 512;
        if (const char* e = std::getenv("VIBESERVER_BULK_KBPS"); e && *e) kbps = std::atof(e);
        return kbps * 1024.0;
    }(), 128.0 * 1024.0);
    return b;
}

/** Block (this connection's thread only) until `bytes` may go out. */
inline void pace(size_t bytes) {
    const double w = bucket().charge(bytes, std::chrono::steady_clock::now());
    if (w > 0) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(w, 5.0)));
}

/** The chunk a paced sender should write at a time: small enough that the pacing is smooth. */
constexpr size_t kChunk = 16 * 1024;

}  // namespace vibebulk
