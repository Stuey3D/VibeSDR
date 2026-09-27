/**
 * ★★★ HOW FAR THE CHANNEL STANDS ABOVE ITS EMPTY NEIGHBOURS — the auto squelch's noise reference.
 *
 * Stuart, 2026-09-27, on 2 m NFM: auto squelch sat SHUT on a voice he had tuned to, and only came
 * right "when the transmission dropped then came back". The tracker learned the noise from the
 * channel ITSELF and reseeded on every retune from the very next reading — so tuning ONTO a signal,
 * which is how 2 m is used (you see it, you click it), taught it the voice as "noise" and parked the
 * threshold 12 dB above the voice for as long as the carrier stayed up.
 *   OpenWebRX has the same flaw (`smeter_level + 10` on a button press), but only when the listener
 *   presses it on a signal; ours re-learning on every retune made that the everyday case.
 *
 * ★★ THE CURE IS A REFERENCE THE SIGNAL CANNOT BE IN: same-width windows either side of the channel,
 *    on the spectrum the listener is LOOKING at. Stuart's instinct — "measure it based on zoom,
 *    especially if Auto IF filter when zooming is on" — is what makes this safe: the IF filter follows
 *    the view, so a neighbour inside the view is inside the filter. A neighbour OUTSIDE it would read
 *    the filter's attenuation, not the band's noise, and open the squelch on nothing.
 *
 * ★ Returned as an EXCESS (channel peak minus neighbour peak, both from the same bins), not a level,
 *   so every backend can use it in its own unit: "what my channel figure would read if the channel
 *   were empty" is `channelFigure - excess`, whether that figure is dBFS, a Kiwi's dBm or radiod SNR.
 *
 * ★ Windows from 2 to 6 channel widths out, both sides: the adjacent one is skipped because it holds
 *   the signal's own skirts. The 25th PERCENTILE of their peaks, not the minimum (a min of ten noise
 *   maxima is biased low, and would lower the threshold into the noise) and not the median (on a
 *   busy band most neighbours are occupied).
 *
 * @returns the excess in dB, or NaN when the view cannot answer honestly: the channel is off-screen
 *          (view unlocked and panned away), the bins are too coarse to resolve it, or fewer than
 *          three empty-able windows fit inside the view. NaN means "fall back to the tracker".
 */
export function channelExcessDb(
  bins: ArrayLike<number>, centerHz: number, spanHz: number,
  vfoHz: number, lowHz: number, highHz: number,
): number {
  const n = bins.length;
  if (n < 16 || !(spanHz > 0)) return NaN;
  const binHz = spanHz / n;
  const widthHz = highHz - lowHz;
  // ★ At least four bins across the channel, or its peak and a neighbour's are the same few coarse,
  //   peak-held bins and the difference between them means nothing.
  if (!(widthHz > 0) || widthHz / binHz < 4) return NaN;
  const startHz = centerHz - spanHz / 2;
  // ★ The outermost 2 % each side is the display's own roll-off, not the band.
  const edge = Math.ceil(n * 0.02);
  const peakIn = (loHz: number): number => {
    const a = Math.floor((loHz - startHz) / binHz);
    const b = Math.ceil((loHz + widthHz - startHz) / binHz);
    if (a < edge || b > n - edge) return NaN;
    let pk = -Infinity;
    for (let i = a; i < b; i++) if (bins[i] > pk) pk = bins[i];
    return pk;
  };
  const chan = peakIn(vfoHz + lowHz);
  if (!Number.isFinite(chan)) return NaN;
  const nb: number[] = [];
  for (let m = 2; m <= 6; m++) {
    for (const s of [-1, 1]) {
      const v = peakIn(vfoHz + lowHz + s * m * widthHz);
      if (Number.isFinite(v)) nb.push(v);
    }
  }
  if (nb.length < 3) return NaN;
  nb.sort((x, y) => x - y);
  return chan - nb[Math.floor((nb.length - 1) * 0.25)];
}

/** ★ How far above the neighbour reference the tracked baseline may sit before it is pulled down.
 *   The channel's own noise can legitimately read a little above its neighbours (the VFO sits near
 *   the tuner's DC offset, and the neighbour figure is a low percentile); beyond this it is a signal. */
export const SQL_NEAR_CEIL_DB = 3;
/** ★ The reference is smoothed before it caps anything: frame to frame it wobbles by the noise's own
 *   spread, and capping on every dip would drag the baseline down into the noise and chatter. ~1 s. */
export const SQL_NEAR_SMOOTH = 0.1;
