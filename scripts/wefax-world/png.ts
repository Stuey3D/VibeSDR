// png.ts — the smallest PNG reader the WEFAX world harness needs (2026-10-07): 8-bit greyscale, grey+alpha, RGB,
// RGBA and palette, 1/2/4/8-bit, non-interlaced. Returns one Uint8Array of grey (0 black … 255 white) per line.
// ★ No npm dependency on purpose: a worktree has no node_modules, and the harness must run in any checkout.
import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

export interface Grey { width: number; height: number; rows: Uint8Array[] }

export function readPngGrey(path: string): Grey {
  const b = readFileSync(path);
  if (b.readUInt32BE(0) !== 0x89504e47) throw new Error(`${path}: not a PNG`);
  let p = 8, width = 0, height = 0, depth = 0, ctype = 0, interlace = 0;
  const idat: Buffer[] = [];
  let palette: Buffer | null = null;
  while (p < b.length) {
    const len = b.readUInt32BE(p), type = b.toString('latin1', p + 4, p + 8), d = b.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') { width = d.readUInt32BE(0); height = d.readUInt32BE(4); depth = d[8]; ctype = d[9]; interlace = d[12]; }
    else if (type === 'PLTE') palette = Buffer.from(d);
    else if (type === 'IDAT') idat.push(Buffer.from(d));
    else if (type === 'IEND') break;
    p += 12 + len;
  }
  if (interlace) throw new Error(`${path}: interlaced PNG not supported`);
  const chans = ({ 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 } as Record<number, number>)[ctype];
  if (!chans || (depth !== 8 && !(depth < 8 && (ctype === 0 || ctype === 3)))) throw new Error(`${path}: PNG type ${ctype}/${depth} not supported`);
  const raw = inflateSync(Buffer.concat(idat));
  const bpp = Math.max(1, (chans * depth) >> 3), stride = Math.ceil((width * chans * depth) / 8);
  const rows: Uint8Array[] = [];
  let prev = new Uint8Array(stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const cur = new Uint8Array(stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, up = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v = src[i];
      if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const pa = Math.abs(up - c), pb = Math.abs(a - c), pc = Math.abs(a + up - 2 * c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
      cur[i] = v & 255;
    }
    const g = new Uint8Array(width);
    for (let x = 0; x < width; x++) {
      if (depth < 8) {
        const bit = x * depth, v = (cur[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
        g[x] = ctype === 3 && palette ? lum(palette[v * 3], palette[v * 3 + 1], palette[v * 3 + 2]) : Math.round((v * 255) / ((1 << depth) - 1));
      } else if (ctype === 0 || ctype === 4) g[x] = cur[x * chans];
      else if (ctype === 3) { const v = cur[x]; g[x] = palette ? lum(palette[v * 3], palette[v * 3 + 1], palette[v * 3 + 2]) : v; }
      else g[x] = lum(cur[x * chans], cur[x * chans + 1], cur[x * chans + 2]);
    }
    rows.push(g);
    prev = cur;
  }
  return { width, height, rows };
}
const lum = (r: number, g: number, b: number) => Math.round(0.299 * r + 0.587 * g + 0.114 * b);

/** Resample every line to `w` pixels (area average), so a 1728-px NOAA product fills a 1809-px line as it would on
 *  air at 120 LPM / IOC 576. */
export function toWidth(img: Grey, w: number): Grey {
  if (img.width === w) return img;
  const k = img.width / w;
  const rows = img.rows.map((r) => {
    const o = new Uint8Array(w);
    for (let x = 0; x < w; x++) {
      const a = x * k, b = (x + 1) * k;
      let s = 0;
      for (let i = Math.floor(a); i < Math.ceil(b); i++) s += (Math.min(b, i + 1) - Math.max(a, i)) * (r[Math.min(i, img.width - 1)]);
      o[x] = Math.round(s / k);
    }
    return o;
  });
  return { width: w, height: img.height, rows };
}
