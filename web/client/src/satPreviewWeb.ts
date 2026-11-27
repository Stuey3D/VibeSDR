/* satPreviewWeb.ts — ★★★ beta97 PASS PREVIEW in the web page's SATELLITE box (Stuart 11-27). The rules and words are
 * src/services/satPreview.ts (one set with the app); this file draws them.
 *
 *  ▶ on a future pass row → the box scrolls so the preview is in view, then (a short beat later, so the start is not
 *  missed) the pass plays: the bird along the server's own track on the half-globe (the same painter as the live box,
 *  globePaint.ts) with its footprint, the trail coloured by this station's reception history, and the same pass on a
 *  small polar sky plot over the history's heat map (where to POINT). Readouts: the pass's real clock (UTC · local),
 *  elevation / azimuth, range, the Doppler at the row's downlink; AOS / MAX / LOS and the duration; the one-line
 *  verdict. It stops at LOS (▶ replays); ❚❚ pauses, the bar scrubs, 0.5× / 1× / 2×. ✕, the row's button again or back
 *  (Escape / a pad's back) returns to the live view. One preview at a time; a pass really happening on the pick takes
 *  priority (the preview stops and says LIVE PASS NOW).
 *
 *  ★★ THE PERF RULE: frames only while it plays (requestAnimationFrame, stopped when paused, ended, hidden or folded);
 *     everything that never moves — dome, land, dotted orbit, chevrons, the receiver — is painted ONCE into an offscreen
 *     canvas and each frame is that picture plus the moving parts; the sky plot is a static SVG whose dot moves by a
 *     transform. */
import { globeFrame, globeLabels, globeLayers, globeScene, type SatTrack } from '../../../src/services/satellites';
import {
  passHistoryKinds, passHistorySource, passKey, passMomentsText, passReadout, passSampleAt, passSkyPaths, passStripText, passTrackFrom,
  passTrailRuns, passVerdict, PassPlayer, PASS_GRADE_CSS, PASS_KEY_TEXT, PASS_LIVE_NOW, PASS_PREVIEW_STOP, PASS_SPEEDS, PASS_START_DELAY_MS, PASS_VERDICT_ROWS,
  passPreviewPress, previewId, skyHistory, type PassGrade, type PassPreviewRow, type PassTrack, type PassVerdict, type SkyHistory,
} from '../../../src/services/satPreview';
import { parseSkyPlotJson, skyPlotQuery, skyPlotScene, skyPlotXY } from '../../../src/utils/skyPlot';
import { paintGlobe } from './globePaint';
import { pushBack } from './gamepad';

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' } as Record<string, string>)[c]);

export const PREVIEW_CSS = `
#satPanel.pv .sat-box{display:none !important}
#satPanel .sat-pv{border:1px solid var(--btn-border);border-radius:8px;padding:6px 8px 8px;margin:0 0 6px}
#satPanel .sat-pv[hidden],#satPanel .sat-pvnote[hidden]{display:none}
#satPanel .sat-pvh{display:flex;align-items:center;gap:8px;margin:0 0 4px}
#satPanel .sat-pvs{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--amber);letter-spacing:.04em;font-size:12px}
#satPanel .sat-pvh button{min-height:28px;padding:1px 10px}
#satPanel .sat-pvg{display:flex;gap:8px;align-items:center;justify-content:center}
#satPanel .sat-pvg canvas{flex:1 1 auto;min-width:0;width:100%;max-width:420px;height:auto;aspect-ratio:640/300;display:block}
#satPanel .sat-pvsky{flex:0 0 auto;width:118px;color:var(--btn-text)}
#satPanel.big .sat-pvsky{width:170px}
#satPanel .sat-pvsky svg{display:block;width:100%;height:auto}
#satPanel .sat-pvro{display:grid;grid-template-columns:1fr 1fr;gap:0 10px;font-variant-numeric:tabular-nums;margin-top:4px}
#satPanel .sat-pvro > div:first-child{grid-column:1/-1;font-size:14px}
#satPanel .sat-pvc{display:flex;align-items:center;gap:6px;margin-top:6px}
#satPanel .sat-pvc button{min-height:30px;padding:1px 9px}
#satPanel .sat-pvbar{position:relative;flex:1 1 auto;min-width:60px;display:flex;align-items:center}
#satPanel .sat-pvbar input{width:100%;accent-color:var(--amber);margin:0}
#satPanel .sat-pvmax{position:absolute;top:-11px;font-size:9px;letter-spacing:.04em;color:var(--amber);transform:translateX(-50%);pointer-events:none}
#satPanel .sat-pvsp{display:flex;gap:3px}
#satPanel .sat-pvsp button{font-size:11px !important;padding:1px 6px !important}
#satPanel .sat-pvv{margin-top:4px}
#satPanel .sat-pvd{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;vertical-align:0;background:var(--text-dim)}
#satPanel .sat-pvk{font-size:11px;margin-top:2px}
#satPanel .sat-pvnote{border:1px solid var(--amber);color:var(--amber);border-radius:8px;padding:6px 8px;margin:0 0 6px}
#satPanel .sat-pvb{flex:0 0 auto;align-self:center;min-height:30px;min-width:34px;padding:2px 8px !important;line-height:0}
#satPanel .sat-pvb svg{width:10px;height:11px;fill:currentColor}
#satPanel .sat-vd{display:block;grid-column:1/-1;font-size:12px;color:var(--btn-text)}
#satPanel .sat-vd i{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;background:var(--text-dim)}
${(['good', 'fair', 'poor'] as const).map((g) => `#satPanel .g-${g}{background:${PASS_GRADE_CSS[g]} !important}`).join('\n')}
`;
/** The row's ▶ (house style: the size key's small SVG, currentColor) */
export const PASS_PLAY_GLYPH = '<svg viewBox="0 0 10 11" aria-hidden="true"><path d="M1.5 1 L9 5.5 L1.5 10 Z"/></svg>';
const PAUSE = '❚❚', PLAY = '▶';

export interface PassPreviewDeps {
  host: HTMLElement;                 // the panel (its computed colours; .pv while a preview shows)
  rxPos: () => { lat: number; lon: number } | null;
  rxName: () => string;
  track?: (norad: number, aos: number, los: number) => void;
  skyGet?: (path: string) => Promise<string | null>;
  skyRadio?: () => string;
  /** something the rows show changed (a verdict, the lit ▶) */
  changed: () => void;
}
export type { PassPreviewRow };
export { previewId };
interface Cur extends PassPreviewRow { key: string; pt: PassTrack | null; player: PassPlayer | null; why: string; armAt: number }

export class PassPreviewView {
  readonly el: HTMLElement;
  readonly note: HTMLElement;
  private cur: Cur | null = null;
  private tracks = new Map<string, SatTrack>();
  private asked = new Set<string>();
  private pts = new Map<string, PassTrack | null>();
  private hist = new Map<string, { h: SkyHistory | null; at: number; busy: boolean }>();
  private raf = 0; private last = 0; private armT = 0; private noteT = 0;
  private bg: HTMLCanvasElement | null = null;
  private ink = { ink: '#ccc', amber: '#ffb000', ground: 'rgba(8,6,2,.96)' };
  private unBack: (() => void) | null = null;
  private q: Record<string, HTMLElement> = {};
  private cv: HTMLCanvasElement;
  private seek: HTMLInputElement;
  private histUsed: { kind: string; h: SkyHistory | null } = { kind: '', h: null };
  /** frames drawn (tests: none while paused / stopped) */
  frames = 0;

  constructor(private d: PassPreviewDeps) {
    this.el = document.createElement('div');
    this.el.className = 'sat-pv'; this.el.hidden = true; this.el.dataset.s = 'pv';
    this.el.setAttribute('aria-label', 'Pass preview');
    this.el.innerHTML = `<div class="sat-pvh"><span class="sat-pvs" data-p="strip"></span>`
      + `<button data-act="pvx" title="${esc(PASS_PREVIEW_STOP)}" aria-label="${esc(PASS_PREVIEW_STOP)}">✕</button></div>`
      + `<div class="sat-pvg"><canvas data-p="globe" width="640" height="300" aria-label="The pass played out on the globe: the satellite along its track, its footprint, the trail coloured by where this station usually hears"></canvas>`
      + `<div class="sat-pvsky" data-p="sky"></div></div>`
      + `<div class="sat-pvro"><div data-p="clock"></div><div data-p="look"></div><div data-p="range"></div><div data-p="dop" style="grid-column:1/-1"></div></div>`
      + `<div class="sat-pvc"><button data-act="pvplay" aria-label="Pause">${PAUSE}</button>`
      + `<div class="sat-pvbar"><input type="range" min="0" max="1000" step="1" value="0" data-p="seek" aria-label="Pass time"><span class="sat-pvmax" data-p="maxmark">MAX</span></div>`
      + `<span class="sat-pvsp">${PASS_SPEEDS.map((v) => `<button data-act="pvspeed" data-v="${v}" class="${v === 1 ? 'on' : ''}" aria-pressed="${v === 1}" title="Play speed">${v}×</button>`).join('')}</span></div>`
      + `<div class="sat-dim" data-p="moments"></div>`
      + `<div class="sat-pvv" data-p="verdict"></div><div class="sat-dim sat-pvk" data-p="src"></div><div class="sat-dim sat-pvk" data-p="key">${esc(PASS_KEY_TEXT)}</div>`;
    this.el.querySelectorAll<HTMLElement>('[data-p]').forEach((e) => { this.q[e.dataset.p!] = e; });
    this.cv = this.q.globe as HTMLCanvasElement;
    this.seek = this.q.seek as HTMLInputElement;
    this.seek.addEventListener('input', () => {
      const c = this.cur; if (!c?.player) return;
      c.player.seek(Number(this.seek.value) / 1000); c.player.playing = false; this.stopRaf(); this.draw(); this.paintPlay();
    });
    this.note = document.createElement('div');
    this.note.className = 'sat-pvnote'; this.note.hidden = true; this.note.setAttribute('aria-live', 'polite');
  }

  get active(): boolean { return !!this.cur; }
  get key(): string { return this.cur?.key ?? ''; }
  /** the player's state (tests) */
  state(): { key: string; t: number; playing: boolean; ended: boolean; frac: number; ready: boolean } | null {
    const c = this.cur; return c ? { key: c.key, t: c.player?.t ?? 0, playing: !!c.player?.playing, ended: !!c.player?.ended, frac: c.player?.frac() ?? 0, ready: !!c.pt } : null;
  }

  // ── tracks (the server's sat_track replies, kept by pass) ────────────────────────────────────────────────────────
  onTrack(t: SatTrack) {
    const k = passKey(t.norad, t.aos);
    this.tracks.set(k, t); this.pts.delete(k);
    if (this.tracks.size > 32) { const first = this.tracks.keys().next().value as string; this.tracks.delete(first); this.pts.delete(first); }
    if (this.cur && passKey(this.cur.norad, this.cur.aos) === k && !this.cur.pt) this.build();
    else this.d.changed();
  }
  private ptFor(norad: number, aos: number, los: number): PassTrack | null | undefined {
    const k = passKey(norad, aos);
    if (this.pts.has(k)) return this.pts.get(k);
    const t = this.tracks.get(k);
    if (!t) return undefined;
    const pt = passTrackFrom(t, this.d.rxPos(), { aos, los });
    this.pts.set(k, pt);
    return pt;
  }
  private ask(norad: number, aos: number, los: number) {
    const k = passKey(norad, aos);
    if (this.asked.has(k) || this.tracks.has(k) || !this.d.track) return;
    this.asked.add(k);
    this.d.track(norad, aos, los);
  }

  // ── the station's history (the sky plot's cells — read once per kind, again after 10 min) ─────────────────────────
  private historyFor(decoder: string | undefined): { kind: string; h: SkyHistory | null; ready: boolean } {
    const kinds = passHistoryKinds(decoder);
    let ready = true;
    for (const k of kinds) {
      const e = this.hist.get(k);
      if (!e || (!e.busy && Date.now() - e.at > 600_000)) this.loadHistory(k);
      if (!this.hist.get(k) || this.hist.get(k)!.busy) ready = false;
    }
    // the bird's own decoder's history when it has a fair amount, else ORBCOMM's (the aerial's general picture)
    for (const k of kinds) { const h = this.hist.get(k)?.h; if (h && h.dots >= 30) return { kind: k, h, ready }; }
    for (const k of kinds) { const h = this.hist.get(k)?.h; if (h) return { kind: k, h, ready }; }
    return { kind: kinds[kinds.length - 1], h: null, ready };
  }
  private loadHistory(kind: string) {
    if (!this.d.skyGet) { this.hist.set(kind, { h: null, at: Date.now(), busy: false }); return; }
    this.hist.set(kind, { h: this.hist.get(kind)?.h ?? null, at: Date.now(), busy: true });
    this.d.skyGet(skyPlotQuery(kind, this.d.skyRadio?.() ?? '', 0, 0, false, 'auto', true)).then((txt) => {
      let h: SkyHistory | null = null;
      try { h = txt ? skyHistory(parseSkyPlotJson(JSON.parse(txt))?.cells) : null; } catch { h = null; }
      this.hist.set(kind, { h, at: Date.now(), busy: false });
    }, () => this.hist.set(kind, { h: null, at: Date.now(), busy: false })).then(() => {
      if (this.cur?.pt) this.paintStatic();
      this.d.changed();
    });
  }

  /** The row's verdict (null = not known yet: its track or the history is still coming) */
  verdictFor(r: PassPreviewRow): PassVerdict | null {
    const pt = this.ptFor(r.norad, r.aos, r.los);
    if (!pt) return null;
    const h = this.historyFor(r.decoder);
    return h.ready ? passVerdict(pt, h.h) : null;
  }
  /** Ask the server for the first few hearable future passes' tracks — ONE a call (the panel renders once a second). */
  askRows(rows: PassPreviewRow[]) {
    for (const r of rows.slice(0, PASS_VERDICT_ROWS)) {
      const k = passKey(r.norad, r.aos);
      if (this.tracks.has(k) || this.asked.has(k)) continue;
      this.historyFor(r.decoder);
      this.ask(r.norad, r.aos, r.los);
      return;
    }
  }

  // ── start / stop ─────────────────────────────────────────────────────────────────────────────────────────────────
  /** ▶ on a row. `live` = a pass is really happening on the pick: no preview, the note says why. */
  start(r: PassPreviewRow, live: boolean) {
    const k = passKey(r.norad, r.aos), id = previewId(r);
    const pr = passPreviewPress(this.cur?.key ?? null, id, live);
    if (!pr.id) { this.stop(pr.note || undefined); return; }   // the row's button again = back to live; a live pass = the note
    this.stop(undefined, true);
    this.cur = { ...r, key: id, pt: null, player: null, why: '', armAt: performance.now() + PASS_START_DELAY_MS };
    this.el.hidden = false; this.d.host.classList.add('pv');
    this.note.hidden = true;
    this.q.strip.textContent = passStripText(r.name, r.aos);
    this.unBack = pushBack(() => { const key = this.key; this.stop(); return (this.d.host.querySelector(`[data-act="pv"][data-key="${key}"]`) as HTMLElement | null) ?? undefined; });
    // ★★ Stuart: the pass list can be far below the globe — bring the preview into view FIRST, then play
    this.el.scrollIntoView({ block: 'start', inline: 'nearest' });
    this.readInk();
    this.historyFor(r.decoder);
    if (this.tracks.has(k)) this.build();
    else { this.blank('Fetching this pass from the server…'); this.asked.delete(k); this.ask(r.norad, r.aos, r.los); }
    this.d.changed();
  }
  /** ✕ / back / the row again / a live pass (with its note). `quiet` = replaced by another preview. */
  stop(note?: string, quiet = false) {
    if (!this.cur) { if (note) this.flashNote(note); return; }
    this.stopRaf(); clearTimeout(this.armT);
    this.cur = null; this.bg = null;
    this.unBack?.(); this.unBack = null;
    this.el.hidden = true; this.d.host.classList.remove('pv');
    if (note) this.flashNote(note);
    if (!quiet) this.d.changed();
  }
  private flashNote(text: string) {
    this.note.textContent = text; this.note.hidden = false;
    clearTimeout(this.noteT); this.noteT = window.setTimeout(() => { this.note.hidden = true; }, 8000);
  }
  /** the panel's every-second tick: a live pass on the pick takes priority; folded / hidden pauses */
  tick(live: boolean) {
    if (!this.cur) return;
    if (live) { this.stop(PASS_LIVE_NOW); return; }
    if (this.raf && !this.el.offsetParent) { this.cur.player && (this.cur.player.playing = false); this.stopRaf(); this.paintPlay(); }
  }

  // ── drawing ──────────────────────────────────────────────────────────────────────────────────────────────────────
  private readInk() {
    const css = getComputedStyle(this.d.host);
    this.ink = { ink: css.color || '#ccc', amber: css.getPropertyValue('--amber').trim() || '#ffb000', ground: css.backgroundColor || 'rgba(8,6,2,.96)' };
  }
  private blank(why: string) {
    const ctx = this.cv.getContext('2d'); ctx?.clearRect(0, 0, this.cv.width, this.cv.height);
    this.q.sky.innerHTML = '';
    for (const k of ['clock', 'look', 'range', 'dop', 'moments', 'verdict', 'src']) this.q[k].textContent = '';
    this.q.clock.textContent = why;
  }
  private build() {
    const c = this.cur!;
    const rx = this.d.rxPos();
    c.pt = this.ptFor(c.norad, c.aos, c.los) ?? null;
    if (!c.pt || !rx) { this.blank(rx ? 'This server sent no track for this pass (it is not above this receiver\'s horizon, or the orbit data has gone stale)' : 'This server has not said where it is, so there is no pass to draw'); return; }
    c.player = new PassPlayer(c.pt.aos, c.pt.los);
    c.player.playing = false;
    this.paintStatic();
    this.draw();
    // ★★ the beat after the scroll: the start is not missed
    const wait = Math.max(0, c.armAt - performance.now());
    this.armT = window.setTimeout(() => { if (this.cur === c && c.player && !c.player.ended) { c.player.playing = true; this.paintPlay(); this.go(); } }, wait);
    this.paintPlay();
  }
  /** Everything that does not move: the globe's back layer (into an offscreen canvas), the sky plot, the words. */
  private paintStatic() {
    const c = this.cur; const rx = this.d.rxPos();
    if (!c?.pt || !rx) return;
    const pt = c.pt, W = this.cv.width, H = this.cv.height;
    this.histUsed = this.historyFor(c.decoder);
    const h = this.histUsed.h;
    this.bg = document.createElement('canvas'); this.bg.width = W; this.bg.height = H;
    const g0 = globeScene(W, H, rx, null, pt.geo, pt.tcaIdx);
    const k = this.inkFor();
    const bctx = this.bg.getContext('2d');
    if (bctx) paintGlobe(bctx, g0, { ...k, labels: { rx: null, sat: null } }, 'back');
    // the sky plot: the history's heat map faint underneath, the pass coloured by it, AOS / MAX / LOS
    const S = 150, sc = skyPlotScene([], S, 0, h ? { cells: h.cells, overlay: [] } : null);
    const sp = passSkyPaths(pt, h, sc.cx, sc.cy, sc.R);
    const f = (n: number) => n.toFixed(1), ink = 'currentColor';
    let s = `<svg viewBox="0 0 ${S} ${S}" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="The pass on the sky: north up, east right, the rim is the horizon">`;
    s += `<circle cx="${f(sc.cx)}" cy="${f(sc.cy)}" r="${f(sc.R)}" fill="${ink}" fill-opacity="0.07"/>`;
    if (sc.cells.length) s += `<g opacity="0.38" data-p="heat">${sc.cells.map((x) => `<path d="${x.d}" fill="${x.fill}" stroke="${x.fill}" stroke-width="0.6"/>`).join('')}</g>`;
    s += `<path d="${sc.cross}" stroke="${ink}" stroke-opacity="0.18" fill="none"/>`;
    for (const r of sc.rings) s += `<circle cx="${f(sc.cx)}" cy="${f(sc.cy)}" r="${f(r.r)}" fill="none" stroke="${ink}" stroke-opacity="${r.el === 0 ? 0.6 : 0.28}"/>`;
    for (const r of sp.runs) s += `<path d="${r.d}" fill="none" stroke-linecap="round" stroke-linejoin="round" data-grade="${r.grade}" ${gradeStroke(r.grade, 2.6)}/>`;
    const mk = (p: [number, number], t: string, anchor: string, dy: number, extra = '') =>
      `<circle cx="${f(p[0])}" cy="${f(p[1])}" r="2.6" fill="${ink}"${extra}/><text x="${f(p[0])}" y="${f(p[1] + dy)}" font-size="9" text-anchor="${anchor}" fill="${ink}" font-family="system-ui,sans-serif">${t}</text>`;
    s += mk(sp.aos, 'AOS', sp.aos[0] < sc.cx ? 'start' : 'end', sp.aos[1] < sc.cy ? 10 : -4);
    s += mk(sp.los, 'LOS', sp.los[0] < sc.cx ? 'start' : 'end', sp.los[1] < sc.cy ? 10 : -4);
    s += `<g data-p="max">${mk(sp.max, `MAX ${pt.maxEl.toFixed(0)}°`, 'middle', -5)}</g>`;
    for (const l of sc.labels) if (!/°$/.test(l.text)) s += `<text x="${f(l.x)}" y="${f(l.y)}" font-size="10" text-anchor="middle" dominant-baseline="middle" fill="${ink}" fill-opacity="0.8" font-family="system-ui,sans-serif">${l.text}</text>`;
    const p0 = skyPlotXY(pt.aosAz, 0, sc.cx, sc.cy, sc.R);
    s += `<g data-p="dot" transform="translate(${f(p0[0])} ${f(p0[1])})"><circle r="4.6" fill="${this.ink.ground}"/><circle r="3.4" fill="${ink}"/></g></svg>`;
    this.q.sky.innerHTML = s;
    this.q.sky.dataset.cx = String(sc.cx); this.q.sky.dataset.cy = String(sc.cy); this.q.sky.dataset.r = String(sc.R);
    this.q.moments.textContent = passMomentsText(pt);
    (this.q.maxmark as HTMLElement).style.left = `${((pt.tca - pt.aos) / Math.max(1, pt.los - pt.aos) * 100).toFixed(1)}%`;
    const v = passVerdict(pt, h);
    this.q.verdict.innerHTML = `<span class="sat-pvd g-${v.grade}"></span>${esc(v.text)}`;
    this.q.verdict.dataset.grade = v.grade;
    this.q.src.textContent = passHistorySource(this.histUsed.kind, h);
    this.q.key.hidden = !h;
    this.draw();
  }
  private inkFor() {
    const sc = this.cv.width / Math.max(60, this.cv.clientWidth || 300);
    return { sc, ...this.ink, ly: globeLayers((this.cv.clientWidth || 300) < 200), font: 10 * sc };
  }
  /** One frame at the player's time: the cached back layer, the trail, the moving parts; the sky plot's dot; the words. */
  private draw() {
    const c = this.cur, rx = this.d.rxPos();
    const ctx = this.cv.getContext('2d');
    if (!c?.pt || !c.player || !rx || !ctx) return;
    this.frames++;
    const pt = c.pt, t = c.player.t, W = this.cv.width, H = this.cv.height;
    const s = passSampleAt(pt, t);
    ctx.clearRect(0, 0, W, H);
    if (this.bg) ctx.drawImage(this.bg, 0, 0);
    const k = this.inkFor();
    const g = globeScene(W, H, rx, { lat: s.lat, lon: s.lon, altKm: s.altKm }, pt.geo, pt.tcaIdx, true);
    // the trail so far, coloured by this station's history (haloed in the ground so it reads over the land)
    const fr = globeFrame(W, H, rx);
    ctx.save(); ctx.beginPath(); ctx.rect(0, 0, W, g.cy); ctx.clip();
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    for (const run of passTrailRuns(pt, this.histUsed.h, t)) {
      const p = new Path2D(); let pen = false;
      for (const q of run.pts) { const v = fr.lifted(q.lat, q.lon, q.altKm); if (!v.vis) { pen = false; continue; } if (pen) p.lineTo(v.x, v.y); else p.moveTo(v.x, v.y); pen = true; }
      ctx.globalAlpha = 0.8; ctx.strokeStyle = this.ink.ground; ctx.lineWidth = 5 * k.sc; ctx.stroke(p);
      ctx.globalAlpha = run.grade === 'none' ? 0.55 : 1; ctx.strokeStyle = run.grade === 'none' ? this.ink.ink : PASS_GRADE_CSS[run.grade]; ctx.lineWidth = 2.6 * k.sc; ctx.stroke(p);
    }
    ctx.restore();
    const L = k.ly.labels ? globeLabels(g, this.d.rxName(), c.name, k.font) : { rx: null, sat: null };
    paintGlobe(ctx, g, { ...k, labels: L }, 'front');
    // the sky plot's dot — a transform only
    const dot = this.q.sky.querySelector('[data-p="dot"]');
    if (dot) { const [x, y] = skyPlotXY(s.az, s.el, Number(this.q.sky.dataset.cx), Number(this.q.sky.dataset.cy), Number(this.q.sky.dataset.r)); dot.setAttribute('transform', `translate(${x.toFixed(1)} ${y.toFixed(1)})`); }
    const r = passReadout(pt, t, c.hz);
    if (this.q.clock.textContent !== r.clock) this.q.clock.textContent = r.clock;
    if (this.q.look.textContent !== r.look) this.q.look.textContent = r.look;
    if (this.q.range.textContent !== r.range) this.q.range.textContent = r.range;
    if (this.q.dop.textContent !== r.doppler) this.q.dop.textContent = r.doppler;
    this.seek.value = String(Math.round(c.player.frac() * 1000));
  }
  private go() {
    if (this.raf) return;
    this.last = performance.now();
    const step = (ts: number) => {
      this.raf = 0;
      const c = this.cur;
      if (!c?.player) return;
      const more = c.player.advance(ts - this.last);
      this.last = ts;
      this.draw();
      if (more) this.raf = requestAnimationFrame(step);
      else this.paintPlay();   // ★ ended or paused: no more frames
    };
    this.raf = requestAnimationFrame(step);
  }
  private stopRaf() { if (this.raf) cancelAnimationFrame(this.raf); this.raf = 0; }
  private paintPlay() {
    const b = this.el.querySelector('[data-act="pvplay"]') as HTMLButtonElement;
    const p = this.cur?.player;
    const playing = !!p?.playing;
    b.textContent = playing ? PAUSE : PLAY;
    b.setAttribute('aria-label', playing ? 'Pause' : p?.ended ? 'Play the pass again' : 'Play');
    b.title = b.getAttribute('aria-label')!;
  }
  /** the preview's own keys (the panel's click handler hands them here) */
  onAct(act: string, b: HTMLButtonElement): boolean {
    const c = this.cur;
    if (act === 'pvx') { this.stop(); return true; }
    if (act === 'pvplay') { if (c?.player) { clearTimeout(this.armT); c.player.toggle(); this.paintPlay(); if (c.player.playing) this.go(); else this.stopRaf(); } return true; }
    if (act === 'pvspeed') {
      if (c?.player) c.player.setSpeed(Number(b.dataset.v));
      this.el.querySelectorAll<HTMLButtonElement>('[data-act="pvspeed"]').forEach((x) => { const on = x === b; x.classList.toggle('on', on); x.setAttribute('aria-pressed', String(on)); });
      return true;
    }
    return false;
  }
}

function gradeStroke(g: PassGrade, w: number): string {
  return g === 'none' ? `stroke="currentColor" stroke-opacity="0.55" stroke-width="${w * 0.7}" stroke-dasharray="2 3"` : `stroke="${PASS_GRADE_CSS[g]}" stroke-width="${w}"`;
}
