/**
 * main.ts — VibeSDR web client entry.
 *
 * Talks to the VibeServer shim running on a phone. Same origin when served by
 * the shim itself (GET /), so the WS URLs are relative; in dev the splash takes
 * an explicit host:port.
 */

import { SpectrumClient, MODE_BANDWIDTHS, type SDRMode } from './spectrum';
import { AudioPlayer } from './audio';
import { initMobileControls } from './mobile';
import { Waterfall, setRenderScale, renderDpr } from './waterfall';
import { resolveAuth, resolveAdminOverride, withAuth, fetchAuthChallenge, vibeAuthToken,
         type AuthState } from './auth';
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

// ★ NO FM-DX. It briefly sat next to WFM, widening the channel filter to recover RDS subcarrier
// amplitude — and measured TEN dB worse for RDS signal-to-noise on a narrow-band radio, which
// Stuart caught in one A/B on a strong local (scatter 16% -> 39%, block sync lost, 2026-07-27).
// The useful half — the noise-corrected deviation readout — now switches itself on whenever the
// Advanced RDS analyser is open, so there is nothing left for a mode to do.
const MODES: SDRMode[] = ['wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl'];
const LS_SERVERS = 'vibesdr_web_servers_v1';   // { "host:port": pin }
const LS_PREFS   = 'vibesdr_web_prefs_v1';

let step = 1000;                 // tuning step, Hz (restored from prefs on load)
/** ★ Wheel = TUNE rather than ZOOM. Same choice the app offers; see the wheel handler. */
let wheelTunes = false;
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

  // ★★ ADMIN AT THE GATE. Reveals the password field and turns CONNECT into an admin connect.
  //    A second press hides it again, so it cannot be left armed by accident.
  const adminRowEl = $('gateAdminRow');
  const adminPwEl  = $<HTMLInputElement>('gateAdminPw');
  /** Set once the operator has been told they would be displacing a listener. */
  let adminConfirmed = false;
  $('btnAdmin').addEventListener('click', () => {
    const showing = !adminRowEl.hidden;
    adminRowEl.hidden = showing;
    $('btnAdmin').textContent = showing ? 'ADMIN' : 'CANCEL ADMIN';
    adminConfirmed = false;
    if (showing) adminPwEl.value = ''; else adminPwEl.focus();
    msg.textContent = showing ? '' : 'Connecting as admin: no time limit, all controls unlocked.';
    msg.className = showing ? '' : 'info';
  });

  const go = async (remember: boolean) => {
    const host = hostEl.value.trim().replace(/^https?:\/\//, '').replace(/\/$/, '');
    const pin = pinEl.value.trim();
    if (!host) { msg.textContent = 'Enter a server address'; return; }
    msg.className = 'info';
    msg.textContent = 'Connecting…';
    // ★ Resolve the admin credentials BEFORE connect(), which picks them up from sessionStorage
    //   and folds them into the auth query so they ride BOTH sockets — same path the IN USE
    //   recovery uses, so there is one mechanism and one place for it to be wrong.
    if (!adminRowEl.hidden && adminPwEl.value) {
      // ★★★ AN ADMIN MUST NOT BOOT SOMEONE WITHOUT KNOWING IT (Stuart, 2026-07-28): "some admins
      // may be kind and let a user keep a session for longer". Connecting as admin CLAIMS the
      // slot, so on an occupied receiver it silently disconnects whoever is listening. Ask first,
      // and say who is there and how long they have left — the operator can then choose to wait.
      // ★ On a FREE receiver there is nobody to displace, so there is nothing to confirm and the
      //   admin goes straight in. The prompt only exists where there is a cost.
      if (!adminConfirmed) {
        let busy = false, freeIn = -1;
        try {
          const r = await fetch(`http://${host}/vibeserver.json`, { cache: 'no-store' });
          const j = await r.json();
          busy = j.busy === true;
          freeIn = typeof j.freeInSec === 'number' ? j.freeInSec : -1;
        } catch { /* unreachable is reported by connect() below */ }
        if (busy) {
          adminConfirmed = true;
          const mins = freeIn > 0 ? Math.max(1, Math.round(freeIn / 60)) : 0;
          msg.className = '';
          msg.innerHTML = 'Someone is listening on this receiver'
            + (mins ? ` — they have about ${mins} minute${mins === 1 ? '' : 's'} left.` : '.')
            + '<br><br>Connecting as admin will <b>disconnect them</b>.'
            + ' Press CONNECT again to take over, or wait for them to finish.';
          $<HTMLButtonElement>('btnConnect').disabled = false;
          $<HTMLButtonElement>('btnSaveConnect').disabled = false;
          return;
        }
      }
      const q = await resolveAdminOverride(`http://${host}`, adminPwEl.value).catch(() => '');
      if (!q) {
        msg.className = ''; msg.textContent = 'This server cannot be given an admin password.';
        return;
      }
      sessionStorage.setItem('vsAdminOverride', q);
    }
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
  // ★ The ADMIN button appears only where there is an admin password to enter. `admin` comes
  //   from /vibeserver.json, which the picker already fetches for every server.
  try {
    const ri = await fetch(`http://${host}/vibeserver.json`, { cache: 'no-store' });
    const ji = await ri.json();
    if (ji.admin === true) $('btnAdmin').hidden = false;
  } catch { /* not a VibeServer, or unreachable — leave the button hidden */ }
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

// ── Audio codec policy ────────────────────────────────────────────────────────
// The operator's setting and OUR OWN position relative to the server, both from
// /vibeserver.json. Read once at connect, because the codec is a query parameter on the
// audio socket's URL and so has to be decided before that socket exists.
type UncompressedPolicy = 'off' | 'choice' | 'compat';
let srvUncompressed: UncompressedPolicy = 'off';
let srvLocal = false;
// ★ Does this server have an admin password at all? Only then is there anything to unlock.
let srvAdminProtected = false;
let adminUnlocked = false;
const RAW_AUDIO_KEY = 'vibesdr.rawAudio';

function prefersRawAudio(): boolean {
  try { return localStorage.getItem(RAW_AUDIO_KEY) === '1'; } catch { return false; }
}
function setPrefersRawAudio(on: boolean) {
  try { localStorage.setItem(RAW_AUDIO_KEY, on ? '1' : '0'); } catch { /* private mode */ }
}

async function loadAudioPolicy(httpBase: string) {
  // ★ Defaults stand if this fails. An older VibeServer has no such fields and a
  // non-VibeServer has no such endpoint — both should leave us on Opus rather than
  // guessing our way into 187 KB/s of someone else's uplink.
  srvUncompressed = 'off';
  srvLocal = false;
  try {
    const r = await fetch(`${httpBase}/vibeserver.json`, { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    if (j.uncompressed === 'choice' || j.uncompressed === 'compat' || j.uncompressed === 'off')
      srvUncompressed = j.uncompressed;
    srvLocal = j.local === true;
    srvAdminProtected = j.admin === true;
  } catch { /* leave the safe defaults */ }
}

/** ★ HIDDEN, not disabled, unless the operator opened it to listeners. An inert control still
 *  reads as an offer, and this one costs someone else 187 KB/s. Also hidden on loopback, where
 *  raw is unconditional and there is no choice left to present. */
/** ★ Protected controls: visibly locked, not missing. A listener who cannot find bias-T
 *  concludes the app lacks it; one who sees it greyed with a reason understands the operator
 *  made a choice — and knows there is a way in if the receiver is theirs. */
function refreshAdminRow() {
  const show = srvAdminProtected;
  const row = document.getElementById('adminRow');
  const note = document.getElementById('adminNote');
  if (row) row.hidden = !show;
  if (note) note.hidden = !show || adminUnlocked;
  const btn = document.getElementById('adminUnlock');
  if (btn) {
    btn.textContent = adminUnlocked ? 'UNLOCKED' : 'UNLOCK';
    btn.classList.toggle('on', adminUnlocked);
  }
  // The protected controls themselves. Disabled while locked, and left alone entirely on a
  // server with no admin password so nothing changes for the vast majority of hosts.
  const locked = show && !adminUnlocked;
  // ★ Buttons AND range inputs. bias-T is a .btn on the RSP panel and an <input> on the
  // dongle one, so both shapes have to be handled or one radio's control stays live.
  for (const id of ['ppm', 'biasT', 'directSampling', 'ahfPpb', 'rspBiasT']) {
    const el = document.getElementById(id) as HTMLInputElement | null;
    if (!el) continue;
    el.disabled = locked;
    const rowEl = el.closest('.mrow') as HTMLElement | null;
    if (rowEl) rowEl.style.opacity = locked ? '0.45' : '1';
  }
}

/** ★★ WIRED UP AT LAST. `doAdminUnlock` existed and NOTHING EVER CALLED IT — the UNLOCK button
 *  only ever had its label set, so pressing it did precisely nothing (Stuart, 2026-07-27).
 *  ★ A control that is drawn, enabled and inert is worse than a missing one: the user concludes
 *  the feature is broken rather than absent, and there is nothing on screen to contradict them. */
function initAdminUnlock() {
  const btn = document.getElementById('adminUnlock');
  const row = document.getElementById('adminPwRow');
  const inp = document.getElementById('adminPwInput') as HTMLInputElement | null;
  btn?.addEventListener('click', () => {
    if (adminUnlocked) return;
    if (!row) return;
    row.hidden = !row.hidden;          // second press hides it again
    if (!row.hidden) inp?.focus();
  });
  const go = () => { const v = inp?.value ?? ''; if (v) void doAdminUnlock(v); };
  document.getElementById('adminPwGo')?.addEventListener('click', go);
  inp?.addEventListener('keydown', (e) => { if ((e as KeyboardEvent).key === 'Enter') go(); });
}

async function doAdminUnlock(pw: string) {
  if (adminUnlocked || !pw) return;
  try {
    // ★ The same challenge-response the PIN uses, and the same nonce endpoint. The password
    // never crosses the wire, and reusing the scheme means it inherits the server's existing
    // brute-force lockout rather than needing its own.
    const ch = await fetchAuthChallenge(`http://${currentHost}`);
    const nonce = ch.nonce;
    if (!nonce) { alert('This server did not offer a challenge.'); return; }
    spec?.send({ type: 'admin_unlock', nonce, token: vibeAuthToken(pw, nonce) });
  } catch (e) {
    alert(`Could not reach the server to unlock: ${(e as Error).message}`);
  }
}

function refreshRawAudioRow() {
  const show = srvUncompressed === 'choice' && !srvLocal;
  const row = document.getElementById('rawAudioRow');
  const note = document.getElementById('rawAudioNote');
  const btn = document.getElementById('rawAudio');
  if (row) row.hidden = !show;
  if (note) note.hidden = !show;
  if (btn) {
    const on = prefersRawAudio();
    btn.textContent = on ? 'ON' : 'OFF';
    btn.classList.toggle('on', on);
  }
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

  // ★★ ADMIN OVERRIDE, stashed by the IN USE screen before it reloaded. Folded into the auth
  // query so it rides BOTH sockets, exactly like the PIN — the spectrum socket claims the slot
  // and the audio socket must be recognised as the same occupant.
  // ★ CONSUMED, not kept: removed the moment it is used, so it applies to this connect attempt
  // and is not silently replayed on every future reload of the tab.
  {
    const ov = sessionStorage.getItem('vsAdminOverride');
    if (ov) {
      sessionStorage.removeItem('vsAdminOverride');
      auth = { ...auth, query: auth.query ? `${auth.query}&${ov}` : ov };
    }
  }

  // ★ ONE session id, on BOTH sockets. The server treats a session as a single occupant, and a
  // browser opens two sockets (spectrum + audio) — without a shared id the audio socket looks like
  // a different client and the occupancy check would reject a client's own audio. Two browsers or
  // two devices get two ids, which is exactly what we want to keep apart.
  const sid = uuid();
  // ★ Bins = the waterfall's DISPLAY width, not its resolution. The server maps
  // its fine 4096-point FFT onto whatever we ask for, scaled to the ZOOMED span
  // (onSpectrum's `step`, peak-held) — so sharpness still improves as you zoom
  // in, at constant bandwidth. 1024 matches UberSDR's native frame width, which
  // every other VibeSDR surface already renders.
  //
  // ★ Safe for the fixed-ring waterfall: ensureRing() only reallocates when the
  // COLUMN count changes, and that is fixed at connect — a window resize still
  // just shows more/fewer rows, so history survives a drag exactly as before.
  // ★★ 1024 BINS, THE SAME AS THE APP (UberSDRClient.VIBE_BINS). Asking for more was tried and
  //    reverted: on an ultrawide it sharpened things only slightly, and it costs a byte per bin
  //    per frame on someone else's uplink plus the FFT work on the serving device. The app is
  //    sharper at the SAME bin count, so resolution was never what the eye was seeing —
  //    processing is (Stuart, 2026-08-01: "I bet its the FFT averaging").
  const specUrl  = `${wsBase}${withAuth('/ws/user-spectrum?user_session_id=' + sid + '&mode=binary8&bins=1024', auth)}`;
  // Ask for Opus ONLY if this browser can decode it (WebCodecs). If not, the server sends raw PCM —
  // heavier, but it just works. The native apps always have Opus; this gate is purely for the
  // unknown browser a web visitor might bring (esp. the public demo). See AudioPlayer.supportsOpus.
  // ★★ …unless we would rather have the RAW stream. Opus at 64 kbps is not transparent:
  // HansVanEijsden (FMDX.org) named the codec by ear on first listen (2026-07-27), and for
  // a DXer straining to pull a station out of the noise, compression artefacts are exactly
  // the thing they are trying to listen past.
  //   • On LOOPBACK we always take raw. The only argument for compressing is bandwidth, and
  //     the host's own browser spends none — so there is nothing to trade against quality.
  //   • Over a network it is the operator's call (see VsUncompressedAudio): "choice" puts a
  //     switch in the audio menu, anything else leaves us on Opus.
  // ★ `?opus` forces Opus even on loopback. Without it the Opus path would be untestable on
  // the Mac dev loop, which is where it is developed — a default that hides a code path from
  // the only machine that exercises it is how that path rots.
  await loadAudioPolicy(httpBase);
  const forceOpus = new URLSearchParams(location.search).has('opus');
  const wantRaw = !forceOpus && (srvLocal || (srvUncompressed === 'choice' && prefersRawAudio()));
  const wantOpus = !wantRaw && await AudioPlayer.supportsOpus();
  const audioUrl = `${wsBase}${withAuth('/ws/audio?user_session_id=' + sid + (wantOpus ? '&codec=opus' : ''), auth)}`;
  // ★★★ WE CAN ALWAYS TAKE OPUS NOW, so this only ever says no when RAW was ASKED for. The old
  // gate answered "no Opus" on every plain-http LAN origin (WebCodecs is [SecureContext]) and the
  // server then refused the uncompressed socket it had just been asked for — silence, on the only
  // origin real listeners use. Which decoder does the work is now a CPU question, logged as such.
  console.info(wantOpus
    ? `[audio] requesting Opus (${await AudioPlayer.supportsWebCodecsOpus() ? 'WebCodecs' : 'WASM'} decoder)`
    : `[audio] requesting uncompressed (${srvLocal ? 'loopback' : 'listener choice'})`);
  refreshRawAudioRow();   // the policy is only known now, and the panel may already be built
  refreshAdminRow();

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
      updateRangeGap(centerHz, bwHz);
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
        // Server's mode wins over the client's built-in default on a fresh visit — the owner can
        // set the starting demodulator, and defaulting to nfm showed the wrong mode + a thin NFM
        // passband until the user clicked. A remembered session still wins over both.
        const initialMode = (last?.mode ?? cfg.serverMode ?? spec!.mode) as SDRMode;
        setMode(initialMode, !!last);
        renderFreq();
        if (last) spec!.tune(last.hz, last.mode, { recenter: true });
      }
    },
    onSummon: () => onSummoned(),
    onBusy: () => showBusy(),
    onEvicted: () => showEvicted(),
    onSessionEnded: (cd) => showSessionEnded(cd),
    onCooldown: (secs) => showCooldown(secs),
    onSessionWarning: (secs) => setTimeLeft(secs),
    onDevice: (present) => showDeviceBanner(present),
    onAdmin: (ok, refused) => {
      if (refused) { adminUnlocked = false; refreshAdminRow(); return; }
      adminUnlocked = ok;
      // ★ Never leave the password sitting in the box, whichever way it went.
      const pwIn = document.getElementById('adminPwInput') as HTMLInputElement | null;
      if (pwIn) pwIn.value = '';
      const pwRow = document.getElementById('adminPwRow');
      if (pwRow) pwRow.hidden = ok;    // wrong password: leave it open to try again
      if (!ok) setStatus('error', 'admin');
      refreshAdminRow();
      if (!ok) alert('That admin password was not accepted.');
    },
    // ★★ THE SERVER'S WORD ON ITS OWN DSP. NR and notch are GLOBAL and sticky —
    // whatever the last listener left is what the radio is doing — so rendering our
    // saved prefs showed NR OFF while it was audibly ON, and only dragging the
    // slider resynced it (Stuart, on MW, 2026-07-28). A control that misreports the
    // radio is worse than a missing one: nothing tells you to look.
    onDspState: (nr, notch) => {
      const nrEl = document.getElementById('nr') as HTMLInputElement | null;
      if (nrEl && (Number(nrEl.value) > 0) !== nr) {
        // Keep the user's chosen STRENGTH when it is on; 0 is the honest "off".
        nrEl.value = nr ? String(Math.max(1, Number(prefs()['nr']) || 50)) : '0';
        nrEl.dispatchEvent(new Event('input'));
      }
      const nEl = document.getElementById('notch');
      if (nEl && nEl.classList.contains('on') !== notch) {
        nEl.classList.toggle('on', notch);
        nEl.textContent = notch ? 'ON' : 'OFF';
      }
    },
    onHwInfo: (gains, rates, locked, maxFps, forceIdle, radio, lockedCentre) => {
      hwGains = gains; hwRates = rates; hwLockedRate = locked;
      hwLockedCentre = lockedCentre ?? 0;
      applyRadioCaps(radio ?? null);
      // THE OWNER'S FRAME-RATE CEILING. Honour it rather than asking for more and being silently
      // clamped: a client that keeps requesting 20 fps and keeps receiving 10 has no way to tell
      // a capped server from a failing link, and the difference matters.
      serverMaxFps = maxFps > 0 ? maxFps : 0;
      if (serverMaxFps > 0 && spec) spec.setFftRate(wantedFps());
      // ★ The owner REQUIRES idle saving (a solar/cellular host, where power outranks a listener's
      // preference). Force it on and lock the control, saying who set it — the same courtesy as a
      // pinned sample rate. Never leave a switch on screen that we would silently ignore.
      applyForcedIdle(forceIdle === true);
      applyRateOptions();
      populateHw();
    },
    onRspStat: (sys, lna, ifgr, overload, settling) => {
      $('initChip').classList.toggle('set', settling);
      // Same fact, two places: beside the gain controls where it can be ACTED on, and on the
      // main screen where it will actually be seen.
      $('ovlChip').classList.toggle('set', overload);
      // ★ The RSP raises this itself when its ADC is clipping — no inference needed, unlike a
      // dongle where it has to be guessed from the spectrum.
      $<HTMLElement>('rspOverload').hidden = !overload;
      // ★ Show what the radio IS doing, not what the sliders were last set to — with AGC on,
      // the IF reduction is the AGC's to move, and a stale slider reading would be a lie.
      $('rspSysGain').textContent = sys > 0 ? `${sys.toFixed(1)} dB` : '—';
      // ★ Under AGC the slider is the AGC's, so it is greyed and unclickable — but it keeps
      // MOVING, because watching the loop work is how you tell it is doing its job. Tweened
      // between updates so it glides rather than hopping.
      const gr = $<HTMLInputElement>('rspIfGr');
      const agcOn = $<HTMLButtonElement>('rspIfAgc').classList.contains('on');
      gr.classList.toggle('agc', agcOn);
      if (agcOn) {
        tweenIfGr(ifgr);
        $('rspIfGrVal').textContent = `${ifgr} dB · AGC`;
      }
      // Keep the RF slider honest too if something else moved the state — and RE-RENDER, or
      // the thumb moves while the label beside it keeps the old number, which reads as the
      // two disagreeing about the same thing.
      const lnaMax = (radioCaps?.lnaStates ?? 10) - 1;
      const el = $<HTMLInputElement>('rspLna');
      if (document.activeElement !== el) {
        el.value = String(lnaMax - lna);
        renderRspVals();
      }
    },
    onRdsX: (x) => {
      const now = Date.now();
      if (grpPrev.at && x.gtot >= grpPrev.tot) {
        const dt = (now - grpPrev.at) / 1000;
        if (dt > 0.2) {
          const inst = (x.gtot - grpPrev.tot) / dt;
          grpRate = grpRate ? grpRate + 0.3 * (inst - grpRate) : inst;
          grpPrev = { tot: x.gtot, at: now };
        }
      } else grpPrev = { tot: x.gtot, at: now };
      rdsExt = x;
      if (rdsPanelOpen()) { renderRds(); drawConstellation(); drawEye(); drawMpx(); }
    },
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
      rdsPi = m.pi;
      rdsBer = m.ber;
      rdsSig = m.sig;
      rdsEcc = m.ecc || 0;
      rdsIso = m.pi > 0
        ? resolveStationIso(m.ecc || undefined, m.pi.toString(16), serverIso || undefined)
        : '';
      if (rdsName) void resolveRdsLogo(rdsName, rdsIso);
      rdsFreq = spec ? spec.frequency : -1;   // this RDS belongs to THIS carrier
      if (rdsPanelOpen()) renderRds();
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
  // ★★★ TELL IT WHETHER THERE IS ANYWHERE TO FALL BACK TO. With the owner's policy OFF the
  //     server REFUSES a socket opened without `codec=opus`, so 'falling back' swapped a
  //     struggling Opus stream for no stream at all — silently. The player keeps rebuilding
  //     the decoder instead, and reports a fault naming who can fix it.
  audio.allowUncompressed = srvUncompressed !== 'off' || srvLocal;
  // ★★★ RUNTIME FALLBACK TO UNCOMPRESSED. `AudioDecoder.isConfigSupported()` said yes and the
  // decoder then failed for real — Edge on Windows 11 played nothing until the user found the
  // uncompressed switch themselves (Stuart, 2026-07-31). A capability probe is a PREDICTION; only
  // decoding is proof. So on the first genuine failure, drop `codec=opus` and reopen: raw PCM is
  // heavier but it always works, and silence is the one outcome worth spending bandwidth to avoid.
  // ★ One shot — AudioPlayer sets opusBroken before calling, so the new player never asks for Opus
  // again this session and cannot ping-pong.
  if (audioUrl.includes('codec=opus')) {
    audio.onOpusFailure = () => {
      const rawUrl = audioUrl.replace(/&codec=opus\b/, '').replace(/\?codec=opus&/, '?');
      console.warn('[audio] reopening without Opus:', rawUrl);
      try { audio?.close(); } catch {}
      audio = new AudioPlayer(rawUrl, {
        onBytes: (n) => { audioBytes += n; },
        onStatus: (st) => { if (st === 'error') setStatus('error', 'audio'); },
      });
      audio.start().catch((e) => console.error('audio restart failed', e));
    };
  }
  // The AudioContext is built after several awaits, so the browser no longer
  // credits it to the Connect click and may leave it suspended. Rather than rely
  // on that chain surviving, always arm a resume on the next real interaction.
  if (NO_AUDIO) console.warn('[bisect] audio disabled by #noaudio');
  else audio.start().catch((e) => console.error('audio start failed', e));

  // PERMANENT resume. Anything that steals focus — a native dialog, a tab switch,
  // the OS — can suspend the AudioContext, and an unsuspend handler that removes
  // itself once it has worked ONCE leaves you stuck with no audio and no way back
  // except a page reload. On a desktop there is no reason to ever sit suspended.
  const kick = () => { void audio?.resume(); };
  for (const ev of ['pointerdown', 'keydown', 'focus'] as const) {
    window.addEventListener(ev, kick);
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden) kick(); });

  // Register the service worker — Chromium will not offer "install" without one. It caches
  // nothing (see /sw.js); it exists purely to satisfy the installability rule.
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('/sw.js').catch(() => { /* http:// LAN, or unsupported */ });
  }

  buildControls();
  initMediaSession();
  if (NO_DEC) console.warn('[bisect] decoders disabled by #nodec');
  else initDecoders(host, auth);
  initIdleThrottle();
  initAdminUnlock();
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
// The operator pinned the captured window (see onHwInfo). A real lock, not a ceiling.
let hwLockedCentre = 0;
/** The resolved PIN credentials, kept so bookmark WRITES can carry them. */
let authState: AuthState | null = null;

/** View signature — the scale and band strip only need redrawing when this changes. */
let lastViewKey = '';

/**
 * ★ THE RENDER LOOP IS NOT FREE, AND IT WAS NOT TIED TO ANYTHING.
 *
 * It ran at the display's rate — 60 fps — no matter how fast frames actually arrived, so asking
 * the server for 5 fps saved the SERVER work and saved the client none at all. MEASURED on an M4:
 * Edge sat at ~40% CPU whether the stream was 20 fps or 5. The cost was never the data; it was
 * redrawing the canvas sixty times a second, which Chromium does far more expensively than Safari.
 *
 * So the render rate now follows the chosen waterfall rate. Three times the frame rate keeps the
 * interpolated scroll smooth (rows are synthesised onto the render clock, which is what stops it
 * stepping), floored at 20 so it never looks juddery and capped at 60 so it never exceeds the
 * display. At 5 fps that is 15 renders a second instead of 60 — the lever the listener actually
 * wanted when they turned the rate down because their machine was struggling.
 */
function renderHz(): number {
  // The render clock must also keep up with the waterfall's EMIT rate (Speed × dpr pixel rows/sec),
  // or a high speed on a Retina canvas would out-run the draw loop and the scroll would stall.
  const emit = wfSpeed * renderDpr();
  return Math.min(60, Math.max(20, Math.max(wantedFps() * 3, emit)));
}

let lastRenderAt = 0;

/**
 * ★ SELF-PROFILER — add #perf to the URL.
 *
 * DevTools needs a responsive browser to draw its own timeline, which is exactly what we do not
 * have on Chromium here. So the page times itself: microseconds around each phase, averaged over a
 * second, printed to the console and shown in the corner. It works when the profiler cannot, and it
 * will work the same way on Windows and Android, where we have no debugger at all.
 *
 * Off unless asked for — the timing itself is cheap but not free.
 */
// Checked LIVE, not captured at load: the hash can be set after the script runs (and we rewrite it
// ourselves when a summon lands), so a snapshot taken once was fragile. Also switchable by hand
// from the console — `vibePerf(true)` — for when a URL cannot easily be edited.
/**
 * ★ BISECT SWITCHES, for the Chromium cost hunt (BUG-vibeserver-chromium-render.md).
 *
 * The self-profiler proved our JS drawing is ~0% of wall on Chromium while the process sits at 38%,
 * so the cost is somewhere we cannot time from inside a frame. These turn whole subsystems OFF so
 * the bill can be attributed by subtraction, which is the only tool left:
 *
 *   #noaudio  — never start the audio pipeline (WebSocket, worklet, decode)
 *   #nowf     — never draw the waterfall or spectrum (data still arrives)
 *   #nodec    — never start the decoder sidecar connection
 *
 * Combine freely, e.g. `#perf,noaudio`. Whichever one collapses the CPU names the culprit.
 */
const NO_AUDIO = location.hash.includes('noaudio');
const NO_WF    = location.hash.includes('nowf');
const NO_DEC   = location.hash.includes('nodec');

let PERF_FORCE: boolean | null = null;
function isPerf(): boolean {
  if (PERF_FORCE !== null) return PERF_FORCE;
  return location.hash.includes('perf') || location.search.includes('perf');
}
(window as unknown as { vibePerf: (on?: boolean) => string }).vibePerf = (on = true) => {
  PERF_FORCE = on;
  return on ? 'perf overlay ON' : 'perf overlay OFF';
};
const perf = { tick: 0, draw: 0, scale: 0, frames: 0, renders: 0 };
let perfEl: HTMLDivElement | null = null;

function perfReport(secs: number) {
  if (!isPerf()) return;
  const n = Math.max(1, perf.renders);
  // ★ JS HEAP (Chromium only). This is the number that says WHOSE leak it is: if the heap climbs,
  // it is our objects; if the heap is flat while the process still grows, the growth is in GPU or
  // canvas memory and no amount of staring at our JS will find it.
  const mem = (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory;
  const heap = mem ? ` · heap ${(mem.usedJSHeapSize / 1048576).toFixed(0)}MB` : '';
  const line =
    `renders ${(perf.renders / secs).toFixed(0)}/s · ` +
    `tick ${(perf.tick / n).toFixed(2)}ms · ` +
    `draw ${(perf.draw / n).toFixed(2)}ms · ` +
    `scale ${(perf.scale / n).toFixed(2)}ms · ` +
    `total ${((perf.tick + perf.draw + perf.scale) / n).toFixed(2)}ms/render · ` +
    `${(((perf.tick + perf.draw + perf.scale) / (secs * 1000)) * 100).toFixed(0)}% of wall` + heap;
  console.log('[perf]', line);
  if (!perfEl) {
    perfEl = document.createElement('div');
    perfEl.style.cssText =
      'position:fixed;left:8px;top:8px;z-index:9999;background:rgba(0,0,0,0.8);color:#ffb833;' +
      'font:11px ui-monospace,monospace;padding:6px 8px;border:1px solid rgba(255,160,0,0.4);' +
      'border-radius:6px;pointer-events:none;white-space:pre';
    document.body.appendChild(perfEl);
  }
  perfEl.textContent = line.replace(/ · /g, '\n');
  perf.tick = perf.draw = perf.scale = perf.renders = 0;
}

function loop() {
  if (!wf || !spec) return;
  const nowMs = performance.now();
  const minGap = 1000 / renderHz() - 1;      // -1ms: never miss a slot to rounding
  if (nowMs - lastRenderAt < minGap) { requestAnimationFrame(loop); return; }
  lastRenderAt = nowMs;

  wf.vfoHz = spec.frequency;
  updateViewOverlays();
  // Passband drives the acrylic sidebands — so bandwidth is something you SEE
  // sitting on the signal, not a number you read.
  wf.filterLow = spec.bandwidthLow;
  wf.filterHigh = spec.bandwidthHigh;
  const measuring = isPerf();
  const t0 = measuring ? performance.now() : 0;
  if (!NO_WF) wf.tick();   // synthesise any waterfall lines now due (see Waterfall.tick)
  const t1 = measuring ? performance.now() : 0;
  if (!NO_WF) wf.draw();
  const t2 = measuring ? performance.now() : 0;

  // ★ THE SCALE AND BAND STRIP ARE NOT PER-FRAME WORK. Both redraw TEXT — frequency labels, band
  // names — and both only change when the view does: a tune, a zoom, or a resize. Redrawing them
  // 60 times a second was pure waste, and expensive waste: text and path rasterisation are the
  // slowest things a 2D canvas does, and markedly slower on Chromium than on Safari's Core
  // Graphics. MEASURED on an M4 serving the same page: Edge 38.7% CPU / 89°C against Safari's
  // 4.9% / 59°C, with Edge driving both the CPU and GPU clocks higher as well.
  //
  // So: redraw them only when the view key actually changes. The waterfall itself still draws
  // every frame — it interpolates rows onto the render clock, which is what makes it scroll
  // smoothly rather than stepping.
  // Key on the SAME span/centre drawScale/drawBands actually render from (wf.*), not spec.spanHz():
  // the two diverge across a sample-rate change, so keying on spec's value left the axis showing a
  // stale span while the waterfall drew the new one (Stuart 2026-07-24 — "wrong but back"). Keep the
  // VFO/rf-centre terms so tuning still forces a redraw.
  const key = `${spec.frequency}|${spec.rfCenterHz()}|${wf.spanHz}|${wf.displayCenterHz()}|${window.innerWidth}`;
  if (key !== lastViewKey) {
    lastViewKey = key;
    drawScale();
    drawBands();
  }
  if (measuring) {
    const t3 = performance.now();
    perf.tick += t1 - t0; perf.draw += t2 - t1; perf.scale += t3 - t2; perf.renders++;
  }

  const now = performance.now();
  if (now - lastBytesAt > 1000) {
    const secs = (now - lastBytesAt) / 1000;
    audioKbps = (audioBytes / 1024) / secs;
    specKbps  = (specBytes / 1024) / secs;
    audioBytes = 0;
    specBytes = 0;
    framesPerSec = frameCount / secs;
    frameCount = 0;
    lastBytesAt = now;
    perfReport(secs);
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
  const dpr = renderDpr();
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
  const dpr = renderDpr();
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
      // ★★ CLIPPED TO ITS OWN SEGMENT. The fit test above leaves a margin, but the shadow copy
      //    is drawn a pixel right and down, and a fractional segment edge can round the other
      //    way — so on a phone the last letter of "FM Broadcast Band" leaked into the band next
      //    to it. A label that escapes its block reads as belonging to the WRONG BAND, which on
      //    a band plan is not cosmetic. Clipping makes the overflow impossible rather than
      //    unlikely, whatever the width, dpr or rounding.
      ctx.save();
      ctx.beginPath();
      ctx.rect(x0, 0, x1 - x0, h);
      ctx.clip();
      ctx.fillStyle = 'rgba(0,0,0,0.55)';
      ctx.fillText(label, (x0 + x1) / 2 - tw / 2 + dpr, h / 2 + dpr);
      ctx.fillStyle = 'rgba(255,255,255,0.95)';
      ctx.fillText(label, (x0 + x1) / 2 - tw / 2, h / 2);
      ctx.restore();
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
// ★ The frequency the RDS data above belongs to. RDS identifies ONE carrier, so the
// moment you tune away it is stale — but it used to be cleared only on a MODE change,
// never on a frequency change, so the name and logo followed you up the band. That was
// invisible while the OS media card was only refreshed at the instant of tuning; now
// that the card tracks the station live, a stale name sits there in plain sight.
let rdsFreq = -1;
// ★★ PI, kept SEPARATELY from the name — because it survives conditions the name does
// not. PS is 8 characters assembled across 4 groups, so losing any one of them leaves
// the name blank; PI is 16 error-protected bits repeated ~11 times a second and
// confirmed by repetition. On a weak station the PI is very often the ONLY thing that
// gets through, and it is a complete identification: it is what a database lookup, a
// learned station and the FM-DX dial are all keyed on. Storing it inside the name meant
// throwing away a confirmed identity for want of a label (Stuart, 2026-07-26).
let rdsPi = -1;
let rdsBer = -1;    // block error rate %, -1 = decoder has no full window yet
let rdsSig = -99;   // 57 kHz level vs pilot, dB
let rdsEcc = 0;     // Extended Country Code (group 1A), 0 = not received

/** Drop RDS state if the dial has moved off the station it came from. */
function expireRdsIfRetuned() {
  if (rdsFreq < 0 || !spec || spec.frequency === rdsFreq) return;
  rdsName = ''; rdsText = ''; rdsIso = ''; rdsLogoUrl = ''; logoQuery = '';
  rdsPi = -1; rdsBer = -1; rdsSig = -99; rdsExt = null;
  grpRate = 0; grpPrev = { tot: 0, at: 0 }; rdsEcc = 0;
  rdsFreq = -1;
}

/** PI as the four hex digits every FM-DXer reads it as, e.g. C06F. */
function piHex(pi: number): string {
  return pi > 0 ? pi.toString(16).toUpperCase().padStart(4, '0') : '';
}

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
  expireRdsIfRetuned();
  if (!spec) return;
  // ★★ The Advanced RDS decoder OWNS the RDS display while it is open — the bar would be
  // the same data, smaller and less complete. Hidden, not emptied: the media card reads
  // the name from here, and clearing it would strip the OS Now Playing title (the same
  // trap as the stale-textContent bug). Only the DRAWING stops.
  if (rdsPanelOpen()) { $('vts').classList.remove('show', 'on'); return; }
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

  // ★★ PI ALONE IS AN IDENTIFICATION. Falling back to it before giving up is the whole
  // point of decoding it separately: on a weak station the name frequently never
  // assembles while the PI arrives cleanly, and "C06F" tells an FM-DXer exactly which
  // transmitter they have caught. Showing nothing in that case discards a confirmed
  // result — which is what made our RDS look far worse than it was, on a signal that
  // was telling us who it was all along (2026-07-26).
  if (!name && rdsPi > 0) {
    name = 'PI ' + piHex(rdsPi);
    flag = isoToFlag(rdsIso);
  }

  // Nothing known here — hide it rather than show an empty bar. ★ A confirmed PI counts as
  // known (see the fallback above); a block-error figure does NOT. Diagnostics belong in the
  // Advanced RDS decoder, not on the bar an ordinary listener reads (Stuart, 2026-07-26).
  if (!name) {
    vts.classList.remove('show', 'on');
    // ★ CLEAR THE TEXT, don't just hide it. updateMediaSession() falls back to this
    // element when there is no RDS name, so a stale value left in the DOM came back as
    // the OS Now Playing title — the old station's name sitting on the card long after
    // tuning away. Hiding an element does not empty it.
    $('vtsName').textContent = '';
    setDecBoxOffset();
    // ...and still republish. This early return used to skip the card update entirely,
    // so the one case that most needed correcting — tuned onto nothing — was the one
    // case that never refreshed.
    updateMediaSession();
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
  // ★ Compare against the RAW message on the dataset, not textContent — while a long message is
  //   circling the element holds TWO copies plus a separator, so textContent never equals `rt`
  //   and every render would look like a change (and refit, and restart the loop).
  const rtChanged = rtInner.dataset.rt !== rt;
  if (rtChanged) { rtInner.dataset.rt = rt; rtInner.textContent = rt; }
  rtEl.classList.toggle('show', showRt);
  // Pin the bar's width while RadioText is on show. Without this the bar is
  // content-sized under a max-width, so it simply GREW to fit each message — which
  // meant the text never overflowed, never scrolled, and the whole bar visibly
  // expanded and contracted on every RadioText update instead.
  vts.classList.toggle('rt', showRt);
  // ★★★ ONLY REFIT WHEN THERE IS SOMETHING NEW TO FIT. This ran on EVERY VTS render — several
  //     times a second — and each call rewrote `--rtShift` and `animation-duration` under a
  //     RUNNING animation. A measurement that wobbles by a pixel (or dips below the scroll
  //     threshold for a single frame) therefore restarted the marquee from the left, so a long
  //     message could never reach its end no matter how fast it scrolled. Stuart could see it
  //     jump on each RDS refresh — the speed was only ever half of this.
  //     The width matters as much as the text: a rotate or a resize genuinely does need a refit.
  if (showRt) {
    // ★ TOLERANCE, not equality. The pill's other readouts (PI, flag, station name) change width
    //   as they update, which moves this box by a pixel or two — and an exact comparison read
    //   that as "the layout changed, refit", several times a second.
    const w = Math.round(rtEl.clientWidth);
    if (rtChanged || Math.abs(w - lastRtFitWidth) > 8) {
      lastRtFitWidth = w;
      requestAnimationFrame(() => fitRadioText(rtEl, rtInner));
    }
  } else {
    lastRtFitWidth = -1;   // next appearance is a fresh fit, not a stale match
  }

  // RDS mark only when the data really IS RDS — not for a bookmark guess. A confirmed
  // PI counts: it came off the subcarrier exactly as a name does.
  const haveRds = !!rdsName || rdsPi > 0;
  const rdsEl = $('vtsRds');
  rdsEl.classList.toggle('show', haveRds);
  // ★ Block error rate on the badge, so RDS quality is a NUMBER rather than an opinion.
  // Errors are counted BEFORE correction, over the last 12 groups (as redsea defines
  // it), so it describes the link and not how hard the decoder worked — which is the
  // figure that tells a DXer whether a missing name means a marginal signal or a
  // decoder that has given up. -1 = not enough data yet, so say nothing.
  // vtsRds is an IMAGE (the RDS logo), so the error rate goes in its tooltip, not its
  // text — and on the PI chip, which is where a DXer is already looking.
  rdsEl.title = rdsBer >= 0
    ? `Live RDS — block error rate ${rdsBer}% (before correction, last 12 groups)`
    : 'Live RDS data';
  // ★ PI ALONGSIDE the name, always, once confirmed — and the BER chip shows even when
  // NOTHING has decoded, which is the case that most needs explaining. "RDS 42%" says the
  // decoder is synced and the blocks are damaged; nothing at all says it never synced.
  // Without that, a blank box means both and neither.
  const piEl = $('vtsPi');
  const piTxt = piHex(rdsPi);
  // ★★ THE CHIP IS THE PI CODE, AND ONLY THE PI CODE. Block error rate and subcarrier level
  // were shown here while they were being used to diagnose the decoder, and they do not
  // belong: an FM-DXer wants them, an ordinary listener reads them as clutter beside a
  // station name. They live in the Advanced RDS decoder, where someone is deliberately
  // examining signal quality (Stuart, 2026-07-26).
  // ★ Kept in the TOOLTIP, so the measurement is never further away than a hover — it cost a
  // whole evening to be able to see these at all.
  piEl.textContent = piTxt;
  const diag = [
    rdsBer >= 0  ? `block error rate ${rdsBer}% (before correction, last 12 groups)` : '',
    rdsSig > -90 ? `subcarrier ${rdsSig.toFixed(0)} dB vs pilot` : '',
  ].filter(Boolean).join(' · ');
  piEl.title = `PI ${piTxt}${diag ? ' — ' + diag : ''}`;
  piEl.classList.toggle('show', !!piTxt);
  const srcEl = $('vtsSrc');
  // innerHTML, not textContent: the source mark is an inline SVG glyph now, and
  // textContent would print the markup as literal text.
  srcEl.innerHTML = src;
  srcEl.classList.toggle('show', !!src && !haveRds);

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
  // The OS card shows the same station identity as this bar, so republish from the same place.
  // Cheap: updateMediaSession() early-returns unless the station/frequency/mode/art changed.
  updateMediaSession();
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
    updateMediaSession();      // the logo arrives late; the card has to be told
  } catch {
    /* no logo — the monogram-less bar is fine */
  }
}

/** Marquee the RadioText only when it doesn't fit. */
/** Width the marquee was last measured against — see the refit guard in the VTS render. */
let lastRtFitWidth = -1;

/** The separator between the two copies of a circling message — it is what tells you the text
 *  has come round again rather than run on into itself. */
const RT_GAP = '\u00A0\u00A0\u00B7\u00A0\u00A0';

/** ★★★ AN OFFSCREEN RULER, so measuring never touches the element that is animating.
 *  fitRadioText used to strip the `scroll` class to measure an undoubled copy and put it back
 *  afterwards — and REMOVING THAT CLASS RESTARTS THE ANIMATION. Every refit therefore snapped the
 *  message back to the left, which is what Stuart saw on each RDS refresh: RadioText arrives in
 *  pieces as it assembles, so "the text changed" fires repeatedly on what is really one message.
 *  With a separate ruler the live element is only ever written to, never reset. */
let rtRuler: HTMLElement | null = null;
function measureRt(inner: HTMLElement, text: string): number {
  if (!rtRuler) {
    rtRuler = document.createElement('span');
    rtRuler.style.cssText = 'position:absolute;left:0;top:0;visibility:hidden;pointer-events:none;'
                          + 'white-space:pre;';
    inner.parentElement?.appendChild(rtRuler);
  }
  // Same font as the live text, or the measurement means nothing.
  const cs = getComputedStyle(inner);
  rtRuler.style.font = cs.font;
  rtRuler.style.letterSpacing = cs.letterSpacing;
  rtRuler.textContent = text;
  return rtRuler.offsetWidth;
}

function fitRadioText(box: HTMLElement, inner: HTMLElement) {
  // ★★★ A CIRCULAR TICKER, NOT A SLIDE-AND-SNAP. The old marquee ran to the end and jumped back,
  //     so the LAST characters were never readable: they arrived at the moment of the reset, in
  //     the very edge the fade mask softens. The message is doubled with a separator and
  //     translated by exactly ONE copy, so the final frame is identical to the first — the loop
  //     is seamless and every character passes through the middle of the pill.
  const text = inner.dataset.rt ?? inner.textContent ?? '';
  const one = measureRt(inner, text);

  if (one - box.clientWidth <= 4) {              // it fits — plain, static text
    inner.classList.remove('scroll');
    inner.style.removeProperty('--rtShift');
    if (inner.textContent !== text) inner.textContent = text;
    return;
  }

  const shift = measureRt(inner, text + RT_GAP); // one copy + separator = one revolution
  const doubled = text + RT_GAP + text;
  if (inner.textContent !== doubled) inner.textContent = doubled;
  inner.style.setProperty('--rtShift', `${-shift}px`);
  // ★ ~42px/s — gentler than the 55 it replaced (Stuart). With a continuous loop a slower pace
  //   costs nothing, since nothing is missed waiting for a reset; the ceiling still guarantees a
  //   full revolution inside a typical 10-20s RadioText refresh.
  inner.style.animationDuration = `${Math.min(16, Math.max(6, shift / 42))}s`;
  // ★ `add` on a class that is already present is a NO-OP — which is the point: an already
  //   circling message keeps its position and its timing, and only genuinely new text starts over.
  inner.classList.add('scroll');
}

/** Keep the decoder box clear of the VTS bar — same idea as the app's
 *  DecoderPanel bottomOffset (it rides above the pill). */
function setDecBoxOffset() {
  const vts = $('vts');
  const showing = vts.classList.contains('show');
  const h = showing ? vts.offsetHeight + 10 : 0;
  // ★★ AND CLEAR OF THE CONTROL CARD. This only counted the VTS, because when it was written
  //    the controls were a BAR that took its own band out of the layout and nothing could
  //    overlap it. The card FLOATS over the waterfall, so the decoder box drew straight
  //    through it (Stuart, 2026-08-01). Measured, not assumed: the card's height changes with
  //    the arrangement and the font clamp.
  const card = document.getElementById('mcard');
  const cardH = card ? card.offsetHeight + 12 : 0;
  document.documentElement.style.setProperty('--decBoxBottom', `${14 + h + cardH}px`);
}
// Exposed so the card's ResizeObserver can re-run it — see mobile.ts.
(window as unknown as { _vibeSetDecBoxOffset?: () => void })._vibeSetDecBoxOffset = setDecBoxOffset;

// ── dB axis ──────────────────────────────────────────────────────────────────
// Five stops down the left of the spectrum, with faint reference lines — same as
// the app. Without it the trace has no scale at all.

function drawDbAxis(ctx: CanvasRenderingContext2D, W: number, H: number) {
  if (H < 30 || !wf) return;
  const { dbMin, dbMax } = wf.getRange();
  if (!isFinite(dbMin) || !isFinite(dbMax) || dbMax <= dbMin) return;

  const dpr = renderDpr();
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
/** ★ The last passband peak in dBFS. updateSignal() computes it as a local, but the card's
 *  meter can be switched to show dBFS or S-units, and both need the raw figure rather than the
 *  0..1 the gradient uses. Mirrored here rather than recomputed, so every readout agrees. */
let lastSigDb = -160;

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
  lastSigDb = sigDb;

  const { dbMin, dbMax } = wf!.getRange();
  const norm = Math.max(0, Math.min(1, (sigDb - dbMin) / Math.max(1, dbMax - dbMin)));

  // Asymmetric smoothing: fast attack, slow decay (same feel as the app's meter).
  sigSmooth += (norm - sigSmooth) * (norm > sigSmooth ? 0.55 : 0.18);
  sigPeak = norm > sigPeak ? norm : Math.max(norm, sigPeak - 0.004);

  $('sigFill').style.width = `${(sigSmooth * 100).toFixed(1)}%`;
  $('sigPeak').style.left = `${(sigPeak * 100).toFixed(1)}%`;

  // Feed the squelch control the same live scale and signal the main meter is drawing, so the ball
  // sits on exactly the level the fill is showing.
  sqlScaleMin = dbMin; sqlScaleMax = dbMax; sqlSigNorm = sigSmooth;
  drawSquelchBar(sigDb);

  // SQUELCH NEEDLE. The gate is a dBFS threshold and `sigDb` is dBFS, so the needle maps onto the
  // bar through exactly the same normalisation as the fill — it lands where the signal would have
  // to reach to open the gate, which is the only position that means anything.
  const sqlOn = squelchDb > -100;
  const sig = $('sig');
  sig.classList.toggle('sqlOn', sqlOn);
  if (sqlOn) {
    const sqlNorm = Math.max(0, Math.min(1, (squelchDb - dbMin) / Math.max(1, dbMax - dbMin)));
    $('sigSql').style.left = `${(sqlNorm * 100).toFixed(1)}%`;
    // Compare against the RAW reading, not the smoothed fill: the smoothing has a slow decay, so a
    // gate that has just closed would keep reading "passing" for most of a second.
    sig.classList.toggle('sqlClosed', sigDb < squelchDb);
  } else {
    sig.classList.remove('sqlClosed');
  }

  // FIXED-WIDTH fields so the row never shifts as values change length. The S-unit is the worst
  // offender (S9+18 → S6 is 5→2 chars) and the row is centred, so any length change re-centres the
  // whole line (Stuart 2026-07-24). Pad each field to its max width and reserve the SQL slot; with
  // #sigLabel monospace + white-space:pre the line is now constant width.
  const dbfsStr = `${sigDb.toFixed(0).padStart(4)} dBFS`;   // "-120 dBFS" .. "  -30 dBFS"
  const suStr   = toSUnit(sigDb).padEnd(5);                 // "S9+60" .. "S6   "
  const snrStr  = `SNR ${snrSmooth.toFixed(0).padStart(2)} dB`;
  const sqlStr  = sqlOn && sigDb < squelchDb ? ' · SQL' : '      ';   // reserve the slot either way
  $('sigLabel').textContent = `${dbfsStr} · ${suStr} · ${snrStr}${sqlStr}`;
}

/** The squelch threshold in dBFS, mirrored here so the meter can draw the needle. −100 = off. */
let squelchDb = -100;

/** Push the current threshold everywhere it is applied. Squelch is a SERVER-side gate, so without
 *  the spec call the client could not tell a closed gate from a muted tab. */
function applySquelch(db: number, persist = true) {
  squelchDb = db;
  spec?.setSquelch(db);
  if (audio) audio.squelchDb = db;
  // The plain slider used to persist under 'squelch'; the restore path still reads that key, so the
  // bar must keep writing it or a reload loses the setting AND resurrects the false muted-tab
  // warning. Restore itself passes persist=false — no point rewriting what we just read.
  if (persist) savePref('squelch', db);
}

/** Draw the live bar + threshold ball. Called every meter frame with the raw (unsmoothed) signal
 *  so the red "gated" state is honest — the smoothed fill decays too slowly to judge the gate by. */
function drawSquelchBar(sigDbRaw: number) {
  const bar = document.getElementById('sqlBar');
  if (!bar) return;
  const on = squelchDb > SQL_OFF;
  bar.classList.toggle('on', on);
  const fill = document.getElementById('sqlFill') as HTMLElement | null;
  if (fill) fill.style.width = `${(Math.max(0, Math.min(1, sqlSigNorm)) * 100).toFixed(1)}%`;
  const n = document.getElementById('sqlNeedle') as HTMLElement | null;
  if (on) {
    const span = Math.max(1, sqlScaleMax - sqlScaleMin);
    const frac = Math.max(0, Math.min(1, (squelchDb - sqlScaleMin) / span));
    if (n) n.style.left = `${(frac * 100).toFixed(1)}%`;
    bar.classList.toggle('closed', sigDbRaw < squelchDb);
  } else {
    // Park the ball at the left when off — visible and grabbable, so the control is discoverable.
    if (n) n.style.left = '0%';
    bar.classList.remove('closed');
  }
  const note = document.getElementById('sqlNote');
  if (note) note.textContent = on
    ? 'Audio passes only above the ball. The level you set is where it stays — the bar underneath moves with the signal, the threshold does not.'
    : 'Off — audio always passes. Drag the ball up from the left to set a threshold.';
}

/** Pointer handling for the squelch bar. Drag anywhere on the bar; drag off the LEFT edge to turn
 *  squelch off — the same gesture as "no threshold at all", not a separate control to find. */
function setupSquelchBar() {
  const bar = document.getElementById('sqlBar');
  if (!bar) return;
  const applyFromX = (clientX: number) => {
    const r = bar.getBoundingClientRect();
    const frac = (clientX - r.left) / Math.max(1, r.width);
    if (frac < -0.04) { applySquelch(SQL_OFF); return; }   // off the left edge = OFF
    const f = Math.max(0, Math.min(1, frac));
    applySquelch(Math.round(sqlScaleMin + f * (sqlScaleMax - sqlScaleMin)));
  };
  let dragging = false;
  const down = (e: PointerEvent) => { dragging = true; bar.setPointerCapture(e.pointerId); applyFromX(e.clientX); e.preventDefault(); };
  const move = (e: PointerEvent) => { if (dragging) { applyFromX(e.clientX); e.preventDefault(); } };
  const up   = (e: PointerEvent) => { dragging = false; try { bar.releasePointerCapture(e.pointerId); } catch {} };
  bar.addEventListener('pointerdown', down);
  bar.addEventListener('pointermove', move);
  bar.addEventListener('pointerup', up);
  bar.addEventListener('pointercancel', up);
}
/** The live meter scale, updated each frame — the squelch bar drag maps pointer x through it. */
let sqlScaleMin = -100, sqlScaleMax = -20;
/** The last smoothed signal fraction (0..1), so the bar can colour the fill red when gated. */
let sqlSigNorm = 0;
const SQL_OFF = -100;

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

/** Spectrum frames counted in the current 1s window, and the last completed count. */
let frameCount = 0;
let framesPerSec = 0;

function noteFrame() {
  frameCount++;
  const now = performance.now();
  const expected = 1000 / Math.max(1, wantedFps());   // judge against what we ASKED for
  if (lastFrameAt) {
    const gap = now - lastFrameAt;
    if (gap > expected * 3) linkQ = 1;
    else if (gap > expected * 1.6) linkQ = 2;
    else linkQ = 3;
  }
  lastFrameAt = now;
}

function updateLink() {
  // ★★★ THE HEALTHY CASE WAS NEVER ASSIGNED. linkQ starts at 0 and the old code only ever
  //     wrote 1 or 0 — the branch for "frames arriving normally" set nothing at all, so the
  //     value kept whatever it had, which from startup was 0. The indicator could therefore
  //     show only the red ✕ or one red bar: THREE GREEN AND TWO YELLOW WERE UNREACHABLE, and
  //     a perfectly healthy link displayed as disconnected (Stuart, 2026-08-01).
  // ★★ Now a full ladder, matching the app's LinkBars: 3 green = solid, 2 yellow = jitter or
  //    some drops, 1 red = stalling/reconnecting, 0 = disconnected. Same four states, the same
  //    meanings and the same colours, so the two clients cannot say different things about
  //    the same link.
  if (!spec || !lastFrameAt) linkQ = 0;
  else {
    const expected = 1000 / Math.max(1, wantedFps());
    const since = performance.now() - lastFrameAt;
    // ★ The thresholds are multiples of the EXPECTED frame interval, not fixed milliseconds:
    //   at 5 fps a 300 ms gap is normal and at 20 fps it is a stall, so a fixed number would
    //   be wrong at one end of the rate ladder or the other.
    if (since > 5000)             linkQ = 0;   // nothing for five seconds — gone
    else if (since > expected * 8) linkQ = 1;   // stalling / reconnecting
    else if (since > expected * 3) linkQ = 2;   // jitter, dropped frames
    else                           linkQ = 3;   // solid
  }
  const el = $('linkBars');
  el.className = `q${linkQ}`;
}

function updateStatus() {
  updateLink();

  // The SPECTRUM is the bigger half of the link (~74 KB/s vs ~47 for audio), so
  // reporting only the audio understated the real traffic by more than half.
  const total = audioKbps + specKbps;
  // Just "IDLE" — the fps counter to its left already shows the throttled rate, so "IDLE 5fps"
  // duplicated it AND ran the row off the right edge (the E of IDLE clipped, Stuart 2026-07-24).
  const idle = throttled ? ' · IDLE' : '';
  const el = $('status');
  // ★ The MEASURED arrival rate, not the rate we asked for. Ask the server for 5 fps and there was
  // previously nothing to confirm it had happened — nor to show a link failing to deliver what it
  // promised. Shown to one decimal below 10, because the difference between 4.6 and 5 matters at
  // the bottom of the ladder and is invisible when rounded.
  const fps = framesPerSec >= 10 ? framesPerSec.toFixed(0) : framesPerSec.toFixed(1);
  el.textContent = `${total.toFixed(0)} KB/s · ${fps} fps · ${rtt.toFixed(0)} ms${idle}`;
  el.title = `spectrum ${specKbps.toFixed(0)} KB/s · audio ${audioKbps.toFixed(0)} KB/s`
    + ` · asking for ${wantedFps()} fps`;

  // Faults go on the METER, not into the status text: a long message there ran
  // off the edge of the screen, and the meter is where you're already looking
  // when you're wondering why there's no sound.
  let fault = '';
  let info = '';
  switch (audio?.health) {
    case 'suspended': fault = 'AUDIO PAUSED — CLICK THE PAGE'; break;
    case 'no-stream': fault = 'AUDIO DISCONNECTED'; break;
    case 'silent':    fault = 'NO SOUND — IS THE TAB MUTED?'; break;
    // ★ The one fault the LISTENER cannot fix: only the server's owner can allow the
    //   uncompressed fallback, so say who has to act rather than just what is wrong.
    // ★★ It should now be unreachable — every browser has the WASM decoder, so nothing is
    //    turned away for lack of Opus. If it ever shows again, both decoders died, and that
    //    is worth knowing rather than presenting as a mysterious silence.
    case 'opus-stuck': fault = 'OPUS FAILING — OWNER MUST ALLOW UNCOMPRESSED AUDIO'; break;
    // Squelch is NOT a fault and no longer takes an overlay — the message clipped inside the
    // meter and hid the very bar you watch while waiting for a signal. It shows as the breathing
    // SQL chip beside the link bars instead (below).
  }
  // A fault replaces the meter and pulses, to grab attention. Squelch does neither:
  // it is expected behaviour, and the meter is exactly what you want to watch while
  // waiting for a signal to break the threshold.
  $('sig').classList.toggle('fault', !!fault);
  $('sigFault').textContent = fault;
  $('sigFault').classList.toggle('show', !!fault);
  $('sigFault').classList.remove('info');

  // SQL chip: shown whenever squelch is armed, BREATHING RED only while it is actually muting.
  const chip = $('sqlChip');
  const sqlArmed = squelchDb > -100;
  chip.classList.toggle('set', sqlArmed);
  chip.classList.toggle('muting', audio?.health === 'squelched');
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
  // A click keeps its familiar octave; a sweep uses a quarter of one, or holding
  // the key would cross the entire zoom range before you could let go.
  const zoomStep = (f: number) => () => { spec!.zoomBy(f); updateViewOverlays(); };
  const OCT = Math.pow(2, 0.25);
  attachHoldSweep($('zoomIn'),  zoomStep(2),   zoomStep(OCT));
  attachHoldSweep($('zoomOut'), zoomStep(0.5), zoomStep(1 / OCT));
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

  // ── Mobile control card ──────────────────────────────────────────────────
  // ★ Wired unconditionally; CSS alone decides whether the card is on screen (≤1280px).
  //   Gating the WIRING on width instead would mean a user who resizes the window gets a
  //   dead card — and resizing is precisely how this layout is meant to be reached.
  initMobileControls({
    nudgeSteps: (n) => nudge(n * step),
    zoomBy:     (f) => { spec?.zoomBy(f); updateViewOverlays(); },
    freqHz:     () => spec?.frequency ?? null,
    freqText:   () => cardFreqText(),
    mode:       () => spec?.mode ?? '',
    stepLabel:  () => formatStep(step),
    openStepMenu: (anchor) => openStepMenu(anchor),
    // sigSmooth is the same 0..1 the desktop meter fills to, and the three readings come from
    // the same figures its status line prints — so the card can never contradict the bar.
    signal:     () => ({
      level: sigSmooth, snr: snrSmooth, dbfs: lastSigDb, sUnit: toSUnit(lastSigDb),
      // ★ The squelch threshold in the SAME normalisation the gradient is drawn in, so the
      //   line lands exactly where the signal would have to reach to open the gate. −1 = off.
      //   Computed here rather than in the card: this is the scale the meter itself uses, and
      //   a second derivation would put the line somewhere subtly wrong.
      // ★ CLOSED, not merely ARMED. A threshold set above the noise is normal; the state
      //   worth shouting about is the gate actively muting right now.
      sqlClosed: squelchDb > -100 && lastSigDb < squelchDb,
      sqlNorm: squelchDb > -100
        ? Math.max(0, Math.min(1, (squelchDb - sqlScaleMin) / Math.max(1, sqlScaleMax - sqlScaleMin)))
        : -1,
    }),
    openFreqEntry: () => $('pill').click(),
    modes:      () => MODES as unknown as string[],
    setMode:    (m) => setMode(m as SDRMode, true),
    openMenu:      () => togglePanel('menu'),
    openAudio:     () => togglePanel('audioPanel'),
    openDecoders:  () => togglePanel('decodersPanel'),
  });
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

  // ★★ AND THE FLOATING ONE, over the waterfall. The menu copy is no use for this: the whole
  //    point is that the dial has gone off screen and you want it back NOW, and a control you
  //    have to open a menu to reach is not an undo. Shown only while it is genuinely
  //    off screen — a button that is always there stops meaning anything.
  const float = document.getElementById('mCentreFloat');
  if (float) {
    float.hidden = !offscreen;
    if (offscreen) {
      const lo = wf.displayCenterHz() - span / 2;
      // Which way did it go? We have already worked it out to decide `offscreen`, and
      // "your station is off to the left" is most of what the user wanted to know.
      // ★★ WRITE ONLY ON CHANGE. This runs on every view update, and replacing textContent
      //    destroys the text node UNDER THE POINTER — if that happens between mousedown and
      //    mouseup the mousedown target is detached, no click is generated, and the button
      //    reads as unresponsive until a press happens to fall between two updates (Stuart:
      //    "it did work but needed a few clicks"). Same bug the card's readouts had.
      const label = spec.frequency < lo ? '‹ ⌖ CENTRE ON VFO' : '⌖ CENTRE ON VFO ›';
      if (float.textContent !== label) float.textContent = label;
    }
  }
}

function updateViewOverlays() {
  if (!wf || !spec) return;
  updateCentreBtn();
  // ★★★ ON A SHARED RECEIVER THE RF CENTRE IS ALWAYS SHOWN, in either VFO mode. Hiding it in
  //     LOCKED made sense when locked meant "the dongle follows the dial, so there is nothing to
  //     see". When the OPERATOR has pinned the window it is the reverse: the centre and the band
  //     edges are fixed context every listener needs — this is the window you are inside, and the
  //     one thing you cannot move. UberSDR does the same, showing its RF centre (15 MHz) whatever
  //     the listener's view mode (Stuart, 2026-08-02).
  if (spec.followVfo && !hwLockedCentre) {
    wf.wallLoHz = wf.wallHiHz = wf.rfCenterHz = null;   // free-running dongle: nothing fixed to show
    return;
  }
  // ★★★ THE RF CENTRE IS THE RADIO'S, NOT THE VIEW'S. spec.rfCenterHz() is derived from the
  //     config's centerFreq, which the server sends as the VIEW centre — so it tracked the dial
  //     and read 4.625 MHz on a receiver actually centred on 6.5 (Stuart, 2026-08-02). That was
  //     invisible while the dongle followed the VFO, because the two were the same number. Once
  //     the operator pins the window they are different things, and the fixed one is the whole
  //     point of showing it: this is the window you are inside.
  const rf = hwLockedCentre > 0 ? hwLockedCentre : spec.rfCenterHz();
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
 * Now Playing artwork — the VibeServer mark.
 *
 * It used to composite the phone's lock-screen recipe (an artwork base with the RTL-TCP logo
 * inset, amber-tinted, bottom-right). That is unnecessary here: the VibeServer icon already
 * carries the family radio glyph AND the triangle-node inset, so there is nothing to add — and
 * one image cannot half-load the way two could, which is what left Now Playing showing a blank
 * square. An RDS station logo still overrides this whenever one is known.
 */
let artworkUrl = '';

function buildArtwork() {
  const base = $<HTMLImageElement>('artBase');
  const use = () => {
    if (!base.naturalWidth) return;
    artworkUrl = base.src;      // already a data: URI, baked in at build time
    updateMediaSession();
  };
  if (base.complete) use(); else base.onload = use;
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

/** Last metadata actually published, so this is safe to call on every RDS frame. */
let lastMediaKey = '';

function updateMediaSession() {
  if (!('mediaSession' in navigator) || !spec) return;
  navigator.mediaSession.playbackState = audio?.muted ? 'paused' : 'playing';
  const freq = `${(spec.frequency / 1e6).toFixed(3)} MHz`;
  const station = rdsName || $('vtsName').textContent || '';
  // Artwork: the RTL-TCP art the app uses, so Now Playing looks the same whether
  // you're listening on the phone or in the browser. If the station has an RDS
  // logo, prefer that — it's a picture of what you're actually hearing.
  const artSrc = rdsLogoUrl || artworkUrl;
  // ★ Publish only on a real change, so this can be called from updateVts() — i.e. on every RDS
  // frame — without rebuilding MediaMetadata constantly. It USED to be called only when tuning,
  // which had the effect exactly backwards: the station name and logo arrive SECONDS after the
  // tune, so the card never showed them, and the next tune published the logo of the station you
  // were LEAVING. That is the Heart logo flashing into the artwork for a split second on the way
  // past. (Stuart, 2026-07-25.)
  const key = `${station}|${freq}|${spec.mode}|${artSrc}`;
  if (key === lastMediaKey) return;
  lastMediaKey = key;
  // Artwork is a BONUS, never a precondition. This used to `return` when artSrc was empty,
  // which threw away the title and artist too — so a browser that hadn't decoded the baked-in
  // image yet (or at all) got a blank card rather than a text-only one. Station and frequency
  // are the parts worth having; publish them either way.
  navigator.mediaSession.metadata = new MediaMetadata({
    title: station && station !== '—' ? station : freq,
    artist: station && station !== '—' ? `${freq} · ${spec.mode.toUpperCase()}` : spec.mode.toUpperCase(),
    album: 'VibeSDR',
    ...(artSrc ? { artwork: [{ src: artSrc, sizes: '512x512', type: 'image/png' }] } : {}),
  });
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
/** The listener's chosen full rate. Their machine's limit, not the server's — see wantedFps().
 *  DERIVED from the Speed + Data Rate controls (computeActiveFps); not set directly by the UI. */
let activeFps = 20;
const IDLE_FPS = 5;

// ── Waterfall Speed (on-screen scroll) + Data Rate (server frames) ──────────────
// Two separate axes. SPEED is the interpolated scroll rate (screen rows/sec); DATA RATE is how many
// real frames/sec the server sends. AUTO data = bring in as much as we'll display, capped at server.
let wfSpeed = 20;      // 10 / 20 / 30 screen rows/sec
// ★ SHARP = one row per received frame (the app's waterfall). SMOOTH = interpolate up to wfSpeed.
// ★★ DEFAULTS TO SMOOTH *HERE* AND SHARP IN THE APP — each keeps the waterfall it already had, so
//    nobody's picture changes on upgrade and the other one is a tap away. The two are not better
//    and worse, they trade detail against motion (Stuart: "they both have their merits").
let wfScroll: 'sharp' | 'smooth' = 'smooth';
let wfDataRate = 0;    // 0 = AUTO, else 20 / 10 / 5

/** The server's fps ceiling (its advertised max, else the built-in 20). */
function serverFpsCap(): number { return serverMaxFps > 0 ? serverMaxFps : 20; }

/** The data rate we actually ASK for (before the idle/serverMax clamp in wantedFps). AUTO never asks
 *  for more than the chosen Speed — no point receiving frames the display would only throw away. */
function computeActiveFps(): number {
  return wfDataRate === 0 ? Math.min(wfSpeed, serverFpsCap()) : wfDataRate;
}

/** Apply both controls: recompute the data rate, set the scroll speed, request the rate, refresh UI. */
function applyWaterfallRates() {
  activeFps = computeActiveFps();
  wf?.setSpeed(wfSpeed);
  wf?.setSharpRows(wfScroll === 'sharp');
  // The speed control belongs to SMOOTH: under SHARP the data rate decides, so showing it would be
  // a control whose every use is a no-op.
  { const r = document.getElementById('rowWfSpeed');    if (r) r.hidden = wfScroll === 'sharp';
    const n = document.getElementById('wfSpeedNote');   if (n) n.hidden = wfScroll === 'sharp';
    const t = document.getElementById('wfScrollNote');
    if (t) t.textContent = wfScroll === 'sharp'
      ? 'One row per received frame — most detail, and it scrolls at the data rate.'
      : 'Extra rows are interpolated between frames — set the speed below. Smoother motion, less real detail.';
    for (const b of Array.from(document.getElementById('wfScrollSeg')?.children ?? []) as HTMLButtonElement[])
      b.classList.toggle('on', b.dataset.wfscroll === wfScroll); }
  spec?.setFftRate(wantedFps());
  refreshSpeedSeg();
  refreshDataRateSeg();
  updateStatus();
}

/** Speed buttons are filtered to >= the chosen Data Rate (Data 20 hides Speed 10). AUTO shows all. */
function refreshSpeedSeg() {
  const seg = document.getElementById('wfSpeedSeg'); if (!seg) return;
  const minSpeed = wfDataRate > 0 ? wfDataRate : 0;
  for (const b of Array.from(seg.children) as HTMLButtonElement[]) {
    const v = Number((b as HTMLButtonElement).dataset.wfspeed);
    (b as HTMLButtonElement).hidden = v < minSpeed;
    b.classList.toggle('on', v === wfSpeed);
  }
}

/** Data-rate buttons above the server's ceiling are hidden; AUTO (0) is always available. */
function refreshDataRateSeg() {
  const seg = document.getElementById('wfRateSeg'); if (!seg) return;
  const cap = serverFpsCap();
  for (const b of Array.from(seg.children) as HTMLButtonElement[]) {
    const v = Number((b as HTMLButtonElement).dataset.wfrate);
    (b as HTMLButtonElement).hidden = v > cap;   // 0 (AUTO) is never > cap
    b.classList.toggle('on', v === wfDataRate);
  }
}
/** The owner's cap, from hwinfo. 0 = uncapped. */
let serverMaxFps = 0;
/** What we should be asking for right now: our own choice, clamped to the server's ceiling. */
function wantedFps(): number {
  const want = throttled ? Math.min(IDLE_FPS, activeFps) : activeFps;
  return serverMaxFps > 0 ? Math.min(want, serverMaxFps) : want;
}

// ── "I'm over here" ──────────────────────────────────────────────────────────
// The menu-bar app raises this tab by setting the URL fragment to #here. Changing only the
// fragment fires hashchange WITHOUT reloading, so the waterfall, the audio and the tuned frequency
// all survive being summoned — which is the whole point of finding this tab instead of opening a
// new one.
function flashHere() {
  document.getElementById('hereFlash')?.remove();
  const el = document.createElement('div');
  el.id = 'hereFlash';
  el.addEventListener('animationend', () => el.remove());
  document.body.appendChild(el);
  // Leave the URL clean, so a refresh or a bookmark does not carry the summons with it.
  history.replaceState(null, '', location.pathname + location.search);
}
window.addEventListener('hashchange', () => {
  if (location.hash === '#here') flashHere();
});

/**
 * The server has no radio — the dongle was unplugged, or it failed.
 *
 * Say so, prominently. The alternative is what actually happened: the page kept serving, the
 * waterfall went black, the controls still worked, and nothing anywhere explained why. The server
 * watches for the dongle coming back and resumes on its own, so the message promises exactly that
 * rather than telling anyone to restart something.
 */
function showDeviceBanner(present: boolean) {
  const id = 'deviceBanner';
  document.getElementById(id)?.remove();
  if (present) return;
  const el = document.createElement('div');
  el.id = id;
  el.style.cssText =
    'position:fixed;left:50%;top:16px;transform:translateX(-50%);z-index:9500;' +
    'background:rgba(40,10,0,0.94);color:#ffb833;border:1px solid rgba(255,120,0,0.6);' +
    'border-radius:8px;padding:10px 16px;font:13px ui-monospace,monospace;text-align:center;' +
    'box-shadow:0 4px 18px rgba(0,0,0,0.6)';
  // Promise ONLY what we can deliver. Automatic resume was tried and withdrawn — reopening the
  // device from the watchdog thread crashed the server (see local_sdr_shim.cpp) — so the message
  // says what is actually true today rather than what we wish it did.
  el.innerHTML = 'No radio connected to this server<br>' +
    '<span style="opacity:0.7;font-size:11px">The receiver was unplugged or has failed. ' +
    'Reconnect it and restart VibeServer to resume.</span>';
  document.body.appendChild(el);
}

/** The server is already serving another listener. One radio, one occupant, until multi-client
 *  lands — so say so plainly and STOP (the client already suppressed its auto-reconnect). A full
 *  overlay, not a toast: nothing else on the page will work, and a dismissable banner would just
 *  invite the user to sit staring at a dead waterfall. */
function showBusy() {
  showRefusal('IN USE',
    'This server is serving another listener. It handles one at a time.<br><br>' +
    'Try again in a little while.',
    // ★ The override box is offered ONLY on the IN USE screen, and only when the server says
    // it HAS an admin password. Offering it everywhere would invite listeners to try guessing
    // it, and offering it on a server with no password set is a puzzle with no answer.
    srvAdminProtected);
}

/** ★★ TAKE THE RADIO BACK. The owner's escape hatch: their own receiver is busy and they need
 *  it. Sends a nonce + HMAC on the connect URL, never the password — see resolveAdminOverride.
 *  ★ A reload is the honest retry: it re-runs the preflight and re-opens both sockets cleanly,
 *  carrying the override credentials this time. */
async function doAdminOverride(password: string, status: HTMLElement) {
  status.textContent = 'checking…';
  const q = await resolveAdminOverride(location.origin, password);
  if (!q) { status.textContent = 'this server cannot be overridden'; return; }
  // Stash for the reload to pick up. sessionStorage, not localStorage: an override is for THIS
  // visit, and a credential that outlives the tab is a credential left lying around.
  sessionStorage.setItem('vsAdminOverride', q);
  location.reload();
}

/** ★ The owner has taken their radio back. Not a fault, and worth saying so — being dropped
 *  with no explanation is what makes people assume the software broke. */
function showEvicted() { showRefusal('TAKEN OVER',
  'The owner of this receiver has taken control using the admin password.<br><br>' +
  'You can try again shortly.'); }

/** ★★ The session limit. Say WHY it ended and WHEN they may return — a bare disconnect on a
 *  public receiver reads as a crash, and the listener blames us rather than understanding they
 *  had a share of a shared radio. */
function showSessionEnded(cooldownSec: number) {
  const m = Math.max(1, Math.round(cooldownSec / 60));
  showRefusal('TIME UP',
    'Your session on this shared receiver has ended, so someone else can have a turn.' +
    `<br><br>You can reconnect in about ${m} minute${m === 1 ? '' : 's'}.`);
}

/** Refused because we came back before our cooldown finished. */
function showCooldown(secs: number) {
  const m = Math.max(1, Math.round(secs / 60));
  showRefusal('PLEASE WAIT',
    'You have just had a turn on this shared receiver.' +
    `<br><br>Try again in about ${m} minute${m === 1 ? '' : 's'}.`);
}

function showRefusal(title: string, bodyHtml: string, offerOverride = false) {
  const id = 'busyOverlay';
  if (document.getElementById(id)) return;
  // ★★★ STOP THE AUDIO SOCKET TOO. It reconnects every 3s on its own (audio.ts:430,
  // guarded only by `closedByUs`), and the spectrum's `refused` flag does not reach
  // it — so after a session ended, the audio path kept knocking and got back IN the
  // moment the cooldown lapsed. The listener sat looking at "TIME UP" while the
  // station started playing again a few minutes later (Stuart, 2026-07-28).
  // ★ Every refusal here is TERMINAL, so every socket must be told, not just the one
  // that received the message.
  try { audio?.close(); } catch {}
  const el = document.createElement('div');
  el.id = id;
  el.style.cssText =
    'position:fixed;inset:0;z-index:9800;background:rgba(8,6,1,0.92);' +
    'display:flex;align-items:center;justify-content:center;text-align:center;' +
    'font:15px ui-monospace,monospace;color:#ffb833';
  el.innerHTML =
    '<div style="max-width:340px;padding:24px"><div style="font-size:20px;letter-spacing:4px;' +
    `margin-bottom:12px">${title}</div>` +
    `<div style="opacity:0.8;line-height:1.5">${bodyHtml}</div>` +
    '<button id="busyRetry" style="margin-top:18px;background:none;border:1px solid rgba(255,180,50,0.5);' +
    'color:#ffb833;border-radius:6px;padding:8px 18px;font:13px ui-monospace,monospace;cursor:pointer">' +
    'Try again</button></div>';
  document.body.appendChild(el);
  // A reload is the honest retry: it re-runs the /connection preflight and re-opens the sockets
  // from scratch, so a slot freed in the meantime is picked up cleanly.
  document.getElementById('busyRetry')?.addEventListener('click', () => location.reload());

  if (offerOverride) {
    const box = document.createElement('div');
    box.style.cssText = 'margin-top:22px;padding-top:18px;border-top:1px solid rgba(255,180,50,0.25)';
    box.innerHTML =
      '<div style="font-size:11px;opacity:0.65;letter-spacing:1px;margin-bottom:8px">' +
      'OWNER OF THIS RECEIVER?</div>' +
      '<input id="ovPw" type="password" placeholder="Admin password" ' +
      'style="background:rgba(0,0,0,0.4);border:1px solid rgba(255,180,50,0.4);color:#ffb833;' +
      'border-radius:6px;padding:7px 10px;font:13px ui-monospace,monospace;width:190px">' +
      '<button id="ovGo" style="margin-left:8px;background:none;border:1px solid rgba(255,180,50,0.5);' +
      'color:#ffb833;border-radius:6px;padding:7px 14px;font:13px ui-monospace,monospace;cursor:pointer">' +
      'Take over</button>' +
      '<div id="ovStatus" style="font-size:11px;opacity:0.7;margin-top:8px;min-height:14px"></div>' +
      '<div style="font-size:10px;opacity:0.5;margin-top:6px;line-height:1.4">' +
      'The current listener is disconnected and told why.</div>';
    el.querySelector('div')?.appendChild(box);
    const pw = document.getElementById('ovPw') as HTMLInputElement | null;
    const st = document.getElementById('ovStatus') as HTMLElement;
    const go = () => { if (pw?.value) void doAdminOverride(pw.value, st); };
    document.getElementById('ovGo')?.addEventListener('click', go);
    pw?.addEventListener('keydown', (e) => { if ((e as KeyboardEvent).key === 'Enter') go(); });
  }
}

/** The server says the person at the host machine is looking for this tab. */
function onSummoned() {
  flashHere();
  // Best effort — browsers rightly refuse to steal focus without a user gesture, and the menu-bar
  // click happened in another app. The flash is what actually does the work; if focus lands too,
  // so much the better.
  try { window.focus(); } catch { /* not permitted — the flash still shows */ }
}

let lastInteraction = Date.now();
let throttled = false;
/** Listener's choice. Off = never ask the server to slow down, however long nobody touches it. */
let idleSaver = true;
/** The owner has made idle saving mandatory (hwinfo.forceIdleSaver). */
let idleForced = false;

/**
 * Offer only the rates the owner actually permits.
 *
 * A ceiling of 10 fps means 20 is not on the menu; a ceiling of 5 leaves no choice at all, so the
 * whole row goes — a control with one option is just a label pretending to be a control. Same rule
 * as the pinned sample rate and the enforced idle saver: never show a switch we would ignore.
 */
function applyRateOptions() {
  // The server ceiling changed (hwinfo). A manual data rate above it falls back to AUTO (which caps
  // itself to the ceiling), then the segments re-filter and the request re-clamps.
  const cap = serverFpsCap();
  if (wfDataRate > cap) wfDataRate = 0;   // AUTO adapts to the new, lower ceiling
  applyWaterfallRates();
}

function applyForcedIdle(forced: boolean) {
  idleForced = forced;
  const btn = document.getElementById('idleSaver') as HTMLButtonElement | null;
  if (!btn) return;
  if (forced) {
    idleSaver = true;
    btn.classList.add('on');
    btn.textContent = 'ON · SERVER';
    btn.disabled = true;
    btn.title = 'The owner of this server requires idle power saving — it may be on battery, solar, or a metered connection.';
  } else {
    btn.disabled = false;
    btn.title = '';
    btn.textContent = idleSaver ? 'ON' : 'OFF';
  }
}

function markActive() {
  lastInteraction = Date.now();
  if (throttled) {
    throttled = false;
    spec?.setFftRate(wantedFps());
    updateStatus();
  }
}

function checkIdle() {
  if (!spec || throttled || !idleSaver) return;
  if (Date.now() - lastInteraction < IDLE_AFTER_MS) return;
  throttled = true;
  spec.setFftRate(wantedFps());
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
    // ★★★ #mcard IS EXEMPT FOR THE SAME REASON #bar IS. This closer runs on POINTERDOWN, so
    //     without the exemption every tap on a card control closed the panels a beat before
    //     that control's own click handler tried to open one — the two fought, and buttons
    //     needed several presses before one happened to land (Stuart, 2026-08-01). #bar was
    //     exempted when it was the only control surface; the card is the second one and
    //     inherited none of it.
    //     ★ The popups are exempt too: #stepMenu and #mModeMenu are anchored menus rather than
    //     PANELS members, so a click on one of their options counted as "outside".
    if (!PANELS.some(id => $(id).contains(t))
        && !t.closest('#bar') && !t.closest('#mcard') && !t.closest('#mCentreFloat')
        && !t.closest('#stepMenu') && !t.closest('#mModeMenu')) closePanels();
  });
}

// ── Decoders ─────────────────────────────────────────────────────────────────
//
// All of these run SERVER-SIDE in the shim (RTTY/NAVTEX/WEFAX/SSTV over
// /ws/dxcluster, FT8/FT4 via subscribe_digital_spots). The browser attaches and
// draws — no WASM, no DSP here. See decoders.ts for the wire formats.

let decoders: DecoderClient | null = null;
let decImgWidth = 0;
// Decoder image buffers (WEFAX/SSTV), mirroring the app's live/prev model. Lines always draw into the
// OFFSCREEN live canvas; the visible #decImage shows either live or the last completed image (prev), so
// a new transmission starting doesn't wipe an image you haven't saved — PREV switches to it while the
// live decode keeps running underneath, and SAVE downloads whichever is shown.
let decLiveCv: HTMLCanvasElement | null = null;   // offscreen: the live image being decoded
let decLiveCtx: CanvasRenderingContext2D | null = null;
let decPrevCv: HTMLCanvasElement | null = null;   // offscreen: the last completed image
let decViewingPrev = false;
let decLiveComplete = false;                       // the live image has hit onImageDone
let decIsRgb = false;                              // for the save filename (sstv vs wefax handled by title)
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

// ── Session countdown ────────────────────────────────────────────────────────
// ★ The server sends a warning at two minutes and again at thirty seconds; the display ticks
// LOCALLY in between. Sending a countdown frame every second would be a message per second per
// listener for a number the client can work out for itself.
// ★ It also means the clock keeps running if a warning frame is lost — the deadline is what we
// hold, not the remaining seconds.
let sessionDeadline = 0;      // epoch ms, 0 = no limit
let sessionTicker: ReturnType<typeof setInterval> | null = null;

function setTimeLeft(secs: number) {
  sessionDeadline = Date.now() + secs * 1000;
  if (!sessionTicker) sessionTicker = setInterval(paintTimeLeft, 1000);
  paintTimeLeft();
}

function paintTimeLeft() {
  const el = document.getElementById('rxTimeLeft');
  if (!el) return;
  if (!sessionDeadline) { el.hidden = true; return; }
  const left = Math.max(0, Math.round((sessionDeadline - Date.now()) / 1000));
  const mm = Math.floor(left / 60), ss = left % 60;
  // ★ RAN OUT. Unlike Jr there is no server list to return to, so the page simply
  // says what happened and stays there — a browser tab that goes quiet with no
  // explanation reads as the receiver having broken. Shown once; the ticker stops
  // so nothing overwrites it, and it remains until the tab is closed.
  if (left === 0) {
    el.textContent = 'Your turn ends in 0:00';
    el.className = 'crit';
    el.hidden = false;
    if (sessionTicker) { clearInterval(sessionTicker); sessionTicker = null; }
    // ★ NO CARD HERE. The app already shows one — showRefusal('TIME UP', …) fires
    // when the server closes the session, and it says MORE than a card of ours
    // could: it names the cooldown ("you can reconnect in about 2 minutes") and
    // offers Try again. Adding a second overlay for the same moment would just be
    // two things racing to explain one event. All this needs to do is stop the
    // clock at 0:00 and leave it there.
    return;
  }
  // ★ Say what the clock IS. A bare "1:47" over a waterfall is a mystery; people assume it is
  // a recording timer or the station's clock.
  el.textContent = `Your turn ends in ${mm}:${String(ss).padStart(2, '0')}`;
  el.className = left <= 30 ? 'crit' : left <= 120 ? 'warn' : '';
  el.hidden = false;
  $('rxBadge').hidden = false;   // the badge may be empty if the owner set no name
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

// ★ RAW vs CONFIRMED was REMOVED 2026-07-28 (Stuart: "it doesn't actually do
// anything extra"). It was a client-side view that showed the unconfirmed value for
// the five block-B fields (PTY/TP/TA/MS/DI) and coloured their labels by confirmation
// state — and on any decent signal the two values are identical, so the button looked
// inert. A control that appears to do nothing is worse than no control.
// ★ The server still sends both, so this is a UI removal only and can be reinstated
// if the raw view ever earns its place (it would need to cover more than five fields).

/** Distance to a spot, km — null when either end is unknown. */
function spotDistanceKm(grid?: string): number | null {
  const me = myPos();
  const them = grid ? gridToLatLon(grid) : null;
  if (!me || !them) return null;
  return haversineKm(me, them);
}

/** Great-circle bearing from the receiver to a spot, degrees true. Same pairing as the distance
 *  above — both are only meaningful because the SERVER publishes where it is (see /location). */
function spotBearingDeg(grid?: string): number | null {
  const me = myPos();
  const them = grid ? gridToLatLon(grid) : null;
  if (!me || !them) return null;
  const φ1 = me.lat * Math.PI / 180, φ2 = them.lat * Math.PI / 180;
  const Δλ = (them.lon - me.lon) * Math.PI / 180;
  const y = Math.sin(Δλ) * Math.cos(φ2);
  const x = Math.cos(φ1) * Math.sin(φ2) - Math.sin(φ1) * Math.cos(φ2) * Math.cos(Δλ);
  return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
}

/** hh:mm:ssZ — the exact time, for the expanded row only. */
function fmtSpotTimeSec(t: number): string {
  const d = new Date(t);
  const p = (n: number) => String(n).padStart(2, '0');
  return `${p(d.getUTCHours())}:${p(d.getUTCMinutes())}:${p(d.getUTCSeconds())}z`;
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
let activeDec: 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'rds' | null = null;

/** RDS programme types, RDS (Europe) table — index 0..31. */
const PTY_EU = [
  'None', 'News', 'Current Affairs', 'Information', 'Sport', 'Education', 'Drama',
  'Culture', 'Science', 'Varied', 'Pop Music', 'Rock Music', 'Easy Listening',
  'Light Classical', 'Serious Classical', 'Other Music', 'Weather', 'Finance',
  "Children's", 'Social Affairs', 'Religion', 'Phone In', 'Travel', 'Leisure',
  'Jazz Music', 'Country Music', 'National Music', 'Oldies Music', 'Folk Music',
  'Documentary', 'Alarm Test', 'Alarm',
];
let rdsExt: import('./spectrum').RdsExt | null = null;
let grpRate = 0;        // groups/sec, smoothed from successive totals
let grpPrev = { tot: 0, at: 0 };

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
    onImageDone: () => { $('decStatus').textContent = 'image complete'; markDecImageComplete(); },
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

  initRdsResize();
  initRspControls();
  initAhfControls();
  $('decodersBtn').onclick = () => togglePanel('decodersPanel');
  $('decClose').onclick = () => closePanels();

  // ── Decoder selection: toggles start/stop, and the MENU STAYS OPEN (skin
  //    semantics, same as the app). Selecting one opens the output box.
  for (const b of Array.from($('decodersPanel').querySelectorAll('[data-dec]')) as HTMLButtonElement[]) {
    b.onclick = () => {
      const mode = b.dataset.dec as 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'rds';
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
  $('decPrev').onclick = () => toggleDecPrev();
  $('decSave').onclick = () => saveDecImage();
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
  const isRds = what === 'rds';
  $('decBox').classList.add('open');
  $('decBox').classList.remove('min');
  // ★ "ADV RDS", never plain "RDS". Basic RDS — station name, RadioText, PI — is ALWAYS on
  // and needs no decoder; a button labelled "RDS" would read as "switch this on to get RDS"
  // and imply the app had none until you did (Stuart, 2026-07-26).
  $('decTitle').textContent = what === 'spots' ? 'FT8 / FT4 SPOTS'
                            : what === 'rds'   ? 'ADV RDS'
                            : what.toUpperCase();
  $('decStatus').textContent = 'listening…';
  $('decImage').classList.toggle('on', image);
  $('decText').classList.toggle('off', image || isSpots);
  $('spotList').classList.toggle('on', isSpots);
  $('spotFilters').classList.toggle('show', isSpots);
  // ★★ Advanced RDS owns the whole body, and HIDES THE STATION BAR while it is open. The
  // bar is a compressed version of the same data; showing both would be saying everything
  // twice, with the smaller copy competing for attention (Stuart, 2026-07-26).
  $('rdsPanel').classList.toggle('show', isRds);
  $('decBox').classList.toggle('rds', isRds);   // lets the panel own its own height
  // ★★ THE SIZE TOGGLE IS FOR EVERY DECODER, not just Advanced RDS. It was gated on `isRds`, so
  //    the one panel whose whole point is a PICTURE — SSTV — had no way to be made bigger, and
  //    WEFAX and the spot list were stuck at one size too (Stuart). RDS keeps its own pair of
  //    heights; the others grow the box itself, which is what makes an image bigger.
  $('rdsSize').classList.toggle('show', true);
  // RAW is an ADV RDS concept only — hide the button outright for every other decoder.
  applyRdsSize();
  $('decText').classList.toggle('off', image || isSpots || isRds);
  if (isRds) { renderRds(); drawConstellation(); drawEye(); drawMpx(); }
  updateVts();
  // Image buffers/buttons only apply to WEFAX/SSTV — reset the buffers on open/switch, and hide the
  // PREV/SAVE buttons entirely for text/spot decoders.
  if (image) resetDecImages();
  else {
    $<HTMLButtonElement>('decPrev').style.display = 'none';
    $<HTMLButtonElement>('decSave').style.display = 'none';
  }
  setDecLive(false);
}

function hideDecBox() {
  $('decBox').classList.remove('open');
  $('rdsPanel').classList.remove('show');
  $('decBox').classList.remove('rds');
  $('rdsSize').classList.remove('show');
  updateVts();      // the bar comes back when Advanced RDS closes
}

/** ★ TWO FIXED SIZES, on a button. Short enough to leave the waterfall usable while tuning,
 *  or tall enough to read every field at once — which are the only two things anyone wanted.
 *  ★ Not a drag: the handle had to live inside a bottom-anchored box where it could not be
 *  grabbed, a miss went through to the waterfall and TUNED THE RADIO, and touch has no
 *  pointer to drag with. A button has none of those failure modes and works everywhere.
 */
let rdsTall = localStorage.getItem('rdsPanelTall') === '1';

function applyRdsSize() {
  const panel = $('rdsPanel');
  const btn = $('rdsSize');
  // Phones get a smaller pair: the waterfall behind still has to be usable for tuning.
  const phone = window.innerWidth <= 760;

  // ★★★ FOR EVERY OTHER DECODER, BIG MEANS WIDER. An SSTV or WEFAX image is drawn at the box's
  //     width, so height alone does nothing for it — the picture only gets bigger if the box
  //     does. The class does the work in CSS so the container query keeps governing the layout
  //     inside at whichever width results.
  $('decBox').classList.toggle('big', rdsTall && !rdsPanelOpen());
  btn.classList.toggle('tall', rdsTall);
  btn.title = rdsTall ? 'Smaller panel' : 'Bigger panel';
  if (!rdsPanelOpen()) { panel.style.height = ''; panel.style.maxHeight = ''; return; }

  if (rdsTall) {
    // ★★ TALL FITS THE CONTENT, it does not reach for a number. A fixed tall height left a
    // large black void whenever the station sent less than the maximum — and RDS fields
    // arrive over MINUTES (RT+ waits on a 3A announcement, CT comes once a minute), so the
    // content genuinely grows while you watch. Fitting means "show me everything there is
    // now", which is what pressing it means; the cap only binds when there is more than the
    // window can hold (Stuart, 2026-07-26).
    panel.style.height = 'auto';
    panel.style.maxHeight = phone ? 'min(56vh, 420px)' : 'min(78vh, 780px)';
  } else {
    // Compact is a deliberate FIXED height: it is the "leave me room to tune" state, and it
    // must not drift as fields arrive.
    panel.style.height = phone ? 'min(30vh, 240px)' : 'min(40vh, 330px)';
    panel.style.maxHeight = 'none';
  }
}

function initRdsResize() {
  applyRdsSize();
  $('rdsSize').onclick = (e) => {
    e.stopPropagation();
    rdsTall = !rdsTall;
    localStorage.setItem('rdsPanelTall', rdsTall ? '1' : '0');
    applyRdsSize();
  };
  // A rotated phone changes which pair of sizes is right.
  window.addEventListener('resize', () => applyRdsSize());
}

/** True while the Advanced RDS decoder owns the RDS display. */
function rdsPanelOpen(): boolean {
  return activeDec === 'rds' && $('decBox').classList.contains('open');
}

/** Fill the Advanced RDS fields. Everything here is data we already received. */
function renderRds() {
  const dash = '—';
  // ★ The flag and logo move into the header while the bar is hidden, so the station keeps
  // the same visual identity it had on the VTS rather than becoming a table of numbers
  // (Stuart, 2026-07-26).
  $('decFlag').textContent = rdsName || rdsPi > 0 ? isoToFlag(rdsIso) : '';
  // ★ TWO COPIES, ON PURPOSE. The header badge is the identity you glance at while the panel
  // is minimised; the big one under the MPX fills space the column already leaves empty and is
  // the one you actually look at. Both are driven from the same URL so they cannot disagree.
  for (const id of ['decLogo', 'rdsLogoBig']) {
    const el = document.getElementById(id) as HTMLImageElement | null;
    if (!el) continue;
    if (rdsLogoUrl) { if (el.src !== rdsLogoUrl) el.src = rdsLogoUrl; el.classList.add('show'); }
    else el.classList.remove('show');
  }
  // ★ HEX AND DECIMAL. Hex is how the standard defines PI, and how it decomposes into
  // country / coverage / reference — but plenty of databases, loggers and older receivers
  // quote it in DECIMAL, so DXers comparing catches see both forms. Showing both saves
  // anyone doing hex arithmetic to match a log entry (Stuart, 2026-07-27).
  $('rxPi').textContent  = rdsPi > 0 ? `${piHex(rdsPi)} · ${rdsPi}` : dash;
  $('rxPs').textContent  = rdsName || dash;
  $('rxRt').textContent  = rdsText || dash;
  // ★ RT+ — the tags point INTO RadioText, so they can only appear once group 3A has
  // announced which group carries them. That announcement is infrequent, which is why this
  // often fills in well after the RadioText itself.
  const rtpA = rdsExt?.rtpArtist ?? '', rtpT = rdsExt?.rtpTitle ?? '';
  $('rxRtp').textContent = (rtpA || rtpT)
    ? (rtpA && rtpT ? `${rtpA} — ${rtpT}` : (rtpT || rtpA))
    : dash;
  $('rxLps').textContent = rdsExt?.longPs?.trim() || dash;
  $('rxPtyn').textContent = rdsExt?.ptyn?.trim() || dash;
  // RDS language codes (IEC 62106). Only the ones a European DXer will actually meet.
  const LANGS: Record<number, string> = {
    1:'Albanian',2:'Breton',3:'Catalan',4:'Croatian',5:'Welsh',6:'Czech',7:'Danish',
    8:'German',9:'English',10:'Spanish',11:'Esperanto',12:'Estonian',13:'Basque',
    14:'Faroese',15:'French',16:'Frisian',17:'Irish',18:'Gaelic',19:'Galician',
    20:'Icelandic',21:'Italian',22:'Lappish',23:'Latin',24:'Latvian',25:'Luxembourgish',
    26:'Lithuanian',27:'Hungarian',28:'Maltese',29:'Dutch',30:'Norwegian',31:'Occitan',
    32:'Polish',33:'Portuguese',34:'Romanian',35:'Romansh',36:'Serbian',37:'Slovak',
    38:'Slovene',39:'Finnish',40:'Swedish',41:'Turkish',42:'Flemish',43:'Walloon',
  };
  const lang = rdsExt?.lang ?? 0;
  $('rxLang').textContent = lang ? (LANGS[lang] ?? `code ${lang}`) : dash;
  const ph = rdsExt?.pinHour ?? -1;
  $('rxPin').textContent = ph >= 0
    ? `day ${rdsExt!.pinDay} ${String(ph).padStart(2,'0')}:${String(rdsExt!.pinMin).padStart(2,'0')}`
    : dash;
  // ★ ODA — which Open Data Applications the station runs. This is what tells you whether an
  // empty "Now playing" means the station sends no RT+, or that we failed to decode it.
  const ODA_NAMES: Record<string, string> = {
    '4BD7': 'RT+', '6552': 'eRT', 'CD46': 'TMC', 'CD47': 'TMC', '0093': 'DAB x-ref',
    '4BD8': 'RT+ (group B)', 'C563': 'ID Logic', '6365': 'RDS2 station logo',
  };
  const odas = rdsExt?.oda ?? [];
  $('rxOda').textContent = odas.length
    ? odas.map(o => `${ODA_NAMES[o.aid] ?? o.aid} in ${o.grp >> 1}${(o.grp & 1) ? 'B' : 'A'}`).join(', ')
    : dash;
  // ★ EON — the sister stations. TA on one of them is why a car radio switches over.
  const eons = rdsExt?.eon ?? [];
  $('rxEon').textContent = eons.length
    ? eons.map(e => {
        const ps = e.ps.trim();
        const f = e.af ? ` ${(e.af / 1000).toFixed(1)}` : '';
        return `${ps || e.pi}${f}${e.ta === 1 ? ' [TA]' : ''}`;
      }).join('  ')
    : dash;
  // ★ In RAW, read the UNCONFIRMED value and mark the label; otherwise the confirmed one.
  // A field is "confirmed" when the confirmed value has actually arrived (>= 0) — that is the
  // same test the panel already uses for "known", so red simply means "seen but not yet
  // trusted" and green means "this is what CONFIRMED mode would show".
  const pty = rdsExt?.pty ?? -1;
  $('rxPty').textContent = pty >= 0 ? `${PTY_EU[pty] ?? '?'} (${pty})` : dash;
  // TP/TA/MS are one-bit flags; show the ones that are SET rather than a row of noes.
  const tpV = rdsExt?.tp ?? -1;
  const taV = rdsExt?.ta ?? -1;
  const msV = rdsExt?.ms ?? -1;
  const f: string[] = [];
  if (tpV === 1) f.push('TP');
  if (taV === 1) f.push('TA');
  if (msV === 1) f.push('Music'); else if (msV === 0) f.push('Speech');
  $('rxFlags').textContent = f.length ? f.join(' · ') : dash;
  // All three share block B, so they confirm together — one label for the row is honest.
  $('rxBer').textContent = rdsBer >= 0 ? `${rdsBer}%` : dash;
  // ★ Say what the level is RELATIVE TO. On its own "-10 dB" invites the reading that the
  // signal is weak, when it is the normal injection ratio for a healthy station.
  // ★ Deviations in kHz, each said against its own spec band so the number explains itself
  // — "6.8 kHz" means nothing without knowing 6.0–7.5 is nominal (Stuart: "make it clear").
  const pdev = rdsExt?.pilotDev ?? 0;
  const rdev = rdsExt?.rdsDev ?? 0;
  const pEl = $('rxPilotDev'), rEl = $('rxRdsDev');
  if (pdev > 0.2) {
    const ok = pdev >= 6.0 && pdev <= 7.5;
    pEl.textContent = `${pdev.toFixed(1)} kHz · ${ok ? 'nominal' : pdev < 6 ? 'low' : 'high'}`;
    pEl.style.color = ok ? '#7dff9a' : '#ffd479';
  } else { pEl.textContent = dash; pEl.style.color = ''; }
  if (rdev > 0.2) {
    // ★★ THE SCALE HAS A CEILING, SO THE LABELS MUST TOO. 7.5% of 75 kHz = 5.6 kHz is the spec
    // maximum, and the old wording ran open-ended: anything above 4 kHz was called "generous",
    // so a reading of 12.9 kHz — physically impossible — was presented as a station doing well
    // (Stuart, 2026-07-27). A number past the ceiling is evidence of a MEASUREMENT problem, not
    // of a strong subcarrier, and must never be dressed up as good news.
    // ★ The server now returns -1 with no block sync, which was the cause in that case; this is
    // the second line of defence, for anything else that could put the estimate out of range.
    const impossible = rdev > 5.8, strong = rdev >= 4.0, low = rdev < 1.5;
    rEl.textContent = `${rdev.toFixed(1)} kHz · ${impossible ? 'over spec — suspect' : low ? 'weak' : strong ? 'generous' : 'typical'}`;
    rEl.style.color = impossible ? '#ff8a7d' : low ? '#ffd479' : '#7dff9a';
  } else { rEl.textContent = dash; rEl.style.color = ''; }
  // ★★★ RDS-to-pilot phase. Correct is near 0 or near 90 (quadrature encoding); the middle
  // ground is a transmitter fault, so say which it is rather than leaving a bare number to
  // be interpreted — the whole reason this field exists is that reading it takes equipment
  // most people do not have (HansVanEijsden, FMDX.org, 2026-07-26).
  const rdsPhase = rdsExt?.phase ?? -1;
  const coh = rdsExt?.phaseCoh ?? 0;
  const phEl = $('rxPhase');
  // ★★★ THE SIZER IS SET FROM CODE, NOT FROM THE MARKUP. index.html held the reserve string by
  // hand — "rotating — encoder not locked to pilot" — and when the DRIFT RATE was added to the
  // live message it grew to "rotating 2°/s — …", one line longer than the box reserved for it.
  // #rxPhase is absolutely positioned, so the overflow landed ON TOP OF RadioText (Stuart's
  // screenshot, 2026-07-28). The old comment said "keep the sizer in step" — a rule a human has
  // to remember is a rule that breaks, so the string now lives in ONE place and the sizer is
  // corrected at render time. Widest digits, because 00 is wider than 2 in most faces.
  const PHASE_RESERVE = 'rotating 00°/s — encoder not locked to pilot';
  const szEl = phEl.parentElement?.querySelector('.sizer') as HTMLElement | null;
  if (szEl && szEl.textContent !== PHASE_RESERVE) szEl.textContent = PHASE_RESERVE;
  // ★★ NEVER STATE A PHASE WE CANNOT STAND BEHIND. The estimate averages unit vectors at
  // twice the symbol angle; if our 57 kHz reference drifts at all, those vectors cancel and
  // the average collapses while STILL producing a plausible angle. Observed on air as
  // Classic FM cycling red/amber/green rapidly, and Heart reading 45 then 27 degrees.
  // ★ A confident wrong number here is worse than none: this field would be telling
  // broadcasters their transmitters are faulty (Stuart, 2026-07-26).
  if (rdsPhase < 0 || coh < 0.35) {
    // ★★ A ROTATING CONSTELLATION IS A DIAGNOSIS, NOT A FAILURE. Low coherence with a LOW
    // block error rate is a distinctive combination: the symbols are being decoded perfectly
    // (differential detection does not care about a steady rotation), but the phase is
    // sweeping — which means the station's encoder is not locked to its pilot at all. It
    // draws as a clean RING rather than two lobes, and calling that "noisy" is exactly wrong.
    // ★ Confirmed on Classic FM by two INDEPENDENT receivers, an RTL-SDR and an SDRplay
    // RSP1B, which agreed (2026-07-26). That is a finding a DXer would buy an analyser for.
    const rotating = rdsPhase >= 0 && coh < 0.35 && rdsBer >= 0 && rdsBer < 20;
    phEl.textContent = rdsPhase < 0 ? dash
                     : rotating     ? 'rotating — encoder not locked to pilot'
                                    : 'unstable — not measurable';
    phEl.style.color = rotating ? '#ffd479' : 'var(--text-dim)';
  } else {
    const d0 = Math.min(rdsPhase, 180 - rdsPhase);   // distance from 0/180
    const d90 = Math.abs(rdsPhase - 90);             // distance from quadrature
    const near = Math.min(d0, d90);
    // ★ Only claim a FAULT when the estimate is genuinely solid — asserting a transmitter
    // defect is serious, so the bar for saying it is higher than for the other states.
    // ★ WIDER BANDS, because the measurement is steadier than the labels were. Heart read
    // 27/25/24 degrees across two receivers and two antennas — three degrees of spread — yet
    // flipped between "off nominal" and "FAULT" because the boundary sat at 25. A verdict
    // that changes on a one-degree drift makes a stable measurement look unstable, which is
    // the opposite of what a diagnostic should do (Stuart, 2026-07-26).
    // ★ FAULT is now reserved for genuinely far out (>40 degrees from BOTH nominals) and
    // still requires a solid estimate — calling a broadcaster's transmitter defective
    // deserves the higher bar.
    // ★★ SLOW ROTATION LOOKS PERFECTLY STEADY. Coherence collapses only when the phase turns
    // FAST relative to the averaging window — Classic FM does, draws a circle, and is caught by
    // the branch above. A station whose encoder is only slightly off its pilot keeps coherence
    // high (67%) while the angle walks the whole range, so it slipped through and displayed a
    // number that swept 0->90 and back (Stuart, on Harborough FM, 2026-07-27).
    // ★ The rate is the honest test, and it does not depend on coherence at all. Our 57 kHz
    // reference IS the station's own pilot tripled, so a locked encoder sits still however weak
    // the signal — a steady march means the subcarrier genuinely is not 3x the pilot.
    // ★ 2 deg/s: comfortably above the wander of a noisy estimate, and it takes 45 s to cross
    // the range at that rate, so nothing that drifts this steadily is doing it by accident.
    // ★ NO EARLY RETURN — this runs inside renderRds(), and everything below (the PI
    // decomposition and the rest of the panel) still has to happen.
    const drift = rdsExt?.phaseDrift ?? 0;
    if (drift >= 2) {
      phEl.textContent = `rotating ${drift.toFixed(0)}°/s — encoder not locked to pilot`;
      phEl.style.color = '#ffd479';
    } else {
      const verdict = near <= 12 ? (d0 <= d90 ? 'in phase' : 'quadrature')
                    : near <= 40 ? 'off nominal'
                    : coh > 0.7  ? 'FAULT'
                    : 'off nominal';
      phEl.textContent = `${rdsPhase.toFixed(0)}° · ${verdict} · ${(coh * 100).toFixed(0)}% steady`;
      phEl.style.color = near <= 12 ? '#7dff9a' : near <= 40 ? '#ffd479' : '#ff8a7d';
    }
  }

  // ── PI decomposition — free, it is arithmetic on a number we already have ──────
  // Country code (top nibble), coverage area, and the programme reference number, as the
  // FM-DX Webserver breaks it out. Coverage is the interesting one to a DXer: it says
  // whether a catch is a local filler or a national network.
  const COV = ['Local', 'International', 'National', 'Supra-regional',
               'Regional 1', 'Regional 2', 'Regional 3', 'Regional 4',
               'Regional 5', 'Regional 6', 'Regional 7', 'Regional 8',
               'Regional 9', 'Regional 10', 'Regional 11', 'Regional 12'];
  $('rxPiDetail').textContent = rdsPi > 0
    ? `${COV[(rdsPi >> 8) & 0xF]} · ref ${rdsPi & 0xFF} · cc ${(rdsPi >> 12) & 0xF}`
    : dash;
  // ★ SAY WHY IT IS BLANK. The flag logic refuses to guess a country: it uses the ECC
  // (group 1A) when present, otherwise it validates the PI's country nibble against the
  // RECEIVER's own country — so a server that does not know where it is resolves to
  // nothing. Correct for the station bar, but in an expert panel an empty field reads as
  // broken rather than as "not established yet" (Stuart, 2026-07-26).
  // ECC is also infrequent — group 1A — so this often fills in later, like the clock.
  $('rxCountry').textContent = rdsIso
    ? `${rdsIso.toUpperCase()}${rdsEcc ? ` · ECC ${rdsEcc.toString(16).toUpperCase()}` : ' · from PI'}`
    : rdsEcc
      ? `ECC ${rdsEcc.toString(16).toUpperCase()} · unmatched`
      : (rdsExt?.gtot ?? 0) > 0 ? 'waiting for ECC (1A)' : dash;

  // DI — four flags, assembled across the four name segments.
  // ★★ THE FIELD THIS WHOLE RAW/CONFIRMED SPLIT CAME FROM. Each flag is ONE BIT in one group,
  // so a single mis-corrected block used to set one permanently. In RAW you can now watch them
  // flicker: a genuine flag sits steady across hundreds of groups, corruption does not — which
  // is the only way to tell "this station really is compressed" from "one bad block said so".
  const di = rdsExt?.di ?? -1;
  if (di < 0) $('rxDi').textContent = dash;
  else {
    const d: string[] = [];
    if (di & 1) d.push('Stereo'); else d.push('Mono');
    if (di & 2) d.push('Artificial head');
    if (di & 4) d.push('Compressed');
    if (di & 8) d.push('Dynamic PTY');
    $('rxDi').textContent = d.join(' · ');
  }

  // ★ CT — transmitted once a minute, so RECEIVING one at all proves whole groups are
  // arriving intact, and its offset identifies the network's timezone.
  // ★ CT is transmitted ONCE A MINUTE (group 4A), against ~11 groups a second — about one
  // group in 660. So a dash means "not caught yet" far more often than "not transmitted",
  // and it needs BOTH blocks C and D intact with no repetition to fall back on. Saying
  // "waiting" instead of "—" is the honest reading, and it stops the user concluding the
  // station does not send it (Stuart: car stereos set their clocks from this, 2026-07-26).
  const ct = rdsExt?.ct ?? -1;
  const g4a = rdsExt?.grp?.[8] ?? 0;      // group 4A = index 4*2+0
  if (ct < 0) {
    $('rxCt').textContent = (rdsExt?.gtot ?? 0) > 0
      ? (g4a > 0 ? 'seen, damaged' : 'waiting… (1/min)')
      : dash;
  } else {
    const hh = String(Math.floor(ct / 60)).padStart(2, '0');
    const mm = String(ct % 60).padStart(2, '0');
    const off = rdsExt!.ctoff;
    const os = off === 0 ? 'UTC' : `UTC${off > 0 ? '+' : '−'}${Math.abs(off) / 2}`;
    $('rxCt').textContent = `${hh}:${mm} ${os}`;
  }

  // ★ Group-type histogram — identifies a transmitter's configuration, and shows whether a
  // weak signal is delivering a representative mix or only the easy groups.
  const grp = rdsExt?.grp ?? [];
  const tot = rdsExt?.gtot ?? 0;
  if (!tot) { $('rxGroups').textContent = dash; $('rxRate').textContent = dash; }
  else {
    const parts: string[] = [];
    for (let i = 0; i < grp.length; i++) {
      if (!grp[i]) continue;
      const name = `${i >> 1}${(i & 1) ? 'B' : 'A'}`;
      parts.push(`${name} ${Math.round((grp[i] / tot) * 100)}%`);
    }
    parts.sort((a, b) => parseInt(b.split(' ')[1]) - parseInt(a.split(' ')[1]));
    // ★ SAY WHAT THE PERCENTAGES ARE OF. There are two percentage figures on this panel —
    // this one and the block ERROR RATE — and a bare "0A 40%" gives no clue which kind it is.
    // Stating the denominator makes the row explain itself: 40% OF 504 GROUPS were type 0A
    // (Stuart, 2026-07-27).
    $('rxGroups').textContent = parts.length
      ? `of ${tot} groups: ${parts.join('  ')}`
      : dash;
    // ★ Rate from SUCCESSIVE DELTAS, not total-over-elapsed. gtot accumulates from when the
    // DECODER started; the panel opens later, so dividing one by the other reported 113/s
    // against a theoretical maximum of 11.4 (Stuart, 2026-07-26). ★ Two clocks with
    // different origins is not a rate.
    $('rxRate').textContent = grpRate > 0
      ? `${grpRate.toFixed(1)}/s of 11.4 · ${tot} total`
      : `${tot} total`;
  }

  // ── ALTERNATIVE FREQUENCIES ──────────────────────────────────────────────────
  // ★★ THESE TWO FIELDS WERE DEAD. The markup, the labels and the tooltips have been here
  // all along and NOTHING EVER WROTE TO THEM, so they showed a permanent dash — including
  // the tooltip's promise that you can tap one to tune (Stuart: "i did wonder why i didnt
  // see them populating", 2026-07-27). The data was on the wire the whole time.
  // ★ A field that is always empty is indistinguishable from a station that never sends the
  // thing. That is the real cost: it quietly libels the transmitter.
  const af = rdsExt?.af ?? [];
  const afSeen = rdsExt?.afseen ?? 0;
  // ★ CONFIRMED against GLIMPSED. AF lists arrive spread over many group 0As and are only
  // accepted after repetition, so a score below 100% means entries are arriving damaged —
  // which is a useful signal-quality reading in its own right, not bookkeeping.
  $('rxAfScore').textContent = afSeen > 0
    ? `${af.length}/${afSeen} · ${Math.round((af.length / afSeen) * 100)}%`
    : (rdsExt?.gtot ?? 0) > 0 ? 'none announced' : dash;

  const afEl = $('rxAf');
  if (!af.length) {
    afEl.textContent = (rdsExt?.gtot ?? 0) > 0 ? 'none announced' : dash;
  } else {
    // Ascending, and de-duplicated: the same AF is re-announced constantly.
    const list = [...new Set(af)].sort((a, b) => a - b);
    afEl.innerHTML = list
      .map(khz => `<a href="#" class="afLink" data-khz="${khz}">${(khz / 1000).toFixed(1)}</a>`)
      .join('  ');
    for (const a of Array.from(afEl.querySelectorAll<HTMLAnchorElement>('.afLink'))) {
      a.onclick = (e) => {
        e.preventDefault();
        const khz = Number(a.dataset.khz);
        if (!spec || !khz) return;
        // Same tune path as a bookmark, and WFM explicitly: every AF is a broadcast FM
        // frequency by definition, so inheriting the current mode would be wrong.
        spec.tune(clampTune(khz * 1000), 'wfm' as SDRMode, { recenter: true, retarget: true });
        setMode('wfm' as SDRMode, false);
        renderFreq();
      };
    }
  }
}

/** Pixels per unit, chosen so the mean lobe distance lands at a comfortable fraction of the
 *  box. Falls back to a sane constant when there is nothing to measure. */
function constellationScale(xy: number[], box: number): number {
  let n = 0, sum = 0;
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const r = Math.hypot(xy[i], xy[i + 1]);
    if (r < 1) continue;
    n++; sum += r;
  }
  if (!n) return (box / 2) / 110;
  const mean = sum / n;
  return (box * 0.30) / Math.max(1, mean);
}

/** Rotation that lays the two BPSK lobes on the horizontal. BPSK has 180-degree ambiguity,
 *  so angles are DOUBLED (folding both lobes onto one), magnitude-weighted so the strong
 *  symbols that define the lobes dominate, averaged, then halved. */
function constellationAngle(xy: number[]): number {
  let sx = 0, sy = 0;
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const x = xy[i], y = xy[i + 1];
    const r2 = x * x + y * y;
    if (r2 < 1) continue;
    const a2 = 2 * Math.atan2(y, x);
    sx += r2 * Math.cos(a2);
    sy += r2 * Math.sin(a2);
  }
  return (sx || sy) ? -0.5 * Math.atan2(sy, sx) : 0;
}

/** ★ A PLAIN-ENGLISH VERDICT from the constellation, because the plot assumes you already
 *  know how to read it. Measures how tightly the points cluster around the two lobes
 *  against how far they scatter — error vector magnitude, in effect. The lobes lie on the
 *  horizontal after de-rotation, so |x| is the wanted signal and y is pure error. */
function constellationVerdict(xy: number[]): { text: string; cls: string } {
  // ★★ DE-ROTATE FIRST. The verdict was computed on the RAW points while only the DRAWING
  // was de-rotated, so the maths saw the diagonal lobes and counted the entire carrier
  // phase offset as error — a visibly clean constellation reported "299% EVM" (Stuart,
  // 2026-07-26). ★ Two consumers of one transform is exactly where this kind of bug lives:
  // share the transform, do not repeat it.
  const rot = constellationAngle(xy);
  const cr = Math.cos(rot), sr = Math.sin(rot);
  let n = 0, sumAbsX = 0, sumY2 = 0, sumXErr2 = 0;
  const rx: number[] = [], ry: number[] = [];
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const r2 = xy[i] * xy[i] + xy[i + 1] * xy[i + 1];
    if (r2 < 1) continue;
    const x = xy[i] * cr - xy[i + 1] * sr;
    const y = xy[i] * sr + xy[i + 1] * cr;
    rx.push(x); ry.push(y);
    n++; sumAbsX += Math.abs(x); sumY2 += y * y;
  }
  if (n < 8) return { text: 'no lock', cls: 'bad' };
  const meanAbsX = sumAbsX / n;
  for (let i = 0; i < rx.length; i++) {
    const dx = Math.abs(rx[i]) - meanAbsX;
    sumXErr2 += dx * dx;
  }
  // Error energy is the scatter off the two ideal points; signal energy is the lobe offset.
  const err = Math.sqrt((sumY2 + sumXErr2) / n);
  if (meanAbsX < 1) return { text: 'no lock', cls: 'bad' };
  const evm = (err / meanAbsX) * 100;
  // ★ EVM assumes two lobes after de-rotation. A ROTATING constellation defeats that — the
  // points are ordered, not scattered — so it reports a huge figure for a signal that is
  // decoding flawlessly. Say what it actually is instead of libelling it as noise.
  if (rdsExt && (rdsExt.phaseCoh ?? 0) < 0.35 && rdsBer >= 0 && rdsBer < 20)
    return { text: 'rotating — unlocked encoder', cls: 'ok' };
  // ★ "SCATTER", not "EVM". Error Vector Magnitude is the correct term and what an engineer
  // expects — but it means nothing to someone new to this, and the whole panel is written to
  // explain itself rather than assume. The proper name lives in the tooltip, so a DXer
  // comparing against other equipment can still find it (Stuart, 2026-07-27).
  if (evm < 45)  return { text: `clean · ${evm.toFixed(0)}% scatter`, cls: 'good' };
  if (evm < 80)  return { text: `usable · ${evm.toFixed(0)}% scatter`, cls: 'ok' };
  return { text: `noisy · ${evm.toFixed(0)}% scatter`, cls: 'bad' };
}

/** ★ The symbol trace — the "two lines" read. Symbol value against time: two clean bands
 *  with a clear gap means every bit is being decided with margin; a filled gap means bits
 *  are landing near the threshold and the block errors follow. */
function drawEye() {
  const c = $<HTMLCanvasElement>('rdsEye');
  const g = c.getContext('2d');
  if (!g) return;
  const W = c.width, H = c.height, mid = H / 2;
  g.fillStyle = '#000';
  g.fillRect(0, 0, W, H);
  // The decision threshold — the line a symbol must not stray across.
  g.strokeStyle = 'rgba(255,160,60,0.35)';
  g.beginPath(); g.moveTo(0, mid); g.lineTo(W, mid); g.stroke();
  const xy = rdsExt?.xy ?? [];
  if (!xy.length) return;
  const rot = constellationAngle(xy);
  const cr = Math.cos(rot), sr = Math.sin(rot);
  const n = xy.length / 2;
  const k = constellationScale(xy, H) * 0.9;   // same scale, a touch of headroom
  g.fillStyle = 'rgba(120,255,140,0.85)';
  for (let i = 0; i < n; i++) {
    const x = xy[i * 2] * cr - xy[i * 2 + 1] * sr;   // the wanted component
    const px = (i / (n - 1)) * (W - 2) + 1;
    const py = mid - x * k;
    g.fillRect(px, py, 1.5, 1.5);
  }
}

/** ★ The MPX spectrum — the whole of what the FM demodulator produces, DC to 100 kHz.
 *  Labelled at the three landmarks, because a spectrum of a signal most listeners have never
 *  seen plotted is a puzzle otherwise: L+R audio at the bottom, the 19 kHz PILOT, the L-R
 *  stereo sidebands around 38 kHz, and RDS at 57 kHz. Everything the stereo and RDS decoders
 *  work from, in one picture. */
function drawMpx() {
  const c = $<HTMLCanvasElement>('rdsMpx');
  const g = c.getContext('2d');
  if (!g) return;
  const W = c.width, H = c.height;
  g.fillStyle = '#000';
  g.fillRect(0, 0, W, H);
  const mpx = rdsExt?.mpx ?? [];

  // Landmarks first, so the trace draws over them rather than under.
  const SPAN = 100000;
  const marks: Array<[number, string]> = [[19000, 'PILOT'], [38000, 'L−R'], [57000, 'RDS']];
  g.font = '7px ui-monospace, monospace';
  for (const [hz, label] of marks) {
    const x = (hz / SPAN) * W;
    g.strokeStyle = 'rgba(255,170,60,0.30)';
    g.beginPath(); g.moveTo(x, 10); g.lineTo(x, H); g.stroke();
    g.fillStyle = 'rgba(255,190,110,0.85)';
    g.fillText(label, Math.min(W - 26, x + 2), 8);
  }
  // L+R occupies DC..15 kHz — a band rather than a line, so shade it.
  g.fillStyle = 'rgba(255,170,60,0.07)';
  g.fillRect(0, 10, (15000 / SPAN) * W, H - 10);
  g.fillStyle = 'rgba(255,190,110,0.85)';
  g.fillText('L+R', 2, 8);

  if (!mpx.length) return;
  // Auto-range on what is present: injection levels vary and a fixed scale would either
  // clip a loud station or flatten a quiet one.
  let lo = 999, hi = -999;
  for (const v of mpx) { if (v < lo) lo = v; if (v > hi) hi = v; }
  if (hi - lo < 12) { hi = lo + 12; }
  g.strokeStyle = '#7dff9a';
  g.lineWidth = 1;
  g.beginPath();
  for (let i = 0; i < mpx.length; i++) {
    const x = (i / (mpx.length - 1)) * W;
    const y = H - ((mpx[i] - lo) / (hi - lo)) * (H - 12);
    if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
  }
  g.stroke();
}

/** The constellation. Two tight clusters = healthy; a diffuse cloud = buried in noise. */
function drawConstellation() {
  const c = $<HTMLCanvasElement>('rdsConst');
  const g = c.getContext('2d');
  if (!g) return;
  const W = c.width, H = c.height, cx = W / 2, cy = H / 2;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#000';
  g.fillRect(0, 0, W, H);
  // Axes, so the two lobes are read against a centre rather than floating.
  g.strokeStyle = 'rgba(120,200,120,0.22)';
  g.lineWidth = 1;
  g.beginPath();
  g.moveTo(cx, 0); g.lineTo(cx, H); g.moveTo(0, cy); g.lineTo(W, cy);
  g.stroke();
  const xy = rdsExt?.xy ?? [];
  const vEl = $('rdsVerdict');
  const v = constellationVerdict(xy);
  vEl.textContent = v.text;
  vEl.className = v.cls;
  vEl.title = 'How far the received symbols land from where they should — lower is tighter. '
            + 'Known technically as EVM (error vector magnitude).';
  if (!xy.length) return;
  // Points arrive as signed bytes scaled x100; the DSP already normalised by the running
  // RMS, so the SCALE is stable and only the SHAPE changes — which is the part that means
  // something.
  // ★★ DE-ROTATE ONTO THE HORIZONTAL, as every other receiver plots it. Our detector is
  // DIFFERENTIAL — it cancels carrier phase in the arithmetic rather than physically
  // de-rotating the signal — so the constellation arrives tilted by however far our
  // pilot-derived 57 kHz reference sits from the station's actual subcarrier. That tilt is
  // real information (it is the phase error the complex I/Q detection exists to tolerate),
  // but it makes the plot incomparable with SDR++ or a hardware receiver, where a Costas
  // loop has already rotated it flat.
  // BPSK has 180-degree ambiguity, so the angle is estimated by DOUBLING each point's angle
  // — which maps both lobes onto one — averaging, and halving. Magnitude-weighted, so the
  // strong symbols that define the lobes count for more than the noise near the origin.
  const rot = constellationAngle(xy);
  const cr = Math.cos(rot), sr = Math.sin(rot);
  // ★★ SCALE TO THE DATA, never to a constant. A constellation carries its meaning in SHAPE
  // — how tight the lobes are and how far they sit from the centre line — so absolute
  // magnitude is not information, and pinning the scale meant a strong station's points flew
  // clean out of the box and a weak one's huddled invisibly at the origin. Fitting the mean
  // lobe distance to a fixed fraction of the box makes the plot readable at every signal
  // level, which is the whole point of it (Stuart, 2026-07-26).
  const k = constellationScale(xy, W);
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const age = i / xy.length;                    // oldest dimmest
    g.fillStyle = `rgba(120,255,140,${0.25 + 0.6 * age})`;
    const px = cx + (xy[i] * cr - xy[i + 1] * sr) * k;
    const py = cy - (xy[i] * sr + xy[i + 1] * cr) * k;
    // 1px dots at this density: 2px squares merge into blobs and hide the shape.
    g.fillRect(px, py, 1.5, 1.5);
  }
}

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

/** Copy the live canvas onto the visible one (used when viewing live, and on return-to-live). */
function blitToVisible(src: HTMLCanvasElement | null) {
  const c = $<HTMLCanvasElement>('decImage');
  if (!src) return;
  if (c.width !== src.width || c.height !== src.height) { c.width = src.width; c.height = src.height; }
  const ctx = c.getContext('2d');
  ctx?.clearRect(0, 0, c.width, c.height);
  ctx?.drawImage(src, 0, 0);
}

function updateDecImageButtons() {
  const prevBtn = $<HTMLButtonElement>('decPrev');
  const hasPrev = !!decPrevCv;
  // PREV is only meaningful once a completed image has been banked. It flips to LIVE while viewing it.
  prevBtn.style.display = hasPrev ? '' : 'none';
  prevBtn.textContent = decViewingPrev ? 'LIVE' : 'PREV';
  // SAVE is available whenever there is something to save (live has any content, or a prev exists).
  $<HTMLButtonElement>('decSave').style.display = (decLiveCv || decPrevCv) ? '' : 'none';
}

function startDecImage(w: number, h: number) {
  // A new transmission is starting. If the live image was COMPLETED, bank it as PREV so it isn't lost
  // before the user saves it. An incomplete live image (partial, we retuned) is just replaced.
  if (decLiveComplete && decLiveCv) {
    decPrevCv = decLiveCv;
    decLiveCv = null; decLiveCtx = null;
  }
  const cv = document.createElement('canvas');
  cv.width = w || decImgWidth || 800;
  cv.height = h || 600;
  decImgWidth = cv.width;
  decLiveCv = cv;
  decLiveCtx = cv.getContext('2d');
  decLiveCtx?.clearRect(0, 0, cv.width, cv.height);
  decLiveComplete = false;
  if (!decViewingPrev) blitToVisible(decLiveCv);
  updateDecImageButtons();
}

function drawDecLine(y: number, w: number, px: Uint8Array, rgb: boolean) {
  decIsRgb = rgb;
  if (!decLiveCtx || !decLiveCv || decLiveCv.width !== w) startDecImage(w, 0);
  if (!decLiveCtx || !decLiveCv) return;
  const cv = decLiveCv;

  if (y >= cv.height) {                     // grow downward rather than clip
    const keep = decLiveCtx.getImageData(0, 0, cv.width, cv.height);
    cv.height = y + 200;
    decLiveCtx = cv.getContext('2d');
    decLiveCtx?.putImageData(keep, 0, 0);
    if (!decLiveCtx) return;
    if (!decViewingPrev) blitToVisible(cv);
  }

  const img = decLiveCtx.createImageData(w, 1);
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
  decLiveCtx.putImageData(img, 0, y);
  // Mirror the just-drawn line to the visible canvas when we're watching live.
  if (!decViewingPrev) {
    const vis = $<HTMLCanvasElement>('decImage');
    if (vis.width !== cv.width || vis.height !== cv.height) blitToVisible(cv);
    else vis.getContext('2d')?.putImageData(img, 0, y);
  }
}

/** Bank the live image as saveable once it finishes; enable PREV on the next image start. */
function markDecImageComplete() {
  decLiveComplete = true;
  updateDecImageButtons();
}

/** Toggle between the live image and the last completed (previous) image. */
function toggleDecPrev() {
  if (!decPrevCv && !decViewingPrev) return;
  decViewingPrev = !decViewingPrev;
  blitToVisible(decViewingPrev ? decPrevCv : decLiveCv);
  updateDecImageButtons();
}

/** Save the currently-shown image to a PNG download (share sheet where available). */
function saveDecImage() {
  const src = decViewingPrev ? decPrevCv : decLiveCv;
  if (!src) return;
  const name = ($('decTitle').textContent || 'image').toLowerCase().replace(/[^a-z0-9]+/g, '') +
    '_' + new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19) + '.png';
  src.toBlob((blob) => {
    if (!blob) return;
    const file = new File([blob], name, { type: 'image/png' });
    const nav = navigator as Navigator & { canShare?: (d: unknown) => boolean };
    if (nav.canShare?.({ files: [file] })) {
      nav.share?.({ files: [file] }).catch(() => {});
      return;
    }
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }, 'image/png');
}

/** Reset the image buffers — on decoder box open/switch. */
function resetDecImages() {
  decLiveCv = null; decLiveCtx = null; decPrevCv = null;
  decViewingPrev = false; decLiveComplete = false;
  const c = $<HTMLCanvasElement>('decImage');
  c.getContext('2d')?.clearRect(0, 0, c.width, c.height);
  updateDecImageButtons();
}

/** ★ EXPAND / COLLAPSE, as the app has. Collapsed is one line to SCAN; expanded adds the message,
 *  grid, bearing and exact time — context to READ. Remembered between sessions like the filters. */
let spotsExpanded = false;

function renderSpots() {
  const host = $('spotList');
  host.classList.toggle('expanded', spotsExpanded);
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
    const bearing = spotsExpanded ? spotBearingDeg(sp.grid) : null;
    const row = document.createElement('div');
    row.className = 'sres spot';
    // The app's columns: time · band · snr · call · country · distance.
    row.innerHTML =
      `<span class="t">${fmtSpotTime(sp.timestamp)}</span>` +
      `<span class="band" style="color:${BAND_COLOUR[sp.band] || 'var(--text-dim)'}">${escapeHtml(sp.band)}</span>` +
      `<span class="snr ${sp.snr >= 0 ? 'pos' : 'neg'}">${sp.snr}</span>` +
      `<span class="call">${escapeHtml(sp.callsign)}</span>` +
      `<span class="cty">${escapeHtml(abbrCountry(country) || '')}</span>` +
      `<span class="km">${km != null ? Math.round(km) + 'km' : ''}</span>` +
      // ★★ Dimmer than line 1 on purpose: this is context to READ, not a scan target. Parts are
      // joined with · and any missing piece simply DROPS OUT — a report or a 73 carries no locator,
      // so the row must not leave a gap where the grid would have been. (The app's rule, verbatim.)
      (spotsExpanded
        // ★★ NO MESSAGE HERE, and it is not an omission we can fix in the client: the server's
        // spot payload is {mode,callsign,snr,frequency,band,grid,timestamp} — it never sends the
        // decoded text, so the app's "CQ RA3Y KO73" line has no equivalent on the wire. Adding
        // `msg` to digital_spot is a SERVER change; until then this shows what exists.
        ? `<span class="sdetail">${escapeHtml([
            sp.mode, sp.grid,
            bearing != null ? `${Math.round(bearing)}°` : '',
            fmtSpotTimeSec(sp.timestamp),
          ].filter(Boolean).join(' · '))}</span>`
        : '');
    row.title = `${sp.mode} · ${sp.grid || 'no grid'} · ${(sp.frequency / 1e6).toFixed(3)} MHz`;
    row.onclick = () => {
      spec?.tune(clampTune(sp.frequency), 'usb', { recenter: true, retarget: true });
      renderFreq();
    };
    host.appendChild(row);
  }
}

/** Header cyclers. ★★ THE APP NO LONGER CYCLES — v10 replaced these with popup menus ("a step-rate
 *  menu instead of cycling through a list", its own changelog). The comment here said "as in the
 *  app", faithfully copying a behaviour that had already gone (Stuart, 2026-08-01). Left cycling
 *  for now and noted so the next person does not read this as current. */
function initSpotFilters() {
  // ★ EXPAND / COLLAPSE — remembered, like the filters beside it.
  {
    const el = $<HTMLButtonElement>('sfExpand');
    const saved = prefs()['spotsExpanded'];
    spotsExpanded = typeof saved === 'boolean' ? saved : false;
    const paint = () => {
      el.textContent = spotsExpanded ? 'COLLAPSE' : 'EXPAND';
      el.classList.toggle('on', spotsExpanded);
    };
    el.onclick = () => { spotsExpanded = !spotsExpanded; savePref('spotsExpanded', spotsExpanded);
                         paint(); renderSpots(); };
    paint();
  }
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
  const me = myPos();

  // ★★★ PARSE, THEN FILTER — never filter by LENGTH and then assert the parse. `gridToLatLon`
  // is strict (^[A-R]{2}[0-9]{2}([A-X]{2})?$), so plenty of strings pass "length >= 4" and
  // still return null: a 5-character grid, a 7-character one, or junk like "AB1X" out of a
  // corrupt FT8 decode. The old code asserted the result non-null with `!` and then read
  // `p.lat`, so ONE malformed grid anywhere in the list threw a TypeError inside the click
  // handler — and the MAP BUTTON THEN DID NOTHING, silently, for as long as that spot stayed
  // in the list. Closing the map and finding it would not reopen is exactly this
  // (Stuart, 2026-07-27). A bad decode must cost us that one spot, not the whole feature.
  const rows: { s: Spot; p: { lat: number; lon: number } }[] = [];
  let dropped = 0;
  for (const s of filteredSpots()) {
    const p = gridToLatLon(s.grid);
    if (p) rows.push({ s, p }); else if (s.grid) dropped++;
  }
  if (dropped) console.warn(`[map] ${dropped} spot(s) had an unparseable grid`);

  const pts = rows.map(({ s, p }) => {
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
  // Persisted like the other preferences. Turning it back ON does not wait for the next idle
  // period to matter; turning it OFF must un-throttle immediately, or the user sits at 5 fps
  // wondering whether the switch did anything.
  // Waterfall SPEED — on-screen scroll rate (10/20/30). Screen-relative (× dpr inside the waterfall),
  // so render resolution no longer changes the speed.
  for (const b of Array.from($('wfScrollSeg').children) as HTMLButtonElement[]) {
    b.onclick = () => {
      wfScroll = (b.dataset.wfscroll === 'smooth') ? 'smooth' : 'sharp';
      savePref('wfScroll', wfScroll);
      applyWaterfallRates();
    };
  }
  for (const b of Array.from($('wfSpeedSeg').children) as HTMLButtonElement[]) {
    b.onclick = () => {
      wfSpeed = Number(b.dataset.wfspeed);
      savePref('wfSpeed', wfSpeed);
      applyWaterfallRates();
    };
  }
  // Waterfall DATA RATE — server frames/sec (AUTO/20/10/5). Applied immediately. Picking a data rate
  // above the current Speed bumps the Speed up (you can't display slower than you receive).
  for (const b of Array.from($('wfRateSeg').children) as HTMLButtonElement[]) {
    b.onclick = () => {
      wfDataRate = Number(b.dataset.wfrate);
      if (wfDataRate > 0 && wfSpeed < wfDataRate) { wfSpeed = wfDataRate; savePref('wfSpeed', wfSpeed); }
      savePref('wfDataRate', wfDataRate);
      applyWaterfallRates();
    };
  }
  // Restore saved choices, then apply once.
  { const s = prefs().wfSpeed;    if (typeof s === 'number' && [10, 20, 30].includes(s)) wfSpeed = s; }
  { const v = prefs().wfScroll;   if (v === 'sharp' || v === 'smooth') wfScroll = v; }
  { const d = prefs().wfDataRate; if (typeof d === 'number' && [0, 20, 10, 5].includes(d)) wfDataRate = d; }
  if (wfDataRate > 0 && wfSpeed < wfDataRate) wfSpeed = wfDataRate;
  applyWaterfallRates();

  toggle('idleSaver', (on) => {
    if (idleForced) return;         // owner-enforced: the control is locked, not merely ignored
    idleSaver = on;
    if (!on && throttled) { throttled = false; spec?.setFftRate(wantedFps()); updateStatus(); }
  }, 'idleSaver', true);

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
  setupSquelchBar();

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

  // ★★ UNCOMPRESSED AUDIO — the one control in this panel that is NOT a live setter. Every
  // other row talks to the shim's DSP over the open socket; the codec is a query parameter
  // on the audio socket's URL, so it is fixed for that socket's lifetime and changing it
  // means reconnecting. Hence the reload rather than a setter call.
  $('rawAudio').onclick = () => {
    setPrefersRawAudio(!prefersRawAudio());
    location.reload();   // last tune and every other setting are restored on connect
  };
  refreshRawAudioRow();

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
  // ★ MOUSE WHEEL — zoom (0) or tune (1). Remembered, like the app's.
  segment('wheelAction', 'v', (v) => { wheelTunes = v === 1; }, 'wheelAction');
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
    // The split slider does nothing while the trace is hidden, so say so rather than letting it
    // drag with no visible effect.
    const rs = document.getElementById('specRatio') as HTMLInputElement | null;
    if (rs) { rs.disabled = !specOn; rs.closest('.mrow')?.classList.toggle('dim', !specOn); }
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
  if (sql !== undefined) applySquelch(sql, false);
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
  spec.setFftRate(wantedFps());

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
  // The server's capture-rate limit is an UP-TO CEILING, not a lock: keep the picker VISIBLE but
  // offer only rates AT OR BELOW the cap. A listener can still pick lower (narrower span); the shim
  // clamps anything above the cap. (Was: hide the picker entirely — wrong for an up-to cap.)
  const cap = hwLockedRate > 0 ? hwLockedRate : Infinity;
  const rateRow = document.getElementById('rowRate');
  const rateLock = document.getElementById('rateLocked');
  // ★ An HF+ has no picker at all (see applyRadioCaps) — do not let the generic cap logic put it
  //   back the moment hwinfo arrives.
  const ahfPinned = radioCaps?.driver === 'airspyhf';
  // ★★★ A LOCKED CENTRE LOCKS THE RATE. The centre and the rate together ARE the captured
  //     window, so on a shared receiver the operator pins both and the listener gets a view and
  //     a VFO inside it — no hardware control at all. Unlike lockedRate (an up-to ceiling, where
  //     keeping the picker is right), there is nothing to choose here, and a picker the server
  //     refuses outright is worse than none: the user concludes the RADIO is broken rather than
  //     the control (Stuart, 2026-08-02: "still have all the controls though").
  const windowLocked = hwLockedCentre > 0;
  if (rateRow)  rateRow.hidden = ahfPinned || windowLocked;
  if (rateLock) rateLock.hidden = !windowLocked || ahfPinned;
  if (rateLock && windowLocked) {
    const v = rateLock.querySelector('.val');
    const shown = hwRates.length && cap !== Infinity ? cap : 0;
    if (v) v.textContent = shown > 0
      ? `${(shown / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MS/s — set by the server.`
      : 'Set by the server.';
  }

  if (hwRates.length) {
    const r = $<HTMLSelectElement>('rate');
    r.innerHTML = '';
    // ASCENDING. The server advertises 3.2 first, and an unsorted list made the highest
    // rate the one the <select> showed by default — see the default below for why that
    // was not merely untidy.
    for (const rate of [...hwRates].sort((a, b) => a - b)) {
      if (rate > cap) continue;   // up-to cap: never offer a rate above the server's ceiling
      const o = document.createElement('option');
      o.value = String(rate);
      const mhz = `${(rate / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MS/s`;
      // ★★ THE WARNING IS THE DONGLE'S, NOT THE RADIO'S. An RTL-SDR cannot sustain more
      // than 2.4 MS/s over USB — above it the dongle drops samples silently, which is what
      // makes it a trap rather than a trade-off, so those rates are labelled. An RSP is a
      // different radio: it runs 8 MS/s happily, and carrying the dongle's caveat across
      // would warn about the very capability the hardware was bought for (Stuart,
      // 2026-07-26). The server already omits what a given radio cannot sustain — 10 MS/s
      // is absent for an RSP for the same measured reason 3.2 is absent for a dongle — so
      // anything still on offer here is safe on THIS receiver.
      const risky = radioCaps?.driver !== 'sdrplay' && rate > RTL_SAFE_RATE;
      o.textContent = risky ? `${mhz} (may drop samples)` : mhz;
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
  // ★★ DEFAULT ON FOR A DUAL-SIDEBAND MODE, matching the app (2026-07-31). AM/SAM/DSB/FM have a
  // passband that is symmetric about the carrier, so leaving the edges unlinked makes every change
  // a two-step and lets a user lopside a filter that has no reason to be. SSB and CW keep them
  // independent. ★ A SAVED preference still wins — this only changes the starting point.
  const savedSync = prefs().bwSync;
  const symmetricMode = ['am', 'sam', 'dsb', 'fm', 'nfm', 'wfm']
    .includes(String(spec?.mode ?? '').toLowerCase());
  bwSync = typeof savedSync === 'boolean' ? savedSync : symmetricMode;
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
    // ★ toFixed(1) rounded the new 6.25 kHz dPMR/NXDN step to a WRONG "6.3kHz".
    //   Two places, then trim: 12.5 stays "12.5kHz", 6.25 reads "6.25kHz".
    return `${k % 1 === 0 ? k : Number(k.toFixed(2))}kHz`;
  }
  return `${hz}Hz`;
}

// ── Tap = one step, hold = accelerating sweep ────────────────────────────────
//
// The same control law as the app's HiFi tuner keys, with the same constants, so
// a button behaves identically whichever surface you are on (BRIEF-inputs §2).
//
// ★ TAP fires IMMEDIATELY on press, not on release, and ten fast taps are ten
//   steps — no debounce, no accumulation.
// ★ HOLD is the only special case: after HOLD_MS of UNBROKEN contact it begins
//   auto-repeating and accelerates smoothly to a ceiling. Release stops it dead.
//
// ★★ Fast clicks can never be mistaken for a hold, and the guarantee is
// STRUCTURAL rather than a heuristic: the timer is armed on press and cancelled
// on EVERY release, so only one unbroken 350 ms can reach it. Nothing watches
// click frequency and nothing looks across clicks. Do NOT add cross-click
// debouncing — that is precisely what would break "rapid taps = rapid steps".
const HOLD_MS = 350;
const SWEEP_LO = 3;          // steps/sec when the sweep starts
const SWEEP_HI = 22;         // steps/sec ceiling
const SWEEP_RAMP_MS = 2500;  // LO -> HI, a continuous ramp rather than gears

/**
 * @param tap   what one press does — the decisive, familiar amount.
 * @param sweep what each auto-repeat tick does. Usually the same as `tap`, but
 *              zoom deliberately differs: a click wants a decisive octave, while
 *              a sweep wants fine travel, or holding it would cross the whole
 *              range in a blink.
 */
function attachHoldSweep(el: HTMLElement, tap: () => void, sweep: () => void = tap) {
  let holdT: number | null = null;
  let tickT: number | null = null;
  const stop = () => {
    if (holdT !== null) { clearTimeout(holdT); holdT = null; }
    if (tickT !== null) { clearTimeout(tickT); tickT = null; }
    el.classList.remove('sweeping');
  };
  el.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;            // ignore right/middle click
    e.preventDefault();                    // no text selection, no double-tap zoom
    stop();
    tap();
    holdT = window.setTimeout(() => {
      holdT = null;
      el.classList.add('sweeping');
      const started = Date.now();
      const tick = () => {
        sweep();
        // Rate recomputed per tick, so the acceleration is smooth.
        const t = Math.min(1, (Date.now() - started) / SWEEP_RAMP_MS);
        tickT = window.setTimeout(tick, 1000 / (SWEEP_LO + (SWEEP_HI - SWEEP_LO) * t));
      };
      tickT = window.setTimeout(tick, 1000 / SWEEP_LO);
    }, HOLD_MS);
  });
  el.addEventListener('pointercancel', stop);
  el.addEventListener('pointerleave', stop);
  // ★ Release ANYWHERE ends it. Listening only on the element would leave a
  // sweep running forever if the pointer drifted off the button before lifting,
  // which is exactly what happens when you press hard and slide.
  window.addEventListener('pointerup', stop);
}

function buildVfo() {
  const saved = prefs().step;
  if (typeof saved === 'number' && saved > 0) step = saved;
  attachHoldSweep($('tuneDown'), () => nudge(-step));
  attachHoldSweep($('tuneUp'),   () => nudge(step));
  // ★★ WRAPPED, NOT PASSED DIRECTLY. openStepMenu now takes an optional anchor, and an
  //    onclick handler is called WITH THE EVENT — so assigning it bare handed a MouseEvent in
  //    as the anchor, getBoundingClientRect() did not exist on it, the handler threw and the
  //    desktop step button stopped working entirely (Stuart, 2026-08-01). Any function that
  //    grows a first parameter must be re-checked wherever it is used bare as a listener.
  $('stepBtn').onclick  = () => openStepMenu();
  syncStep();
  renderFreq();
}

/** Walk the ladder — kept for the keyboard shortcut, where cycling is the right
 *  gesture because there is nothing to point at. */
function cycleStep() {
  if (!spec) return;
  const steps = stepsForFreq(spec.frequency);
  const i = steps.indexOf(step);
  setStep(steps[(i + 1) % steps.length]);
}

function setStep(v: number) {
  step = v;
  $('stepBtn').textContent = formatStep(step);
  // ★ The card's own step button carries the same label — updating only the desktop one left
  //   the mobile button showing the previous step after every change.
  const m = document.getElementById('mStep');
  if (m) m.textContent = formatStep(step);
  savePref('step', step);
}

/** ★ A MENU, NOT A CYCLE. The ladder has grown to the point where reaching the
 *  step you want means clicking through the ones you don't — and on the HF ladder
 *  that is a lot of clicks to go the wrong way round. A list you point at is the
 *  right control once the options stop being few (Stuart: "bothered me for ages").
 *  ★ The keyboard [ and ] keep cycling: there is nothing to aim at from a key. */
/** ★ ANCHOR IS A PARAMETER because the same popup is opened from two buttons. Anchoring it
 *  to `stepBtn` unconditionally would have measured a HIDDEN element on a narrow window —
 *  the desktop bar is display:none there, so getBoundingClientRect() returns zeros and the
 *  menu lands in the top-left corner instead of on the button you tapped. */
function openStepMenu(anchor?: HTMLElement) {
  if (!spec) return;
  document.getElementById('stepMenu')?.remove();
  const steps = stepsForFreq(spec.frequency);
  const btn = anchor ?? $('stepBtn');
  const r = btn.getBoundingClientRect();

  const m = document.createElement('div');
  m.id = 'stepMenu';
  m.style.cssText = 'position:fixed;z-index:9998;background:#0d0d0d;border:1px solid #ffa000;'
    + 'border-radius:8px;padding:4px;display:flex;flex-direction:column;gap:2px;'
    + 'font:12px/1.4 var(--mono,monospace);box-shadow:0 6px 24px rgba(0,0,0,.6);'
    + 'max-height:60vh;overflow:auto';
  for (const v of steps) {
    const b = document.createElement('button');
    b.textContent = formatStep(v);
    b.style.cssText = 'background:none;border:0;color:' + (v === step ? '#ffe566' : '#ddd')
      + ';padding:7px 14px;text-align:right;cursor:pointer;border-radius:5px;font:inherit';
    b.onmouseenter = () => { b.style.background = 'rgba(255,160,0,.18)'; };
    b.onmouseleave = () => { b.style.background = 'none'; };
    b.onclick = () => { setStep(v); close(); };
    m.appendChild(b);
  }
  document.body.appendChild(m);

  // Anchor ABOVE the button when there is no room below — the VFO sits at the
  // bottom of the window, so "below" is usually off-screen.
  const mh = m.offsetHeight;
  const top = (r.top - mh - 6 > 0) ? r.top - mh - 6 : Math.min(r.bottom + 6, innerHeight - mh - 8);
  m.style.left = `${Math.max(8, Math.min(r.left, innerWidth - m.offsetWidth - 8))}px`;
  m.style.top = `${Math.max(8, top)}px`;

  const close = () => {
    m.remove();
    document.removeEventListener('mousedown', onDoc, true);
    document.removeEventListener('keydown', onKey, true);
  };
  const onDoc = (e: MouseEvent) => { if (!m.contains(e.target as Node)) close(); };
  const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
  setTimeout(() => {
    document.addEventListener('mousedown', onDoc, true);
    document.addEventListener('keydown', onKey, true);
  }, 0);
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

// ★★ A DETENT AT A HARDWARE GAP, not a silent clamp and not a hard wall. An Airspy HF+ tunes
// 0.5 kHz-31 MHz and 60-260 MHz with NOTHING in between — the gap is absent hardware, not a
// weak spot. Three ways to handle it and only one is honest:
//   • allow it — the dial sits on a dead frequency and the radio looks broken;
//   • clamp silently — the dial stops moving with no reason given, which reads as a bug;
//   • BOUNCE, and say why. Tuning into the gap parks you on the edge with a message; tune
//     again in the same direction and you jump to the far side (Stuart's design, 2026-07-27).
// It teaches the shape of the radio instead of hiding it.
let gapNudgeDir = 0;        // which way the last bounce was heading, 0 = not bounced

/** Where the running radio can actually tune, or null when it has no gaps (a dongle). */
function tuneRanges(): [number, number][] | null {
  const r = radioCaps?.ranges;
  return r && r.length > 1 ? r : null;
}

function clampTune(hz: number): number {
  const want = Math.round(hz);
  const ranges = tuneRanges();
  if (!ranges) { gapNudgeDir = 0; return Math.max(MIN_TUNE_HZ, Math.min(MAX_TUNE_HZ, want)); }

  // Inside a real window: nothing to do, and any pending bounce is cancelled.
  for (const [lo, hi] of ranges) if (want >= lo && want <= hi) { gapNudgeDir = 0; return want; }

  // In a gap. Work out which way we were travelling and which edges bracket us.
  const cur = spec?.frequency ?? want;
  const dir = want >= cur ? 1 : -1;
  let below = -Infinity, above = Infinity;
  for (const [lo, hi] of ranges) {
    if (hi < want && hi > below) below = hi;
    if (lo > want && lo < above) above = lo;
  }
  // ★ Second nudge the SAME way = jump the gap. The first parks on the edge and explains; only
  // a deliberate repeat crosses, so a fast scroll cannot fling you into another band by
  // accident.
  if (gapNudgeDir === dir) {
    gapNudgeDir = 0;
    const target = dir > 0 ? above : below;
    if (Number.isFinite(target)) {
      showTuneGapMsg(`Jumped to ${(target / 1e6).toFixed(3)} MHz`);
      return target;
    }
  }
  gapNudgeDir = dir;
  const edge = dir > 0 ? below : above;
  if (!Number.isFinite(edge)) return Math.max(MIN_TUNE_HZ, Math.min(MAX_TUNE_HZ, want));
  const other = dir > 0 ? above : below;
  showTuneGapMsg(Number.isFinite(other)
    ? `${(edge / 1e6).toFixed(3)} MHz is the edge of this radio's range — tune ${dir > 0 ? 'up' : 'down'} again to jump to ${(other / 1e6).toFixed(3)} MHz`
    : `${(edge / 1e6).toFixed(3)} MHz is the edge of this radio's range`);
  return edge;
}

/** ★ Black out the part of the window the radio cannot tune, and say what it is.
 *  Only ever ONE region: the visible span is far narrower than any real gap, so a window can
 *  overlap at most one edge. Handling several would be code for a case that cannot occur. */
function updateRangeGap(centerHz: number, bwHz: number) {
  const el = document.getElementById('rangeGap');
  const note = document.getElementById('rangeGapNote');
  const ranges = tuneRanges();
  if (!el || !note || !ranges || bwHz <= 0) { el?.classList.remove('show'); return; }

  const lo = centerHz - bwHz / 2, hi = centerHz + bwHz / 2;
  // The window's own edges, and the range that contains the middle of it.
  const inRange = ranges.find(([a, b]) => centerHz >= a && centerHz <= b);
  if (!inRange) { el.classList.remove('show'); return; }
  const [rLo, rHi] = inRange;

  let x0 = 0, x1 = 0, msg = '';
  if (hi > rHi) {                       // dead space on the RIGHT
    x0 = (rHi - lo) / bwHz; x1 = 1;
    const next = ranges.filter(([a]) => a > rHi).sort((p, q) => p[0] - q[0])[0];
    msg = next
      ? `${(rHi / 1e6).toFixed(3)} MHz is the top of this range.\nTune up again to jump to ${(next[0] / 1e6).toFixed(3)} MHz.`
      : `${(rHi / 1e6).toFixed(3)} MHz is the top of this radio's range.`;
  } else if (lo < rLo) {                // dead space on the LEFT
    x0 = 0; x1 = (rLo - lo) / bwHz;
    const prev = ranges.filter(([, b]) => b < rLo).sort((p, q) => q[1] - p[1])[0];
    msg = prev
      ? `${(rLo / 1e6).toFixed(3)} MHz is the bottom of this range.\nTune down again to jump to ${(prev[1] / 1e6).toFixed(3)} MHz.`
      : `${(rLo / 1e6).toFixed(3)} MHz is the bottom of this radio's range.`;
  } else { el.classList.remove('show'); return; }

  x0 = Math.max(0, Math.min(1, x0)); x1 = Math.max(0, Math.min(1, x1));
  if (x1 - x0 <= 0.005) { el.classList.remove('show'); return; }   // a sliver is just noise
  el.style.left  = `${x0 * 100}%`;
  el.style.width = `${(x1 - x0) * 100}%`;
  note.textContent = msg;
  el.classList.add('show');
  el.hidden = false;
}

let gapMsgTimer: number | null = null;
function showTuneGapMsg(text: string) {
  const el = document.getElementById('tuneGapMsg');
  if (!el) return;
  el.textContent = text;
  el.classList.add('show');
  if (gapMsgTimer) clearTimeout(gapMsgTimer);
  gapMsgTimer = window.setTimeout(() => el.classList.remove('show'), 4000);
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

/**
 * The card's frequency readout, in the unit the USER chose in the entry panel.
 *
 * ★★★ SPLIT INTO A MAIN AND A FINE PART. The step ladder's finest step is 10 Hz
 * (sdrTypes.STEP_LABELS), and MHz at three decimals resolves to 1 kHz — so a 500 Hz or 100 Hz
 * step changed the radio and not the display. The fine digits carry the resolution the ladder
 * actually offers; the card dims them so the figure stays scannable (see refresh()).
 * ★ Both parts are FIXED WIDTH per unit, because the island is sized by its contents — a readout
 *   that grows a digit moves every control with it (see #mFreq's reservation in index.html).
 */
function cardFreqText(): { main: string; fine: string; unit: string } | null {
  if (!spec) return null;
  const hz = Math.round(spec.frequency);
  if (freqUnit === 'hz') return { main: String(hz), fine: '', unit: 'Hz' };
  if (freqUnit === 'khz') {
    // kHz: three decimals is already 1 Hz, so there is nothing finer to split off.
    return { main: (hz / 1e3).toFixed(3), fine: '', unit: 'kHz' };
  }
  // MHz: 3 decimals reads as kHz (what people quote), and the last two carry 100 Hz and 10 Hz.
  const main = Math.floor(hz / 1e3) / 1e3;
  const fine = Math.abs(hz) % 1000;                    // Hz within the kHz
  return {
    main: main.toFixed(3),
    fine: String(Math.floor(fine / 10)).padStart(2, '0'),
    unit: 'MHz',
  };
}

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

  // ★ Accept whatever decimal separator the keyboard gives. A Dutch (and most European)
  // layout puts a COMMA on `inputmode="decimal"`, and stripping it turned 100,5 into 1005
  // — so the listener was tuned to 1005 MHz having asked for 100.5. Silently wrong, and
  // invisible to anyone testing on a UK/US layout. Both separators present means the comma
  // is a thousands separator (1,234.5) and is dropped instead.
  const normaliseDecimal = (t: string) => {
    const hasComma = t.includes(','), hasDot = t.includes('.');
    if (hasComma && hasDot) return t.replace(/,/g, '');
    if (hasComma) return t.replace(/,/g, '.');
    return t;
  };
  const go = () => {
    const raw = normaliseDecimal($<HTMLInputElement>('freqInput').value);
    const v = parseFloat(raw.replace(/[^\d.]/g, ''));
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
  // Normalise in the field too, so the user SEES a `.` whatever their layout offers.
  $<HTMLInputElement>('freqInput').oninput = (e) => {
    const el = e.target as HTMLInputElement;
    const v = normaliseDecimal(el.value);
    if (v !== el.value) {
      const at = el.selectionStart;
      el.value = v;
      if (at != null) el.setSelectionRange(at, at);   // 1:1 substitution, so the caret holds
    }
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

/** ★★ Render the controls the RUNNING radio actually has.
 *
 *  A dongle has one gain slider. An RSP has an LNA state, a separate IF gain REDUCTION, AGC
 *  over the IF stage, and switchable notches — and showing a dongle's single slider for all
 *  of that is not a simplification but a misrepresentation. It also hid a real fault: the LNA
 *  sat wide open whatever the slider did, flooding the front end (Stuart, 2026-07-26).
 *
 *  ★ THE DIRECTION IS INVERTED, and it is what catches developers out: SDRplay gains are
 *  REDUCTIONS. 20 dB of IF reduction is MAXIMUM gain; 59 dB is minimum. Both the label and
 *  the readout say so, because a slider that silently means the opposite of every other
 *  slider in the app is a trap.
 */
/** SDRplay's own AGC working point. The API defaults to -60, which is a different thing —
 *  see the setpoint note in sdrplay_source.h. */
const AGC_DEFAULT = -30;

let radioCaps: import('./spectrum').RadioCaps | null = null;

// ── Airspy HF+ ───────────────────────────────────────────────────────────────
const AHF_PREFS = { ahfAtt: 'ahf_att', ahfPpb: 'ahf_ppb' } as const;

function ahfSend(msg: Record<string, unknown>) { spec?.send({ type: 'ahf_control', ...msg }); }

/** Push every current HF+ setting. Called when the radio announces itself, so a reconnect or a
 *  server restart restores what the user chose — the same contract pushAllRspSettings has, and
 *  for the same reason: the server rebuilds its state and would otherwise open on defaults
 *  while the panel still showed the operator's choices. */
function pushAllAhfSettings() {
  if (radioCaps?.driver !== 'airspyhf') return;
  ahfSend({
    att:    Number($<HTMLInputElement>('ahfAtt').value),
    lna:    $('ahfLna').classList.contains('on') ? 1 : 0,
    thresh: $('ahfThreshSeg').querySelector('.on')?.getAttribute('data-th') === '1' ? 1 : 0,
    ppb:    Number($<HTMLInputElement>('ahfPpb').value),
  });
  // ★ AGC LAST. It owns the gain path, so sending it first would let it immediately override
  // the manual attenuation we just set — the same ordering trap the RSP's IFGR has.
  ahfSend({ agc: $('ahfAgc').classList.contains('on') ? 1 : 0 });
}

/** Manual gain controls are meaningless while the radio's own AGC owns the front end. Disable
 *  rather than hide: they are still the right controls, just not yours to set at that moment,
 *  and hiding them would make AUTO look like it removed features. */
function renderAhfEnabled() {
  const auto = $('ahfAgc').classList.contains('on');
  $<HTMLElement>('rowAhfAtt').style.opacity = auto ? '0.45' : '1';
  $<HTMLInputElement>('ahfAtt').disabled = auto;
  $<HTMLElement>('rowAhfThresh').style.opacity = auto ? '1' : '0.45';
  for (const b of Array.from($('ahfThreshSeg').children) as HTMLButtonElement[]) b.disabled = !auto;
  renderAhfVals();
}

function renderAhfVals() {
  const att = Number($<HTMLInputElement>('ahfAtt').value);
  const stepDb = radioCaps?.attStepDb ?? 6;
  // ★ Under AGC the slider's number is NOT what the radio is doing — the AGC owns the front end
  //   and libairspyhf has no getter to ask it what it chose. Showing the last MANUAL figure made
  //   a greyed "48 dB" read as 48 dB of applied attenuation when the AGC was actually running
  //   wide open. Say who is in charge instead of quoting a number that is not in effect.
  $('ahfAttVal').textContent = $('ahfAgc').classList.contains('on')
    ? 'set by AGC'
    : `${att * stepDb} dB${att === 0 ? ' · none' : ''}`;
  const ppb = Number($<HTMLInputElement>('ahfPpb').value);
  $('ahfPpbVal').textContent = `${ppb} ppb`;
}

function applyAhfCaps(caps: import('./spectrum').RadioCaps | null) {
  const steps = (caps?.attSteps ?? 9) - 1;
  $<HTMLInputElement>('ahfAtt').max = String(Math.max(0, steps));
  renderAhfVals();
  renderAhfEnabled();
  // The radio has just told us what it is — the moment to tell it what the user last chose.
  pushAllAhfSettings();
}

function initAhfControls() {
  const p = prefs();
  for (const [id, key] of Object.entries(AHF_PREFS)) {
    const v = p[key];
    if (typeof v === 'number') $<HTMLInputElement>(id).value = String(v);
  }
  // ★★ SET THE LABEL AS WELL AS THE CLASS. The class is what pushAllAhfSettings reads and what
  // gets sent to the radio; the TEXT is all the user sees. Restoring one without the other made
  // the button lie — the preamp came back ON, was correctly pushed as ON, and the label still
  // read OFF (Stuart, on reconnect, 2026-07-27). The radio was right and the button was wrong,
  // which is the worse way round: you cannot tell by looking.
  const setToggle = (id: string, on: boolean, onText: string, offText: string) => {
    const el = $(id);
    el.classList.toggle('on', on);
    el.textContent = on ? onText : offText;
  };
  if (typeof p['ahf_agc'] === 'boolean') setToggle('ahfAgc', p['ahf_agc'], 'AUTO', 'MANUAL');
  if (typeof p['ahf_lna'] === 'boolean') setToggle('ahfLna', p['ahf_lna'], 'ON', 'OFF');

  $('ahfAgc').onclick = () => {
    const on = !$('ahfAgc').classList.contains('on');
    $('ahfAgc').classList.toggle('on', on);
    $('ahfAgc').textContent = on ? 'AUTO' : 'MANUAL';
    savePref('ahf_agc', on);
    renderAhfEnabled();
    ahfSend({ agc: on ? 1 : 0 });
    // ★ Re-assert the attenuation on the way OUT of auto: the radio has been moving it, so the
    // slider and the hardware have drifted apart and the slider is the user's intent.
    if (!on) ahfSend({ att: Number($<HTMLInputElement>('ahfAtt').value) });
  };
  $('ahfLna').onclick = () => {
    const on = !$('ahfLna').classList.contains('on');
    $('ahfLna').classList.toggle('on', on);
    $('ahfLna').textContent = on ? 'ON' : 'OFF';
    savePref('ahf_lna', on);
    ahfSend({ lna: on ? 1 : 0 });
  };
  $<HTMLInputElement>('ahfAtt').oninput = () => {
    renderAhfVals();
    const v = Number($<HTMLInputElement>('ahfAtt').value);
    ahfSend({ att: v }); savePref('ahf_att', v);
  };
  $<HTMLInputElement>('ahfPpb').oninput = () => {
    const el = $<HTMLInputElement>('ahfPpb');
    let v = Number(el.value);
    // ★★ A CENTRE DETENT AT ZERO, the same idea as the RSP's AGC target snapping to -30.
    // ±5000 ppb across a few hundred pixels means nudging this control is easy and getting
    // back to exactly 0 is not — Stuart knocked it off centre and could not return
    // (2026-07-27). Zero is the value everyone starts from and the one to come back to when
    // an experiment did not help, so it has to be a gesture rather than a pixel hunt.
    // ★ ±20 ppb is deliberately NARROW. An HF+'s own error runs to hundreds of ppb, so a
    // generous detent would swallow legitimate corrections — this catches a slip, not a
    // deliberate setting. (20 ppb is 2 Hz at 100 MHz.)
    if (Math.abs(v) <= 20) { v = 0; el.value = '0'; }
    renderAhfVals();
    ahfSend({ ppb: v }); savePref('ahf_ppb', v);
  };
  segment('ahfThreshSeg', 'th', (th) => ahfSend({ thresh: th }), 'ahf_thresh');
}


function applyRadioCaps(caps: import('./spectrum').RadioCaps | null) {
  radioCaps = caps;
  const isRsp = caps?.driver === 'sdrplay';
  const isAhf = caps?.driver === 'airspyhf';
  $<HTMLElement>('rspCtls').hidden = !isRsp;
  $<HTMLElement>('ahfCtls').hidden = !isAhf;
  // ★★★ NO SAMPLE-RATE PICKER ON AN HF+. Its rate is fixed at open, because changing it on a
  //     LIVE radio is a path no other SDR client takes: SDR++ (mainline and Brown) grey the
  //     control out while running, gr-osmosdr sets it once at construction, and OpenWebRX is
  //     profile-based and never changes it on the fly. Ours could — and it mis-tuned the radio,
  //     put audio a full span off with an image beside it, and once wedged the USB endpoint hard
  //     enough that the host needed rebooting (2026-08-01/02).
  //     ★ Stuart: "we cannot have users wedge a device that shouldn't really have its sample rate
  //     changed on the fly." The owner still chooses the rate — in the server's config, applied at
  //     startup, which is the same stop-and-start every other client requires.
  //     ★★ HIDDEN, not disabled: a greyed control still reads as an offer, and there is nothing
  //     here for a listener to unlock. The server enforces it too (see the sampleRate handler).
  { const rr = document.getElementById('rowRate'); if (rr) rr.hidden = isAhf;
    const rl = document.getElementById('rateLocked'); if (rl && isAhf) rl.hidden = true; }
  // Dongle-only controls: hidden on anything that is not a dongle, so no inert switches.
  // ★ PPM lives in here, and an HF+ must not show it — it has its own calibration in PARTS
  // PER BILLION, and two frequency-correction controls disagreeing about units is exactly the
  // "which one is real?" confusion the RSP bias-T duplication caused.
  for (const el of Array.from(document.querySelectorAll('.rtlOnly')) as HTMLElement[])
    el.hidden = isRsp || isAhf;
  $('radioName').textContent = caps?.model
    ? (isRsp ? `SDRplay ${caps.model}` : caps.model)
    : (caps?.driver === 'rtl' ? 'RTL-SDR' : '—');
  // The dongle's single gain slider is meaningless on an RSP, and on an HF+ there is no
  // variable gain stage at all — hide it rather than leave a control whose label lies.
  const gainRow = $('gain').closest('.mrow') as HTMLElement | null;
  if (gainRow) gainRow.hidden = isRsp || isAhf;
  const autoRow = $('gainAuto').closest('.mrow') as HTMLElement | null;
  if (autoRow) autoRow.hidden = isRsp || isAhf;
  // ★ THE PROTECTED CONTROLS ARE BUILT HERE, so the lock has to be re-applied here. The
  // Airspy and RSP panels only exist once the radio has announced itself, which happens AFTER
  // the admin state is first resolved — so applying it only at connect left the per-radio
  // controls (calibration especially) enabled on a protected server (Stuart, 2026-07-27).
  refreshAdminRow();
  if (isAhf) { applyAhfCaps(caps); refreshAdminRow(); return; }
  if (!isRsp) return;

  const n = caps?.lnaStates ?? 10;
  const lna = $<HTMLInputElement>('rspLna');
  lna.max = String(n - 1);
  if (typeof prefs()['rsp_lna'] !== 'number')
    lna.value = String(Math.floor((n - 1) / 2));    // mid gain by default, never wide open
  const gr = $<HTMLInputElement>('rspIfGr');
  gr.min = String(caps?.ifGrMin ?? 20);
  gr.max = String(caps?.ifGrMax ?? 59);
  $<HTMLButtonElement>('rspRfNotch').hidden  = !caps?.rfNotch;
  $<HTMLButtonElement>('rspDabNotch').hidden = !caps?.dabNotch;
  $<HTMLButtonElement>('rspBiasT').hidden    = !caps?.biasT;
  renderRspVals();
  // ★ The radio has just told us what it is — which is also the moment to tell it what the
  // user last chose. Covers a server restart, a reconnect, and a fresh page load alike.
  pushAllRspSettings();
}

function rspSend(msg: Record<string, unknown>) {
  spec?.send({ type: 'rsp_control', ...msg });
}

function renderRspVals() {
  const n = radioCaps?.lnaStates ?? 10;
  // Slider position is GAIN; the hardware wants a STATE, which counts the other way.
  const lnaMax = (radioCaps?.lnaStates ?? 10) - 1;
  const pos = Number($<HTMLInputElement>('rspLna').value);
  const lna = lnaMax - pos;
  const gr  = Number($<HTMLInputElement>('rspIfGr').value);
  // ★ Say which END is more gain, every time. "LNA 3" means nothing on its own.
  // Show the state too — an FM-DXer comparing against SDRuno or SDRconnect wants the actual
  // LNA state, not a slider position we invented.
  $('rspLnaVal').textContent =
    `${pos}/${lnaMax} · LNA ${lna}${lna === 0 ? ' · max' : lna === lnaMax ? ' · min' : ''}`;
  $('rspIfGrVal').textContent = `${gr} dB${gr <= 20 ? ' · max gain' : gr >= 59 ? ' · min gain' : ''}`;
  const sp = Number($<HTMLInputElement>('rspAgcSet').value);
  // ★ Say which way it drives. "-45 dBfs" alone tells nobody whether that is more or less.
  // ★ Name the default where it sits. -30 dBFS is SDRplay's OWN working point (the API's
  // default is -60, which is a different and much gentler thing), so it is the value a user
  // will want to come back to after experimenting — and a number means nothing without
  // knowing where home is (Stuart, 2026-07-26).
  $('rspAgcSetVal').textContent =
    `${sp} dBfs${sp === AGC_DEFAULT ? ' · default' : sp >= -25 ? ' · hard' : sp <= -60 ? ' · gentle' : ''}`;
}

/** Glide the IF thumb to a new AGC value. Status arrives at 5 Hz; a slider that teleports
 *  between readings looks like a glitch rather than a loop settling. */
let ifGrTween = 0;
function tweenIfGr(target: number) {
  const gr = $<HTMLInputElement>('rspIfGr');
  const from = Number(gr.value);
  if (Math.abs(target - from) < 0.5) { gr.value = String(target); return; }
  cancelAnimationFrame(ifGrTween);
  const t0 = performance.now();
  const step = (t: number) => {
    const k = Math.min(1, (t - t0) / 180);
    gr.value = String(from + (target - from) * k);
    if (k < 1) ifGrTween = requestAnimationFrame(step);
  };
  ifGrTween = requestAnimationFrame(step);
}

/** ★★ RSP settings are REMEMBERED and RE-SENT on connect.
 *
 *  They were UI-only, so a server restart brought the radio back on its defaults while the
 *  panel still showed the positions you had chosen — the controls and the hardware quietly
 *  disagreeing, which is worse than losing them outright because nothing looks wrong
 *  (Stuart, 2026-07-26: "all the gains revert", "resets back to -60 whilst showing -30").
 *  ★ And the UI defaults now MATCH what the server opens with. A control whose resting
 *  position differs from the radio's is lying before anyone has touched it.
 */
const RSP_PREFS = {
  lna: 'rspLna', ifgr: 'rspIfGr', agcset: 'rspAgcSet',
} as const;
const RSP_TOGGLES = {
  ifagc: 'rspIfAgc', rfnotch: 'rspRfNotch', dabnotch: 'rspDabNotch', biast: 'rspBiasT',
} as const;

/** Push every current RSP setting to the server. Called whenever a radio announces itself,
 *  so a reconnect or a server restart restores what the user chose. */
function pushAllRspSettings() {
  if (radioCaps?.driver !== 'sdrplay') return;
  const lnaMax = (radioCaps?.lnaStates ?? 10) - 1;
  rspSend({
    lna:      lnaMax - Number($<HTMLInputElement>('rspLna').value),
    agcset:   Number($<HTMLInputElement>('rspAgcSet').value),
    rfnotch:  $('rspRfNotch').classList.contains('on') ? 1 : 0,
    dabnotch: $('rspDabNotch').classList.contains('on') ? 1 : 0,
    biast:    $('rspBiasT').classList.contains('on') ? 1 : 0,
  });
  // ★ AGC last, and the IF reduction only when it is OFF — the server refuses a manual
  // IFGR while the AGC owns the register, so sending them the other way round would drop
  // the value silently.
  const agcOn = $('rspIfAgc').classList.contains('on');
  rspSend({ ifagc: agcOn ? 1 : 0 });
  if (!agcOn) rspSend({ ifgr: Number($<HTMLInputElement>('rspIfGr').value) });
}

function initRspControls() {
  const p = prefs();
  // Restore before wiring, so nothing fires a send with a stale value.
  for (const [key, id] of Object.entries(RSP_PREFS)) {
    const v = p[`rsp_${key}`];
    if (typeof v === 'number') $<HTMLInputElement>(id).value = String(v);
  }
  for (const [key, id] of Object.entries(RSP_TOGGLES)) {
    const v = p[`rsp_${key}`];
    const on = typeof v === 'boolean' ? v : key === 'ifagc';   // AGC defaults on
    $(id).classList.toggle('on', on);
  }

  const lna = $<HTMLInputElement>('rspLna');
  const gr  = $<HTMLInputElement>('rspIfGr');
  lna.oninput = () => {
    renderRspVals();
    const lnaMax = (radioCaps?.lnaStates ?? 10) - 1;
    rspSend({ lna: lnaMax - Number(lna.value) });   // slider is gain, hardware wants state
    savePref('rsp_lna', Number(lna.value));
  };
  gr.oninput  = () => {
    renderRspVals(); rspSend({ ifgr: Number(gr.value) }); savePref('rsp_ifgr', Number(gr.value));
  };
  const sp = $<HTMLInputElement>('rspAgcSet');
  sp.oninput = () => {
    // ★ A SOFT DETENT AT THE DEFAULT. Dragging near -30 snaps to it, so getting back to
    // SDRplay's working point is a gesture rather than a pixel-hunt. Narrow enough (±2 dB)
    // that it never fights someone deliberately choosing -28 or -32.
    let v = Number(sp.value);
    if (Math.abs(v - AGC_DEFAULT) <= 2) { v = AGC_DEFAULT; sp.value = String(v); }
    renderRspVals(); rspSend({ agcset: v }); savePref('rsp_agcset', v);
  };
  const toggle = (id: string, key: string) => {
    const b = $<HTMLButtonElement>(id);
    b.onclick = () => {
      const on = !b.classList.contains('on');
      b.classList.toggle('on', on);
      rspSend({ [key]: on ? 1 : 0 });
      savePref(`rsp_${key}`, on);
      // Turning AGC OFF hands the IF reduction back, so send the slider's value with it.
      if (key === 'ifagc' && !on) rspSend({ ifgr: Number($<HTMLInputElement>('rspIfGr').value) });
    };
  };
  for (const [key, id] of Object.entries(RSP_TOGGLES)) toggle(id, key);
}

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
    // ★★ WHEEL = ZOOM **OR** TUNE. The app has offered this choice for a while and this client did
    // not, so the same gesture did different things depending on which one you were in front of —
    // reaching for the wheel to tune and getting a zoom instead (Stuart, 2026-08-01).
    // ★ Scroll UP means IN / UP-BAND in both modes, matching the app's `dir` convention exactly, so
    // the direction never reverses between the two.
    if (wheelTunes) {
      nudge((e.deltaY < 0 ? 1 : -1) * step);   // `step` is the tuning step the user already chose
      return;
    }
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
      // ★ T = taller/shorter, but ONLY while the panel that it resizes is open. A shortcut
      // that does nothing visible is worse than no shortcut: the user cannot tell whether
      // they pressed the wrong key or the app is broken (Stuart, 2026-07-26).
      case 't': case 'T':
        if (rdsPanelOpen()) { $('rdsSize').click(); e.preventDefault(); }
        break;
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
