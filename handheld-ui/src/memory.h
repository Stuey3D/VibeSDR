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
    if (!dabActive() && s.vfoHz > 0) {
        j += ",\"f\":" + memNum(s.vfoHz) + ",\"mode\":" + jsonQuote(s.mode) + ",\"lo\":" + std::to_string(s.bwLow)
           + ",\"hi\":" + std::to_string(s.bwHigh);
        if (s.spanHz > 0) j += ",\"span\":" + memNum(s.spanHz);
    } else if (dabActive()) {
        // ★★★ IN DAB: the multiplex + service, AND the station before DAB (Stuart, 2026-11-07: "when powering down the
        //     unit it needs to remember the last station and settings rather than coming back to 9410"). This wrote
        //     no dial at all in DAB — a few seconds in, the memory lost its station, and a power-off in DAB came back at
        //     the engine's 9.410 MHz. The DAB block is not a frequency to tune; the pre-DAB pair is where leaving DAB
        //     goes (dabLeave), so it is kept as the dial.
        const DabUi& u = dabUi();
        if (u.prevHz > 0 && !u.prevMode.empty()) j += ",\"f\":" + memNum(u.prevHz) + ",\"mode\":" + jsonQuote(u.prevMode);
        const int blk = dabShownBlock();
        if (blk >= 0 && blk < kDabBlockN) {
            j += ",\"dab\":{\"block\":" + jsonQuote(kDabBlocks[blk].name);
            const double sid = s.msg("dab")["sid"].num(0);
            if (sid > 0 && dabReport(s)) j += ",\"sid\":" + memNum(sid);
            j += "}";
        }
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
    // ★★ THE AUDIO MENU'S OWN DSP — NOISE REDUCTION (+ strength), AUTO NOTCH, NOISE BLANKER (Stuart, 2026-11-12, on
    //    the R36S: "I prefer it on so I would want this to be remembered setting"). Listener settings with no owner
    //    default on the server, exactly like the squelch below, so they live here with it. The radio's REPORT, never
    //    the switch's drawing. (The Broadcast FM row is different: it has owner defaults, and on the handheld the engine
    //    keeps the user's change AS them — vibe_fm_defaults.h adoptAsDefaults — so it is not restored from here.)
    if (h.has("nr")) {
        j += std::string(",\"nr\":") + (h["nr"].boolean() ? "true" : "false");
        if (h["nrStrength"].kind == Json::Num && h["nrStrength"].num(-1) >= 0) j += ",\"nrStr\":" + memNum(h["nrStrength"].num(0));
    }
    if (h.has("notch")) j += std::string(",\"notch\":") + (h["notch"].boolean() ? "true" : "false");
    if (h.has("nbx")) j += std::string(",\"nbx\":") + (h["nbx"].boolean() ? "true" : "false");
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
    const int dabBlk = m["dab"].kind == Json::Obj ? dabBlockIndex(m["dab"]["block"].str()) : -1;
    if (dabBlk >= 0) {
        // ★ back into DAB on that multiplex + service; the saved dial is where leaving DAB will go (not tuned now — it
        //   would only be a visit on the way)
        dabEnter(a, s, dabBlk, m["dab"]["sid"].num(0));
        DabUi& u = dabUi();
        if (m.has("f") && m["f"].num(0) > 0 && !m["mode"].str().empty()) { u.prevHz = m["f"].num(0); u.prevMode = m["mode"].str(); }
    } else if (m.has("f") && m["f"].num(0) > 0) {
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
    {   // ★ the audio menu's DSP (see memBody) — only what differs from what the radio reports, and only what it offers
        const Json& h = s.msg("hwinfo");
        auto jb = [](bool v) { return v ? "true" : "false"; };
        if (m.has("nr") && h.has("nr")) {
            const bool on = m["nr"].boolean();
            const double str = m.has("nrStr") ? m["nrStr"].num(-1) : -1;
            const double now = h["nrStrength"].kind == Json::Num ? h["nrStrength"].num(-1) : -1;
            if (on != h["nr"].boolean() || (str >= 0 && std::fabs(str - now) > 1e-3)) {
                std::string b = std::string("{\"type\":\"nr\",\"on\":") + jb(on);
                if (str >= 0) b += ",\"strength\":" + memNum(str);
                e.send(b + "}");
            }
        }
        if (m.has("notch") && h.has("notch") && m["notch"].boolean() != h["notch"].boolean())
            e.send(std::string("{\"type\":\"notch\",\"on\":") + jb(m["notch"].boolean()) + "}");
        if (m.has("nbx") && h.has("nbx") && m["nbx"].boolean() != h["nbx"].boolean())
            e.send(std::string("{\"type\":\"nbx\",\"on\":") + jb(m["nbx"].boolean()) + "}");
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
