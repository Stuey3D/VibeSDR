import { Inflate } from 'pako';

/** 8 MB of text. The largest real control message (a config with band plans) is tens of KB. */
export const INFLATE_CAP = 8 * 1024 * 1024;

/**
 * ★★★ GUNZIP WITH A CEILING (security pass, 2026-10-03). The spectrum socket's JSON control messages arrive
 * gzipped, and `ungzip()` inflates whatever it is given in one go: a few KB from a hostile server can expand
 * to gigabytes and take the app down (a "gzip bomb"). This streams through pako's Inflate and gives up the
 * moment the output passes `cap` — throwing, so the caller's guard() drops the frame and counts it, as it
 * already does for a corrupt one.
 * ★ Counted in UTF-16 units of the decoded text, which never exceed the bytes inflated.
 */
export function ungzipToStringCapped(bytes: Uint8Array, cap = INFLATE_CAP): string {
  const inf = new Inflate({ to: 'string' });
  const parts: string[] = [];
  let total = 0;
  inf.onData = (chunk) => {
    const s = chunk as unknown as string;
    total += s.length;
    if (total > cap) throw new Error(`inflated frame exceeds ${cap} bytes — dropped`);
    parts.push(s);
  };
  inf.onEnd = () => {};
  const pushed = inf.push(bytes, true);
  if (inf.err) throw new Error(`inflate failed: ${inf.msg || inf.err}`);
  // ★ A truncated stream sets no error; it just never reaches its end. Half a message is not a message.
  if (!pushed || !(inf as unknown as { ended?: boolean }).ended) throw new Error('inflate: incomplete gzip frame');
  return parts.join('');
}
