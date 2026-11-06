/**
 * handheld.ts — the R36S handheld's page (docs/v12/R36S-HANDHELD-IMAGE.md §0.1-§0.2, Stuart 2026-11-05: "a proper
 * portable radio just powered by VibeSDR").
 *
 * ★★★ ONLY WHEN THE ENGINE SAYS SO. The image starts Cog on `http://127.0.0.1:48000/?handheld=1`; the hint alone does
 *     nothing — the page asks the engine (`/handheld/session`, answered by vibeserver --handheld's supervisor) and only a
 *     handheld engine answers. Every server, every browser, every app: this file is one URL test and nothing else.
 *
 * What it does, all of it behind isHandheld():
 *  · boot: no landing page; "Please connect an SDR" until a radio is plugged in, then straight into the receiver; a
 *    radio arriving reloads the page (one clean start per radio — its own memory, its own caps).
 *  · PER-RADIO MEMORY: the page's own prefs (vibesdr_web_prefs_v1) minus the LOOK keys, posted to the engine whenever
 *    they change (checked every 2 s), written to disk by the engine (every ~30 s, on unplug, on power-off) and put back
 *    when that radio returns. The look (palette, colours, display font …) and the handheld's own settings stay the
 *    device's, whichever radio is plugged in.
 *  · the hardware service's events (long-poll): the volume overlay, the 10 % banner, the reset toast, headphones.
 *  · the menu: three accordions — R36S HARDWARE · SDR HARDWARE · DISPLAY — one level, opening one closes the others,
 *    B / back collapses (gamepad pushBack). SDR HARDWARE and DISPLAY are the page's own existing rows, MOVED (every
 *    listener stays bound), never copied.
 *  · the Performance overlay: plain DOM text, ≤ 2 updates a second, no canvas.
 *  · recordings to the card (Cog has no downloads), the import/ folder, export/, EXPORT DIAGNOSTICS, the clock.
 *  · the device's location: typed (a locator or lat, lon) or SET FROM MAP — an in-page map (handheldMap.ts), from the
 *    menu's LOCATION row or from "Please connect an SDR" (no radio needed).
 */
import { pushBack } from './gamepad';
import { openMapPick, locatorToLatLon } from './handheldMap';
import type { Spot, TimeSync } from './decoders';

const LS_PREFS = 'vibesdr_web_prefs_v1';
const LS_HH = 'vibesdr_handheld_v1';
const SS_RELOAD = 'vsHhMemReload';
const SS_RESET_SEEN = 'vsHhResetSeen';

export interface HandheldSession {
  handheld: true;
  state: 'none' | 'starting' | 'running';
  radio: { serial: string; driver: string; name: string } | null;
  memory: Record<string, unknown> | null;
  resetSeq: number;
  resetSerial: string;
  eventSeq: number;
  hwd: boolean;
  card: boolean;
}

/** What the page lends this module — main.ts owns all of it. */
export interface HandheldHost {
  /** The measured rate of real spectrum frames from the engine, per second. */
  specFps(): number;
  /** Frames the page actually drew in the last second. */
  uiFps(): number;
  /** The waterfall: `fps` real frames a second; `interpolate` = repeat rows to 20 (the measured-cost fallback). */
  setSpectrumRate(fps: number, interpolate: boolean): void;
  /** The drawing cap (30 / 60). */
  setUiFpsCap(fps: number): void;
  /** The screen went off / on: stop drawing (the audio plays on) / start again. */
  setScreenOff(off: boolean): void;
  /** Bookmark text from the card → the receiver's list. */
  importBookmarks(text: string): Promise<{ ok: boolean; imported: number; message: string }>;
  /** The bookmarks as the page exports them. */
  exportBookmarks(): string;
  openMenu(): void;
  /** The device's location changed: the page reads the receiver's /location again (map centre, distances). */
  locationChanged(): void;
}

let on = false;
let host: HandheldHost | null = null;
let session: HandheldSession | null = null;
export function isHandheld(): boolean { return on; }
export function handheldHinted(): boolean {
  try { return new URLSearchParams(location.search).has('handheld'); } catch { return false; }
}

// ── The handheld's own settings (the DEVICE's, never a radio's) ──────────────────────────────────────────────────────
interface HhSettings { uiFps: 30 | 60; specRate: 5 | 10 | 20; screenOffMin: number; perf: boolean }
function hhSettings(): HhSettings {
  let s: Partial<HhSettings> = {};
  try { s = JSON.parse(localStorage.getItem(LS_HH) || '{}'); } catch { /* fresh */ }
  return {
    uiFps: s.uiFps === 60 ? 60 : 30,
    specRate: s.specRate === 5 || s.specRate === 10 ? s.specRate : 20,
    screenOffMin: typeof s.screenOffMin === 'number' && [0, 1, 2, 5, 10].includes(s.screenOffMin) ? s.screenOffMin : 0,
    perf: s.perf === true,
  };
}
function saveHh(p: Partial<HhSettings>) {
  const s = { ...hhSettings(), ...p };
  try { localStorage.setItem(LS_HH, JSON.stringify(s)); } catch { /* storage blocked: this visit only */ }
}
/** ★★ 20 real rows a second, or 10 real + repeated rows to 20 — ONE switch (`?wf20=interp`, or this device's
 *  `wf20` in vibesdr_handheld_v1), default real. Stuart: interpolation only if a MEASUREMENT on the R36S says 20 real is
 *  too heavy — "until we measure we will not know". */
function wf20Interpolates(): boolean {
  try {
    const q = new URLSearchParams(location.search).get('wf20');
    if (q) return q === 'interp';
    return (JSON.parse(localStorage.getItem(LS_HH) || '{}') as { wf20?: string }).wf20 === 'interp';
  } catch { return false; }
}
export function handheldUiFps(): number { return hhSettings().uiFps; }
export function handheldSpecRate(): { fps: number; interpolate: boolean } {
  const r = hhSettings().specRate;
  return r === 20 && wf20Interpolates() ? { fps: 10, interpolate: true } : { fps: r, interpolate: false };
}

// ── Per-radio memory ────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ The LOOK and the device's own choices — kept whatever radio is plugged in. Everything else in the page's prefs
 *  (the dial and mode per server, the filter, gain and front-end settings, squelch, the step, DAB, decoders' choices,
 *  the view) belongs to the RADIO. */
const GLOBAL_KEY = /^(palette|ctl|freqUnit|vfoColor|volume|specRatio|specShow|wf(Scroll|Speed|Boost|Data|DataRate)$|lastHost|tapSnap|ptrFreq|font|look|theme|disp|padTuck|wheelTunes)/;
function prefsBlob(): Record<string, unknown> {
  try { return JSON.parse(localStorage.getItem(LS_PREFS) || '{}'); } catch { return {}; }
}
function radioPart(p: Record<string, unknown>): Record<string, unknown> {
  const o: Record<string, unknown> = {};
  for (const k of Object.keys(p).sort()) if (!GLOBAL_KEY.test(k)) o[k] = p[k];
  return o;
}
/** Put a radio's memory into the page's prefs (the look stays). True when that changed anything. */
function applyMemory(mem: Record<string, unknown> | null): boolean {
  const cur = prefsBlob();
  const next: Record<string, unknown> = {};
  for (const k of Object.keys(cur)) if (GLOBAL_KEY.test(k)) next[k] = cur[k];
  if (mem) for (const k of Object.keys(mem)) if (!GLOBAL_KEY.test(k)) next[k] = mem[k];
  const changed = JSON.stringify(radioPart(cur)) !== JSON.stringify(radioPart(next));
  if (changed) { try { localStorage.setItem(LS_PREFS, JSON.stringify(next)); } catch { /* blocked */ } }
  return changed;
}
let lastPushed = '';
async function pushMemory(force = false) {
  if (!session?.radio) return;
  const body = JSON.stringify(radioPart(prefsBlob()));
  if (!force && body === lastPushed) return;
  try {
    const r = await fetch('/handheld/memory', { method: 'POST', body: JSON.stringify({ serial: session.radio.serial, prefs: JSON.parse(body) }) });
    if (r.ok) lastPushed = body;
  } catch { /* the engine is restarting — the next tick tries again */ }
}

// ── Boot ────────────────────────────────────────────────────────────────────────────────────────────────────────────
async function getSession(): Promise<HandheldSession | null> {
  try {
    const r = await fetch('/handheld/session', { cache: 'no-store', signal: AbortSignal.timeout(4000) });
    if (!r.ok) return null;
    const j = await r.json();
    return j && j.handheld === true ? j as HandheldSession : null;
  } catch { return null; }
}

/** ★ Ask the engine; null = not a handheld (boot as ever). Applies this radio's memory first — if that changed the
 *  page's prefs, reloads ONCE so every reader at module load sees them (a guard in sessionStorage stops a loop). */
export async function handheldBoot(): Promise<HandheldSession | null> {
  const s = await getSession();
  if (!s) return null;
  on = true; session = s;
  document.documentElement.dataset.handheld = '1';
  injectStyle();
  if (s.state !== 'none' && s.radio) {
    const changed = applyMemory(s.memory);
    if (changed && sessionStorage.getItem(SS_RELOAD) !== s.radio.serial) {
      sessionStorage.setItem(SS_RELOAD, s.radio.serial);
      location.reload();
      return new Promise(() => { /* reloading */ });
    }
    lastPushed = JSON.stringify(radioPart(prefsBlob()));
  }
  sessionStorage.removeItem(SS_RELOAD);
  if (s.resetSeq > 0 && sessionStorage.getItem(SS_RESET_SEEN) !== String(s.resetSeq)) {
    sessionStorage.setItem(SS_RESET_SEEN, String(s.resetSeq));
    setTimeout(() => toast('Radio settings reset', 'Start + Select at power-on put this radio back to its defaults.'), 600);
  }
  void eventLoop(s.eventSeq);
  setInterval(() => { void pushMemory(); }, 2000);
  window.addEventListener('pagehide', () => { void pushMemory(true); });
  initScreenTimeout();
  if (hhSettings().perf) setPerf(true);
  if (s.state === 'none') showConnect();
  else if (s.state === 'starting') showConnect(`Starting ${s.radio?.name || 'the radio'}…`);
  return s;
}

/** Once the receiver is up (main.ts startApp): the host's hooks, the card's import folder, the DEVICE key. */
export function handheldStarted(h: HandheldHost) {
  host = h;
  const sr = handheldSpecRate();
  h.setSpectrumRate(sr.fps, sr.interpolate);
  h.setUiFpsCap(hhSettings().uiFps);
  void runImport();
}

// ── Events ──────────────────────────────────────────────────────────────────────────────────────────────────────────
let lastVolume = -1;
async function eventLoop(since: number) {
  for (;;) {
    let j: { seq: number; events: Array<Record<string, unknown>> } | null = null;
    try {
      const r = await fetch(`/handheld/events?since=${since}`, { cache: 'no-store', signal: AbortSignal.timeout(35000) });
      if (r.ok) j = await r.json();
    } catch { /* the engine is restarting */ }
    if (!j) { await new Promise((res) => setTimeout(res, 1500)); continue; }
    if (j.seq < since) since = 0;                      // ★ the engine restarted: its numbering began again
    for (const e of j.events) { try { onEvent(e); } catch (err) { console.warn('[handheld] event', err); } }
    since = Math.max(since, j.seq);
  }
}
function onEvent(e: Record<string, unknown>) {
  switch (e.event) {
    case 'volume': {
      const v = Number(e.volume);
      if (Number.isFinite(v)) { volumeOverlay(v, e.muted === true); lastVolume = v; paintVolumeRow(v); }
      break;
    }
    case 'battery_low': batteryBanner(Number(e.pct)); break;
    case 'battery': if (e.charging === true || Number(e.pct) > 10) batteryBanner(-1); paintBattery(); break;
    case 'output': toast(e.output === 'headphones' ? 'Headphones' : 'Speaker', ''); break;
    case 'shutdown': toast('Switching off', 'Settings saved.'); break;
    // ★ The service turned the screen off (a short press of power, or our own SCREEN OFF AFTER): draw nothing.
    case 'screen': screenOff = e.screen === 'off'; host?.setScreenOff(screenOff); break;
    case 'radio': {
      const st = String(e.state);
      const radio = e.radio as HandheldSession['radio'];
      if (st === 'none') { if (session?.radio) void pushMemory(true); session = session && { ...session, state: 'none', radio: null }; showConnect(); }
      else if (st === 'starting') showConnect(`Starting ${radio?.name || 'the radio'}…`);
      else if (st === 'running') {
        // ★ A radio has come (back): one clean start — its memory, its caps, its controls.
        if (!session?.radio || session.radio.serial !== radio?.serial || connectUp()) location.reload();
      }
      break;
    }
    default: break;
  }
}

// ── The screens ─────────────────────────────────────────────────────────────────────────────────────────────────────
function injectStyle() {
  if (document.getElementById('hhStyle')) return;
  const st = document.createElement('style');
  st.id = 'hhStyle';
  // ★ Server furniture goes (§0.1 "No server furniture"): the landing page, admin, chat, listener counts, the link meter
  //   and every data-rate / compression choice. The RADIO's and the DSP's rows all stay.
  st.textContent = `
html[data-handheld] #splash, html[data-handheld] #btnAdmin, html[data-handheld] #adminRow, html[data-handheld] #adminPwRow,
html[data-handheld] #adminServerRow, html[data-handheld] #chatPanel, html[data-handheld] #secListeners,
html[data-handheld] #srvHealth, html[data-handheld] #srvBattery,
html[data-handheld] #linkBars, html[data-handheld] #rawAudioRow, html[data-handheld] #rawAudioNote, html[data-handheld] #mShared,
html[data-handheld] .hhHide { display: none !important; }
#hhConnect { position: fixed; inset: 0; z-index: 9000; background: #080601; color: #ffb833; display: flex; flex-direction: column;
  align-items: center; justify-content: center; text-align: center; font: 600 20px/1.4 ui-monospace, Menlo, monospace; letter-spacing: 2px; }
#hhConnect .sub { font-size: 12px; opacity: .7; letter-spacing: 1px; margin-top: 14px; max-width: 80vw; font-weight: 400; }
#hhConnect .logo { font-size: 30px; letter-spacing: 8px; margin-bottom: 28px; }
#hhConnect .hhKey { margin-top: 10px; font: 600 12px ui-monospace, Menlo, monospace; letter-spacing: 1px; color: #ffb833;
  background: transparent; border: 1px solid #ffb833; border-radius: 6px; padding: 7px 14px; }
#hhConnect .hhKey.gpFocus { background: #ffb833; color: #080601; }
#hhVol { position: fixed; left: 50%; bottom: 14%; transform: translateX(-50%); z-index: 9500; background: rgba(8,6,1,.88);
  border: 1px solid #ffb833; border-radius: 10px; padding: 10px 16px; color: #ffb833; font: 600 14px ui-monospace, monospace;
  display: none; align-items: center; gap: 12px; pointer-events: none; }
#hhVol .bar { width: 180px; height: 8px; background: rgba(255,184,51,.2); border-radius: 4px; overflow: hidden; }
#hhVol .bar i { display: block; height: 100%; background: #ffb833; }
#hhBatt { position: fixed; top: 0; left: 0; right: 0; z-index: 9400; background: #a31515; color: #fff; text-align: center;
  font: 600 13px ui-monospace, monospace; padding: 6px; display: none; }
#hhToast { position: fixed; left: 50%; top: 12%; transform: translateX(-50%); z-index: 9600; background: rgba(8,6,1,.92);
  border: 1px solid #ffb833; border-radius: 10px; padding: 10px 18px; color: #ffb833; font: 600 14px ui-monospace, monospace;
  text-align: center; display: none; max-width: 86vw; }
#hhToast .sub { font-weight: 400; font-size: 11px; opacity: .8; margin-top: 4px; }
#hhPerf { position: fixed; top: 2px; right: 2px; z-index: 8000; background: rgba(0,0,0,.55); color: #e8e8e8;
  font: 10px/1.25 ui-monospace, Menlo, monospace; padding: 2px 6px; border-radius: 4px; pointer-events: none; white-space: pre; }
#hhPerf b { font-weight: 600; border: 1px solid rgba(232,232,232,.6); border-radius: 3px; padding: 0 3px; }
.hhAcc { display: block; width: 100%; text-align: left; margin: 6px 0 0; padding: 10px 12px; font-weight: 700; letter-spacing: 2px; }
.hhAcc::after { content: '▸'; float: right; }
.hhAcc[aria-expanded="true"]::after { content: '▾'; }
.hhAccBody { padding: 2px 0 8px 6px; border-left: 2px solid rgba(255,184,51,.35); margin-left: 4px; }
.hhAccBody[hidden] { display: none !important; }
.hhFig { font-variant-numeric: tabular-nums; }
`;
  document.head.appendChild(st);
}

function connectUp(): boolean { return !!document.getElementById('hhConnect'); }
/** ★ "Please connect an SDR" — the whole screen, on every route into "no radio" (boot, unplug). */
export function showConnect(status = '') {
  let el = document.getElementById('hhConnect');
  if (!el) {
    el = document.createElement('div');
    el.id = 'hhConnect';
    el.innerHTML = '<div class="logo">VIBESDR</div><div>Please connect an SDR</div>'
      + '<div class="sub">RTL-SDR · Airspy HF+ · Airspy R2 / Mini<br>SDRplay RSP · HackRF · USRP<br><br>into the USB-C port</div>'
      + '<div class="sub" id="hhConnectSt"></div>'
      // ★ The location wants no radio (Stuart 2026-11-05: set it before the first one is plugged in) — the one key here
      + '<div class="sub" id="hhConnectLoc"></div><button type="button" class="hhKey" id="hhConnectMap">SET LOCATION FROM MAP</button>';
    // ★ The D-pad stays on this screen's one key, never the page hidden behind it (gamepad.ts scope)
    el.dataset.navscope = '';
    document.body.appendChild(el);
    (el.querySelector('#hhConnectMap') as HTMLButtonElement).onclick = () => void openLocationMap();
    void refreshLocation();
  }
  const st = document.getElementById('hhConnectSt');
  if (st) st.textContent = status;
}

let volTimer = 0;
function volumeOverlay(v: number, muted: boolean) {
  let el = document.getElementById('hhVol');
  if (!el) {
    el = document.createElement('div'); el.id = 'hhVol';
    el.innerHTML = '<span id="hhVolTxt"></span><span class="bar"><i id="hhVolBar"></i></span>';
    document.body.appendChild(el);
  }
  (document.getElementById('hhVolTxt') as HTMLElement).textContent = muted ? 'MUTED' : `VOL ${Math.round(v)}`;
  (document.getElementById('hhVolBar') as HTMLElement).style.width = `${muted ? 0 : Math.max(0, Math.min(100, v))}%`;
  el.style.display = 'flex';
  clearTimeout(volTimer);
  volTimer = window.setTimeout(() => { el!.style.display = 'none'; }, 1500);
}
function batteryBanner(pct: number) {
  let el = document.getElementById('hhBatt');
  if (!el) { el = document.createElement('div'); el.id = 'hhBatt'; document.body.appendChild(el); }
  if (pct < 0 || !Number.isFinite(pct)) { el.style.display = 'none'; return; }
  el.textContent = `Battery ${Math.round(pct)} % — plug in the charger. VibeSDR switches off cleanly at 3 %.`;
  el.style.display = 'block';
}
let toastTimer = 0;
export function toast(title: string, sub: string) {
  let el = document.getElementById('hhToast');
  if (!el) { el = document.createElement('div'); el.id = 'hhToast'; document.body.appendChild(el); }
  el.innerHTML = '';
  const t = document.createElement('div'); t.textContent = title; el.appendChild(t);
  if (sub) { const s = document.createElement('div'); s.className = 'sub'; s.textContent = sub; el.appendChild(s); }
  el.style.display = 'block';
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => { el!.style.display = 'none'; }, 3500);
}

// ── The hardware service, through the engine ────────────────────────────────────────────────────────────────────────
async function device(body: Record<string, unknown>): Promise<{ ok: boolean; error?: string }> {
  try {
    const r = await fetch('/handheld/device', { method: 'POST', body: JSON.stringify(body) });
    return await r.json();
  } catch { return { ok: false, error: 'the engine did not answer' }; }
}
interface DevState { ok?: boolean; brightness?: number; volume?: number; muted?: boolean; output?: string;
                     battery?: { pct: number; charging: boolean }; screen?: string }
async function deviceState(): Promise<{ state: DevState }> {
  try { return await (await fetch('/handheld/device', { cache: 'no-store' })).json(); }
  catch { return { state: {} }; }
}

// ── Screen-off after (DISPLAY) ──────────────────────────────────────────────────────────────────────────────────────
let lastInput = Date.now(), dimmed = false, screenOff = false, savedBright = 120;
function noteInput() {
  lastInput = Date.now();
  if (screenOff) { screenOff = false; host?.setScreenOff(false); void device({ action: 'screen-on' }); }
  if (dimmed) { dimmed = false; void device({ set: 'brightness', value: savedBright }); }
}
function initScreenTimeout() {
  for (const ev of ['keydown', 'pointerdown', 'wheel'] as const) window.addEventListener(ev, noteInput, { capture: true, passive: true });
  setInterval(() => {
    // ★ A pad presses no DOM event: read it (4×/s, only while a timeout is set) — a button down or a stick past half.
    const mins = hhSettings().screenOffMin;
    if (!mins) return;
    const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) as Gamepad[] : [];
    if (pads.some((g) => g.buttons.some((b) => b.pressed) || g.axes.some((a) => Math.abs(a) > 0.5))) noteInput();
    const idle = (Date.now() - lastInput) / 60000;
    if (!dimmed && !screenOff && idle >= mins / 2) {
      dimmed = true;
      void deviceState().then((d) => { if (typeof d.state.brightness === 'number') savedBright = d.state.brightness;
                                         void device({ set: 'brightness', value: Math.min(savedBright, 20) }); });
    }
    if (!screenOff && idle >= mins) { screenOff = true; host?.setScreenOff(true); void device({ action: 'screen-off' }); }
  }, 250);
}

// ── Performance overlay ─────────────────────────────────────────────────────────────────────────────────────────────
interface Perf { cpu: number; tempC: number; engineCpu: number; cores: number; ramUsedMb: number; ramTotalMb: number;
                 battery: { pct: number; charging: boolean; minutesLeft: number } }
let perfTimer = 0, lastPerf: Perf | null = null;
const fig = (v: number, unit: string, dp = 0) => (Number.isFinite(v) && v >= 0 ? v.toFixed(dp) + unit : '—');
function perfLines(p: Perf | null): string[] {
  const ui = host ? host.uiFps() : 0, sp = host ? host.specFps() : 0;
  const bat = p && p.battery.pct >= 0
    ? `${p.battery.pct} %${p.battery.charging ? ' charging' : p.battery.minutesLeft >= 0 ? ` ~${Math.floor(p.battery.minutesLeft / 60)}h${String(p.battery.minutesLeft % 60).padStart(2, '0')}` : ''}`
    : '—';
  return [
    `CPU ${p ? fig(p.cpu, ' %') : '—'}  ${p ? fig(p.tempC, ' °C') : ''}`,
    `DSP ${p ? fig(p.engineCpu, ' %') : '—'}  RAM ${p && p.ramUsedMb >= 0 ? `${p.ramUsedMb}/${p.ramTotalMb} MB` : '—'}`,
    `UI ${ui.toFixed(0)} fps  Spectrum ${sp.toFixed(1)} fps`,
    `Battery ${bat}`,
  ];
}
function setPerf(onNow: boolean) {
  saveHh({ perf: onNow });
  clearInterval(perfTimer);
  let el = document.getElementById('hhPerf');
  if (!onNow) { el?.remove(); return; }
  if (!el) { el = document.createElement('div'); el.id = 'hhPerf'; document.body.appendChild(el); }
  let n = 0;
  const tick = async () => {
    if (n++ % 2 === 0) { try { lastPerf = await (await fetch('/handheld/perf', { cache: 'no-store' })).json(); } catch { /* next tick */ } }
    const lines = perfLines(lastPerf);
    // ★ one text write, ≤ 2 a second: the CPU box is a bordered <b>, the rest plain text
    el!.innerHTML = '';
    const b = document.createElement('b'); b.textContent = lines[0]; el!.appendChild(b);
    el!.appendChild(document.createTextNode('\n' + lines.slice(1).join('\n')));
  };
  void tick();
  perfTimer = window.setInterval(() => { void tick(); }, 500);
}

// ── Recordings (the card's Recordings/) ─────────────────────────────────────────────────────────────────────────────
export async function saveRecordingToCard(blob: Blob, hz: number, mode: string, startMs: number): Promise<string> {
  const q = `hz=${Math.round(hz)}&mode=${encodeURIComponent(mode)}&start=${Math.floor(startMs / 1000)}`;
  const r = await fetch(`/handheld/recordings?${q}`, { method: 'POST', body: blob });
  const j = await r.json().catch(() => ({}));
  if (!r.ok || !j.ok) throw new Error(j.error || 'the card did not take it');
  toast('Saved to the card', `Recordings/${j.name}`);
  return j.name;
}
export interface CardRecording { name: string; bytes: number; mtime: number }
export async function listCardRecordings(): Promise<CardRecording[]> {
  try { return (await (await fetch('/handheld/recordings', { cache: 'no-store' })).json()).recordings || []; } catch { return []; }
}
export function cardRecordingUrl(name: string): string { return `/handheld/recordings/${encodeURIComponent(name)}`; }
export async function deleteCardRecording(name: string): Promise<boolean> {
  try { return (await fetch(cardRecordingUrl(name), { method: 'DELETE' })).ok; } catch { return false; }
}

// ── import/ export/ diagnostics ─────────────────────────────────────────────────────────────────────────────────────
/** ★ Every file waiting in the card's import/ — the page parses it (it knows every bookmark format), the engine moves it
 *  to import/done/ with a .txt saying what happened. At start, and from the IMPORT key. */
async function runImport(announceNone = false): Promise<void> {
  if (!host) return;
  let files: Array<{ name: string; text?: string; tooBig?: boolean }> = [];
  try { files = (await (await fetch('/handheld/import', { cache: 'no-store' })).json()).files || []; } catch { /* no card */ }
  if (!files.length) { if (announceNone) toast('Nothing to import', 'Put bookmark backups in the card\'s import folder.'); return; }
  let total = 0, okFiles = 0;
  for (const f of files) {
    let res = { ok: false, imported: 0, message: 'too big to import (over 1 MB)' };
    if (!f.tooBig && typeof f.text === 'string') {
      try { res = await host.importBookmarks(f.text); } catch (e) { res = { ok: false, imported: 0, message: e instanceof Error ? e.message : String(e) }; }
    }
    if (res.ok) { okFiles++; total += res.imported; }
    try { await fetch('/handheld/import/result', { method: 'POST', body: JSON.stringify({ name: f.name, ok: res.ok, imported: res.imported, message: res.message }) }); }
    catch { /* the file stays in import/ and is tried again next time */ }
  }
  toast(`Imported ${total} bookmark${total === 1 ? '' : 's'}`, `${okFiles} of ${files.length} file${files.length === 1 ? '' : 's'} — results in import/done/`);
}
async function exportBookmarks() {
  if (!host) return;
  try {
    const r = await fetch('/handheld/export?kind=bookmarks', { method: 'POST', body: host.exportBookmarks() });
    const j = await r.json();
    toast(j.ok ? 'Bookmarks exported' : 'Export failed', j.ok ? j.name : (j.error || ''));
  } catch { toast('Export failed', 'the engine did not answer'); }
}
async function exportDiagnostics(btn: HTMLButtonElement) {
  btn.disabled = true; btn.textContent = 'WORKING…';
  try {
    const j = await (await fetch('/handheld/diagnostics', { method: 'POST', body: '' })).json();
    toast(j.ok ? 'Diagnostics saved' : 'Diagnostics failed', j.ok ? j.name : (j.error || ''));
  } catch { toast('Diagnostics failed', 'the engine did not answer'); }
  btn.disabled = false; btn.textContent = 'EXPORT DIAGNOSTICS';
}

// ── The clock ───────────────────────────────────────────────────────────────────────────────────────────────────────
let lastSync: TimeSync | null = null, lastSyncAt = 0;
const ft8Dts: Array<{ dt: number; at: number }> = [];
/** From the decoder socket (main.ts): a corroborated time-station minute. */
export function noteTimeSync(t: TimeSync) { lastSync = t; lastSyncAt = Date.now(); paintClockKeys(); }
/** From the decoder socket: every FT8 spot's DT. */
export function noteSpot(s: Spot) {
  if (s.mode !== 'FT8' || typeof s.dt !== 'number') return;
  ft8Dts.push({ dt: s.dt, at: Date.now() });
  while (ft8Dts.length > 200) ft8Dts.shift();
  paintClockKeys();
}
function ft8Median(): { dt: number; n: number } | null {
  const now = Date.now();
  const v = ft8Dts.filter((x) => now - x.at < 120_000 && Math.abs(x.dt) < 5).map((x) => x.dt).sort((a, b) => a - b);
  if (v.length < 3) return null;
  const m = v.length % 2 ? v[(v.length - 1) / 2] : (v[v.length / 2 - 1] + v[v.length / 2]) / 2;
  return { dt: m, n: v.length };
}
async function setClock(body: Record<string, unknown>, what: string) {
  try {
    const j = await (await fetch('/handheld/clock', { method: 'POST', body: JSON.stringify(body) })).json();
    toast(j.ok ? `Clock set ${what}` : 'The clock was not set', j.ok ? new Date(j.utcMs).toISOString().replace('T', ' ').slice(0, 21) + ' UTC' : (j.error || ''));
  } catch { toast('The clock was not set', 'the engine did not answer'); }
}
function paintClockKeys() {
  const st = document.getElementById('hhClkStation') as HTMLButtonElement | null;
  if (st) {
    const fresh = lastSync && Date.now() - lastSyncAt < 90_000;
    st.hidden = !fresh;
    if (fresh && lastSync) st.textContent = `SET CLOCK FROM ${lastSync.station}`;
  }
  const ft = document.getElementById('hhClkFt8') as HTMLButtonElement | null;
  if (ft) {
    const m = ft8Median();
    ft.hidden = !m;
    if (m) ft.textContent = `ALIGN TO FT8 (DT ${m.dt >= 0 ? '+' : ''}${m.dt.toFixed(2)} s, ${m.n})`;
  }
}

// ── The device's location ───────────────────────────────────────────────────────────────────────────────────────────
interface Loc { locator: string; lat: string; lon: string; set: boolean }
let loc: Loc | null = null;
/** Every place that shows it: the menu's LOCATION row and "Please connect an SDR". */
function paintLocation(j: Loc) {
  loc = j;
  const t = !j.set ? 'not set' : j.locator ? j.locator : `${j.lat}, ${j.lon}`;
  const now = document.getElementById('hhLocNow'); if (now) now.textContent = t;
  const c = document.getElementById('hhConnectLoc'); if (c) c.textContent = `Location: ${t}`;
}
async function refreshLocation() {
  try { paintLocation(await (await fetch('/handheld/location', { cache: 'no-store' })).json()); } catch { /* the engine is restarting */ }
}
/** ★ The ONE way a location is set — typed, cleared or from the map: the supervisor keeps it and applies it live. */
async function postLocation(body: Record<string, unknown>) {
  try {
    const r = await fetch('/handheld/location', { method: 'POST', body: JSON.stringify(body) });
    const j = await r.json();
    if (!r.ok) { toast('Location not set', j.error || ''); return; }
    paintLocation(j); host?.locationChanged();
    toast(j.set ? 'Location set' : 'Location cleared', j.set ? (j.locator || `${j.lat}, ${j.lon}`) : '');
  } catch { toast('Location not set', 'the engine did not answer'); }
}
/** SET FROM MAP: the in-page map (handheldMap.ts), centred on the location if there is one, else the whole world. */
async function openLocationMap() {
  if (!loc) await refreshLocation();
  let start: { lat: number; lon: number } | null = null;
  if (loc?.set) {
    const la = parseFloat(loc.lat), lo = parseFloat(loc.lon);
    start = Number.isFinite(la) && Number.isFinite(lo) ? { lat: la, lon: lo } : locatorToLatLon(loc.locator);
  }
  openMapPick({ start, onPick: (lat, lon) => void postLocation({ lat, lon }) });
}

// ── The menu: three accordions ──────────────────────────────────────────────────────────────────────────────────────
function el<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Record<string, string> = {}, text = ''): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text) e.textContent = text;
  return e;
}
function row(label: string, ...kids: Node[]): HTMLDivElement {
  const r = el('div', { class: 'mrow' });
  const l = el('label'); l.innerHTML = label; r.appendChild(l);
  for (const k of kids) r.appendChild(k);
  return r;
}
function segOf(items: Array<[string, string]>, current: string, pick: (v: string) => void): HTMLDivElement {
  const s = el('div', { class: 'seg' });
  for (const [v, t] of items) {
    const b = el('button', { class: 'btn' + (v === current ? ' on' : ''), 'data-v': v }, t);
    b.onclick = () => { for (const o of Array.from(s.children)) o.classList.toggle('on', o === b); pick(v); };
    s.appendChild(b);
  }
  return s;
}
function slider(id: string, min: number, max: number, value: number, onSet: (v: number) => void): [HTMLInputElement, HTMLSpanElement] {
  const i = el('input', { type: 'range', id, min: String(min), max: String(max), value: String(value) }) as HTMLInputElement;
  const v = el('span', { class: 'val hhFig', id: id + 'Val' }, String(value));
  let t = 0;
  i.oninput = () => { v.textContent = i.value; clearTimeout(t); t = window.setTimeout(() => onSet(Number(i.value)), 120); };
  return [i, v];
}

const accs: Array<{ btn: HTMLButtonElement; body: HTMLDivElement; unback?: () => void }> = [];
function setOpen(i: number, open: boolean) {
  accs.forEach((a, k) => {
    const o = k === i && open;
    a.btn.setAttribute('aria-expanded', String(o));
    a.body.hidden = !o;
    if (!o && a.unback) { a.unback(); a.unback = undefined; }
    if (o && !a.unback) a.unback = pushBack(() => { a.unback = undefined; setOpen(k, false); });
  });
  if (open) refreshDeviceRows();
}
function accordion(menu: HTMLElement, before: Node | null, title: string): HTMLDivElement {
  const btn = el('button', { class: 'btn hhAcc', 'aria-expanded': 'false' }, title) as HTMLButtonElement;
  const body = el('div', { class: 'hhAccBody' }) as HTMLDivElement;
  body.hidden = true;
  const i = accs.length;
  accs.push({ btn, body });
  btn.onclick = () => setOpen(i, btn.getAttribute('aria-expanded') !== 'true');
  menu.insertBefore(btn, before);
  menu.insertBefore(body, before);
  return body;
}
/** Every top-level child of `menu` from the <h2> named `from` up to (not including) the next <h2> in `stop`. */
function sectionNodes(menu: HTMLElement, from: string, stop: string[]): Node[] {
  const kids = Array.from(menu.childNodes);
  const at = kids.findIndex((n) => n instanceof HTMLElement && n.tagName === 'H2' && n.textContent?.trim() === from);
  if (at < 0) return [];
  const out: Node[] = [];
  for (let i = at; i < kids.length; i++) {
    const n = kids[i];
    if (i > at && n instanceof HTMLElement && (n.tagName === 'H2' && stop.includes(n.textContent?.trim() || '')
        || n.id === 'adminServerRow')) break;
    out.push(n);
  }
  return out;
}

let refreshDeviceRows: () => void = () => {};
function paintVolumeRow(v: number) {
  const i = document.getElementById('hhVolume') as HTMLInputElement | null;
  if (i && document.activeElement !== i) { i.value = String(v); const t = document.getElementById('hhVolumeVal'); if (t) t.textContent = String(v); }
}
function paintBattery() { refreshDeviceRows(); }

/** ★★ The handheld menu (Stuart 2026-11-05): R36S HARDWARE · SDR HARDWARE · DISPLAY at the top, one level, the
 *  rest of the menu (bookmarks, game controller, about) as it was. Called once the receiver's controls exist. */
export function buildHandheldMenu(menu: HTMLElement) {
  if (!on || menu.dataset.hhBuilt) return;
  menu.dataset.hhBuilt = '1';
  // ★ menu's OWN head — every panel has a #menuHead (the id is reused), so getElementById finds another panel's.
  const head = Array.from(menu.children).find((c) => c.id === 'menuHead') || null;
  const firstAfterHead = head ? head.nextSibling : menu.firstChild;
  // SDR HARDWARE: the RADIO and ANTENNA sections, moved (their listeners move with them).
  const radioNodes = sectionNodes(menu, 'RADIO', ['DISPLAY']);
  const displayNodes = sectionNodes(menu, 'DISPLAY', ['GAME CONTROLLER', 'SERVER', 'ABOUT']);
  const r36 = accordion(menu, firstAfterHead, 'R36S HARDWARE');
  const sdr = accordion(menu, firstAfterHead, 'SDR HARDWARE');
  const disp = accordion(menu, firstAfterHead, 'DISPLAY');
  // ★ The VIEW rows that sit at the top of RADIO on a small screen (VFO CENTRE, CENTRE / FIT — .mOnly) are display,
  //   not the radio's hardware: they go to DISPLAY.
  const viewRows: Node[] = [];
  for (const n of radioNodes) {
    if (n instanceof HTMLElement && n.tagName === 'H2' && n.textContent?.trim() === 'RADIO') { n.remove(); continue; }
    if (n instanceof HTMLElement && n.classList.contains('mOnly')) { viewRows.push(n); continue; }
    sdr.appendChild(n);
  }
  // DISPLAY: the handheld's own rows first, then the page's look and view rows.
  const s = hhSettings();
  const [bri, briV] = slider('hhBright', 1, 255, 120, (v) => { savedBright = v; void device({ set: 'brightness', value: v }); });
  disp.appendChild(row('BRIGHTNESS', bri, briV));
  disp.appendChild(row('SPECTRUM RATE<span class="sub">Real frames from the radio each second — every waterfall row is real</span>',
    segOf([['5', '5'], ['10', '10'], ['20', '20']], String(s.specRate), (v) => {
      saveHh({ specRate: Number(v) as 5 | 10 | 20 });
      const sr = handheldSpecRate(); host?.setSpectrumRate(sr.fps, sr.interpolate);
    })));
  disp.appendChild(row('DRAWING RATE<span class="sub">How often the screen is redrawn</span>',
    segOf([['30', '30 fps'], ['60', '60 fps']], String(s.uiFps), (v) => { saveHh({ uiFps: Number(v) as 30 | 60 }); host?.setUiFpsCap(Number(v)); })));
  disp.appendChild(row('SCREEN OFF AFTER<span class="sub">Dims at half the time; audio keeps playing; any button wakes it</span>',
    segOf([['0', 'NEVER'], ['1', '1 m'], ['2', '2 m'], ['5', '5 m'], ['10', '10 m']], String(s.screenOffMin),
          (v) => { saveHh({ screenOffMin: Number(v) }); noteInput(); })));
  for (const n of viewRows) disp.appendChild(n);
  for (const n of displayNodes) {
    if (n instanceof HTMLElement && n.tagName === 'H2' && n.textContent?.trim() === 'DISPLAY') { n.remove(); continue; }
    disp.appendChild(n);
  }
  // ★ The page's waterfall SPEED / DATA / boost rows are a link's controls: the handheld's SPECTRUM RATE replaces them.
  for (const id of ['wfScrollSeg', 'wfDataSeg', 'wfBoostSeg']) document.getElementById(id)?.closest('.mrow')?.classList.add('hhHide');
  for (const id of ['wfScrollNote', 'wfDataNote', 'wfBoostNote']) document.getElementById(id)?.classList.add('hhHide');

  // R36S HARDWARE
  const [vol, volV] = slider('hhVolume', 0, 100, 60, (v) => { void device({ set: 'volume', value: v }); });
  r36.appendChild(row('VOLUME', vol, volV));
  const batt = el('span', { class: 'val hhFig', id: 'hhBattVal', style: 'flex:1' }, '—');
  r36.appendChild(row('BATTERY', batt));
  const clockNow = el('span', { class: 'val hhFig', id: 'hhClockNow', style: 'flex:1' }, '—');
  r36.appendChild(row('CLOCK<span class="sub">UTC</span>', clockNow));
  const nudges = el('div', { class: 'seg', style: 'flex-wrap:wrap' });
  for (const [t, ms] of [['−1 m', -60000], ['−1 s', -1000], ['−0.1 s', -100], ['+0.1 s', 100], ['+1 s', 1000], ['+1 m', 60000]] as Array<[string, number]>) {
    const b = el('button', { class: 'btn' }, t); b.onclick = () => void setClock({ deltaMs: ms, source: `nudge ${t}` }, `(${t})`); nudges.appendChild(b);
  }
  r36.appendChild(row('NUDGE', nudges));
  const setIn = el('input', { type: 'text', id: 'hhClockSet', placeholder: 'YYYY-MM-DD HH:MM:SS', style: 'flex:1;min-width:0' }) as HTMLInputElement;
  const setB = el('button', { class: 'btn' }, 'SET');
  setB.onclick = () => {
    const m = /^(\d{4})-(\d\d)-(\d\d)[ T](\d\d):(\d\d)(?::(\d\d(?:\.\d+)?))?$/.exec(setIn.value.trim());
    if (!m) { toast('Not a time', 'Type it as YYYY-MM-DD HH:MM:SS, in UTC'); return; }
    const ms = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], 0) + Math.round(parseFloat(m[6] || '0') * 1000);
    void setClock({ utcMs: ms, source: 'by hand' }, 'by hand');
  };
  r36.appendChild(row('SET UTC', setIn, setB));
  const stB = el('button', { class: 'btn', id: 'hhClkStation', hidden: '' }) as HTMLButtonElement;
  stB.onclick = () => {
    if (!lastSync) return;
    // ★ (utcMs + rxDelayMs) − hostMs is the error of the ENGINE's clock at the boundary; it moves evenly, so it is
    //   still the error now — applied as a delta at the moment the engine sets it.
    void setClock({ deltaMs: lastSync.utcMs + lastSync.rxDelayMs - lastSync.hostMs, source: lastSync.station }, `from ${lastSync.station}`);
  };
  const ftB = el('button', { class: 'btn', id: 'hhClkFt8', hidden: '' }) as HTMLButtonElement;
  ftB.onclick = () => {
    const m = ft8Median(); if (!m) return;
    // ★ A positive DT = signals arrive late against our clock = our clock is FAST by that much: set it back.
    void setClock({ deltaMs: Math.round(-m.dt * 1000), source: 'FT8' }, `to FT8 (${m.n} decodes)`);
  };
  const clkSeg = el('div', { class: 'seg', style: 'flex-wrap:wrap' }); clkSeg.append(stB, ftB);
  r36.appendChild(clkSeg);
  r36.appendChild(el('div', { class: 'lbl wrap' }, '"Set clock from" appears after a time station (MSF, DCF77, WWV, WWVB, JJY) has given two '
    + 'consecutive parity-checked minutes that agree; "Align to FT8" after three or more FT8 decodes.'));
  // ★★ LOCATION (Stuart 2026-11-05): no GPS, no internet — the owner says where the radio is. A locator (4 or 6
  //    characters, the pad keyboard types it) or exact latitude / longitude; the DEVICE's, whichever radio is plugged
  //    in. Applied live by the engine: distances, ranges and the map centre follow at once.
  const locNow = el('span', { class: 'val hhFig', id: 'hhLocNow', style: 'flex:1' }, '—');
  const locMap = el('button', { class: 'btn', id: 'hhLocMap' }, 'SET FROM MAP');
  locMap.onclick = () => void openLocationMap();
  r36.appendChild(row('LOCATION<span class="sub">Where this receiver is — for distances, ranges and the map</span>', locNow, locMap));
  const locIn = el('input', { type: 'text', id: 'hhLocIn', placeholder: 'IO92nh  or  52.25, -0.89', autocomplete: 'off', style: 'flex:1;min-width:0' }) as HTMLInputElement;
  const locSet = el('button', { class: 'btn', id: 'hhLocSet' }, 'SET');
  const locClr = el('button', { class: 'btn', id: 'hhLocClear' }, 'CLEAR');
  r36.appendChild(row('SET LOCATION', locIn, locSet, locClr));
  locSet.onclick = () => {
    const t = locIn.value.trim();
    const ll = /^(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)$/.exec(t);
    if (ll) void postLocation({ lat: parseFloat(ll[1]), lon: parseFloat(ll[2]) });
    else void postLocation({ locator: t });
  };
  locClr.onclick = () => void postLocation({ clear: true });
  void refreshLocation();
  const cardV = el('span', { class: 'val hhFig', id: 'hhCardVal', style: 'flex:1' }, '—');
  r36.appendChild(row('CARD', cardV));
  const imp = el('button', { class: 'btn' }, 'IMPORT'); imp.onclick = () => void runImport(true);
  const exp = el('button', { class: 'btn' }, 'EXPORT BOOKMARKS'); exp.onclick = () => void exportBookmarks();
  const diag = el('button', { class: 'btn' }, 'EXPORT DIAGNOSTICS') as HTMLButtonElement; diag.onclick = () => void exportDiagnostics(diag);
  const files = el('div', { class: 'seg', style: 'flex-wrap:wrap' }); files.append(imp, exp, diag);
  r36.appendChild(row('FILES', files));
  // Performance
  const perfB = el('button', { class: 'btn' + (hhSettings().perf ? ' on' : ''), id: 'hhPerfBtn' }, hhSettings().perf ? 'ON' : 'OFF');
  perfB.onclick = () => { const n = !hhSettings().perf; setPerf(n); perfB.classList.toggle('on', n); perfB.textContent = n ? 'ON' : 'OFF'; };
  const pseg = el('div', { class: 'seg' }); pseg.appendChild(perfB);
  r36.appendChild(row('PERFORMANCE<span class="sub">An overlay at the top of the screen</span>', pseg));
  const perfFig = el('div', { class: 'lbl wrap hhFig', id: 'hhPerfFig', style: 'white-space:pre' }, '');
  r36.appendChild(perfFig);
  // Power
  const armed: Record<string, number> = {};
  const pw = (label: string, action: string) => {
    const b = el('button', { class: 'btn' }, label) as HTMLButtonElement;
    b.onclick = () => {
      // ★ Twice, on purpose: a pad's A pressed by accident must not switch the radio off.
      if (Date.now() - (armed[action] || 0) > 3000) { armed[action] = Date.now(); b.textContent = 'PRESS AGAIN'; setTimeout(() => { b.textContent = label; }, 3000); return; }
      void pushMemory(true).then(() => device({ action }));
    };
    return b;
  };
  const pseg2 = el('div', { class: 'seg' }); pseg2.append(pw('POWER OFF', 'poweroff'), pw('RESTART', 'reboot'));
  r36.appendChild(row('POWER', pseg2));

  let clockIv = 0;
  refreshDeviceRows = () => {
    void deviceState().then((d) => {
      const st = d.state || {};
      if (typeof st.brightness === 'number' && document.activeElement !== bri) { bri.value = String(st.brightness); briV.textContent = String(st.brightness); }
      if (typeof st.volume === 'number') paintVolumeRow(st.volume);
      if (st.battery && typeof st.battery.pct === 'number') batt.textContent = `${st.battery.pct} %${st.battery.charging ? ' · charging' : ''}`;
      else batt.textContent = st.ok === false ? 'the hardware service is not running' : '—';
    });
    void fetch('/handheld/card', { cache: 'no-store' }).then((r) => r.json()).then((c) => {
      cardV.textContent = c.present ? `${(c.freeBytes / 2 ** 30).toFixed(1)} GB free of ${(c.totalBytes / 2 ** 30).toFixed(1)} GB` : 'no card';
    }).catch(() => { cardV.textContent = '—'; });
    void fetch('/handheld/perf', { cache: 'no-store' }).then((r) => r.json()).then((p: Perf) => {
      lastPerf = p; perfFig.textContent = perfLines(p).join('\n');
      if (p.battery.pct >= 0 && !p.battery.charging && p.battery.minutesLeft >= 0) batt.textContent += ` · about ${Math.floor(p.battery.minutesLeft / 60)} h ${p.battery.minutesLeft % 60} min left`;
    }).catch(() => {});
    paintClockKeys();
    clearInterval(clockIv);
    clockIv = window.setInterval(() => {
      if (r36.hidden) { clearInterval(clockIv); return; }
      clockNow.textContent = new Date().toISOString().replace('T', ' ').slice(0, 21);
      if (lastPerf) perfFig.textContent = perfLines(lastPerf).join('\n');
      paintClockKeys();
    }, 100);
  };
  if (lastVolume >= 0) paintVolumeRow(lastVolume);
}

/** The DEVICE key (where CHAT sits on a server — the island keeps both cells): the menu, R36S HARDWARE open. */
export function openDeviceMenu() {
  host?.openMenu();
  const i = accs.findIndex((a) => a.btn.textContent === 'R36S HARDWARE');
  if (i >= 0) setOpen(i, true);
}
