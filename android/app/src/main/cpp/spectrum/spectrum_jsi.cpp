// VibeSDR — the SpectrumEngine as a JSI host object (docs/NATIVE-SPECTRUM-ENGINE.md, stage 1).
//
// ONE object, reachable from BOTH runtimes: WaterfallView creates it on the JS thread
// (global.__vibeSpectrumCreate()) and a worklet captures it, so the same C++ engine is called from JS (push, state)
// and from the UI thread (poll). Every method makes its jsi values in the runtime that called it.
// ★ Images and paths come from the vibe_rnskia exports of the patched react-native-skia: made on the engine thread,
//   wrapped here on the UI runtime WITHOUT external memory pressure (cheap dispose — the worklet disposes the
//   previous one itself).
#include "spectrum_engine.h"
#include "spec_socket.h"
#include <map>
#include <mutex>
#include <jsi/jsi.h>
#include <react-native-skia/VibeExport.h>
#include <cstring>
#include <memory>
#include <string>
#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace jsi = facebook::jsi;
using vibespec::SpectrumEngine;

namespace {

void* mkImage(const uint8_t* px, int w, int h, size_t rb) { return vibe_rnskia::makeGray8Image(px, w, h, rb); }
void* mkPath(const float* xy, int n, bool close) { return vibe_rnskia::makePolyPath(xy, n, close); }
void rlImage(void* p) { vibe_rnskia::releaseImage(p); }
void rlPath(void* p) { vibe_rnskia::releasePath(p); }

double num(jsi::Runtime& rt, const jsi::Object& o, const char* k, double dflt) {
    const jsi::Value v = o.getProperty(rt, k);
    return v.isNumber() ? v.asNumber() : dflt;
}
bool flag(jsi::Runtime& rt, const jsi::Object& o, const char* k, bool dflt) {
    const jsi::Value v = o.getProperty(rt, k);
    return v.isBool() ? v.getBool() : dflt;
}

// ── The native spectrum sockets (spec_socket.h), by the id VibeSpecSocket.kt gives each ──────────────────────
std::mutex gSinkMx;
std::map<int, std::shared_ptr<vibespec::SpecSocketSink>> gSinks;
std::shared_ptr<vibespec::SpecSocketSink> sinkFor(int id) {
    std::lock_guard<std::mutex> lk(gSinkMx);
    auto it = gSinks.find(id);
    return it == gSinks.end() ? nullptr : it->second;
}

/** A JS ArrayBuffer that owns its bytes (no copy back to JS land beyond this one). */
class VecBuffer : public jsi::MutableBuffer {
public:
    explicit VecBuffer(std::vector<uint8_t>&& v) : v_(std::move(v)) {}
    size_t size() const override { return v_.size(); }
    uint8_t* data() override { return v_.data(); }
private:
    std::vector<uint8_t> v_;
};

class SpectrumHost : public jsi::HostObject {
public:
    SpectrumHost() : engine_(std::make_shared<SpectrumEngine>(mkImage, mkPath, rlImage, rlPath)) {
        vibespec::setCurrentEngine(engine_);   // the native spectrum socket feeds the SDR screen's engine
    }
    ~SpectrumHost() override { vibespec::clearCurrentEngine(engine_.get()); }

    jsi::Value get(jsi::Runtime& rt, const jsi::PropNameID& nameId) override {
        const std::string name = nameId.utf8(rt);
        auto e = engine_;
        auto fn = [&](int argc, auto body) {
            return jsi::Function::createFromHostFunction(rt, nameId, argc,
                [e, body](jsi::Runtime& r, const jsi::Value&, const jsi::Value* a, size_t c) -> jsi::Value {
                    return body(r, *e, a, c);
                });
        };
        // ── JS thread ──
        if (name == "push") return fn(6, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            // push(bins: Float32Array, centerHz, bwHz, trueCenterHz | undefined, lastInteractAt, trueBwHz | undefined)
            if (c < 3 || !a[0].isObject()) return jsi::Value::undefined();
            const jsi::Object arr = a[0].asObject(r);
            const jsi::ArrayBuffer buf = arr.getProperty(r, "buffer").asObject(r).getArrayBuffer(r);
            const size_t off = (size_t)arr.getProperty(r, "byteOffset").asNumber();
            const size_t len = (size_t)arr.getProperty(r, "length").asNumber();
            if (off + len * sizeof(float) > buf.size(r)) return jsi::Value::undefined();
            const float* bins = reinterpret_cast<const float*>(buf.data(r) + off);
            const double tc = (c > 3 && a[3].isNumber()) ? a[3].asNumber() : NAN;
            if (c > 4 && a[4].isNumber()) en.setLastInteractAt(a[4].asNumber());   // read at arrival, as enqueueFrame did
            const double tb = (c > 5 && a[5].isNumber()) ? a[5].asNumber() : NAN;
            en.pushFrame(bins, (int)len, a[1].asNumber(), a[2].asNumber(), tc, tb);
            return jsi::Value::undefined();
        });
        if (name == "pushWide") return fn(1, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            // pushWide(frame: Uint8Array) — ★ V12 an SPWB full-capture row the JS socket received (wf_wide.h)
            if (c < 1 || !a[0].isObject()) return jsi::Value::undefined();
            const jsi::Object arr = a[0].asObject(r);
            const jsi::ArrayBuffer buf = arr.getProperty(r, "buffer").asObject(r).getArrayBuffer(r);
            const size_t off = (size_t)arr.getProperty(r, "byteOffset").asNumber();
            const size_t len = (size_t)arr.getProperty(r, "length").asNumber();
            if (off + len > buf.size(r)) return jsi::Value::undefined();
            en.pushWide(buf.data(r) + off, len);
            return jsi::Value::undefined();
        });
        if (name == "setView") return fn(2, [](jsi::Runtime&, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c >= 2) en.setView(a[0].asNumber(), a[1].asNumber());
            return jsi::Value::undefined();
        });
        if (name == "setConfig") return fn(1, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c < 1 || !a[0].isObject()) return jsi::Value::undefined();
            const jsi::Object o = a[0].asObject(r);
            vibespec::EngineConfig k;
            k.width = num(r, o, "width", 0); k.wfTop = num(r, o, "wfTop", 0); k.specH = num(r, o, "specH", 0);
            k.specShow = flag(r, o, "specShow", true); k.peakHold = flag(r, o, "peakHold", true);
            k.smoothTune = flag(r, o, "smoothTune", true); k.targetRows = (int)num(r, o, "targetRows", 0);
            k.displayCapHz = (int)num(r, o, "displayCapHz", 0);
            k.panelHz = (int)num(r, o, "panelHz", 0);
            k.stampViewRows = (int)num(r, o, "stampViewRows", 0);       // ★★ the scale stamps (wf_stamps.h)
            k.stampMinGapRows = (int)num(r, o, "stampMinGapRows", 0);
            k.ringRows = (int)num(r, o, "ringRows", 0);                 // ★★ the ring depth to grow to (wf_ring_grow.h)
            en.setConfig(k);
            return jsi::Value::undefined();
        });
        if (name == "setSettings") return fn(1, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c < 1 || !a[0].isObject()) return jsi::Value::undefined();
            const jsi::Object o = a[0].asObject(r);
            vibedsp::SpectrumProcessorSettings s;
            s.autoContrast = num(r, o, "autoContrast", 5); s.minRangeDb = num(r, o, "minRangeDb", 30);
            s.manual = flag(r, o, "manual", false); s.manualMinDb = num(r, o, "manualMinDb", -120);
            s.manualMaxDb = num(r, o, "manualMaxDb", -20); s.specFloor = num(r, o, "specFloor", 0);
            s.specPeakScale = num(r, o, "specPeakScale", 10); s.smoothingFrames = num(r, o, "smoothingFrames", 5);
            s.avgFrames = num(r, o, "avgFrames", 0); s.spatialSmooth = flag(r, o, "spatialSmooth", true);
            s.peakHold = flag(r, o, "peakHold", true); s.wfBrightness = num(r, o, "wfBrightness", 0);
            s.wfContrast = num(r, o, "wfContrast", 0); s.wfSharpness = num(r, o, "wfSharpness", 0);
            en.setSettings(s);
            return jsi::Value::undefined();
        });
        if (name == "setInteractAt") return fn(1, [](jsi::Runtime&, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c >= 1 && a[0].isNumber()) en.setLastInteractAt(a[0].asNumber());
            return jsi::Value::undefined();
        });
        if (name == "setBackground") return fn(1, [](jsi::Runtime&, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c >= 1) en.setBackground(a[0].isBool() && a[0].getBool());
            return jsi::Value::undefined();
        });
        // ★★ V12 2026-10-30 the audio's playout delay, ms (−1 = unknown) — the clock waits for the sound (wf_av_sync.h).
        if (name == "rowGeom") return fn(1, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            double lo = 0, span = 0;   // ★★ a tap on an old row tunes by its own scale (#57)
            if (c < 1 || !a[0].isNumber() || !en.rowGeom(a[0].asNumber(), lo, span)) return jsi::Value::null();
            jsi::Array arr(r, 2); arr.setValueAtIndex(r, 0, lo); arr.setValueAtIndex(r, 1, span);
            return arr;
        });
        if (name == "setAudioDelay") return fn(1, [](jsi::Runtime&, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            en.setAudioDelay(c >= 1 && a[0].isNumber() ? a[0].asNumber() : -1);
            return jsi::Value::undefined();
        });
        if (name == "setCovered") return fn(1, [](jsi::Runtime&, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c >= 1) en.setCovered(a[0].isBool() && a[0].getBool());
            return jsi::Value::undefined();
        });
        if (name == "liveRange") return fn(0, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value*, size_t) -> jsi::Value {
            int mn, mx;
            if (!en.liveRange(mn, mx)) return jsi::Value::null();   // unchanged since the last read
            jsi::Array out(r, 2);
            out.setValueAtIndex(r, 0, mn); out.setValueAtIndex(r, 1, mx);
            return out;
        });
        // ── UI thread ──
        if (name == "poll") return fn(0, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value*, size_t) -> jsi::Value {
            vibespec::UiUpdate u;
            if (!en.pollUi(u)) return jsi::Value::null();
            jsi::Object o(r);
            if (u.image) o.setProperty(r, "image", vibe_rnskia::wrapImage(r, u.image));   // adopts the handle
            o.setProperty(r, "head", u.head);
            o.setProperty(r, "texReady", u.texReady);
            o.setProperty(r, "quant", u.quant);
            o.setProperty(r, "avgMs", u.avgFrameMs);
            o.setProperty(r, "rowRate", u.rowRate);   // ★ V12 the fixed output clock's rate (0 = NATIVE / actual rows)
            o.setProperty(r, "scrollGen", (double)u.scrollGen);
            o.setProperty(r, "glide", u.glide);
            o.setProperty(r, "glideDur", u.glideDur);
            if (u.image) {                                   // ride with the image (WaterfallView enginePoll)
                o.setProperty(r, "moLo", u.moLo); o.setProperty(r, "moHi", u.moHi);
                o.setProperty(r, "fanK", u.fanK); o.setProperty(r, "fanC", u.fanC);
                o.setProperty(r, "ringRows", u.ringRows);    // ★★ the depth of THIS image (the shader's uRows)
            }
            o.setProperty(r, "specGen", (double)u.specGen);
            o.setProperty(r, "specHidden", u.specHidden);
            if (!u.specTarget.empty()) {
                jsi::Array arr(r, u.specTarget.size());
                for (size_t i = 0; i < u.specTarget.size(); i++) arr.setValueAtIndex(r, i, (double)u.specTarget[i]);
                o.setProperty(r, "spec", arr);
            }
            o.setProperty(r, "stampGen", (double)u.stampGen);   // ★★ the scale stamps (wf_stamps.h), when they changed
            if (u.stampsNew) {
                jsi::Array arr(r, u.stamps.size());
                for (size_t i = 0; i < u.stamps.size(); i++) {
                    const auto& st = u.stamps[i];
                    jsi::Object so(r);
                    so.setProperty(r, "id", (double)st.id); so.setProperty(r, "frame", st.frame);
                    so.setProperty(r, "lo", st.lo); so.setProperty(r, "span", st.span); so.setProperty(r, "born", st.born);
                    arr.setValueAtIndex(r, i, so);
                }
                o.setProperty(r, "stamps", arr);
            }
            o.setProperty(r, "peakGen", (double)u.peakGen);
            if (u.peakPath) o.setProperty(r, "peak", vibe_rnskia::wrapPath(r, u.peakPath));   // adopts the handle
            return o;
        });
        return jsi::Value::undefined();
    }

private:
    std::shared_ptr<SpectrumEngine> engine_;
};

}  // namespace

/** Installs global.__vibeSpectrumCreate(), __vibeSpecSockDrain(id) and __vibeSpecSockMirror(...). JS thread. */
void vibeSpectrumInstall(jsi::Runtime& rt) {
    // Drain everything the native socket received, IN ORDER: [{k:0,text}|{k:1,data:ArrayBuffer}|{k:2,bins:Float32Array,freq,bytes}]
    rt.global().setProperty(rt, "__vibeSpecSockDrain", jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forAscii(rt, "__vibeSpecSockDrain"), 1,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value* a, size_t c) -> jsi::Value {
            auto sink = c >= 1 && a[0].isNumber() ? sinkFor((int)a[0].asNumber()) : nullptr;
            if (!sink) return jsi::Array(r, 0);
            auto items = sink->drain();
            jsi::Array out(r, items.size());
            jsi::Function f32 = r.global().getPropertyAsFunction(r, "Float32Array");
            size_t i = 0;
            for (auto& it : items) {
                jsi::Object o(r);
                o.setProperty(r, "k", (int)it.kind);
                if (it.kind == vibespec::SockItem::kText) {
                    o.setProperty(r, "text", jsi::String::createFromUtf8(r, it.text));
                } else if (it.kind == vibespec::SockItem::kBinary) {
                    o.setProperty(r, "data", jsi::ArrayBuffer(r, std::make_shared<VecBuffer>(std::move(it.bytes))));
                } else {
                    std::vector<uint8_t> raw(it.bins.size() * sizeof(float));
                    std::memcpy(raw.data(), it.bins.data(), raw.size());
                    jsi::ArrayBuffer ab(r, std::make_shared<VecBuffer>(std::move(raw)));
                    o.setProperty(r, "bins", f32.callAsConstructor(r, ab));
                    o.setProperty(r, "freq", it.frequency);
                    o.setProperty(r, "bytes", (double)it.wireBytes);
                    o.setProperty(r, "at", it.atMs);
                }
                out.setValueAtIndex(r, i++, o);
            }
            return out;
        }));
    // __vibeSpecSockMirror(id, inFlight, viewCenterHz, viewBinBw, sharedDial) — the client's view state
    rt.global().setProperty(rt, "__vibeSpecSockMirror", jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forAscii(rt, "__vibeSpecSockMirror"), 5,
        [](jsi::Runtime&, const jsi::Value&, const jsi::Value* a, size_t c) -> jsi::Value {
            if (c < 5 || !a[0].isNumber()) return jsi::Value::undefined();
            auto sink = sinkFor((int)a[0].asNumber());
            if (sink) sink->setViewMirror(a[1].isBool() && a[1].getBool(), a[2].isNumber() ? a[2].asNumber() : 0,
                                          a[3].isNumber() ? a[3].asNumber() : 0, a[4].isBool() && a[4].getBool());
            return jsi::Value::undefined();
        }));
    // __vibeSpecSockCounts(id) → {frames, bytes, ms}: the link meter's counts since the last call (SpecSocketSink::takeCounts)
    rt.global().setProperty(rt, "__vibeSpecSockCounts", jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forAscii(rt, "__vibeSpecSockCounts"), 1,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value* a, size_t c) -> jsi::Value {
            auto sink = c >= 1 && a[0].isNumber() ? sinkFor((int)a[0].asNumber()) : nullptr;
            if (!sink) return jsi::Value::undefined();
            uint32_t frames; uint64_t bytes; double ms;
            sink->takeCounts(frames, bytes, ms);
            jsi::Object o(r);
            o.setProperty(r, "frames", (double)frames);
            o.setProperty(r, "bytes", (double)bytes);
            o.setProperty(r, "ms", ms);
            return o;
        }));
    rt.global().setProperty(rt, "__vibeSpectrumCreate", jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forAscii(rt, "__vibeSpectrumCreate"), 0,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            return jsi::Object::createFromHostObject(r, std::make_shared<SpectrumHost>());
        }));
}

#if defined(__ANDROID__)
// ── VibeSpecSocket.kt → the sinks (OkHttp's reader thread). Each returns TRUE when JS should be told to drain. ──
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeSinkCreate(JNIEnv*, jclass, jint id, jboolean vibeServer) {
    std::lock_guard<std::mutex> lk(gSinkMx);
    gSinks[id] = std::make_shared<vibespec::SpecSocketSink>(vibeServer == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeSinkDestroy(JNIEnv*, jclass, jint id) {
    std::lock_guard<std::mutex> lk(gSinkMx);
    gSinks.erase(id);
}
// ★ Text arrives as real UTF-8 BYTES (Kotlin's toByteArray): GetStringUTFChars hands back Java's MODIFIED UTF-8,
//   which encodes emoji and other 4-byte characters differently — a station name or chat line would arrive garbled.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeSinkText(JNIEnv* env, jclass, jint id, jbyteArray utf8) {
    auto sink = sinkFor(id);
    if (!sink || !utf8) return JNI_FALSE;
    const jsize n = env->GetArrayLength(utf8);
    jbyte* p = env->GetByteArrayElements(utf8, nullptr);
    const bool notify = sink->onText(reinterpret_cast<const char*>(p), (size_t)n);
    env->ReleaseByteArrayElements(utf8, p, JNI_ABORT);
    return notify ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeSinkBinary(JNIEnv* env, jclass, jint id, jbyteArray data) {
    auto sink = sinkFor(id);
    if (!sink || !data) return JNI_FALSE;
    const jsize n = env->GetArrayLength(data);
    jbyte* p = env->GetByteArrayElements(data, nullptr);
    const bool notify = sink->onBinary(reinterpret_cast<const uint8_t*>(p), (size_t)n);
    env->ReleaseByteArrayElements(data, p, JNI_ABORT);
    return notify ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeInstall(JNIEnv*, jclass, jlong runtimePtr) {
    auto* rt = reinterpret_cast<jsi::Runtime*>(runtimePtr);
    if (!rt) return JNI_FALSE;
    vibeSpectrumInstall(*rt);
    return JNI_TRUE;
}
#endif
