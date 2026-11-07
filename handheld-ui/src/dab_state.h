// dab_state.h — DAB on the R36S: what the web client's DAB mode KNOWS and DOES (web/client/src/main.ts dabSetMode,
// dabUiOn/Off, dabTune + DabBlockStepper, dabGoTo, dabArmRestore/dabApplyRestore, the band scan), with no drawing —
// that is dab_box.h. Include AFTER the App class (both files are App free functions, like sec_*.h).
//
// ★★★ UI ONLY. DAB's weak-signal lock is the crown jewel (memory feedback_dab_weak_signal_crown_jewel): nothing here
//     touches the DSP. It sends exactly the web client's messages — {"type":"dab","on":1[,"channel":i][,"sid":n]},
//     {"type":"dab","on":0}, {"type":"dab_service","sid":n}, {"type":"dab_scan","on":1|0}, {"type":"dab_scan_get"} —
//     and reads the LEGACY `dab` report (this client never asks for dab=2, so every report is whole: the list, the
//     playing service's radio text, the counters the meter needs).
// ★★★ ONE STATION'S RADIO TEXT, NOT THE MULTIPLEX'S (Stuart, 2026-11-07: "DAB should lose the all station radio text on
//     this device as that will be too much"). The report's per-row `dls` (the PAD scanner's whole-multiplex label scan)
//     is never read here; only the PLAYING service's text (the report's top-level `dls`) is shown. The scan itself is the
//     ENGINE's (vibedab::dabScanLabelsOn) and no client message turns it off — see the report for the engine line.
// ★★ THE SHARED-DIAL CONTRACT: the client sends what the person did; the block shown is the server's (the report's
//    `channel`), except for the label the stepper is holding while a burst of presses settles (DabAskGuard's rule).
// ★★ ARRIVALS: engine.h keeps the LAST message of each type, so an identical second `dab_off` is invisible by content.
//    RadioState::seq[type] (one counter per type, bumped in Engine::onText) says a message ARRIVED — see the wiring.
#pragma once
#include "dab_image.h"
#include "dab_quality.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace hh {

/** Band III, mirroring vibe_dab_channels.h (and main.ts DAB_BLOCKS) — the index IS the `channel` the server takes. */
struct DabBlock { const char* name; double hz; };
inline const DabBlock kDabBlocks[] = {
    {"5A", 174928e3}, {"5B", 176640e3}, {"5C", 178352e3}, {"5D", 180064e3},
    {"6A", 181936e3}, {"6B", 183648e3}, {"6C", 185360e3}, {"6D", 187072e3},
    {"7A", 188928e3}, {"7B", 190640e3}, {"7C", 192352e3}, {"7D", 194064e3},
    {"8A", 195936e3}, {"8B", 197648e3}, {"8C", 199360e3}, {"8D", 201072e3},
    {"9A", 202928e3}, {"9B", 204640e3}, {"9C", 206352e3}, {"9D", 208064e3},
    {"10A", 209936e3}, {"10N", 210096e3}, {"10B", 211648e3}, {"10C", 213360e3}, {"10D", 215072e3},
    {"11A", 216928e3}, {"11N", 217088e3}, {"11B", 218640e3}, {"11C", 220352e3}, {"11D", 222064e3},
    {"12A", 223936e3}, {"12N", 224096e3}, {"12B", 225648e3}, {"12C", 227360e3}, {"12D", 229072e3},
    {"13A", 230784e3}, {"13B", 232496e3}, {"13C", 234208e3}, {"13D", 235776e3}, {"13E", 237488e3}, {"13F", 239200e3},
};
constexpr int kDabBlockN = (int)(sizeof kDabBlocks / sizeof kDabBlocks[0]);
inline int dabBlockIndex(const std::string& name) { for (int i = 0; i < kDabBlockN; i++) if (name == kDabBlocks[i].name) return i; return -1; }

/** TS 101 756 table 12 — main.ts DAB_PTY. */
inline const char* dabPty(int p) {
    static const char* t[] = {"None", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture", "Science",
        "Varied", "Pop Music", "Rock Music", "Easy Listening", "Light Classical", "Serious Classical", "Other Music", "Weather",
        "Finance", "Children's", "Social Affairs", "Religion", "Phone In", "Travel", "Leisure", "Jazz", "Country",
        "National Music", "Oldies", "Folk", "Documentary"};
    return p >= 0 && p < 30 ? t[p] : "";
}

/** ★ THE THREE LAYOUTS (VIBE_HHUI_DAB_LAYOUT=A|B|C, for the screenshots; C is the default — see dab_box.h). */
enum DabLayout { DAB_A_SHEET = 0, DAB_B_STRIP = 1, DAB_C_PAGE = 2 };

/** Everything DAB keeps between frames (UI thread only). */
struct DabUi {
    bool on = false;                 // this client is in DAB (the web's dabOn)
    bool grabNext = false;           // give the box the D-pad on the next frame (the web's autoGrab on entry)
    bool wantRowFocus = false;       // a fresh grab lands on the playing (else first) station row once there are rows
    int layout = DAB_C_PAGE;
    bool slideFull = false;          // the slide full-screen (a press on it; BACK / ENTER closes)
    bool scopesOn = false;           // what we last told the engine (dab_scopes)
    double slideSid = 0;             // the service the shown slide belongs to
    // the block: what the label says, what the server last reported, the burst being settled (DabBlockStepper)
    int channel = -1, serverBlock = -1;
    int stepPending = -1; double stepSendAt = 0;
    int askBlock = -1; double askUntil = 0;     // DabAskGuard: the block we asked for, held against older reports
    // arrivals (RadioState::seq)
    uint64_t seenDab = 0, seenOff = 0, seenErr = 0, seenScan = 0;
    double lastReportAt = 0;         // nowSec() of the last `dab` report
    bool haveReport = false;         // a report for THIS session (not a stale one from before)
    double eid = -1;
    DabQualityMeter meter;
    // the box
    enum Pane { P_STATIONS, P_SIGNAL, P_SCAN, P_BLOCKS } pane = P_STATIONS;
    Scroller listScroll, sigScroll, scanScroll, blkScroll;
    std::string scanOpen;            // the scan row expanded (one at a time on this screen)
    bool scanMine = false;
    // leaving: the restore the web arms (dabArmRestore) and spends on the first config out of DAB
    double prevHz = 0; std::string prevMode;
    bool leaving = false; double leftAt = 0; uint64_t offAtLeave = 0, cfgAtLeave = 0;
    bool restoreArmed = false; double restoreHz = 0; std::string restoreMode;
    // "tuning in" (main.ts dabPickedAt / the PCM run): a picked station's line until its decoder runs clean
    double pickedAt = 0, pcmLast = -1, pcmRoseAt = 0, pcmRunStart = 0;
    std::string waiting;
    std::map<std::string, std::string> fit;     // text cut to a width, by "<font>|<width>|<text>"
};
inline DabUi& dabUi() {
    static DabUi u = [] {
        DabUi x;
        if (const char* l = std::getenv("VIBE_HHUI_DAB_LAYOUT")) x.layout = (*l == 'A' || *l == 'a') ? DAB_A_SHEET : (*l == 'B' || *l == 'b') ? DAB_B_STRIP : DAB_C_PAGE;
        return x;
    }();
    return u;
}

/** How many messages of `type` have arrived (RadioState::seq — see the wiring). */
inline uint64_t dabArrived(const RadioState& s, const char* type) { auto it = s.seq.find(type); return it == s.seq.end() ? 0 : it->second; }

/** The client is in DAB. */
inline bool dabActive() { return dabUi().on; }

/** The report to draw from, or nullptr: none yet this session, or one about the block we just LEFT (a burst of presses
 *  — the web sets dabState = null on every press, so the list empties at once rather than showing the old multiplex). */
inline const Json* dabReport(const RadioState& s) {
    DabUi& u = dabUi();
    if (!u.on || !u.haveReport) return nullptr;
    const Json& j = s.msg("dab");
    if (j.isNull() || j.has("error")) return nullptr;
    if (u.stepPending >= 0) return nullptr;
    if (u.askBlock >= 0 && dabBlockIndex(j["channel"].str()) != u.askBlock) return nullptr;
    return &j;
}
/** The block the label shows (the stepper's, the asked-for one, else the server's). */
inline int dabShownBlock() {
    DabUi& u = dabUi();
    if (u.stepPending >= 0) return u.stepPending;
    if (u.askBlock >= 0) return u.askBlock;
    return u.serverBlock >= 0 ? u.serverBlock : u.channel;
}
/** The playing service's row in the report, or nullptr. */
inline const Json* dabPlaying(const Json& r) {
    const double sid = r["sid"].num(0);
    if (sid <= 0) return nullptr;
    for (auto& sv : r["services"].a) if (sv["sid"].num(-1) == sid) return &sv;
    return nullptr;
}
/** dabTypes.ts dabServiceStereo: the playing service's own audio headers, never the FM pilot's. */
inline bool dabServiceStereo(const Json* r) {
    if (!r) return false;
    if ((*r)["stereo"].kind == Json::Bool) return (*r)["stereo"].b;
    const std::string& cd = (*r)["codecDetail"].str();
    return cd.size() >= 6 && cd.compare(cd.size() - 6, 6, "Stereo") == 0;
}

// ── what the person did ───────────────────────────────────────────────────────────────────────────────────────────────

inline void dabSendBlock(App& a, int idx, double sid = 0) {
    DabUi& u = dabUi();
    char b[120];
    if (sid > 0) std::snprintf(b, sizeof b, "{\"type\":\"dab\",\"on\":1,\"channel\":%d,\"sid\":%.0f}", idx, sid);
    else std::snprintf(b, sizeof b, "{\"type\":\"dab\",\"on\":1,\"channel\":%d}", idx);
    a.eng_->send(b);
    u.askBlock = idx; u.askUntil = nowSec() + 8;
    u.channel = idx;
    a.str_["dab.channel"] = kDabBlocks[idx].name;   // ★ the web's savePref('dabChannel') — this run's memory
}

/** Into DAB (main.ts dabSetMode(true)): JOIN a receiver already on a multiplex (no channel sent — the dial stays where
 *  everybody is), else ask for the block this device was last on (else the server's own last block, else 12B). */
inline void dabEnter(App& a, const RadioState& s, int wantBlock = -1, double wantSid = 0) {
    DabUi& u = dabUi();
    if (u.on) return;
    u.on = true; u.grabNext = true; u.leaving = false; u.restoreArmed = false;
    u.haveReport = false; u.meter.reset(); u.pane = DabUi::P_STATIONS; u.stepPending = -1; u.askBlock = -1; u.eid = -1;
    u.listScroll = {}; u.sigScroll = {}; u.scanMine = false;
    u.seenDab = dabArrived(s, "dab");
    const bool serverInDab = nowSec() - u.lastReportAt < 2.5 && !s.msg("dab")["channel"].str().empty();
    if (wantBlock >= 0) dabSendBlock(a, wantBlock, wantSid);   // ★ a bookmark / a scan row: that block, that service
    else if (serverInDab) { u.haveReport = true; u.serverBlock = dabBlockIndex(s.msg("dab")["channel"].str()); }
    else {
        const int remembered = a.str_.count("dab.channel") ? dabBlockIndex(a.str_["dab.channel"]) : -1;
        if (remembered >= 0) dabSendBlock(a, remembered);
        else a.eng_->send("{\"type\":\"dab\",\"on\":1}");
    }
    a.eng_->send("{\"type\":\"dab_scan_get\"}");   // ★ the last band scan's list (MULTIPLEXES), for anyone
}

inline void dabReleaseBox(App& a) {
    for (size_t i = 0; i < a.sheets_.size();) { if (a.sheets_[i] == App::S_DAB) a.sheets_.erase(a.sheets_.begin() + (long)i); else i++; }
    a.nav_.forget("dab");
}

/** Out of DAB (dabSetMode(false)): arm the restore BEFORE the mode goes — the remembered pair is the only record of where
 *  the radio was — then leave. `wantMode` = a mode picked in the picker (it wins over the remembered one); AM is the
 *  fallback by choice (Stuart, 2026-09-25: its hiss is gentler than an FM squelch roar). */
inline void dabLeave(App& a, const RadioState& s, const std::string& wantMode = "") {
    DabUi& u = dabUi();
    if (!u.on) return;
    u.restoreArmed = true;
    u.restoreHz = u.prevHz;
    u.restoreMode = !wantMode.empty() ? wantMode : !u.prevMode.empty() ? u.prevMode : "am";
    u.leaving = true; u.leftAt = nowSec(); u.offAtLeave = dabArrived(s, "dab_off"); u.cfgAtLeave = s.configSeq;
    if (u.scanMine && s.msg("dab_scan")["running"].boolean()) a.eng_->send("{\"type\":\"dab_scan\",\"on\":0}");
    a.eng_->send("{\"type\":\"dab\",\"on\":0}");
    u.on = false; u.haveReport = false; u.meter.reset(); u.stepPending = -1; u.askBlock = -1; u.scanMine = false; u.pickedAt = 0;
    u.scopesOn = false; u.slideFull = false;   // ★ the engine forgets a socket's scopes when DAB ends (dabPeersForget)
    dabReleaseBox(a);
}

/** Step the multiplex — L2 / R2, the left stick's jog and its hold-sweep all end here in DAB (main.ts dabTune). The label
 *  moves on every press; the server is told the block the person STOPPED on, once, 450 ms after the last press
 *  (DAB_STEP_SETTLE_MS). CLAMPED at the ends, never wrapped. Returns true when DAB took the press. */
inline bool dabTune(App& a, int n) {
    DabUi& u = dabUi();
    if (!u.on) return false;
    if (n == 0) return true;
    int cur = dabShownBlock();
    if (cur < 0) cur = dabBlockIndex("12B");
    const int next = std::max(0, std::min(kDabBlockN - 1, cur + n));
    if (next == cur) return true;
    u.stepPending = next; u.stepSendAt = nowSec() + 0.45;
    u.channel = next;
    return true;
}
/** ★ Zoom is fixed in DAB — the multiplex IS the view (main.ts dabLockControls). True when DAB took the press. */
inline bool dabZoom(App& a, int n) {
    if (!dabUi().on) return false;
    if (n) a.toast("Zoom is fixed in DAB — the multiplex is the view");
    return true;
}
/** A block picked (the BLOCK pane, a scan row): at once — a pick is the answer, a burst still settling must not
 *  override it (dabGoTo). */
inline void dabGoBlock(App& a, int idx, double sid = 0) {
    DabUi& u = dabUi();
    if (idx < 0 || idx >= kDabBlockN) return;
    u.stepPending = -1;
    if (u.serverBlock == idx && u.askBlock < 0 && sid > 0) { a.eng_->send("{\"type\":\"dab_service\",\"sid\":" + std::to_string((long long)sid) + "}"); }
    else if (u.serverBlock != idx || u.askBlock >= 0) { u.meter.reset(); dabSendBlock(a, idx, sid); }
    u.pickedAt = sid > 0 ? nowSec() : 0; u.pcmRunStart = 0; u.pcmRoseAt = 0;
}
/** main.ts dabGoTo: a DAB bookmark (block centre ±50 kHz + its service id, sid <= 0 = whatever plays) — into DAB first
 *  if need be. For bm_page.h / search_page.h, whose DAB rows can now play (their "this UI has no DAB" note is stale). */
inline void dabGoTo(App& a, const RadioState& s, double hz, double sid) {
    int idx = -1;
    for (int i = 0; i < kDabBlockN; i++) if (std::fabs(kDabBlocks[i].hz - hz) < 50000) { idx = i; break; }
    if (idx < 0) return;
    if (!dabUi().on) dabEnter(a, s, idx, sid > 0 ? sid : 0);
    else dabGoBlock(a, idx, sid > 0 ? sid : 0);
}
/** A station row pressed. */
inline void dabPickService(App& a, double sid) {
    DabUi& u = dabUi();
    u.pickedAt = nowSec(); u.pcmRunStart = 0; u.pcmRoseAt = 0;
    a.eng_->send("{\"type\":\"dab_service\",\"sid\":" + std::to_string((long long)sid) + "}");
}

/** The mode picker's press (call it before eng_->setMode): "dab" toggles DAB (main.ts toggleDabMode); any other mode
 *  picked while in DAB leaves DAB for it. Returns true when it handled the press (the caller then closes the sheet). */
inline bool dabModePick(App& a, const RadioState& s, const std::string& id) {
    if (id == "dab") { if (dabUi().on) dabLeave(a, s); else dabEnter(a, s); return true; }
    if (dabUi().on) { dabLeave(a, s, id); return true; }
    return false;
}

// ── once a frame ──────────────────────────────────────────────────────────────────────────────────────────────────────

inline void dabImagesTick(App& a, const RadioState& s);
/** Call once a loop, after eng.pump (with the session running or not). */
inline void dabTick(App& a, const RadioState& s) {
    DabUi& u = dabUi();
    const double now = nowSec();
    // the dial as it is now, while DAB does not have it (dabNoteNonDab — every config while out of DAB)
    if (!u.on && !u.leaving && s.session == "running" && s.vfoHz > 0 && !s.mode.empty()) { u.prevHz = s.vfoHz; u.prevMode = s.mode; }
    // a box to grab, on the frame after entry (the picker closes itself first)
    if (u.grabNext && u.on) {
        u.grabNext = false;
        bool there = false; for (auto sh : a.sheets_) if (sh == App::S_DAB) there = true;
        if (!there) { a.open(App::S_DAB); u.wantRowFocus = true; }
    }
    // dab_off: the radio left DAB (us, another listener on a shared dial, the server's own exit)
    const uint64_t off = dabArrived(s, "dab_off");
    if (off != u.seenOff) {
        u.seenOff = off;
        if (u.on) { u.on = false; u.haveReport = false; u.meter.reset(); u.stepPending = -1; u.askBlock = -1; u.slideFull = false; dabReleaseBox(a); }
        u.scopesOn = false;
    }
    // the restore, spent on the first config out of DAB (dabApplyRestore) — on top of the server's truth
    if (u.restoreArmed && off > u.offAtLeave && s.configSeq > u.cfgAtLeave) {
        u.restoreArmed = false; u.leaving = false;
        if (!u.restoreMode.empty() && u.restoreMode != s.mode) a.eng_->setMode(u.restoreMode);
        if (u.restoreHz > 0 && std::fabs(s.vfoHz - u.restoreHz) > 1) a.tuneTo(a.clampTune(u.restoreHz));
    }
    if (u.leaving && now - u.leftAt > 6) { u.leaving = false; u.restoreArmed = false; }   // ★ an old server that never says dab_off
    // a refusal, in the server's own words
    const uint64_t err = dabArrived(s, "dab_error");
    if (err != u.seenErr) {
        u.seenErr = err;
        const std::string why = s.msg("dab_error")["why"].str();
        if (!why.empty()) a.toastLong(why);
        if (u.on && !u.haveReport) { u.on = false; u.meter.reset(); dabReleaseBox(a); }
    }
    // the report
    const uint64_t rep = dabArrived(s, "dab");
    if (rep != u.seenDab) {
        u.seenDab = rep;
        const Json& j = s.msg("dab");
        if (j.has("error")) {
            a.toast("DAB: " + j["error"].str());
        } else if (!j["channel"].str().empty()) {
            u.lastReportAt = now;
            // ★ a receiver already in DAB (a shared radio, a reload): the box opens on the first report (dabUiOn) —
            //   but never on a report racing our own exit
            if (!u.on && !u.leaving && s.session == "running") { dabEnter(a, s); }
            if (u.on) {
                u.haveReport = true;
                u.serverBlock = dabBlockIndex(j["channel"].str());
                if (u.askBlock >= 0 && (u.serverBlock == u.askBlock || now > u.askUntil)) u.askBlock = -1;
                if (u.askBlock < 0 && u.stepPending < 0) {
                    u.channel = u.serverBlock;
                    u.meter.push(j, now * 1000.0);
                    // ★ a new multiplex = back to the list, and only then (comparing services is what the box is for)
                    const double eid = j["eid"].num(-1);
                    if (eid != u.eid) { u.eid = eid; if (u.pane == DabUi::P_SIGNAL) u.pane = DabUi::P_STATIONS; }
                    // the decoder's PCM counter rising for a second = the picked station is playing (main.ts decoderFlowing)
                    const double pc = j["pcmPushed"].num(-1);
                    if (pc >= 0 && pc != u.pcmLast) {
                        if (u.pcmLast >= 0 && pc > u.pcmLast) { if (now - u.pcmRoseAt > 1.5) u.pcmRunStart = now; u.pcmRoseAt = now; }
                        u.pcmLast = pc;
                    }
                }
            }
        }
    }
    // the burst settled: the one send
    if (u.on && u.stepPending >= 0 && now >= u.stepSendAt) {
        const int b = u.stepPending;
        u.stepPending = -1;
        if (b != u.serverBlock || u.askBlock >= 0) { u.meter.reset(); dabSendBlock(a, b); }
    }
    // the band scan
    const uint64_t sc = dabArrived(s, "dab_scan");
    if (sc != u.seenScan) {
        u.seenScan = sc;
        const Json& j = s.msg("dab_scan");
        if (j.has("error") && !j["running"].boolean()) { if (u.scanMine) a.toastLong(j["error"].str()); u.scanMine = false; }
        else if (u.scanMine && u.on && j["running"].boolean()) u.pane = DabUi::P_SCAN;
    }
    // "tuning in" for a picked station: until its decoder has run clean for a second (the web's gain + audio halves are
    // not visible here — the engine plays the speaker itself)
    u.waiting.clear();
    if (u.on && u.pickedAt > 0) {
        const Json* r = dabReport(s);
        const bool flowing = u.pcmRunStart >= u.pickedAt && u.pcmRoseAt >= u.pickedAt + 0.4 && (u.pcmRoseAt - u.pcmRunStart) >= 1.0 && now - u.pcmRoseAt < 1.5;
        if (flowing) u.pickedAt = 0;
        else if (r && (*r)["locked"].boolean() && (*r)["sid"].num(0) > 0) {
            const int secs = (int)(now - u.pickedAt);
            char b[160];
            if (secs < 30) std::snprintf(b, sizeof b, "Tuning in — waiting for the decoder to run clean… %ds", secs);
            else std::snprintf(b, sizeof b, "Still tuning in (%ds) — a weak multiplex can take a while and the decoder has not given up", secs);
            u.waiting = b;
        }
    }
    dabImagesTick(a, s);
}

/** The pictures, once a frame from dabTick: the playing service's slide on a new `seq`, every listed station's logo
 *  (the one the multiplex sent, else its kept slide), and the uploads. */
inline void dabImagesTick(App& a, const RadioState& s) {
    DabUi& u = dabUi();
    DabImages& im = dabImages();
    const double now = nowSec();
    const Json* r = dabReport(s);
    if (!u.on) { if (im.slide()) im.clearSlide(); u.slideFull = false; im.tick(now); return; }
    if (r) {
        const double sid = (*r)["sid"].num(0);
        if (sid != u.slideSid) { u.slideSid = sid; im.clearSlide(); u.slideFull = false; }
        const Json& sl = (*r)["slide"];
        const Json* pl = dabPlaying(*r);
        uint64_t key = sl.kind == Json::Obj ? (uint64_t)sl["seq"].num(0) : 0;
        std::string path = "/vibeserver/dabslide";
        if (!key && pl && (*pl)["logoSlide"].boolean()) { key = (1ull << 40) | (uint64_t)sid; path += "?sid=" + std::to_string((uint64_t)sid); }
        if (key && sid > 0 && key != im.slideSeqShown()) {
            static uint64_t asked = 0; static double askedSid = 0, askedAt = 0;
            // ★ once per new slide; a miss (the engine had not stored it yet) is asked again after 10 s
            if (key != asked || sid != askedSid || now - askedAt > 10) { asked = key; askedSid = sid; askedAt = now; im.setSlideWanted(key); im.wantSlide(a.eng_->port(), key, path); }
        }
        for (auto& sv : (*r)["services"].a) {
            const uint32_t id = (uint32_t)sv["sid"].num(0);
            if (!id) continue;
            const std::string path = sv["logoAir"].boolean() ? "/vibeserver/dablogoair?sid=" + std::to_string(id)
                                   : sv["logoSlide"].boolean() ? "/vibeserver/dabslide?sid=" + std::to_string(id) : "";
            im.wantLogo(a.eng_->port(), id, path, now);
        }
    }
    im.tick(now);
}

// ── for the VTS bar (vts.h) ───────────────────────────────────────────────────────────────────────────────────────────

/** What DAB has to say about the station being listened to, for the VTS: "" outside DAB. The PLAYING service's radio
 *  text only (never another row's), or the "tuning in" line while a picked station starts. */
struct DabNowPlaying { bool active = false; std::string service, shortLabel, dls, codec, sidHex; bool dabPlus = false, stereo = false; };
inline DabNowPlaying dabNowPlaying(const RadioState& s) {
    DabNowPlaying n;
    const Json* r = dabReport(s);
    if (!r) return n;
    n.active = true;
    const Json* p = dabPlaying(*r);
    if (p) {
        n.service = (*p)["label"].str(); n.shortLabel = (*p)["short"].str(); n.codec = (*p)["codec"].str();
        n.dabPlus = n.codec == "DAB+";
        char b[16]; std::snprintf(b, sizeof b, "%04X", (unsigned)(*p)["sid"].num(0)); n.sidHex = b;
    }
    n.dls = !dabUi().waiting.empty() ? dabUi().waiting : (*r)["dls"].str();
    n.stereo = dabServiceStereo(r);
    return n;
}
/** The one line: "BBC Radio 1: Now playing …", "" outside DAB. */
inline std::string dabVtsText(const RadioState& s) {
    const DabNowPlaying n = dabNowPlaying(s);
    if (!n.active || n.service.empty()) return "";
    return n.dls.empty() ? n.service : n.service + ": " + n.dls;
}

}  // namespace hh
