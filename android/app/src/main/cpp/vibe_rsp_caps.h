// WHAT EACH RSP MODEL HAS — the front-end controls, as one table (2026-10-22, the RSP setup audit).
//
// ★★★ AGENTS.md: "a control that only works on one radio should not be there." The RSP family does not share a
//     front end, and every panel — the app's hardware panel, the web client, the setup page — draws a control only
//     when the radio's hwinfo says it has it. hwinfo is written from SdrplaySource::has*(), which read this table.
//     It used to be six switch statements spread through sdrplay_source.cpp; one table is what a test can hold.
// ★ Sources: SDRplay's API 3.15 headers (vendor/openrsp/include) and SoapySDRPlay3. Only the RSP1A and RSP1B have
//   been on air here; the rest are as the headers describe them (✗ untested on hardware).
// ★★ THE SETUP PAGE HAS A COPY for a radio that is not running (rspCaps in vibe_setup_page.h, keyed by model name),
//    and scripts/check-rsp-caps.mjs holds that copy to this table — one rule, two readers, checked.
//
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-rsp-caps.cpp
#pragma once
#include <string>
#include <vector>

namespace vibe {
namespace rspcaps {

/** SDRplay's hwVer codes (sdrplay_api.h SDRPLAY_*_ID) — numbers here so this header needs no API header;
 *  sdrplay_source.cpp static_asserts them against the real ones. */
enum HwVer : int { kRsp1 = 1, kRsp2 = 2, kRspDuo = 3, kRspDx = 4, kRsp1B = 6, kRspDxR2 = 7, kRsp1A = 255 };

struct Caps {
    const char* model;       // as printed on the case, and as hwinfo's radio.model reports it
    bool rfNotch;            // broadcast FM/MW notch
    bool dabNotch;           // Band III notch
    bool biasT;
    bool hdr;                // dx family: high dynamic range below 2 MHz
    bool amNotch;            // RSPduo: AM notch on tuner 1's Hi-Z port
    bool extRef;             // 24 MHz reference output
    std::vector<std::string> ports;   // empty = one socket, no selector
};

inline Caps forHwVer(int hwVer) {
    switch (hwVer) {
        //                     model       rf     dab    biasT  hdr    am     extRef ports
        case kRsp1:   return { "RSP1",     false, false, false, false, false, false, {} };
        case kRsp1A:  return { "RSP1A",    true,  true,  true,  false, false, false, {} };
        case kRsp1B:  return { "RSP1B",    true,  true,  true,  false, false, false, {} };
        // ★ The RSP2 has no DAB notch, and its Hi-Z port is the AM port (amPortSel), drawn as a third socket.
        case kRsp2:   return { "RSP2",     true,  false, true,  false, false, true,  {"A", "B", "Hi-Z"} };
        // ★★ The Duo's "antenna" is a TUNER (single-tuner mode); Hi-Z is tuner 1's AM port. A change re-opens it.
        case kRspDuo: return { "RSPduo",   true,  true,  true,  false, true,  true,
                               {"Tuner 1 50Ω", "Tuner 1 Hi-Z", "Tuner 2 50Ω"} };
        case kRspDx:  return { "RSPdx",    true,  true,  true,  true,  false, false, {"A", "B", "C"} };
        case kRspDxR2:return { "RSPdx-R2", true,  true,  true,  true,  false, false, {"A", "B", "C"} };
        default:      return { "RSP",      false, false, false, false, false, false, {} };
    }
}

}  // namespace rspcaps
}  // namespace vibe
