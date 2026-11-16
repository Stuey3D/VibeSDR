// vibe_paging_gate.h — WHERE PAGING MAY RUN, as one pure rule (V12, 2026-11-16). The shim asks it twice: may this
// engine offer paging at all (vibeserver.json `"paging":true`, LocalSdrShim::pagingCapable), and may THIS decoder socket
// start it (the attach handler). test-paging-gate.cpp walks every combination.
//
// ★★★ LOCAL HARDWARE ONLY (docs/v12/DECODERS-WAVE2.md "PAGING"): the user's own receiver, heard on that device's own
//     screen — never a server, never a LAN, tunnel or proxy client, nothing to the gallery or a log. Pager decoding is
//     legal in some countries and not in others (the UK, for one), and some of the traffic is medical. Two places it
//     runs, both the user's own device with its own radio:
//       · the Android CLIENT app's library (VIBE_PAGING, never VibeServer Lite) — the phone's radio, in the phone's app;
//       · the R36S handheld's radio process (`vibeserver --handheld-radio`, LocalSdrShim::setHandheld) — the handheld's
//         radio, on the handheld's own screen (vibe-hhui). It binds 127.0.0.1 only and the supervisor in front of it
//         (handheld.cpp) binds 127.0.0.1 only too.
// ★★ EACH LOCK IS ENOUGH ON ITS OWN:
//     1. built      — the engine is compiled in (VIBE_PAGING);
//     2. process    — this process is one of the two above (DecoderHost::pagingProcess: false in every server process);
//     3. ownDevice  — the Android app, or the handheld profile (g_vsHandheld) — said again, independently of 2;
//     4. !serving   — the engine is not serving (g_serveOnLan false: it is bound to 127.0.0.1, so no LAN, tunnel or proxy
//                     client can exist — the handheld child calls setServeOnLan(false));
//     5. usbRadio   — a radio plugged into this device, with our own IQ: not rtl_tcp, SpyServer or an Icom (somebody
//                     else's receiver, or not one we demodulate);
//     6. loopback   — (attach only) the decoder socket's peer is 127.0.0.1 / ::1;
//     7. !vfoB      — (attach only) never on VFO B (one receiver, the dial the user is looking at).
#pragma once

namespace vibe {

/** May this engine offer paging at all? (vibeserver.json `"paging":true` + `pagingModes` only when true.) */
inline bool pagingGateEngine(bool built, bool process, bool ownDevice, bool serving, bool usbRadio) {
    return built && process && ownDevice && !serving && usbRadio;
}

/** May this decoder socket start paging? Everything the engine needs, AND a loopback peer, AND not VFO B. */
inline bool pagingGateAttach(bool engineOk, bool loopbackPeer, bool vfoB) {
    return engineOk && loopbackPeer && !vfoB;
}

/** The refusal's words, where the decoder's output would have been: the phone's app, the handheld, or a server. */
inline const char* pagingRefusalText(bool handheld, bool android) {
    if (handheld) return "Paging runs only on this handheld's own screen, on a radio plugged into it.";
    if (android) return "Paging runs only in the VibeSDR app, on a radio plugged into this phone.";
    return "Paging is never decoded by a server — only on a radio plugged into your own phone or R36S handheld.";
}

/** ★★★ THE SPEAKER WHILE PAGING RUNS (the R36S engine's speaker, local_sdr_shim.cpp sendWs): silence while any paging
 *  receiver runs in this process, the person's own level otherwise — which is never changed, so it comes back as it was
 *  whenever paging stops. */
inline float pagingSpeakerLevel(float level, int pagingRunning) { return pagingRunning > 0 ? 0.0f : level; }

}  // namespace vibe
