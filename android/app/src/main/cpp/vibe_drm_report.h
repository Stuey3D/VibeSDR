// vibe_drm_report.h — THE DRM REPORT, {"type":"drm"}: what one listener's DRM receiver says, as the JSON the spectrum socket
// carries (docs/v12/DRM.md "The report"). Split out of vibe_drm_listener.h (2026-10-26) so the wire format is tested on
// its own (vibeserver/test-drm-report.cpp) — no thread, no codec, no socket.
//
// ★★ BACKWARD COMPATIBLE: every field an older client reads keeps its name, type and meaning. The Signal pane's fields
//    (V12, 2026-10-26) are NEW and OPTIONAL — a client that does not know them ignores them, and a field this receiver has
//    not measured yet is simply absent (never a placeholder number a client could mistake for a measurement).
// ★ Words where the standard has words; the numbers a DXer reads (MER, mode, QAM, rate) as numbers.
#pragma once
#include "decoders/drm/drm_receiver.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace vibe {

/** JSON string escape for labels and text that came off the air. */
inline std::string drmJsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

/** What the LISTENER knows beyond the receiver: its decoder, its audio counters, its queue. */
struct DrmReportExtras {
    bool canDecode = false;           // DrmAudioDecoder::canDecode for this station's codec
    bool decoderOpen = false;
    bool decoderTried = false;
    std::string why;                  // canDecode's sentence (when it cannot), else the decoder's own (when it failed to open)
    std::string backend;              // the decoder playing it ("FAAD2", "AudioToolbox", …) while open
    int64_t audioUnits = 0, audioBad = 0, concealed = 0, underruns = 0;
    uint64_t dropped = 0;             // IQ blocks the listener's queue dropped (the decoder thread fell behind)
    double blanked = 0;               // fraction of samples the pulse blanker zeroed
};

inline std::string drmReportJson(const vibedrm::RxStatus& s, const DrmReportExtras& x) {
    std::string j = "{\"type\":\"drm\"";
    auto b = [&](const char* k, bool v) { j += std::string(",\"") + k + "\":" + (v ? "true" : "false"); };
    char buf[512];
    b("sync", s.timeSync); b("freq", s.freqSync); b("frame", s.frameSync); b("fac", s.facOk); b("sdc", s.sdcOk); b("msc", s.mscOk);
    b("locked", s.locked);
    if (s.mode != vibedrm::Mode::None) { j += ",\"robustness\":\""; j += vibedrm::modeLetter(s.mode); j += "\""; }
    if (s.locked) {
        static const double bwKhz[6] = { 4.5, 5, 9, 10, 18, 20 };
        std::snprintf(buf, sizeof buf, ",\"bwKhz\":%g,\"mscQam\":%d,\"sdcQam\":%d,\"interleave\":\"%s\",\"codeRate\":%.2f,"
                      "\"snr\":%.1f,\"mscMer\":%.1f,\"offsetHz\":%.1f,\"clockPpm\":%.1f",
                      s.so >= 0 && s.so < 6 ? bwKhz[s.so] : 0.0, 1 << (2 * s.mscLevels), 1 << (2 * s.sdcLevels),
                      s.longInterleave ? "long" : "short", s.codeRateB, s.facMerDb, s.mscMerDb, s.freqOffsetHz, s.sroPpm);
        j += buf;
        // ── ★★ V12 the Signal pane: the signal's own parameters ──
        if (s.so >= 0) { std::snprintf(buf, sizeof buf, ",\"so\":%d", s.so); j += buf; }
        if (s.codeRateA > 0) { std::snprintf(buf, sizeof buf, ",\"codeRateA\":%.2f", s.codeRateA); j += buf; }
        if (s.sdcRate > 0) { std::snprintf(buf, sizeof buf, ",\"sdcRate\":%.2f", s.sdcRate); j += buf; }
        if (s.sdcMerDb != 0) { std::snprintf(buf, sizeof buf, ",\"sdcMer\":%.1f", double(s.sdcMerDb)); j += buf; }
        if (s.delayMs >= 0) { std::snprintf(buf, sizeof buf, ",\"delayMs\":%.2f", double(s.delayMs)); j += buf; }
        if (s.dopplerHz >= 0) { std::snprintf(buf, sizeof buf, ",\"dopplerHz\":%.2f", double(s.dopplerHz)); j += buf; }
        if (s.fac.serviceId) { std::snprintf(buf, sizeof buf, ",\"serviceId\":\"%06X\"", unsigned(s.fac.serviceId)); j += buf; }
        const vibedrm::ServiceSdc& sv = s.sdc.svc[size_t(s.fac.shortId & 3)];
        if (!sv.label.empty()) j += ",\"label\":" + drmJsonStr(sv.label);
        const char* lang = vibedrm::languageName(s.fac.language);
        if (*lang) j += ",\"language\":" + drmJsonStr(lang);
        if (!sv.lang3.empty()) j += ",\"lang3\":" + drmJsonStr(sv.lang3);
        if (!sv.country2.empty()) j += ",\"country\":" + drmJsonStr(sv.country2);
        if (!s.fac.isData) { const char* pty = vibedrm::programmeTypeName(s.fac.descriptor); if (*pty) j += ",\"programme\":" + drmJsonStr(pty); }
        if (!s.text.empty()) j += ",\"text\":" + drmJsonStr(s.text);
        if (s.sdc.haveTime) { std::snprintf(buf, sizeof buf, ",\"utc\":\"%02d:%02d\"", s.sdc.hour, s.sdc.minute); j += buf; }
        if (sv.audio.present) {
            j += ",\"codec\":" + drmJsonStr(sv.audio.codecName()) + ",\"audioMode\":" + drmJsonStr(sv.audio.modeName());
            std::snprintf(buf, sizeof buf, ",\"audioRate\":%d,\"bitrate\":%d", sv.audio.sampleRateHz(), s.bitrateBps); j += buf;
            // ★ SBR and parametric stereo are AAC's (the SDC's flags, §6.4.3.10); xHE-AAC carries its own tools in its config
            if (sv.audio.coding == 0) { b("sbr", sv.audio.sbr != 0); b("ps", sv.audio.audioMode == 1); }
            b("playable", x.canDecode && x.decoderOpen);
            if (!x.canDecode) j += ",\"why\":" + drmJsonStr(x.why);
            else if (!x.decoderOpen && x.decoderTried) j += ",\"why\":" + drmJsonStr(x.why);
            if (x.decoderOpen) j += ",\"decoder\":" + drmJsonStr(x.backend);
            std::snprintf(buf, sizeof buf, ",\"audioUnits\":%lld,\"audioBad\":%lld,\"concealed\":%lld,\"underruns\":%lld",
                          (long long)x.audioUnits, (long long)x.audioBad, (long long)x.concealed, (long long)x.underruns);
            j += buf;
        }
        std::snprintf(buf, sizeof buf, ",\"sdcOk\":%lld,\"sdcBad\":%lld,\"superFrames\":%lld",
                      (long long)s.sdcGood, (long long)s.sdcBad, (long long)s.superFrames);
        j += buf;
    }
    // ── ★★ V12 the Signal pane's lock group: sent while searching too — it is how the pane shows a re-acquisition ──
    if (s.timeSync) {
        j += ",\"lockS\":[";
        for (int i = 0; i < 6; ++i) { std::snprintf(buf, sizeof buf, "%s%.1f", i ? "," : "", s.lockS[i] < 0 ? -1.0 : s.lockS[i]); j += buf; }
        j += "]";
    }
    std::snprintf(buf, sizeof buf, ",\"reacq\":%lld", (long long)s.reacquisitions); j += buf;
    if (!s.lastLoss.empty()) { j += ",\"lostWhy\":" + drmJsonStr(s.lastLoss); std::snprintf(buf, sizeof buf, ",\"lostAtS\":%.1f", s.lastLossAtS); j += buf; }
    if (x.blanked > 0) { std::snprintf(buf, sizeof buf, ",\"blankedPct\":%.2f", x.blanked * 100.0); j += buf; }
    std::snprintf(buf, sizeof buf, ",\"facOk\":%lld,\"facBad\":%lld,\"dropped\":%llu}", (long long)s.facGood, (long long)s.facBad,
                  (unsigned long long)x.dropped);
    j += buf;
    return j;
}

}  // namespace vibe
