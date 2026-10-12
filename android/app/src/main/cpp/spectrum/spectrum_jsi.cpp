// VibeSDR — the SpectrumEngine as a JSI host object (docs/NATIVE-SPECTRUM-ENGINE.md, stage 1).
//
// ONE object, reachable from BOTH runtimes: WaterfallView creates it on the JS thread
// (global.__vibeSpectrumCreate()) and a worklet captures it, so the same C++ engine is called from JS (push, state)
// and from the UI thread (poll). Every method makes its jsi values in the runtime that called it.
// ★ Images and paths come from the vibe_rnskia exports of the patched react-native-skia: made on the engine thread,
//   wrapped here on the UI runtime WITHOUT external memory pressure (cheap dispose — the worklet disposes the
//   previous one itself).
#include "spectrum_engine.h"
#include <jsi/jsi.h>
#include <react-native-skia/VibeExport.h>
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

class SpectrumHost : public jsi::HostObject {
public:
    SpectrumHost() : engine_(std::make_shared<SpectrumEngine>(mkImage, mkPath, rlImage, rlPath)) {}

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
        if (name == "push") return fn(5, [](jsi::Runtime& r, SpectrumEngine& en, const jsi::Value* a, size_t c) -> jsi::Value {
            // push(bins: Float32Array, centerHz, bwHz, trueCenterHz | undefined, lastInteractAt)
            if (c < 3 || !a[0].isObject()) return jsi::Value::undefined();
            const jsi::Object arr = a[0].asObject(r);
            const jsi::ArrayBuffer buf = arr.getProperty(r, "buffer").asObject(r).getArrayBuffer(r);
            const size_t off = (size_t)arr.getProperty(r, "byteOffset").asNumber();
            const size_t len = (size_t)arr.getProperty(r, "length").asNumber();
            if (off + len * sizeof(float) > buf.size(r)) return jsi::Value::undefined();
            const float* bins = reinterpret_cast<const float*>(buf.data(r) + off);
            const double tc = (c > 3 && a[3].isNumber()) ? a[3].asNumber() : NAN;
            if (c > 4 && a[4].isNumber()) en.setLastInteractAt(a[4].asNumber());   // read at arrival, as enqueueFrame did
            en.pushFrame(bins, (int)len, a[1].asNumber(), a[2].asNumber(), tc);
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
            o.setProperty(r, "scrollGen", (double)u.scrollGen);
            o.setProperty(r, "glide", u.glide);
            o.setProperty(r, "glideDur", u.glideDur);
            o.setProperty(r, "specGen", (double)u.specGen);
            o.setProperty(r, "specHidden", u.specHidden);
            if (!u.specTarget.empty()) {
                jsi::Array arr(r, u.specTarget.size());
                for (size_t i = 0; i < u.specTarget.size(); i++) arr.setValueAtIndex(r, i, (double)u.specTarget[i]);
                o.setProperty(r, "spec", arr);
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

/** Installs global.__vibeSpectrumCreate() on the JS runtime. Call on the JS thread. */
void vibeSpectrumInstall(jsi::Runtime& rt) {
    rt.global().setProperty(rt, "__vibeSpectrumCreate", jsi::Function::createFromHostFunction(
        rt, jsi::PropNameID::forAscii(rt, "__vibeSpectrumCreate"), 0,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
            return jsi::Object::createFromHostObject(r, std::make_shared<SpectrumHost>());
        }));
}

#if defined(__ANDROID__)
extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibesdr_app_VibeSpectrum_nativeInstall(JNIEnv*, jclass, jlong runtimePtr) {
    auto* rt = reinterpret_cast<jsi::Runtime*>(runtimePtr);
    if (!rt) return JNI_FALSE;
    vibeSpectrumInstall(*rt);
    return JNI_TRUE;
}
#endif
