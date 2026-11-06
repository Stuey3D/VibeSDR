// sdr_more.h — MENU → SDR HARDWARE on the Airspy HF+ (§4a.4), Airspy R2 / Mini (§4a.5), HackRF One (§4a.6) and USRP
// B2xx (§4a.7). Labels and help verbatim from docs/v12/R36S-NATIVE-MENU-INVENTORY.md (the web's titles become the
// help text: the R36S has no hover).
#pragma once
// (included by sec_sdr.h, after App is declared)

namespace hh {
namespace sdr {

// ── Airspy HF+ ──────────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ THE HF+ REPORTS NONE OF ITS SETTINGS. The engine holds them (g_dsp.ahfAgc/ahfAtt/ahfLna/ahfAgcHigh/ahfPpb) but no
//    message carries them back, so a client can only know them by setting them. The web asserts its saved prefs on
//    every caps message; here the four operating settings (AGC, threshold, attenuation, preamp) are asserted ONCE per
//    radio with the web's defaults, so what the rows show is what the radio runs. CALIBRATION is NOT asserted: it is
//    persisted by the engine, owner-only, and a 0 pushed from here would wipe the owner's correction — its row reads
//    "not reported" until it is moved. If the engine ever reports them (radio.ahfAgc / ahfThresh / ahfAtt / ahfLna /
//    ahfPpb — asked for in the report), the rows adopt those and nothing is asserted.
inline void ahfRows(S& x, float& y) {
    const Json& r = x.radio();
    auto& n = x.A.num_;
    const bool reports = r.has("ahfAgc");
    const std::string who = x.s.radioSerial + "|" + r["model"].str();
    if (!n.count("sdr.ahf.agc")) { n["sdr.ahf.agc"] = 1; n["sdr.ahf.thresh"] = 0; n["sdr.ahf.att"] = 0; n["sdr.ahf.lna"] = 0; }
    if (reports) {
        n["sdr.ahf.agc"] = r["ahfAgc"].num(1); n["sdr.ahf.thresh"] = r["ahfThresh"].num(0);
        n["sdr.ahf.att"] = r["ahfAtt"].num(0); n["sdr.ahf.lna"] = r["ahfLna"].num(0);
        if (r.has("ahfPpb")) n["sdr.ahf.ppb"] = r["ahfPpb"].num(0);
    } else if (x.A.str_["sdr.ahf.asserted"] != who) {
        x.A.str_["sdr.ahf.asserted"] = who;
        send(x, "{\"type\":\"ahf_control\",\"att\":" + std::to_string((int)n["sdr.ahf.att"]) + ",\"lna\":" + std::to_string((int)n["sdr.ahf.lna"])
                + ",\"thresh\":" + std::to_string((int)n["sdr.ahf.thresh"]) + "}");
        sendKV(x, "ahf_control", "agc", (long)n["sdr.ahf.agc"]);
    }
    const bool agc = n["sdr.ahf.agc"] > 0.5;
    // AGC
    if (rowToggle(x, y, "sdr.ahf.agc", "AGC", "", agc, "AUTO", "MANUAL")) {
        n["sdr.ahf.agc"] = agc ? 0 : 1;
        sendKV(x, "ahf_control", "agc", agc ? 0 : 1);
        if (agc) sendKV(x, "ahf_control", "att", (long)n["sdr.ahf.att"]);
    }
    // AGC THRESHOLD — only under AUTO
    {
        const int sel = (int)n["sdr.ahf.thresh"];
        std::string help = "Airspy: the threshold tells the AGC to tolerate an extra 3 dB of signal power before setting the next attenuation level. "
                           "HIGH = the front end is MORE sensitive, and is what they recommend when chasing marginal signals in the presence of very "
                           "strong blockers (~100 dB apart). LOW = less sensitive, engaging earlier.";
        help += sel ? "\nHIGH: Backs off later, keeping more gain. Better for weak stations; more overload risk."
                    : "\nLOW: Backs off earlier. Conservative — for a crowded band with strong local stations.";
        const int hit = rowSeg(x, y, "sdr.ahf.thresh", "AGC THRESHOLD", help, {"LOW", "HIGH"}, sel, agc);
        if (hit >= 0 && hit != sel) { n["sdr.ahf.thresh"] = hit; sendKV(x, "ahf_control", "thresh", hit); }
    }
    // ATTENUATION — only under MANUAL
    {
        const int steps = std::max(1, (int)r["attSteps"].num(8)), stepDb = (int)r["attStepDb"].num(6);
        double v = std::min<double>(n["sdr.ahf.att"], steps - 1);
        const std::string val = agc ? "set by AGC" : fmtI("%d dB", (int)v * stepDb) + (v == 0 ? " · none" : "");
        if (rowSlider(x, y, "sdr.ahf.att", "ATTENUATION",
                      "Airspy's own rule of thumb with the AGC off: push the attenuation until the noise floor sits around -100 dBFS. Less attenuation "
                      "than that does not necessarily improve SNR, but it definitely costs dynamic range. If in doubt, turn the AGC on and let it decide.",
                      v, 0, steps - 1, 1, val, !agc, 110)) {
            n["sdr.ahf.att"] = v;
            sendKV(x, "ahf_control", "att", (long)v);
        }
    }
    // PREAMP
    {
        const bool on = n["sdr.ahf.lna"] > 0.5;
        if (rowToggle(x, y, "sdr.ahf.lna", "PREAMP",
                      "Airspy calls this misleading: it is really a POSTAMP, acting on the ADC and DSP back-end rather than the antenna. Turning it OFF "
                      "buys about 12 dB of dynamic range by scrapping the quantisation-noise margin. Leave it off unless the band is genuinely quiet and "
                      "the signal is weak.", on)) {
            n["sdr.ahf.lna"] = on ? 0 : 1;
            sendKV(x, "ahf_control", "lna", on ? 0 : 1);
        }
    }
    // CALIBRATION — ±5000 ppb; D-pad steps 100, ENTER switches to fine (5) steps and back
    {
        const bool known = n.count("sdr.ahf.ppb") > 0;
        const bool fine = n["sdr.ahf.ppbFine"] > 0.5;
        double v = known ? n["sdr.ahf.ppb"] : 0;
        const std::string val = known ? fmtI("%d ppb", (int)v) : "not reported";
        bool pressed = false;
        // ★ no ±20 detent here (the web's is for a dragged thumb): D-pad steps land exactly, and the detent would
        //   swallow every fine step away from 0
        if (rowSlider(x, y, "sdr.ahf.ppb", "CALIBRATION", fine ? "Fine steps (5 ppb) \u2014 press to go back to 100." : "Steps of 100 ppb \u2014 press for fine steps (5 ppb).",
                      v, -5000, 5000, fine ? 5 : 100, val, true, 110, &pressed)) {
            n["sdr.ahf.ppb"] = v;
            sendKV(x, "ahf_control", "ppb", (long)v);
        }
        if (pressed) n["sdr.ahf.ppbFine"] = fine ? 0 : 1;
    }
}

// ── Airspy R2 / Mini ────────────────────────────────────────────────────────────────────────────────────────────────
inline void aspSend(S& x, const char* key, long v) { sendKV(x, "airspy_control", key, v); }

inline void airspyRows(S& x, float& y) {
    const Json& r = x.radio();
    const Json& lim = r["aspLimit"];
    const int stageMax = std::max(1, (int)r["stageMax"].num(15));
    const int mode = (int)held(x, "asp.mode", r["gainMode"].num(1));
    if (lim.kind == Json::Obj) {
        // ★ the owner's band limit: the server has put the radio on the band's curve and refuses any other
        const std::string curve = lim["curve"].str() == "sensitivity" ? "Sensitivity" : "Linearity";
        y = note(x, y, lim["locked"].boolean()
            ? "Gain: " + curve + " " + std::to_string((int)lim["max"].num()) + " — locked by the owner on this band."
            : "Gain: " + curve + ", up to " + std::to_string((int)lim["max"].num()) + " of 21 — limited by the owner on this band. Free mode and the manual stages are off here.");
    } else {
        static const char* desc[3] = {"Digs for the weakest signal; more prone to overload on a crowded band.",
                                      "Keeps strong neighbours from spoiling what you are on.",
                                      "Set the LNA, mixer and VGA yourself."};
        std::string help = "Sensitive and Linear set all three stages from the one gain slider above. Free hands you the three stages. Linear is the safer default on a busy band.";
        if (mode >= 0 && mode <= 2) help += "\n" + std::string(desc[mode]);
        const int hit = rowSeg(x, y, "sdr.asp.mode", "GAIN MODE", help, {"SENSITIVE", "LINEAR", "FREE"}, mode);
        if (hit >= 0 && hit != mode) { hold(x, "asp.mode", hit); aspSend(x, "mode", hit); }
        if (mode == 2) {
            const bool lnaAgc = held(x, "asp.lnaAgc", r["lnaAgc"].boolean() ? 1 : 0) > 0.5;
            const bool mixAgc = held(x, "asp.mixerAgc", r["mixerAgc"].boolean() ? 1 : 0) > 0.5;
            if (rowToggle(x, y, "sdr.asp.lnaAgc", "LNA AGC", "The radio's own automatic gain for the LNA. With this on, the LNA slider below is set by the radio and yours does nothing.", lnaAgc)) {
                hold(x, "asp.lnaAgc", lnaAgc ? 0 : 1); aspSend(x, "lnaAgc", lnaAgc ? 0 : 1);
            }
            if (rowToggle(x, y, "sdr.asp.mixerAgc", "MIXER AGC", "The radio's own automatic gain for the mixer. With this on, the mixer slider below is set by the radio and yours does nothing.", mixAgc)) {
                hold(x, "asp.mixerAgc", mixAgc ? 0 : 1); aspSend(x, "mixerAgc", mixAgc ? 0 : 1);
            }
            struct St { const char* key; const char* label; const char* help; bool agc; };
            const St st[3] = {{"lna", "LNA", "", lnaAgc}, {"mixer", "MIXER", "", mixAgc},
                              {"vga", "VGA", "The VGA has no automatic mode on this radio — it is always yours to set.", false}};
            for (auto& s : st) {
                double v = std::max(0.0, std::min<double>(stageMax, held(x, std::string("asp.") + s.key, r[s.key].num(0))));
                if (rowSlider(x, y, std::string("sdr.asp.") + s.key, s.label, s.help, v, 0, stageMax, 1, s.agc ? "set by AGC" : std::to_string((int)v), !s.agc, 110)) {
                    hold(x, std::string("asp.") + s.key, v); aspSend(x, s.key, (long)v);
                }
            }
            y = note(x, y, "The VGA has no automatic setting on this radio — it is always yours.");
        }
    }
    {
        const bool on = held(x, "asp.biast", r["biasT"].boolean() ? 1 : 0) > 0.5;
        if (rowToggle(x, y, "sdr.asp.biast", "BIAS-T", "4.5 V on the aerial socket for a powered antenna or an LNA. DC into the wrong antenna can damage it.", on)) {
            hold(x, "asp.biast", on ? 0 : 1); aspSend(x, "biast", on ? 0 : 1);
        }
    }
    if (!(r.has("hasPacking") && !r["hasPacking"].boolean())) {
        const bool on = held(x, "asp.packing", r["packing"].boolean() ? 1 : 0) > 0.5;
        if (rowToggle(x, y, "sdr.asp.packing", "USB PACKING", "Sends samples packed 12-bit instead of padded to 16, which cuts the USB bandwidth by a quarter. Useful on a shared hub or a marginal cable; no effect on what you hear.", on)) {
            hold(x, "asp.packing", on ? 0 : 1); aspSend(x, "packing", on ? 0 : 1);
        }
    }
}

// ── HackRF One (EXPERIMENTAL) ───────────────────────────────────────────────────────────────────────────────────────
inline void hrfSend(S& x, const char* key, long v) { sendKV(x, "hackrf_control", key, v); }

inline void hackrfRows(S& x, float& y) {
    const Json& r = x.radio();
    if (x.hwi()["gainLocked"].boolean()) { textRow(x, y, "HACKRF", "", "gain set by the owner"); return; }   // the web hides the panel
    textRow(x, y, "HACKRF", "", "EXPERIMENTAL · manual gain");
    // RF AMP (owner) — ON takes a second press: the web's confirm()
    {
        const bool on = held(x, "hrf.amp", r["amp"].num(0) > 0 ? 1 : 0) > 0.5;
        const bool armed = x.now - x.A.num_["sdr.hrf.ampArmed"] < 4;
        if (rowToggle(x, y, "sdr.hrf.amp", "RF AMP (owner)",
                      "A 0 or +14 dB amplifier ahead of everything (the U14 MGA-81563). It is the part of a HackRF that most commonly dies: the front "
                      "end is largely unprotected, maximum safe RX power is -5 dBm, and static or a nearby transmitter can destroy it. A blown one goes "
                      "DEAF — switching the amp on makes signals drop instead of rise. Owner-only, and off unless you know the aerial.",
                      on, "ON", armed ? "PRESS AGAIN" : "OFF")) {
            if (on) { hold(x, "hrf.amp", 0); hrfSend(x, "amp", 0); }
            else if (armed) { x.A.num_["sdr.hrf.ampArmed"] = 0; hold(x, "hrf.amp", 1); hrfSend(x, "amp", 1); }
            else { x.A.num_["sdr.hrf.ampArmed"] = x.now; x.A.toast("+14 dB, maximum safe input -5 dBm — press again to switch the amp ON"); }
        }
    }
    // LNA + VGA, together under the owner's ceiling (gainCap, tenths): each slider's top is the cap minus the other
    const int capDb = (int)x.hwi()["gainCap"].num(-1) >= 0 ? (int)x.hwi()["gainCap"].num() / 10 : -1;
    const int lnaMaxHw = (int)r["lnaMax"].num(40), lnaStep = std::max(1, (int)r["lnaStep"].num(8));
    const int vgaMaxHw = (int)r["vgaMax"].num(62), vgaStep = std::max(1, (int)r["vgaStep"].num(2));
    double lna = held(x, "hrf.lna", r["lna"].num(0)), vga = held(x, "hrf.vga", r["vga"].num(0));
    int lnaTop = lnaMaxHw, vgaTop = vgaMaxHw;
    if (capDb >= 0) {
        lnaTop = std::max(0, std::min(lnaMaxHw, (capDb - (int)vga) / lnaStep * lnaStep));
        vgaTop = std::max(0, std::min(vgaMaxHw, (capDb - (int)lna) / vgaStep * vgaStep));
    }
    {
        double v = std::min<double>(lna, lnaTop);
        if (rowSlider(x, y, "sdr.hrf.lna", "LNA GAIN",
                      "The front-end (IF) gain stage, 0-40 dB in 8 dB steps. Raise this BEFORE the VGA: gain taken here costs the least noise figure. "
                      "Back it off if the waterfall shows images or the audio breaks up on peaks — an 8-bit radio clips where others cope.",
                      v, 0, std::max(lnaTop, 0), lnaStep, fmtI("%d dB", (int)v) + (v == 0 ? " · none" : ""), lnaTop > 0, 110)) {
            hold(x, "hrf.lna", v); hrfSend(x, "lna", (long)v);
        }
    }
    {
        double v = std::min<double>(vga, vgaTop);
        if (rowSlider(x, y, "sdr.hrf.vga", "VGA GAIN",
                      "The baseband (VGA) gain stage, 0-62 dB in 2 dB steps. It comes AFTER the mixer, so it amplifies noise along with signal — use "
                      "it to top up once the LNA is set, not as the main control.",
                      v, 0, std::max(vgaTop, 0), vgaStep, fmtI("%d dB", (int)v) + (v == 0 ? " · none" : ""), vgaTop > 0, 110)) {
            hold(x, "hrf.vga", v); hrfSend(x, "vga", (long)v);
        }
    }
    {
        const bool on = held(x, "hrf.biast", r["biast"].num(0) > 0 ? 1 : 0) > 0.5;
        if (rowToggle(x, y, "sdr.hrf.biast", "BIAS-T (owner)",
                      "Puts 3.3 V at 50 mA on the antenna port to power a mast-head amplifier. Leave it OFF unless you know what is on the end of the coax "
                      "— owner-only.", on)) {
            hold(x, "hrf.biast", on ? 0 : 1); hrfSend(x, "biast", on ? 0 : 1);
        }
    }
}

// ── USRP B2xx (EXPERIMENTAL): the generic GAIN row carries the gain; these are the radio's own AGC and its reference ──
inline void usrpRows(S& x, float& y) {
    const Json& r = x.radio();
    if (r["hwAgc"].boolean() && !x.hwi()["gainLocked"].boolean()) {
        const bool on = held(x, "usrp.agc", r["agcOn"].boolean() ? 1 : 0) > 0.5;
        if (rowToggle(x, y, "sdr.usrp.agc", "RADIO AGC",
                      "The USRP's own automatic gain, in its AD9361 chip — not VibeSDR's AGC. Moving the gain slider turns it off.", on)) {
            hold(x, "usrp.agc", on ? 0 : 1);
            if (!on) send(x, "{\"type\":\"gain\",\"auto\":true}");
            else if (!x.s.gains.empty()) sendGain(x, x.s.gains[(size_t)gainIndexOf(x.s, x.s.gainNow)]);
        }
    }
    const std::string clk = r["clock"].str();
    if (r["clockSources"].size() && !clk.empty() && clk != "internal") {
        std::string t = clk == "gpsdo" ? "GPSDO" : clk == "external" ? "External 10 MHz" : clk;
        if (r["refLocked"].kind == Json::Bool) t += r["refLocked"].b ? " · locked" : " · NOT locked";
        if (r["gpsdo"].boolean() && r["gpsLocked"].kind == Json::Bool) t += r["gpsLocked"].b ? " · GPS locked" : " · no GPS lock";
        textRow(x, y, "REFERENCE", "", t);
    }
}

}  // namespace sdr
}  // namespace hh
