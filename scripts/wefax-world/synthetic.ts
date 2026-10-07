// synthetic.ts — chart STYLES the world's WEFAX stations send that no committed real image covers yet (2026-10-07).
// Each is drawn phased and upright at `width` px, deterministic (fixed seed per style). See docs/WEFAX-WORLD-STATIONS.md
// for which stations send which. Real images always beat these: a synthetic chart only says what we THINK a format is.
import type { Grey } from './png.ts';

export function synthetic(name: string, W: number): Grey {
  let seed = [...name].reduce((a, c) => (a * 31 + c.charCodeAt(0)) & 0x7fffffff, 7);
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const H = 1250;
  const paperOf = (s: string) => (s.startsWith('black') ? 12 : 250), inkOf = (s: string) => (s.startsWith('black') ? 235 : 20);
  const paper = paperOf(name), ink = inkOf(name);
  const rows = Array.from({ length: H }, () => new Uint8Array(W).fill(paper));
  const dot = (x: number, y: number, v = ink) => { y = Math.round(y); if (y >= 0 && y < H) rows[y][((Math.round(x) % W) + W) % W] = v; };
  const thick = (x: number, y: number, w = 2, v = ink) => { for (let k = 0; k < w; k++) dot(x + k, y, v); };
  // a "coast": a wandering polyline, drawn 2 px wide
  const coast = (x0: number, x1: number, y0: number, y1: number) => {
    let x = x0 + rnd() * (x1 - x0), dx = 0;
    for (let y = y0; y < y1; y++) { dx = Math.max(-3, Math.min(3, dx + (rnd() - 0.5))); x = Math.max(x0, Math.min(x1, x + dx)); thick(x, y); }
  };
  const isobar = (cx: number, amp: number, per: number, x0: number, x1: number) => {
    for (let y = 0; y < H; y++) { const x = cx + amp * Math.sin(y / per + cx); if (x >= x0 && x <= x1) thick(x, y, 2); }
  };
  const textBlock = (x0: number, x1: number, y0: number, lines: number, pitch = 22, fill = 0.35) => {
    for (let l = 0; l < lines; l++) for (let x = x0; x < x1; x += 14) {
      if (rnd() < 0.12) continue;                                              // spaces between words
      for (let y = 0; y < 12; y++) for (let k = 0; k < 10; k++) if (rnd() < fill) dot(x + k, y0 + l * pitch + y);
    }
  };
  const grid = (x0: number, x1: number, every: number, lean: number) => {
    for (let gx = x0 + every / 2; gx < x1; gx += every) for (let y = 0; y < H; y++) { const x = gx + lean * (y - H / 2); if (x > x0 && x < x1) thick(x, y, 2); }
    for (let y = 60; y < H; y += every) for (let x = x0; x <= x1; x++) dot(x, y), dot(x, y + 1);
  };
  const header = (x0: number, x1: number) => textBlock(x0, x1, 4, 2, 22, 0.45);

  if (name === 'edge-map' || name === 'black-edge-map') {
    // ★ A map drawn to the very ends of the line, no frame, no white border (NOAA/JMA-style edge-to-edge analysis).
    header(0, W);
    grid(0, W - 1, 300, 0.05);
    for (let i = 0; i < 6; i++) coast(rnd() * W * 0.8, W, 60, H);
    for (const c of [200, 640, 1010, 1420, 1700]) isobar(c, 90, 70 + rnd() * 60, 0, W - 1);
    for (let i = 0; i < 30; i++) textBlock(rnd() * (W - 120), 0, 60 + rnd() * (H - 120), 1);
    for (let i = 0; i < 25; i++) { const x = rnd() * W, y = 60 + rnd() * (H - 120); textBlock(x, x + 70, y, 1); }
  } else if (name === 'black-bordered') {
    // ★ An inverted chart: black paper, white frame + lines (some stations' satellite / analysis products).
    header(60, W - 60);
    for (let y = 40; y < H - 20; y++) { thick(60, y, 3); thick(W - 63, y, 3); }
    grid(64, W - 64, 260, 0.03);
    for (let i = 0; i < 5; i++) coast(80, W - 80, 40, H - 20);
    for (const c of [300, 800, 1300]) isobar(c, 80, 90, 64, W - 64);
  } else if (name === 'satellite' || name === 'satellite-framed') {
    // ★ A grey-scale cloud image (NOAA "SATELLITE IMAGE", JMA/KMA/BoM imagery): every column carries picture,
    //   smooth grey from black sea to white cloud, a lat/lon grid, a header strip. Framed: a white surround.
    const framed = name === 'satellite-framed', x0 = framed ? 90 : 0, x1 = framed ? W - 90 : W;
    const waves = Array.from({ length: 14 }, () => [rnd() * 0.02 + 0.002, rnd() * 0.02 + 0.002, rnd() * 6.28, 0.3 + rnd()]);
    for (let y = 40; y < H; y++) for (let x = x0; x < x1; x++) {
      let v = 0; for (const [a, b, p, g] of waves) v += g * Math.sin(a * x + b * y + p);
      rows[y][x] = Math.max(0, Math.min(255, Math.round(110 + 32 * v + (rnd() - 0.5) * 30)));
    }
    for (let gx = x0 + 150; gx < x1; gx += 300) for (let y = 40; y < H; y++) if ((y >> 2) & 1) thick(gx + 0.04 * y, y, 2, 255);
    textBlock(x0, x1, 4, 1, 22, 0.5);
  } else if (name === 'text-page') {
    // ★ A schedule / notice page: lines of text on white, no frame, ragged right edge.
    header(100, 1700);
    for (let p = 0; p < 40; p++) textBlock(110, 110 + 400 + rnd() * 1180, 70 + p * 28, 1, 22, 0.4);
  } else if (name === 'test-pattern') {
    // ★ NOAA's TEST PATTERN: grey-scale bars across the whole line, then a block of resolution lines.
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      if (y < 600) rows[y][x] = Math.round((Math.floor((x / W) * 16) / 15) * 255);
      else rows[y][x] = ((x >> (2 + ((y >> 6) & 3))) & 1) ? 250 : 20;
    }
  } else throw new Error(`synthetic: unknown style ${name}`);
  return { width: W, height: H, rows };
}
