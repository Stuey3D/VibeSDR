/**
 * FM broadcast tuning limits for the FM-DX Webserver backend.
 *
 * ★ THE SERVER TELLS US NOTHING. fm-dx-webserver keeps its tuning range in
 * config.json (`webserver.tuningLimit` / `tuningLowerLimit` / `tuningUpperLimit`,
 * MHz) and exposes it in NO machine-readable form: it is absent from
 * /static_data, from /api and from the /text WebSocket payload. The only trace
 * is a rendered string on the index page — and only when a limit is switched on.
 * So "read the range from the server and adapt" is not available to us; the
 * `refine from /static_data later` note in FmdxAdapter was never achievable.
 *
 * So TUNING is bounded by the RECEIVER instead of by a region: a TEF6686 sweeps
 * 64–108 MHz continuously, covering OIRT (65.9–74), the Japanese band (76–95)
 * and the owner who parks below 87.5 — the case that started this, a user on
 * 84 MHz whom our 87.5 floor clamped out of reach.
 *
 * The DIAL is a separate question and stays 87.5–108, the right scale for very
 * nearly every server. It stretches down only to where the server has been SEEN
 * to tune (TunerScreen's `confirmedLo`), never to where we merely asked it to:
 * an out-of-range T<kHz> is dropped silently by the server, our display snaps
 * back a second later, and a dial stretched by that attempt would leave the user
 * staring at 20 MHz of nothing.
 *
 * Overshooting an owner's limit is therefore safe by construction — the command
 * simply doesn't take, and TunerScreen's convergence timeout says so.
 */

/** Full tuning sweep of the TEF6686 family — the widest any FM-DX server reaches,
 *  and the only bound we put on a tune command. */
export const FMDX_TUNE_LO = 64_000_000;
export const FMDX_TUNE_HI = 108_000_000;

/** The dial's resting extent: the ITU-R1 band almost every server uses. It is a
 *  FLOOR ON THE DISPLAY, not on tuning, and grows downward on confirmation. */
export const FMDX_DIAL_VIEW_LO = 87_500_000;
export const FMDX_DIAL_VIEW_HI = 108_000_000;
