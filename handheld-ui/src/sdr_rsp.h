// sdr_rsp.h — MENU → SDR HARDWARE on an SDRplay RSP (the web's #rspCtls; spec §4a.3, labels and help verbatim).
// ★ State: rspstat (5 Hz live telemetry: lna, ifgr, rfAgc, agcSet, notches, VibeAGC) and hwinfo (radio caps, rspBiasT).
//   The ONE setting neither reports is whether the IF AGC is switched on — that is kept in App::num_ ("sdr.rsp.ifagc",
//   the web's pref rsp_ifagc, default ON) and corrected from rspstat when VibeAGC is seen running (it only runs under
//   the IF AGC). See the report: the engine should add `ifAgc` to rspstat.
#pragma once
// (included by sec_sdr.h, after App is declared)

namespace hh {
namespace sdr {

// ── the AGC readout (src/services/rspAgcReadout.ts, ported: the same words, the same amber rules) ──────────────────
inline std::string vagcReasonText(int code) {
    switch (code) {
        case 1: return "the IF was at its 59 dB rail and the converter still hot";
        case 2: return "the converter was clipping with the IF nearly spent";
        case 3: return "the IF reduction sat over the front-end load line";
        case 4: return "the IF had room for a whole rung";
        case 11: return "the SDRplay API reported a device failure";
        case 12: return "the RSP service stopped answering";
        case 13: return "the driver rejected 3 gain writes";
        case 14: return "3 RF gain writes in a row were not honoured";
        default: return "";
    }
}
inline bool rspCause(const Json& m, bool ifAgc, std::string& tag, std::string& why) {
    if (!ifAgc) return false;
    if ((int)m["vst"].num(-1) == 5) {
        const std::string r = vagcReasonText((int)m["vrs"].num(0));
        tag = "AGC HANDED BACK";
        why = "VibeAGC handed back to the SDRplay's own IF AGC" + (r.empty() ? std::string() : " — " + r);
        return true;
    }
    if ((int)m["vagc"].num(0) == 1 && m["vblind"].num(0) >= 3) {
        tag = "NO ADC LEVEL"; why = fmtI("No fresh ADC level for %d s — the loop cannot see the signal", (int)m["vblind"].num(0)); return true;
    }
    const int rail = (int)m["vrail"].num(0);
    if (rail >= 10) { tag = "IF RAIL"; why = fmtI("The IF has sat at its 59 dB rail for %d s with RF gain still to cut", rail); return true; }
    if (-rail >= 10) { tag = "IF FLOOR"; why = fmtI("The IF has sat at its floor for %d s with RF gain still to add", -rail); return true; }
    return false;
}
inline std::string rspLine(const Json& m, bool ifAgc) {
    if (!ifAgc) return "Manual gain — no AGC is running.";
    const std::string peak = m["adcPeak"].kind == Json::Num && m["adcPeak"].n > -98 ? dbfs(m["adcPeak"].n) : "—";
    if ((int)m["vagc"].num(0) == 1) {
        const std::string r = vagcReasonText((int)m["vrs"].num(0));
        const int vst = (int)m["vst"].num(-1);
        const std::string st = vst == 1 ? "settling"
                             : vst == 3 ? "cutting RF" + (r.empty() ? std::string() : " (" + r + ")")
                             : vst == 4 ? "adding RF" + (r.empty() ? std::string() : " (" + r + ")")
                             : vst == 2 ? "holding" : "running";
        const std::string tgt = m["vtgt"].kind == Json::Num ? " · target " + dbfs(m["vtgt"].n) : "";
        std::string w;
        if (m["vwpm"].kind == Json::Num) { const int n = (int)m["vwpm"].n; w = fmtI(" · %d gain write", n) + (n == 1 ? "" : "s") + " in the last minute"; }
        const std::string miss = m["vmiss"].num(0) > 0 ? fmtI(" — %d did not land", (int)m["vmiss"].n) : "";
        return "VibeAGC: " + st + " · ADC " + peak + " dBFS" + tgt + w + miss;
    }
    const bool rf = !m.has("rfAgc") || m["rfAgc"].boolean();
    const std::string tgt = m["agcSet"].kind == Json::Num ? " · target " + dbfs(m["agcSet"].n) : "";
    return std::string("The SDRplay's own IF AGC") + (rf ? " + our RF walker" : "") + " · ADC " + peak + " dBFS" + tgt;
}
/** RspAgcWatch: amber at least 30 s once raised, and until the cause has been gone 10 s. ★ Fed only while the menu is
 *  drawn (the web feeds it on every rspstat), so a fault that came and went while the menu was shut is not latched. */
inline void rspWatch(S& x, const Json& m, bool ifAgc, bool& warn, std::string& tag, std::string& why) {
    auto& n = x.A.num_;
    const double t = x.now * 1000;
    std::string ct, cw;
    const bool c = rspCause(m, ifAgc, ct, cw);
    if (!ifAgc) { n["sdr.rsp.warnSince"] = -1; n["sdr.rsp.quietSince"] = -1; }
    else if (c) {
        if (n.count("sdr.rsp.warnSince") == 0 || n["sdr.rsp.warnSince"] < 0) n["sdr.rsp.warnSince"] = t;
        x.A.str_["sdr.rsp.tag"] = ct; x.A.str_["sdr.rsp.why"] = cw; n["sdr.rsp.quietSince"] = -1;
    } else if (n.count("sdr.rsp.warnSince") && n["sdr.rsp.warnSince"] >= 0) {
        if (n["sdr.rsp.quietSince"] < 0) n["sdr.rsp.quietSince"] = t;
        if (t - n["sdr.rsp.warnSince"] >= 30000 && t - n["sdr.rsp.quietSince"] >= 10000) { n["sdr.rsp.warnSince"] = -1; n["sdr.rsp.quietSince"] = -1; }
    }
    warn = n.count("sdr.rsp.warnSince") && n["sdr.rsp.warnSince"] >= 0 && !x.A.str_["sdr.rsp.tag"].empty();
    tag = warn ? x.A.str_["sdr.rsp.tag"] : ""; why = warn ? x.A.str_["sdr.rsp.why"] : "";
}

inline void rspSend(S& x, const char* key, long v) { sendKV(x, "rsp_control", key, v); }

inline void rspRows(S& x, float& y) {
    const Json& m = x.s.msg("rspstat");
    const Json& r = x.radio();
    const Json& h = x.hwi();
    const bool live = m.kind == Json::Obj;
    const bool restricted = h["gainLocked"].boolean();        // rspRestricted() on the handheld: the per-band gain lock
    const bool agcLocked = h["agcLocked"].boolean();
    const bool running = (int)m["vagc"].num(0) == 1;          // VibeAGC is driving the RSP now
    // the IF AGC switch (see the file comment): the pref, forced on by the owner's lock or by VibeAGC running
    auto& pref = x.A.num_["sdr.rsp.ifagc"];
    if (!x.A.num_.count("sdr.rsp.ifagcSet")) { pref = 1; x.A.num_["sdr.rsp.ifagcSet"] = 1; }
    if (running || agcLocked) pref = 1;
    const bool ifOn = pref > 0.5;
    const bool rfOn = held(x, "rsp.rfagc", m["rfAgc"].boolean() ? 1 : 0) > 0.5;
    const bool hasMode = r["vagcMode"].kind == Json::Num;

    // SYSTEM GAIN + OVERLOAD
    {
        const float y0 = y;
        textRow(x, y, "SYSTEM GAIN", "", live && m.has("sysGain") ? fmt("%.1f dB", m["sysGain"].num()) : "—");
        if (m["overload"].num(0) > 0) {
            const std::string t = "OVERLOAD";
            const float w = textW(x, x.c.f.small, t) + 16, vx = x.a.r() - 6 - textW(x, x.c.f.body, fmt("%.1f dB", m["sysGain"].num())) - 12 - w;
            x.c.g.rrect(vx, y0 + 10, w, 24, 6, th::red);
            textIn(x.c, x.c.f.small, t, {vx, y0 + 10, w, 24}, th::white);
        }
    }
    // the AGC status line
    if (live) {
        bool warn; std::string tag, why;
        rspWatch(x, m, ifOn, warn, tag, why);
        y = note(x, y - 4, rspLine(m, ifOn) + (why.empty() ? "" : "\n" + why), warn ? th::amber : th::dim);
    }

    // RF GAIN (LNA) — right = more gain: the wire's LNA state counts the other way
    const int lnaN = (int)(m["lnaN"].num(0) > 0 ? m["lnaN"].num() : r["lnaStates"].num(10));
    const int lnaMax = std::max(1, lnaN - 1);
    const int lnaState = (int)held(x, "rsp.lna", m.has("lna") ? m["lna"].num() : lnaMax - lnaMax / 2);
    if (!restricted) {
        const int st = std::max(0, std::min(lnaMax, lnaState));
        double pos = lnaMax - st;
        const bool agcDrives = ifOn && rfOn;
        std::string val = fmtI("%d/", (int)pos) + fmtI("%d · LNA ", lnaMax) + std::to_string(st)
                        + (st == 0 ? " · max" : st == lnaMax ? " · min" : "") + (agcDrives ? " · AGC" : "");
        const std::string help = agcDrives && running ? "Handled by VibeAGC — it keeps the RF gain as high as the ADC allows." : "";
        if (rowSlider(x, y, "sdr.rsp.lna", "RF GAIN (LNA)", help, pos, 0, lnaMax, 1, val, !agcDrives, 190)) {
            const int want = lnaMax - (int)pos;
            hold(x, "rsp.lna", want);
            sendKVSlider(x, "rsp_control", "lna", want);
        }
    }

    // IF GAIN RED.
    {
        const int floorDb = (int)h["ifGrFloor"].num(-1);
        const int lo = std::max((int)r["ifGrMin"].num(20), floorDb >= 20 ? floorDb : 0), hi = (int)r["ifGrMax"].num(59);
        double gr = held(x, "rsp.ifgr", m.has("ifgr") ? m["ifgr"].num() : 40);
        gr = std::max<double>(lo, std::min<double>(hi, gr));
        const int g = (int)std::lround(gr);
        const std::string val = fmtI("%d dB", g) + (g <= 20 ? " · max gain" : g >= 59 ? " · min gain" : "")
                              + (ifOn ? (running ? " · VibeAGC" : " · IF AGC") : "");
        std::string help = agcLocked ? "Locked: the owner has locked the IF AGC on, and the AGC owns this gain."
                         : running && ifOn ? "Handled by VibeAGC — it sets the IF gain reduction from the measured ADC level."
                         : "IF gain reduction, in dB. Available only with the IF AGC off.";
        if (floorDb >= 20) help += fmtI("\nThe owner requires at least %d dB of IF gain reduction on this band", floorDb);
        if (rowSlider(x, y, "sdr.rsp.ifgr", "IF GAIN RED.", help, gr, lo, hi, 1, val, !ifOn, 190)) {
            hold(x, "rsp.ifgr", gr);
            sendKVSlider(x, "rsp_control", "ifgr", (long)gr);
        }
    }

    // AGC — the owner's choice of loop (the handheld is the owner: adminGate passes on loopback)
    if (hasMode) {
        const int mode = (int)held(x, "rsp.vagc", r["vagcMode"].num());
        static const char* desc[3] = {
            "VibeAGC with the OpenRSP driver, the SDRplay's own IF AGC with SDRplay's official API",
            "VibeAGC: VibeSDR's own gain control for both stages of the RSP",
            "The RSP's own IF AGC (in the SDRplay API), with the RF AGC row stepping the LNA"};
        std::string help = mode == 0 ? (r["vagcAuto"].boolean() ? "→ VibeAGC (OpenRSP driver)" : "→ SDRplay's own (official API)") : "";
        if (mode >= 0 && mode <= 2) help += (help.empty() ? "" : "\n") + std::string(desc[mode]);
        const int hit = rowSeg(x, y, "sdr.rsp.vagc", "AGC", help, {"AUTO", "VIBEAGC", "SDRPLAY"}, mode);
        if (hit >= 0 && hit != mode) { hold(x, "rsp.vagc", hit); rspSend(x, "vagcmode", hit); }
    }

    if (!restricted) {
        // IF AGC
        {
            const std::string by = ifOn && (running || hasMode) ? (running ? " · VIBEAGC" : " · SDRPLAY") : "";
            const std::string help = agcLocked ? "The owner has locked the IF AGC on for this receiver, so the manual IF gain is not available."
                : running ? "AUTO: VibeAGC runs the IF (and the RF, with RF AGC on). Tap for MANUAL gain — that stops VibeAGC."
                : "AUTO: the SDRplay's own IF AGC. Tap for MANUAL gain.";
            if (rowToggle(x, y, "sdr.rsp.ifagc", "IF AGC" + by, help, ifOn, "AUTO", "AUTO")) {
                if (ifOn && agcLocked) x.A.toast("The owner has locked the IF AGC on");
                else {
                    pref = ifOn ? 0 : 1;
                    rspSend(x, "ifagc", ifOn ? 0 : 1);
                    if (ifOn) rspSend(x, "ifgr", std::lround(held(x, "rsp.ifgr", m["ifgr"].num(40))));
                }
            }
        }
        // RF AGC
        {
            const std::string by = ifOn && rfOn && running ? " · VIBEAGC" : "";
            if (rowToggle(x, y, "sdr.rsp.rfagc", "RF AGC" + by, "", rfOn, "AUTO", "AUTO")) {
                hold(x, "rsp.rfagc", rfOn ? 0 : 1);
                rspSend(x, "rfagc", rfOn ? 0 : 1);
                if (rfOn) rspSend(x, "lna", std::max(0, std::min(lnaMax, lnaState)));
            }
        }
        // AGC TARGET — ★ no snap to −30 within ±2 (the web's is for a dragged thumb; with D-pad steps of 1 it would pin the
        //   slider at −30 for ever)
        if (!r.has("agcSetPoint") || r["agcSetPoint"].boolean()) {
            const bool vOwns = ifOn && running;
            double sp = held(x, "rsp.agcset", m.has("agcSet") ? m["agcSet"].num() : -30);
            sp = std::max(-72.0, std::min(-10.0, sp));
            const int spi = (int)std::lround(sp);
            const std::string val = vOwns
                ? (m["vtgt"].kind == Json::Num ? dbfs(m["vtgt"].n) + " dBFS · VibeAGC" : "VibeAGC")
                : fmtI("%d dBfs", spi) + (x.s.mode == "dab" && spi != -30 ? " · DAB" : spi == -30 ? " · default" : spi >= -25 ? " · hard" : spi <= -60 ? " · gentle" : "");
            const std::string help = vOwns
                ? "VibeAGC's own target (the ADC peak it holds), set per band by VibeAGC — not adjustable here. Choose SDRPLAY on the AGC row to steer the SDRplay's own AGC with this slider."
                : "";
            if (rowSlider(x, y, "sdr.rsp.agcset", std::string("AGC TARGET") + (vOwns ? " · VIBEAGC" : ""), help, sp, -72, -10, 1, val, !vOwns, 170)) {
                hold(x, "rsp.agcset", sp);
                sendKVSlider(x, "rsp_control", "agcset", (long)sp);
            }
        }
        // FILTERS
        {
            const bool autoNotch = m["autoNotch"].num(0) > 0, userNotch = !m.has("userNotch") || m["userNotch"].num(1) > 0;
            const bool owned = autoNotch || !userNotch;
            struct F { const char* key; const char* label; bool on; bool enabled; std::string help; };
            std::vector<F> fs;
            const std::string ownedWhy = autoNotch
                ? "Set automatically from the frequency being received — the receiver keeps the notches clear of the band you are on. Not a manual control while this is on."
                : "This receiver's owner keeps the notches for themselves — they are part of the front end, so one listener changing them changes them for everybody.";
            if (r["rfNotch"].boolean())
                fs.push_back({"rfnotch", "RF NOTCH", held(x, "rsp.rfnotch", m["rfNotch"].num(h["rfNotch"].boolean() ? 1 : 0)) > 0.5, !owned,
                              owned ? ownedWhy : "Broadcast notch — covers MW and FM. Wanted ON elsewhere in the spectrum, where a strong local broadcast transmitter is what overloads the front end, and OFF when MW or FM is what you came to hear"});
            if (r["dabNotch"].boolean())
                fs.push_back({"dabnotch", "DAB NOTCH", held(x, "rsp.dabnotch", m["dabNotch"].num(h["dabNotch"].boolean() ? 1 : 0)) > 0.5, !owned,
                              owned ? ownedWhy : "DAB band notch, same reasoning as the FM notch"});
            if (r["biasT"].boolean()) {
                // ★ the RSP's OWN bias-tee: hwinfo `rspBiasT` (the persisted choice), else radio.biasTOn
                const bool on = h.has("rspBiasT") ? h["rspBiasT"].boolean() : r["biasTOn"].boolean();
                fs.push_back({"biast", "BIAS-T", held(x, "rsp.biast", on ? 1 : 0) > 0.5, true, "Bias-T — powers an active antenna or a mast-head amplifier"});
            }
            if (r["amNotch"].boolean())
                fs.push_back({"amnotch", "AM NOTCH", held(x, "rsp.amnotch", r["amNotchOn"].boolean() ? 1 : 0) > 0.5, true, "MW notch — cuts the medium-wave broadcast band, separate from the RF notch"});
            if (r["hdr"].boolean())
                fs.push_back({"hdr", "HDR", held(x, "rsp.hdr", r["hdrOn"].boolean() ? 1 : 0) > 0.5, true, "High dynamic range — the dx family's path below 2 MHz, where a strong medium-wave signal is what overloads the front end"});
            if (r["extRef"].boolean())
                fs.push_back({"extref", "24 MHz REF", held(x, "rsp.extref", r["extRefOn"].boolean() ? 1 : 0) > 0.5, true, "24 MHz reference output — for locking another receiver to this one's clock"});
            if (!fs.empty()) {
                y = x.A.menuRow(x.c, x.a, y, std::string("FILTERS") + (owned ? (autoNotch ? " · AUTO" : " · OPERATOR") : ""), "");
                std::vector<std::string> labels; std::vector<bool> lit, en;
                for (auto& f : fs) { labels.push_back(f.label); lit.push_back(f.on); en.push_back(true); }
                int foc;
                const int hit = buttonGrid(x, y, "sdr.rsp.f", labels, lit, en, foc, 3);
                if (hit >= 0) {
                    F& f = fs[(size_t)hit];
                    if (!f.enabled) x.A.toast(autoNotch ? "The notches are on automatic" : "The owner has reserved the notch filters");
                    else { hold(x, std::string("rsp.") + f.key, f.on ? 0 : 1); rspSend(x, f.key, f.on ? 0 : 1); }
                }
                // ★ the highlighted filter's explanation (the web's title — the R36S has no hover), under the group
                y = note(x, y - 2, foc >= 0 ? fs[(size_t)foc].help : (owned ? ownedWhy : ""));
            }
        }
    }
    // the frozen-gain-API chip lives on the main screen on the web; here it is a row while rspstat says so
    if (m["gainStuck"].num(0) > 0) {
        y = note(x, y, "SDRPLAY GAIN API FAILURE · TAP TO RESET (MAY CAUSE AN AUDIO DROP)", th::amber);
        BtnStyle st; st.font = &x.c.f.btn;
        if (button(x.c, "sdr.rsp.restart", {x.a.x + 6, y, x.a.w - 12, 44}, "RESET THE GAIN API", st)) send(x, "{\"type\":\"rsp_agc_restart\"}");
        y += 56;
    }
}

}  // namespace sdr
}  // namespace hh
