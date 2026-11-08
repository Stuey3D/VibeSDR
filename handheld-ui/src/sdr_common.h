// sdr_common.h — MENU → SDR HARDWARE: the rows the web page's #menu RADIO range draws outside the per-driver panels —
// RECEIVER, GAIN, VIBE AGC, SAMPLE RATE, IF FILTER, PPM, BIAS-T, DIGITAL AGC, DIRECT SAMPLING — and the ANTENNA
// section. Spec: docs/v12/R36S-NATIVE-MENU-INVENTORY.md §4a.1, §4a.2, §4a.8 (labels and help verbatim from there).
// ★ Every value drawn is the radio's report (hwinfo); nothing is asserted at the radio when the menu is built.
#pragma once
// (included by sec_sdr.h, after App is declared)

namespace hh {
namespace sdr {

// ── RECEIVER (§4a.1 row 1) ──────────────────────────────────────────────────────────────────────────────────────────
inline void receiverRow(S& x, float& y) {
    const std::string d = x.s.hwDriver, model = x.radio()["model"].str();
    std::string name = model;
    if (d == "sdrplay" && !model.empty()) name = "SDRplay " + model;
    else if (d == "icom") name = "Icom " + model + " · receive only";
    else if (model.empty()) name = d == "rtl" ? "RTL-SDR" : "—";
    textRow(x, y, "RECEIVER", "", name);
}

// ── GAIN (§4a.2; also the Airspy R2's preset curve and the USRP's gain) ─────────────────────────────────────────────
inline int gainIndexOf(const RadioState& s, int tenths) {
    int idx = 0;
    for (size_t i = 0; i < s.gains.size(); i++)
        if (std::abs(s.gains[i] - tenths) < std::abs(s.gains[(size_t)idx] - tenths)) idx = (int)i;
    return idx;
}
/** The slider's top index under the owner's ceiling (hwinfo gainCap, tenths; -1 none). */
inline int gainTopIndex(S& x) {
    const int cap = (int)x.hwi()["gainCap"].num(-1);
    int top = (int)x.s.gains.size() - 1;
    if (cap >= 0) while (top > 0 && x.s.gains[(size_t)top] > cap) top--;
    return top;
}

inline void gainRows(S& x, float& y) {
    const RadioState& s = x.s;
    const std::string d = s.hwDriver;
    if (s.gains.empty()) return;                              // rtl_tcp without gain control: nothing to offer
    const bool locked = x.hwi()["gainLocked"].boolean();
    const bool airspy = d == "airspy";
    if (airspy && (int)x.radio()["gainMode"].num(1) == 2) return;   // ★ FREE: the three stages below replace it (see report)
    const int nowIdx = gainIndexOf(s, s.gainNow);
    if (locked) {
        // ★ The web hides the row; a read-out (not a control) says why the gain will not move.
        textRow(x, y, "GAIN", "", (airspy ? fmtI("%d of 21", s.gainNow / 10) : fmt("%.1f dB", s.gainNow / 10.0)) + " · set by the owner");
        return;
    }
    // ★ the switch's own (held) state, not only the engine's: switching VibeAGC off unlocks the slider AT ONCE (it stayed
    //   locked until a sample-rate change made the engine report again — Stuart, 2026-11-07)
    const bool vibeAgc = d == "rtl" && held(x, "vagc", s.agc ? 1 : 0) > 0.5;   // VibeAGC owns the gain: the slider follows it
    const int top = gainTopIndex(x);
    double v = held(x, "gain", nowIdx);
    v = std::min<double>(v, top);
    const int gi = (int)v;
    const std::string val = airspy ? fmtI("%d of 21", s.gains[(size_t)gi] / 10) : fmt("%.1f dB", s.gains[(size_t)gi] / 10.0);
    std::string help;
    if (top < (int)s.gains.size() - 1)
        help = fmt("The owner has limited gain to %.1f dB on this band", s.gains[(size_t)top] / 10.0);
    if (airspy && top < (int)s.gains.size() - 1) help = fmtI("The owner has limited gain to %d of 21 on this band", s.gains[(size_t)top] / 10);
    if (rowSlider(x, y, "sdr.gain", "GAIN", help, v, 0, top, 1, val, !vibeAgc, 110)) {
        hold(x, "gain", v);
        sendGain(x, s.gains[(size_t)v]);
    }

    // VIBE AGC — rtl only (§4a.2 row 2)
    if (d == "rtl") {
        const bool agcLocked = x.hwi()["agcLocked"].boolean();
        const bool on = held(x, "vagc", s.agc ? 1 : 0) > 0.5;
        const std::string help2 = agcLocked
            ? "VibeSDR Custom AGC for RTL-SDR. The owner has locked it on for this receiver."
            : "VibeSDR Custom AGC for RTL-SDR";
        if (rowToggle(x, y, "sdr.vagc", "VIBE AGC", help2, on, "AUTO", "AUTO")) {
            if (on && agcLocked) x.A.toast("The owner has locked VibeAGC on");
            else if (!on) { hold(x, "vagc", 1); send(x, "{\"type\":\"gain\",\"auto\":true}"); }
            else { hold(x, "vagc", 0); sendGain(x, s.gains[(size_t)std::min(gi, top)]); }
        }
    }
}

// ── DECODER RATE BOOST — under SAMPLE RATE ("it doesnt need a super long label") (Stuart, 2026-11-07: "a button under the sample rate
//    options in the sdr menu to Allow decoder to change sample rate"). The server's own switch (Setup's "change the sample
//    rate when a mode needs it", {"type":"dabBoost"}, hwinfo `dabBoost`): ADS-B raises the radio to 2.4 MS/s while it
//    runs (and DAB to 2.048), then puts the chosen rate back. Only where the server reports it (no dead control).
inline void rateBoostRow(S& x, float& y) {
    const Json& h = x.hwi();
    if (!h.has("dabBoost")) return;
    const bool on = held(x, "rboost", h["dabBoost"].boolean() ? 1 : 0) > 0.5;
    if (rowToggle(x, y, "sdr.rboost", "DECODER RATE BOOST",
                  "Lets a decoder raise the sample rate it needs (ADS-B 2.4 MS/s) while it runs; your rate comes back when it stops.", on)) {
        hold(x, "rboost", on ? 0 : 1);
        send(x, std::string("{\"type\":\"dabBoost\",\"on\":") + (on ? "0" : "1") + "}");
    }
}

// ── USB TRANSFER TEST (rtl only, the R36S's comb at every eighth of the sample rate; 2026-11-08) ─────────────────────
// ★ How much the dongle sends per USB transfer: AUTO (32 ms of samples) or a fixed size. It changes the rhythm in which the
//   console collects the data; if the comb moves or weakens with it, the USB activity is what reaches the receiver. Each
//   change restarts the capture (a second of silence). Not saved — the engine starts on AUTO.
inline void usbXferRow(S& x, float& y) {
    static const int kbs[5] = {0, 16, 64, 128, 256};
    const std::vector<std::string> labels = {"AUTO — 32 ms of samples", "16 KB", "64 KB", "128 KB", "256 KB"};
    // ★ the UI's own record, not held(): the engine reports nothing back for this test, and held() falls back to the
    //   report after 3 s — the row would snap back to AUTO while the engine kept the size. Starts AUTO, as the engine does.
    static int sel = 0;
    const int hit = rowSelect(x, y, "sdr.usbxfer", "USB TRANSFER TEST", "How the console collects the dongle's data. If the evenly "
                              "spaced spikes move or weaken, USB activity is reaching the receiver. Restarts the radio briefly; "
                              "not saved.", labels, sel);
    if (hit >= 0 && hit != sel) { sel = hit; sendKV(x, "rtlxfer", "kb", kbs[hit]); }
}

// ── SAMPLE RATE (§4a.2) — every driver but the HF+ and the Icom ─────────────────────────────────────────────────────
inline void rateRow(S& x, float& y) {
    const RadioState& s = x.s;
    const std::string d = s.hwDriver;
    const double lockedRate = x.hwi()["lockedRate"].num(0);
    const double cap = lockedRate > 0 ? lockedRate : 1e30;
    if (x.hwi()["lockedCentre"].num(0) > 0) {
        textRow(x, y, "SAMPLE RATE", "", lockedRate > 0 && !s.rates.empty() ? msps(lockedRate) + " — set by the server." : "Set by the server.");
        return;
    }
    if (s.rates.empty()) return;
    if (s.mode == "dab") {
        // ★ In DAB the rate is not a choice: the server refuses any other value. Shown held, with what the radio runs.
        rowSelect(x, y, "sdr.rate", "SAMPLE RATE", "", {msps(s.rateNow > 0 ? s.rateNow : 2048000) + " (DAB)"}, 0, {}, false);
        return;
    }
    std::vector<double> rs;
    for (double r : s.rates) if (r <= cap) rs.push_back(r);
    std::sort(rs.begin(), rs.end());
    if (s.rateNow > 0 && std::find(rs.begin(), rs.end(), s.rateNow) == rs.end()) { rs.push_back(s.rateNow); std::sort(rs.begin(), rs.end()); }
    std::vector<std::string> labels;
    for (double r : rs) {
        std::string l = msps(r);
        if (d == "sdrplay") l += fmtI(" — %d-bit ADC", r <= 6048000 ? 14 : r <= 8064000 ? 12 : r <= 9216000 ? 10 : 8);
        // ★ the DONGLE's caveat (it drops samples over USB above 2.4 MS/s) — on rtl only. The web adds it for every
        //   driver but sdrplay, which warns an Airspy / HackRF / USRP owner off the rates the radio was bought for.
        else if (d == "rtl" && r > 2400000) l += " (may drop samples)";
        labels.push_back(l);
    }
    // ★ What the radio runs: hwinfo rateNow — but the engine sends no hwinfo after a sampleRate (only a config), so
    //   rateNow goes stale on a change. config's maxBandwidth is the full device span, which IS the capture rate: when
    //   it names one of the offered rates, it is the fresher report.
    double reported = s.rateNow;
    for (double r : rs) if (s.maxSpanHz > 0 && std::fabs(r - s.maxSpanHz) < r * 0.001) reported = r;
    const double now = held(x, "rate", reported);
    int sel = -1;
    for (size_t i = 0; i < rs.size(); i++) if (std::fabs(rs[i] - now) < 0.5) sel = (int)i;
    const int hit = rowSelect(x, y, "sdr.rate", "SAMPLE RATE", "", labels, sel);
    if (hit >= 0) { hold(x, "rate", rs[(size_t)hit]); sendKV(x, "sampleRate", "value", std::lround(rs[(size_t)hit])); }
    rateBoostRow(x, y);
    if (d == "rtl") usbXferRow(x, y);
}

// ── IF FILTER (§4a.2, rtl only) ─────────────────────────────────────────────────────────────────────────────────────
inline void ifFilterRow(S& x, float& y) {
    const Json& h = x.hwi();
    if (!h.has("tunerBw") && !h.has("tunerBwAuto")) return;    // a server that cannot set it: no row
    std::vector<int> ws;
    for (auto& w : h["tunerBws"].a) ws.push_back((int)w.num());
    if (ws.empty()) ws = {2800000, 2000000, 1400000, 1000000, 700000, 500000, 350000};   // the markup's list (older server)
    std::sort(ws.begin(), ws.end(), std::greater<int>());
    const int cur = (int)h["tunerBw"].num(0);
    const bool autoNow = h["tunerBwAuto"].boolean();
    if (!autoNow && cur > 0 && std::find(ws.begin(), ws.end(), cur) == ws.end()) { ws.push_back(cur); std::sort(ws.begin(), ws.end(), std::greater<int>()); }
    const int ifCap = (int)h["ifCap"].num(-1);
    std::vector<int> vals = {-1, 0};
    std::vector<std::string> labels = {"Auto — follows zoom", "Wide — set by sample rate"};
    std::vector<bool> ok = {true, ifCap <= 0};
    for (size_t i = 0; i < ws.size(); i++) {
        const int w = ws[i];
        std::string l = w >= 1000000 ? fmt("%.1f MHz", w / 1e6) : fmtI("%d kHz", (int)std::lround(w / 1e3));
        if (i == ws.size() - 1) l += " — narrowest";
        vals.push_back(w); labels.push_back(l); ok.push_back(ifCap <= 0 || w <= ifCap);
    }
    const double want = held(x, "ifbw", autoNow ? -1 : cur);
    int sel = 0;
    for (size_t i = 0; i < vals.size(); i++) if (vals[i] == (int)want) sel = (int)i;
    const std::string help = ifCap > 0 ? fmtI("The owner has capped this band at %d kHz", ifCap / 1000) : "";
    const int hit = rowSelect(x, y, "sdr.ifbw", "IF FILTER", help, labels, sel, ok);
    if (hit >= 0) { hold(x, "ifbw", vals[(size_t)hit]); sendKV(x, "tunerbw", "value", vals[(size_t)hit]); }
}

// ── PPM (§4a.2, rtl + sdrplay) ──────────────────────────────────────────────────────────────────────────────────────
inline void ppmRow(S& x, float& y) {
    // ★ hwinfo reports the correction in force (`ppm`), so the slider ADOPTS it — the web paints its own pref instead.
    double v = held(x, "ppm", x.hwi()["ppm"].num(0), kSilent);
    if (rowSlider(x, y, "sdr.ppm", "PPM", "", v, -50, 50, 1, fmtI("%d", (int)v), true, 60)) {
        hold(x, "ppm", v);
        sendKV(x, "ppm", "value", (long)v);
    }
}

// ── BIAS-T · DIGITAL AGC · DIRECT SAMPLING (§4a.2, rtl only) ────────────────────────────────────────────────────────
inline bool isBlogV4(const std::string& model) {
    std::string m;
    for (char ch : model) m += (char)std::tolower((unsigned char)ch);
    for (size_t p = m.find("blog"); p != std::string::npos; p = m.find("blog", p + 1)) {
        if (p > 0 && std::isalnum((unsigned char)m[p - 1])) continue;
        size_t q = p + 4;
        while (q < m.size() && std::isspace((unsigned char)m[q])) q++;
        if (m.compare(q, 2, "v4") != 0) continue;
        q += 2;
        if (q < m.size() && m[q] == 'l') q++;
        if (q >= m.size() || !std::isalnum((unsigned char)m[q])) return true;
    }
    return false;
}

inline void rtlRows(S& x, float& y) {
    const Json& h = x.hwi();
    // BIAS-T — the DONGLE's bias-tee: hwinfo top-level `biasT` (never radio.biasT, which is another radio's capability)
    {
        const bool on = held(x, "biasT", h["biasT"].boolean() ? 1 : 0) > 0.5;
        if (rowToggle(x, y, "sdr.biasT", "BIAS-T", "", on)) {
            hold(x, "biasT", on ? 0 : 1);
            send(x, std::string("{\"type\":\"biasT\",\"on\":") + (on ? "false" : "true") + "}");
        }
    }
    // DIGITAL AGC — the RTL2832's own AGC, what {"type":"agc"} commands (hwinfo `digitalAgc`)
    {
        const bool on = held(x, "dagc", h["digitalAgc"].boolean() ? 1 : 0, kSilent) > 0.5;
        if (rowToggle(x, y, "sdr.dagc", "DIGITAL AGC", "", on)) {
            if (on && h["agcLocked"].boolean()) x.A.toast("The owner has locked the AGC on");
            else { hold(x, "dagc", on ? 0 : 1); send(x, std::string("{\"type\":\"agc\",\"on\":") + (on ? "false" : "true") + "}"); }
        }
    }
    // DIRECT SAMPLING
    if (!x.radio()["noDirectSampling"].boolean()) {
        const std::string model = x.radio()["model"].str();
        const bool v4 = isBlogV4(model);
        const bool hasAuto = h.has("autoDs");
        const bool autoDs = h["autoDs"].boolean();
        std::string help = v4
            ? "This radio is " + model + " and does not require direct sampling: it has an up-converter built in, so short wave, "
              "medium wave and long wave already arrive through the tuner. Leaving it off is correct here — switching it on "
              "bypasses the tuner and takes the gain controls with it."
            : "To receive HF and below — the short wave bands, medium wave and long wave — this radio needs direct sampling "
              "ON. The tuner is bypassed, so the gain controls go with it, and the VHF and UHF bands stay quiet until you switch "
              "it back off.";
        if (autoDs && !v4) {
            std::string mhz = fmt("%.3f", h["dsBelowHz"].num(0) / 1e6);
            while (!mhz.empty() && mhz.back() == '0') mhz.pop_back();
            if (!mhz.empty() && mhz.back() == '.') mhz.pop_back();
            help += "\n\nThis receiver's owner has set AUTOMATIC direct sampling: the server switches the tuner out by itself below "
                    + mhz + " MHz, and back in above it.";
        }
        const int reported = autoDs ? 2 : (h["ds"].num(0) > 0 ? 1 : 0);
        const int sel = (int)held(x, "ds", reported);
        std::vector<std::string> opts = {"OFF", "ON"};
        if (hasAuto) opts.push_back("AUTO");
        // ★ block layout, as the web's: the label, the note, then the choice on its own line
        y = x.A.menuRow(x.c, x.a, y, "DIRECT SAMPLING", help);
        const int hit = segmented(x.c, "sdr.ds", {x.a.x + 6, y, x.a.w - 12, 40}, opts, sel);
        y += 52;
        if (hit >= 0 && hit != sel) {
            hold(x, "ds", hit);
            if (hit == 2) sendKV(x, "autoDirectSampling", "value", 1);
            else {
                if (autoDs) sendKV(x, "autoDirectSampling", "value", 0);
                sendKV(x, "directSampling", "value", hit == 1 ? 2 : 0);
            }
        }
    }
}

// ── ANTENNA (§4a.8) — every driver ──────────────────────────────────────────────────────────────────────────────────
inline void antennaRows(S& x, float& y) {
    const Json& r = x.radio();
    const Json& ants = r["antennas"];
    if (ants.size() < 2) return;
    const bool lockedTag = r["antennaLocked"].boolean();
    // ★ On the handheld the page is the owner (adminGate passes on loopback), so a locked aerial only carries the tag.
    y = heading(x, y, "ANTENNA");
    std::vector<std::string> ports, labels;
    for (auto& a : ants.a) {
        const std::string p = a.str();
        std::string n;
        for (char ch : r["antennaNames"][p].str()) n += (unsigned char)ch < 0x20 ? ' ' : ch;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        while (!n.empty() && n.front() == ' ') n.erase(0, 1);
        ports.push_back(p);
        labels.push_back(n.empty() ? p : (!p.empty() && p.back() == ')' ? n + " · " + p : n + " (" + p + ")"));
    }
    y = x.A.menuRow(x.c, x.a, y, lockedTag ? "IN USE · OWNER" : "IN USE", "");
    const std::string cur = r["antenna"].str();
    const double heldIdx = held(x, "ant", (double)(std::find(ports.begin(), ports.end(), cur) - ports.begin()));
    std::vector<bool> lit, en(ports.size(), true);
    for (size_t i = 0; i < ports.size(); i++) lit.push_back((int)heldIdx == (int)i);
    int foc;
    const int hit = buttonGrid(x, y, "sdr.ant", labels, lit, en, foc, 1);
    if (hit >= 0 && ports[(size_t)hit] != cur) {
        hold(x, "ant", hit);
        send(x, "{\"type\":\"antenna\",\"antenna\":" + jsonQuote(ports[(size_t)hit]) + "}");
    }
    const Json& sw = r["antennaSwitch"];
    if (sw.kind == Json::Obj) {
        std::string say;
        if (!sw["connected"].boolean()) say = "The antenna switch is not answering" + (sw["error"].str().empty() ? std::string() : " (" + sw["error"].str() + ")") + ".";
        else if (sw["state"].str() == "several") say = "The switch reports more than one antenna connected.";
        else if (sw["state"].str() == "unknown") say = "Waiting for the switch to say which antenna is connected.";
        y = note(x, y, say);
    }
}

}  // namespace sdr
}  // namespace hh
