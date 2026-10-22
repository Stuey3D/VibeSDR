// scripts/test-jr-icom.sh — Jr's IcomRadio.swift against the contract example (docs/ICOM-SUPPORT-PLAN.md) and the
// same cases scripts/test_icom_radio.ts checks for the app: labels, FIL widths, passband, span ladder, S-meter, merge,
// banners, absent-field defaults, and that controlMessage only ever carries what the radio listed (receive only).
import Foundation
var fails = 0, n = 0
func check(_ c: Bool, _ what: String) { n += 1; if !c { fails += 1; print("FAIL:", what) } }
let hw = """
{"type":"hwinfo","radio":{"driver":"icom","model":"IC-7300","civ":"94","link":"usb","transport":"cp210x","linesHeldLow":true,
 "baud":115200,"noIq":true,"sharedOnly":true,"canTransmit":false,
 "modes":["lsb","usb","am","cw","rtty","fm","cwr","rttyr","usbd","lsbd"],"modeNow":"usbd",
 "filters":[1,2,3],"filNow":1,
 "filterWidths":{"ssb":[3000,2400,1800],"cw":[1200,500,250],"rtty":[2400,500,250],"am":[9000,6000,3000],"fm":[15000,10000,7000]},
 "att":[0,20],"attNow":0,"preamp":[0,1,2],"preampNow":1,"agc":["fast","mid","slow"],"agcNow":"mid",
 "rfGain":[0,255],"rfGainNow":255,"nb":true,"nbNow":false,"nr":true,"nrNow":false,"anf":true,"anfNow":false,
 "scope":{"points":475,"spans":[5000,10000,20000,50000,100000,200000,500000,1000000],"span":50000,
          "modes":["centre","fixed"],"mode":"centre","speeds":["fast","mid","slow"],"fps":19.9},
 "ranges":[[30000,74800000]],"tx":false,"linkUp":true,"ownerLock":true,"rfOpen":false,
 "audio":"ALSA hw:2,0 (CODEC)","audioProblem":"","refusedFrames":0}}
"""
let j = try! JSONSerialization.jsonObject(with: Data(hw.utf8)) as! [String: Any]
let r = IcomRadio.parse(j["radio"] as! [String: Any])!
check(r.model == "IC-7300", "model")
check(r.modes.count == 10 && r.modeNow == "usbd", "modes")
check(r.filters == [1,2,3] && r.filNow == 1, "filters")
check(r.spans.count == 8 && r.span == 50000, "spans")
check(r.rangeLo == 30000 && r.rangeHi == 74_800_000, "ranges")
check(r.ownerLock && r.linkUp && !r.tx, "flags")
check(r.banner == "", "no banner")
check(!r.mayTune(admin: false) && r.mayTune(admin: true), "owner lock")
check(IcomRadio.modeLabel("usbd") == "USB-D" && IcomRadio.modeLabel("cwr") == "CW-R" && IcomRadio.modeLabel("rttyr") == "RTTY-R" && IcomRadio.modeLabel("am") == "AM", "labels")
check(IcomRadio.clientMode("rtty") == "lsb" && IcomRadio.clientMode("rttyr") == "usb" && IcomRadio.clientMode("cwr") == "cwl" && IcomRadio.clientMode("fm") == "nfm", "client mode")
check(r.filLabel(1) == "FIL1 3k" && r.filLabel(2) == "FIL2 2.4k" && r.filLabel(3) == "FIL3 1.8k", "fil labels \(r.filLabel(2))")
check(IcomRadio.fmtWidth(500) == "500" && IcomRadio.fmtWidth(15000) == "15k", "fmtWidth")
let pb = r.passband(defaultLow: 50, defaultHigh: 2800)!
check(pb.low == 50 && pb.high == 3050, "usb passband")
check(IcomRadio.spanStep(r.spans, current: 50000, dir: 1) == 20000, "zoom in")
check(IcomRadio.spanStep(r.spans, current: 50000, dir: -1) == 100000, "zoom out")
check(IcomRadio.spanStep(r.spans, current: 5000, dir: 1) == 5000, "floor")
check(IcomRadio.spanStep(r.spans, current: 1_000_000, dir: -1) == 1_000_000, "ceiling")
check(IcomRadio.spanStep(r.spans, current: 102_400, dir: 0) == 100000, "nearest")
check(IcomRadio.sMeter(-30.4)?.text == "S4", "S4 \(String(describing: IcomRadio.sMeter(-30.4)))")
check(IcomRadio.sMeter(0)?.text == "S9" && abs(IcomRadio.sMeter(0)!.frac - 0.6) < 1e-9, "S9")
check(IcomRadio.sMeter(20)?.text == "S9+20", "S9+20")
check(IcomRadio.sMeter(-54)?.text == "S0" && IcomRadio.sMeter(-80)?.frac == 0, "S0")
check(IcomRadio.sMeter(nil) == nil, "unread")
let line = """
{"type":"icom","freq":7074000,"mode":"rtty","fil":2,"sRaw":48,"sDbOverS9":-30.4,"tx":true,"link":true,"fps":19.9,"span":20000,"scopeMode":"centre"}
"""
let st = IcomStatus.parse(try! JSONSerialization.jsonObject(with: Data(line.utf8)) as! [String: Any])!
check(st.freq == 7_074_000 && st.mode == "rtty" && st.fil == 2 && st.sDbOverS9 == -30.4 && st.tx && st.span == 20000, "status")
let m = r.merged(st)
check(m.modeNow == "rtty" && m.filNow == 2 && m.tx && m.span == 20000, "merge")
check(m.banner == "The radio is transmitting — receive paused" && m.quiet, "tx banner")
check(m.filLabel(2) == "FIL2 500", "rtty widths")
var d = m; d.tx = false; d.linkUp = false
check(d.banner == "The radio is not answering — VibeServer is retrying", "link banner")
let unread = IcomStatus.parse(["type": "icom", "sRaw": -1, "sDbOverS9": -999.0, "link": false])!
check(unread.sDbOverS9 == nil && !unread.link, "unread meter / link down")
// absent fields: lock defaults ON, link up
let bare = IcomRadio.parse(["driver": "icom"])!
check(bare.ownerLock && bare.linkUp && bare.model == "Icom" && bare.spans.isEmpty, "absent defaults")
check(IcomRadio.parse(["driver": "rtl"]) == nil, "not icom")
// receive only: controlMessage drops values the radio didn't list, never anything else
check(r.controlMessage(mode: "usbd")?["mode"] as? String == "usbd", "mode msg")
check(r.controlMessage(mode: "wfm") == nil && r.controlMessage(fil: 4) == nil, "refuses unlisted")
let cm = r.controlMessage(mode: "usb", fil: 2)!
check(Set(cm.keys) == ["type", "mode", "fil"] && cm["type"] as? String == "icom_control", "keys")
// ── FIXED scope mode: the carrier line sits at the tuned frequency within the sweep's REAL edges ──
check(!r.scopeFixed, "centre mode by default (hwinfo scope.mode)")
var fx = r; fx.scopeMode = "fixed"
check(fx.scopeFixed, "fixed")
let fixedHw = IcomRadio.parse(["driver": "icom", "scope": ["mode": "fixed", "spans": [50000.0]]])!
check(fixedHw.scopeFixed, "hwinfo scope.mode fixed parsed")
check(IcomRadio.parse(["driver": "icom", "scope": ["mode": "sideways"]])!.scopeMode == "centre", "unknown scope mode → centre")
let stFixed = IcomStatus.parse(["type": "icom", "scopeMode": "fixed", "sRaw": 10, "sDbOverS9": -20.0])!
check(r.merged(stFixed).scopeFixed, "status line scopeMode fixed merges")
let stNone = IcomStatus.parse(["type": "icom", "sRaw": 10])!
check(fx.merged(stNone).scopeFixed, "a status line without scopeMode keeps hwinfo's")
check(fx.merged(IcomStatus.parse(["type": "icom", "scopeMode": "centre"])!).scopeMode == "centre", "and back to centre")
// fixed edges 7.000-7.100 MHz: centre 7.05 MHz, span 100 kHz
check(IcomRadio.vfoFraction(vfo: 7_074_000, centre: 7_050_000, span: 100_000).map { abs($0 - 0.74) < 1e-9 } == true, "FT8 at 74 % across a 7.000-7.100 window")
check(IcomRadio.vfoFraction(vfo: 7_000_000, centre: 7_050_000, span: 100_000) == 0, "left edge")
check(IcomRadio.vfoFraction(vfo: 7_100_000, centre: 7_050_000, span: 100_000) == 1, "right edge")
check(IcomRadio.vfoFraction(vfo: 7_150_000, centre: 7_050_000, span: 100_000) == nil, "tuned past the right edge → no line")
check(IcomRadio.vfoFraction(vfo: 6_990_000, centre: 7_050_000, span: 100_000) == nil, "tuned below the left edge → no line")
check(IcomRadio.vfoFraction(vfo: 7_074_000, centre: 7_050_000, span: 0) == nil, "no span yet → no line")
check(IcomRadio.vfoFraction(vfo: 7_074_000, centre: 7_074_000, span: 50_000) == 0.5, "centre mode lands dead centre")
check(IcomRadio.vfoFraction(vfo: 14_074_000.5, centre: 14_074_000.5, span: 1_000_000) == 0.5, "Double, not Int (arm64_32)")
let e1 = IcomRadio.edges(vfo: 7_074_000, centre: 7_050_000, span: 100_000, fixed: true)
check(e1.lo == 7_000_000 && e1.hi == 7_100_000, "ticker axis = the fixed window")
let e2 = IcomRadio.edges(vfo: 7_074_000, centre: 7_050_000, span: 100_000, fixed: false)
check(e2.lo == 7_024_000 && e2.hi == 7_124_000, "centre mode: the axis follows the VFO")

// ── The FIRST VIEW on a VibeServer: no zoom on a radio Jr has never seen (it might be an Icom, lock off) ──
check(!IcomRadio.sendViewAtOpen(isVibe: true, knownIcom: false, seenBefore: false), "never seen → hold the zoom until the first config")
check(IcomRadio.sendViewAtOpen(isVibe: true, knownIcom: false, seenBefore: true), "seen before, not an Icom → zoom at once (ordinary connects unchanged)")
check(IcomRadio.sendViewAtOpen(isVibe: true, knownIcom: true, seenBefore: true), "a remembered Icom → its own span / reset path")
check(IcomRadio.sendViewAtOpen(isVibe: false, knownIcom: false, seenBefore: false), "UberSDR → unchanged")
check(IcomRadio.sendHeldViewOnFirstConfig(isIcom: false, shared: false), "first config: an ordinary per-listener SDR gets the held view")
check(!IcomRadio.sendHeldViewOnFirstConfig(isIcom: true, shared: false), "first config: an Icom (lock off, one listener) is adopted, never re-spanned")
check(!IcomRadio.sendHeldViewOnFirstConfig(isIcom: true, shared: true), "first config: an Icom, shared → adopted")
check(!IcomRadio.sendHeldViewOnFirstConfig(isIcom: false, shared: true), "first config: a shared dial → the joiner adopts")

print("\(n - fails)/\(n) passed")
exit(fails == 0 ? 0 : 1)
