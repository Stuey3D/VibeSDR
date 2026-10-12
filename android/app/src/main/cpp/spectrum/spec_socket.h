// VibeSDR — SpecSocketSink: the native end of the VibeServer / UberSDR spectrum socket (NATIVE-SPECTRUM-ENGINE stage 2).
//
// ★★★ WHY: the waterfall crawled whenever the JS thread was busy (XCover, DAB list, 2026-10-12) because React Native's
//   WebSocket delivers every message ON the JS thread — the native engine could only be fed when JS got round to it.
//   The socket is now OkHttp (VibeSpecSocket.kt, configured exactly as RN's) and every message lands HERE first, on
//   the socket's own thread:
//   · a SPEC frame is decoded (SpecDecoder), put in display order, given its geometry and pushed into the engine AT
//     ONCE — the waterfall no longer waits for JS;
//   · EVERY message — that frame (already decoded), text, gzip control, a frame the JS would count as a fault — is
//     also queued IN ORDER for JS, which drains the queue when it is free and runs the client exactly as before: the
//     meters' per-frame smoothing, squelch, link monitor, settle timers, config, DAB. Nothing is dropped or coalesced
//     (Stuart's meters step once per frame — see the spec in docs/NATIVE-SPECTRUM-ENGINE.md).
// ★★ Config messages that change binCount / binBandwidth are read HERE too (inflated with zlib), so the decoder resizes
//   in sequence with the frames — waiting for JS would decode deltas into the wrong array while JS was behind.
// ★ The engine's geometry follows _emitSpectrum's substitution rules from a MIRROR of the client's view state that JS
//   updates when it zooms, pans, settles or adopts (setViewMirror).
#pragma once
#include "spec_decoder.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vibespec {

class SpectrumEngine;

struct SockItem {
    enum Kind { kText, kBinary, kSpec } kind = kText;
    std::string text;                // kText
    std::vector<uint8_t> bytes;      // kBinary (gzip control / anything the JS parser must see)
    std::vector<float> bins;         // kSpec: decoded bins in SERVER order (the client half-swaps, as before)
    double frequency = 0;            // kSpec
    size_t wireBytes = 0;            // kSpec: the frame's size on the wire (specBytes)
    double atMs = 0;                 // kSpec: when it ARRIVED (epoch ms, Date.now()'s clock) — see takeCounts
};

class SpecSocketSink {
public:
    explicit SpecSocketSink(bool vibeServer) : vibe_(vibeServer) {}

    // ── Socket thread (JNI). Return TRUE when the queue went from empty to non-empty: tell JS. ──
    bool onText(const char* utf8, size_t len);
    bool onBinary(const uint8_t* p, size_t len);

    // ── JS thread ──
    std::deque<SockItem> drain();
    /** The client's view state, mirrored whenever it changes (WsClient._mirrorView). */
    void setViewMirror(bool inFlight, double viewCenterHz, double viewBinBw, bool sharedDial);
    /** ★★ The link meter's per-second counts, taken WHERE THE FRAMES ARRIVE: spectrum frames and binary bytes since
     *  the last call, and the time that covers (steady clock). Counted in JS they measured the JS thread: a rotation
     *  froze it for 1-2 s on the XCover 4s, the 1 s tick saw a "starved" second, the controller stepped a rung down
     *  and the bars fell to 2 of 3 with the network perfectly healthy (Stuart, 2026-10-12). */
    void takeCounts(uint32_t& frames, uint64_t& bytes, double& ms);

private:
    bool push(SockItem&& it);
    void readConfig(const std::string& json);   // binCount / binBandwidth, in sequence with the frames
    void feedEngine(const std::vector<float>& bins, double frequency);

    const bool vibe_;
    std::mutex mx_;
    std::deque<SockItem> q_;
    SpecDecoder dec_;
    double statusBinBw_ = 0;                 // status.binBandwidth (config)
    bool inFlight_ = false, sharedDial_ = false;
    double viewCenter_ = 0, viewBinBw_ = 0;
    std::vector<float> display_;             // half-swapped scratch for the engine
    std::atomic<uint32_t> nFrames_{0};
    std::atomic<uint64_t> nBytes_{0};
    std::chrono::steady_clock::time_point countFrom_ = std::chrono::steady_clock::now();   // JS thread only
};

/** The engine the native socket feeds: the SDR screen's current WaterfallView (set by its host object). */
void setCurrentEngine(const std::shared_ptr<SpectrumEngine>& e);
void clearCurrentEngine(const SpectrumEngine* e);

}  // namespace vibespec
