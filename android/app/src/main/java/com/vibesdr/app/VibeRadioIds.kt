package com.vibesdr.app

/**
 * ★★ WHICH USB DEVICES ARE RADIOS WE SERVE — moved out of VibeLocalSdrModule's companion (beta88) so the SERVER process
 * can ask without loading a React Native class: VibeServerRestore and the service run there, and touching the module's
 * companion initialises the module class, whose superclass is React Native's. The module's companion now points here.
 */
object VibeRadioIds {
    // RTL-SDR VID/PID allowlist (from SDR++ Brown), packed as (vid<<16)|pid.
    // internal, not private: VibeServerRestore matches on the SAME list — a copy
    // would drift and quietly stop recognising a dongle on the restore path only.
    internal val RTL_SDR_VIDPIDS: Set<Int> = listOf(
        0x0bda to 0x2832, 0x0bda to 0x2838, 0x0413 to 0x6680, 0x0413 to 0x6f0f,
        0x0458 to 0x707f, 0x0ccd to 0x00a9, 0x0ccd to 0x00b3, 0x0ccd to 0x00b4,
        0x0ccd to 0x00b5, 0x0ccd to 0x00b7, 0x0ccd to 0x00b8, 0x0ccd to 0x00b9,
        0x0ccd to 0x00c0, 0x0ccd to 0x00c6, 0x0ccd to 0x00d3, 0x0ccd to 0x00d7,
        0x0ccd to 0x00e0, 0x1554 to 0x5020, 0x15f4 to 0x0131, 0x15f4 to 0x0133,
        0x185b to 0x0620, 0x185b to 0x0650, 0x185b to 0x0680, 0x1b80 to 0xd393,
        0x1b80 to 0xd394, 0x1b80 to 0xd395, 0x1b80 to 0xd397, 0x1b80 to 0xd398,
        0x1b80 to 0xd39d, 0x1b80 to 0xd3a4, 0x1b80 to 0xd3a8, 0x1b80 to 0xd3af,
        0x1b80 to 0xd3b0, 0x1d19 to 0x1101, 0x1d19 to 0x1102, 0x1d19 to 0x1103,
        0x1d19 to 0x1104, 0x1f4d to 0xa803, 0x1f4d to 0xb803, 0x1f4d to 0xc803,
        0x1f4d to 0xd286, 0x1f4d to 0xd803
    ).map { (vid, pid) -> (vid shl 16) or pid }.toSet()

    /** Airspy HF+ — see isAirspyHf(). Mirrored in res/xml/device_filter.xml (DECIMAL there). */
    internal const val AIRSPYHF_VID = 0x03eb
    internal const val AIRSPYHF_PID = 0x800c

    /** HackRF One — see isHackRf(). Mirrored in res/xml/device_filter.xml (DECIMAL there). */
    internal const val HACKRF_VID = 0x1d50
    internal const val HACKRF_PID = 0x6089

    /** ★ Airspy R2 / Mini — ONE id for both models; the board id read at open tells them
     *  apart (see airspy_source.cpp). Mirrored in device_filter.xml and in the engine's
     *  start() dispatch: three copies of one fact, and they must agree. */
    internal const val AIRSPY_VID = 0x1d50
    internal const val AIRSPY_PID = 0x60a1

    /** ★ Any radio a server can be running on — the same four allowlists as isSupportedRadio(),
     *  reachable without a module instance. The service asks it of a USB device that has just
     *  DETACHED, to know whether the server's radio is what left (VibeServerRestore.noteRadioGone). */
    internal fun isServableRadio(vid: Int, pid: Int): Boolean =
        VibeRsp.isRsp(vid, pid) ||      // ★ API 26+ only — VibeRsp.supported
        RTL_SDR_VIDPIDS.contains((vid shl 16) or pid) ||
        (vid == AIRSPYHF_VID && pid == AIRSPYHF_PID) ||
        (vid == HACKRF_VID && pid == HACKRF_PID) ||
        (vid == AIRSPY_VID && pid == AIRSPY_PID)
}
