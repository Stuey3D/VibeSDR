// sec_sdr.h — MENU → SDR HARDWARE, per radio driver (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
// The web handheld page's SDR HARDWARE accordion (the #menu RADIO range, handheld.ts buildHandheldMenu), drawn for the
// radio that is plugged in: branch on hwinfo radio.driver (rtl · sdrplay · airspyhf · airspy · hackrf · usrp · icom) and
// draw only the controls THAT radio has (AGENTS.md: a control that only works on one radio should not be there).
// Spec: docs/v12/R36S-NATIVE-MENU-INVENTORY.md §4a. Rows in the web markup's order:
//   RECEIVER · GAIN · VIBE AGC · [RSP | HF+ | R2 | USRP | HackRF panel] · SAMPLE RATE · IF FILTER · PPM · AUTO PPM · BIAS-T ·
//   DIGITAL AGC · DIRECT SAMPLING · ANTENNA
// ★★ NOTHING IS SENT WHEN THE MENU IS BUILT, except the HF+'s four operating settings once per radio (it reports none
//    of them — sdr_more.h). Every other row reads what the radio reports; the web's page-build re-sends (bias-T, RSP
//    LNA/AGC/notches, HackRF LNA/VGA, Airspy mode) are deliberately not repeated: the native UI keeps no per-radio
//    prefs, and the radio is the authority on its own state (shared_dial_contract).
// ★ The .mOnly rows (VFO CENTRE, CENTRE, FIT) belong to DISPLAY, not here.
#pragma once
// (included by app.h after App is declared)

#include "sdr_ui.h"
#include "sdr_common.h"
#include "sdr_rsp.h"
#include "sdr_more.h"

namespace hh {

inline float App::secSdr(Ctx& c, const RadioState& s, Rect a, float y) {
    sdr::S x{*this, c, s, a, nowSec()};
    const std::string& d = s.hwDriver;
    sdr::receiverRow(x, y);
    if (d.empty()) return sdr::note(x, y, s.connected ? "Waiting for the radio to describe itself." : "No radio connected.");
    if (d == "icom") {
        // ★ USB SDRs only on the R36S (handheld.ts:289); the Icom panel (icomUi.ts) is not ported.
        return sdr::note(x, y, "This radio's own controls are not on the handheld.");
    }
    if (d == "rtl" || d == "airspy" || d == "usrp") sdr::gainRows(x, y);
    if (d == "sdrplay") sdr::rspRows(x, y);
    else if (d == "airspyhf") sdr::ahfRows(x, y);
    else if (d == "airspy") sdr::airspyRows(x, y);
    else if (d == "usrp") sdr::usrpRows(x, y);
    else if (d == "hackrf") sdr::hackrfRows(x, y);
    if (d != "airspyhf") sdr::rateRow(x, y);
    if (d == "rtl") sdr::ifFilterRow(x, y);
    if (d == "rtl" || d == "sdrplay") sdr::ppmRow(x, y);
    sdr::afcRows(x, y);   // ★★ V12 AUTO PPM — any radio the engine says can use it (after the manual PPM it sits on top of)
    if (d == "rtl") sdr::rtlRows(x, y);
    sdr::antennaRows(x, y);
    return y;
}


}  // namespace hh
