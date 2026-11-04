// vibe_icom_decode.h — what an Icom must be set to so that a decoder hears its signal (V12, receive only).
//
// ★★★ THE DECODERS ARE THE SAME ON BOTH KINDS OF RADIO (Stuart, 2026-11-04). On an SDR a listener tunes the VFO over the
//     signal and presses the decoder; the decoder reads that listener's demodulated audio, which they go on HEARING
//     (FT8, SSTV, WEFAX, RTTY — "you still hear it"). An Icom gives us exactly that audio — its own demodulator's, on its
//     USB codec — and it already reaches the decoders (onIcomPcm → onAudio → feedShared). What differs is only that the
//     radio's OWN mode and filter decide what the audio contains: a 500 Hz CW filter or an AM detector is no use to an
//     FSK decoder. So on an Icom, pressing a decoder changes ONLY what is needed to deliver the tuned signal as audio:
//       • the mode — a sideband, where the radio is in something that is not one (AM / FM → USB; CW / RTTY → the
//         sideband on the same side). ★ NOT the DATA form: DATA (1A 06) only chooses the TRANSMIT audio source — the
//         7300's USB codec carries the receive audio in USB and USB-D alike — and SSB-D has filter presets of its own,
//         which owner guides say ship narrower than FT8 needs ("long-press FIL1, set 3.0 kHz"). A listener already in
//         USB-D / LSB-D (an owner's FT8 setup) is left in it;
//       • the filter — FIL1, the widest (factory 3.0 kHz in SSB, icom_models.h — 200–3000 Hz is FT8's whole slot);
//       • and NEVER the dial, except where the radio's mode gives the dial a different meaning from the sideband the
//         decoder needs (CW / RTTY: the dial is the SIGNAL, not a carrier) — then the radio is moved by the decoder's
//         own audio pitch so the signal the listener tuned lands where the decoder listens, and the readout keeps
//         showing what they tuned (`offsetHz`: shown = radio + offsetHz).
//     The audio stays AUDIBLE — no decoder that is heard on an SDR is muted on an Icom. All of it is put back when the
//     decoder stops (the shim's IcomDecodeHold).
//
// ★★★ WHAT ONE AUDIO CHANNEL CANNOT DO is not offered at all (AGENTS.md — never a control that only works on one radio):
//     every decoder that reads IQ rather than audio (the CW and PSK SKIMMERS across a segment, digital spots BAND,
//     PAGING), the whole-radio modes (DAB, ADS-B, ISM, NDB, AIS — already off on an Icom), and DRM, which needs
//     ≥ 4.5 kHz of RF with its phase intact: the IC-7300's widest SSB / SSB-D IF filter is 3.6 kHz, and AM's 10 kHz goes
//     through an envelope detector (docs: the report on branch v12-icom-decoders).
//
// ★ Pure — no shim, no radio: tested on its own (vibeserver/test-icom-decode.cpp).
#pragma once
#include <string>

namespace vibeicomdec {

/** A decoder the Icom cannot run at all: "" = it can, else the plain words a refusal carries. */
inline std::string notOfferedWhy(const std::string& ext) {
    if (ext == "cwskim" || ext == "pskskim")
        return "the skimmer needs a wideband capture \xe2\x80\x94 an Icom gives one demodulated audio channel (use ONE SIGNAL)";
    if (ext == "paging") return "paging needs the radio's IQ, and an Icom has none";
    if (ext == "drm") return "DRM needs at least 4.5 kHz of RF with its phase intact \xe2\x80\x94 the radio's widest SSB filter is 3.6 kHz";
    return {};
}

/** Where in the AUDIO a decoder listens (Hz) — the pitch the tuned signal must come out at when the radio has to be
 *  moved (CW / RTTY modes). `msgCentre` is the attach message's own centre_frequency when it sent one (≤ 0 = none). */
inline double audioCentreFor(const std::string& ext, double msgCentre) {
    if (msgCentre > 0) return msgCentre;
    if (ext == "navtex") return 500.0;                       // vibe_decoder_host.h: NAVTEX's default centre
    if (ext == "wefax") return 1900.0;                       // the carrier the clients send
    if (ext == "sstv") return 1900.0;                        // 1500–2300 Hz, the middle
    if (ext == "ft8" || ext == "spots") return 1500.0;       // the 200–3000 Hz slot, the middle
    if (ext == "hell") return 980.0;
    if (ext == "dsc") return 1700.0;                         // vibe_decoder_host.h: MF/HF DSC's centre
    return 1000.0;                                           // RTTY, SITOR-A, MFSK / Olivia / THOR, JTTY, PSK, SELCAL
}

struct Plan {
    bool ok = true;            ///< false = not offered on an Icom (`why`)
    std::string why;
    std::string mode;          ///< the radio mode to set ("" = keep the one it is in)
    int fil = 0;               ///< the filter to set (0 = keep)
    long offsetHz = 0;         ///< shown = radio + offsetHz while it runs (0 = the dial is not touched)
    bool changes() const { return !mode.empty() || fil > 0 || offsetHz != 0; }
};

/** ★★ THE PLAN for decoder `ext` with the radio in `radioMode` (our names: usb, usbd, lsb, lsbd, am, fm, cw, cwr, rtty,
 *  rttyr) on filter `fil`. `centreHz` = audioCentreFor(). "spots" / "ft8" is DIGITAL SPOTS: SINGLE. */
inline Plan plan(const std::string& ext, const std::string& radioMode, int fil, double centreHz) {
    Plan p;
    if (const std::string w = notOfferedWhy(ext); !w.empty()) { p.ok = false; p.why = w; return p; }
    if (radioMode.empty()) return p;                         // not read yet: nothing to judge, nothing changed
    const std::string& m = radioMode;
    const bool ssb = m == "usb" || m == "lsb", data = m == "usbd" || m == "lsbd";
    const bool cwMode = m == "cw" || m == "cwr", rttyMode = m == "rtty" || m == "rttyr";
    // ★ The time signals are read in whatever the listener tunes them in (MSF / DCF77: CW or AM), as on an SDR.
    if (ext == "time" || ext == "msf" || ext == "dcf77" || ext == "rwm" || ext == "wwv" || ext == "wwvb" || ext == "jjy") return p;
    // ★ CW: CW itself, or any sideband (a wider USB filter lists every CW signal in it) — kept. From AM / FM / RTTY,
    //   the radio's CW mode at its widest filter (the dial is then the signal, as the listener tuned it).
    if (ext == "cw") {
        if (cwMode || ssb || data) return p;
        p.mode = "cw"; p.fil = 1; return p;
    }
    // ★ VHF APRS (1200 Bd) and DSC channel 70 are FM signals: the radio's FM is what delivers them.
    if ((ext == "aprs" || ext == "dsc") && m == "fm") return p;
    // ★★ Everything else is tones in an SSB channel: RTTY, NAVTEX, SITOR-A, WEFAX, SSTV, PSK, MFSK / Olivia / THOR,
    //    Feld Hell, JTTY, SELCAL, SAME, HF DSC / APRS, and DIGITAL SPOTS (FT8 / FT4 …).
    if (ssb || data) { if (fil != 1) p.fil = 1; return p; }               // the listener's own sideband: only FIL1
    if (m == "am" || m == "fm") { p.mode = "usb"; p.fil = 1; return p; }   // the dial is the carrier in both: no move
    if (cwMode || rttyMode) {
        /* ★ The dial is the SIGNAL here (CW: what zero-beats at the pitch; RTTY: the 7300 reads its mark-space pair
         *  there). USB-D for CW and RTTY-R (the upper side), LSB-D for CW-R and RTTY (an Icom's RTTY is LSB-sided), with
         *  the radio moved by the decoder's pitch so the tuned signal comes out at `centreHz`:
         *    USB: audio = rf - radio → radio = shown - centre (offset +centre);
         *    LSB: audio = radio - rf → radio = shown + centre (offset -centre). */
        const bool upper = m == "cw" || m == "rttyr";
        p.mode = upper ? "usb" : "lsb"; p.fil = 1;
        p.offsetHz = upper ? (long)centreHz : -(long)centreHz;
        return p;
    }
    return p;   // a mode we do not know: leave the radio alone
}

}  // namespace vibeicomdec
