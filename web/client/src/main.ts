/**
 * main.ts — VibeSDR web client entry.
 *
 * Talks to the VibeServer shim running on a phone. Same origin when served by
 * the shim itself (GET /), so the WS URLs are relative; in dev the splash takes
 * an explicit host:port.
 */

import { SpectrumClient, MODE_BANDWIDTHS, type SDRMode } from './spectrum';
import { AudioPlayer } from './audio';
import { Waterfall } from './waterfall';
import { resolveAuth, withAuth, type AuthState } from './auth';
import { COLORMAP_NAMES } from '../../../src/assets/colormapUtils';
import { stepsForFreq } from '../../../src/services/sdrTypes';

/** The fastest an RTL-SDR can actually sustain over USB. Above this the dongle DROPS
 *  SAMPLES — audio glitches, gaps in the waterfall — and it does so silently, which is
 *  what makes it a trap rather than a trade-off. Higher rates stay on offer (some dongles
 *  and some hosts cope, and the extra span is real), but they are never the default and
 *  never unlabelled. */
const RTL_SAFE_RATE = 2_400_000;
import {
  BAND_PLAN, getBandsAtRegion, bandTuneDefaults, type Band,
} from '../../../src/constants/bandPlan';
import { deriveItuRegion } from '../../../src/services/stations';
import { resolveStationIso, isoToFlag, ituToIso } from '../../../src/services/rdsCountry';
import { countryForCallsign } from '../../../src/services/callsignCountry';
import { abbrCountry } from '../../../src/assets/countryAbbr';
import { gridToLatLon, haversineKm } from '../../../src/services/grid';
import { lookupStationLogo } from '../../../src/services/stationLogo';
import {
  loadStations, loadBookmarks, getBookmarks, getStations, addBookmark, removeBookmark,
  exportBookmarks, importBookmarks, search, type SearchResult,
  loadServerBookmarks, getServerBookmarks, saveToServer, removeFromServer,
} from './search';
import { parseBookmarksAny } from '../../../src/services/userBookmarks';
import { DecoderClient, type Spot } from './decoders';
import {
  saveRecording, listRecordings, deleteRecording, formatSize, formatDuration,
} from './recordings';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const MODES: SDRMode[] = ['wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl'];
const LS_SERVERS = 'vibesdr_web_servers_v1';   // { "host:port": pin }
const LS_PREFS   = 'vibesdr_web_prefs_v1';

let step = 1000;                 // tuning step, Hz (restored from prefs on load)
let spec: SpectrumClient | null = null;
let audio: AudioPlayer | null = null;
let wf: Waterfall | null = null;
/** Last config's frequency window — see onConfig, for the stale-history wipe. */
let lastWindow: { lo: number; hi: number } | null = null;

// ── Saved servers (PIN per host:port, like the app) ──────────────────────────

function savedServers(): Record<string, string> {
  try { return JSON.parse(localStorage.getItem(LS_SERVERS) || '{}'); } catch { return {}; }
}
function saveServer(host: string, pin: string) {
  const s = savedServers();
  s[host] = pin;
  localStorage.setItem(LS_SERVERS, JSON.stringify(s));
}
/** Frequency + mode are remembered PER SERVER — two receivers rarely want the
 *  same dial. Everything else (display, DSP, hardware) is global. */
let currentHost = '';

function lastTuned(): { hz: number; mode: SDRMode } | null {
  const all = (prefs().tuned ?? {}) as Record<string, { hz: number; mode: SDRMode }>;
  const t = all[currentHost];
  return t && t.hz > 0 ? t : null;
}

function saveTuned() {
  if (!spec || !currentHost || !spec.frequency) return;
  const all = (prefs().tuned ?? {}) as Record<string, { hz: number; mode: SDRMode }>;
  all[currentHost] = { hz: Math.round(spec.frequency), mode: spec.mode };
  savePref('tuned', all);
}

function prefs(): Record<string, unknown> {
  try { return JSON.parse(localStorage.getItem(LS_PREFS) || '{}'); } catch { return {}; }
}
function savePref(k: string, v: unknown) {
  const p = prefs(); p[k] = v;
  localStorage.setItem(LS_PREFS, JSON.stringify(p));
}

// ── Splash ───────────────────────────────────────────────────────────────────

function initSplash() {
  const hostEl = $<HTMLInputElement>('host');
  const pinEl  = $<HTMLInputElement>('pin');
  const msg    = $('splashMsg');

  // When the shim serves this page, the VibeServer IS this origin — there is
  // nothing for the user to type, so the address field doesn't exist. It only
  // appears on the dev server (port 8080), which is served from the Mac and has
  // to be told which radio to talk to.
  const isDev = location.port === '8080' || !location.host;
  const saved = savedServers();

  if (isDev) {
    $('hostRow').hidden = false;
    hostEl.value = (prefs().lastHost as string) || 'localhost:48000';
  } else {
    hostEl.value = location.host;
  }
  if (saved[hostEl.value]) pinEl.value = saved[hostEl.value];

  const go = async (remember: boolean) => {
    const host = hostEl.value.trim().replace(/^https?:\/\//, '').replace(/\/$/, '');
    const pin = pinEl.value.trim();
    if (!host) { msg.textContent = 'Enter a server address'; return; }
    msg.className = 'info';
    msg.textContent = 'Connecting…';
    $<HTMLButtonElement>('btnConnect').disabled = true;
    $<HTMLButtonElement>('btnSaveConnect').disabled = true;
    try {
      await connect(host, pin);
      if (remember) saveServer(host, pin);
      savePref('lastHost', host);
    } catch (e) {
      msg.className = '';
      msg.textContent = e instanceof Error ? e.message : String(e);
      $<HTMLButtonElement>('btnConnect').disabled = false;
      $<HTMLButtonElement>('btnSaveConnect').disabled = false;
    }
  };

  $<HTMLFormElement>('connForm').addEventListener('submit', (e) => { e.preventDefault(); go(false); });
  $('btnSaveConnect').addEventListener('click', () => go(true));

  // Ask the server whether it even wants a PIN, and shape the splash to the
  // answer: no PIN => a single START button, nothing to fill in. Don't
  // auto-connect — the click is also the user gesture the browser wants before
  // it will start audio.
  if (!isDev) void shapeSplash(hostEl.value);
}

/** No PIN on this server? Then there is nothing to ask — just START. */
async function shapeSplash(host: string) {
  try {
    const r = await fetch(`http://${host}/vibeserver/auth`, { cache: 'no-store' });
    const j = await r.json();
    if (j.required) return;                      // PIN needed: leave the form as-is
    $('pinRow').hidden = true;
    $<HTMLButtonElement>('btnSaveConnect').hidden = true;
    $('btnConnect').textContent = 'START';
  } catch {
    // Unreachable — leave the form up so the error is visible.
  }
}

// ── Connect ──────────────────────────────────────────────────────────────────

/** UUID v4. NOT crypto.randomUUID() — that is secure-context-only, and a
 *  VibeServer is plain http:// on a LAN IP, so it's undefined there.
 *  crypto.getRandomValues() has no such restriction. */
function uuid(): string {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const h = [...b].map(x => x.toString(16).padStart(2, '0')).join('');
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

async function connect(host: string, pin: string) {
  currentHost = host;
  const httpBase = `http://${host}`;
  const wsBase = `ws://${host}`;

  let auth: AuthState;
  try {
    auth = await resolveAuth(httpBase, pin);
  } catch (e) {
    if (e instanceof Error && e.message === 'PIN required') throw new Error('This server needs a PIN');
    // Only a genuine fetch failure means "unreachable". Anything else is a real
    // error and must surface as itself — flattening everything into "can't
    // reach" is what hid the secure-context crypto failure.
    if (e instanceof TypeError) {
      throw new Error(`Can't reach ${host} — check the address and that Server mode is running`);
    }
    throw e;
  }

  const specUrl  = `${wsBase}${withAuth('/ws/user-spectrum?user_session_id=' + uuid() + '&mode=binary8', auth)}`;
  const audioUrl = `${wsBase}${withAuth('/ws/audio', auth)}`;

  // The shim only rejects a bad PIN at WS-upgrade time (401), so surface that
  // as a splash error rather than silently retrying forever.
  await new Promise<void>((resolve, reject) => {
    const probe = new WebSocket(specUrl);
    const t = setTimeout(() => { probe.close(); reject(new Error('Server did not respond')); }, 6000);
    probe.onopen = () => { clearTimeout(t); probe.close(); resolve(); };
    probe.onerror = () => { clearTimeout(t); reject(new Error(auth.required ? 'Wrong PIN, or server refused the connection' : 'Server refused the connection')); };
  });

  startApp(specUrl, audioUrl, host, auth);
}

// ── App ──────────────────────────────────────────────────────────────────────

function startApp(specUrl: string, audioUrl: string, host: string, auth: AuthState) {
  authState = auth;
  $('splash').classList.add('hidden');
  $('app').classList.add('live');

  const canvas = $<HTMLCanvasElement>('wf');
  const p = prefs();
  wf = new Waterfall(canvas, {
    palette: (p.palette as string) || 'gqrx',
    // The pref stores the SLIDER's value (0-60 percent), not a fraction.
    specRatio: typeof p.specRatio === 'number' ? p.specRatio / 100 : 0.25,
  });

  wf.onDrawAxis = (ctx, w, h) => drawDbAxis(ctx, w, h);

  spec = new SpectrumClient(specUrl, {
    onBins: (bins, centerHz, bwHz) => {
      noteFrame();
      wf!.push(bins, centerHz, bwHz);
      updateSignal(bins, centerHz, bwHz);
    },
    onConfig: (cfg) => {
      // Drop stale waterfall history when the new window shares no frequency with
      // the old one. Overlapping windows (pan, zoom) keep their history — the rows
      // are still about the band you're looking at. A jump has nothing in common
      // with what's on screen, and letting it scroll out takes ~30s at 18 rows/s.
      const span = cfg.binCount * cfg.binBandwidth;
      const lo = cfg.centerFreq - span / 2, hi = cfg.centerFreq + span / 2;
      if (wf && lastWindow && (hi <= lastWindow.lo || lo >= lastWindow.hi)) wf.clearHistory();
      lastWindow = { lo, hi };

      if (!spec!.frequency) {
        // A shared link's frequency wins over the remembered dial.
        if (applyShareParams()) return;
        // First config. Resume where this server was left, if we've been here
        // before; otherwise park the VFO at the view centre.
        const last = lastTuned();
        spec!.frequency = last?.hz ?? cfg.centerFreq;
        setMode(last?.mode ?? spec!.mode, !!last);
        renderFreq();
        if (last) spec!.tune(last.hz, last.mode, { recenter: true });
      }
    },
    onHwInfo: (gains, rates, locked) => { hwGains = gains; hwRates = rates; hwLockedRate = locked; populateHw(); },
    onRds: (m) => {
      $('stereo').classList.toggle('on', m.stereo);
      // RDS is the station naming itself — it outranks any bookmark guess.
      const ps = m.ps.trim();
      const rt = m.radiotext.trim();
      // PS is the station's NAME (8 chars); RadioText is its message. They are
      // different things and the app shows both — don't collapse them.
      if (ps !== rdsName) {
        rdsName = ps;
        rdsLogoUrl = '';
      }
      rdsText = rt;
      if (!rdsName && rt) rdsName = rt;   // some stations send only RadioText
      // Transmitter country from the RDS Extended Country Code + PI, as the app
      // does (rdsCountry.eccPiToIso) — that's what the flag comes from.
      // ECC + PI when the station sends an ECC (unambiguous); otherwise the PI's
      // country nibble CHECKED against the receiver's own country — a validation, not
      // an assumption. A Spanish station on sporadic-E has a nibble inconsistent with a
      // British receiver, so it resolves to nothing rather than to a wrong flag.
      rdsIso = m.pi > 0
        ? resolveStationIso(m.ecc || undefined, m.pi.toString(16), serverIso || undefined)
        : '';
      if (rdsName) void resolveRdsLogo(rdsName, rdsIso);
      updateVts();
    },
    onStatus: (s, detail) => {
      setStatus(s, detail);
      // Server-side settings live on the SERVER, so restoring the sliders isn't
      // enough — they have to be re-sent, or the UI shows values the radio isn't
      // actually using. Also covers reconnects, where the shim starts fresh.
      if (s === 'open') pushSettingsToServer();
    },
    onRtt: (ms) => { rtt = ms; },
    onBytes: (n) => { specBytes += n; },
  });
  spec.connect();

  audio = new AudioPlayer(audioUrl, {
    onBytes: (n) => { audioBytes += n; },
    onStatus: (s) => { if (s === 'error') setStatus('error', 'audio'); },
  });
  // The AudioContext is built after several awaits, so the browser no longer
  // credits it to the Connect click and may leave it suspended. Rather than rely
  // on that chain surviving, always arm a resume on the next real interaction.
  audio.start().catch((e) => console.error('audio start failed', e));

  // PERMANENT resume. Anything that steals focus — a native dialog, a tab switch,
  // the OS — can suspend the AudioContext, and an unsuspend handler that removes
  // itself once it has worked ONCE leaves you stuck with no audio and no way back
  // except a page reload. On a desktop there is no reason to ever sit suspended.
  const kick = () => { void audio?.resume(); };
  for (const ev of ['pointerdown', 'keydown', 'focus'] as const) {
    window.addEventListener(ev, kick);
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden) kick(); });

  buildControls();
  initMediaSession();
  initDecoders(host, auth);
  initIdleThrottle();
  window.addEventListener('resize', () => { wf!.resize(); });
  window.addEventListener('beforeunload', saveTuned);
  requestAnimationFrame(loop);
}

// ── Render loop ──────────────────────────────────────────────────────────────

let rtt = 0;
let audioBytes = 0;
let specBytes = 0;
let lastBytesAt = performance.now();
let audioKbps = 0;
let specKbps = 0;
let hwGains: number[] = [];
let hwRates: number[] = [];
/** >0 = the SERVER pinned the capture rate; the picker is hidden. */
let hwLockedRate = 0;
/** The resolved PIN credentials, kept so bookmark WRITES can carry them. */
let authState: AuthState | null = null;

function loop() {
  if (!wf || !spec) return;
  wf.vfoHz = spec.frequency;
  updateViewOverlays();
  // Passband drives the acrylic sidebands — so bandwidth is something you SEE
  // sitting on the signal, not a number you read.
  wf.filterLow = spec.bandwidthLow;
  wf.filterHigh = spec.bandwidthHigh;
  wf.tick();      // synthesise any waterfall lines now due (see Waterfall.tick)
  wf.draw();
  drawScale();
  drawBands();

  const now = performance.now();
  if (now - lastBytesAt > 1000) {
    const secs = (now - lastBytesAt) / 1000;
    audioKbps = (audioBytes / 1024) / secs;
    specKbps  = (specBytes / 1024) / secs;
    audioBytes = 0;
    specBytes = 0;
    lastBytesAt = now;
    updateStatus();
    updateRecTime();
    checkIdle();
    saveTuned();   // once a second, not per tune — a drum-fast nudge would thrash localStorage
  }
  requestAnimationFrame(loop);
}

// ── Frequency scale ──────────────────────────────────────────────────────────

/** Pick a tick step that yields ~6-10 labels across the span, from a 1/2/5 ladder. */
function tickStep(spanHz: number, targetTicks: number): number {
  const raw = spanHz / targetTicks;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 5, 10]) if (raw <= mag * m) return mag * m;
  return mag * 10;
}

/** Label a frequency at a resolution matched to the tick step — the units must
 *  SWITCH with the span, not just the decimal places, or a wide span overflows
 *  the label and a narrow one shows nothing changing. */
function fmtTick(hz: number, step: number): string {
  if (step >= 1e6) return (hz / 1e6).toFixed(0) + 'M';
  if (step >= 1e5) return (hz / 1e6).toFixed(1) + 'M';
  if (step >= 1e4) return (hz / 1e6).toFixed(2) + 'M';
  if (step >= 1e3) return (hz / 1e6).toFixed(3) + 'M';
  if (step >= 100) return (hz / 1e3).toFixed(1) + 'k';
  return (hz / 1e3).toFixed(2) + 'k';
}

function drawScale() {
  const c = $<HTMLCanvasElement>('scale');
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.round(c.clientWidth * dpr);
  const h = Math.round(c.clientHeight * dpr);
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const ctx = c.getContext('2d')!;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  if (!spec || !wf || !wf.spanHz) return;

  const span = wf.spanHz;
  const lo = wf.displayCenterHz() - span / 2;
  const step = tickStep(span, 8);
  const first = Math.ceil(lo / step) * step;

  ctx.font = `${11 * dpr}px ui-monospace, Menlo, monospace`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'bottom';

  for (let f = first; f < lo + span; f += step) {
    const x = ((f - lo) / span) * w;
    ctx.strokeStyle = 'rgba(255,160,0,0.35)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x + 0.5, h);
    ctx.lineTo(x + 0.5, h - 5 * dpr);
    ctx.stroke();
    ctx.fillStyle = 'rgba(255,184,51,0.75)';
    ctx.fillText(fmtTick(f, step), x, h - 7 * dpr);
  }

  // VFO marker on the scale.
  const vx = ((spec.frequency - lo) / span) * w;
  if (vx >= 0 && vx <= w) {
    ctx.strokeStyle = 'rgba(255,229,102,0.9)';
    ctx.beginPath();
    ctx.moveTo(vx + 0.5, 0);
    ctx.lineTo(vx + 0.5, h);
    ctx.stroke();
  }
}


// ── Band-plan strip ──────────────────────────────────────────────────────────
// The coloured bar above the ticker: which bands the current span crosses. The
// app draws the same thing (WaterfallView BAND_H) — without it the spectrum is
// just numbers, and you can't see that you're sitting in the middle of 40m.

// BAND_COLS — verbatim from the app (WaterfallView). Everything was one shade of
// amber before, so the bands were indistinguishable.
const BAND_COLS: Record<string, string> = {
  ham:       'rgba(207,0,0,0.92)',
  broadcast: 'rgba(9,0,255,0.92)',
  utility:   'rgba(7,189,0,0.92)',
  cb:        'rgba(255,119,0,0.92)',
};

/** 11m CB special-case: typed 'utility' in bandPlan.ts but coloured orange. */
function bandColour(b: Band): string {
  if (b.name.includes('CB')) return BAND_COLS.cb;
  return BAND_COLS[b.type] ?? BAND_COLS.utility;
}

/**
 * ITU region, from the receiver's longitude — the app derives it exactly this way
 * (deriveItuRegion(serverLongitude ?? recvLon)). It MATTERS: the 80m ham band is
 * 3.5–3.8 in R1 but 3.5–4.0 in R2, and the AM broadcast band's top edge and
 * channel spacing differ too. Showing the wrong region's edges is worse than
 * showing none. Falls back to R1 until a grid is set.
 */
function ituRegion(): number {
  const me = myPos();
  return deriveItuRegion(me ? me.lon : undefined) || 1;
}

function drawBands() {
  const c = $<HTMLCanvasElement>('bands');
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.round(c.clientWidth * dpr);
  const h = Math.round(c.clientHeight * dpr);
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const ctx = c.getContext('2d')!;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  if (!wf || !wf.spanHz) return;

  const span = wf.spanHz;
  const lo = wf.displayCenterHz() - span / 2;
  const hi = lo + span;
  const xOf = (hz: number) => ((hz - lo) / span) * w;

  ctx.font = `${10 * dpr}px ui-monospace, Menlo, monospace`;
  ctx.textBaseline = 'middle';

  const region = ituRegion();

  for (const b of BAND_PLAN) {
    if (b.hi < lo || b.lo > hi) continue;                       // not in view
    // Region-scoped: an 80m edge or an AM band-top from the wrong ITU region is
    // simply the wrong information.
    if (b.regions && b.regions.length && !b.regions.includes(region)) continue;

    const x0 = Math.max(0, xOf(b.lo));
    const x1 = Math.min(w, xOf(b.hi));
    if (x1 - x0 < 1) continue;

    ctx.fillStyle = bandColour(b);
    ctx.fillRect(x0, 0, x1 - x0, h);

    // Full title first ("40m Ham Band"), as the app shows it. Only fall back to
    // the short label ("40m") when the segment genuinely can't hold the full one.
    const full = b.name;
    const short = b.bandLabel || b.name;
    const wFull = ctx.measureText(full).width;
    const wShort = ctx.measureText(short).width;
    const room = x1 - x0;

    let label = '';
    let tw = 0;
    if (room > wFull + 8 * dpr) { label = full; tw = wFull; }
    else if (room > wShort + 8 * dpr) { label = short; tw = wShort; }

    if (label) {
      ctx.fillStyle = 'rgba(0,0,0,0.55)';
      ctx.fillText(label, (x0 + x1) / 2 - tw / 2 + dpr, h / 2 + dpr);
      ctx.fillStyle = 'rgba(255,255,255,0.95)';
      ctx.fillText(label, (x0 + x1) / 2 - tw / 2, h / 2);
    }
  }
}

// ── VTS — station steward ────────────────────────────────────────────────────
// Station identity: RDS when we have it, otherwise the nearest bookmark/EiBi
// station within 150 kHz. Green when dead on it (±99 Hz) — the app's thresholds
// (stations.ts VTS_ON_HZ / VTS_MAX_KHZ), so the two behave the same.

// ON-TUNE ONLY. The skin's "nearest bookmark within 150 kHz, with an offset and
// an arrow" was dropped from the app because it threw false positives — a station
// 80 kHz away is not the one you're listening to, and saying so is worse than
// saying nothing. VTS appears only when you're essentially ON the bookmark.
const VTS_ON_HZ = 99;

let rdsName = '';
let rdsText = '';   // RDS RadioText — the message, distinct from the PS name
let rdsIso = '';        // transmitter country, from RDS ECC + PI
let rdsLogoUrl = '';    // resolved station logo (radio-browser)

/**
 * Logos for the bookmark LIST, resolved lazily and remembered.
 *
 * Deliberately logo-only: the RDS name is what the transmitter itself broadcasts, so
 * it is authoritative, and letting a crowd-sourced database overrule it risks turning
 * a correct "Heart" into "Heart 96.6 (Northampton)". The logo is the part the internet
 * can add that RDS never could, and it can't corrupt anything.
 *
 * null = looked up, nothing found — cached so we don't ask again on every render.
 */
const bmLogos = new Map<string, string | null>();
let logoQuery = '';     // guards against a stale async logo landing late

function updateVts() {
  if (!spec) return;
  const hz = spec.frequency;
  // Region-aware, ham before broadcast before utility — the app's VTS ordering.
  const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
  const band = getBandsAtRegion(hz, ituRegion())
    .sort((a, b) => (order[a.type] ?? 9) - (order[b.type] ?? 9))[0] ?? null;
  const vts = $('vts');

  // RDS wins — it's the station telling you who it is, rather than us guessing
  // from a bookmark that happens to be nearby.
  let name = rdsName;
  let flag = rdsName ? isoToFlag(rdsIso) : '';
  let src = '';
  let logo = rdsName ? rdsLogoUrl : '';

  // RDS is always genuine — the station is naming itself. A bookmark only counts
  // when we're actually sitting on it.
  if (!name) {
    const near = nearestStation(hz);
    if (near && Math.abs(near.frequency - hz) <= VTS_ON_HZ) {
      name = near.name;
      flag = near.flag || '';
      // Source mark, as in the app: EiBi schedule vs the user's own bookmark.
      src = SRC_LABEL[near.source] ?? '';
      // A logo for a BOOKMARK-identified station too, not only an RDS one. The logo
      // was gated on rdsName, so an AM or shortwave station recognised from a bookmark
      // showed its name and its source glyph but never its logo — even though the same
      // station carried one perfectly well in the bookmark list two panels away.
      logo = bmLogos.get(`${near.name}|`) ?? '';
      if (!bmLogos.has(`${near.name}|`)) void primeStationLogo(near.name);
    }
  }

  // Nothing known here — hide it rather than show an empty bar.
  if (!name) {
    vts.classList.remove('show', 'on');
    setDecBoxOffset();
    return;
  }

  $('vtsName').textContent = name;
  $('vtsBand').textContent = band ? (band.bandLabel || band.name) : '';
  $('vtsFlag').textContent = flag;

  // RadioText, when the station is sending one. Scroll it only if it actually
  // overflows — a short message shouldn't slide around for no reason.
  const rtEl = $('vtsRt');
  const rtInner = $('vtsRtInner');
  const rt = rdsName ? rdsText : '';
  const showRt = !!rt && rt !== name;
  if (rtInner.textContent !== rt) rtInner.textContent = rt;
  rtEl.classList.toggle('show', showRt);
  // Pin the bar's width while RadioText is on show. Without this the bar is
  // content-sized under a max-width, so it simply GREW to fit each message — which
  // meant the text never overflowed, never scrolled, and the whole bar visibly
  // expanded and contracted on every RadioText update instead.
  vts.classList.toggle('rt', showRt);
  if (showRt) requestAnimationFrame(() => fitRadioText(rtEl, rtInner));

  // RDS mark only when the data really IS RDS — not for a bookmark guess.
  $('vtsRds').classList.toggle('show', !!rdsName);
  const srcEl = $('vtsSrc');
  // innerHTML, not textContent: the source mark is an inline SVG glyph now, and
  // textContent would print the markup as literal text.
  srcEl.innerHTML = src;
  srcEl.classList.toggle('show', !!src && !rdsName);

  const logoEl = $<HTMLImageElement>('vtsLogo');
  if (logo) {
    if (logoEl.src !== logo) logoEl.src = logo;
    logoEl.classList.add('show');
  } else {
    logoEl.classList.remove('show');
  }

  vts.classList.add('show');
  vts.classList.add('on');   // if it's showing at all, we're on the station
  setDecBoxOffset();
}

/**
 * Station logo for the RDS station (radio-browser, the same source the app's
 * FM-DX tuner uses). Needs internet ON THIS MACHINE, which a desktop has even
 * when the phone is on a hotspot — and it degrades to no logo if not.
 */
async function resolveRdsLogo(name: string, iso: string) {
  const key = `${name}|${iso}`;
  if (logoQuery === key) return;
  logoQuery = key;
  rdsLogoUrl = '';
  try {
    const url = await lookupStationLogo(name, iso || undefined, serverIso || undefined);
    // A slow lookup must not overwrite a station we've since tuned away from.
    if (logoQuery !== key) return;
    rdsLogoUrl = url || '';
    updateVts();
  } catch {
    /* no logo — the monogram-less bar is fine */
  }
}

/** Marquee the RadioText only when it doesn't fit. */
function fitRadioText(box: HTMLElement, inner: HTMLElement) {
  const overflow = inner.scrollWidth - box.clientWidth;
  if (overflow > 4) {
    inner.style.setProperty('--rtShift', `${-overflow - 8}px`);
    // ~30px/sec, so a long message takes its time rather than whipping past.
    inner.style.animationDuration = `${Math.max(6, (overflow + 8) / 30 * 2)}s`;
    inner.classList.add('scroll');
  } else {
    inner.classList.remove('scroll');
    inner.style.removeProperty('--rtShift');
  }
}

/** Keep the decoder box clear of the VTS bar — same idea as the app's
 *  DecoderPanel bottomOffset (it rides above the pill). */
function setDecBoxOffset() {
  const vts = $('vts');
  const showing = vts.classList.contains('show');
  const h = showing ? vts.offsetHeight + 10 : 0;
  document.documentElement.style.setProperty('--decBoxBottom', `${14 + h}px`);
}

// ── dB axis ──────────────────────────────────────────────────────────────────
// Five stops down the left of the spectrum, with faint reference lines — same as
// the app. Without it the trace has no scale at all.

function drawDbAxis(ctx: CanvasRenderingContext2D, W: number, H: number) {
  if (H < 30 || !wf) return;
  const { dbMin, dbMax } = wf.getRange();
  if (!isFinite(dbMin) || !isFinite(dbMax) || dbMax <= dbMin) return;

  const dpr = Math.min(2, window.devicePixelRatio || 1);
  ctx.font = `${10 * dpr}px ui-monospace, Menlo, monospace`;
  ctx.textBaseline = 'middle';
  ctx.textAlign = 'left';

  const STOPS = 5;
  for (let i = 0; i < STOPS; i++) {
    const t = i / (STOPS - 1);
    const y = t * H;
    const db = dbMax - t * (dbMax - dbMin);

    ctx.strokeStyle = 'rgba(255,180,60,0.10)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, y + 0.5);
    ctx.lineTo(W, y + 0.5);
    ctx.stroke();

    const label = `${db.toFixed(0)}`;
    const ly = Math.max(6 * dpr, Math.min(H - 6 * dpr, y));
    ctx.fillStyle = 'rgba(0,0,0,0.8)';
    ctx.fillText(label, 4 * dpr + dpr, ly + dpr);
    ctx.fillStyle = 'rgba(255,180,60,0.90)';
    ctx.fillText(label, 4 * dpr, ly);
  }
}

// ── Signal meter (derived from the SPEC bins — the shim sends no S-meter) ────

let sigSmooth = 0, sigPeak = 0;

let snrSmooth = 0;

function updateSignal(bins: Float32Array, centerHz: number, bwHz: number) {
  if (!spec) return;
  const n = bins.length;
  const hzPerBin = bwHz / n;
  const lo = centerHz - bwHz / 2;
  const b0 = Math.max(0, Math.floor((spec.frequency + spec.bandwidthLow - lo) / hzPerBin));
  const b1 = Math.min(n - 1, Math.ceil((spec.frequency + spec.bandwidthHigh - lo) / hzPerBin));

  // Signal = strongest bin in the demod passband.
  let sigDb = -160;
  for (let i = b0; i <= b1; i++) if (bins[i] > sigDb) sigDb = bins[i];

  // Noise floor = a low percentile of the WHOLE frame. Not the mean: a strong
  // carrier drags a mean upward and the SNR reads low exactly when the signal is
  // strongest. Sampled every 8th bin — this runs per frame.
  const sample: number[] = [];
  for (let i = 0; i < n; i += 8) sample.push(bins[i]);
  sample.sort((a, b) => a - b);
  const noiseDb = sample[Math.floor(sample.length * 0.25)] ?? -120;

  const snr = Math.max(0, sigDb - noiseDb);
  snrSmooth += (snr - snrSmooth) * 0.2;

  const { dbMin, dbMax } = wf!.getRange();
  const norm = Math.max(0, Math.min(1, (sigDb - dbMin) / Math.max(1, dbMax - dbMin)));

  // Asymmetric smoothing: fast attack, slow decay (same feel as the app's meter).
  sigSmooth += (norm - sigSmooth) * (norm > sigSmooth ? 0.55 : 0.18);
  sigPeak = norm > sigPeak ? norm : Math.max(norm, sigPeak - 0.004);

  $('sigFill').style.width = `${(sigSmooth * 100).toFixed(1)}%`;
  $('sigPeak').style.left = `${(sigPeak * 100).toFixed(1)}%`;
  $('sigLabel').textContent =
    `${sigDb.toFixed(0)} dBFS · ${toSUnit(sigDb)} · SNR ${snrSmooth.toFixed(0)} dB`;
}

/** dBFS -> S-unit, 6 dB per unit (lifted from the skin's _toSUnit ladder). */
function toSUnit(dbfs: number): string {
  if (dbfs >= -73) return `S9+${Math.min(60, Math.round((dbfs + 73) / 6) * 6)}`;
  const ladder = [-79, -85, -91, -97, -103, -109, -115];
  for (let i = 0; i < ladder.length; i++) if (dbfs >= ladder[i]) return `S${8 - i}`;
  return 'S1';
}

// ── Status ───────────────────────────────────────────────────────────────────

function setStatus(s: string, detail?: string) {
  const el = $('status');
  if (s === 'closed' || s === 'error') {
    el.innerHTML = `<span class="bad">${escapeHtml(detail || s.toUpperCase())}</span>`;
  } else if (s === 'connecting') {
    el.textContent = 'CONNECTING…';
  } else {
    updateStatus();
  }
}

// ── Link quality ─────────────────────────────────────────────────────────────
//
// Measured from SPEC FRAME TIMING, not RTT — a link that has stopped delivering
// frames is broken even if pings still come back, and that's what the app keys
// off too (its own note: an FFT-timing reading taken after the jitter buffer
// "stays green while the network is failing").
//
//   3 green  frames arriving on schedule
//   2 amber  gaps up to 3x the expected interval — jitter or drops
//   1 red    stalled: nothing for over 3x
//   0 (✕)    socket down

let lastFrameAt = 0;
let linkQ: 0 | 1 | 2 | 3 = 0;

function noteFrame() {
  const now = performance.now();
  const expected = 1000 / (throttled ? IDLE_FPS : ACTIVE_FPS);
  if (lastFrameAt) {
    const gap = now - lastFrameAt;
    if (gap > expected * 3) linkQ = 1;
    else if (gap > expected * 1.6) linkQ = 2;
    else linkQ = 3;
  }
  lastFrameAt = now;
}

function updateLink() {
  // No frame for a long time = stalled, regardless of what the last gap said.
  if (!spec || !lastFrameAt) linkQ = 0;
  else {
    const expected = 1000 / (throttled ? IDLE_FPS : ACTIVE_FPS);
    const since = performance.now() - lastFrameAt;
    if (since > expected * 8) linkQ = 1;
    if (since > 5000) linkQ = 0;
  }
  const el = $('linkBars');
  el.className = `q${linkQ}`;
}

function updateStatus() {
  updateLink();

  // The SPECTRUM is the bigger half of the link (~74 KB/s vs ~47 for audio), so
  // reporting only the audio understated the real traffic by more than half.
  const total = audioKbps + specKbps;
  const idle = throttled ? ` · IDLE ${IDLE_FPS}fps` : '';
  const el = $('status');
  el.textContent = `${total.toFixed(0)} KB/s · ${rtt.toFixed(0)} ms${idle}`;
  el.title = `spectrum ${specKbps.toFixed(0)} KB/s · audio ${audioKbps.toFixed(0)} KB/s`;

  // Faults go on the METER, not into the status text: a long message there ran
  // off the edge of the screen, and the meter is where you're already looking
  // when you're wondering why there's no sound.
  let fault = '';
  let info = '';
  switch (audio?.health) {
    case 'suspended': fault = 'AUDIO PAUSED — CLICK THE PAGE'; break;
    case 'no-stream': fault = 'AUDIO DISCONNECTED'; break;
    case 'silent':    fault = 'NO SOUND — IS THE TAB MUTED?'; break;
    case 'squelched': info  = 'NO AUDIO — SQUELCH ENABLED'; break;
  }
  // A fault replaces the meter and pulses, to grab attention. Squelch does neither:
  // it is expected behaviour, and the meter is exactly what you want to watch while
  // waiting for a signal to break the threshold.
  $('sig').classList.toggle('fault', !!fault);
  $('sig').classList.toggle('squelched', !fault && !!info);
  $('sigFault').textContent = fault || info;
  $('sigFault').classList.toggle('show', !!fault || !!info);
  $('sigFault').classList.toggle('info', !fault && !!info);
}

// ── Controls ─────────────────────────────────────────────────────────────────

function buildControls() {
  // Mode buttons
  const modes = $('modes');
  modes.innerHTML = '';
  for (const m of MODES) {
    const b = document.createElement('button');
    b.className = 'btn';
    b.textContent = m.toUpperCase();
    b.dataset.mode = m;
    b.onclick = () => setMode(m, true);
    modes.appendChild(b);
  }

  buildVfo();

  // No anchor = zoom about the LISTEN VFO: the station you're on stays put and the
  // span closes in around it.
  $('zoomIn').onclick    = () => { spec!.zoomBy(2); updateViewOverlays(); };
  $('zoomOut').onclick   = () => { spec!.zoomBy(0.5); updateViewOverlays(); };
  $('zoomReset').onclick = () => spec!.resetView();

  const lock = $<HTMLButtonElement>('lockBtn');
  lock.onclick = () => {
    spec!.followVfo = !spec!.followVfo;
    lock.classList.toggle('on', spec!.followVfo);
    lock.textContent = spec!.followVfo ? 'LOCK' : 'FREE';
    // Walls and the RF-centre marker only mean anything once the view is free to
    // wander. CENTRE is governed separately — see updateCentreBtn().
    updateViewOverlays();
  };

  // Snap the view back onto the VFO. Works whether locked or not — being locked
  // is exactly the case where you have no other way to bring it back.
  $('centreBtn').onclick = () => {
    spec!.pan(spec!.frequency);
    updateViewOverlays();
  };

  const vol = $<HTMLInputElement>('vol');
  vol.value = String(((prefs().volume as number) ?? 0.8) * 100);
  audio!.volume = Number(vol.value) / 100;
  vol.oninput = () => {
    audio!.volume = Number(vol.value) / 100;
    savePref('volume', audio!.volume);
  };
  const mute = $<HTMLButtonElement>('muteBtn');
  mute.onclick = () => {
    audio!.muted = !audio!.muted;
    mute.classList.toggle('on', audio!.muted);
    updateMediaSession();
  };

  initFreqEntry();
  initBw();
  initPanels();
  initRecorder();
  initSearch();
  initBookmarks();
  buildMenu();

  // Station list comes from the SERVER (the app's cached EiBi) — the browser
  // can't fetch eibispace.de itself, no CORS headers there. Absent = degrade to
  // bookmarks + band plan, both of which are local.
  void loadBookmarks();
  void loadServerLocation(currentHost);
  void loadStations(currentHost).then((n) => {
    if (n) console.info(`stations: ${n} from server`);
  });
  // Stations this receiver has actually HEARD (learned from RDS by the shim). The
  // auth suffix goes with it so the SAVE path can write back — the shim gates
  // POST/DELETE on the same PIN that guards the stream.
  void loadServerBookmarks(currentHost, authState?.query ?? '').then((n) => {
    if (n) console.info(`server bookmarks: ${n} heard by this receiver`);
  });
  // ...and keep asking. RDS learning takes ~20 s of held PS to commit, so the station
  // you are sitting on RIGHT NOW is learned long after this page loaded. Fetching once
  // at boot meant a freshly learned bookmark never appeared until you reloaded — it
  // looked like the learning was broken when it had actually worked. The native app has
  // polled for this all along (SDRScreen.tsx); the web client never did.
  setInterval(() => {
    const before = JSON.stringify(getServerBookmarks().map(b => [b.frequency, b.name]));
    void loadServerBookmarks(currentHost, authState?.query ?? '').then(() => {
      const after = JSON.stringify(getServerBookmarks().map(b => [b.frequency, b.name]));
      // Only repaint on a real change — an unconditional re-render every 30 s would
      // reset the list's scroll position under the user's finger.
      if (after !== before) renderBookmarks();
    });
  }, 30_000);
  initWaterfallInput();
  initKeyboard();
}

/**
 * Unlocked-view overlays: the capture walls and the RF-centre marker.
 *
 * Neither is in the protocol — the client REPRODUCES the shim's own arithmetic
 * (SpectrumClient.rfCenterHz / panSpan). The dongle follows the view only until
 * the VFO would fall out of the captured band, then it locks; past that the
 * shim just crops further into the capture. So once you pan far enough, where
 * the HARDWARE is (dashed marker) and where you are LOOKING part company — and
 * the walls are where the capture runs out entirely.
 */
/**
 * CENTRE is offered whenever the LISTEN VFO is off screen — whatever the lock
 * says. Re-locking does NOT drag the view back (the server only recentres on a
 * tune), so you could end up locked, with the VFO somewhere off-screen, and no
 * button to get back to it. The button's job is "I can't see what I'm listening
 * to", and that condition has nothing to do with the lock.
 */
function updateCentreBtn() {
  if (!wf || !spec) return;
  const span = wf.spanHz;
  let offscreen = false;
  if (span > 0) {
    const lo = wf.displayCenterHz() - span / 2;
    const hi = lo + span;
    offscreen = spec.frequency < lo || spec.frequency > hi;
  }
  $('centreBtn').hidden = !offscreen && spec.followVfo;
}

function updateViewOverlays() {
  if (!wf || !spec) return;
  updateCentreBtn();
  if (spec.followVfo) {
    wf.wallLoHz = wf.wallHiHz = wf.rfCenterHz = null;   // locked: nothing to show
    return;
  }
  const rf = spec.rfCenterHz();
  const fs = spec.captureBandwidth();
  wf.rfCenterHz = rf;

  // WALLS = the CAPTURED-BAND EDGES (dongle ± Fs/2) — exactly what the app shows
  // (SDRScreen: "Hard walls at the captured-band edges … visible as you scroll the
  // view across the band"). They mark the 2.4 MHz the radio is actually receiving
  // right now, so as you pan you can see where the window is and where the RF
  // centre had to move to. I'd originally made them the pan LIMIT, which is a
  // different thing and not what the app means by a wall.
  wf.wallLoHz = fs ? rf - fs / 2 : null;
  wf.wallHiHz = fs ? rf + fs / 2 : null;
  wf.captureLoHz = null;   // the walls now do this job; no second bracket
  wf.captureHiHz = null;
}

// ── OS media controls (macOS Now Playing, media keys) ────────────────────────
//
// The Media Session API only attaches to a real MEDIA ELEMENT — a Web Audio graph
// alone doesn't register with the OS. So we keep a silent, looping <audio> alive
// purely to claim the Now Playing slot, and map the transport controls onto the
// radio:
//
//   play / pause  -> unmute / mute
//   next / prev   -> tune one step up / down
//
// Metadata shows the station (RDS or bookmark) and the frequency, so the menu bar
// and the media keys say something useful.

/**
 * Now Playing artwork — composited exactly as the PHONE does it
 * (VibeStreamService.refreshArtwork): the artwork base, with the RTL-TCP logo
 * inset bottom-right at 30% of the width, 4.5% padding, amber-tinted (#ffb833)
 * on a dark rounded inset. Same two source images, same numbers, so the lock
 * screen looks identical whether you're listening on the phone or in the browser.
 */
let artworkUrl = '';

function buildArtwork() {
  const base = $<HTMLImageElement>('artBase');
  const inset = $<HTMLImageElement>('artInset');
  const make = () => {
    if (!base.naturalWidth) return;
    const S = 512;
    const c = document.createElement('canvas');
    c.width = c.height = S;
    const ctx = c.getContext('2d')!;
    ctx.drawImage(base, 0, 0, S, S);

    if (inset.naturalWidth) {
      const size = S * 0.30;
      const pad = S * 0.045;
      const x = S - size - pad;
      const y = S - size - pad;

      // Dark rounded inset behind the logo, as the app draws it.
      ctx.fillStyle = 'rgba(12,10,6,0.92)';
      const r = size * 0.18;
      ctx.beginPath();
      ctx.moveTo(x + r, y);
      ctx.arcTo(x + size, y, x + size, y + size, r);
      ctx.arcTo(x + size, y + size, x, y + size, r);
      ctx.arcTo(x, y + size, x, y, r);
      ctx.arcTo(x, y, x + size, y, r);
      ctx.closePath();
      ctx.fill();

      // The logo is black line art — tint it amber, as the app does.
      const t = document.createElement('canvas');
      t.width = t.height = Math.round(size);
      const tc = t.getContext('2d')!;
      const p = size * 0.14;
      tc.drawImage(inset, p, p, size - 2 * p, size - 2 * p);
      tc.globalCompositeOperation = 'source-in';
      tc.fillStyle = '#ffb833';
      tc.fillRect(0, 0, t.width, t.height);
      ctx.drawImage(t, x, y);
    }
    artworkUrl = c.toDataURL('image/png');
    updateMediaSession();
  };
  if (base.complete) make(); else base.onload = make;
  inset.onload = make;
}

function initMediaSession() {
  if (!('mediaSession' in navigator)) return;
  buildArtwork();

  // No dummy element: AudioPlayer now plays out through a REAL <audio> fed by a
  // MediaStream from the audio graph, which is what the OS actually attaches to.
  // (A silent placeholder doesn't register — the element has to be playing real
  // audio. UberSDR ran into the same wall and streams WebM/Opus into an element
  // for exactly this reason.)
  const ms = navigator.mediaSession;
  ms.setActionHandler('play', () => {
    if (audio) { audio.muted = false; $('muteBtn').classList.remove('on'); }
    void audio?.resume();
    updateMediaSession();
  });
  ms.setActionHandler('pause', () => {
    if (audio) { audio.muted = true; $('muteBtn').classList.add('on'); }
    updateMediaSession();
  });
  ms.setActionHandler('nexttrack', () => nudge(step));
  ms.setActionHandler('previoustrack', () => nudge(-step));
  updateMediaSession();
}

function updateMediaSession() {
  if (!('mediaSession' in navigator) || !spec) return;
  const freq = `${(spec.frequency / 1e6).toFixed(3)} MHz`;
  const station = rdsName || $('vtsName').textContent || '';
  // Artwork: the RTL-TCP art the app uses, so Now Playing looks the same whether
  // you're listening on the phone or in the browser. If the station has an RDS
  // logo, prefer that — it's a picture of what you're actually hearing.
  const artSrc = rdsLogoUrl || artworkUrl;
  if (!artSrc) return;
  navigator.mediaSession.metadata = new MediaMetadata({
    title: station && station !== '—' ? station : freq,
    artist: station && station !== '—' ? `${freq} · ${spec.mode.toUpperCase()}` : spec.mode.toUpperCase(),
    album: 'VibeSDR',
    artwork: [{ src: artSrc, sizes: '512x512', type: 'image/png' }],
  });
  navigator.mediaSession.playbackState = audio?.muted ? 'paused' : 'playing';
}

// ── Idle power saving ────────────────────────────────────────────────────────
//
// The app's client-side idle slowdown saves the SERVER nothing — the phone still
// computes and transmits every frame. So here we throttle the server instead:
// after IDLE_AFTER_MS with no interaction, ask it to drop its spectrum rate. The
// engine then genuinely skips the FFT work (and the Wi-Fi radio goes quiet with
// it), which is what matters for a solar-powered server at the allotment.
//
// Audio is untouched — an idle server still sounds identical. That's deliberate:
// you leave it listening and walk away; it's the WATERFALL nobody is watching.

const IDLE_AFTER_MS = 30_000;
const ACTIVE_FPS = 20;
const IDLE_FPS = 5;

let lastInteraction = Date.now();
let throttled = false;

function markActive() {
  lastInteraction = Date.now();
  if (throttled) {
    throttled = false;
    spec?.setFftRate(ACTIVE_FPS);
    updateStatus();
  }
}

function checkIdle() {
  if (!spec || throttled) return;
  if (Date.now() - lastInteraction < IDLE_AFTER_MS) return;
  throttled = true;
  spec.setFftRate(IDLE_FPS);
  updateStatus();
}

function initIdleThrottle() {
  for (const ev of ['pointerdown', 'pointermove', 'wheel', 'keydown'] as const) {
    window.addEventListener(ev, markActive, { passive: true });
  }
  // A backgrounded tab isn't watching either — throttle immediately, and wake on
  // return rather than waiting out the timer.
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { lastInteraction = 0; checkIdle(); }
    else markActive();
  });
}

// ── Search + bookmarks ───────────────────────────────────────────────────────

/**
 * Source marks. "MY" and "SRV" were opaque — you can't tell what they mean without
 * being told. A MONITOR means "saved in this browser, on this computer"; a SERVER
 * RACK means "saved on the receiver itself, shared with everyone who connects".
 *
 * Inline SVG, not emoji: emoji render differently on every platform (and in colour),
 * while these inherit the amber and stay crisp at 12px.
 */
const ICON_LOCAL =
  '<svg class="srcIcon" viewBox="0 0 16 16" aria-label="Saved in this browser">' +
  '<rect x="1" y="2" width="14" height="9" rx="1" fill="none" stroke="currentColor" stroke-width="1.3"/>' +
  '<path d="M5 14h6M8 11v3" stroke="currentColor" stroke-width="1.3" fill="none"/></svg>';
const ICON_SERVER =
  '<svg class="srcIcon" viewBox="0 0 16 16" aria-label="Saved on the receiver">' +
  '<rect x="2" y="2" width="12" height="4.4" rx="1" fill="none" stroke="currentColor" stroke-width="1.3"/>' +
  '<rect x="2" y="9.6" width="12" height="4.4" rx="1" fill="none" stroke="currentColor" stroke-width="1.3"/>' +
  '<circle cx="4.6" cy="4.2" r="0.8" fill="currentColor"/>' +
  '<circle cx="4.6" cy="11.8" r="0.8" fill="currentColor"/></svg>';

const SRC_LABEL: Record<string, string> = {
  user: ICON_LOCAL, server: ICON_SERVER, eibi: 'EiBi', band: 'BAND',
};

function initSearch() {
  const el = $<HTMLInputElement>('search');
  const list = $('searchResults');
  let results: SearchResult[] = [];
  let sel = -1;

  const close = () => { list.classList.remove('open'); sel = -1; };

  const render = () => {
    if (!results.length) { close(); return; }
    list.innerHTML = '';
    results.forEach((r, i) => {
      const row = document.createElement('div');
      row.className = 'sres' + (i === sel ? ' sel' : '');
      row.innerHTML =
        `<span class="f">${(r.frequency / 1e6).toFixed(3)}</span>` +
        `<span class="n">${r.flag ? r.flag + ' ' : ''}${escapeHtml(r.name)}` +
        (r.detail ? ` <span class="src">${escapeHtml(r.detail)}</span>` : '') +
        `</span>` +
        `<span class="src">${SRC_LABEL[r.source] ?? ''}</span>`;
      // A logo makes a long result list scannable at a glance. EiBi rows carry their
      // transmitter country, so those resolve without any guesswork at all.
      if (r.source !== 'band') void attachBookmarkLogo(row, r.name, r.itu);
      row.onclick = () => { tuneTo(r); close(); };
      list.appendChild(row);
    });
    list.classList.add('open');
  };

  el.oninput = () => {
    results = search(el.value);
    sel = -1;
    render();
  };
  el.onfocus = () => { if (results.length) list.classList.add('open'); };
  el.onblur = () => setTimeout(close, 150);   // let a click land first

  el.onkeydown = (e) => {
    if (e.key === 'Escape') { el.blur(); close(); return; }
    if (!results.length) return;
    if (e.key === 'ArrowDown') { sel = Math.min(results.length - 1, sel + 1); render(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { sel = Math.max(0, sel - 1); render(); e.preventDefault(); }
    else if (e.key === 'Enter') {
      tuneTo(results[Math.max(0, sel)]);
      el.blur();
      close();
      e.preventDefault();
    }
  };
}

/**
 * Mode + step for a discrete jump, from the band plan — exactly what the app does
 * (bandTuneDefaults). Without it, typing 96.6 MHz while listening to 648 kHz AM
 * leaves you demodulating an FM broadcast station in AM with a ±5 kHz passband:
 * the audio breaks up, and the span (sized from the AM bandwidth) is far too
 * narrow. The band knows what it wants; use it.
 */
function applyBandDefaults(hz: number) {
  if (!spec) return;
  const d = bandTuneDefaults(hz, ituRegion());
  if (d.mode && d.mode !== spec.mode) setMode(d.mode as SDRMode, true);
  if (d.step) {
    step = d.step;
    savePref('step', step);
  }
  syncStep();
}

function tuneTo(r: SearchResult) {
  if (!spec || !r) return;
  // An explicit mode on the result wins; otherwise take the band's.
  const bandMode = bandTuneDefaults(r.frequency, ituRegion()).mode;
  const mode = (r.mode || bandMode || spec.mode) as SDRMode;
  spec.tune(clampTune(r.frequency), mode, { recenter: true, retarget: true });
  setMode(mode, false);
  // A bookmark can carry its own passband — honour it rather than the mode default.
  if (typeof r.bandwidthLow === 'number' && typeof r.bandwidthHigh === 'number') {
    spec.setBandwidth(r.bandwidthLow, r.bandwidthHigh);
    syncBw();
  }
  renderFreq();
  syncStep();
}

function initBookmarks() {
  $('bookmarksBtn').onclick = () => {
    togglePanel('bookmarksPanel');
    renderBookmarks();
  };
  $('bmClose').onclick = () => $('bookmarksPanel').classList.remove('open');

  // Bookmark whatever we're listening to right now. The name comes from an
  // in-page field: a native prompt() SUSPENDS the AudioContext in Safari, which
  // silently killed the audio mid-listen.
  const nameEl = $<HTMLInputElement>('bmName');
  const addNow = async () => {
    if (!spec) return;
    const name = nameEl.value.trim()
      || `${(spec.frequency / 1e6).toFixed(3)} MHz ${spec.mode.toUpperCase()}`;
    await addBookmark({
      name,
      frequency: Math.round(spec.frequency),
      mode: spec.mode,
      group: null, comment: null, extension: null,
      bandwidth_low: spec.bandwidthLow,
      bandwidth_high: spec.bandwidthHigh,
    });
    nameEl.value = '';
    renderBookmarks();
  };
  $('bmAdd').onclick = addNow;
  nameEl.onkeydown = (e) => { if (e.key === 'Enter') { void addNow(); e.preventDefault(); } };

  // Save on the RECEIVER — shared with every client, and it survives this browser.
  // The shim gates the write on the PIN, which is what becomes the admin credential
  // when public servers arrive.
  $('bmAddServer').onclick = async () => {
    if (!spec) return;
    const name = nameEl.value.trim() || rdsName || `${(spec.frequency / 1e6).toFixed(3)} MHz`;
    const ok = await saveToServer(spec.frequency, name, spec.mode);
    $('bmMsg').textContent = ok
      ? `Saved "${name}" on the receiver`
      : 'Could not save on the receiver (is the PIN right?)';
    if (ok) { nameEl.value = ''; renderBookmarks(); }
  };

  // Export: the same UberSDR-importable JSON the phone app writes, so bookmarks
  // move between browser, phone and desktop UberSDR.
  $('bmExport').onclick = () => {
    const blob = new Blob([exportBookmarks()], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'vibesdr-bookmarks.json';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
  };

  // Where an import LANDS matters, so the button says which: this browser, or the
  // receiver. bmImportTarget is read by the file handler below.
  let bmImportTarget: 'local' | 'server' = 'local';
  $('bmImport').onclick = () => { bmImportTarget = 'local'; $('bmFile').click(); };
  $('bmImportServer').onclick = () => { bmImportTarget = 'server'; $('bmFile').click(); };
  $<HTMLInputElement>('bmFile').onchange = async (e) => {
    const f = (e.target as HTMLInputElement).files?.[0];
    if (!f) return;
    try {
      const text = await f.text();
      if (bmImportTarget === 'server') {
        // parseBookmarksAny, NOT JSON.parse. UberSDR exports YAML, and the app already
        // has a parser that handles both — calling JSON.parse here threw on every real
        // UberSDR export, which is exactly the file people have to import.
        const rows = parseBookmarksAny(text, '');
        // Push each one to the receiver. Sequential on purpose: the shim answers with
        // the whole list every time, and firing 145 concurrent writes at a phone is a
        // good way to make a working server look broken.
        let n = 0;
        for (const b of rows) {
          if (!b?.name || !b?.frequency) continue;
          if (await saveToServer(Number(b.frequency), String(b.name), b.mode || undefined)) n++;
        }
        renderBookmarks();
        $('bmMsg').textContent = n
          ? `Imported ${n} bookmark${n === 1 ? '' : 's'} to the receiver`
          : 'Nothing imported (is the PIN right?)';
        (e.target as HTMLInputElement).value = '';
        return;
      }
      const n = await importBookmarks(text);
      renderBookmarks();
      $('bmMsg').textContent = `Imported ${n} bookmark${n === 1 ? '' : 's'}`;
    } catch (err) {
      $('bmMsg').textContent = err instanceof Error ? err.message : 'Import failed';
    }
    $<HTMLInputElement>('bmFile').value = '';
  };
}

function renderBookmarks() {
  const host = $('bmList');
  host.innerHTML = '';

  // Two lists in one, distinguished by their glyph: a MONITOR for the ones saved in
  // this browser, a SERVER RACK for the ones on the receiver (learned from RDS, or
  // saved by hand, and shared with every client).
  type Row = {
    name: string; frequency: number; mode?: string;
    local: boolean; heard?: boolean;
    bwLo?: number | null; bwHi?: number | null;
  };
  const rows: Row[] = [
    ...getBookmarks().map(b => ({
      name: b.name, frequency: b.frequency, mode: b.mode, local: true,
      bwLo: b.bandwidth_low, bwHi: b.bandwidth_high,
    })),
    ...getServerBookmarks().map(b => ({
      name: b.name, frequency: b.frequency, mode: b.mode ?? 'wfm', local: false,
      heard: !(b as any).manual,
    })),
  ];

  if (!rows.length) {
    host.innerHTML = '<div class="sres"><span class="n">No bookmarks yet — tune something and press ADD. Stations heard over RDS are added here automatically.</span></div>';
    return;
  }

  for (const b of rows.sort((a, z) => a.frequency - z.frequency)) {
    const row = document.createElement('div');
    row.className = 'sres';
    row.title = b.local ? 'Saved in this browser'
              : b.heard ? 'Heard by this receiver (expires if it stops being heard)'
              : 'Saved on the receiver';
    row.innerHTML =
      `<span class="src">${b.local ? ICON_LOCAL : ICON_SERVER}</span>` +
      `<span class="f">${(b.frequency / 1e6).toFixed(3)}</span>` +
      `<span class="n">${escapeHtml(b.name)}</span>` +
      `<span class="src">${(b.mode || '').toUpperCase()}</span>`;
    row.onclick = () => {
      tuneTo({
        name: b.name, frequency: b.frequency, mode: b.mode,
        source: b.local ? 'user' : 'server',
        bandwidthLow: b.bwLo, bandwidthHigh: b.bwHi,
      });
      $('bookmarksPanel').classList.remove('open');
    };
    const del = document.createElement('button');
    del.className = 'btn';
    del.textContent = '✕';
    del.onclick = async (e) => {
      e.stopPropagation();
      if (b.local) await removeBookmark(b.name, b.frequency);
      else await removeFromServer(b.frequency);
      renderBookmarks();
    };
    row.appendChild(del);
    host.appendChild(row);

    // Logo, lazily — for BOTH kinds. Local bookmarks were skipped on the theory that
    // "a local bookmark is whatever the user called it", but importing an UberSDR list
    // disproves that: it is full of real station names that match perfectly well, and
    // showing logos on one list and not the other just looks broken.
    void attachBookmarkLogo(row, b.name);
  }
}

/**
 * Resolve (and cache) a station logo, then slot it into the row.
 *
 * `itu` is EiBi's transmitter-country code, and it is AUTHORITATIVE — the schedule
 * states the country outright, so it is used as a hard FILTER rather than the
 * receiver's country as a mere preference. No guessing at all for EiBi rows.
 */
/**
 * Warm the logo cache for a station we've just identified, so the VTS can paint it.
 *
 * The cache is claimed IMMEDIATELY (set to null before the fetch) because the VTS runs
 * on every frame: without that, one un-cached station fires a lookup per frame until
 * the first reply lands.
 */
async function primeStationLogo(name: string) {
  const key = `${name}|`;
  if (bmLogos.has(key)) return;
  bmLogos.set(key, null);
  const url = await lookupStationLogo(name, undefined, serverIso || undefined)
    .catch(() => null);
  bmLogos.set(key, url ?? null);
}

async function attachBookmarkLogo(row: HTMLElement, name: string, itu?: string) {
  const iso = itu ? ituToIso(itu) : '';
  const key = `${name}|${iso}`;
  let url = bmLogos.get(key);
  if (url === undefined) {
    url = await lookupStationLogo(
      name, iso || undefined, iso ? undefined : (serverIso || undefined),
    ).catch(() => null);
    bmLogos.set(key, url ?? null);
  }
  if (!url || !row.isConnected) return;
  const img = document.createElement('img');
  img.className = 'bmLogo';
  img.src = url;
  img.alt = '';
  row.insertBefore(img, row.firstChild);
}


// ── Panels ───────────────────────────────────────────────────────────────────
// Centred pop-ups, one at a time. Click-outside and Escape close them — a modal
// you can only dismiss with its own CLOSE button is a modal that traps people.

const PANELS = ['menu', 'audioPanel', 'decodersPanel', 'recordingsPanel',
                'bookmarksPanel', 'freqPanel'];

function closePanels() {
  for (const id of PANELS) $(id).classList.remove('open');
}

function togglePanel(id: string) {
  const open = $(id).classList.contains('open');
  closePanels();
  if (!open) $(id).classList.add('open');
}

function initPanels() {
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closePanels();
  });
  // Click outside a panel closes it. The panels are centred pop-ups whose dim is
  // a box-shadow, so there is no backdrop element to hang this on.
  window.addEventListener('pointerdown', (e) => {
    const t = e.target as HTMLElement;
    if (!PANELS.some(id => $(id).contains(t)) && !t.closest('#bar')) closePanels();
  });
}

// ── Decoders ─────────────────────────────────────────────────────────────────
//
// All of these run SERVER-SIDE in the shim (RTTY/NAVTEX/WEFAX/SSTV over
// /ws/dxcluster, FT8/FT4 via subscribe_digital_spots). The browser attaches and
// draws — no WASM, no DSP here. See decoders.ts for the wire formats.

let decoders: DecoderClient | null = null;
let decCtx: CanvasRenderingContext2D | null = null;
let decImgWidth = 0;
const spots: Spot[] = [];

/** Skin BAND_COLOUR — markers coloured by band (verbatim from the app's map). */
const BAND_COLOUR: Record<string, string> = {
  '2200m': '#9b30d9', '630m': '#c71585', '160m': '#e8001e', '80m': '#ff5500',
  '60m': '#ff8c00', '40m': '#ffd700', '30m': '#aacc00', '20m': '#00cc44',
  '17m': '#00ccaa', '15m': '#00aaff', '12m': '#0055ff', '11m': '#6600ff',
  '10m': '#cc00cc',
};

// Spot filters — the app's sf-mode / sf-band / sf-age cyclers.
const SF_MODES = ['ALL', 'FT8', 'FT4'];
const SF_BANDS = ['ALL', '160m', '80m', '60m', '40m', '30m', '20m', '17m', '15m', '12m', '10m'];
const SF_AGES: Array<{ label: string; minutes: number }> = [
  { label: 'AGE', minutes: 0 }, { label: '15m', minutes: 15 },
  { label: '30m', minutes: 30 }, { label: '1h', minutes: 60 },
];
let sfMode = 0, sfBand = 0, sfAge = 0;

/** UTC hh:mm, as the app shows it. */
function fmtSpotTime(t: number): string {
  const d = new Date(t);
  return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`;
}

/** Spots after the header filters. */
function filteredSpots(): Spot[] {
  const cutoff = SF_AGES[sfAge].minutes ? Date.now() - SF_AGES[sfAge].minutes * 60_000 : 0;
  return spots.filter(s =>
    (SF_MODES[sfMode] === 'ALL' || s.mode === SF_MODES[sfMode]) &&
    (SF_BANDS[sfBand] === 'ALL' || s.band === SF_BANDS[sfBand]) &&
    (!cutoff || s.timestamp >= cutoff));
}

/**
 * RECEIVER position — served by the shim (GET /location), NOT taken from this
 * browser.
 *
 * That distinction matters: the server might be sitting at a relative's house in
 * another town, and once VibeServer can be public it could be listened to from
 * anywhere in the world. Distances, map centring and the ITU REGION are all
 * properties of the ANTENNA. Using the listener's position would give nonsense
 * distances and, worse, the wrong region's band edges.
 *
 * There is deliberately NO manual grid entry. A locator is a property of the
 * ANTENNA, so asking the listener for one can only produce a wrong answer — if the
 * server publishes no position, we show that plainly and do without distances.
 */
let serverLoc: { lat: number; lon: number; label?: string; country?: string; grid?: string } | null = null;
/**
 * The RECEIVER's country — a TIE-BREAK ONLY, never a filter and never a flag.
 *
 * It is tempting to assume a station you can hear is in your own country, and for the
 * ordinary case it is. But sporadic-E drops a Spanish station into a Northampton
 * receiver, and people living near a border hear three countries routinely — so using
 * this as truth would put a Union Jack on a Spanish station, which is worse than
 * showing nothing. It only breaks ties between otherwise equally good name matches.
 */
let serverIso = '';
let serverName = '';

function myPos(): { lat: number; lon: number } | null { return serverLoc; }

/** "Northampton IO92nh", or "52.24, -0.90" / "IO92nh" if only one is known. */
function locLine(): string {
  if (!serverLoc) return '';
  const { lat, lon, label, country, grid } = serverLoc;
  const place = label || `${lat.toFixed(2)}, ${lon.toFixed(2)}`;
  // The host may have named the receiver BY its locator, in which case place and
  // grid are the same thing — don't print "IO92nh IO92nh".
  const isGrid = /^[A-R]{2}[0-9]{2}([A-X]{2})?$/i.test(place);
  // "Moulton, United Kingdom IO92ng" — the country is skipped when the place IS the
  // grid (nothing to qualify) or when it would just repeat the place name.
  const where = (country && !isGrid && country !== place) ? `${place}, ${country}` : place;
  return grid && !isGrid ? `${where} ${grid}` : where;
}

async function loadServerLocation(host: string) {
  try {
    const r = await fetch(`http://${host}/location`, { cache: 'no-store' });
    const j = await r.json();
    serverName = typeof j.name === 'string' ? j.name : '';
    serverIso = typeof j.iso === 'string' ? j.iso : '';
    if (typeof j.lat === 'number' && typeof j.lon === 'number') {
      serverLoc = { lat: j.lat, lon: j.lon, label: j.label, country: j.country, grid: j.grid };
    }
    // Menu row.
    const el = $('rxLoc');
    el.textContent = serverLoc
      ? `Receiver: ${locLine()}`
      : 'Receiver location not set — distances and the band plan are unavailable.';
    // Spectrum overlay, UberSDR-style: name on top, location under it.
    $('rxName').textContent = serverName;
    $('rxWhere').textContent = locLine();
    $('rxBadge').hidden = !serverName && !serverLoc;
    renderSpots();
  } catch {
    // Older shim with no /location at all — say nothing rather than guess.
  }
}

/** Distance to a spot, km — null when either end is unknown. */
function spotDistanceKm(grid?: string): number | null {
  const me = myPos();
  const them = grid ? gridToLatLon(grid) : null;
  if (!me || !them) return null;
  return haversineKm(me, them);
}

interface RttySettings { shift: number; baud: number; encoding: string; inverted: boolean }

// Verbatim from the app (DecoderClient RTTY_PRESETS).
const RTTY_PRESETS: Record<string, RttySettings> = {
  ham:       { shift: 170, baud: 45.45, encoding: 'ITA2',    inverted: false },
  weather:   { shift: 450, baud: 50,    encoding: 'ITA2',    inverted: true  },
  'sitor-b': { shift: 170, baud: 100,   encoding: 'CCIR476', inverted: false },
};

let rtty: RttySettings = { ...RTTY_PRESETS.ham };
let wefaxLpm = 120;
let activeDec: 'rtty' | 'navtex' | 'wefax' | 'sstv' | null = null;

/** Params for the current mode — the shim's startDecoder reads these. */
function decParams(mode: string): Record<string, unknown> {
  if (mode === 'rtty') {
    return {
      center_frequency: 1000, shift: rtty.shift, baud_rate: rtty.baud,
      encoding: rtty.encoding, inverted: rtty.inverted, framing: '5N1.5',
    };
  }
  if (mode === 'wefax') {
    return { lpm: wefaxLpm, carrier: 1900, deviation: 400, image_width: 1809 };
  }
  return {};
}

function initDecoders(host: string, auth: AuthState) {
  decoders = new DecoderClient(host, auth, {
    onText: (t) => {
      const el = $('decText');
      el.textContent = (el.textContent + t).slice(-8000);
      el.scrollTop = el.scrollHeight;
      setDecLive(true);
    },
    onState: (st) => {
      $('decStatus').textContent = st ? 'decoding…' : 'listening…';
      setDecLive(!!st);
    },
    onImageStart: (w, h) => startDecImage(w, h),
    onImageLine: (y, w, px, rgb) => { drawDecLine(y, w, px, rgb); setDecLive(true); },
    onImageDone: () => { $('decStatus').textContent = 'image complete'; },
    onSstvMode: (name) => { $('decStatus').textContent = name; },
    onStatus: (t) => { $('decStatus').textContent = t; },
    onSpot: (sp) => {
      spots.unshift(sp);
      if (spots.length > 500) spots.pop();
      renderSpots();
      setDecLive(true);
    },
  });
  decoders.connect();

  $('decodersBtn').onclick = () => togglePanel('decodersPanel');
  $('decClose').onclick = () => closePanels();

  // ── Decoder selection: toggles start/stop, and the MENU STAYS OPEN (skin
  //    semantics, same as the app). Selecting one opens the output box.
  for (const b of Array.from($('decodersPanel').querySelectorAll('[data-dec]')) as HTMLButtonElement[]) {
    b.onclick = () => {
      const mode = b.dataset.dec as 'rtty' | 'navtex' | 'wefax' | 'sstv';
      if (activeDec === mode) { stopDecoder(); return; }
      activeDec = mode;
      decoders!.attach(mode, decParams(mode));
      showDecBox(mode);
      syncDecButtons();
    };
  }

  // RTTY settings — presets fill the individual controls, as in the app.
  segButtons('rttyPreset', 'preset', (v) => {
    rtty = { ...RTTY_PRESETS[v as string] };
    syncRttyControls();
    reattachIf('rtty');
  });
  segButtons('rttyShift', 'shift', (v) => { rtty.shift = Number(v); reattachIf('rtty'); });
  segButtons('rttyBaud', 'baud', (v) => { rtty.baud = Number(v); reattachIf('rtty'); });
  segButtons('rttyEnc', 'enc', (v) => { rtty.encoding = String(v); reattachIf('rtty'); });
  const inv = $<HTMLButtonElement>('rttyInv');
  inv.onclick = () => {
    rtty.inverted = !rtty.inverted;
    inv.classList.toggle('on', rtty.inverted);
    inv.textContent = rtty.inverted ? 'ON' : 'OFF';
    reattachIf('rtty');
  };
  segButtons('wefaxLpm', 'lpm', (v) => { wefaxLpm = Number(v); reattachIf('wefax'); });

  // Spots + map.
  const spotsBtn = $<HTMLButtonElement>('spotsBtn');
  spotsBtn.onclick = () => {
    const on = !decoders!.spotsEnabled;
    decoders!.setSpots(on);
    spotsBtn.classList.toggle('on', on);
    if (on) showDecBox('spots'); else if (!activeDec) hideDecBox();
  };
  $('mapBtn').onclick = openSpotsMap;


  // Output box chrome.
  initSpotFilters();
  $('decClr').onclick = () => { $('decText').textContent = ''; };
  $('decMin').onclick = () => $('decBox').classList.toggle('min');
  $('decHide').onclick = () => { stopDecoder(); decoders!.setSpots(false);
    $<HTMLButtonElement>('spotsBtn').classList.remove('on'); hideDecBox(); };
}

/** Wire a segmented control; `on` marks the selected button. */
function segButtons(id: string, attr: string, apply: (v: string) => void) {
  const host = $(id);
  const btns = Array.from(host.children) as HTMLButtonElement[];
  for (const b of btns) {
    b.onclick = () => {
      for (const x of btns) x.classList.remove('on');
      b.classList.add('on');
      apply(b.dataset[attr]!);
    };
  }
}

/** Reflect the current RTTY settings back into the buttons (after a preset). */
function syncRttyControls() {
  const mark = (id: string, attr: string, val: string) => {
    for (const b of Array.from($(id).children) as HTMLButtonElement[]) {
      b.classList.toggle('on', b.dataset[attr] === val);
    }
  };
  mark('rttyShift', 'shift', String(rtty.shift));
  mark('rttyBaud', 'baud', String(rtty.baud));
  mark('rttyEnc', 'enc', rtty.encoding);
  const inv = $<HTMLButtonElement>('rttyInv');
  inv.classList.toggle('on', rtty.inverted);
  inv.textContent = rtty.inverted ? 'ON' : 'OFF';
}

/** A settings change while running must re-attach — the shim builds the decoder
 *  from the attach params, so it can't be tweaked in place. */
function reattachIf(mode: string) {
  if (activeDec === mode) decoders?.attach(mode as 'rtty' | 'wefax', decParams(mode));
}

function stopDecoder() {
  decoders?.detach();
  activeDec = null;
  syncDecButtons();
  if (!decoders?.spotsEnabled) hideDecBox();
  else showDecBox('spots');
}

function syncDecButtons() {
  for (const b of Array.from($('decodersPanel').querySelectorAll('[data-dec]')) as HTMLButtonElement[]) {
    b.classList.toggle('on', b.dataset.dec === activeDec);
  }
  $('rttySettings').hidden = activeDec !== 'rtty';
  $('wefaxSettings').hidden = activeDec !== 'wefax';
}

function showDecBox(what: string) {
  const image = what === 'wefax' || what === 'sstv';
  const isSpots = what === 'spots';
  $('decBox').classList.add('open');
  $('decBox').classList.remove('min');
  $('decTitle').textContent = what === 'spots' ? 'FT8 / FT4 SPOTS' : what.toUpperCase();
  $('decStatus').textContent = 'listening…';
  $('decImage').classList.toggle('on', image);
  $('decText').classList.toggle('off', image || isSpots);
  $('spotList').classList.toggle('on', isSpots);
  $('spotFilters').classList.toggle('show', isSpots);
  setDecLive(false);
}

function hideDecBox() { $('decBox').classList.remove('open'); }

let decLiveTimer = 0;
function setDecLive(on: boolean) {
  const dot = $('decDot');
  dot.classList.toggle('live', on);
  // Fall back to idle if nothing decodes for a couple of seconds.
  if (on) {
    clearTimeout(decLiveTimer);
    decLiveTimer = window.setTimeout(() => dot.classList.remove('live'), 2500);
  }
}

function startDecImage(w: number, h: number) {
  const c = $<HTMLCanvasElement>('decImage');
  // WEFAX declares no height — the image grows until the transmission stops.
  c.width = w || decImgWidth || 800;
  c.height = h || 600;
  decImgWidth = c.width;
  decCtx = c.getContext('2d');
  decCtx?.clearRect(0, 0, c.width, c.height);
}

function drawDecLine(y: number, w: number, px: Uint8Array, rgb: boolean) {
  const c = $<HTMLCanvasElement>('decImage');
  if (!decCtx || c.width !== w) startDecImage(w, 0);
  if (!decCtx) return;

  if (y >= c.height) {                     // grow downward rather than clip
    const keep = decCtx.getImageData(0, 0, c.width, c.height);
    c.height = y + 200;
    decCtx = c.getContext('2d');
    decCtx?.putImageData(keep, 0, 0);
    if (!decCtx) return;
  }

  const img = decCtx.createImageData(w, 1);
  for (let x = 0; x < w; x++) {
    const o = x << 2;
    if (rgb) {
      img.data[o] = px[x * 3];
      img.data[o + 1] = px[x * 3 + 1];
      img.data[o + 2] = px[x * 3 + 2];
    } else {
      const v = px[x];                     // WEFAX is greyscale
      img.data[o] = img.data[o + 1] = img.data[o + 2] = v;
    }
    img.data[o + 3] = 255;
  }
  decCtx.putImageData(img, 0, y);
}

function renderSpots() {
  const host = $('spotList');
  host.innerHTML = '';

  // Newest per callsign+band — a station calling CQ every cycle would otherwise
  // fill the whole list with itself.
  const seen = new Set<string>();
  const rows = filteredSpots().filter(sp => {
    const k = `${sp.callsign}|${sp.band}`;
    if (seen.has(k)) return false;
    seen.add(k);
    return true;
  }).slice(0, 80);

  for (const sp of rows) {
    const country = countryForCallsign(sp.callsign);
    const km = spotDistanceKm(sp.grid);
    const row = document.createElement('div');
    row.className = 'sres spot';
    // The app's columns: time · band · snr · call · country · distance.
    row.innerHTML =
      `<span class="t">${fmtSpotTime(sp.timestamp)}</span>` +
      `<span class="band" style="color:${BAND_COLOUR[sp.band] || 'var(--text-dim)'}">${escapeHtml(sp.band)}</span>` +
      `<span class="snr ${sp.snr >= 0 ? 'pos' : 'neg'}">${sp.snr}</span>` +
      `<span class="call">${escapeHtml(sp.callsign)}</span>` +
      `<span class="cty">${escapeHtml(abbrCountry(country) || '')}</span>` +
      `<span class="km">${km != null ? Math.round(km) + 'km' : ''}</span>`;
    row.title = `${sp.mode} · ${sp.grid || 'no grid'} · ${(sp.frequency / 1e6).toFixed(3)} MHz`;
    row.onclick = () => {
      spec?.tune(clampTune(sp.frequency), 'usb', { recenter: true, retarget: true });
      renderFreq();
    };
    host.appendChild(row);
  }
}

/** Header cyclers, as in the app: tap to step through the options. */
function initSpotFilters() {
  const cycle = (id: string, get: () => number, set: (i: number) => void,
                 labels: string[]) => {
    const el = $<HTMLButtonElement>(id);
    const paint = () => {
      el.textContent = labels[get()];
      el.classList.toggle('on', get() !== 0);
    };
    el.onclick = () => { set((get() + 1) % labels.length); paint(); renderSpots(); };
    paint();
  };
  cycle('sfMode', () => sfMode, (i) => { sfMode = i; }, SF_MODES);
  cycle('sfBand', () => sfBand, (i) => { sfBand = i; }, SF_BANDS);
  cycle('sfAge', () => sfAge, (i) => { sfAge = i; }, SF_AGES.map(a => a.label));
}

/**
 * FT8 map — opens in a NEW TAB. Leaflet + tiles need the internet, which would
 * break this page's "self-contained, no external requests" property; a separate
 * tab keeps that intact and gives the map real screen space. Spots are baked into
 * the page as data, so it needs no connection back to the server.
 */
/**
 * FT8 map — opens in a NEW TAB, with docked stats panels.
 *
 * Modelled on UberSDR's Digital Spots Map rather than the phone app's sheet: on a
 * desktop there's room to DOCK the statistics beside the map, so they're always
 * readable. The app's full-screen overlay was a small-screen compromise we don't
 * have to inherit.
 *
 * Opens even with no spots — a button that does nothing reads as broken.
 */
function openSpotsMap() {
  const rows = filteredSpots().filter(s => s.grid && s.grid.length >= 4);
  const me = myPos();

  const pts = rows.map(s => {
    const p = gridToLatLon(s.grid)!;
    return {
      callsign: s.callsign, grid: s.grid, mode: s.mode, band: s.band, snr: s.snr,
      frequency: s.frequency, timestamp: s.timestamp,
      lat: p.lat, lon: p.lon,
      country: countryForCallsign(s.callsign) || '',
      km: me ? Math.round(haversineKm(me, p)) : null,
      colour: BAND_COLOUR[s.band] || '#aaaaaa',
    };
  });

  // Closing tags assembled at runtime — a literal </script> or </style> here would
  // terminate the page's OWN inline <script> when the bundle is inlined.
  const ES = '<' + '/script>';
  const ST = '<' + '/style>';

  const html = `<!doctype html><meta charset="utf-8"><title>VibeSDR — Digital Spots Map</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js">${ES}
<style>
  :root{--amber:#ffb833;--dim:rgba(255,160,0,0.45);--bg:rgba(8,6,2,0.94);
        --bdr:rgba(255,160,0,0.30);--hi:#ffe566}
  html,body{height:100%;margin:0;background:#080601;
    font:12px ui-monospace,"SF Mono",Menlo,monospace;color:var(--amber)}
  #wrap{display:flex;flex-direction:column;height:100%}
  /* Top bar — counts + the toggles, as UberSDR does. */
  #top{display:flex;align-items:center;gap:14px;padding:7px 12px;
    background:var(--bg);border-bottom:1px solid var(--bdr);flex:0 0 auto}
  #count{color:var(--hi);font-weight:bold}
  #top label{display:inline-flex;gap:4px;align-items:center;color:var(--dim);cursor:pointer}
  #top .sp{flex:1 1 auto}
  #main{position:relative;flex:1 1 auto;min-height:0}
  #m{position:absolute;inset:0}
  .panel{position:absolute;z-index:1000;background:var(--bg);
    border:1px solid var(--bdr);border-radius:8px}
  /* Docked stats — left column, scrollable, NOT covering the map. */
  #stats{top:10px;left:10px;width:265px;max-height:calc(100% - 20px);overflow-y:auto;padding:10px 12px}
  #stats.hide,#legend.hide,#summary.hide{display:none}
  .sect{font-size:9px;letter-spacing:1.5px;color:var(--dim);margin:10px 0 6px;
    border-top:1px solid rgba(255,160,0,0.15);padding-top:8px}
  .sect:first-child{border-top:none;margin-top:0;padding-top:0}
  .row{display:flex;justify-content:space-between;gap:8px;margin:3px 0;font-size:11px}
  .row b{color:var(--hi);font-weight:bold}
  .bar{height:3px;background:rgba(255,160,0,0.12);border-radius:2px;margin:2px 0 5px}
  .bar i{display:block;height:3px;border-radius:2px;background:var(--amber)}
  .sub{font-size:9px;color:var(--dim);margin:-2px 0 6px}
  /* Compass rose — 8 bearings, like UberSDR's. */
  #rose{display:grid;grid-template-columns:repeat(4,1fr);gap:4px 8px}
  #rose div{font-size:10px}
  #rose .n{color:var(--dim)}
  /* Legend — right. */
  #legend{top:10px;right:10px;padding:8px 10px;min-width:110px}
  .lrow{display:flex;align-items:center;gap:7px;padding:2px 0;white-space:nowrap;font-size:11px}
  .sw{width:11px;height:11px;border-radius:50%;flex:0 0 11px}
  /* Summary strip — bottom, like UberSDR's latest/rarest row. */
  #summary{bottom:10px;left:50%;transform:translateX(-50%);padding:8px 14px;
    display:flex;gap:26px;font-size:11px;white-space:nowrap}
  #summary .k{color:var(--dim);font-size:9px;letter-spacing:1px}
  #summary .v{color:var(--hi)}
  #empty{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
    color:var(--dim);pointer-events:none;z-index:900;font-size:13px}
  .pop{font:12px ui-monospace,Menlo,monospace;color:#111;line-height:1.5}
${ST}
<div id="wrap">
  <div id="top">
    <span id="count">0</span><span style="color:var(--dim)">spots</span>
    <label><input type="checkbox" id="tStats" checked>Stats</label>
    <label><input type="checkbox" id="tSummary" checked>Summary</label>
    <label><input type="checkbox" id="tLegend" checked>Legend</label>
    <span class="sp"></span>
    <span id="clock" style="color:var(--dim)"></span>
  </div>
  <div id="main">
    <div id="m"></div>
    <div id="empty"></div>
    <div class="panel" id="stats"></div>
    <div class="panel" id="legend"></div>
    <div class="panel" id="summary"></div>
  </div>
</div>
<script>
const spots = ${JSON.stringify(pts)};
const me = ${JSON.stringify(me)};
const COL = ${JSON.stringify(BAND_COLOUR)};

const map = L.map('m', { worldCopyJump: true })
  .setView(me ? [me.lat, me.lon] : [25, 5], me ? 4 : 3);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  { attribution: '&copy; OpenStreetMap', maxZoom: 14 }).addTo(map);

function radius(snr) {
  const s = Math.max(-24, Math.min(12, snr));
  return 4 + ((s + 24) / 36) * 9;
}
for (const s of spots) {
  L.circleMarker([s.lat, s.lon], {
    radius: radius(s.snr), color: '#00000066', weight: 1,
    fillColor: s.colour, fillOpacity: 0.85,
  }).addTo(map).bindPopup(
    '<div class="pop"><b>' + s.callsign + '</b><br>' +
    (s.country ? s.country + '<br>' : '') + s.grid +
    (s.km != null ? ' · ' + s.km + ' km' : '') + '<br>' +
    s.mode + ' · ' + s.band + ' · ' + (s.snr > 0 ? '+' : '') + s.snr + ' dB<br>' +
    (s.frequency / 1e6).toFixed(3) + ' MHz</div>');
}
if (me) {
  L.circleMarker([me.lat, me.lon], { radius: 7, color: '#fff', weight: 2,
    fillColor: '#e05050', fillOpacity: 1 }).addTo(map)
    .bindPopup('<div class="pop"><b>RX</b><br>Receiver</div>');
  for (const km of [1000, 2500, 5000]) {
    L.circle([me.lat, me.lon], { radius: km * 1000, color: 'rgba(255,160,0,0.30)',
      weight: 1, fill: false, dashArray: '4 6' }).addTo(map);
  }
}
const all = spots.map(s => [s.lat, s.lon]);
if (me) all.push([me.lat, me.lon]);
if (all.length > 1) map.fitBounds(all, { padding: [60, 60] });

document.getElementById('count').textContent = String(spots.length);
document.getElementById('empty').textContent = spots.length
  ? '' : 'No spots with a grid yet — leave DIGITAL SPOTS running.';

// ── Stats ────────────────────────────────────────────────────────────────────
function bearing(a, b) {
  const toR = (d) => d * Math.PI / 180, toD = (r) => r * 180 / Math.PI;
  const dLon = toR(b.lon - a.lon);
  const y = Math.sin(dLon) * Math.cos(toR(b.lat));
  const x = Math.cos(toR(a.lat)) * Math.sin(toR(b.lat)) -
            Math.sin(toR(a.lat)) * Math.cos(toR(b.lat)) * Math.cos(dLon);
  return (toD(Math.atan2(y, x)) + 360) % 360;
}
const DIRS = ['N','NE','E','SE','S','SW','W','NW'];
function tally(fn) {
  const o = {};
  for (const s of spots) { const k = fn(s); if (k) o[k] = (o[k] || 0) + 1; }
  return o;
}
function barList(counts, colourFor, limit) {
  const e = Object.entries(counts).sort((a, b) => b[1] - a[1]).slice(0, limit || 8);
  if (!e.length) return '<div class="sub">Nothing yet</div>';
  const max = e[0][1];
  return e.map(([k, n]) =>
    '<div class="row"><span>' + k + '</span><b>' + n + '</b></div>' +
    '<div class="bar"><i style="width:' + Math.round(n / max * 100) + '%' +
    (colourFor && colourFor(k) ? ';background:' + colourFor(k) : '') + '"></i></div>').join('');
}

let html = '';
const withKm = spots.filter(s => s.km != null).sort((a, b) => a.km - b.km);

// Distance buckets — UberSDR's four bands, as percentages of the total.
if (withKm.length) {
  const max = withKm[withKm.length - 1].km, min = withKm[0].km;
  const step = Math.max(1, (max - min) / 4);
  html += '<div class="sect">DISTANCE</div>';
  for (let i = 0; i < 4; i++) {
    const lo = Math.round(min + i * step), hi = Math.round(min + (i + 1) * step);
    const n = withKm.filter(s => s.km >= lo && (i === 3 ? s.km <= hi : s.km < hi)).length;
    const pct = Math.round(n / withKm.length * 100);
    html += '<div class="row"><span>' + lo + '–' + hi + ' km</span><b>' + n + ' (' + pct + '%)</b></div>' +
            '<div class="bar"><i style="width:' + pct + '%"></i></div>';
  }

  html += '<div class="sect">BEARING</div><div id="roseWrap"></div>';
}

if (withKm.length) {
  const c = withKm[0], f = withKm[withKm.length - 1];
  html += '<div class="sect">RANGE</div>' +
    '<div class="row"><span>Closest</span><b>' + c.km + ' km</b></div>' +
    '<div class="sub">' + c.callsign + ' (' + c.grid + ') · ' + (c.country || '') +
    ' · ' + c.band + ' · ' + c.snr + 'dB</div>' +
    '<div class="row"><span>Farthest</span><b>' + f.km + ' km</b></div>' +
    '<div class="sub">' + f.callsign + ' (' + f.grid + ') · ' + (f.country || '') +
    ' · ' + f.band + ' · ' + f.snr + 'dB</div>';
} else if (spots.length) {
  html += '<div class="sect">RANGE</div><div class="sub">' +
    'No receiver location published — distances unavailable.</div>';
}

html += '<div class="sect">TOP COUNTRIES</div>' + barList(tally(s => s.country), null, 10);
html += '<div class="sect">BY BAND</div>' + barList(tally(s => s.band), (b) => COL[b], 12);
html += '<div class="sect">BY MODE</div>' + barList(tally(s => s.mode), null, 6);
html += '<div class="sect">TOTALS</div>' +
  '<div class="row"><span>Spots</span><b>' + spots.length + '</b></div>' +
  '<div class="row"><span>Countries</span><b>' +
    new Set(spots.map(s => s.country).filter(Boolean)).size + '</b></div>' +
  '<div class="row"><span>Bands</span><b>' +
    new Set(spots.map(s => s.band)).size + '</b></div>';
document.getElementById('stats').innerHTML = html;

// Bearing rose — only meaningful with a receiver position.
if (me && withKm.length) {
  const counts = new Array(8).fill(0);
  for (const s of spots) {
    const b = bearing(me, s);
    counts[Math.round(b / 45) % 8]++;
  }
  const tot = counts.reduce((a, b) => a + b, 0) || 1;
  document.getElementById('roseWrap').innerHTML =
    '<div id="rose">' + DIRS.map((d, i) =>
      '<div><span class="n">' + d + '</span> <b>' + counts[i] + '</b><br>' +
      '<span class="n">' + Math.round(counts[i] / tot * 100) + '%</span></div>').join('') +
    '</div>';
}

// Legend
const bandsUsed = [...new Set(spots.map(s => s.band))];
document.getElementById('legend').innerHTML =
  (me ? '<div class="lrow"><span class="sw" style="background:#e05050"></span>Receiver</div>' : '') +
  (bandsUsed.length
    ? bandsUsed.map(b => '<div class="lrow"><span class="sw" style="background:' +
        (COL[b] || '#aaa') + '"></span>' + b + '</div>').join('')
    : '<div class="lrow" style="color:var(--dim)">No bands yet</div>') +
  '<div class="lrow" style="color:var(--dim);font-size:9px;margin-top:4px">Size = SNR</div>';

// Summary strip — latest and rarest, as UberSDR shows along the bottom.
const byTime = [...spots].sort((a, b) => b.timestamp - a.timestamp);
const cCounts = tally(s => s.country);
const rarest = Object.entries(cCounts).sort((a, b) => a[1] - b[1])[0];
const latest = byTime[0];
function cell(k, v, sub) {
  return '<div><div class="k">' + k + '</div><div class="v">' + v + '</div>' +
         (sub ? '<div class="k">' + sub + '</div>' : '') + '</div>';
}
document.getElementById('summary').innerHTML = spots.length
  ? cell('LATEST', (latest.country || latest.callsign) + ' · ' + latest.band,
         latest.callsign + ' · ' + latest.mode + ' · ' + latest.snr + 'dB') +
    (rarest ? cell('RAREST COUNTRY', rarest[0] + ' (' + rarest[1] + ')', '') : '') +
    cell('SPOTS', String(spots.length), new Set(spots.map(s => s.country).filter(Boolean)).size + ' countries')
  : cell('WAITING', 'no spots yet', '');

// Clock + panel toggles
function tick() {
  const d = new Date();
  document.getElementById('clock').textContent =
    'UTC ' + d.toISOString().slice(11, 19) + ' · ' + d.toLocaleTimeString();
}
tick(); setInterval(tick, 1000);
const bind = (id, el) => {
  document.getElementById(id).onchange = (e) =>
    document.getElementById(el).classList.toggle('hide', !e.target.checked);
};
bind('tStats', 'stats'); bind('tSummary', 'summary'); bind('tLegend', 'legend');
${ES}`;

  const w = window.open('', '_blank');
  if (!w) { $('decStatus').textContent = 'popup blocked'; return; }
  w.document.write(html);
  w.document.close();
}


interface NearStation {
  name: string; frequency: number; flag?: string;
  source: 'user' | 'eibi' | 'server';
}

/** Nearest station (user bookmark, then server/EiBi) — on-tune candidates only. */
function nearestStation(hz: number): NearStation | null {
  let best: NearStation | null = null;
  let bestOff = VTS_ON_HZ;
  for (const b of getBookmarks()) {
    const off = Math.abs(b.frequency - hz);
    if (off < bestOff) {
      bestOff = off;
      best = { name: b.name, frequency: b.frequency, source: 'user' };
    }
  }
  // Stations the receiver has HEARD outrank the EiBi schedule: EiBi says what
  // exists, this says what actually comes in here.
  for (const b of getServerBookmarks()) {
    const off = Math.abs(b.frequency - hz);
    if (off < bestOff) {
      bestOff = off;
      best = { name: b.name, frequency: b.frequency, source: 'server' };
    }
  }
  for (const st of getStations()) {
    const off = Math.abs(st.frequency - hz);
    if (off < bestOff) {
      bestOff = off;
      best = {
        name: st.name, frequency: st.frequency, flag: st.flag,
        source: st.source === 'server' ? 'server' : 'eibi',
      };
    }
  }
  return best;
}

// ── Recorder ─────────────────────────────────────────────────────────────────

function recordingName(hz: number, mode: string, at: Date): string {
  const stamp = at.toISOString().replace(/[:.]/g, '-').slice(0, 19);
  return `VibeSDR_${(hz / 1e6).toFixed(3)}MHz_${mode.toUpperCase()}_${stamp}.wav`;
}

function initRecorder() {
  const btn = $<HTMLButtonElement>('recBtn');
  btn.onclick = async () => {
    if (!audio || !spec) return;
    if (!audio.recording) {
      audio.startRecording();
      btn.classList.add('rec');
      btn.textContent = '■ STOP';
      return;
    }
    const seconds = audio.recordedSeconds;
    const blob = audio.stopRecording();
    btn.classList.remove('rec');
    btn.textContent = '● REC';
    $('recTime').textContent = '';
    if (!blob) return;

    // Kept, not just downloaded — a recording you can't find again isn't a feature.
    // The RECORDINGS panel plays, downloads and deletes them.
    const at = new Date();
    await saveRecording({
      name: recordingName(spec.frequency, spec.mode, at),
      frequency: Math.round(spec.frequency),
      mode: spec.mode,
      createdAt: at.getTime(),
      seconds,
      bytes: blob.size,
      blob,
    });
    $('recordingsBtn').classList.add('on');
    setTimeout(() => $('recordingsBtn').classList.remove('on'), 1500);
  };

  $('recordingsBtn').onclick = () => {
    togglePanel('recordingsPanel');
    void renderRecordings();
  };
  $('recsClose').onclick = () => $('recordingsPanel').classList.remove('open');
}

async function renderRecordings() {
  const host = $('recsList');
  const list = await listRecordings();
  host.innerHTML = '';
  if (!list.length) {
    host.innerHTML = '<div class="sres"><span class="n">No recordings yet — press ● REC.</span></div>';
    return;
  }
  for (const r of list) {
    const row = document.createElement('div');
    row.className = 'sres';
    row.style.cursor = 'default';
    row.innerHTML =
      `<span class="f">${(r.frequency / 1e6).toFixed(3)}</span>` +
      `<span class="n">${escapeHtml(r.mode.toUpperCase())} · ${formatDuration(r.seconds)} · ${formatSize(r.bytes)}` +
      `<br><span class="src">${new Date(r.createdAt).toLocaleString()}</span></span>`;

    const dl = document.createElement('button');
    dl.className = 'btn';
    dl.textContent = 'SAVE';
    dl.onclick = () => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(r.blob);
      a.download = r.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
    };

    const del = document.createElement('button');
    del.className = 'btn';
    del.textContent = '✕';
    del.onclick = async () => { await deleteRecording(r.id); void renderRecordings(); };

    const player = document.createElement('audio');
    player.controls = true;
    player.preload = 'none';
    player.src = URL.createObjectURL(r.blob);

    row.append(dl, del, player);
    host.appendChild(row);
  }
}

function updateRecTime() {
  if (!audio?.recording) return;
  const s = Math.floor(audio.recordedSeconds);
  $('recTime').textContent =
    `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

// ── Menu: Radio / Audio / Display ────────────────────────────────────────────

/** Wire a slider: live label, live effect, persisted. */
function slider(
  id: string, valId: string,
  fmt: (v: number) => string,
  apply: (v: number) => void,
  prefKey?: string,
) {
  const el = $<HTMLInputElement>(id);
  const lbl = $(valId);
  const saved = prefKey ? prefs()[prefKey] : undefined;
  if (typeof saved === 'number') el.value = String(saved);
  const run = () => {
    const v = Number(el.value);
    lbl.textContent = fmt(v);
    apply(v);
  };
  el.oninput = () => { run(); if (prefKey) savePref(prefKey, Number(el.value)); };
  run();
}

/** Wire a toggle button: ON/OFF text, live effect, persisted. */
function toggle(id: string, apply: (on: boolean) => void, prefKey?: string, initial = false) {
  const el = $<HTMLButtonElement>(id);
  const saved = prefKey ? prefs()[prefKey] : undefined;
  let on = typeof saved === 'boolean' ? saved : initial;
  const run = () => {
    el.classList.toggle('on', on);
    el.textContent = on ? 'ON' : 'OFF';
    apply(on);
  };
  el.onclick = () => { on = !on; run(); if (prefKey) savePref(prefKey, on); };
  run();
}

/** Wire a segmented control (data-<attr> on each button). */
function segment(id: string, attr: string, apply: (v: number) => void, prefKey?: string) {
  const host = $(id);
  const btns = Array.from(host.children) as HTMLButtonElement[];
  const saved = prefKey ? prefs()[prefKey] : undefined;
  const pick = (v: number, fire: boolean) => {
    for (const b of btns) b.classList.toggle('on', Number(b.dataset[attr]) === v);
    if (fire) apply(v);
  };
  for (const b of btns) {
    b.onclick = () => {
      const v = Number(b.dataset[attr]);
      pick(v, true);
      if (prefKey) savePref(prefKey, v);
    };
  }
  pick(typeof saved === 'number' ? saved : Number(btns[0].dataset[attr]), false);
}

function buildMenu() {
  $('menuBtn').onclick   = () => togglePanel('menu');
  $('menuClose').onclick = () => $('menu').classList.remove('open');
  // Audio DSP lives in its own drawer (as the app's AudioSheet does) — these are
  // controls you use WHILE listening, not settings you configure once.
  $('audioBtn').onclick   = () => togglePanel('audioPanel');
  $('audioClose').onclick = () => $('audioPanel').classList.remove('open');

  // ── Radio (server-side hardware; ranges filled in from hwinfo) ────────────
  $<HTMLInputElement>('ppm').oninput = () => {
    const v = Number($<HTMLInputElement>('ppm').value);
    $('ppmVal').textContent = String(v);
    spec!.setHwPpm(v);
    savePref('ppm', v);
  };
  toggle('biasT', (on) => spec!.setHwBiasT(on), 'biasT');
  toggle('agc',   (on) => spec!.setHwAgc(on),   'agc');
  segment('dsSeg', 'ds', (v) => spec!.setHwDirectSampling(v as 0 | 1 | 2), 'directSampling');

  const gainAuto = $<HTMLButtonElement>('gainAuto');
  gainAuto.onclick = () => {
    const on = !gainAuto.classList.contains('on');
    gainAuto.classList.toggle('on', on);
    $<HTMLInputElement>('gain').disabled = on;
    if (on) spec!.setHwGain(0, true);
    else spec!.setHwGain(hwGains[Number($<HTMLInputElement>('gain').value)] ?? 0, false);
  };

  // ── Audio (server-side DSP in the shim) ──────────────────────────────────
  slider('sql', 'sqlVal',
    (v) => (v <= -100 ? 'OFF' : `${v} dB`),
    // Mirror the setting into the audio engine: squelch is applied SERVER-side, so
    // without this the client cannot tell a closed squelch from a muted tab.
    (v) => { spec!.setSquelch(v); if (audio) audio.squelchDb = v; },
    'squelch');

  slider('nr', 'nrVal',
    (v) => (v === 0 ? 'OFF' : `${v}%`),
    (v) => spec!.setNr(v > 0, v / 100),
    'nr');

  toggle('notch', (on) => spec!.setNotch(on), 'notch');
  toggle('stereoBtn', (on) => {
    $('stereoBtn').textContent = on ? 'ON' : 'OFF';
    spec!.setStereo(on);
  }, 'stereo', true);
  segment('deemphSeg', 'tau', (us) => spec!.setDeemph(us * 1e-6), 'deemph');

  // ── Display / Waterfall / Spectrum ───────────────────────────────────────
  // The full set the app exposes, split into the sections it uses. All of it
  // feeds SignalProcessor, which is the APP's module — so a setting here does
  // exactly what the same setting does on the phone.
  const pal = $<HTMLSelectElement>('palette');
  for (const name of [...COLORMAP_NAMES].sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }))) {
    const o = document.createElement('option');
    o.value = name; o.textContent = name;
    pal.appendChild(o);
  }
  pal.value = wf!.palette;
  pal.onchange = () => { wf!.setPalette(pal.value); savePref('palette', pal.value); };

  slider('specRatio', 'specRatioVal', (v) => `${v}%`,
    (v) => wf!.setSpecRatio(v / 100), 'specRatio');

  // ── Waterfall ────────────────────────────────────────────────────────────
  // Coarse: AUTO auto-ranges the display window; MANUAL pins it to min/max dB.
  const coarseBtns = Array.from($('wfCoarse').children) as HTMLButtonElement[];
  const applyCoarse = (mode: string) => {
    const manual = mode === 'manual';
    for (const b of coarseBtns) b.classList.toggle('on', b.dataset.coarse === mode);
    $('rowAutoContrast').hidden = manual;
    $('rowMinDb').hidden = !manual;
    $('rowMaxDb').hidden = !manual;
    wf!.applySettings(manual
      ? { manualRange: { minDb: Number($<HTMLInputElement>('minDb').value),
                         maxDb: Number($<HTMLInputElement>('maxDb').value) } }
      : { manualRange: null });
    savePref('wfCoarse', mode);
  };
  for (const b of coarseBtns) b.onclick = () => applyCoarse(b.dataset.coarse!);

  slider('autoContrast', 'autoContrastVal', (v) => String(v),
    (v) => wf!.applySettings({ autoContrast: v }), 'autoContrast');
  const pushManual = () => {
    const lo = Number($<HTMLInputElement>('minDb').value);
    const hi = Number($<HTMLInputElement>('maxDb').value);
    if (($('rowMinDb') as HTMLElement).hidden) return;
    wf!.applySettings({ manualRange: { minDb: Math.min(lo, hi - 1), maxDb: hi } });
  };
  slider('minDb', 'minDbVal', (v) => String(v), () => pushManual(), 'minDb');
  slider('maxDb', 'maxDbVal', (v) => String(v), () => pushManual(), 'maxDb');

  slider('bright', 'brightVal', (v) => String(v),
    (v) => wf!.applySettings({ wfBrightness: v }), 'wfBrightness');
  slider('contrast', 'contrastVal', (v) => String(v),
    (v) => wf!.applySettings({ wfContrast: v }), 'wfContrast');
  slider('sharp', 'sharpVal', (v) => String(v),
    (v) => wf!.applySettings({ wfSharpness: v }), 'wfSharpness');
  toggle('spatialSmooth', (on) => wf!.applySettings({ spatialSmooth: on }), 'spatialSmooth', true);

  // ── Spectrum trace ───────────────────────────────────────────────────────
  const showBtn = $<HTMLButtonElement>('specShow');
  const savedShow = prefs().specShow;
  let specOn = typeof savedShow === 'boolean' ? savedShow : true;
  const applyShow = () => {
    wf!.showSpec = specOn;
    showBtn.classList.toggle('on', specOn);
    showBtn.textContent = specOn ? 'SHOW' : 'HIDE';
  };
  showBtn.onclick = () => { specOn = !specOn; applyShow(); savePref('specShow', specOn); };
  applyShow();

  slider('smooth', 'smoothVal', (v) => String(v),
    (v) => wf!.applySettings({ smoothingFrames: v }), 'smoothingFrames');
  slider('specFloor', 'specFloorVal', (v) => String(v),
    (v) => wf!.applySettings({ specFloor: v }), 'specFloor');
  slider('specPeak', 'specPeakVal', (v) => `${(v / 10).toFixed(1)}×`,
    (v) => wf!.applySettings({ specPeakScale: v }), 'specPeakScale');
  slider('specAlpha', 'specAlphaVal', (v) => `${v}%`,
    (v) => { wf!.specAlpha = v / 100; }, 'specAlpha');
  toggle('peakHold', (on) => wf!.applySettings({ peakHold: on }), 'peakHold', true);

  applyCoarse((prefs().wfCoarse as string) || 'auto');

  // Back to the app's defaults, without hunting every slider.
  $('dispReset').onclick = () => {
    for (const k of ['autoContrast', 'minDb', 'maxDb', 'wfBrightness', 'wfContrast',
                     'wfSharpness', 'smoothingFrames', 'specFloor', 'specPeakScale',
                     'specAlpha', 'specRatio', 'spatialSmooth', 'peakHold', 'specShow',
                     'wfCoarse', 'palette']) {
      const p = prefs();
      delete p[k];
      localStorage.setItem(LS_PREFS, JSON.stringify(p));
    }
    location.reload();
  };

  // ── VFO (needle + acrylic sidebands), as in the app ──────────────────────
  const colEl = $<HTMLInputElement>('vfoColor');
  const savedCol = prefs().vfoColor;
  if (typeof savedCol === 'string') colEl.value = savedCol;
  wf!.vfoColor = colEl.value;
  colEl.oninput = () => { wf!.vfoColor = colEl.value; savePref('vfoColor', colEl.value); };

  slider('vfoGlow', 'vfoGlowVal', (v) => String(v),
    (v) => { wf!.vfoIntensity = v; }, 'vfoIntensity');
  slider('vfoFrost', 'vfoFrostVal', (v) => (v === 0 ? 'OFF' : String(v)),
    (v) => { wf!.vfoFrost = v; }, 'vfoFrost');
}

/**
 * Re-send every SERVER-side setting we've persisted. Called whenever the
 * spectrum socket opens (first connect, and every reconnect — the shim keeps no
 * per-client state, so a reconnect silently reverts the radio to defaults).
 *
 * Client-side settings (palette, brightness, spectrum split…) don't appear here:
 * they're applied locally when the menu is built.
 */
function pushSettingsToServer() {
  if (!spec) return;
  const p = prefs();
  const num = (k: string) => (typeof p[k] === 'number' ? p[k] as number : undefined);
  const bool = (k: string) => (typeof p[k] === 'boolean' ? p[k] as boolean : undefined);

  // Mirror to the audio engine here too, or a page RELOAD with squelch already saved
  // restores the squelch but not the engine's knowledge of it — and the false
  // "IS THE TAB MUTED?" warning comes straight back.
  const sql = num('squelch');
  if (sql !== undefined) { spec.setSquelch(sql); if (audio) audio.squelchDb = sql; }
  const nr = num('nr');             if (nr !== undefined) spec.setNr(nr > 0, nr / 100);
  const notch = bool('notch');      if (notch !== undefined) spec.setNotch(notch);
  const stereo = bool('stereo');    if (stereo !== undefined) spec.setStereo(stereo);
  const deemph = num('deemph');     if (deemph !== undefined) spec.setDeemph(deemph * 1e-6);
  const ppm = num('ppm');           if (ppm !== undefined) spec.setHwPpm(ppm);
  const biasT = bool('biasT');      if (biasT !== undefined) spec.setHwBiasT(biasT);
  const agc = bool('agc');          if (agc !== undefined) spec.setHwAgc(agc);
  const ds = num('directSampling'); if (ds !== undefined) spec.setHwDirectSampling(ds as 0 | 1 | 2);

  // Re-assert the frame rate: the shim keeps whatever it was last set to, so a
  // reconnect could otherwise land in a stuck 5 fps with no way back.
  spec.setFftRate(throttled ? IDLE_FPS : ACTIVE_FPS);

  // Gain and sample rate wait for hwinfo — we can't validate them until the
  // server has told us what this dongle actually supports.
}

/** The server tells us its real gain steps and sample rates (hwinfo) — the
 *  client can't query a remote dongle, so the controls are built from that. */
function populateHw() {
  if (hwGains.length) {
    const g = $<HTMLInputElement>('gain');
    g.min = '0';
    g.max = String(hwGains.length - 1);
    const savedIdx = prefs().gainIdx;
    g.value = String(typeof savedIdx === 'number'
      ? Math.min(hwGains.length - 1, savedIdx)
      : hwGains.length - 1);
    const show = () => {
      const tenths = hwGains[Number(g.value)] ?? 0;
      $('gainVal').textContent = `${(tenths / 10).toFixed(1)} dB`;
    };
    g.oninput = () => {
      show();
      spec!.setHwGain(hwGains[Number(g.value)] ?? 0, false);
      savePref('gainIdx', Number(g.value));
    };
    show();
    // Push the restored gain — otherwise the slider shows a value the radio
    // isn't using.
    if (typeof savedIdx === 'number') spec!.setHwGain(hwGains[Number(g.value)] ?? 0, false);
  }
  // The SERVER pinned the capture rate: hide the picker and say who set it.
  // Offering a control whose every use is silently dropped is worse than offering
  // none — the shim ignores a client's sampleRate outright when locked.
  const pinned = hwLockedRate > 0;
  const rateRow = document.getElementById('rowRate');
  const rateLock = document.getElementById('rateLocked');
  if (rateRow)  rateRow.hidden = pinned;
  if (rateLock) {
    rateLock.hidden = !pinned;
    const val = rateLock.querySelector('.val');
    if (pinned && val) {
      const mhz = (hwLockedRate / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
      val.textContent = `${mhz} MS/s · set by server`;
    }
  }

  if (hwRates.length && !pinned) {
    const r = $<HTMLSelectElement>('rate');
    r.innerHTML = '';
    // ASCENDING. The server advertises 3.2 first, and an unsorted list made the highest
    // rate the one the <select> showed by default — see the default below for why that
    // was not merely untidy.
    for (const rate of [...hwRates].sort((a, b) => a - b)) {
      const o = document.createElement('option');
      o.value = String(rate);
      const mhz = `${(rate / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MS/s`;
      // An RTL-SDR cannot sustain more than 2.4 MS/s over USB — above it the dongle
      // drops samples, which shows up as glitches in the audio and gaps in the
      // waterfall. It is offered because some dongles cope and some users want the
      // span, but it must never be chosen FOR them, and never without saying so.
      o.textContent = rate > RTL_SAFE_RATE ? `${mhz} (may drop samples)` : mhz;
      r.appendChild(o);
    }
    r.onchange = () => {
      spec!.setHwSampleRate(Number(r.value));
      savePref('sampleRate', Number(r.value));
    };

    // THE DEFAULT, and it MUST be sent, not merely displayed.
    //
    // Before: with no saved preference nothing was selected and nothing was sent — so the
    // dropdown sat on whatever happened to be first (3.2 MS/s) while the server was
    // actually running at 2.4. The UI was not just defaulting badly, it was LYING about
    // the rate the radio was at. Now the client picks a safe rate, tells the server, and
    // what you read is what you get.
    const saved = prefs().sampleRate;
    const wanted = (typeof saved === 'number' && hwRates.includes(saved))
      ? saved
      : (hwRates.includes(RTL_SAFE_RATE)
          ? RTL_SAFE_RATE
          // No 2.4 on offer: take the fastest rate that is still safe, and if even the
          // slowest is over the line, take the slowest of a bad lot.
          : ([...hwRates].sort((a, b) => a - b).filter(x => x <= RTL_SAFE_RATE).pop()
             ?? Math.min(...hwRates)));
    r.value = String(wanted);
    spec!.setHwSampleRate(wanted);
  }
}

function setMode(m: SDRMode, send: boolean) {
  if (!spec) return;
  if (send) spec.setMode(m);
  else { spec.mode = m; const bw = MODE_BANDWIDTHS[m]; spec.bandwidthLow = bw[0]; spec.bandwidthHigh = bw[1]; }
  $('modeLbl').textContent = m.toUpperCase();
  for (const b of Array.from($('modes').children) as HTMLButtonElement[]) {
    b.classList.toggle('on', b.dataset.mode === m);
  }
  if (m !== 'wfm') {
    $('stereo').classList.remove('on');
    rdsName = ''; rdsText = ''; rdsIso = ''; rdsLogoUrl = ''; logoQuery = '';
  }
  updateVts();
  syncBw();
}

// ── Demodulator bandwidth ────────────────────────────────────────────────────
//
// Mirrored edge sliders, as in the app (ModeSelector): the LEFT slider runs
// -max..0 and sets the lower edge, the RIGHT runs 0..+max and sets the upper.
// SYNC mirrors them. A single "width" slider can't express SSB, where the
// passband sits entirely on one side of the carrier.

/** Per-edge cap (Hz) for each mode — drives the slider ranges. */
const BW_EDGE_MAX: Record<SDRMode, number> = {
  usb: 6000,    lsb: 6000,
  am: 20000,    sam: 20000,
  cwu: 2000,    cwl: 2000,
  fm: 30000,    nfm: 30000,
  wfm: 250000,
};

let bwSync = false;

function fmtHz(hz: number): string {
  const a = Math.abs(hz);
  return a >= 1000 ? `${(a / 1000).toFixed(a >= 100_000 ? 0 : 1)}k` : `${Math.round(a)}`;
}

function edgeLabel(hz: number): string {
  return `${hz < 0 ? '−' : '+'}${fmtHz(hz)}`;
}

/** Push the current edges to the server and redraw the labels. */
function applyBw(low: number, high: number) {
  if (!spec) return;
  spec.setBandwidth(Math.round(low), Math.round(high));
  $('bwLoVal').textContent = edgeLabel(low);
  $('bwHiVal').textContent = edgeLabel(high);
  $<HTMLInputElement>('bwLo').value = String(-Math.round(low));   // magnitude (see syncBw)
  $<HTMLInputElement>('bwHi').value = String(Math.round(high));
}

/** Re-range the sliders for the current mode and load its current passband. */
function syncBw() {
  if (!spec) return;
  const max = BW_EDGE_MAX[spec.mode] ?? 6000;
  const step = max > 50_000 ? 1000 : max > 10_000 ? 100 : 10;
  const lo = $<HTMLInputElement>('bwLo');
  const hi = $<HTMLInputElement>('bwHi');
  // The LOW slider carries a MAGNITUDE (0..max), not the signed edge, and is mirrored
  // in CSS (#bwLo { transform: scaleX(-1) }). A native range always fills from its
  // minimum to the thumb — so with a -max..0 range the orange grew from the OUTER edge
  // inward, i.e. it showed the bandwidth you were NOT using. Magnitude + mirror makes
  // both halves fill outward from the carrier, so the orange always means "this much
  // bandwidth", the same on both sides.
  lo.min = '0'; lo.max = String(max); lo.step = String(step);
  hi.min = '0'; hi.max = String(max); hi.step = String(step);
  lo.value = String(-Math.max(-max, Math.min(0, spec.bandwidthLow)));   // signed -> magnitude
  hi.value = String(Math.min(max, Math.max(0, spec.bandwidthHigh)));
  $('bwLoVal').textContent = edgeLabel(spec.bandwidthLow);
  $('bwHiVal').textContent = edgeLabel(spec.bandwidthHigh);
}

function initBw() {
  const lo = $<HTMLInputElement>('bwLo');
  const hi = $<HTMLInputElement>('bwHi');

  lo.oninput = () => {
    const v = -Number(lo.value);        // slider holds a magnitude; the edge is negative
    if (bwSync) applyBw(v, -v);
    else applyBw(v, spec!.bandwidthHigh);
  };
  hi.oninput = () => {
    const v = Number(hi.value);
    if (bwSync) applyBw(-v, v);
    else applyBw(spec!.bandwidthLow, v);
  };

  const sync = $<HTMLButtonElement>('bwSync');
  const savedSync = prefs().bwSync;
  bwSync = typeof savedSync === 'boolean' ? savedSync : false;
  sync.classList.toggle('on', bwSync);
  sync.onclick = () => {
    bwSync = !bwSync;
    sync.classList.toggle('on', bwSync);
    savePref('bwSync', bwSync);
    // Mirror immediately off the wider edge, so turning SYNC on does something
    // predictable rather than silently waiting for the next drag.
    if (bwSync && spec) {
      const w = Math.max(Math.abs(spec.bandwidthLow), Math.abs(spec.bandwidthHigh));
      applyBw(-w, w);
    }
  };

  syncBw();
}


/** The step ladder is band-aware — the app switches to VHF steps above 30 MHz,
 *  where 10 Hz is uselessly small for broadcast FM and repeaters. */
function formatStep(hz: number): string {
  if (hz >= 1000) {
    const k = hz / 1000;
    return `${k % 1 === 0 ? k : k.toFixed(1)}kHz`;
  }
  return `${hz}Hz`;
}

function buildVfo() {
  const saved = prefs().step;
  if (typeof saved === 'number' && saved > 0) step = saved;
  $('tuneDown').onclick = () => nudge(-step);
  $('tuneUp').onclick   = () => nudge(step);
  $('stepBtn').onclick  = cycleStep;
  syncStep();
  renderFreq();
}

/** Click the step button to walk the ladder for the current band. */
function cycleStep() {
  if (!spec) return;
  const steps = stepsForFreq(spec.frequency);
  const i = steps.indexOf(step);
  step = steps[(i + 1) % steps.length];
  $('stepBtn').textContent = formatStep(step);
  savePref('step', step);
}

/** Keep the step legal for the band we're in — the HF and VHF ladders differ,
 *  so a step carried across 30 MHz can land off-ladder. */
function syncStep() {
  if (!spec) return;
  const steps = stepsForFreq(spec.frequency);
  if (!steps.includes(step)) {
    // Nearest step on the new ladder, so crossing the boundary doesn't jolt.
    step = steps.reduce((a, s) => Math.abs(s - step) < Math.abs(a - step) ? s : a, steps[0]);
  }
  $('stepBtn').textContent = formatStep(step);
}

// Tuner limits. These are the RADIO's range, NOT the current view — an earlier
// version derived the ceiling from centerFreq + maxBandwidth, which capped tuning
// to the window you happened to be looking at (typing 96.6 while parked at 89 MHz
// landed you at ~91 MHz). The window follows the VFO; it does not constrain it.
//
// R820T/R860 tuners reach ~1.7 GHz, and direct sampling gets down to HF, so the
// only honest bounds are the tuner's own. The shim retunes the dongle to follow.
const MIN_TUNE_HZ = 10_000;
const MAX_TUNE_HZ = 1_800_000_000;

function clampTune(hz: number): number {
  return Math.max(MIN_TUNE_HZ, Math.min(MAX_TUNE_HZ, Math.round(hz)));
}

function nudge(hz: number) {
  if (!spec) return;
  // Snap to the step grid so repeated nudges stay on round frequencies.
  const mag = Math.abs(hz);
  const next = Math.round((spec.frequency + hz) / mag) * mag;
  spec.tune(clampTune(next));
  syncStep();
  renderFreq();
}

// ── Frequency display + entry ────────────────────────────────────────────────
//
// The unit chosen in the entry popup also drives the tuning block's readout, so
// the two always agree — a dial reading MHz while you type kHz is how people
// mis-tune by a factor of a thousand.

type FreqUnit = 'hz' | 'khz' | 'mhz';
const UNIT_DIV: Record<FreqUnit, number> = { hz: 1, khz: 1e3, mhz: 1e6 };
const UNIT_DP:  Record<FreqUnit, number> = { hz: 0, khz: 3, mhz: 3 };
const UNIT_LBL: Record<FreqUnit, string> = { hz: 'Hz', khz: 'kHz', mhz: 'MHz' };

let freqUnit: FreqUnit = 'mhz';

function renderFreq() {
  if (!spec) return;
  updateVts();
  updateMediaSession();
  const hz = Math.round(spec.frequency);
  $('freq').textContent = (hz / UNIT_DIV[freqUnit]).toFixed(UNIT_DP[freqUnit]);
  $('freqUnit').textContent = UNIT_LBL[freqUnit];
}

function setFreqUnit(u: FreqUnit) {
  freqUnit = u;
  savePref('freqUnit', u);
  for (const b of Array.from($('freqUnitSeg').children) as HTMLButtonElement[]) {
    b.classList.toggle('on', b.dataset.unit === u);
  }
  renderFreq();
}

function initFreqEntry() {
  const saved = prefs().freqUnit;
  if (saved === 'hz' || saved === 'khz' || saved === 'mhz') freqUnit = saved;

  $('pill').onclick = () => {
    togglePanel('freqPanel');
    const el = $<HTMLInputElement>('freqInput');
    el.value = (spec!.frequency / UNIT_DIV[freqUnit]).toFixed(UNIT_DP[freqUnit]);
    $('freqMsg').textContent = '';
    setTimeout(() => { el.focus(); el.select(); }, 60);
  };
  $('freqClose').onclick = () => closePanels();

  for (const b of Array.from($('freqUnitSeg').children) as HTMLButtonElement[]) {
    b.onclick = () => {
      const prev = freqUnit;
      const u = b.dataset.unit as FreqUnit;
      // Keep the typed VALUE meaningful across a unit change: convert it rather
      // than reinterpreting 100.7 MHz as 100.7 kHz.
      const el = $<HTMLInputElement>('freqInput');
      const hz = parseFloat(el.value) * UNIT_DIV[prev];
      setFreqUnit(u);
      if (isFinite(hz)) el.value = (hz / UNIT_DIV[u]).toFixed(UNIT_DP[u]);
    };
  }

  const go = () => {
    const v = parseFloat($<HTMLInputElement>('freqInput').value.replace(/[^\d.]/g, ''));
    if (!isFinite(v) || v <= 0) { $('freqMsg').textContent = 'Enter a frequency'; return; }
    spec!.tune(clampTune(v * UNIT_DIV[freqUnit]), undefined, { recenter: true, retarget: true });
    renderFreq();
    syncStep();
    closePanels();
  };
  $('freqGo').onclick = go;
  $<HTMLInputElement>('freqInput').onkeydown = (e) => {
    if (e.key === 'Enter') { go(); e.preventDefault(); }
  };

  $('freqShare').onclick = shareFrequency;
  setFreqUnit(freqUnit);
}

/**
 * Share the current tuning as a link. Same query shape the APP shares
 * (SDRScreen.onShareStation): ?freq=&mode=&bwl=&bwh= — so a shared link opens
 * this same page pointing at the same server, tuned identically.
 *
 * navigator.clipboard is SECURE-CONTEXT ONLY and a VibeServer is plain http on a
 * LAN IP, so it is undefined there. Fall back to the old execCommand path, and
 * if even that fails, show the URL so it can be copied by hand.
 */
async function shareFrequency() {
  if (!spec) return;
  const base = `http://${currentHost}/`;
  const url = `${base}?freq=${Math.round(spec.frequency)}&mode=${spec.mode}`
    + `&bwl=${Math.round(spec.bandwidthLow)}&bwh=${Math.round(spec.bandwidthHigh)}`;

  const msg = $('freqMsg');
  try {
    if (window.isSecureContext && navigator.clipboard) {
      await navigator.clipboard.writeText(url);
      msg.textContent = 'Link copied';
      return;
    }
    const ta = document.createElement('textarea');
    ta.value = url;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    msg.textContent = ok ? 'Link copied' : url;
  } catch {
    msg.textContent = url;
  }
}

/** A shared link opens tuned to the same station. */
function applyShareParams() {
  if (!spec) return false;
  const q = new URLSearchParams(location.search);
  const f = Number(q.get('freq'));
  if (!f) return false;
  const mode = (q.get('mode') || spec.mode) as SDRMode;
  const bwl = Number(q.get('bwl'));
  const bwh = Number(q.get('bwh'));
  spec.frequency = clampTune(f);
  setMode(mode, true);
  spec.tune(spec.frequency, mode, { recenter: true });
  if (bwl && bwh) { applyBw(bwl, bwh); syncBw(); }
  renderFreq();
  return true;
}

// ── Waterfall input: click-to-tune, drag-to-pan, wheel-to-zoom ───────────────

function initWaterfallInput() {
  const c = $<HTMLCanvasElement>('wf');
  let dragging = false;
  let moved = false;
  let startX = 0;
  let startCenter = 0;   // view centre when the drag began

  c.addEventListener('pointerdown', (e) => {
    if (!spec) return;
    dragging = true;
    moved = false;
    startX = e.clientX;
    // Anchor to our PREDICTED view, not the rendered frame. wf.centerHz is the
    // centre of the last frame the SERVER sent — it lags by a frame or two, so
    // panning relative to it measures from a stale base, fights the frames still
    // in flight, and snaps back when a config echo lands. That's the treacle.
    startCenter = spec.viewCenterHz();
    c.setPointerCapture(e.pointerId);
    c.classList.add('panning');
  });

  c.addEventListener('pointermove', (e) => {
    if (!dragging || !spec || !wf) return;
    const dx = e.clientX - startX;
    if (!moved && Math.abs(dx) < 2) return;
    moved = true;
    // Absolute from the drag start — never accumulate, never read back from the
    // display. Dragging right pulls lower frequencies into view.
    let target = startCenter - dx * wf.hzPerPx();

    // Don't let the view leave the reachable band; the server would clamp it
    // anyway and the snap-back would look like a bug. Flash the edge so a drag
    // that stops dead has a visible reason (the wall is usually off-screen when
    // you're zoomed in).
    const pan = spec.panSpan();
    if (pan) {
      if (target < pan.loHz) { target = pan.loHz; wf.wallHitAt = performance.now(); wf.wallHitSide = 'lo'; }
      else if (target > pan.hiHz) { target = pan.hiHz; wf.wallHitAt = performance.now(); wf.wallHitSide = 'hi'; }
    }

    spec.pan(target);
    updateViewOverlays();
  });

  c.addEventListener('pointerup', (e) => {
    dragging = false;
    c.classList.remove('panning');
    if (!moved && spec && wf) {
      const rect = c.getBoundingClientRect();
      // Snap the click to the step grid, so the arrows carry on from a round number.
      const hz = Math.round(wf.xToHz(e.clientX - rect.left) / step) * step;
      spec.tune(clampTune(hz));
      syncStep();
      renderFreq();
    }
  });

  c.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (!spec || !wf) return;
    // Anchor on the CURSOR: the frequency under the pointer stays under the
    // pointer, so you zoom into whatever you were looking at.
    const rect = c.getBoundingClientRect();
    const anchor = wf.xToHz(e.clientX - rect.left);
    spec.zoomBy(e.deltaY < 0 ? 1.25 : 0.8, anchor);
    updateViewOverlays();
  }, { passive: false });
}

function initKeyboard() {
  window.addEventListener('keydown', (e) => {
    if (!spec) return;
    const tgt = e.target as HTMLElement;
    if (tgt && /INPUT|SELECT|TEXTAREA/.test(tgt.tagName)) return;

    // Arrow keys tune by the selected step (×10 with Shift for a fast run).
    const d = step * (e.shiftKey ? 10 : 1);
    switch (e.key) {
      case 'ArrowLeft':  nudge(-d); e.preventDefault(); break;
      case 'ArrowRight': nudge(d);  e.preventDefault(); break;
      case '[': case ']': cycleStep(); e.preventDefault(); break;
      case 'ArrowUp':    spec.zoomBy(1.25); updateViewOverlays(); e.preventDefault(); break;
      case 'ArrowDown':  spec.zoomBy(0.8);  updateViewOverlays(); e.preventDefault(); break;
      case 'm': audio!.muted = !audio!.muted; $('muteBtn').classList.toggle('on', audio!.muted); break;
      default: {
        // Mode letter keys: first mode whose name starts with the key.
        const m = MODES.find(x => x[0] === e.key.toLowerCase());
        if (m) setMode(m, true);
      }
    }
  });
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!
  ));
}

initSplash();
