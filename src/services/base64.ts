/**
 * bytesToBase64 — the pair-table encoder from LocalAudioPlayer, shared.
 *
 * ★ OwrxAdapter and KiwiAdapter each carried a naive four-lookups-per-triple encoder and ran it
 *   on EVERY audio packet (20–50 a second, 4–8 KB of PCM each) on the JS thread. Two lookups per
 *   triple and chunked output is ~2× cheaper; the real fix — never letting PCM reach JS — is the
 *   native audio pump, which Android has and iOS does not yet.
 */
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_PAIRS: string[] = (() => {
  const t = new Array<string>(4096);
  for (let i = 0; i < 64; i++) for (let j = 0; j < 64; j++) t[(i << 6) | j] = B64[i] + B64[j];
  return t;
})();

export function bytesToBase64(b: Uint8Array): string {
  const n = b.length;
  const parts: string[] = [];
  const CHUNK = 3 * 2048;                 // a multiple of 3, so padding only ever occurs at the end
  for (let off = 0; off < n; off += CHUNK) {
    const end = Math.min(off + CHUNK, n);
    let out = '';
    let i = off;
    for (; i + 2 < end; i += 3) {
      const v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
      out += B64_PAIRS[v >> 12] + B64_PAIRS[v & 4095];
    }
    if (i < end) {
      const rem = end - i;
      const v = (b[i] << 16) | ((rem > 1 ? b[i + 1] : 0) << 8);
      out += B64_PAIRS[v >> 12] + (rem > 1 ? B64[(v >> 6) & 63] + '=' : '==');
    }
    parts.push(out);
  }
  return parts.join('');
}
