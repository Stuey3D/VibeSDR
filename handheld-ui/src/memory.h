// memory.h — PER-RADIO MEMORY for the native UI (the web handheld page's, handheld.ts + docs/v12/R36S-NATIVE-MENU-INVENTORY.md
// §8): where the radio was and how it was set up, saved to the engine (POST /handheld/memory {"serial","prefs"}) and put
// back when THAT radio (its serial) is plugged in again.
//
// ★★★ WHY (Stuart, 2026-11-07): "we do need to remember where the SDR was previously set so that those with RTL's with
//     sensitive ports dont have to keep setting things up and tuning again if they knock it offline." The engine starts a
//     replugged radio at its default (9.410 MHz AM); the web page restored its prefs blob, the native UI did nothing.
// ★ Kept under prefs.hhui so it never collides with the web page's own keys (the vibe.browser fallback reads the same
//   blob). The engine writes it to $DATA/handheld/radio-<serial>.json (every ~30 s, on unplug, on power-off).
// ★ The front-end settings (gain / AGC, rate, IF filter, PPM, bias-T, direct sampling) are restored only on the SAME
//   driver they were saved from — the engine's rows mean different things on different radios (AGENTS.md).
#pragma once

namespace hh {

struct MemState {
    std::string restoredSerial;     // the radio whose memory has been applied (or found empty) this plug-in
    double restoredAt = 0, lastCheck = 0;
    std::string lastBody;
};
inline MemState& memState() { static MemState m; return m; }

inline std::string memNum(double v) { char b[40]; std::snprintf(b, sizeof b, "%.10g", v); return b; }

/** The radio's settings as the UI sees them now (the engine's reports, not guesses). */
inline std::string memBody(App& a, const RadioState& s) {
    const Json& h = s.msg("hwinfo");
    std::string j = "{\"v\":1,\"driver\":" + jsonQuote(s.hwDriver);
    if (!dabActive() && s.vfoHz > 0) {      // ★ in DAB the dial is the block, not a frequency to come back to
        j += ",\"f\":" + memNum(s.vfoHz) + ",\"mode\":" + jsonQuote(s.mode) + ",\"lo\":" + std::to_string(s.bwLow)
           + ",\"hi\":" + std::to_string(s.bwHigh);
        if (s.spanHz > 0) j += ",\"span\":" + memNum(s.spanHz);
    }
    j += ",\"step\":" + std::to_string(a.set_.step);
    if (s.hwDriver == "rtl") {
        j += ",\"gain\":" + std::to_string(s.gainNow) + ",\"agc\":" + (s.agc ? "true" : "false");
        if (s.hw("rateNow").num(0) > 0) j += ",\"rate\":" + memNum(s.hw("rateNow").num(0));
        if (h.has("biasT")) j += std::string(",\"biasT\":") + (h["biasT"].boolean() ? "true" : "false");
        if (s.hw("ppm").kind == Json::Num) j += ",\"ppm\":" + memNum(s.hw("ppm").num(0));
        if (s.hw("ds").kind == Json::Num) j += ",\"ds\":" + memNum(s.hw("ds").num(0));
        if (s.hw("tunerBw").kind == Json::Num || s.hw("tunerBwAuto").kind == Json::Bool)
            j += ",\"ifbw\":" + memNum(s.hw("tunerBwAuto").boolean() ? -1 : s.hw("tunerBw").num(0));
    }
    {
        const auto& S = aud::st();
        if (a.num_.count("audio.sqlDb")) j += ",\"sql\":" + memNum(a.num_["audio.sqlDb"]);
        j += ",\"vol\":" + memNum(S.vol);
    }
    return j + "}";
}

/** Put a radio's saved settings back (once per plug-in, after its first config + hwinfo). */
inline void memRestore(App& a, const RadioState& s, const Json& m) {
    Engine& e = *a.eng_;
    const bool sameDriver = m["driver"].str() == s.hwDriver;
    if (sameDriver && s.hwDriver == "rtl") {
        // the front end first (a rate change restarts the stream), then the dial
        if (m.has("rate") && std::fabs(m["rate"].num(0) - s.hw("rateNow").num(0)) > 1)
            e.send("{\"type\":\"sampleRate\",\"value\":" + memNum(std::round(m["rate"].num(0))) + "}");
        if (m.has("ds")) e.send("{\"type\":\"directSampling\",\"value\":" + memNum(m["ds"].num(0) > 0 ? 2 : 0) + "}");
        if (m["agc"].boolean()) e.send("{\"type\":\"gain\",\"auto\":true}");
        else if (m.has("gain")) e.gain((int)m["gain"].num(0));
        if (m.has("biasT")) e.send(std::string("{\"type\":\"biasT\",\"on\":") + (m["biasT"].boolean() ? "true" : "false") + "}");
        if (m.has("ppm")) e.send("{\"type\":\"ppm\",\"value\":" + memNum(m["ppm"].num(0)) + "}");
        if (m.has("ifbw")) e.send("{\"type\":\"tunerbw\",\"value\":" + memNum(m["ifbw"].num(0)) + "}");
    }
    if (m.has("step") && m["step"].num(0) > 0) a.set_.step = (int)m["step"].num(0);
    if (m.has("f") && m["f"].num(0) > 0) {
        // ★ the frequency FIRST: setMode sends a tune at the dial it knows, which would visit the default 9.410 on the way
        const std::string mode = m["mode"].str();
        const double f = m["f"].num(0);
        e.tune(f);
        if (!mode.empty() && mode != s.mode) e.setMode(mode);
        if (m.has("lo") && m.has("hi")) e.setBandwidth((int)m["lo"].num(0), (int)m["hi"].num(0));
        if (m.has("span") && m["span"].num(0) > 0) e.zoom(f, m["span"].num(0));
        a.wfp_.setView(f, m.has("span") ? m["span"].num(0) : s.spanHz);
        a.wfp_.notifyInteraction(nowSec());
    }
    if (m.has("sql")) aud::applySquelch(a, m["sql"].num(aud::SQL_OFF), true);
    if (m.has("vol")) { aud::st().vol = m["vol"].num(80); aud::sendLevel(a); }
    char b[80];
    std::snprintf(b, sizeof b, "%s — back where you left it", s.radioName.empty() ? "Radio" : s.radioName.c_str());
    a.toast(b);
}

/** Once a frame: restore on a plug-in, then save what changed (every 2 s, only when the settings differ). */
inline void memTick(App& a, const RadioState& s) {
    MemState& M = memState();
    const double t = nowSec();
    if (s.session != "running" || s.radioSerial.empty()) { M.restoredSerial.clear(); M.lastBody.clear(); return; }
    if (M.restoredSerial != s.radioSerial) {
        // ★ wait for the radio's own first reports, so the restore is measured against what it really runs
        if (!s.connected || s.configSeq == 0 || !s.last.count("hwinfo")) return;
        M.restoredSerial = s.radioSerial;
        M.restoredAt = t;
        M.lastBody.clear();
        const Json& m = s.memory["hhui"];
        if (m.kind == Json::Obj) memRestore(a, s, m);
        return;
    }
    // ★ hold the saves for a moment after a restore: the radio answers each setting in turn, and saving its in-between
    //   state would overwrite the memory with half of it
    if (t - M.restoredAt < 4 || t - M.lastCheck < 2) return;
    M.lastCheck = t;
    const std::string body = memBody(a, s);
    if (body == M.lastBody) return;
    M.lastBody = body;
    const std::string post = "{\"serial\":" + jsonQuote(s.radioSerial) + ",\"prefs\":{\"hhui\":" + body + "}}";
    std::thread([p = a.eng_->port(), post] { httpRequest(p, "POST", "/handheld/memory", post, 5); }).detach();
}

}  // namespace hh
