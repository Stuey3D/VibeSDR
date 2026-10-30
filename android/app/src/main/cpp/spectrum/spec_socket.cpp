// VibeSDR — SpecSocketSink. See spec_socket.h.
#include "spec_socket.h"
#include "spectrum_engine.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <zlib.h>

namespace vibespec {

namespace {
std::mutex gEngMx;
std::weak_ptr<SpectrumEngine> gEngine;

// Inflate a gzip control frame, capped like boundedInflate (8 MB). Empty on any error.
std::string gunzip(const uint8_t* p, size_t len) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return {};
    std::string out;
    char buf[16384];
    zs.next_in = const_cast<Bytef*>(p);
    zs.avail_in = (uInt)len;
    int r;
    do {
        zs.next_out = (Bytef*)buf; zs.avail_out = sizeof buf;
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&zs); return {}; }
        out.append(buf, sizeof buf - zs.avail_out);
        if (out.size() > 8u * 1024 * 1024) { inflateEnd(&zs); return {}; }
    } while (r != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    return r == Z_STREAM_END ? out : std::string();
}

// The number after "key": in a flat JSON object, or NAN. (Config is a flat object; nothing else is read here.)
double jsonNumber(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = s.find(k);
    if (at == std::string::npos) return NAN;
    at = s.find(':', at + k.size());
    if (at == std::string::npos) return NAN;
    const char* c = s.c_str() + at + 1;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    char* end = nullptr;
    const double v = std::strtod(c, &end);
    return end == c ? NAN : v;
}
bool isConfig(const std::string& s) {
    size_t at = s.find("\"type\"");
    if (at == std::string::npos) return false;
    at = s.find(':', at);
    if (at == std::string::npos) return false;
    at = s.find('"', at);
    return at != std::string::npos && s.compare(at, 8, "\"config\"") == 0;
}
}  // namespace

void setCurrentEngine(const std::shared_ptr<SpectrumEngine>& e) { std::lock_guard<std::mutex> lk(gEngMx); gEngine = e; }
void clearCurrentEngine(const SpectrumEngine* e) {
    std::lock_guard<std::mutex> lk(gEngMx);
    if (gEngine.lock().get() == e) gEngine.reset();
}

bool SpecSocketSink::push(SockItem&& it) {
    std::lock_guard<std::mutex> lk(mx_);
    const bool wasEmpty = q_.empty();
    q_.push_back(std::move(it));
    return wasEmpty;
}

std::deque<SockItem> SpecSocketSink::drain() {
    std::lock_guard<std::mutex> lk(mx_);
    std::deque<SockItem> out;
    out.swap(q_);
    return out;
}

void SpecSocketSink::takeCounts(uint32_t& frames, uint64_t& bytes, double& ms) {
    const auto t = std::chrono::steady_clock::now();
    frames = nFrames_.exchange(0, std::memory_order_relaxed);
    bytes = nBytes_.exchange(0, std::memory_order_relaxed);
    ms = std::chrono::duration<double, std::milli>(t - countFrom_).count();
    countFrom_ = t;
}

void SpecSocketSink::setViewMirror(bool inFlight, double c, double bb, bool shared) {
    std::lock_guard<std::mutex> lk(mx_);
    inFlight_ = inFlight; viewCenter_ = c; viewBinBw_ = bb; sharedDial_ = shared;
}

void SpecSocketSink::readConfig(const std::string& json) {
    if (!isConfig(json)) return;
    const double bc = jsonNumber(json, "binCount");
    const double bb = jsonNumber(json, "binBandwidth");
    if (std::isfinite(bc) && bc > 0) dec_.setBinCount((int)bc);   // as the JS: a different count reallocates now
    if (std::isfinite(bb) && bb > 0) statusBinBw_ = bb;
}

bool SpecSocketSink::onText(const char* utf8, size_t len) {
    SockItem it;
    it.kind = SockItem::kText;
    it.text.assign(utf8, len);
    readConfig(it.text);
    return push(std::move(it));
}

bool SpecSocketSink::onBinary(const uint8_t* p, size_t len) {
    nBytes_.fetch_add(len, std::memory_order_relaxed);
    if (SpecDecoder::isGzip(p, len)) {
        readConfig(gunzip(p, len));
        SockItem it; it.kind = SockItem::kBinary; it.bytes.assign(p, p + len);   // the JS still parses and handles it
        return push(std::move(it));
    }
    double freq = 0;
    const SpecDecoder::Result r = dec_.feed(p, len, freq);
    if (r == SpecDecoder::kWide) {
        // ★★ V12 the full-capture row: straight to the engine's layer (wf_wide.h), on this thread, at its arrival. NOT queued
        //    for JS — the JS path would hand it to the engine again; its bytes are already in the link meter's count.
        std::shared_ptr<SpectrumEngine> e;
        { std::lock_guard<std::mutex> lk(gEngMx); e = gEngine.lock(); }
        if (e) e->pushWideNative(p, len);
        return false;
    }
    if (r == SpecDecoder::kSpectrum) {
        feedEngine(dec_.bins(), freq);
        nFrames_.fetch_add(1, std::memory_order_relaxed);
        SockItem it; it.kind = SockItem::kSpec; it.bins = dec_.bins(); it.frequency = freq; it.wireBytes = len;
        it.atMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return push(std::move(it));
    }
    // A fault or an ignored frame: give the JS the raw bytes so it counts (or ignores) it exactly as before.
    SockItem it; it.kind = SockItem::kBinary; it.bytes.assign(p, p + len);
    return push(std::move(it));
}

// The engine gets the frame NOW, with the geometry _emitSpectrum would give it (spec §4, steps 4-7).
void SpecSocketSink::feedEngine(const std::vector<float>& bins, double freq) {
    std::shared_ptr<SpectrumEngine> e;
    { std::lock_guard<std::mutex> lk(gEngMx); e = gEngine.lock(); }
    if (!e) return;
    bool inFlight, shared; double vC, vBB;
    { std::lock_guard<std::mutex> lk(mx_); inFlight = inFlight_; shared = sharedDial_; vC = viewCenter_; vBB = viewBinBw_; }
    const int n = (int)bins.size();
    if (n == 0) return;
    const double bc = n;
    double centerHz = freq;
    double bwHz = statusBinBw_ * bc;
    const bool deviates = std::fabs(freq - vC) > 1 || std::fabs(statusBinBw_ - vBB) > vBB * 1e-6;
    if (inFlight && vBB > 0) { centerHz = vC; bwHz = vBB * bc; }
    else if (vibe_ && shared && vBB > 0 && deviates) { /* the JS adopts the frame's geometry */ }
    else if (vBB > 0 && deviates) { centerHz = vC; bwHz = vBB * bc; }
    // Half-swap into display order — the JS's `unwrapped` (an odd last element keeps its old value).
    if ((int)display_.size() != n) display_.assign(n, 0.f);
    const int half = n >> 1;
    std::memcpy(display_.data(), bins.data() + half, sizeof(float) * half);
    std::memcpy(display_.data() + half, bins.data(), sizeof(float) * half);
    e->pushFrameNative(display_.data(), n, centerHz, bwHz, freq, statusBinBw_ > 0 ? statusBinBw_ * bc : NAN);   // ★ the bins' real span
}

}  // namespace vibespec
