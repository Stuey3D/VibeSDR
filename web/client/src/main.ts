/**
 * main.ts — VibeSDR web client entry.
 *
 * Talks to the VibeServer shim running on a phone. Same origin when served by
 * the shim itself (GET /), so the WS URLs are relative; in dev the splash takes
 * an explicit host:port.
 */

import { SpectrumClient, MODE_BANDWIDTHS, type SDRMode, type DabState } from './spectrum';
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
  loadServerBookmarks, getServerBookmarks, saveToServer, removeFromServer, setTunableWindow,
  setBookmarkAdminAuth,
} from './search';
import { parseBookmarksAny } from '../../../src/services/userBookmarks';
import { DecoderClient, type Spot } from './decoders';
import { initChat, chatOpened, onSaid as chatSaid, onDial as chatDial,
         onDialRefused as chatRefused, chatAvailable } from './chat';
import { initAdmin, closeAdmin, openAdmin, startAdminTicketRenewal } from './admin';
import { httpBase, wsBase } from './origin';
import { saveAdminTicket, getAdminTicket, clearAdminTicket, inAdminMode, adminTicketQuery } from './adminticket';

/** True when THIS process is the front door — it owns no radio, so START and the PIN are
 *  meaningless here and ADMIN means 'unlock every radio', not 'connect as admin'. */
/** ★★ SEEDED FROM THE PAGE, NOT FROM THE FETCH. The front-door process stamps
 *  data-frontdoor on <html> as it serves the bundle, so the answer is already here at
 *  first paint and the splash never draws the single-radio controls it is about to hide.
 *  showSplashRadios() still sets it from /vibeserver/radios — that is the authority, and
 *  the only source an older, unstamped server has. */
let isFrontDoor = document.documentElement.hasAttribute('data-frontdoor');

/** ★★★ ADMIN ON SCREEN MEANS "YOU PROVED IT ON THIS PAGE", NOT "A TICKET EXISTS SOMEWHERE".
 *  sessionStorage survives a reload, so a ticket from an earlier sign-in made the banner and the
 *  admin buttons appear on a page where nobody had typed anything — and the renewal timer kept
 *  that alive for the whole life of the tab (Stuart, 2026-08-08: "I've not entered the password
 *  yet, the admin mode title nor the setup and admin controls buttons should be visible").
 *  ★★ The TICKET still persists, because it has one job the flag cannot do: carry proof across the
 *  navigation from the landing page into /r/<serial>/, which is a fresh page load. So the
 *  credential outlives the page and the CLAIM does not. */
let adminSignedInThisView = false;
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

/** ★★ Demodulators the OWNER has switched off (hwinfo.blocked). Lower-cased on arrival.
 *  ★ The server publishes 'cw' for the CW family while the client offers cwu/cwl, so a blocked
 *  'cw' must take both — a half-applied block leaves one dead button, which is the exact failure
 *  hiding them is meant to prevent. */
let blockedModes = new Set<string>();
function isModeBlocked(m: string): boolean {
  const k = m.toLowerCase();
  if (blockedModes.has(k)) return true;
  if ((k === 'cwu' || k === 'cwl') && blockedModes.has('cw')) return true;
  return false;
}
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
    hostEl.value = location.host + BASE_PATH;
  }
  if (saved[hostEl.value]) pinEl.value = saved[hostEl.value];

  // ★ Set by showSplashRadios() once it knows what this process is. The front door owns no radio,
  //   which changes what the ADMIN button means (see adminSignIn).
  const paintAdminMode = () => {
    const on = adminSignedInThisView && inAdminMode();
    const b = document.getElementById('splashAdminBanner');
    if (b) (b as HTMLElement).hidden = !on;
    const tools = document.getElementById('splashAdminTools');
    if (tools) (tools as HTMLElement).hidden = !on;
    const btn = document.getElementById('btnAdmin');
    if (btn && btn.textContent !== 'CANCEL ADMIN') btn.textContent = on ? 'SIGN OUT' : 'ADMIN';
  };

  // ★★ ADMIN AT THE GATE. Reveals the password field and turns CONNECT into an admin connect.
  //    A second press hides it again, so it cannot be left armed by accident.
  const adminRowEl = $('gateAdminRow');
  const adminPwEl  = $<HTMLInputElement>('gateAdminPw');
  /** Set once the operator has been told they would be displacing a listener. */
  let adminConfirmed = false;

  /**
   * ★★★ ON THE FRONT DOOR, SIGNING IN AS ADMIN IS NOT A CONNECTION.
   *
   *     The door owns no radio, so the old path — resolve the password, then connect() — had
   *     nothing to connect to and failed with "Server refused the connection" every time. Worse,
   *     it could not have worked even if it had a radio: the admin handshake is a per-PROCESS
   *     nonce, so a credential proved at the door is rejected by every radio (see
   *     vibe_admin_ticket.h).
   *
   *     So here the password unlocks the PAGE: prove it once, take a ticket that every radio on
   *     this machine will accept, and let the owner pick a radio — including one that is in use.
   */
  const adminSignIn = async (): Promise<boolean> => {
    const pw = adminPwEl.value;
    if (!pw) { msg.className = ''; msg.textContent = 'Enter the admin password.'; return false; }
    msg.className = 'info'; msg.textContent = 'Checking…';
    try {
      const q = await resolveAdminOverride(httpBase(location.host), pw);
      if (!q) { msg.className = ''; msg.textContent = 'This server has no admin password set.'; return false; }
      const r = await fetch(`${httpBase(location.host)}/vibeserver/admin-ticket?${q}`, { cache: 'no-store' });
      if (!r.ok) {
        msg.className = '';
        // ★ 401 here means the password was wrong OR this address is in the brute-force lockout.
        //   Saying "wrong password" during a lockout sends the owner round in circles retyping a
        //   password that is perfectly correct.
        msg.textContent = r.status === 401
          ? 'Wrong admin password — or too many attempts; wait a moment and try again.'
          : 'This server cannot issue an admin session.';
        return false;
      }
      const j = await r.json();
      saveAdminTicket(String(j.ticket || ''), Number(j.ttl) || 600);
      if (!inAdminMode()) { msg.className = ''; msg.textContent = 'The server issued no admin session.'; return false; }
      adminSignedInThisView = true;
      // Keep it for the admin PAGE too, which signs its own requests.
      adminPassword = pw;
      setBookmarkAdminAuth(async () => resolveAdminOverride(httpBase(location.host), adminPassword));
      paintAdminMode();
      void showSplashRadios();     // busy radios become reachable the moment we are admin
      msg.className = 'ok';
      msg.textContent = 'ADMIN MODE — pick a radio. You can take over one that is in use.';
      adminRowEl.hidden = true;
      $('btnAdmin').textContent = 'ADMIN';
      return true;
    } catch {
      msg.className = ''; msg.textContent = 'Could not reach the server.';
      return false;
    }
  };
  // ★ Straight to the tools, without taking a radio. Both work from the ticket alone — the admin
  //   API accepts it (see vibe_admin_ticket.h), and the setup page is served by this same process.
  document.getElementById('btnSplashAdmin')?.addEventListener('click', () => {
    if (!adminSignedInThisView || !inAdminMode()) return;
    // ★ The landing page never runs startApp(), so nothing has wired the panel yet. Idempotent.
    initAdmin(() => location.host, () => adminPassword);
    // ★ Renewal runs for as long as the TAB holds a ticket, not only while the admin panel is
    //   open — see startAdminTicketRenewal. The panel-scoped version let the lease lapse during
    //   ordinary listening, which is when it matters most.
    startAdminTicketRenewal();
    openAdmin(location.host, adminPassword);
  });
  document.getElementById('btnSplashSetup')?.addEventListener('click', () => {
    if (!adminSignedInThisView || !inAdminMode()) return;
    // ★ Hand the ticket over so setup does not ask for a password that was typed seconds ago.
    location.href = `${location.origin}/setup?${adminTicketQuery()}`;
  });

  $('btnAdmin').addEventListener('click', () => {
    // ★ Already signed in: the button becomes SIGN OUT. Leaving no way out meant the only way to
    //   drop admin was to close the tab, and an owner who has finished should not have to.
    if (adminSignedInThisView && inAdminMode()) {
      clearAdminTicket();
      adminSignedInThisView = false;
      adminPassword = '';
      paintAdminMode();
      $('btnAdmin').textContent = 'ADMIN';
      msg.className = ''; msg.textContent = 'Signed out of admin.';
      void showSplashRadios();
      return;
    }
    const showing = !adminRowEl.hidden;
    adminRowEl.hidden = showing;
    $('btnAdmin').textContent = showing ? 'ADMIN' : 'CANCEL ADMIN';
    adminConfirmed = false;
    if (showing) adminPwEl.value = ''; else adminPwEl.focus();
    msg.textContent = showing ? ''
      : (isFrontDoor ? 'Enter the admin password to unlock every radio on this server.'
                     : 'Connecting as admin: no time limit, all controls unlocked.');
    msg.className = showing ? '' : 'info';
  });

  // ★★★ THE BANNER MUST FOLLOW THE TRUTH, NOT THE LAST BUTTON PRESS. Painted only on sign-in and
  //     sign-out, it was stale both ways — still claiming ADMIN MODE after the ticket lapsed. It
  //     claims real powers, so it has to take itself down.
  // ★★ AND IT LIVES HERE, in initSplash, beside the function it calls. It was briefly called from
  //    startApp() instead — a DIFFERENT SCOPE, so it threw ReferenceError and killed the rest of
  //    startApp: audio and RDS came up (they are wired earlier) and the waterfall and frequency
  //    never did. A blank receiver that plays sound is a confusing way to learn about a typo.
  paintAdminMode();
  setInterval(paintAdminMode, 5000);

  const go = async (remember: boolean) => {
    // ★★★ THE FRONT DOOR HAS NOTHING TO CONNECT TO. Pressing Enter in the admin box ran the whole
    //     connect path against a process that owns no radio, which is why it always ended in
    //     "Server refused the connection". Here, signing in IS the action: it unlocks the page and
    //     the radio cards below become the way in.
    if (isFrontDoor && !adminRowEl.hidden) { await adminSignIn(); return; }
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
          const r = await fetch(`${httpBase(host)}/vibeserver.json`, { cache: 'no-store' });
          const j = await r.json();
          // ★★★ CLAIMABLE IS NOT BUSY. Past the guarantee the slot belongs to whoever wants it,
          //     so warning "connecting will disconnect them" describes the SERVER'S OWN RULE as
          //     if it were the operator's doing — and makes an ordinary arrival feel like an
          //     eviction (Stuart, 2026-08-20). The card says FREE for exactly this case; this
          //     gate has to agree with it.
          busy = j.busy === true && j.claimable !== true;
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
      const q = await resolveAdminOverride(httpBase(host), adminPwEl.value).catch(() => '');
      if (!q) {
        msg.className = ''; msg.textContent = 'This server cannot be given an admin password.';
        return;
      }
      sessionStorage.setItem('vsAdminOverride', q);
      // ★ The second press of CONNECT, after being told plainly that it disconnects the current
      //   listener, is the deliberate act — so this connection may displace them. A first press on
      //   a FREE receiver never gets here with adminConfirmed set, and displaces nobody.
      if (adminConfirmed) sessionStorage.setItem('vsTakeover', '1');
      // ★ Keep the password for the admin PAGE, which has to sign its own requests. Without
      //   this an owner who connected as admin from the splash arrived fully unlocked and
      //   then could not open the admin page without retyping it.
      adminPassword = adminPwEl.value;
      setBookmarkAdminAuth(async () => resolveAdminOverride(httpBase(host), adminPassword));
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

  // ★★★ ?join — OPEN THE RECEIVER, NOT THE FRONT DOOR.
  //
  //     On a machine with several radios every process serves this same landing page, so a card
  //     linking to another radio's address landed the listener on ANOTHER list of radios — and the
  //     card for the radio serving the page just reloaded it (Stuart, 2026-08-08: "the landing
  //     page just reloads"). A radio card has to mean "let me listen to THIS one".
  // ★ Same path as pressing START, so there is one way in and not two that can drift.
  if (new URLSearchParams(location.search).has('join')) {
    // ★★ SUBMIT THE FORM, DO NOT CALL go() DIRECTLY, AND WAIT FOR LOAD. Calling go() from here ran
    //    it mid-initialisation — before the rest of this module had finished wiring the page up —
    //    and the first thing it touched that did not exist yet threw, leaving a WHITE SCREEN with
    //    no error a user could see (Stuart, 2026-08-08: "all of them just went to a white screen").
    // ★ requestSubmit() goes through the exact path pressing START does, so there is one way in.
    const joinNow = () => {
      try { $<HTMLFormElement>('connForm').requestSubmit(); }
      catch { go(false); }   // very old browsers: fall back, still after load
    };
    if (document.readyState === 'complete') setTimeout(joinNow, 0);
    else window.addEventListener('load', () => setTimeout(joinNow, 0), { once: true });
  }
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
    const ri = await fetch(`${httpBase(host)}/vibeserver.json`, { cache: 'no-store' });
    const ji = await ri.json();
    if (ji.admin === true) $('btnAdmin').hidden = false;
  } catch { /* not a VibeServer, or unreachable — leave the button hidden */ }
  try {
    const r = await fetch(`${httpBase(host)}/vibeserver/auth`, { cache: 'no-store' });
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
/** ★★ The receiver runs ONE dial that everybody hears (an unlocked radio with room for several).
 *  What changes here is not what you may do — anybody may tune — but what happens WITHOUT you
 *  doing anything: nothing at all. See the restore in onConfig. */
let srvSharedDial = false;
let adminUnlocked = false;
/** ★★ THE ADMIN PASSWORD, IN MEMORY ONLY, for the duration of this tab.
 *  The admin API signs every request with HMAC(password, fresh nonce), so the page genuinely
 *  needs the secret itself — a stored nonce would work exactly once.
 *  ★★★ NOT localStorage, NOT sessionStorage, NOT a cookie. It lives in this closure and dies
 *  with the tab, which is the same rule the override credential follows. Anything persisted is
 *  a credential left lying around on a machine that, by definition, is used by whoever walks
 *  up to it — which is the very thing the idle re-lock exists to defend against.
 */
let adminPassword = '';
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
  srvSharedDial = false;
  try {
    const r = await fetch(`${httpBase}/vibeserver.json`, { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    if (j.uncompressed === 'choice' || j.uncompressed === 'compat' || j.uncompressed === 'off')
      srvUncompressed = j.uncompressed;
    srvLocal = j.local === true;
    srvAdminProtected = j.admin === true;
    /* ★★ THE SERVER DECIDES WHETHER DAB IS OFFERED. Only it knows the EFFECTIVE limits — the
     *  tunable set after allow/block lists and the rate the receiver will actually run at — so a
     *  V4 locked to FM or held below 2.048 MS/s never draws the button at all. */
    dabCapable = j.dab === true;
    // ★★★ IS THE DIAL SHARED? Needed BEFORE the first config arrives, because the very first
    //     thing this client does with a config is restore the frequency it was last on — and on a
    //     shared dial that would drag the whole room to somebody's remembered station the instant
    //     they connected (Stuart, 2026-08-20). Read from the same probe that already tells us
    //     about audio and admin, so it costs nothing and cannot arrive late.
    srvSharedDial = j.tuneMode === 'open';
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
  // ★★★ THE PASSWORD BOX FOLLOWS THE TICKET, NOT THE SOCKET. Hiding it whenever `adminUnlocked`
  //     was true meant an owner whose PAGE TICKET had expired — while the socket unlock was still
  //     perfectly valid — was told by the admin button to enter a password and given nowhere to
  //     type it. Two credentials with two lifetimes, and the UI assumed one implied the other
  //     (Stuart, 2026-08-14: "asking for a password I cannot enter until I refresh the page").
  const pwRowNow = document.getElementById('adminPwRow');
  if (pwRowNow) pwRowNow.hidden = adminUnlocked && inAdminMode();
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
  // ★★★ hrfAmp AND hrfBiasT ARE IN HERE DELIBERATELY. Stuart: "the preamp control MUST be
  //     treated with the same caution as the Bias-T and password protected so a stranger cannot
  //     enable the preamp and blow it up... Unlike the Airspy the HackRF preamp is extremely
  //     sensitive." It is the U14 amp, the part of a HackRF that most commonly dies.
  for (const id of ['ppm', 'biasT', 'directSampling', 'ahfPpb', 'rspBiasT', 'hrfAmp', 'hrfBiasT']) {
    const el = document.getElementById(id) as HTMLInputElement | null;
    if (!el) continue;
    el.disabled = locked;
    const rowEl = el.closest('.mrow') as HTMLElement | null;
    if (rowEl) rowEl.style.opacity = locked ? '0.45' : '1';
  }
  // ★ The RSP's gain controls follow a DIFFERENT rule to the list above — they are gated on the
  //   receiver being LOCKED (shared front end), not merely on a password existing. Unlocking is
  //   the moment they should appear, so re-apply it here.
  if (radioCaps?.driver === 'sdrplay') applyRspLock();
  if (radioCaps?.driver === 'hackrf')  applyHrfLock();
  // ★★ The SERVER ADMIN door. Present only for an unlocked owner — a listener has no use for
  //    it, and a button whose only job is to refuse you is worse than no button.
  //    ★ It also has to DISAPPEAR on the idle re-lock, which is why this lives here rather than
  //      being set once when the password is accepted.
  const srv = document.getElementById('adminServerRow');
  if (srv) srv.hidden = !adminUnlocked;
  // ★★ THE BOOKMARK WRITE BUTTONS. Saving to the receiver changes it for everyone who connects
  //    afterwards, so it belongs with the other protected controls. The server enforces this
  //    (vsAdminHttpOk); hiding them here is so a listener is not offered a button that will be
  //    refused — an inert control reads as a broken feature.
  //    ★ Only where there is something to protect: on a server with no admin password nothing is
  //      gated, and hiding them would remove a working feature from a personal receiver.
  {
    const gated = srvAdminProtected && !adminUnlocked;
    for (const id of ['bmImportServer', 'bmAddServer']) {
      const el = document.getElementById(id) as HTMLButtonElement | null;
      if (el) el.hidden = gated;
    }
  }
  // ★★★ AND THE PAGE ITSELF MUST CLOSE. Leaving it open after a re-lock would leave DISCONNECT
  //     and BLOCK buttons on screen that the server will now refuse — the exact "drawn, enabled
  //     and inert" trap the unlock row's own comment warns about, on the most consequential
  //     controls in the whole client.
  if (!adminUnlocked) closeAdmin();
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
  return sendAdminUnlock(pw);
}

/**
 * Prove admin to the SERVER. Separate from doAdminUnlock's guard on purpose — see the reconnect
 * caller below, which must be able to re-prove while the UI already believes we are admin.
 */
async function sendAdminUnlock(pw: string) {
  if (!pw) return;
  try {
    // ★ The same challenge-response the PIN uses, and the same nonce endpoint. The password
    // never crosses the wire, and reusing the scheme means it inherits the server's existing
    // brute-force lockout rather than needing its own.
    const ch = await fetchAuthChallenge(httpBase(currentHost));
    const nonce = ch.nonce;
    if (!nonce) { alert('This server did not offer a challenge.'); return; }
    adminPassword = pw;   // the admin page signs its own requests with it
    // ★ Bookmark writes are admin-gated on the server, so they need the same credential.
    setBookmarkAdminAuth(async () => resolveAdminOverride(httpBase(currentHost), adminPassword));
    spec?.send({ type: 'admin_unlock', nonce, token: vibeAuthToken(pw, nonce) });
  } catch (e) {
    alert(`Could not reach the server to unlock: ${(e as Error).message}`);
  }
}


/** ★★★ ONE ID PER VISIT, NOT PER PAGE LOAD.
 *
 *  This was `uuid()`, minted fresh every time the page ran. On a multi-radio server that is once
 *  per RADIO: the landing page, into the Airspy, back, into the RSP — each is a navigation, so one
 *  person having a look around arrived in the log as three separate sessions. Stuart, 2026-08-11:
 *  "if I go into the Airspy and then use the back button and then switch to the SDRPlay that
 *  should only count 1 session. If I full close the page then come back then it should count as
 *  multiple sessions."
 *
 *  ★★ sessionStorage IS EXACTLY THAT RULE, and it is why it is used rather than a cookie or
 *     localStorage: it survives reloads and same-tab navigation, and the browser clears it when
 *     the TAB closes. So "wandering between radios" keeps one id and "closing the page and coming
 *     back" earns a new one — with no expiry to tune and no state kept on the server.
 *  ★ Per TAB, which is also right: two tabs are two occupants and must not share an id, or one
 *    would be mistaken for the other's audio socket.
 *  ★ Falls back to a fresh uuid where storage is unavailable (private modes, embedded webviews).
 *    Losing the grouping is a worse log; throwing is a broken receiver.
 */
function visitSessionId(): string {
  try {
    const k = 'vsVisitSession';
    const had = sessionStorage.getItem(k);
    if (had) return had;
    const made = uuid();
    sessionStorage.setItem(k, made);
    return made;
  } catch { return uuid(); }
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
  // ★★★ FOLLOW THE PAGE'S OWN SCHEME. These were hardcoded to http:// and ws://, so a server put
  //     behind an HTTPS reverse proxy served an https page whose auth fetch, config fetch and
  //     every WebSocket were plain http/ws — which the browser BLOCKS as mixed content. The
  //     receiver then failed to connect with nothing wrong at the proxy, and it reads as a broken
  //     server (Saber, 2026-08-08: "cant use my vibeserver publicly ... doesnt follow the http
  //     scheme from the URL already set").
  // ★★ The test is the PAGE's protocol, not the host's: an https page may not load ANY http
  //    subresource, wherever it points. On a plain http page both stay as they were.
  const httpBaseUrl = httpBase(host);
  const wsBaseUrl = wsBase(host);

  let auth: AuthState;
  try {
    auth = await resolveAuth(httpBaseUrl, pin);
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
    // ★★ AND THE ADMIN TICKET, which is how an owner who signed in at the FRONT DOOR arrives here
    //    as admin — the nonce above was proved to a different process and this radio has never
    //    heard of it. Folded in the same way so it rides BOTH sockets: the spectrum socket claims
    //    the slot (evicting an occupant if the owner confirmed that), and the audio socket must be
    //    recognised as the same admin or it would be refused as a second listener.
    // ★ NOT consumed: an admin session has to survive moving between radios and reloading.
    const tq = adminTicketQuery();
    if (tq) auth = { ...auth, query: auth.query ? `${auth.query}&${tq}` : tq };

    // ★★★ AND WHETHER WE ASKED TO INTERRUPT ANYBODY. The server evicts the occupant whenever a
    //     connection arrives holding admin — which is right for a takeover and wrong for a
    //     reconnect, because the credential above is REPLAYED every time. Two of the owner's own
    //     clients on one radio then took it off each other for as long as both were open.
    // ★★ One-shot and CONSUMED: set by the IN USE take-over box and by confirming a takeover at
    //    the splash, both of which are a finger on a button. Everything else — a reload, a
    //    reconnect, moving between radios — says 0 and displaces nobody, while still arriving as
    //    admin.
    // ★ It has to survive a reload, because taking over IS a reload (see doAdminOverride), which
    //   is why it lives in sessionStorage rather than in a variable.
    const wantTakeover = sessionStorage.getItem('vsTakeover') === '1';
    sessionStorage.removeItem('vsTakeover');
    const tk = `vs_takeover=${wantTakeover ? '1' : '0'}`;
    auth = { ...auth, query: auth.query ? `${auth.query}&${tk}` : tk };
  }

  // ★ ONE session id, on BOTH sockets. The server treats a session as a single occupant, and a
  // browser opens two sockets (spectrum + audio) — without a shared id the audio socket looks like
  // a different client and the occupancy check would reject a client's own audio. Two browsers or
  // two devices get two ids, which is exactly what we want to keep apart.
  // ★★ AND ONE ID FOR THE WHOLE VISIT — see visitSessionId().
  const sid = visitSessionId();
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
  const specUrl  = `${wsBaseUrl}${withAuth('/ws/user-spectrum?user_session_id=' + sid + '&mode=binary8&bins=1024', auth)}`;
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
  await loadAudioPolicy(httpBaseUrl);
  const forceOpus = new URLSearchParams(location.search).has('opus');
  const wantRaw = !forceOpus && (srvLocal || (srvUncompressed === 'choice' && prefersRawAudio()));
  const wantOpus = !wantRaw && await AudioPlayer.supportsOpus();
  const audioUrl = `${wsBaseUrl}${withAuth('/ws/audio?user_session_id=' + sid + (wantOpus ? '&codec=opus' : ''), auth)}`;
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
  // ★ The owner's notice had to outrank the splash to be seen at all; on the receiver it must
  //   outrank NOTHING, or it covers the panels. See showOwnerNotice.
  const on = document.getElementById('ownerNotice');
  if (on) on.style.zIndex = '48';

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
      // ★★ PAINT THE CARD'S METER ON THE FRAME, not on the card's 250 ms poll. Everything else in
      //    that poll describes something a human changes (frequency, mode, recording) and 4 Hz is
      //    plenty; the meter describes the BAND, and at 4 Hz nineteen of every twenty readings the
      //    server sends were thrown away — which is both the lag and the jerkiness, since a bar
      //    stepping four times a second cannot look smooth however well it is smoothed.
      mobileUi?.paintSignal();
      updateRangeGap(centerHz, bwHz);
    },
    onConfig: (cfg) => {
      // ★★ ALSO HERE, not only in onHwInfo. lockedWindow() needs the capture bandwidth, which
      //    arrives with the CONFIG — and the two messages have no guaranteed order, so setting it
      //    on one alone leaves the filter unset whenever that one lands first. Cheap and
      //    idempotent, so run it on both rather than reasoning about which wins.
      setTunableWindow(lockedWindow());
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
        // ★★ A RETURNING VISITOR KEEPS THEIR DIAL; a new one lands where the OWNER said.
        //    Stuart, 2026-08-05: "its fine when the user has visited the server before, the
        //    settings are remembered like they are in the app — this is for new users and users
        //    who have cleared their cookies."
        //    ★ cfg.centerFreq is the VIEW centre, not the VFO. On a locked receiver the view sits
        //      at the locked centre, so falling back to it parked a first-time visitor on the
        //      middle of the band instead of the owner's landing frequency. Same precedence the
        //      line below already uses for mode: remembered > server > client default.
        // ★★★ CLAMPED, because a REMEMBERED frequency is from a DIFFERENT TIME. The owner may have
        //     moved the window since — Stuart changed this receiver's centre four times in an
        //     evening — so the last place you were listening can be nowhere this receiver now
        //     reaches. Restored blind, it put the dial on 10 kHz on a 2.8-10.8 MHz receiver: the
        //     server clamped the audio to the edge and the readout went on claiming 10 kHz, which
        //     is the one thing on screen a listener cannot check (2026-08-06).
        // ★★★ ON A SHARED DIAL, ARRIVING IS NOT TUNING. Restoring a remembered frequency is a
        //     move nobody made: the listener has just connected and said nothing, and everybody
        //     already here would hear the station change under them. So a joiner ADOPTS the dial
        //     as it stands and the memory is simply not applied — it is still saved, and it still
        //     applies on every receiver where the dial is your own.
        //  ★ The landing frequency is not applied either, for the same reason: it is where a NEW
        //    SESSION starts, and the server already knows whether this is one (it applies the
        //    landing itself when nobody is listening). Sending it from here would re-park a busy
        //    receiver on the owner's default.
        const adopt = srvSharedDial ? (cfg.serverVfo ?? cfg.centerFreq) : (last?.hz ?? cfg.serverVfo ?? cfg.centerFreq);
        spec!.frequency = clampTune(adopt);
        // Server's mode wins over the client's built-in default on a fresh visit — the owner can
        // set the starting demodulator, and defaulting to nfm showed the wrong mode + a thin NFM
        // passband until the user clicked. A remembered session still wins over both.
        const initialMode = (srvSharedDial ? (cfg.serverMode ?? spec!.mode)
                                           : (last?.mode ?? cfg.serverMode ?? spec!.mode)) as SDRMode;
        // ★ `false` on a shared dial: setMode's second argument SENDS the mode to the server, and
        //   the mode is shared here too — adopting what the radio is already doing must not be
        //   broadcast back to it as a change.
        setMode(initialMode, !srvSharedDial && !!last);
        renderFreq();
        if (srvSharedDial) { /* adopted above — say nothing, move nothing */ }
        else if (last) spec!.tune(clampTune(last.hz), last.mode, { recenter: true });
        // ★ A first-time visitor must be TUNED to the landing frequency, not merely shown it —
        //   setting spec.frequency only moves the dial readout; the demodulator has to be told.
        //   (Not on a shared dial: there the server has already placed the radio, and this client
        //   has just adopted it.)
        else if (cfg.serverVfo) spec!.tune(clampTune(cfg.serverVfo), initialMode, { recenter: true });
      }
    },
    onSummon: () => onSummoned(),
    onBusy: (q) => showBusy(q),
    // ★ Our turn. The slot is reserved for this session for a few seconds only, and a reload is
    //   the honest claim: it re-runs the preflight and opens both sockets cleanly, exactly as the
    //   admin-override path does. Anything cleverer races the reservation window for no gain.
    onYourTurn: () => {
      showRefusal('YOUR TURN', 'A slot has freed up and is being held for you.<br><br>Connecting…', false);
      setTimeout(() => location.reload(), 600);
    },
    onEvicted: () => showEvicted(),
    onBanned: () => showBanned(),
    // ★ Drop audio that was demodulated at the old frequency. Without this the previous station
    //   plays on for as long as the buffer is deep — which is what made tuning feel laggy.
    // ★ And the waterfall rows held for pacing, for the same reason: they were computed at the
    //   OLD centre, so releasing them after the dial moves paints the previous frequency at the
    //   new one's position. Both buffers flush together or the two displays disagree.
    onRetuneJump: () => { audio?.flush(); if (!NO_WF) wf?.flushHeld(); },
    onAdminRelocked: (idleMin) => showAdminRelocked(idleMin),
    onAdminSuperseded: () => {
      // ★★ AND LET THE TICKET GO. Keeping it left this tab believing it was still admin the next
      //    time it reconnected — it would re-present a credential the server had already moved to
      //    somebody else, and the badge would come back on for a session that has no admin rights.
      //    Losing admin has to STAY lost until an owner deliberately unlocks again.
      clearAdminTicket();
      showPill('Admin taken by a more recent login elsewhere — you are still listening');
    },
    // ★ Named, and phrased as a choice rather than a refusal — the visitor already has a radio,
    //   and closing it frees this one immediately. No countdown: there is nothing to wait for.
    onElsewhere: (radio: string) => showRefusal('ALREADY LISTENING',
      `You are already listening on <b>${radio}</b> from this address.<br><br>`
      + 'This receiver serves one radio per listener, so that nobody takes them all. '
      + 'Close the other one and this will let you straight in.'),
    // ★★ THE SOFT LIMIT'S ONE WARNING. A pill, not an overlay: the listener has not been refused
    //    anything and is still hearing the radio — putting a modal over it would take away the
    //    seconds the notice exists to give them.
    // ★★★ ASKED, NOT KICKED — and the answer is any use of the radio at all. The button is a
    //     convenience, not the mechanism: tuning, changing mode or zooming clears it server-side
    //     just as well, because the server counts a control message as evidence a human is here.
    // ★★ AN OVERLAY IS RIGHT HERE, unlike the soft-limit pill: this one needs an answer, and a
    //    notice that can be missed is exactly what would disconnect somebody who was listening.
    onIdleCheck: (secs) => showIdleCheck(secs),
    onIdleClosed: () => showRefusal('DISCONNECTED',
      'This receiver releases a listener who has gone quiet, so somebody else can use it.'
      + '<br><br>Nothing is wrong — press Try again to carry on listening.'),
    onHandover: (secs) => showPill(
      `Someone is waiting for this radio — you have about ${Math.max(1, secs)} seconds`, 12000),
    onHandoverOff: () => showPill('They stopped waiting — the radio is still yours', 6000),
    onSessionEnded: (cd, fresh) => showSessionEnded(cd, fresh),
    onCooldown: (secs) => showCooldown(secs),
    // ★ Shown to EVERYONE, not only the admin. "3 of 30 listening" answers the question a
    //   visitor actually has — is there room, and is anyone else here — and it is the number the
    //   owner wanted at a glance. `busy` was a yes/no built for a one-at-a-time receiver.
    onUsers: (n, max) => {
      listenerCount = n; listenerMax = max;
      const el = document.getElementById('rxUsers');
      if (el) el.textContent = n > 0
        ? `${n} listening${max > 1 ? ` of ${max}` : ''}`
        : '';
    },
    // ── ★★ THE SHARED DIAL. Only a receiver running open or spectator tuning sends these; on an
    //    ordinary one this never fires, and the buttons stay in the disabled state they start in.
    onDial: (d) => {
      chatDial(d);
      const on = d.mode !== 'exclusive';
      for (const id of ['chatBtn', 'mChat']) {
        const b = document.getElementById(id) as HTMLButtonElement | null;
        if (!b) continue;
        // ★★★ DISABLED, NOT HIDDEN (Stuart, 2026-08-20). A vanishing button collapsed the right
        //     stack to one cell and left portrait with three buttons where the layout wants four,
        //     so the island changed shape depending on which receiver you were on. Greying keeps
        //     one geometry everywhere.
        b.disabled = !on;
        // ★★ And it must say why it is grey, or "permanently disabled" reads as "broken". The
        //    reason belongs here, beside the condition that causes it — not in a static title
        //    that would be wrong half the time.
        b.title = on
          ? 'Ask about the dial — canned messages only'
          : 'Chat is only on shared-dial receivers, where several people share one tuner';
      }
    },
    onDialRefused: () => { chatRefused(); togglePanel('chatPanel'); },
    onSaid: (from, id, admin) => chatSaid(from, id, admin),
    // ★★★ SOMEBODY ELSE MOVED THE DIAL — REDRAW WHAT THEY MOVED. The readout, the mode and the
    //     VFO marker are all drawn from values this client chose, and on a shared receiver it
    //     chose none of them. Without this the audio followed and the screen did not.
    onDialMoved: (hz, mode) => {
      renderFreq();
      if (mode && mode !== undefined) setMode(mode, false);   // ★ false: adopting, not commanding
      updateViewOverlays();
      // ★ Say it on the strip, briefly. A frequency that changes on its own reads as a fault
      //   unless something on screen accounts for it — the strip is where the room's state lives,
      //   so it is where "moved to 96.600" belongs (the brief asks for exactly this).
      const strip = document.getElementById('dialStrip');
      if (strip && !strip.hidden) {
        const mhz = (hz / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
        const was = strip.textContent || '';
        strip.textContent = `moved to ${mhz} MHz`;
        setTimeout(() => { if (strip.textContent === `moved to ${mhz} MHz`) strip.textContent = was; }, 2500);
      }
    },
    onSessionWarning: (secs) => setTimeLeft(secs),
    onDevice: (present) => showDeviceBanner(present),
    // ★ Pushed the instant the owner posts one — the people already watching the spectrum
    //   misbehave are exactly who it is for.
    onNotice: (text: string) => showOwnerNotice(text),
    /* ★ DAB arrives twice a second with the whole station list — see BRIEF-dab.md for why it is
     *  sent entire rather than as deltas. A null means the server left DAB mode. */
    onDab: (d: DabState) => { dabState = d ?? null; if (!d) dabOn = false; dabRender(); },
    onAdmin: (ok, refused) => {
      if (refused) {
        adminUnlocked = false;
        // ★★★ AND PUT THE PASSWORD BOX BACK. Unlocking HIDES this row, and this early return
        //     never un-hid it — so after the idle re-lock the owner was told to unlock by the
        //     admin page while the menu offered no way to do it. A path that hides a control on
        //     success must un-hide it on the way back, or the way back does not exist.
        const pwRowBack = document.getElementById('adminPwRow');
        if (pwRowBack) pwRowBack.hidden = false;
        refreshAdminRow();
        return;
      }
      adminUnlocked = ok;
      // ★ An admin session has no time limit on the server, so it must have no countdown here.
      if (ok) clearTimeLeft();
      // ★ Never leave the password sitting in the box, whichever way it went.
      const pwIn = document.getElementById('adminPwInput') as HTMLInputElement | null;
      if (pwIn) pwIn.value = '';
      const pwRow = document.getElementById('adminPwRow');
      if (pwRow) pwRow.hidden = ok;    // wrong password: leave it open to try again
      if (!ok) setStatus('error', 'admin');
      refreshAdminRow();
      if (!ok) alert('That admin password was not accepted.');
    },
    // ★★ THE SERVER'S WORD ON ITS OWN DSP. These controls are GLOBAL and sticky —
    // whatever the last listener left is what the radio is doing — so rendering our
    // saved prefs showed NR OFF while it was audibly ON, and only dragging the
    // slider resynced it (Stuart, on MW, 2026-07-28). A control that misreports the
    // radio is worse than a missing one: nothing tells you to look.
    // ★★★ AND IT IS EVERY STICKY CONTROL, not the two that happened to be reported first.
    // This handler took `(nr, notch)` for a fortnight while the auto notch, the RSP's RF and
    // DAB notches, the bias-T and the NR STRENGTH all went on lying in exactly the same way
    // (Stuart, 2026-08-10, having left auto notch on for the Airspy and found the RSP showing
    // it OFF while it was on). Anything the SERVER remembers belongs in this message.
    onDspState: (s) => {
      const nrEl = document.getElementById('nr') as HTMLInputElement | null;
      if (nrEl) {
        // ★★ THE SERVER'S STRENGTH WINS WHEN IT HAS ONE. Rendering `on` but keeping OUR saved
        //    number drew a figure the radio was not using — the switch agreed and the value
        //    lied, which is harder to spot than the switch being wrong (Stuart, 2026-08-10).
        //    Falls back to the saved strength only when the server never had one.
        const want = !s.nr ? 0
          : s.nrStrength !== undefined ? Math.max(1, Math.round(nrPercent(s.nrStrength)))
          : Math.max(1, Number(prefs()['nr']) || 50);
        if (Number(nrEl.value) !== want) {
          nrEl.value = String(want);
          nrEl.dispatchEvent(new Event('input'));
        }
      }
      setToggleTo('notch', s.notch, 'notch');
      // ★ Sticky, so it must be RESTORED from the server's own report — the button has to say
      //   what the radio is doing, not what this tab last asked for.
      /* ★★★ THE LABEL GOES THROUGH setToggleTo TOO — IT MUST OBEY THE SAME PRESS GUARD. These
       *      were two statements: setToggleTo set the HIGHLIGHT and the next line set the TEXT.
       *      The guard that protects a just-pressed button from a stale report only covered the
       *      first, so inside that window the highlight froze while the label followed the old
       *      server value — a button lit up reading OFF. Stuart: "its lit up like its on but it
       *      says its off, i suspect a typo." Not a typo: two writers, one guarded.
       *  ★ One call per control now, so the pair cannot come apart again. */
      setToggleTo('wspBtn', s.wsp, 'wsp', (on) => on ? 'NR ON'  : 'NR OFF');
      setToggleTo('imsBtn', s.ims, 'ims', (on) => on ? 'IMS ON' : 'IMS OFF');
      setToggleTo('ceqBtn', s.ceq, 'ceq', (on) => on ? 'CEQ ON' : 'CEQ OFF');
      setToggleTo('nbBtn',  s.nb,  'nb',  (on) => on ? 'NB ON'  : 'NB OFF');
      // ★ AUTO BW is decided once for the whole receiver (it changes the demodulator, not a
      //   per-listener effect), so the server's word is the only truth here.
      // ★ ONLY WHEN THE SERVER ACTUALLY STATED IT. `!!s.autobw` turned "not reported" into "off"
      //   — see the note in onDspState. An older server that says nothing leaves the button alone.
      if (typeof s.autobw === 'boolean') {
        srvAutoBw = s.autobw;
        setToggleTo('autoBwBtn', s.autobw, 'autobw',
                    (on) => on ? 'AUTO BW ON' : 'AUTO BW OFF');
      }
      // ★ The RSP front-end notches, which are sticky in exactly the same way and were the
      //   other half of the same report. Absent = the server has no opinion; leave the
      //   control alone rather than inventing an "off" it never said.
      // ★★★ ONLY WHEN WE ARE NOT THE ONES SETTING THEM. On an UNRESTRICTED receiver the
      //     client is the source of truth: applyRadioCaps() has just called
      //     pushAllRspSettings() to restore the owner's saved front end, and it did so from
      //     the SAME hwinfo message we are reading here — so this snapshot was taken BEFORE
      //     that push landed. Rendering it would draw the pre-push state over the values we
      //     have just commanded, i.e. swap one lie for another. On a restricted or shared
      //     receiver we push nothing, the server's word is the only truth, and that is
      //     precisely the case the bug was reported on.
      if (rspRestricted()) {
        if (s.rfNotch  !== undefined) setToggleTo('rspRfNotch',  s.rfNotch,  'rsp_rfnotch');
        if (s.dabNotch !== undefined) setToggleTo('rspDabNotch', s.dabNotch, 'rsp_dabnotch');
        if (s.rspBiasT !== undefined) setToggleTo('rspBiasT',    s.rspBiasT, 'rsp_biast');
      }
      // ★ The DONGLE's bias-tee is a different button on a different radio, and it is not
      //   behind the RSP gate — see the two-bias-tees note in spectrum.ts.
      if (s.biasT !== undefined) setToggleTo('biasT', s.biasT, 'biasT');
    },
    // ★ Rebuild the mode buttons whenever the server tells us what it allows. It arrives with
    //   hwinfo, i.e. AFTER the controls are first built, so this must re-render rather than
    //   assume it can filter at build time.
    onBlockedModes: (list) => {
      blockedModes = new Set(list.map(m => m.toLowerCase()));
      // If we are somehow already ON a blocked mode (an owner switched it off mid-session),
      // move to the first one that is still allowed rather than leaving a dead selection.
      if (spec?.mode && isModeBlocked(String(spec.mode))) {
        const first = MODES.find(m => !isModeBlocked(m));
        if (first) setMode(first, true);
      }
    },
    onHwInfo: (gains, rates, locked, maxFps, forceIdle, radio, lockedCentre, gainCap, agcLocked, gainLocked, ifGrFloor,
               gainNow, agc, ovlSteps, adcPeak) => {
      hwGains = gains; hwRates = rates; hwLockedRate = locked;
      hwGainNow = typeof gainNow === 'number' ? gainNow : -1;
      hwAgcOn = agc === true;                       // ★ the live flag the chip reads through
      maybeExplainRtlAutomation();   // ★ we may only now know the radio is running its own gain
      maybeShowGainMinNotice();
      /* ★★★ AND RENDER IT INTO THE BUTTON. Removing the client's push (see pushOnInit) stopped it
       *     overriding the owner's saved config — but without this half the button then showed
       *     whatever this browser last stored, which after a reload was OFF on a radio whose AGC
       *     was ON. Stuart pressed it to "turn it on" and thereby turned it off, every time:
       *     "all those settings that were meant to be on still had to be set manually after a
       *     page refresh." A control that must not COMMAND from storage has to READ from the
       *     radio, or it just lies more quietly than before. Both halves or neither. */
      setToggleTo('agc', hwAgcOn, 'agc');
      // ★ Paint the chip from STATE, so it is right on arrival and after a reload — not only after
      //   the loop happens to move while you are watching.
      {
        const chip = $('ovlChip');
        const dB = (hwGainNow / 10).toFixed(1);
        const pk = pkText();
        if (agc && hwGainNow >= 0) {
          chip.textContent = `AGC ${dB} dB${ifText()}${pk}`;
          chip.classList.add('set', 'easing');
        } else if ((ovlSteps ?? 0) > 0 && hwGainNow >= 0) {
          chip.textContent = `GAIN HELD ${dB} dB${pk}`;
          chip.classList.add('set', 'easing');
        } else if (!agc) {
          // ★ CLEAR THE TEXT, not just the styling. Removing the classes left the previous
          //   "AGC … dB" sitting on screen — the readout Stuart saw with the AGC switched off.
          chip.textContent = '';
          chip.classList.remove('set', 'easing', 'fault');
        }
      }
      // ★★★ THE BUTTON FOLLOWS THE RADIO, NOT ONLY THE MOUSE. Its "on" class was set ONLY by a
      //     click, so a server that already has the AGC running greeted every listener with the
      //     button OFF and the gain slider live — and they had to switch on a thing that was
      //     already on to find out it was. Stuart, 2026-08-21: "when the AGC switch is enabled in
      //     the GUI I still have to enable it manually in the menu of the client".
      //  ★★ hwinfo has carried `agc` all along; nothing was reading it. This is the same fault as
      //     the gain slider before it — a control drawn from local state rather than from what the
      //     receiver actually reports, which is guaranteed to be wrong for everybody but the person
      //     who last touched it.
      //  ★ Only when the server actually says: `agc` is absent from radios that have no such loop,
      //    and a missing field must not read as "off".
      if (typeof agc === 'boolean') {
        $<HTMLButtonElement>('gainAuto').classList.toggle('on', agc);
        $<HTMLInputElement>('gain').disabled = agc;
      }
      hwGainCap = typeof gainCap === 'number' ? gainCap : -1;
      hwAgcLocked = agcLocked === true;
      hwGainLocked = gainLocked === true;
      hwIfGrFloor = typeof ifGrFloor === 'number' ? ifGrFloor : -1;
      applyIfGainCap();
      // ★ Say WHY it cannot be turned off, where the hand is already going. Locked is the owner's
      //   decision, not a fault, and an unexplained dead control reads as the latter.
      {
        const ga = $<HTMLButtonElement>('gainAuto');
        ga.classList.toggle('locked', hwAgcLocked);
        ga.title = hwAgcLocked
          ? "VibeAGC \u2014 VibeSDR's own AGC for RTL-SDR. The owner has locked it on for this receiver."
          : "VibeAGC \u2014 VibeSDR's own AGC for RTL-SDR. It watches the ADC for overload and moves the tuner "
            + "gain to suit. The dongle's built-in AGC is unreliable and is never used.";
      }
      // ★ Re-applied on EVERY hwinfo, because the server re-sends it when the ceiling changes —
      //   which is how the slider follows the radio down on tuning into a limited band.
      applyGainCap();
      applyHrfGainCap();   // ★ hwinfo carries the cap, and it CHANGES as the listener tunes bands
      applyGainLocked();   // ★ ...and so does the LOCK, which is per band for the same reason
      hwLockedCentre = lockedCentre ?? 0;
      // ★ Search is narrowed to what this receiver can actually reach — see setTunableWindow().
      //   Set from here because this is where the lock becomes known, and re-set on every hwinfo
      //   so a server whose window changes does not leave the filter describing the old one.
      setTunableWindow(lockedWindow());
      // ★★★ AND CATCH ANYTHING THAT GOT OUT. Clamping each call site fixes the paths we thought
      //     of; this fixes the ones we did not. The window only becomes KNOWN when hwinfo arrives,
      //     which can be after a session has already been restored — so the guard belongs here,
      //     where the answer first exists, rather than being duplicated at every tune.
      //     ★ Silent when nothing is wrong, which is almost always: this only fires when the dial
      //       is genuinely outside a window the server would refuse anyway.
      const win = lockedWindow();
      if (win && spec && spec.frequency && (spec.frequency < win[0] || spec.frequency > win[1])) {
        const to = Math.max(win[0], Math.min(win[1], spec.frequency));
        console.warn(`[tune] ${(spec.frequency / 1e6).toFixed(3)} MHz is outside this receiver's `
                   + `${(win[0] / 1e6).toFixed(3)}–${(win[1] / 1e6).toFixed(3)} MHz — moved to `
                   + `${(to / 1e6).toFixed(3)}`);
        spec.tune(to, undefined, { recenter: true, retarget: true });
        renderFreq();
      }
      // ★★★ THE HEAVY HALF RUNS ONLY WHEN SOMETHING STRUCTURAL CHANGED — AND THAT IS THE STUTTER.
      //     hwinfo now arrives on EVERY AGC gain move, and this handler rebuilt the radio controls
      //     and the gain list in the DOM each time: `r.innerHTML = ''` and a fresh <option> per
      //     gain step, on the MAIN THREAD. That is the thread feeding the audio decoder, so each
      //     rebuild delayed Opus packets and the decoder CONCEALED the gap — and packet-loss
      //     concealment is precisely what we kept measuring: 40-60 ms (two or three 20 ms frames)
      //     of attenuated, mono-collapsed audio fading back in. It appears in a RECORDING because
      //     the recording taps the decoded stream, after concealment.
      //  ★★★ WHICH IS WHY EVERY SERVER-SIDE COUNTER STAYED CLEAN. No IQ was dropped, no lock was
      //      contended, no frame was lost, the sink never ran dry. Stuart got there from the
      //      symptom — "I wonder if simply animating the slider was the issue" — after a build
      //      that simply stopped sending the announce cut the artefacts from ten in 35 s to three
      //      in 110 s.
      //  ★★ A gain MOVE changes no structure: same radio, same gain list, same rates, same caps.
      //     Only the VALUE moves, and that is a slider position and a chip — cheap, and done
      //     unconditionally above. The rebuild is for a genuinely different radio.
      //  ★ Compared by content, not identity: these arrive freshly parsed from JSON every time, so
      //    a reference check would never match and this would rebuild for ever.
      const hwSig = JSON.stringify([gains, rates, locked, maxFps, forceIdle, radio ?? null,
                                    lockedCentre, gainCap, agcLocked]);
      if (hwSig === lastHwSig) return;
      lastHwSig = hwSig;
      applyRadioCaps(radio ?? null);
      // THE OWNER'S FRAME-RATE CEILING. Honour it rather than asking for more and being silently
      // clamped: a client that keeps requesting 20 fps and keeps receiving 10 has no way to tell
      // a capped server from a failing link, and the difference matters.
      serverMaxFps = maxFps > 0 ? maxFps : 0;
      // ★★★ ONLY WHEN IT ACTUALLY CHANGES. hwinfo now arrives on every AGC gain move, and this
      //     line re-sent the rate each time — which made the server rebuild this listener's
      //     pipeline, resetting stereo and RDS and dipping the audio for ~40 ms. A restatement is
      //     not a request. (The server refuses a no-op rate too; both ends, deliberately.)
      if (serverMaxFps > 0 && spec) {
        const fps = wantedFps();
        if (fps !== lastSentFps) { lastSentFps = fps; spec.setFftRate(fps); }
      }
      // ★ The owner REQUIRES idle saving (a solar/cellular host, where power outranks a listener's
      // preference). Force it on and lock the control, saying who set it — the same courtesy as a
      // pinned sample rate. Never leave a switch on screen that we would silently ignore.
      applyForcedIdle(forceIdle === true);
      applyRateOptions();
      populateHw();
    },
    // ★★★ THE DONGLE'S PROTECTION, SAID OUT LOUD. The RSP gets a hardware overload flag and shows
    //     a lamp; the RTL has no flag, so we measure it — and because we also ACT on it, the chip
    //     says what happened rather than only that something is wrong. Stuart's wording:
    //     "Overload: Gain ↓" while it is backing off, "Overload Passed: Gain ↑" as it recovers.
    //  ★★ IT CLEARS ITSELF once the gain is all the way back to the owner's value. A chip that
    //     stayed up would become part of the furniture, and this one is meant to be noticed.
    //  ★ The recovering state is calmer than the fault state on purpose: going up is good news,
    //    and it must not read as a second alarm.
    onOverload: (steps: number, dir: number, gainTenthDb: number, agc: boolean,
                 adcPeak?: number) => {
      const chip = $('ovlChip');
      const dB = (gainTenthDb / 10).toFixed(1);
      const pk = pkText();
      // ★★★ THE MOVEMENT IS THE NEWS; THE RESTING PLACE IS THE READOUT. While the loop is stepping,
      //     the chip says WHICH WAY — that is a thing happening, and worth an alarm's colours going
      //     down. Once it settles it stops shouting and simply says where the gain ended up
      //     (Stuart, 2026-08-21: "once it is settled then it needs to change ... it can show the
      //     new gain rate").
      //  ★★ In AGC mode the settled state is PERMANENT and that is correct: the gain moving is the
      //     normal condition of an AGC, so a readout is honest where a warning would be a lie you
      //     learn to ignore. Under manual protection it goes quiet instead, because there the
      //     normal condition is "the gain is where you put it".
      window.clearTimeout(ovlClearTimer);
      chip.textContent = (dir > 0 ? `GAIN ↑ ${dB} dB` : `OVERLOAD: GAIN ↓ ${dB} dB`) + pk;
      chip.classList.toggle('easing', dir > 0);
      chip.classList.add('set');
      ovlClearTimer = window.setTimeout(() => {
        // ★ hwAgcOn, NOT the captured `agc` — see its declaration.
        if (hwAgcOn) {
          // Settled, and the AGC owns the gain from here — show it, quietly.
          chip.textContent = `AGC ${dB} dB${ifText()}${pk}`;
          chip.classList.add('easing');
        } else if (steps > 0) {
          // Still holding the gain below what was asked for: say so, or the slider and the radio
          // appear to disagree with nobody explaining why.
          chip.textContent = `GAIN HELD ${dB} dB${pk}`;
          chip.classList.add('easing');
        } else {
          chip.textContent = '';                    // ★ text too, or the old state lingers
          chip.classList.remove('set', 'easing', 'fault');
        }
      }, 4000);
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
      const agcOn = $<HTMLButtonElement>('rspIfAgc').classList.contains('on');
      applyRspLock();          // ★ who OWNS the slider — never decided by this message alone
      if (agcOn) {
        // ★ Telemetry MOVES the thumb; it does not decide who owns it. Watching the reduction
        //   ride up and down is the only evidence a listener has that the AGC is alive.
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
    /* ★★★ LIGHTNING. The server sends this for THIS listener — it is already gated on the band
     *   their own VFO sits in and on whether the activity amounts to a storm, so a non-zero rate
     *   is the whole decision and there is nothing to re-judge here. Anything else would be a
     *   client deciding something only the server can know.
     * ★ The rate rides in the TOOLTIP rather than the chip. The chip answers "what are those
     *   lines?", which is the question somebody actually has; the number is for whoever looks. */
    onLightning: (ratePerMin: number, agoSecs: number) => {
      const el = document.getElementById('rxLightning');
      if (!el) return;
      const on = ratePerMin > 0;
      el.hidden = !on;
      if (on) {
        const ago = agoSecs >= 0 && agoSecs < 90 ? `, last ${Math.round(agoSecs)}s ago` : '';
        el.title = 'Lightning nearby — the broadband lines across the spectrum and the jumps in '
                 + 'the noise floor are sferics, not a fault with the receiver '
                 + `(about ${Math.round(ratePerMin)}/min${ago})`;
      }
    },
    onTunerBw: (hz: number, rfCentreHz: number, auto: boolean) => {
      hwTunerBw = hz;
      hwTunerAuto = auto;
      // ★ The IF-filter mode arrives separately from caps, so this is often the moment the
      //   explanation becomes possible to write correctly.
      maybeExplainRtlAutomation();
      const sel = document.getElementById('tunerBw') as HTMLSelectElement | null;
      /* ★ In Auto the picker stays on "Auto" while the WIDTH underneath moves with the zoom —
       *   showing the derived number would make the control look as though it were being changed
       *   by someone else every time the listener zoomed. The width is on the status chip, which
       *   is a readout and not a control. */
      if (sel && auto) sel.value = '-1';
      else if (sel && String(hz) !== sel.value) {
        // ★ If the server reports a width the list does not offer (an owner set it in the config),
        //   show it rather than silently snapping the picker to something else.
        if (![...sel.options].some(o => o.value === String(hz))) {
          const o = document.createElement('option');
          o.value = String(hz);
          o.textContent = hz > 0 ? `${(hz / 1e3).toFixed(0)} kHz` : 'Wide — set by sample rate';
          sel.appendChild(o);
        }
        sel.value = String(hz);
      }
    },
    onAdcStat: (peak: number, clip: number) => {
      adcPeakDbfs = Number.isFinite(peak) && peak > -90 ? peak : null;
      adcClipPct  = Number.isFinite(clip) ? clip : 0;
      /* ★★★ AND REPAINT, or the warning waits for news it is not part of. The chip is written on
       *     hwinfo and on `ovl` — both of which arrive when the GAIN MOVES — so a converter that
       *     starts railing at a gain nobody is changing would say nothing until the loop happened
       *     to act. That is precisely backwards: the moment worth showing is the one BEFORE the
       *     correction, which is the moment Stuart screenshotted.
       *  ★ Only while the loop owns the gain and is settled — mid-move the chip is saying which
       *    way it is going, and that is the more urgent message. */
      if (!hwAgcOn || hwGainNow < 0) return;
      const chip = $('ovlChip');
      if (!chip.classList.contains('easing')) return;   // a move is being announced; leave it
      chip.textContent = `AGC ${(hwGainNow / 10).toFixed(1)} dB${ifText()}${pkText()}`;
      chip.classList.toggle('fault', adcClipPct >= 0.01);
    },
    onSigStat: (chan, floor) => {
      if (!Number.isFinite(chan) || !Number.isFinite(floor)) return;
      srvChanDb = chan; srvFloorDb = floor; srvSigValid = true;
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
      // ★★★ DO NOT THROW THE LOGO AWAY WHEN THE NAME CHANGES. Many stations — every BBC one —
      //     send a DYNAMIC PS: the 8-character name cycles ("BBC R1", "NOW ON", "RADIO 1"), so
      //     this cleared the logo several times a minute and then looked up a fragment that
      //     matches nothing. The visible result is a logo that flashes and vanishes, on the very
      //     stations whose logos are easiest to find (Stuart, 2026-08-11: "a quick flash of the
      //     BBC Radio 1 logo does pop up then go again even when tuned to BBC Radio 1").
      //     ★★ THE PI CODE IS THE STATION; the PS is decoration it may change at will. So the
      //        logo belongs to the PI, and only a PI change invalidates it — see rdsLogoPi.
      if (ps !== rdsName) rdsName = ps;
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
      // ★ Look up once per STATION, not once per name fragment. A retry is allowed while we have
      //   no logo yet, because a later PS fragment ("BBC RADIO 1") may match where an earlier one
      //   ("NOW ON") could not — but a logo we already have is never discarded for a new fragment.
      if (rdsPi > 0 && rdsPi !== rdsLogoPi) {
        rdsLogoPi = rdsPi;
        rdsLogoUrl = '';
        logoQuery = '';
        logoDnsKey = '';
        rdsLogoProvisional = false;
  logoFromIdentity = false;
        logoFromIdentity = false;
      }
      // ★★★ AND UPGRADE A DERIVED ANSWER THE MOMENT THE REAL ECC ARRIVES. A logo found from an
      //     ECC we DERIVED rests on the assumption that the station is in the receiver's own
      //     country — which is fine inland and wrong on a border, where two countries can carry
      //     stations of the same name and even the same PI at different frequencies (Stuart,
      //     2026-08-14: "just in case you live near a border"). The transmitted ECC settles it,
      //     but it can arrive minutes later, long after we have a picture — and `!rdsLogoUrl`
      //     alone would never look again, so the guess would simply LOCK, which is the failure
      //     this whole identity path exists to prevent.
      // ★ One upgrade only: once we have asked with a real ECC the answer is authoritative, and
      //   rdsLogoProvisional stays false so a repeating ECC cannot start a request loop.
      // ★★ Re-ask when a PROVISIONAL logo could now be improved: either a real ECC has arrived
      //    (which may correct a derived one), or we have a PI and identity has not been tried yet
      //    for it. `logoDnsKey` records the attempt, so this cannot loop on a station that simply
      //    is not in RadioDNS — it asks once per PI+frequency and then leaves it alone.
      if (!rdsLogoUrl || (rdsLogoProvisional && (rdsEcc > 0 || rdsPi > 0)))
        void resolveRdsLogoBest(rdsIso);
      rdsFreq = spec ? spec.frequency : -1;   // this RDS belongs to THIS carrier
      if (rdsPanelOpen()) renderRds();
      updateVts();
    },
    onStatus: (s, detail) => {
      setStatus(s, detail);
      // Server-side settings live on the SERVER, so restoring the sliders isn't
      // enough — they have to be re-sent, or the UI shows values the radio isn't
      // actually using. Also covers reconnects, where the shim starts fresh.
      if (s === 'open') {
        pushSettingsToServer();
        // ★★★ ADMIN LIVES IN THE SERVER PROCESS, AND THE CLIENT'S BELIEF OUTLIVES IT.
        //
        //     `adminOk` is per-process state on the server, so a RESTART clears it — and every
        //     settings save restarts the server. The page went on showing ADMIN MODE, because
        //     that flag is ours, so the session countdown stayed hidden while the server had
        //     quietly demoted us to an ordinary listener. The limit then expired against someone
        //     who believed they were exempt, with no countdown to warn them: booted mid-listen,
        //     dropped back to the landing frequency, and then held on the cooldown that follows a
        //     timeout (Stuart, 2026-08-11, on the public demo — "I was already in admin mode, I
        //     shouldn't have hit a time limit", "there was no countdown clock").
        //     ★★ So RE-PROVE IT ON EVERY OPEN, not once per sign-in. This is the same family as
        //        the reconnect that re-attached decoders without their parameters, and as the
        //        settings above: whatever the server forgets across a restart, the client must
        //        say again. A credential we still hold costs one message to re-present.
        //     ★ Only when we hold the password. The splash's admin TICKET rides on the connect
        //       URL and is therefore re-presented by the reconnect itself.
        if (adminUnlocked && adminPassword) void sendAdminUnlock(adminPassword);
      }
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
  // ★★ AWAIT THE RESUME, THEN RE-CHECK. resume() is asynchronous, so a fixed 150 ms re-check ran
  //    while the context was still reporting `suspended` — the badge stayed up for ever even
  //    though the click had worked, and only vanished if you clicked the badge itself (Stuart,
  //    2026-08-08: "I clicked on the screen but the pill stayed until I clicked it").
  // ★★★ THE GATE MUST NOT VANISH BETWEEN PRESS AND RELEASE. kick() runs on POINTERDOWN and ends
  //     by re-checking, which REMOVES the overlay the moment the context resumes — so the mouseup
  //     and click that complete the very same gesture landed on the waterfall underneath and moved
  //     the dial (Stuart, 2026-08-08: "you click start audio and it nudges the waterfall"). The
  //     shield was being taken away halfway through the gesture it exists to absorb.
  // ★★ So a gesture on the gate HOLDS it up until the gesture is over. This is not a delay for
  //    appearance's sake: the overlay is the only thing standing between that click and the dial.
  let gateGestureActive = false;
  const kick = () => { void (async () => { await audio?.resume(); showAudioGate(); })(); };
  for (const ev of ['pointerdown', 'keydown', 'focus'] as const) {
    window.addEventListener(ev, kick);
  }

  // ★★★ SAY WHY THERE IS NO SOUND YET.
  //
  //     A browser will not start audio until the user has interacted with THIS page. Pressing
  //     START used to be that interaction, so audio began with the waterfall and nobody ever saw
  //     this. Opening a receiver straight from a radio card (?join) arrives with no gesture on the
  //     new page — so the waterfall runs, the controls look right, and it is SILENT until you
  //     happen to touch something. Stuart, 2026-08-08: "no audio yet the now playing controls are
  //     correct", then "ahh now I've tuned it restored".
  //
  // ★★ A SILENT RECEIVER THAT LOOKS FINE IS THE WORST OUTCOME — the listener concludes the RADIO
  //    is broken. One line on screen turns a mystery into a click.
  // ★ It disappears the moment audio runs, and never appears at all where the browser allows
  //   audio without a gesture.
  function showAudioGate() {
    const need = !!audio && audio.suspended && !NO_AUDIO;
    let el = document.getElementById('audioGate');
    // ★ Never mid-gesture — see gateGestureActive. The pointerup that follows is swallowed by the
    //   overlay's own handler, which then takes it down.
    if (!need) {
      if (el && !gateGestureActive) {
        el.remove();
        // ★ Anything held back while the gate covered the screen can be said now.
        vtsPumpNotices();
      }
      return;
    }
    if (el) return;

    // ★★ A GATE, NOT A HINT. A small badge left the receiver looking live while it was silent, and
    //    a listener reads that as a broken radio rather than as something to click. Dimming the
    //    spectrum says "not started yet" before a word is read, and puts the one action needed in
    //    the middle of the screen where it cannot be missed (Stuart, 2026-08-08, after UberSDR).
    el = document.createElement('div');
    el.id = 'audioGate';
    el.style.cssText = 'position:fixed;inset:0;z-index:60;display:flex;flex-direction:column;'
      + 'align-items:center;justify-content:center;gap:14px;background:rgba(0,0,0,.62);'
      + 'backdrop-filter:blur(1.5px);cursor:pointer';

    const btn = document.createElement('button');
    btn.type = 'button';
    btn.textContent = 'START';
    btn.style.cssText = 'font:600 15px/1 ui-monospace,monospace;letter-spacing:.22em;'
      + 'padding:16px 52px;border-radius:10px;cursor:pointer;'
      + 'color:var(--amber,#ffb000);background:rgba(0,0,0,.55);'
      + 'border:1px solid var(--amber,#ffb000);box-shadow:0 0 24px rgba(255,176,0,.25)';

    const why = document.createElement('div');
    why.textContent = 'Your browser needs a click before it will play audio';
    why.style.cssText = 'font:11px/1.5 ui-monospace,monospace;letter-spacing:.06em;'
      + 'color:var(--amber,#ffb000);opacity:.7;text-align:center;padding:0 1em';

    el.appendChild(btn); el.appendChild(why);

    // ★★★ THE GATE MUST EAT THE CLICK. Being on top is not enough: the waterfall listens for
    //     pointer events on the window, so the click that started audio ALSO fell through and
    //     retuned the radio — the listener's first act on the page moved the dial without them
    //     asking (Stuart, 2026-08-08: "clicking through to the waterfall underneath and is
    //     tuning").
    // ★★ Captured and stopped on EVERY pointer and mouse event of the gesture, not just
    //    pointerdown: the tuning handler may key off click, mouseup or touchend, and swallowing
    //    only the first of them leaves the rest to land on the dial.
    // ★ The whole overlay is the target, not just the button — anywhere is a fair place to click
    //   when the instruction is "click to start".
    // ★★★ STOP IT PROPAGATING, DO NOT preventDefault() IT. stopPropagation alone keeps the
    //     gesture off the waterfall — the tuning handlers listen on `window`, so a captured stop
    //     here never reaches them. preventDefault() does something else entirely: it CANCELS the
    //     click that a pointerdown/touchstart would have produced, which killed the gate's own
    //     gesture. The badge then sat there until the listener poked something underneath, i.e.
    //     exactly the click-through we added it to prevent (Stuart, 2026-08-08: "the audio doesnt
    //     start when you press start it still requires interaction underneath").
    const swallow = (e: Event) => {
      e.stopPropagation();
      (e as Event & { stopImmediatePropagation?: () => void }).stopImmediatePropagation?.();
    };
    // ★★ RESUME ON THE EVENTS THAT ACTUALLY CARRY USER ACTIVATION. `pointerdown` only counts for
    //    a MOUSE — on touch the browser grants activation on pointerup/touchend/click, so a
    //    phone tapping the gate got no activation at all and resume() was rejected silently.
    const kickers = new Set<string>(['pointerdown', 'pointerup', 'mouseup', 'touchend', 'click']);
    for (const ev of ['pointerdown', 'pointerup', 'mousedown', 'mouseup', 'click',
                      'touchstart', 'touchend', 'dblclick', 'wheel'] as const) {
      el.addEventListener(ev, (e) => {
        swallow(e);
        if (ev === 'wheel' || ev === 'dblclick') e.preventDefault();   // scroll/zoom, not a gesture
        // Hold the shield up from the first press of the gesture...
        if (ev === 'pointerdown' || ev === 'mousedown' || ev === 'touchstart') gateGestureActive = true;
        if (kickers.has(ev)) kick();
        // ...and only let it go once the gesture is finished, on the NEXT tick so the rest of this
        // gesture is still swallowed by an overlay that is still there.
        if (ev === 'click' || ev === 'pointerup' || ev === 'mouseup' || ev === 'touchend') {
          setTimeout(() => { gateGestureActive = false; showAudioGate(); }, 0);
        }
      }, { capture: true, passive: false });
    }
    document.body.appendChild(el);
  }
  // ★★★ AN ADMIN SESSION MUST NOT DIE IN THE OWNER'S HANDS. The ticket is deliberately short
  //     (ten minutes), but the menu hides its password box once the controls are unlocked, so an
  //     expiry mid-session would leave the owner unlocked-but-powerless with no way to prove
  //     themselves again short of going back to the landing page.
  // ★★ RENEWING REQUIRES A VALID TICKET, so this is a sliding session, not an unlimited one: it
  //    survives only while the tab is open and the current lease is still good. The server's own
  //    admin idle re-lock is unaffected and still governs.
  setInterval(() => {
    // ★ Only a page that actually signed in keeps the lease alive. Renewing from any page holding
    //   a stored ticket made admin permanent for the life of the tab; now a reload stops the
    //   renewals and the credential lapses on its own.
    if (!adminSignedInThisView || !inAdminMode()) return;
    const t = adminTicketQuery();
    if (!t) return;
    void fetch(`${httpBase(location.host)}/vibeserver/admin-ticket?${t}`, { cache: 'no-store' })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { if (j?.ticket) saveAdminTicket(String(j.ticket), Number(j.ttl) || 600); })
      .catch(() => { /* a blip must not end the session; the next tick tries again */ });
  }, 4 * 60 * 1000);

  setTimeout(showAudioGate, 600);
  // ★ And keep watching. Anything can suspend a context again — a tab switch, the OS, a phone
  //   call — so the badge has to be able to come BACK as well as go away. Cheap, and it means the
  //   badge can never be left saying something that is no longer true.
  setInterval(showAudioGate, 1000);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) kick(); });

  // Register the service worker — Chromium will not offer "install" without one. It caches
  // nothing (see /sw.js); it exists purely to satisfy the installability rule.
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register(P('/sw.js')).catch(() => { /* http:// LAN, or unsupported */ });
  }

  buildControls();
  initMediaSession();
  if (NO_DEC) console.warn('[bisect] decoders disabled by #nodec');
  else initDecoders(host, auth);
  initIdleThrottle();
  initAdminUnlock();
  initAdmin(() => currentHost, () => adminPassword);
  // ★ Also honoured here, for the case where the page was ALREADY connected when the fragment
  //   arrived. The early opener below covers the normal arrival.
  if (location.hash === '#admin' && inAdminMode()) openAdmin(currentHost, adminPassword);
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
/** ★★ The owner's gain ceiling in force right now, in the radio's units; -1 = none. Re-sent by
 *  the server whenever it changes, so it follows the listener across bands. */
let hwGainCap = -1;
/** ★ The radio's ACTUAL gain, from hwinfo. -1 = auto, or a server too old to send it. */
let hwGainNow = -1;
/* ★★★ WHETHER THE AGC IS ON, READ LIVE — NOT CAPTURED. onOverload takes `agc` as a PARAMETER and
 *     then reads it inside a setTimeout, so switching VibeAGC off between the event and the timer
 *     left the chip writing "AGC 8.7 dB" over a gain the listener had set by hand. Stuart, on
 *     2026-08-24, with the slider at 14.4: "no the agc wasnt enabled but the readout was still on
 *     screen ... I had manually set it" — and I spent the next twenty minutes reading an ADC
 *     overload log looking for a fault in the loop, because the screen told me the loop was
 *     driving.
 *  ★★ Same shape as the settings that reverted on every start (2026-08-22): a value captured in a
 *     closure is a value that WILL be stale, so it is held here and read through, never passed in.
 *  ★ A readout that names a mode the radio is not in is worse than a blank one. */
let hwAgcOn = false;

/* ★★★ THE "GAIN IS AT MINIMUM" NOTICE. See the markup note above #gainMinNote for why it exists,
 *   why it lives above the bar rather than in the top toast, and why it fades.
 * ★ Once per CONNECTION: a receiver deliberately parked at minimum must not keep announcing
 *   itself. Reset when a new radio's capabilities arrive. */
let gainMinShown = false;
let gainMinTimer: number | undefined;

/** What "minimum" means per radio, and only where the gain is actually MANUAL — a radio whose AGC
 *  is running is not deaf, it is being driven, and warning about that is crying wolf.
 *  ★ The RSP and HF+ are deliberately absent: both normally run their own AGC, and "minimum gain"
 *    on them is a max IF gain REDUCTION or a max attenuator — different quantities, worth
 *    measuring on hardware before claiming. */
function gainIsAtMinimum(): boolean {
  if (radioCaps?.driver === 'hackrf') {
    const lna = Number((radioCaps as any).lna ?? 0), vga = Number((radioCaps as any).vga ?? 0);
    return lna === 0 && vga === 0;
  }
  if (!hwAgcOn && hwGains.length > 0 && hwGainNow >= 0) return hwGainNow <= hwGains[0];
  return false;
}

/** ★ IN THE VTS NOW, not an overlay of its own — see vtsNotice(). The wording is the overlay's,
 *  which was Stuart's: the fear is somebody meeting a flat spectrum and concluding the receiver
 *  is broken rather than turned down. */
const GAIN_MIN_MSG =
  "This receiver's gain is at minimum — on a strong local signal that may be deliberate, "
  + 'but if the spectrum looks empty, raise the gain before deciding the receiver is dead.';

function hideGainMinNotice() {
  vtsClearNotice('gainmin');
  if (gainMinTimer) { clearTimeout(gainMinTimer); gainMinTimer = undefined; }
}

function maybeShowGainMinNotice() {
  // ★ THE MOMENT THE GAIN MOVES IT GOES. By then it has been acted on, and leaving it up would
  //   describe a state the radio is no longer in.
  if (!gainIsAtMinimum()) { hideGainMinNotice(); return; }
  if (gainMinShown) return;
  gainMinShown = true;
  /* ★★ 30 SECONDS. Long enough to survive a page still settling — waterfall filling, audio
   *    starting — and to be read by somebody who has not decided where to look yet. A notice gone
   *    before the thing it explains has finished appearing has told nobody anything. */
  vtsNotice('gainmin', GAIN_MIN_MSG, '', 30000);
}

/* ★★★ EXPLAIN THE ZOOM-FOLLOWING IF FILTER, BECAUSE NOTHING ELSE DOES IT THIS WAY.
 *   A tuner whose real selectivity moves when you zoom is, as far as we know, ours alone — so a
 *   listener has no prior experience to fall back on, and the behaviour reads as a fault: signals
 *   "disappear" when you zoom in and the radio "overloads for no reason" when you zoom out.
 *   Stuart: "I am thinking we are probably the first SDR to change IF filter width upon zooming
 *   in and out so a message to explain the behaviour would be good."
 * ★★ THE VTS BECAUSE IT SCROLLS. This is two sentences, not a chip — the bar is the one surface
 *    that can carry a long line and get rid of it again by itself.
 * ★ RTL ONLY, and only when the automation is actually ON. Explaining a mode the radio is not in
 *   is worse than silence: it describes behaviour the listener will never see and then they
 *   distrust the next thing we tell them. Same rule as the controls — see AGENTS.md. */
let rtlAutoExplained = false;

function maybeExplainRtlAutomation() {
  if (rtlAutoExplained) return;
  if (radioCaps?.driver !== 'rtl') return;
  // ★ Wait until we actually know. hwinfo and the tuner-bandwidth message arrive separately from
  //   caps, so firing on caps alone would announce "no automation" on a radio that has both.
  if (!hwTunerAuto && !hwAgcOn) return;
  rtlAutoExplained = true;
  const msg = hwTunerAuto && hwAgcOn
    ? 'Automatic gain and IF filtering are on. Zoom in to narrow the tuner\u2019s filter \u2014 it can clean up a signal crowded by strong neighbours. Zoom out and the receiver may overload briefly until the AGC settles.'
    : hwTunerAuto
    ? 'Automatic IF filtering is on \u2014 zooming in narrows the tuner\u2019s filter, so adjust the gain to suit.'
    : 'Automatic gain is on \u2014 the receiver sets its own gain and takes a moment to settle after a big change.';
  vtsNotice('rtlauto', msg, '', 20000);
}

let hwTunerBw = 0;
let hwTunerAuto = false;
let hwRfCentre = 0;
/* ★★★ THE CONVERTER'S OWN FIGURES, AND THE CHIP SHOWS THE ONE THAT MATTERS. It reported `pk` and
 *     nothing else — a PEAK, which we established on 2026-08-24 is not the harm: 96.1 sounds its
 *     best at -1.2 dBFS with nothing on the rail, while 104.2 at 7.7 dB reads 0.0 dBFS with 28% of
 *     samples railed and the band in ruins. Both used to print as a reassuring "pk -1".
 *  ★★ Stuart caught exactly that (2026-08-25): "25.4db gain but no massive overload at 103" —
 *     from a chip reading pk -1 while the loop was at the top of a climb it was about to be driven
 *     down from. His own follow-up was the diagnosis: "indication issue i think".
 *  ★ Only meaningful while the automation is on: the server does not measure these otherwise, on
 *    purpose, so nothing here asks for them and the chip already goes quiet with the AGC off. */
let adcClipPct = 0, adcPeakDbfs: number | null = null;
const pkText = () => {
  if (adcClipPct >= 0.01) return ` · ${adcClipPct.toFixed(1)}% RAILED`;
  return adcPeakDbfs === null ? '' : ` · pk ${adcPeakDbfs.toFixed(1)} dBFS`;
};
/** ★ The IF width beside the gain, because with VibeClarity running they are two halves of one
 *  decision and a gain figure alone does not explain what the receiver did. Silent when the filter
 *  is wide, which is the case that needs no explanation. */
const ifText = () => (hwTunerBw > 0
  ? ` · IF ${(hwTunerBw / 1e3).toFixed(0)} kHz${hwTunerAuto ? ' auto' : ''}`
  : '');
/* ★ The readout says the WIDTH and nothing else. An earlier version claimed the filter "costs
 *   ~11 dB on the wanted signal", from a differential measurement whose normalisation window
 *   (+/-96 kHz) overlapped the point being read — so the figure described the method, not the
 *   radio, and it never made physical sense for a 450 kHz filter 100 kHz from its centre. */
/** Clears the "overload passed" chip once the gain is home — see onOverload. */
let ovlClearTimer = 0;
let hwAgcLocked = false;
/** The owner has FIXED the gain on this band — see the hwinfo field. Per band, so it changes as
 *  the listener tunes; every control it governs is re-applied on each hwinfo. */
let hwGainLocked = false;
/** The least IF gain reduction the owner allows here, in dB — the slider's own units; -1 = none. */
let hwIfGrFloor = -1;
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
/** ★★★ ONE FORWARDED PORT: THIS PAGE MAY BE SERVED UNDER /r/<serial>/.
 *
 *  With several radios the owner forwards ONE port. The front door on it routes by path prefix and
 *  hands the whole connection to the right radio's process, so a listener reaches every radio at
 *  the same address — `…:48000/r/240513CA60/` — and never sees a second port.
 *
 *  ★★ EVERY URL THIS CLIENT BUILDS HAS TO CARRY THAT PREFIX, and there is exactly one lever that
 *     does it: `host`. The client already composes every request as `${httpBase(host)}/…` and every
 *     socket as `${wsBase(host)}/…`, so putting the prefix INSIDE host makes both follow without
 *     touching the dozens of call sites. The handful of absolute fetch('/…') calls are prefixed
 *     explicitly below — those are the ones that would silently miss it.
 *  ★ Empty on a single-radio server, where the page is served at the root exactly as before. */
/**
 * ★★★ NO PINCH-ZOOM ON THE RADIO SCREEN — and CSS cannot do this on iOS.
 *
 * `user-scalable=no` in the viewport meta is the wrong fix twice over: it takes zoom away from
 * people who need it everywhere, and iOS has IGNORED it since iOS 10 (see the note beside
 * `#splash input` in index.html). `touch-action: manipulation` removes double-tap zoom but
 * deliberately leaves pinch alone. WebKit gesture events are the only thing that stops it.
 *
 * ★★ WHY IT HAS TO STOP. A pinch here is almost never meant: the surface is a waterfall you drag
 *    and buttons you tap in quick succession. Once the page is scaled, every hit target has moved
 *    while the layout still believes otherwise — which reads as controls that have stopped
 *    working rather than as a page that has been zoomed (Stuart, 2026-08-22).
 *
 * ★★★ ONLY WHILE THE RADIO IS SHOWING. The splash is a form: text to read, fields to fill in, and
 *     it scrolls. Zoom stays available there. This is a scoped removal of a browser affordance,
 *     not a blanket one.
 * ★ gesturestart is WebKit-only, so this is inert in other browsers rather than needing a test.
 */
(() => {
  const radioShowing = () => {
    const splash = document.getElementById('splash');
    return !!splash && splash.classList.contains('hidden');
  };
  for (const ev of ['gesturestart', 'gesturechange', 'gestureend']) {
    document.addEventListener(ev, (e: Event) => {
      if (radioShowing()) e.preventDefault();
    }, { passive: false });
  }
})();

const BASE_PATH = (() => {
  const m = location.pathname.match(/^\/r\/[^/]+/);
  return m ? m[0] : '';
})();
/** An absolute path on THIS receiver, prefix included. */
const P = (path: string) => BASE_PATH + path;

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
  // ★★★ THE WATERFALL HOLD IS OFF. It froze the display while tuning (Stuart, 2026-08-10: "when I
  //     tune the RSP everything freezes completely then resumes when I stop tuning") and the
  //     mechanism is plain in hindsight: held rows are flushed on every retune, because a row
  //     computed at the old centre must not be painted at the new one's position — but DRAGGING
  //     the dial is a continuous stream of retunes. The queue was emptied faster than the hold
  //     let it fill, so nothing was ever released and the picture stopped until the dial did.
  //  ★★ THE TWO REQUIREMENTS ARE IN DIRECT CONFLICT and one line cannot satisfy both: the hold
  //     needs rows to SURVIVE long enough to smooth the link, and correctness needs them
  //     DISCARDED the instant the dial moves. Anything that keeps this idea has to reconcile
  //     them — e.g. hold rows tagged with their centre and release those matching the CURRENT
  //     centre rather than flushing wholesale, so a drag re-labels the queue instead of emptying
  //     it. Not attempted here; the live receiver comes first.
  //  ★★ RE-ENABLED once the conflict was actually resolved: a retune now opens a PASSTHROUGH
  //     window (rows go straight through while the dial moves, exactly as before any of this) and
  //     the depth RAMPS back in afterwards so re-engaging never stalls either. Verified against a
  //     simulated drag before going anywhere near the live receiver — the old algorithm freezes
  //     for the full length of the drag in that harness (3296 ms), the new one never exceeds
  //     192 ms in any case including a retune every 40 ms.
  //   ★ Kill switch is still one line: setHoldMs(0) restores the pre-buffer waterfall exactly.
  if (!NO_WF && audio) wf.setHoldMs(audio.jitterMs);
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
/** ★ Which PI the current logo belongs to. The PI identifies the STATION; the PS is a label it
 *  may change every few seconds, so the logo is keyed on the one that does not move. */
let rdsLogoPi = -1;
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
  rdsName = ''; rdsText = ''; rdsIso = ''; rdsLogoUrl = ''; logoQuery = ''; logoDnsKey = ''; rdsLogoPi = -1;
  rdsLogoProvisional = false;
  logoFromIdentity = false;
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

/**
 * Station logos remembered BY FREQUENCY, so the bookmark list can show the broadcaster's own
 * artwork instead of a name match.
 *
 * ★★★ A BOOKMARK HAS NO PI, AND IDENTITY NEEDS ONE. RadioDNS is keyed on PI + ECC + frequency —
 *     the things a transmitter states — while a bookmark stores a name and a frequency, so the
 *     list could only ever fall back to the crowd-sourced name search. Hence Stuart's report: the
 *     panel showed the real BBC and Heart artwork while the bookmark rows beside it showed the old
 *     favicons (2026-08-14).
 * ★★ BUT WE LEARN THE ANSWER EVERY TIME SOMEBODY LISTENS. When identity resolves a logo we know
 *    the frequency it belongs to, so recording it here turns "stations this receiver has actually
 *    heard" into a lookup table the bookmark list can use — no schema change, no re-saving old
 *    bookmarks, and it improves by itself as the band gets explored.
 * ★ localStorage, keyed to 10 kHz: bookmarks persist across sessions, so their logos should too.
 */
const FREQ_LOGO_KEY = 'vsFreqLogos';
function freqLogoKey(hz: number): string { return String(Math.round(hz / 10000)); }
function loadFreqLogos(): Record<string, string> {
  try { return JSON.parse(localStorage.getItem(FREQ_LOGO_KEY) || '{}') || {}; } catch { return {}; }
}
function rememberFreqLogo(hz: number, url: string): void {
  if (!(hz > 0) || !url) return;
  try {
    const all = loadFreqLogos();
    const k = freqLogoKey(hz);
    if (all[k] === url) return;
    all[k] = url;
    localStorage.setItem(FREQ_LOGO_KEY, JSON.stringify(all));
  } catch { /* private mode — the name search still applies */ }
}
let logoQuery = '';     // guards against a stale async logo landing late
/** ★ The PI/ECC/frequency already asked of the server's RadioDNS route — one request per station,
 *  not one per RDS update. Cleared wherever the logo is, so a new station is asked afresh. */
let logoDnsKey = '';
/** ★ True when the logo we are showing was found from a DERIVED ECC (the receiver's country), so
 *  it is our inference and not the transmitter's word. A real ECC arriving later replaces it. */
let rdsLogoProvisional = false;
/** ★★★ WHERE THE CURRENT LOGO CAME FROM. Identity (PI+ECC+frequency, the broadcaster's own file)
 *  OUTRANKS a name match, always — and without recording the source there is nothing to enforce
 *  that with. Making name results provisional accidentally let the name ladder RUN AFTER a
 *  successful RadioDNS lookup and overwrite it, including with '' when radio-browser had no
 *  match: "RadioDNS is not showing up any icons anymore" (Stuart, 2026-08-14). A ranking that
 *  exists only in the ORDER things happen is not a ranking; it is a race with an opinion. */
let logoFromIdentity = false;
/** The last multipath figure we could stand behind, kept so a dip in confidence dims the reading
 *  instead of replacing it. Cleared on retune — it describes a station, not a receiver. */
let mpHeld = '';

/** ★★★ BAND CROSSING, as the app announces it. The web VTS computed the band and then hid the
 *  whole bar whenever there was no station name — so on HF, where most of the dial has no
 *  bookmark, crossing into 40m told you nothing at all (Stuart, 2026-08-05: "the VTS isn't
 *  displaying the band when you move over a band boundary like it does in the app").
 *  ★ Keyed on the JOINED band names, exactly as SDRScreen's vtsCheck does: moving WITHIN a band
 *    must not re-announce it, and a frequency covered by two overlapping bands is a different
 *    place from one covered by either alone.
 *  ★ NOT announced on the first tune. Arriving somewhere is not crossing into it, and an
 *    announcement on page load is noise before the user has done anything. */
/** ★★ THE CONDITIONS THE VTS ANNOUNCES. Kept here rather than fetched on the crossing: a band
 *  change should say something IMMEDIATELY, and a fetch would either delay the announcement or
 *  arrive after it had gone. Refreshed on the same slow timer that feeds the landing page.
 *  ★ The app does exactly this for UberSDR (SDRScreen's showBandNotif reads getBandSnrDb), so a
 *    listener sees the same thing on the web client as on their phone (Stuart, 2026-08-06). */
let bandCond: { measured: Record<string, number>; predicted: Record<string, string> } =
  { measured: {}, predicted: {} };

async function refreshBandConditions(): Promise<void> {
  try {
    const r = await fetch(P('/vibeserver/conditions'), { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    const m: Record<string, number> = {};
    for (const x of (j.measured || [])) m[x.band] = x.snrDb;
    bandCond = { measured: m, predicted: j.solar?.bands || {} };
  } catch { /* leave the previous reading; a stale one is better than none */ }
}

let vtsBandKey: string | null = null;
let vtsBandInit = false;
let vtsBandMsg = '';
let vtsBandSub = '';
let vtsBandUntil = 0;
/** ★★★ EIGHT SECONDS, AND A TIMER THAT ACTUALLY FIRES. The expiry was checked only when something
 *  ELSE re-rendered the bar, so once the last event passed the message simply stayed on screen —
 *  "the VTS seems to get stuck showing either a station name or Band change" (Stuart, 2026-08-06).
 *  A time-based expiry needs a time-based trigger; an event-driven one is not an expiry at all.
 *  ★ 8000 ms matches the app's NOTIF_MS, so the two behave the same. */
const VTS_BAND_MS = 8000;
let vtsHideTimer: ReturnType<typeof setTimeout> | null = null;
/** When the current STATIC entry stops being worth showing. 0 = nothing static up. */
let vtsStaticUntil = 0;
let vtsLastHz = -1;

/** ★★ WHAT PERSISTS AND WHAT DOES NOT — the app's rule, verbatim. LIVE data (RDS) HOLDS on screen,
 *  because it keeps changing and is genuinely about what you are hearing right now. STATIC data —
 *  a bookmark or EiBi name, a band change — is an announcement, so it says its piece and goes.
 *  Leaving a bookmark name up forever makes the bar a permanent label that is only sometimes
 *  right, which is how it ends up describing a frequency you left ten minutes ago. */
/** Marquee whatever overflows. ★ Measured, not guessed: the distance is the difference between
 *  the text's real width and the box it has to live in, so the animation stops exactly at the end
 *  of the words rather than at some fraction that happens to look right at one window size.
 *  ★★ The text is wrapped in a span because the ELEMENT must stay put (it is the clip) while its
 *     CONTENT moves. Wrapping is done here rather than in the markup so every caller — RDS,
 *     bookmark, EiBi, band conditions — gets it without having to remember. */
/** How far #vtsName overflowed at the last measurement, px. 0 = it fits. */
let vtsNameOver = 0;
/** The scroll duration actually applied to #vtsName, seconds — the notice's life is derived
 *  from THIS rather than recomputed, so the two cannot disagree. */
let vtsNameDurS = 0;
/** What the notice slot last actually WROTE to the bar. Null = nothing of ours is on screen.
 *  Guarding on this is what lets the scroll animation survive from one frame to the next. */
let vtsRenderedMsg: string | null = null;
let vtsRenderedSub: string | null = null;
/** Reading pace for a notice, px/s. Slower than the band line's implicit 33 px/s because a
 *  notice is prose to be read once, not a label to be recognised. */
const VTS_NOTICE_PX_PER_S = 42;
/** A beat at the end so the last words can be read before it leaves. */
const VTS_NOTICE_TAIL_MS = 2500;

function applyVtsScroll(isLive: boolean) {
  const vts = $('vts');
  vtsNameOver = 0;
  /* ★ `notice` selects the near-instant lead-in keyframes; a band announcement keeps the original
   *  ones, where a pause before moving is fine at its short duration.
   *  ★★ SET HERE rather than in the notice branch, because it must also be REMOVED — a station
   *     name rendering after a notice would otherwise inherit the notice's timing. Every render
   *     path calls this function; only one of them is the notice. */
  vts.classList.toggle('notice', !!vtsNoticeKey);
  vts.classList.toggle('live', isLive);
  /* ★★★ MEASURE UNDER THE CONSTRAINED LAYOUT, or nothing ever scrolls.
   *   The `max-width` and `overflow: hidden` that make the text clip live on `#vts.scroll`, and
   *   this function decides whether to ADD that class — so it was measuring the element at its
   *   full natural width, where scrollWidth == clientWidth and `over` is always 0. The class was
   *   therefore never added, and the check that would have added it could never pass: a
   *   chicken-and-egg that silently disabled the whole feature. Stuart, 2026-08-27: "its not
   *   scrolling."
   * ★★ RDS was unaffected, which is exactly why this hid for so long — RadioText has its OWN
   *    scroller (#vtsRtInner, see fitRadioText), so the one long line people watch every day
   *    scrolled fine while this path never did.
   * ★ Add first, measure, then toggle off if nothing actually overflows. Reading scrollWidth
   *   forces the layout, so the measurement below sees the clamp already applied. */
  vts.classList.add('scroll');
  let any = false;
  for (const id of ['vtsName', 'vtsText']) {
    const el = document.getElementById(id);
    if (!el) continue;
    const txt = el.textContent ?? '';
    if (!txt) { el.style.removeProperty('--vts-slide-dist'); continue; }
    if (el.firstElementChild?.tagName !== 'SPAN') el.innerHTML = `<span>${escapeHtml(txt)}</span>`;
    const span = el.firstElementChild as HTMLElement;
    // Read the natural width against the space actually available.
    const over = span.scrollWidth - el.clientWidth;
    if (over > 4) {
      any = true;
      el.style.setProperty('--vts-slide-dist', `${-over}px`);
      /* ★★★ ONE PLACE SETS THE DURATION. It used to be set here for every element and then
       *   OVERWRITTEN afterwards for a notice — two writers for one value, and the second wrote
       *   it after the animation had already started with the first, which a browser is not
       *   obliged to honour. That is why a notice still ran out mid-sentence after the life was
       *   supposedly matched to it (Stuart: "still isnt showing fully before the scroll times
       *   out"). The notice's own pace is decided HERE and nothing rewrites it.
       * ★ Speed scales with LENGTH, like the app's `Math.max(2200, dist * 16)`: a long RadioText
       *   and a short band line should read at the same pace, not take the same time. */
      const isNotice = !!vtsNoticeKey && id === 'vtsName';
      const durS = isNotice ? Math.max(4, over / VTS_NOTICE_PX_PER_S)
                            : Math.max(4, over * 0.03);
      el.style.setProperty('--vts-slide-dur', `${durS.toFixed(1)}s`);
      if (id === 'vtsName') { vtsNameOver = over; vtsNameDurS = durS; }
    } else {
      el.style.removeProperty('--vts-slide-dist');
    }
  }
  vts.classList.toggle('scroll', any);

  /* ★★★ A NOTICE STAYS UP LONG ENOUGH TO BE READ TO THE END.
   *   The life was a fixed 20-30 s while the scroll duration scaled with LENGTH, so the two had
   *   no reason to agree — and on a long explanation they did not: the message was removed
   *   mid-sentence. So the LENGTH decides the LIFE, not the other way round. We control the
   *   words, so the words control the time.
   * ★★ DERIVED FROM THE DURATION ACTUALLY APPLIED ABOVE (vtsNameDurS), not recomputed here from
   *   the same inputs. Two derivations of one number is how they drift apart, which is exactly
   *   the fault this is fixing.
   * ★ Once per notice (vtsNoticeSized). updateVts() runs on every spectrum frame and on the hold
   *   timer, so extending on each pass would push the deadline for ever and it would never go. */
  if (vtsNoticeKey && !vtsNoticeSized && vtsNameOver > 4 && vtsNameDurS > 0) {
    vtsNoticeSized = true;
    // The travel occupies 94% of the run (keyframes 3%..97%), after a 0.35 s delay, and then a
    // beat to read the tail before it leaves.
    const needed = 350 + vtsNameDurS * 1000 + VTS_NOTICE_TAIL_MS;
    if (needed > vtsBandUntil - Date.now()) {
      vtsBandUntil = Date.now() + needed;
      if (vtsHideTimer) clearTimeout(vtsHideTimer);
      vtsHideTimer = window.setTimeout(() => {
        vtsHideTimer = null;
        vtsNoticeKey = '';
        updateVts();
        vtsPumpNotices();
      }, needed + 60);
    }
  }
}


function vtsHoldFor(ms: number) {
  if (vtsHideTimer) clearTimeout(vtsHideTimer);
  vtsHideTimer = setTimeout(() => { vtsHideTimer = null; updateVts(); }, ms);
}

function fmtBandFreq(hz: number): string {
  if (hz >= 1_000_000) {
    const mhz = hz / 1_000_000;
    return (mhz === Math.floor(mhz) ? mhz.toFixed(0) : mhz.toFixed(mhz < 10 ? 3 : 1)) + ' MHz';
  }
  return Math.round(hz / 1000) + ' kHz';
}

/** ★★★ ONE TRANSIENT SLOT IN THE VTS, not a new popup per thing we want to say.
 *
 *  The band announcement already owned the bar for a few seconds and already scrolls a line
 *  longer than the window, which is exactly what an explanation needs — so anything else worth
 *  explaining goes through the same door. Stuart: "we can also move the 0 Gain message we added
 *  last night to the VTS too so we arent inventing new popups."
 *  ★★ WHY THAT MATTERS BEYOND TIDINESS: every separate overlay has to decide where it sits, what
 *     it covers and when it leaves, and they collide as a matter of course — the top toast is
 *     already carrying session timeouts, the owner's maintenance notice and the VTS itself.
 *     A single slot has one answer to all three questions.
 *  ★★★ AND THEY QUEUE RATHER THAN CLOBBER. Two of these can become true in the same instant —
 *     connecting to an RTL that is BOTH running the zoom-following IF filter AND parked at
 *     minimum gain is one connection, two things worth knowing — and last-writer-wins would show
 *     the first for a few milliseconds before the second wiped it. Stuart asked for exactly this
 *     on Jr: "it needs to cycle with the timeout message so once one is dismissed the next is
 *     there ready."
 *  ★ KEYED, so a notice can be WITHDRAWN when it stops being true (the gain notice goes the
 *    moment the gain moves) whether it is showing or still waiting its turn. A queue you cannot
 *    retract from shows stale news, which is the fault we deleted the chat replay for.
 */
type VtsNotice = { key: string; msg: string; sub: string; ms: number };
let vtsQueue: VtsNotice[] = [];
let vtsNoticeKey = '';
/** Whether the showing notice has already had its life sized to its length (see applyVtsScroll). */
let vtsNoticeSized = false;

function vtsPumpNotices() {
  if (vtsNoticeKey || !vtsQueue.length) return;    // one is still on screen
  /* ★★★ NOT WHILE THE AUDIO GATE IS UP. The gate is a full-screen overlay, so a notice shown
   *   underneath it scrolls, times out and is gone before the listener has clicked START —
   *   Stuart, 2026-08-27: "I noticed it scrolling behind the overlay". Which is the worst case
   *   for THIS message in particular: it explains the receiver's behaviour to somebody who has
   *   just arrived, and they are the only person who cannot see it.
   * ★★ HELD, NOT DROPPED. The queue keeps it and the gate's own teardown pumps it, so the
   *   explanation arrives when there is somebody able to read it. That is the whole reason the
   *   slot is a queue rather than a variable. */
  if (document.getElementById('audioGate')) return;
  const n = vtsQueue.shift()!;
  vtsNoticeKey = n.key;
  vtsNoticeSized = false;
  vtsBandMsg = n.msg;
  vtsBandSub = n.sub;
  vtsBandUntil = Date.now() + n.ms;
  // ★ The hold timer is what ends this notice AND starts the next: without pumping here a queued
  //   message would sit until the next band crossing happened to redraw the bar.
  if (vtsHideTimer) clearTimeout(vtsHideTimer);
  vtsHideTimer = window.setTimeout(() => {
    vtsHideTimer = null;
    vtsNoticeKey = '';
    updateVts();
    vtsPumpNotices();
  }, n.ms + 60);
  updateVts();
}

function vtsNotice(key: string, msg: string, sub: string, ms: number) {
  if (vtsNoticeKey === key || vtsQueue.some(n => n.key === key)) return;   // already said
  vtsQueue.push({ key, msg, sub, ms });
  vtsPumpNotices();
}

/** Withdraw a notice by key — showing or merely queued. */
function vtsClearNotice(key: string) {
  vtsQueue = vtsQueue.filter(n => n.key !== key);
  if (vtsNoticeKey !== key) return;
  vtsNoticeKey = '';
  vtsNoticeSized = false;
  vtsBandUntil = 0;
  if (vtsHideTimer) { clearTimeout(vtsHideTimer); vtsHideTimer = null; }
  updateVts();
  vtsPumpNotices();
}

function checkBandCrossing(hz: number) {
  const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
  const bands = getBandsAtRegion(hz, ituRegion())
    .sort((a, b) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
  const key = bands.length ? bands.map(b => b.name).join('|') : null;
  if (!vtsBandInit) { vtsBandInit = true; vtsBandKey = key; return; }
  if (key === vtsBandKey) return;
  vtsBandKey = key;
  if (!bands.length) return;                    // left the band plan: nothing to announce
  const p = bands[0];
  const region = ituRegion();
  // ★ Same sentence as the app's, minus the band-conditions figure: that comes from UberSDR's
  //   /api/noisefloor/latest, which a VibeServer does not serve. Inventing one would be worse
  //   than omitting it — see "no inferred hardware readouts".
  // ★ Stuart's wording: the BAND first, then its conditions spelled out. "BAND: 7.000-7.200 MHz"
  //   led with numbers the reader already has on the dial; the NAME is what a band change is.
  // ★ Name first, then the range: the NAME is what a band change IS, and the range is the useful
  //   detail that follows (Stuart asked for the range back, 2026-08-06 — it belongs, just not
  //   leading).
  /* ★★★ A NOTICE OUTRANKS A BAND ANNOUNCEMENT, and they share this slot — vtsBandMsg and
   *   vtsBandUntil are the same two variables — so a crossing would simply overwrite an
   *   explanation that is mid-sentence. Connecting crosses into a band, which is exactly when a
   *   notice is up; the app had the identical collision and showed "BAND: 87.5 MHz–108 MHz"
   *   where the explanation had been (Stuart, 2026-08-28).
   * ★ Dropped rather than queued: vtsBandKey is already latched above, and the band is written
   *   along the top of the spectrum anyway. The explanation has one chance; the band does not. */
  if (vtsNoticeKey) return;
  vtsBandMsg = `${p.name} · ${fmtBandFreq(p.lo)}–${fmtBandFreq(p.hi)}`
             + (bands.length > 1 && region ? ` (ITU R${region})` : '');
  // ★★★ AND WHAT THE BAND IS DOING, when this receiver has something to say about it. The
  //     measured figure is THIS aerial right now, so it is the part worth leading with; the
  //     prediction follows as context. Silent for a band we do not measure — an FM profile has
  //     nothing to report about HF, and inventing a verdict would be worse than saying nothing.
  const lbl = p.bandLabel || '';
  const meas = lbl ? bandCond.measured[lbl] : undefined;
  const pred = lbl ? bandCond.predicted[lbl] : undefined;
  if (meas !== undefined) {
    const word = meas >= 15 ? 'Excellent' : meas >= 9 ? 'Good' : meas >= 4 ? 'Fair' : 'Poor';
    vtsBandMsg += ` — Conditions: Predicted: ${pred || '—'}`
                + ` / Actual: ${word} (${meas.toFixed(0)} dB)`;
  } else if (pred) {
    vtsBandMsg += ` — Conditions: Predicted: ${pred}`;
  }
  vtsBandSub = bands.slice(1).map(b => b.name).join('  │  ');
  vtsBandUntil = Date.now() + VTS_BAND_MS;
  vtsHoldFor(VTS_BAND_MS + 60);      // ★ re-render when it expires, or it never disappears
  // ★ The app also applies band-aware mode/step defaults on crossing, but ONLY when the tuning
  //   was NOT hands-on (lock screen, watch, car controls). In a browser every tune is hands-on,
  //   so there is no case to apply it to — porting it would only yank the mode out from under
  //   someone who had just chosen it.
}

function updateVts() {
  expireRdsIfRetuned();
  if (!spec) return;
  // ★ A move of the dial is a NEW announcement, so the static clock restarts rather than the old
  //   one continuing to run against a station you have already left.
  if (spec.frequency !== vtsLastHz) { vtsLastHz = spec.frequency; vtsStaticUntil = 0; }
  checkBandCrossing(spec.frequency);
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
  // ★ Tracks whether what we are about to show is LIVE (the station naming itself) or STATIC (our
  //   guess from a list). Only the live kind may stay up — see vtsHoldFor().
  let live = !!rdsName;
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
  // ★★ THE BAND ANNOUNCEMENT OWNS THE BAR WHILE IT IS LIVE, station or not. It is transient and
  //    it is about where you have just ARRIVED, which is exactly the moment it is worth reading.
  if (Date.now() < vtsBandUntil) {
    /* ★★★ ONLY WRITE THE TEXT WHEN IT CHANGES, or nothing can ever scroll.
     *   updateVts() runs on EVERY spectrum frame — about fifteen times a second — and this branch
     *   assigned textContent every time. That destroys the <span> the slide animation lives on,
     *   applyVtsScroll dutifully rebuilds it, and rebuilding restarts the animation from zero. So
     *   the line twitched at the start and never advanced: "it attempted to scroll the message but
     *   then stopped" (Stuart, 2026-08-27). Every earlier fix — the missing applyVtsScroll call,
     *   the chicken-and-egg measurement, the duration, the life — was correct and made no
     *   difference, because the animation was being killed and restarted before it could move.
     * ★★ RDS was never affected, which is why this survived three rounds of looking at it:
     *    RadioText scrolls through its own element (#vtsRtInner) and never comes through here.
     * ★ The classes and the hidden siblings are idempotent, so they can be set every frame. It is
     *   textContent that is destructive, and it is the only thing now guarded. */
    if (vtsRenderedMsg !== vtsBandMsg || vtsRenderedSub !== vtsBandSub) {
      vtsRenderedMsg = vtsBandMsg;
      vtsRenderedSub = vtsBandSub;
      $('vtsName').textContent = vtsBandMsg;
      $('vtsBand').textContent = vtsBandSub;
      vts.classList.add('show');
      vts.classList.remove('on');
      // ★ Measured and started ONCE, on the render that actually changed the words.
      applyVtsScroll(false);
      setDecBoxOffset();
    }
    for (const id of ['vtsRds', 'vtsSrc', 'vtsLogo', 'vtsFlag', 'vtsPi'])
      ($(id) as HTMLElement).style.display = 'none';
    vts.classList.add('show');
    vts.classList.remove('on');
    return;
  }
  /* ★★★ RETIRE AN EXPIRED NOTICE HERE, not only on its timer. vtsHideTimer is shared with
   *   vtsHoldFor(), which a band crossing calls — so a band change during a notice replaces the
   *   very timer that was meant to clear vtsNoticeKey, and the key then sticks for ever: the
   *   queue stops pumping and no later notice is ever shown. The render path knows the deadline
   *   has passed, so it can say so without depending on which timer survived. */
  if (vtsNoticeKey) { vtsNoticeKey = ''; vtsNoticeSized = false; vtsPumpNotices(); }
  vtsRenderedMsg = null;
  for (const id of ['vtsRds', 'vtsSrc', 'vtsLogo', 'vtsFlag', 'vtsPi'])
    ($(id) as HTMLElement).style.removeProperty('display');

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
  applyVtsScroll(live || rdsPi > 0);
  // ★★ Static content gets a life; live RDS does not. A PI-only identification counts as live —
  //   it is the transmitter telling us who it is, and it will keep arriving.
  if (!live && rdsPi <= 0) {
    // ★★★ START THE CLOCK ONCE. The condition here used to ALSO fire on expiry — so the moment the
    //     deadline passed it was pushed another 8 s into the future, and the hide below could
    //     never be reached. The bar therefore renewed itself forever, which is exactly the
    //     "still sticking" Stuart saw after the first fix (2026-08-06).
    //     ★ The two questions are DIFFERENT and must not share a branch: "has this clock started?"
    //       is `!vtsStaticUntil`; "has it run out?" is the comparison. Answering them together
    //       makes expiry indistinguishable from a fresh start.
    if (!vtsStaticUntil) vtsStaticUntil = Date.now() + VTS_BAND_MS;
    if (Date.now() >= vtsStaticUntil) { vts.classList.remove('show', 'on'); setDecBoxOffset(); return; }
    vtsHoldFor(vtsStaticUntil - Date.now() + 60);
  } else {
    vtsStaticUntil = 0;                       // live: no expiry at all
    if (vtsHideTimer) { clearTimeout(vtsHideTimer); vtsHideTimer = null; }
  }
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
    if (logoEl.src !== logo) {
      // ★★★ A URL THAT RESOLVES IS NOT A PICTURE THAT LOADS. radio-browser's favicons are
      //     submitted by users and plenty are dead links, expired hosts or hotlink-blocked — the
      //     lookup succeeds, the <img> 404s, and the bar shows an EMPTY BOX where a logo should
      //     be (Stuart, 2026-08-11: "Classic FM gets a blank box"). An empty frame is worse than
      //     no frame: it reads as a broken app rather than a station without artwork.
      //     ★ So the element is only SHOWN once the image has actually decoded, and a failure
      //       hides it and forgets the URL so the fallback (name, monogram) takes over.
      logoEl.classList.remove('show');
      logoEl.onload  = () => { logoEl.classList.add('show'); };
      logoEl.onerror = () => {
        logoEl.classList.remove('show');
        if (rdsLogoUrl === logo) rdsLogoUrl = '';   // do not retry a dead link this session
      };
      logoEl.src = logo;
    } else if (logoEl.complete && logoEl.naturalWidth > 0) {
      logoEl.classList.add('show');
    }
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
/**
 * ★★★ TRY THE FULL NAME FIRST, THEN THE SHORT ONE, THEN THE MESSAGE.
 *
 * The 8-character PS is often a FRAGMENT of a rotating message rather than the station's name —
 * "NOW ON", "RADIO 1", "BBC R1" — and looking a fragment up finds nothing. Three sources say who
 * the station is, in descending order of how much we should trust them:
 *   1. LONG PS — what stations transmit precisely BECAUSE eight characters is not enough. "BBC
 *      Radio 1" arrives whole, which is exactly what a name search wants.
 *   2. PS — right for the many stations that send a static one (Heart, Flex, Horizon all work).
 *   3. RadioText — a message, not a name, but it usually OPENS with the station's name, so its
 *      first few words are worth one attempt when the other two have failed. Trimmed to that.
 *
 * ★★ Order matters and so does stopping: the first source that yields a logo wins, and the rest
 *    are not tried. Searching the RadioText of a station we have already identified is how a
 *    "Now playing: Adele" turns into somebody else's artwork.
 * ★ Stuart, 2026-08-11: "why does it not use the station name? then the radio text afterwards?"
 */
async function resolveRdsLogoBest(iso: string) {
  // ★★★ THE BROADCASTER'S OWN ARTWORK FIRST, KEYED ON THE PI — a NAME search cannot be trusted to
  //     identify a station, and a wrong answer LOCKS. The logo is invalidated only by a PI change
  //     (rdsLogoPi, and rightly — the PS rotates), so a name that was briefly wrong when the
  //     lookup happened is never re-examined: one of the weaker BBC Radio 3 signals wore the
  //     Radio 1 roundel and stayed that way (Stuart, 2026-08-11). Both ways the name can lie
  //     produce exactly that — a PS damaged in the noise ("BBC R1" is two bits from "BBC R3"), or
  //     an EON group naming ANOTHER network, and that screenshot showed 30% 14A traffic listing
  //     BBC R1 first.
  //     ★★ RadioDNS is keyed on PI + ECC + frequency, which is the identity the transmitter
  //        error-protects and repeats 11 times a second, and it returns the BROADCASTER'S OWN
  //        file rather than a crowd-sourced favicon that may 404. That is what the server-side
  //        lookup was built for; until now nothing called it.
  //     ★ Server-side ON PURPOSE: it needs DNS SRV and CNAME resolution, which a page cannot do.
  //       An older server has no such route, so a 404 here just falls through to the name ladder.
  const hz = spec ? spec.frequency : 0;
  // ★★★ NO LONGER GATED ON A TRANSMITTED ECC — that gate is why this almost never fired. The ECC
  //     rides in group 1A and a great many stations never send it, so `rdsEcc` stays 0 for ever
  //     and the lookup was skipped for exactly the stations that most needed it: BBC Radio 1 (1A
  //     at 11% of groups) showed its real artwork while Heart, on the same receiver, showed a
  //     generic favicon — the only difference being whether the transmitter names its country
  //     (Stuart, 2026-08-14). The SERVER now derives it from the receiver's own country when we
  //     send none, so "00" here means "you work it out", not "unknown, give up".
  if (rdsPi > 0 && hz > 0) {
    const piHex  = rdsPi.toString(16).toUpperCase().padStart(4, '0');
    const eccHex = rdsEcc.toString(16).toUpperCase().padStart(2, '0');
    const key = `${piHex}|${eccHex}|${Math.round(hz)}`;
    // ★ ONE REQUEST PER STATION. This runs on every RDS update while we have no logo, and the
    //   miss path costs the server a DNS round trip — unguarded it would be several a second.
    if (logoDnsKey !== key) {
      logoDnsKey = key;
      try {
        const r = await fetch(P(`/vibeserver/stationlogo?pi=${piHex}&ecc=${eccHex}&freq=${Math.round(hz)}`),
                              { cache: 'no-store' });
        if (r.ok) {
          const url = String((await r.json())?.logo ?? '');
          // The dial may have moved while DNS was resolving — this answer belongs to the station
          // we ASKED about, not to whatever is tuned now.
          if (url && logoDnsKey === key) {
            rdsLogoUrl = url;
            logoFromIdentity = true;
            // ★ Remember it against the FREQUENCY — this is what lets the bookmark list show the
            //   broadcaster's own artwork for a station with no PI stored against it.
            rememberFreqLogo(hz, url);
            // ★ Remember HOW we identified it. Derived (we sent "00") is provisional and will be
            //   re-asked when the transmitter finally states its country; transmitted is final.
            rdsLogoProvisional = (rdsEcc <= 0);
            updateVts();
            updateMediaSession();
            return;
          }
        }
      } catch { /* no server route, or offline — the name ladder below still applies */ }
    }
  }
  // ★★★ THE NAME LADDER DOES NOT RUN ONCE IDENTITY HAS ANSWERED. The PI is error-protected and
  //     repeated eleven times a second; a name is eight characters of rotating text that one bad
  //     decode can turn into another station. There is no circumstance in which the second should
  //     be allowed to replace the first.
  if (logoFromIdentity && rdsLogoUrl) return;

  const longPs = (rdsExt?.longPs ?? '').trim();
  // ★ Only the opening of the RadioText: the station name, not the track.
  const rtHead = rdsText.split(/[-–|:]/)[0].trim().split(/\s+/).slice(0, 4).join(' ');
  const seen = new Set<string>();
  for (const cand of [longPs, rdsName, rtHead]) {
    const name = (cand || '').trim();
    if (name.length < 3 || seen.has(name.toLowerCase())) continue;
    seen.add(name.toLowerCase());
    await resolveRdsLogo(name, iso);
    if (rdsLogoUrl) return;              // found one — stop looking
  }
}

async function resolveRdsLogo(name: string, iso: string) {
  const key = `${name}|${iso}`;
  if (logoQuery === key) return;
  logoQuery = key;
  // ★ The previous logo STAYS while this lookup runs. Blanking here made every lookup a visible
  //   flicker even when it was about to succeed with the same URL.
  try {
    const url = await lookupStationLogo(name, iso || undefined, serverIso || undefined);
    // A slow lookup must not overwrite a station we've since tuned away from.
    if (logoQuery !== key) return;
    // ★ A slow name lookup may land AFTER identity has answered — it must not undo it.
    if (logoFromIdentity && rdsLogoUrl) return;
    rdsLogoUrl = url || '';
    // ★★★ A NAME-SEARCH RESULT IS PROVISIONAL, AND SAYING SO IS THE WHOLE FIX. A NAME IS NOT AN
    //     IDENTITY: radio-browser matched "BBC 3CR" to a generic Radioplayer icon, and because a
    //     logo we already have was never re-examined, that wrong picture LOCKED — even though the
    //     server was returning the correct BBC Three Counties artwork the whole time, keyed on the
    //     PI (Stuart, 2026-08-14: "icon is wrong"). The name usually resolves FIRST, before the PI
    //     has even arrived, so first-past-the-post meant the weakest evidence always won.
    // ★★ Marking it provisional lets the identity path overrule it the moment the PI is known —
    //    the same rule the app follows, and the same failure the app already suffered when a weak
    //    Radio 3 wore the Radio 1 roundel.
    if (rdsLogoUrl) rdsLogoProvisional = true;
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
/** ★ Module-level so the SPECTRUM FRAME handler can repaint the card's meter. It is set
 *  during UI init, which happens before any frame arrives; the `?.` covers the gap. */
let mobileUi: ReturnType<typeof initMobileControls> | null = null;
/** ★ The last passband peak in dBFS. updateSignal() computes it as a local, but the card's
 *  meter can be switched to show dBFS or S-units, and both need the raw figure rather than the
 *  0..1 the gradient uses. Mirrored here rather than recomputed, so every readout agrees. */
let lastSigDb = -160;

let snrSmooth = 0;

/** ★ The server's own channel power / noise floor, off the full-rate FFT (see the 'sig' message).
 *  `srvSigValid` stays false until the first one arrives, so a server too old to send them keeps
 *  the frame-derived fallback. (This client only ever talks to a VibeServer — the multi-backend
 *  Kiwi/OpenWebRX/SpyServer world is the APP's, not this one's.) */
let srvChanDb = -160, srvFloorDb = -120, srvSigValid = false;

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
  let noiseDb = sample[Math.floor(sample.length * 0.25)] ?? -120;

  // ★★★ PREFER THE SERVER'S MEASUREMENT, BECAUSE EVERYTHING ABOVE IS MEASURED IN WHATEVER
  //     RESOLUTION THE USER HAPPENS TO BE ZOOMED TO. A frame's bins narrow as you zoom in, so a
  //     carrier's power concentrates into fewer of them while the per-bin noise floor falls —
  //     and the meter climbed toward full scale for no reason but the zoom (Stuart, 2026-08-03:
  //     "the bar is relative to the zoom not the S meter").
  //     The server measures both on the FULL-RATE FFT at a fixed resolution, so they hold still.
  //     `chan` is the same quantity the SQUELCH uses — the one meter that was always right.
  //   ★ The frame-derived figures above remain the fallback for a server too old to send `sig`,
  //     so a client on an older VibeServer still gets a meter rather than a dead one.
  if (srvSigValid) { sigDb = srvChanDb; noiseDb = srvFloorDb; }

  const snr = Math.max(0, sigDb - noiseDb);
  // ★ FAST UP, SLOW DOWN — the same asymmetry the bar itself uses, and for the same reason: a
  //   meter that lags a signal appearing is useless, while one that falls back gently is readable.
  //   A single symmetric coefficient has to be a compromise between those, and was.
  snrSmooth += (snr - snrSmooth) * (snr > snrSmooth ? 0.30 : 0.12);   // matches the bar
  lastSigDb = sigDb;

  // ★★★ THE METER HAS ITS OWN FIXED SCALE, AND MUST — IT USED TO BORROW THE WATERFALL'S.
  //     `wf.getRange()` is the AUTO-CONTRAST range: it re-fits itself to whatever is on screen,
  //     so zooming into a signal raises the floor and BOTH the fill and the squelch needle
  //     climbed towards full scale. Fixing the signal measurement was necessary but not
  //     sufficient — a correct reading divided by a moving range still moves (Stuart,
  //     2026-08-03: "if I zoom all the way in both max out nearly, which for setting squelch
  //     is no good"). A signal meter must never depend on a DISPLAY setting.
  //   ★ The endpoints are the S-unit ladder in toSUnit(): S1 = -115 dBFS, S9+60 = -13. So the
  //     bar and the S-unit text beside it now describe the same scale instead of two different
  //     ones, and a given signal always lands in the same place — which is what makes it
  //     possible to SET A SQUELCH THRESHOLD and have it still mean that later.
  const dbMin = METER_MIN_DBFS, dbMax = METER_MAX_DBFS;
  const norm = Math.max(0, Math.min(1, (sigDb - dbMin) / (dbMax - dbMin)));

  // Asymmetric smoothing: fast attack, slow decay (same feel as the app's meter).
  // ★★ SMOOTHED ENOUGH TO RIDE OVER THE AGC'S STAIRCASE. The reading is post-AGC dBFS, and the
  //    RSP's AGC moves its IF reduction in whole-dB STEPS — so a signal rising smoothly is
  //    measured as rise, step, rise, step, and the bar "moves a little, halts, moves some more"
  //    (Stuart, 2026-08-05, on the Buzzer). That is the receiver being visible in a reading that
  //    should only show the signal.
  //    ★ Gentler than before on BOTH edges (was 0.55/0.18). At 20 Hz these are time constants of
  //      roughly 150 ms up and 400 ms down: still quicker than the old 4 Hz repaint could manage
  //      at any coefficient, and slow enough that a 1 dB AGC step is a ripple rather than a stair.
  //    ★ Attack still faster than release. A meter that lags a signal appearing is useless; one
  //      that falls back gently is readable.
  sigSmooth += (norm - sigSmooth) * (norm > sigSmooth ? 0.30 : 0.12);
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
/** ★ The signal meter's FIXED scale, in dBFS — deliberately the same span as the S-unit ladder
 *  below (S1 = -115, S9+60 = -13), with a little room under S1. It is NOT the waterfall's
 *  auto-contrast range: that one re-fits to whatever is on screen, which made the meter and the
 *  squelch needle drift with zoom. See updateSignal(). */
const METER_MIN_DBFS = -121, METER_MAX_DBFS = -13;

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
  // ★★★ ONE FORMAT, ALWAYS. This dropped the decimal above 10 fps, so a rate hovering around ten
  //     alternated between "9.7" and "11" — the string changed LENGTH twice a second, the stats
  //     line grew and shrank, and the signal and AGC meters beside it wobbled with it (Stuart,
  //     2026-08-23). A readout that changes width is a readout that moves its neighbours.
  //  ★ Fixed digits are only half of it: tabular-nums in the CSS stops 1 being narrower than 8,
  //    which is the same wobble one decimal place further down.
  //  ★★ AND PADDED TO A FIXED WIDTH, which one decimal place alone does NOT give: "9.7" is three
  //     characters and "10.6" is four, so the line still changed length every time the rate
  //     crossed ten — which is precisely where it was hovering. U+2007 is a FIGURE SPACE, the
  //     width of a digit, so the padding lines up exactly with tabular figures beside it.
  const fps = framesPerSec.toFixed(1).padStart(4, '\u2007');
  // ★★★ UNDERRUNS ARE SHOWN, and only once there has been one. A stutter is the single most
  //     reported audio fault and every vantage point the page had — bytes arriving, frames
  //     decoding, even a clean recording — is measured BEFORE playout, so all causes looked
  //     alike. This counter says whether the sound card ran dry (starvation: link, main-thread
  //     stall, or the server pausing) or did not (then the fault is in what we handed it).
  //  ★ Absent at zero, so a healthy listener never sees a scary-looking counter at 0.
  // ★★★ NOT ON THE STATUS LINE. These were added to chase the AGC stutter (2026-08-21) and went
  //     out to the public demo, where a listener saw "1 dry" over the waterfall — Stuart: "oh crap
  //     we've left diagnostics visible in the webclient". A counter that means nothing to the
  //     person reading it is worse than no counter: it reads as a fault report on a receiver that
  //     is working.
  //  ★ Kept, in the TOOLTIP, because they earned their place — `dry` (playout ran out) and `skip`
  //    (audio arrived faster than it could be played) are opposite faults that sound identical,
  //    and they are what separated a starved sink from a stalled one. Available on hover when
  //    somebody is actually diagnosing, invisible when nobody is.
  /* ★★★ THE BUFFER DEPTH, BECAUSE IT EXPLAINS THE ONE COMPLAINT NOTHING ELSE DOES. It is
   *     adaptive — 150 ms to start, +60 ms per underrun, ceiling 400 — and `wf.setHoldMs` holds the
   *     WATERFALL back by exactly this much so picture and sound stay together. So a link that has
   *     driven it up does not merely delay the audio, it delays the whole display, and the receiver
   *     feels sluggish to tune while sounding perfectly clean. That is "really laggy for tuning,
   *     but no audio drops" (Stuart, 2026-08-26, on another owner's server).
   *  ★★ IT WAS COMPUTED, USED, AND SHOWN TO NOBODY. The single number that distinguishes "this
   *     link is bursty" from "this receiver is slow" was already in hand and unreadable, so the
   *     question could only be argued rather than looked up.
   *  ★ A depth is a statistic, not a fault — unlike the dry/skip counters above, which read as an
   *    error report to a listener and are deliberately kept in the tooltip. */
  const buf = audio ? ` · buf ${audio.jitterMs.toFixed(0).padStart(3, '\u2007')} ms` : '';
  /* ★★ LABEL THE MILLISECONDS, BECAUSE THERE ARE NOW TWO OF THEM AND THEY MEAN OPPOSITE THINGS.
   *  A bare "29 ms" beside "buf 150 ms" reads as a second buffer figure — Stuart misread his own
   *  readout that way (2026-08-26) while we were using it to tell a bursty link from a slow box,
   *  which is exactly the moment ambiguity costs something. "ping" rather than "rtt": it is the
   *  word a listener already knows, and this row is read by listeners, not only by us. */
  el.textContent = `${total.toFixed(0)} KB/s · ${fps} fps · ping ${rtt.toFixed(0)} ms${buf}${idle}`;
  // ★ And say what they MEAN on hover — the row has room for a label, not for a sentence.
  el.title = `spectrum ${specKbps.toFixed(0)} KB/s · audio ${audioKbps.toFixed(0)} KB/s`
    + ` · asking for ${wantedFps()} fps`
    + ` · ping = round trip to the receiver`
    + (audio ? ` · buf = audio buffered ahead (grows on a bursty link; the waterfall is held`
             + ` back to match, so a big buffer feels laggy to tune)` : '')
    + (audio && (audio.underruns || audio.skips)
        ? ` · audio: ${audio.underruns} dry, ${audio.skips} skip` : '');

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

  // ★★ THE SQUELCH IS SAID ONCE, in the SNR field beside the frequency, where the eye already is
  //    when you are wondering why there is no audio. There used to be a second chip down in the
  //    status row saying the same thing — and because the two were separate elements they BREATHED
  //    OUT OF STEP, which reads as two different states rather than one said twice (Stuart,
  //    2026-08-21). Removed rather than synchronised: the fix for saying something twice is to say
  //    it once.
}

// ── Controls ─────────────────────────────────────────────────────────────────

// ── ★★★ THE LANDING-PAGE SPECTROGRAM ────────────────────────────────────────────────────────
// The server's own 24-hour record of the locked band, drawn as the splash background. It is
// SERVER-side (see spectroFeed in the shim), so it is already populated the first time anyone
// visits and it survives a reload — a landing page whose history starts empty on every visit has
// nothing to say.
//
// Wire format from GET /vibeserver/spectrogram (binary, because 1440x512 as JSON text would be
// megabytes for a picture):
//   "VSPG" | u8 ver | u16 bins | u16 rows | f64 centreHz | f64 spanHz
//   then per row: i64 epoch-ms, then `bins` bytes of dB
/** ★★★ EVERY RADIO ON THIS MACHINE, WITH ITS LIVE STATE.
 *
 *  The directory (`/vibeserver/radios`) comes from the config file and is honest about being a
 *  directory: it knows what exists and on which port, not who is listening. Each radio is its own
 *  PROCESS, so only that process knows whether it is busy — which is why the live half is fetched
 *  from each radio's own `/vibeserver.json` rather than aggregated on the server. It also means a
 *  radio that is down simply does not answer, and says so, instead of the directory claiming it is
 *  fine.
 *
 *  ★★ FULL MEANS GREYED OUT, AND SAYING WHEN. A receiver you cannot have is worth showing — but
 *     only if the page says why and for how long. Every server already publishes `freeInSec` for
 *     its own queue, so the countdown here is the same number the queue screen uses rather than a
 *     second guess at it.
 *  ★ Nothing renders for a single-radio machine: the landing page it has always had is correct,
 *    and a list of one is just noise.
 */
/** ★★★ ONE IMPLEMENTATION OF "WHAT DOES THIS RADIO SAY", used by the first paint AND by the live
 *  refresh below. Two copies of this would drift the moment either was touched, and the drift would
 *  be invisible: the card would simply disagree with itself between refreshes. This file has paid
 *  for that lesson more than once (the tour cards, the macOS download block).
 *  ★ `blocked` is the same test the card's clickability uses — a radio that is full is unreachable
 *    unless you are the admin, and a radio that is DOWN is unreachable for everybody. */
/** ★★★ THE FRESHEST STATUS PER RADIO, because the CARD refreshes and the CLICK HANDLER does not.
 *  refreshSplashRadios() updates the state text in place every 10 s (deliberately — a redraw can
 *  swallow a click), but the takeover confirm was built by the LAST FULL RENDER and closed over
 *  the status object from that moment. So the card said FREE while the dialog quoted a countdown
 *  from minutes earlier: "the page says the airspy is free but the admin dialog is saying 1:17
 *  left" (Stuart, 2026-08-20). Anything a handler decides with must be READ AT CLICK TIME, and
 *  this is where it is read from. Keyed by serial, written by both renderers.
 *  ★★ THE GENERAL SHAPE, worth remembering: an in-place refresh leaves every closure behind it
 *     stale, and the staleness is invisible because the visible half is correct. */
const latestRadioStatus = new Map<string, any>();

function radioCardState(r: any, st: any): { state: string; blocked: boolean } {
  const mmss = (sec: number) => {
    const m = Math.floor(sec / 60), sx = Math.max(0, Math.floor(sec % 60));
    return `${m}:${String(sx).padStart(2, '0')}`;
  };
  const listeners = Number(st?.listeners) || 0;
  const max = Number(st?.maxUsers) || Number(r.users) || 1;
  const waiting = Number(st?.waiting) || 0;
  const freeIn = typeof st?.freeInSec === 'number' ? st.freeInSec : -1;
  const down = !st;
  // ★★★ CLAIMABLE MEANS SAY "FREE", EVEN THOUGH SOMEBODY IS ON IT. On a soft-limit server a
  //     listener past their guarantee holds the radio only until somebody wants it — so the slot
  //     IS available to whoever arrives, and the machinery to hand it over is the server's
  //     business, not something a stranger should have to reason about.
  // ★★★ Stuart, 2026-08-19, and the reasoning is the whole point: present it as free "otherwise
  //     new visitors may be put off using a radio if someone is lingering on it". Nobody should
  //     feel they are kicking a stranger off when the rule is on their side.
  // ★★ THE ADMIN VIEWS ARE NOT TOUCHED BY THIS. `listeners` stays true, so the admin table and the
  //    connection stats keep showing what is actually happening — "obviously the admin menu and
  //    connection stats should still show its actual status" (same conversation). The polite
  //    fiction lives on this card and nowhere else.
  // ★ Absent = today's behaviour, so an older server still reads as IN USE.
  const claimable = st?.claimable === true;
  const full = !down && max > 0 && listeners >= max && !claimable;
  const admin = inAdminMode();
  let state: string;
  if (down)         state = 'NOT RESPONDING';
  else if (full && admin) state = freeIn >= 0 ? `IN USE ${mmss(freeIn)} · TAKE OVER`
                                              : 'IN USE · TAKE OVER';
  else if (full && freeIn >= 0) state = `FULL · FREE IN ${mmss(freeIn)}`;
  else if (full && waiting > 0) state = `FULL · ${waiting} WAITING`;
  else if (full)    state = 'IN USE';
  // ★★ A CLAIMABLE SHARED RADIO WOULD OTHERWISE READ "2 OF 2 LISTENING", which is true and looks
  //    exactly like FULL — the impression this decision exists to avoid. Say FREE, because it is.
  else if (claimable) state = 'FREE';
  else if (max > 1) state = `${listeners} OF ${max} LISTENING`;
  else              state = 'FREE';
  return { state, blocked: down || (full && !admin) };
}

/** ★★★ THE AERIAL ICONS — ONE SET, DRAWN THE SAME EVERYWHERE.
 *
 *  Line art rather than emoji, deliberately: this page is monospace amber on black and an emoji
 *  is a full-colour cartoon in the middle of it — which is exactly what the satellite dish looked
 *  like (Stuart, 2026-08-19). These inherit currentColor, so they dim with the text they sit in.
 *
 *  ★★ KEYED, AND THE KEY IS WHAT TRAVELS (RadioConfig::antennaIcon). An unknown key falls back to
 *     `wire` rather than drawing nothing, so a server newer than this client still renders.
 *  ★ 24x24, stroke-only, no fill: they sit at 11px beside the description and a filled shape at
 *    that size is a blob. */
/** ★★★ THE AERIAL ICONS — ONE SET, AND THE KEYS ARE THE WIRE FORMAT.
 *
 *  ★★★ THESE ARE DUPLICATED IN vibe_setup_page.h, which draws the same eleven in its picker. Two
 *      copies is the cost of the page being a C++ raw string; the KEYS are what must never drift,
 *      because they are what a config stores and what every other client will look up. Change a
 *      drawing freely — change or remove a key and an owner's saved choice stops resolving.
 *
 *  ★★ Stroke-only line art, inheriting currentColor so it dims with the text it sits in. The tip
 *     BALLS are filled and the loop's circle is not: the parent <svg> sets fill:none, so a bare
 *     <circle> draws as a ring, which reads as a small loop on a stick rather than the end of an
 *     aerial (Stuart, 2026-08-19: "the ball needs to be solid so it looks like a tip").
 *
 *  ★ Drawn on a 24x24 grid and shown at 12px, which is the size that decided every one of them. */
const ANT_ICONS: Record<string, string> = {
  vertical:    '<circle cx="12" cy="3.4" r="1.5" fill="currentColor" stroke="none"/>'
             + '<path d="M12 5.2V21"/>',
  groundplane: '<circle cx="12" cy="3" r="1.5" fill="currentColor" stroke="none"/>'
             + '<path d="M12 4.8v8.7M12 13.5l-7 5M12 13.5l7 5"/>',
  // ★ The taper is the point, and it needs three stroke widths in one drawing — which is why
  //   these are raw markup rather than a single path string.
  whip:        '<circle cx="12" cy="3.5" r="1.6" fill="currentColor" stroke="none"/>'
             + '<path d="M12 5.1v4.3" stroke-width="1"/>'
             + '<path d="M12 9.4v5.2" stroke-width="1.9"/>'
             + '<path d="M12 14.6V21" stroke-width="2.9"/>',
  // ★★ The disc sits at the TOP of the cone, where the whip meets it — this was drawn upside down
  //    once, cone-down onto a base rail, until Stuart sent a photograph of a real one.
  discone:     '<circle cx="12" cy="2.8" r="1.5" fill="currentColor" stroke="none"/>'
             + '<path d="M12 4.6v6.6M3.5 11.2h17M12 11.2L6.2 20.5M12 11.2L17.8 20.5"/>',
  dipole:      '<path d="M3 8h8M13 8h8M12 9v11"/>',
  longwire:    '<path d="M3 6v4M21 6v4M3 8c6 5 12 5 18 0M12 10.5V20"/>',
  loop:        '<circle cx="12" cy="9" r="6"/><path d="M10.6 14.8L11.4 20M13.4 14.8L12.6 20"/>',
  // ★ Fed like `loop` — gap in the conductor, twin leads — so the two read as one family.
  deltaloop:   '<path d="M12 3.4L4.4 15.6M12 3.4L19.6 15.6M4.4 15.6h6.3M13.3 15.6h6.3'
             + 'M11 16.4V21M13 16.4V21"/>',
  // ★ Two helices meeting at top, middle and bottom. No mast: the crossings imply the axis, and
  //   a third line down the centre is mud at 12px.
  qfh:         '<path d="M12 3.5C6.5 6 6.5 9.5 12 12C17.5 14.5 17.5 18 12 20.5"/>'
             + '<path d="M12 3.5C17.5 6 17.5 9.5 12 12C6.5 14.5 6.5 18 12 20.5"/>',
  yagi:        '<path d="M4 12h16M6 5v14M10 7.5v9M14 9v6M18 10.5v3"/>',
  dish:        '<path d="M6 4a9 9 0 0 1 0 15M6 11.5h6M12 11.5V20"/>',
};

/** The aerial icon for a radio, as inline SVG — or nothing at all.
 *  ★★★ NO FALLBACK DRAWING. An owner who has not chosen gets no icon, rather than one we picked
 *      for them: guessing would put a vertical beside a description that says "loop", which is
 *      worse than a bare line of text and is the kind of inferred detail this project has already
 *      decided against elsewhere. It also means every server that never touches this looks
 *      exactly as it does today.
 *  ★ An UNKNOWN key is treated the same as none — a server newer than this client draws nothing
 *    rather than the wrong aerial. */
function antIcon(key?: unknown): string {
  const d = typeof key === 'string' ? ANT_ICONS[key] : undefined;
  if (!d) return '';
  return `<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" `
       + `stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" `
       + `style="vertical-align:-2px;margin-right:5px;opacity:.9">${d}</svg>`;
}

/** ★★★ THE OWNER'S PERMANENT MESSAGE ON THE LANDING SCREEN — house rules, why the waterfall
 *  slows down, or a link if people can chip in.
 *
 *  ★★★ NOT the same thing as showOwnerNotice(). That one is TRANSIENT — it expires in minutes, is
 *      dismissible, and is drawn over the receiver once you are connected, because it answers
 *      "something is happening right now". This is permanent and sits on the landing screen
 *      BEFORE you commit. Sharing one mechanism would mean a donation link that expires, or a
 *      maintenance warning nobody can dismiss.
 *
 *  ★★★ THE LINK IS A SEPARATE FIELD AND ITS SCHEME IS RE-CHECKED HERE. The server checked it when
 *      it loaded the config and again when it served the directory — and this checks it a third
 *      time, because THIS is the line that actually builds the href. `javascript:` in an href runs
 *      on click; "the server would never send that" is exactly the assumption that makes a
 *      cross-site hole, and this client talks to servers we do not control.
 *  ★ Text is escaped and never linkified: parsing owner prose into markup is the other way this
 *    goes wrong, and a discrete URL field needs no parsing at all. */
function showLandingMessage(text?: unknown, linkUrl?: unknown, linkLabel?: unknown): void {
  const host = document.getElementById('splashOwnerMsg');
  if (!host) return;
  const msg = typeof text === 'string' ? text.trim() : '';
  const rawUrl = typeof linkUrl === 'string' ? linkUrl.trim() : '';
  const url = /^https?:\/\//i.test(rawUrl) ? rawUrl : '';
  if (!msg && !url) { host.innerHTML = ''; host.style.display = 'none'; return; }
  const label = (typeof linkLabel === 'string' && linkLabel.trim())
    ? linkLabel.trim()
    // ★ Fall back to the HOST, not the whole URL: a bare tracking-laden link as anchor text is
    //   unreadable, and the host is the part that tells you where you are being sent.
    : (() => { try { return new URL(url).host; } catch { return url; } })();
  host.style.display = '';
  // ★★ CENTRED FOR A SENTENCE, RANGED LEFT FOR A PARAGRAPH. This was sized for a one-liner, and
  //    Stuart's first real message was 407 characters — which centring turns into twelve short
  //    ragged lines with both edges moving, the hardest thing to read on the page. A block that
  //    stays centred while its TEXT ranges left keeps the layout symmetrical and the prose
  //    readable. ★ 90 characters is roughly where a message stops being a caption.
  const long = msg.length > 90;
  host.style.textAlign = long ? 'left' : 'center';
  host.style.maxWidth = long ? '560px' : '340px';
  host.innerHTML =
      (msg ? `<div style="white-space:pre-wrap">${escapeHtml(msg)}</div>` : '')
    + (url ? `<div style="margin-top:6px"><a href="${escapeHtml(url)}" target="_blank" `
           // ★ noopener is not decoration: without it the opened page gets window.opener and can
           //   navigate this tab somewhere else.
           + `rel="noopener noreferrer" style="color:var(--amber)">${escapeHtml(label)}</a></div>` : '');
}

async function showSplashRadios(): Promise<void> {
  const host = document.getElementById('splashRadios');
  if (!host) return;
  let dir: any;
  try {
    const r = await fetch(P('/vibeserver/radios'), { cache: 'no-store' });
    if (!r.ok) return;
    dir = await r.json();
  } catch { return; }
  const radios: any[] = Array.isArray(dir?.radios) ? dir.radios : [];

  // ★★★ ON THE FRONT DOOR THERE IS NOTHING TO START. It owns no radio, so START has nothing to
  //     connect to — it tried anyway and failed with "enter a server address" — and the listener
  //     count and range above it belong to a radio, not to a door. You pick a radio instead.
  isFrontDoor = dir?.frontDoor === true;
  if (dir?.frontDoor) {
    const hide = (id: string) => { const e = document.getElementById(id); if (e) e.style.display = 'none'; };
    // ★★ NOT the whole form — ADMIN lives inside it, and ADMIN is the entire reason this process
    //    exists: it is how an owner gets in when every radio has died. Hide only the parts that
    //    connect to a radio this process does not have.
    hide('btnConnect');          // START — a radio card is the start button here
    hide('btnSaveConnect');
    hide('pinRow');              // the PIN gates a RADIO, and there is none behind this door
    hide('hostRow');
    hide('splashListeners');     // a listener count and a range belong to a radio, not a door
  }

  // ★★ THE OWNER'S STANDING MESSAGE, from the directory. Rendered before the early return below,
  //    because a machine with ONE radio has no picker and still has something to say.
  if (dir?.landingMessage || dir?.landingLinkUrl)
    showLandingMessage(dir.landingMessage, dir.landingLinkUrl, dir.landingLinkLabel);

  if (radios.length < 2 && !dir?.frontDoor) { host.innerHTML = ''; return; }

  // ★★★ ONE AERIAL LINE, NOT TWO. Past this point we are drawing CARDS, and each card carries its
  //     own aerial — so the standalone line above the list is the same sentence twice (Stuart,
  //     2026-08-19: "the antenna detail in the box per radio is perfect, it doesnt need to be
  //     separate like it is as well").
  // ★★ It is not dead code: it is the ONLY place a SIMPLE one-radio machine can show an aerial,
  //    because that server never gets here — it took the early return above. loadOwnerNotice()
  //    fills it from /vibeserver.json before this runs, which is why it has to be cleared HERE
  //    rather than simply not set: by now it may already be on screen.
  const solo = document.getElementById('splashAntenna');
  if (solo) { solo.textContent = ''; solo.style.display = 'none'; }

  // ★ Ask every radio at once. One slow or dead radio must not hold up the others: each entry
  //   resolves independently and an unreachable one is rendered as unreachable.
  const live = await Promise.all(radios.map(async (r) => {
    try {
      // ★ Same origin, different PATH — the front door routes it. Asking each radio's own port
      //   would need every one of them forwarded, which is the thing this exists to avoid.
      // ★★★ LINK BY THE OPAQUE ID, NOT THE SERIAL. The serial ended up in the address bar, in
      //     history, in bookmarks and in any link a listener shared. `id` is derived from it and
      //     resolves to the same radio; the server still accepts the serial, so an older link and
      //     an older server both keep working.
      const rid = encodeURIComponent((r as any).id || r.serial);
      const base = `${location.origin}/r/${rid}`;
      const resp = await fetch(`${base}/vibeserver.json`, { cache: 'no-store' });
      if (!resp.ok) { latestRadioStatus.delete(r.serial); return null; }
      const j = await resp.json();
      latestRadioStatus.set(r.serial, j);
      return j;
    } catch { return null; }
  }));

  const mhz = (v: number) => (v / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
  const mmss = (sec: number) => {
    const m = Math.floor(sec / 60), s = Math.max(0, Math.floor(sec % 60));
    return `${m}:${String(s).padStart(2, '0')}`;
  };

  host.innerHTML = radios.map((r, i) => {
    const st = live[i];
    const listeners = Number(st?.listeners) || 0;
    const max = Number(st?.maxUsers) || Number(r.users) || 1;
    const down = !st;
    // ★ Text and reachability both from radioCardState — see the note on it.
    // ★★★ BOTH VALUES FROM ONE PLACE. This took `state` from radioCardState and then computed
    //     `full` ITSELF for the clickability — two definitions of "is this radio available", and
    //     they drifted the moment one of them learned about `claimable`. A soft-limit radio held
    //     past its guarantee said FREE and was greyed out and unclickable, so nobody could take it
    //     and the limit had effectively become unlimited (Stuart, 2026-08-19).
    // ★★ That is exactly what radioCardState's own comment warns about — "two copies of this would
    //    drift the moment either was touched, and the drift would be invisible". It was written to
    //    stop this and the caller quietly kept its own copy anyway.
    const { state, blocked } = radioCardState(r, st);

    const lo = Number(st?.rangeLo) || 0, hi = Number(st?.rangeHi) || 0;
    // ★★★ SAY WHAT IT COVERS, NOT WHERE IT IS PARKED. This showed a single frequency for any radio
    //     that was not locked, so a dongle reaching 1.7 GHz and an HF+ that stops at 31 MHz looked
    //     the same — one number each, and nothing to choose between them (Stuart, 2026-08-09).
    // ★★ A LOCKED RADIO IS ITS WINDOW and needs no qualifier: "1 kHz – 2000 MHz, restrictions in
    //    place" about a receiver fixed to 2.8–10.8 MHz is true and useless. State the window.
    // ★ The label is only for a radio that CAN roam whose owner has allowed or blocked bands —
    //   that is the case where a listener needs the detail, so that is where the tooltip goes.
    const hzTxt = (h: number) => h >= 1e6
      ? `${(h / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MHz`
      : `${Math.round(h / 1e3)} kHz`;
    const cov: [number, number][] = Array.isArray((r as any).coverage) ? (r as any).coverage : [];
    const restricted = !!(r as any).restricted;
    const lists = [ (r as any).allowList ? `Allowed: ${(r as any).allowList}` : '',
                    (r as any).blockList ? `Blocked: ${(r as any).blockList}` : '' ]
                  .filter(Boolean).join(' — ');
    // ★★★ SAY WHAT A LISTENER MAY ACTUALLY TUNE, IN WORDS WHERE THERE ARE WORDS. A restricted
    //     radio recited the HARDWARE's reach and then apologised for it — "1 kHz – 31 MHz,
    //     60 MHz – 260 MHz · RESTRICTIONS IN PLACE" — which describes a receiver nobody can use as
    //     advertised, and buries what it is FOR (Stuart, 2026-08-20). The server names the
    //     permitted bands from its own region-aware plan; we prefer those words to any numbers.
    //  ★ `allowed` (the permitted RANGES) is the fallback when a slice has no name, because an
    //    owner's arbitrary window is real and unnameable — better its figures than a vague label.
    const named: string[] = Array.isArray((r as any).allowedNames) ? (r as any).allowedNames : [];
    const allowed: [number, number][] = Array.isArray((r as any).allowed) ? (r as any).allowed : [];
    let range: string;
    let rangeTitle = '';
    if (lo > 0 && hi > lo) {
      range = `${mhz(lo)} – ${mhz(hi)} MHz`;
    } else if (restricted && named.length) {
      range = named.join(', ');
      rangeTitle = lists || 'The operator has limited where this receiver may tune.';
    } else if (restricted && allowed.length) {
      range = allowed.map(([a, b]) => `${hzTxt(a)} – ${hzTxt(b)}`).join(', ');
      rangeTitle = lists || 'The operator has limited where this receiver may tune.';
    } else if (cov.length) {
      range = cov.map(([a, b]) => `${hzTxt(a)} – ${hzTxt(b)}`).join(', ')
            + (restricted ? ' · RESTRICTIONS IN PLACE' : ' · UNRESTRICTED');
      if (restricted) rangeTitle = lists || 'The operator has limited where this receiver may tune.';
    } else {
      range = `${mhz(Number(r.centreHz) || 0)} MHz`;
    }
    // ★★★ "SHARED" WAS TRUE OF TWO COMPLETELY DIFFERENT RECEIVERS. A locked-centre radio gives
    //     every listener their own VFO inside one window; an unlocked one gives them all the same
    //     dial, which is a different experience and the one worth coming for. The word said
    //     neither (Stuart, 2026-08-20: "we need to differentiate the tuning").
    // ★ Read from the DIRECTORY's `locked`, which is the radio's mode — not from whether a centre
    //   happens to be set, so the two can never disagree.
    // ★ ONE TEST, TWO USES — the wording below and the etiquette line are the same fact, so they
    //   are read from the same expression rather than each deciding for itself.
    const sharedDial = max > 1 && !(r as any).locked;
    const kind = max <= 1 ? 'one listener at a time'
               : (r as any).locked ? 'individual VFOs · locked RF centre'
                                   : 'shared VFO · unlocked RF centre';
    // ★ A radio that is full or down is not a link. Greying it out but leaving it clickable would
    //   send someone to a page that refuses them, which is worse than saying so here.
    // ★★★ UNLESS YOU ARE THE ADMIN. The owner can take a radio back off whoever is using it, so a
    //     full one must stay reachable for them — that is the whole point of signing in here. A
    //     radio that is DOWN stays unreachable for everybody: there is nothing to take over, and
    //     offering it would just be a link to a failure.
    // ★ From the DIRECTORY entry, not the live probe: a radio that is down still has an aerial,
    //   and that is exactly when a visitor is deciding whether to come back for it.
    const ant = String((r as any).antenna || '').trim();
    const admin = inAdminMode();
    const dim = blocked ? 'opacity:.45;cursor:not-allowed' : 'cursor:pointer';
    const tag = blocked ? 'div' : 'a';
    // ★ ?join so the click opens the RECEIVER. Without it the primary's card reloads this very
    //   page, and a secondary's card shows that radio's own copy of this list.
    const href = blocked ? ''
               : ` href="${location.origin}/r/${encodeURIComponent((r as any).id || r.serial)}/?join=1"`;
    return `<${tag}${href} class="radioCard" data-serial="${r.serial}" style="display:block;text-align:left;`
         + `border:1px solid rgba(255,176,0,.35);border-radius:8px;padding:10px 12px;margin:8px 0;`
         + `text-decoration:none;color:inherit;${dim}">`
         + `<div style="display:flex;justify-content:space-between;gap:12px">`
         + `<strong style="letter-spacing:.05em">${escapeHtml(r.label)}</strong>`
         + `<span class="rcState" style="font-size:11px;opacity:.85">${state}</span></div>`
         + `<div class="sub" style="margin-top:2px;font-size:11px;opacity:.7"${
              rangeTitle ? ` title="${rangeTitle.replace(/"/g, '&quot;')}"` : ''
            }>${range} · ${kind}</div>`
         // ★★ THE AERIAL, UNDER THE RANGE IT QUALIFIES. That order is the point: the range says
         //    where this radio CAN tune, and the aerial says where it will actually hear anything.
         //    A dongle advertising 1 kHz – 1.7 GHz above "Discone, good to 300 MHz" tells a visitor
         //    something neither line says alone.
         // ★ ESCAPED — owner-authored text going into innerHTML. So is the label beside it, which
         //   was not before: it comes from the same place and had the same hole.
         + (ant ? `<div class="sub" style="margin-top:2px;font-size:11px;opacity:.55">`
                + `${antIcon((r as any).antennaIcon)}${escapeHtml(ant)}</div>` : '')
         // ★★★ SAY THE ETIQUETTE BEFORE THEY ARE IN, NOT AFTER THEY HAVE MOVED THE DIAL. On a
         //     SHARED VFO everyone is on the same dial, so tuning is not a private act — it takes
         //     the station away from everybody else listening. A newcomer has no way to know that
         //     from a card that says "shared VFO · unlocked RF centre": that names the mechanism,
         //     not the courtesy it asks for (Stuart, 2026-08-22).
         //  ★★ ONLY in that mode. On a locked centre each listener has their own VFO inside the
         //     window and may tune freely — telling them to ask would be false, and a rule that is
         //     sometimes untrue teaches people to ignore the rest of the card.
         //  ★ There is a chat on the receiver, which is what "ask" means in practice.
         + (sharedDial ? `<div class="sub" style="margin-top:3px;font-size:11px;opacity:.75;`
                       + `letter-spacing:.06em">ASK BEFORE TUNING &mdash; everyone here shares one dial`
                       + `</div>` : '')
         + `</${tag}>`;
  }).join('');

  // ★★★ AN ADMIN MUST NOT BOOT SOMEONE WITHOUT MEANING TO. Stuart, 2026-07-28: "some admins may
  //     be kind and let a user keep a session for longer". Taking over a busy radio disconnects
  //     whoever is listening, so the click asks first and says how long they had left — the owner
  //     can then decide to wait. A FREE radio costs nobody anything, so it just opens.
  if (inAdminMode()) {
    host.querySelectorAll('a.radioCard').forEach((el) => {
      const a = el as HTMLAnchorElement;
      const i = radios.findIndex((r) => r.serial === a.dataset.serial);
      if (i < 0) return;
      // ★★★ ATTACH ALWAYS, DECIDE AT CLICK. Whether a radio is busy was read up to ten seconds
      //     ago; the previous code used it to decide whether to attach the handler AT ALL, so a
      //     radio that filled up after the render got no confirm and one that emptied got a
      //     dialog about a listener who had left. Nothing here is decided until the click.
      a.addEventListener('click', (ev) => {
        // ★★★ RE-READ THE STATUS. refreshSplashRadios() updates the card text in place without
        //     rebuilding these handlers, so anything captured at render time is stale by design —
        //     that is what put "they have 1:17 left" under a card saying FREE (Stuart,
        //     2026-08-20).
        const cur = latestRadioStatus.get(radios[i].serial);
        const curMax = Number(cur?.maxUsers) || Number(radios[i].users) || 1;
        // ★★★ CLAIMABLE IS NOT BUSY, and neither is empty. Past the guarantee the incumbent holds
        //     the slot only until somebody wants it, so arriving IS the rule working — the card
        //     says FREE for exactly this case and a dialog asking whether to disconnect them
        //     contradicts it and makes the arriving user feel like a queue-jumper ("should not be
        //     made to feel guilty for taking it over"). They see the handover countdown and
        //     nothing else.
        const busyNow = !!cur && cur.claimable !== true
                        && curMax > 0 && (Number(cur.listeners) || 0) >= curMax;
        if (!busyNow) return;                       // the link just follows
        const left = typeof cur.freeInSec === 'number' && cur.freeInSec > 0
          ? ` They have ${mmss(cur.freeInSec)} left.` : '';
        const who = curMax > 1 ? `${curMax} listeners are` : 'Someone is';
        if (!confirm(`${who} using ${radios[i].label}.${left}\n\n`
                   + `Taking over will disconnect ${curMax > 1 ? 'one of them' : 'them'}. Continue?`))
          ev.preventDefault();
        // ★ Answering that confirm is the deliberate act — see the takeover flag in connect().
        //   Without it the navigation that follows arrives as an ordinary admin reconnect and
        //   would queue behind the very listener the owner just chose to displace.
        else sessionStorage.setItem('vsTakeover', '1');
      });
    });
  }
}

/** ★★ WHO IS ALREADY HERE. Stuart asked for the count on the landing page as well as in the
 *  status bar — and it is the same question the queue answers: a visitor deciding whether to
 *  bother wants to know there is room BEFORE they press START, not after they are refused.
 *  ★ Silent on failure. This is decoration on a page that must work without it; a server that
 *    does not answer (or an older one with no `waiting` field) simply shows nothing. */
async function showSplashListeners(): Promise<void> {
  // ★ The per-radio list refreshes on the same beat, so a countdown on the landing page ticks
  //   rather than freezing at whatever it was when the page opened.
  void showSplashRadios();
  const el = document.getElementById('splashListeners');
  if (!el) return;
  try {
    // ★ /vibeserver.json — NOT /vibeserver/identity, which does not exist and 404s in
    //   silence behind the try/catch. The endpoint is named for the file it pretends to be.
    const r = await fetch(P('/vibeserver.json'), { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    const n = Number(j.listeners) || 0, max = Number(j.maxUsers) || 0, wait = Number(j.waiting) || 0;
    if (!max) return;
    let txt = `${n} LISTENING OF ${max}`;
    if (wait > 0) txt += ` · ${wait} WAITING`;
    else if (n >= max) txt += ' · FULL';
    // ★ SAY WHAT THIS RECEIVER COVERS, IN WORDS. The spectrogram behind the page shows it on its
    //   axis, but reading a range off an axis is work — and it is the first thing a visitor wants
    //   to know (Stuart, 2026-08-06). Omitted entirely when the centre is not locked: a
    //   free-running dongle has no fixed range and a made-up one would be worse than none.
    const lo = Number(j.rangeLo) || 0, hi = Number(j.rangeHi) || 0;
    if (lo > 0 && hi > lo) {
      const mhz = (v: number) => (v / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
      txt += `\n${mhz(lo)} – ${mhz(hi)} MHz`;
    }
    el.style.whiteSpace = 'pre-line';
    el.textContent = txt;
  } catch { /* decoration only */ }
}

/** ★★★ WHAT THE SUN SUGGESTS, BESIDE WHAT THIS RECEIVER CAN HEAR. The pairing is the feature: a
 *  solar prediction models the ionosphere over a region, while the measured column is one aerial
 *  in one garden — and for somebody deciding whether to listen HERE, the second is the one that
 *  answers the question. Neither alone would.
 *  ★★ ONLY BANDS THIS RECEIVER COVERS. A window centred on 6.5 MHz reaches 80/60/40/30m, so
 *     showing 10m would be listing a verdict nobody here can act on — the same rule as filtering
 *     the search to the tunable window. On an FM profile there is no HF at all and the whole
 *     block stays hidden, which is the correct answer rather than an empty table.
 *  ★ Silent on failure and on an older server: this is decoration on a page that must work
 *    without it. */
async function showSplashConditions(): Promise<void> {
  const el = document.getElementById('splashConditions');
  if (!el) return;
  try {
    const r = await fetch(P('/vibeserver/conditions'), { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    const measured: Array<{ band: string; snrDb: number }> = j.measured || [];
    const solar = j.solar;
    // ★ THE MEASURED LIST DEFINES WHICH BANDS EXIST HERE. It is derived from what is inside the
    //   captured span, so using it as the key set is what makes the profile filter automatic —
    //   nothing here needs to know the centre frequency or the span.
    if (!measured.length) { el.innerHTML = ''; return; }
    // ★★ THE SAME FOUR WORDS IN BOTH COLUMNS. The value of this block is the COMPARISON, and a
    //    reader cannot compare "Good" against "Busy" — they would have to learn two scales to
    //    notice that the prediction and the aerial disagree, which is the one thing worth
    //    noticing. The dB is shown too, because the word is a bucket and the number is the
    //    measurement.
    const rate = (db: number) => db >= 15 ? 'Excellent'
                               : db >= 9  ? 'Good'
                               : db >= 4  ? 'Fair'
                                          : 'Poor';
    const DIV = 'padding:1px 12px;opacity:0.95;border-right:1px solid rgba(255,184,51,0.28)';
    const rows = measured.map((m) => {
      const pred = solar?.bands?.[m.band] || '—';
      // ★ The rule sits on the PREDICTED cell's right edge, so it runs the height of the table
      //   automatically — a separate element would have to be measured and kept in step.
      return `<tr>
        <td style="padding:1px 12px 1px 0;color:var(--amber);text-align:right">${m.band}</td>
        <td style="${DIV}">${pred}</td>
        <td style="padding:1px 0 1px 12px;opacity:0.95">${rate(m.snrDb)}` +
        `<span style="opacity:0.6"> (${m.snrDb.toFixed(0)} dB)</span></td></tr>`;
    }).join('');
    const solarLine = solar ? ` &nbsp;·&nbsp; SFI ${solar.sfi} · K ${solar.kp}` : '';
    el.innerHTML =
      `<div style="opacity:0.8;letter-spacing:1px;margin-bottom:4px">BAND CONDITIONS${solarLine}</div>` +
      `<table style="margin:0 auto;border-collapse:collapse;font-size:11px">` +
      `<tr style="opacity:0.65"><td></td>` +
      `<td style="padding:1px 12px;border-right:1px solid rgba(255,184,51,0.28)">PREDICTED</td>` +
      `<td style="padding-left:12px">ACTUAL</td></tr>${rows}</table>`;
  } catch { /* decoration only */ }
}

async function drawSplashSpectrogram(): Promise<void> {
  const cv = document.getElementById('splashSpectro') as HTMLCanvasElement | null;
  if (!cv) return;
  // ★ Ask for exactly what this canvas can draw. More is wasted bytes on a landing page; fewer
  //   is the blur we are trying to get rid of.
  const wantW = Math.min(2048, Math.max(512, Math.floor(cv.clientWidth  * devicePixelRatio)));
  const wantH = Math.min(1440, Math.max(180, Math.floor(cv.clientHeight * devicePixelRatio)));
  let buf: ArrayBuffer;
  try {
    const r = await fetch(P(`/vibeserver/spectrogram?bins=${wantW}&rows=${wantH}`), { cache: 'no-store' });
    if (!r.ok) return;
    buf = await r.arrayBuffer();
  } catch { return; }
  const dv = new DataView(buf);
  if (buf.byteLength < 25) return;
  if (String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)) !== 'VSPG') return;
  const bins = dv.getUint16(5, true), rows = dv.getUint16(7, true);
  const centre = dv.getFloat64(9, true), span = dv.getFloat64(17, true);
  if (!rows || !bins) return;

  const W = cv.width  = Math.max(640, Math.floor(cv.clientWidth  * devicePixelRatio));
  const H = cv.height = Math.max(360, Math.floor(cv.clientHeight * devicePixelRatio));
  const g = cv.getContext('2d');
  if (!g) return;
  g.fillStyle = '#000'; g.fillRect(0, 0, W, H);

  // ★ Rows are drawn NEWEST AT THE BOTTOM, matching the live waterfall — a visitor should not
  //   have to re-learn which way time runs between the landing page and the receiver.
  const bytes = new Uint8Array(buf);
  // ★★★ AUTO-CONTRAST, from the data itself. A fixed dB window is what made this read as fog:
  //     the noise floor moves with the band, the time of day and the aerial, so a hard-coded
  //     range either crushes everything into one colour or blows the strong carriers flat.
  //     Percentiles rather than min/max — one lightning crash would otherwise set the top of the
  //     scale and darken the entire day.
  const hist = new Uint32Array(256);
  for (let r = 0; r < rows; r++) {
    const off = 25 + r * (8 + bins) + 8;
    for (let i = 0; i < bins; i++) hist[bytes[off + i]]++;
  }
  const total = rows * bins;
  const pct = (p: number) => {
    let seen = 0;
    for (let v = 0; v < 256; v++) { seen += hist[v]; if (seen >= total * p) return v; }
    return 255;
  };
  // ★ Tuned by rendering the Pi's real HF band and LOOKING at it, not by taste. 0.10/0.995 with
  //   a 0.62 gamma washed the noise floor into a glow and the carriers stopped standing out;
  //   0.25/0.999 straight was so dark only the strongest lines showed. This keeps the floor dark
  //   enough to read band structure while carriers stay bright.
  const loV = pct(0.30), hiV = Math.max(loV + 6, pct(0.998));

  /* ★★★ SONAR GREEN, NOT AMBER — because the page's TEXT is amber. The spectrogram is the
   *   backdrop to the whole landing screen, and an amber ramp behind amber type is the one
   *   combination that cannot work: at the levels a busy band produces, the carriers sat at
   *   exactly the brightness of the words in front of them and the text disappeared into the
   *   picture (Stuart, 2026-08-27: "the amber text is not that clear over the spectrogram").
   * ★★ Green puts the backdrop on the OTHER side of the colour wheel from the type, so the two
   *   separate at every level rather than only at the extremes — and it is a palette this app
   *   already ships and Stuart already uses, so the page still looks like the instrument.
   * ★ THE REAL STOPS, interpolated — not a green approximated by eye. Copied from
   *   src/assets/colormaps.ts 'Sonar Green', which is what the waterfall draws, so the landing
   *   page and the receiver agree about what a signal of a given strength looks like. */
  const SONAR_GREEN: [number, number, number][] = [
    [0x00, 0x00, 0x00], [0x00, 0x08, 0x00], [0x00, 0x1a, 0x00], [0x00, 0x33, 0x00],
    [0x00, 0x50, 0x00], [0x00, 0x78, 0x00], [0x00, 0xaa, 0x00], [0x00, 0xcc, 0x00],
    [0x00, 0xff, 0x00], [0x80, 0xff, 0x80], [0xcc, 0xff, 0xcc], [0xef, 0xff, 0xff],
  ];
  const sonarGreen = (t: number): [number, number, number] => {
    const x = Math.max(0, Math.min(1, t)) * (SONAR_GREEN.length - 1);
    const i = Math.min(SONAR_GREEN.length - 2, Math.floor(x));
    const f = x - i;
    const a = SONAR_GREEN[i], b = SONAR_GREEN[i + 1];
    return [Math.round(a[0] + (b[0] - a[0]) * f),
            Math.round(a[1] + (b[1] - a[1]) * f),
            Math.round(a[2] + (b[2] - a[2]) * f)];
  };

  const img = g.createImageData(bins, rows);
  for (let r = 0; r < rows; r++) {
    const off = 25 + r * (8 + bins) + 8;
    for (let i = 0; i < bins; i++) {
      const v = bytes[off + i];
      // Gamma lifts the mid-levels: linear, almost everything sat at the bottom of the ramp.
      const t = Math.pow(Math.max(0, Math.min(1, (v - loV) / (hiV - loV))), 0.80);
      const p = ((rows - 1 - r) * bins + i) * 4;
      const [cr, cg, cb] = sonarGreen(t);
      img.data[p]     = cr;
      img.data[p + 1] = cg;
      img.data[p + 2] = cb;
      img.data[p + 3] = 255;
    }
  }
  // Stretch the (bins x rows) image over the whole panel.
  const tmp = document.createElement('canvas');
  tmp.width = bins; tmp.height = rows;
  tmp.getContext('2d')!.putImageData(img, 0, 0);
  // ★ NEAREST-NEIGHBOUR. We now ask the server for roughly one bin per device pixel, so
  //   smoothing has nothing to interpolate and only softens carriers that are one pixel wide —
  //   which are exactly the ones worth seeing.
  g.imageSmoothingEnabled = false;
  g.drawImage(tmp, 0, 0, W, H);

  // ── STAMPS ───────────────────────────────────────────────────────────────────────────────
  // ★ Stuart asked for both: without them it is a pretty texture, and with them it is a record
  //   you can actually read something off.
  const px = devicePixelRatio;
  g.font = `${11 * px}px ui-monospace, monospace`;
  g.textBaseline = 'top';
  // ★ FULL ALPHA HERE, because the canvas is dimmed TWICE downstream: opacity 0.68 on the element
  //   and a radial black gradient over the middle of it. A label drawn at 0.85 therefore lands
  //   near 0.4 in the corners and far less in the centre. Brightening the pen lifts the LABELS
  //   without touching the image, which is the thing the dimming exists to hold back.
  g.fillStyle = 'rgba(255,205,130,1)';
  g.strokeStyle = 'rgba(255,200,120,0.30)';

  /** ★★★ EVERY LABEL GETS A DARK HALO. The background is live spectrum, so it cannot be designed
   *  around: a strong carrier puts a bright vertical streak straight through a time stamp and
   *  washes it out (Stuart, 2026-08-06). Brightening the text alone does not fix that — bright
   *  text on a bright streak is still unreadable; it needs CONTRAST, which means darkening what
   *  is immediately behind it.
   *  ★★ A shadow rather than a filled rectangle: it costs no layout, needs no measuring, and
   *     leaves no hard edges over a picture whose whole job is to be looked at. Same technique as
   *     the HTML text on this page, so the two match.
   *  ★ Drawn TWICE at low blur rather than once at high: a single wide blur reads as a smudge,
   *    two tight ones read as an outline. */
  const label = (text: string, x: number, y: number) => {
    g.save();
    g.shadowColor = 'rgba(0,0,0,0.95)';
    g.shadowBlur = 4 * px;
    g.fillText(text, x, y);
    g.shadowBlur = 2 * px;
    g.fillText(text, x, y);
    g.restore();
  };
  g.lineWidth = 1 * px;

  // Frequency, across the top. ★ THE GRID LINES ALWAYS DRAW; the LABELS are skipped when they
  //   would collide. Clamping a label back into view instead — which is what this did — stacks
  //   the leftmost two on top of each other on a narrow window and neither is readable.
  //   A missing label is a small loss; two labels superimposed is worse than none.
  let lastRight = -1e9;
  for (let k = 0; k <= 4; k++) {
    const x = (W - 1) * (k / 4);
    const hz = centre - span / 2 + span * (k / 4);
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, H); g.stroke();
    const lbl = `${(hz / 1e6).toFixed(3)} MHz`;
    const w = g.measureText(lbl).width;
    const tx = Math.max(2 * px, Math.min(W - w - 2 * px, x - w / 2));
    if (tx < lastRight + 8 * px) continue;      // would touch the previous one — leave it out
    label(lbl, tx, 4 * px);
    lastRight = tx + w;
  }
  // Time, down the left — oldest at the top, newest at the bottom.
  const tFirst = Number(dv.getBigInt64(25, true));
  const tLast  = Number(dv.getBigInt64(25 + (rows - 1) * (8 + bins), true));
  const hhmm = (ms: number) => {
    const d = new Date(ms);
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  };
  // ★ Same rule down the side, and fewer steps on a short panel — five time stamps in 200 pixels
  //   is a stack, not a scale.
  const steps = H < 260 * px ? 2 : 4;
  let lastBottom = -1e9;
  for (let k = 0; k <= steps; k++) {
    const y = (H - 1) * (k / steps);
    // ★★★ NEWEST AT THE TOP, BECAUSE THAT IS WHERE THE IMAGE PUTS IT — and because it is what the
    //     waterfall does ("blit at top, scroll down", waterfall.ts). The drawing above maps source
    //     row 0 (the OLDEST) to the BOTTOM via `rows - 1 - r`; these labels ran the other way and
    //     claimed the top was oldest. So the picture and its own axis disagreed, and reading the
    //     axis made the spectrogram look like it ran bottom-to-top when it never did (Stuart,
    //     2026-08-06 — he asked for the image to be flipped, which would have made it genuinely
    //     wrong; the labels were the fault).
    //     ★ A time axis that contradicts the picture it labels is worse than no axis: it is
    //       believed, and it is the only thing telling you which end is now.
    const ms = tLast - (tLast - tFirst) * (k / steps);
    g.beginPath(); g.moveTo(0, y); g.lineTo(W, y); g.stroke();
    // ★★ KEEP CLEAR OF THE FREQUENCY ROW. The newest stamp sits at y=0, which is exactly where
    //    the frequency labels are drawn, so "21:16" and "2.500 MHz" printed on top of each other
    //    in the top-left corner (Stuart, 2026-08-05). Push it below that row rather than skipping
    //    it — the oldest time is the one that tells you how far back the picture goes.
    //    ★ Safe to nudge because the collision rule below still runs: if the nudge pushes it into
    //      the next stamp, that one is dropped rather than stacked.
    //    ★★ NOW ALSO CLEAR OF THE CAPTION ROW. The BAND ACTIVITY caption is drawn into the canvas
    //       at y=20px (see below), so a stamp pushed to 19px would share its line — fine on a wide
    //       window where one is left-aligned and the other centred, and a collision the moment the
    //       window narrows. Reserve both rows rather than relying on the width.
    const kTopReserved = 34 * px;
    const ty = Math.min(H - 14 * px, Math.max(kTopReserved, y + 3 * px));
    if (ty < lastBottom + 4 * px) continue;
    label(hhmm(ms), 4 * px, ty);
    lastBottom = ty + 12 * px;
  }
  // ── BAND LABELS ALONG THE BOTTOM ─────────────────────────────────────────────────────────
  // Stuart asked for the same aesthetic as the frequency scale above: |   40M Ham Band   | with
  // the framing rules ON THE LABEL'S OWN ROW, so the band is bracketed rather than underlined.
  // ★ A label is drawn only if its band's visible width can actually hold the text — a clipped
  //   band name reads as a different band, which is worse than an unlabelled stretch of edge.
  const lo = centre - span / 2, hi = centre + span / 2;
  // ★ Clear of #splashSpectroTip, which is an HTML element sitting over the bottom-left of
  //   this canvas — the labels are drawn INTO the image and cannot know about it.
  const bandY = H - 30 * px;
  // ★ BRIGHTER THAN THE GRID, AND ON PURPOSE. These inherited the frequency grid's 0.20 alpha,
  //   which is right for a rule you should read past and wrong for one that carries information:
  //   the brackets were invisible until a band happened to fall on a quiet stretch of spectrum
  //   (Stuart, 2026-08-05: "I didn't realise they were even there until I saw the 90m band").
  //   A label nobody can see is the same as no label. Restored below so nothing downstream
  //   silently inherits the brighter pen.
  const gridStroke = g.strokeStyle;
  g.strokeStyle = 'rgba(255,200,120,0.55)';
  g.lineWidth = 1.5 * px;
  for (const b of BAND_PLAN) {
    if (b.regions && !b.regions.includes(1)) continue;   // one region's plan, not three overlaid
    if (b.hi <= lo || b.lo >= hi) continue;
    const x0 = ((Math.max(b.lo, lo) - lo) / span) * (W - 1);
    const x1 = ((Math.min(b.hi, hi) - lo) / span) * (W - 1);
    // ★★ FALL BACK TO THE SHORT NAME, THEN TO NO NAME AT ALL — but always draw the frame.
    //    Requiring room for the full name silently dropped EVERY band: across 8 MHz of HF even
    //    40m is about fifty pixels wide and "40M HAM BAND" needs a hundred, so the rule was
    //    correct in principle and drew nothing whatsoever in practice.
    //    ★ The bracket is worth drawing on its own: it shows WHERE the band is, which is most of
    //      the value, and an unlabelled bracket is honest where a clipped name is misleading.
    const full  = b.bandLabel ? `${b.bandLabel.toUpperCase()} ${b.name.replace(b.bandLabel, '').trim()}` : b.name;
    const short_ = b.bandLabel ? b.bandLabel.toUpperCase() : b.name.split(' ')[0];
    const room = x1 - x0;
    // ★ `bandLbl`, not `label` — the halo helper above is called label(), and a local of the same
    //   name would shadow it inside this loop only, so the band names would silently lose their
    //   backing while every other label kept one.
    const bandLbl = room >= g.measureText(full).width + 18 * px ? full
                  : room >= g.measureText(short_).width + 12 * px ? short_
                  : '';
    const w = bandLbl ? g.measureText(bandLbl).width : 0;
    const mid = (x0 + x1) / 2;
    // The two framing rules sit on the label's row and stop short of the text, which is what
    // makes it read as a bracket rather than a strikethrough.
    g.beginPath();
    if (bandLbl) {
      g.moveTo(x0 + 2 * px, bandY + 5 * px); g.lineTo(mid - w / 2 - 6 * px, bandY + 5 * px);
      g.moveTo(mid + w / 2 + 6 * px, bandY + 5 * px); g.lineTo(x1 - 2 * px, bandY + 5 * px);
    } else {
      g.moveTo(x0 + 2 * px, bandY + 5 * px); g.lineTo(x1 - 2 * px, bandY + 5 * px);
    }
    // Uprights at the edges, so the band is closed off at both ends.
    g.moveTo(x0 + 2 * px, bandY); g.lineTo(x0 + 2 * px, bandY + 10 * px);
    g.moveTo(x1 - 2 * px, bandY); g.lineTo(x1 - 2 * px, bandY + 10 * px);
    g.stroke();
    if (bandLbl) label(bandLbl, mid - w / 2, bandY);
  }
  g.strokeStyle = gridStroke;
  g.lineWidth = 1 * px;

  // ★★★ DRAWN INTO THE IMAGE, NOT FLOATED OVER IT. This is a caption for the spectrogram, and as
  //     an HTML element it sat outside the canvas's opacity and the radial dimmer — so it could
  //     never match the frequency, time and band labels no matter how its colour was tuned, and
  //     two values would have to be kept in step by hand forever (Stuart, 2026-08-06). Drawing it
  //     with the same pen on the same canvas makes it match BY CONSTRUCTION.
  const mins = Math.round((tLast - tFirst) / 60000);
  const cap = `BAND ACTIVITY · ${mins < 60 ? mins + ' MIN' : (mins / 60).toFixed(1) + ' H'} · `
            + `${hhmm(tFirst)}–${hhmm(tLast)}`;
  const capW = g.measureText(cap).width;
  label(cap, (W - capW) / 2, 20 * px);
  const tip = document.getElementById('splashSpectroTip');
  if (tip) tip.textContent = '';       // the element stays for layout; the text now lives on canvas
}

/* ── DAB (experimental) ──────────────────────────────────────────────────────────────────────
 *  ★★ OFFERED ONLY WHERE IT CAN WORK. The server decides — it is the only side that knows the
 *     EFFECTIVE limits after allow/block lists and a locked sample rate — and publishes `dab` in
 *     /vibeserver.json. AGENTS.md: never draw a control whose every use is a no-op.
 *  ★ Labelled "DAB (Experimental)" because it is, and because a user who knows to expect rough
 *    edges reports them instead of concluding the receiver is broken.
 */
let dabCapable = false;
let dabOn = false;
let dabState: DabState | null = null;
let dabPane: 'stations' | 'signal' = 'stations';
let dabChannel = -1;
let dabLastEid = -1;
/* ★★★ THE VFO DAB BORROWED, so it can be given back. dabRender points the readout at the
 *  multiplex centre — right while DAB is on — but that is the CLIENT's own frequency field and
 *  nothing put it back on the way out. Stuart, 2026-09-04: back on FM with RDS decoding Heart
 *  perfectly, the readout stuck at 208064 kHz and "tuning is stuck fully" — because every tune he
 *  sent was computed from a dial that thought it was in Band III. */
let dabPrevFreq = 0;

/** Band III, mirroring vibe_dab_channels.h — including the OFFSET blocks 10N/11N/12N, which are
 *  easy to miss and are genuinely on air. */
const DAB_BLOCKS: { name: string; hz: number }[] = [
  ['5A',174928],['5B',176640],['5C',178352],['5D',180064],
  ['6A',181936],['6B',183648],['6C',185360],['6D',187072],
  ['7A',188928],['7B',190640],['7C',192352],['7D',194064],
  ['8A',195936],['8B',197648],['8C',199360],['8D',201072],
  ['9A',202928],['9B',204640],['9C',206352],['9D',208064],
  ['10A',209936],['10N',210096],['10B',211648],['10C',213360],['10D',215072],
  ['11A',216928],['11N',217088],['11B',218640],['11C',220352],['11D',222064],
  ['12A',223936],['12N',224096],['12B',225648],['12C',227360],['12D',229072],
  ['13A',230784],['13B',232496],['13C',234208],['13D',235776],['13E',237488],['13F',239200],
].map(([n, k]) => ({ name: String(n), hz: Number(k) * 1000 }));

function dabSetPane(p: 'stations' | 'signal') {
  dabPane = p;
  const st = document.getElementById('dabStations');
  const sg = document.getElementById('dabSignal');
  const bt = document.getElementById('dabPane');
  if (st) st.style.display = dabOn && p === 'stations' ? 'block' : 'none';
  if (sg) sg.style.display = dabOn && p === 'signal' ? 'block' : 'none';
  // ★ The label names where the button GOES, and the state is readable without pressing it.
  if (bt) bt.innerHTML = p === 'stations' ? 'SIGNAL &#9656;' : '&#9666; STATIONS';
}

function dabRender() {
  const st = document.getElementById('dabStations');
  const sg = document.getElementById('dabSignal');
  const lbl = document.getElementById('dabMuxLbl');
  const d = dabState;
  /* ★★★ THE DIAL FOLLOWS THE MULTIPLEX. The readout is the client's own, driven by the VFO, and
   *  DAB does not move a VFO — so stepping 12B -> 11D left it reading 225657 kHz while the mux bar
   *  said 222.064 (11D). Two readings of one fact, disagreeing, which is the thing the tuning
   *  block exists not to do. */
  if (d && spec && Math.abs(spec.frequency - d.centreHz) > 1) {
    spec.frequency = d.centreHz;
    mobileUi?.refresh();          // the card owns the readout; nudge it rather than paint it here
  }
  if (lbl) lbl.textContent = d
    ? `${(d.centreHz / 1e6).toFixed(3)} (${d.channel})${d.label ? ' — ' + d.label : ''}`
    : (dabChannel >= 0 ? `${(DAB_BLOCKS[dabChannel].hz / 1e6).toFixed(3)} (${DAB_BLOCKS[dabChannel].name})` : '—');
  if (!d || !st || !sg) return;

  /* ★★ RESET TO THE LIST WHEN THE ENSEMBLE CHANGES, and only then. A new multiplex means a new
   *  list and the old figures describe a receiver you have left — but changing SERVICE inside one
   *  mux must NOT bounce you back, because comparing services is exactly what the panel is for.
   *  (Same lesson as the bookmark search: do not throw away the view somebody is working in.) */
  if (d.eid !== dabLastEid) { dabLastEid = d.eid; dabSetPane('stations'); }

  st.innerHTML = d.services.length
    ? d.services.map(sv => `<div class="dabSvc${sv.sid === d.sid ? ' on' : ''}" data-sid="${sv.sid}">`
        + `<span class="nm">${escapeHtml(sv.label || '(unnamed)')}</span>`
        + `<span class="cod">${sv.codec}</span></div>`).join('')
    : `<div style="padding:14px;opacity:.6">${d.locked ? 'Reading the multiplex…' : 'Searching for a multiplex…'}</div>`;
  for (const el of Array.from(st.querySelectorAll('.dabSvc')) as HTMLElement[])
    el.onclick = () => { spec?.dabService(Number(el.dataset.sid)); };

  const row = (k: string, v: string) => `<div class="row"><span>${k}</span><span>${v}</span></div>`;
  sg.innerHTML =
    '<h4>SERVICE</h4>'
    + row('Codec', d.services.find(x => x.sid === d.sid)?.codec ?? '—')
    + row('Bit rate', d.bitrate ? d.bitrate + ' kbit/s' : '—')
    + row('Protection', d.protection || '—')
    + '<h4>ERROR RATE</h4>'
    + row('FIB this frame', `${d.fibOk} / ${d.fibTotal}`)
    + row('FIB pass rate', (d.fibRate * 100).toFixed(1) + ' %')
    + '<h4>MULTIPLEX</h4>'
    + row('Ensemble', d.label || '—')
    + row('EId', '0x' + d.eid.toString(16).toUpperCase().padStart(4, '0'))
    + row('Services', String(d.services.length))
    + '<h4>PHYSICAL LAYER</h4>'
    + row('Lock', d.locked ? 'locked' : 'searching')
    + row('Null depth', d.nullDepthDb.toFixed(1) + ' dB')
    + row('Frequency offset', `${d.offsetHz.toFixed(0)} Hz (${d.offsetPpm.toFixed(2)} ppm)`)
    + row('Carrier shift', String(d.carrierShift))
    + row('Phase reference', d.prs.toFixed(3))
    + row('Frames seen', String(d.frames));
}

function dabTune(delta: number) {
  if (dabChannel < 0) dabChannel = DAB_BLOCKS.findIndex(b => b.name === '12B');
  // ★ CLAMP, never wrap: 13F -> 5A in one press is not what a band scan meant.
  dabChannel = Math.max(0, Math.min(DAB_BLOCKS.length - 1, dabChannel + delta));
  dabState = null;
  spec?.dab(true, dabChannel);
  dabRender();
}

function dabSetMode(on: boolean) {
  dabOn = on;
  const mux = document.getElementById('dabMux');
  const bt  = document.getElementById('dabPane');
  if (mux) mux.style.display = on ? 'flex' : 'none';
  if (bt)  bt.style.display  = on ? '' : 'none';
  const dt = document.getElementById('decText');
  if (dt) dt.style.display = on ? 'none' : '';
  if (on) {
    if (dabChannel < 0) dabChannel = DAB_BLOCKS.findIndex(b => b.name === '12B');
    if (spec && !dabPrevFreq) dabPrevFreq = spec.frequency;   // give it back on the way out
    spec?.dab(true, dabChannel);
    dabSetPane('stations');
    // ★★ The decoder box is ALWAYS OPEN in DAB — the station list IS the tuning UI.
    document.getElementById('decBox')?.classList.add('open');
    /* ★★★ AND IT MUST SAY DAB. Opening the box directly skips openDecoder(), which is what
     *  normally sets the title — so it kept whatever decoder was last used and announced a DAB
     *  ensemble as "RTTY" (Stuart's screenshot, 2026-09-04). The one job of a box header is to
     *  say what you are looking at. */
    const dt2 = document.getElementById('decTitle');
    if (dt2) dt2.textContent = 'DAB';
    const ds2 = document.getElementById('decStatus');
    if (ds2) ds2.textContent = 'tuning…';
  } else {
    spec?.dab(false);
    dabState = null;
    // ★ Hand the dial back — see dabPrevFreq. The server restores its own centre; this is the
    //   client's copy, and the two disagreeing is what left the tuning stuck.
    if (spec && dabPrevFreq) { spec.frequency = dabPrevFreq; mobileUi?.refresh(); }
    dabPrevFreq = 0;
    const ds3 = document.getElementById('decStatus');
    if (ds3) ds3.textContent = '';
  }
  dabSetPane(dabPane);
  dabRender();
}

/* ★★★ THE DESKTOP BAR'S MODE ROW IS GONE. #bar was retired in favour of the one unified card
 *  (it is `display:none` in every layout), so buildModeButtons() was building buttons into a
 *  container nobody can see, and marking the active one on them. The mode picker that exists is
 *  the card's, built in mobile.ts from deps.modes().
 *  ★ This is what hid the DAB button: it was added HERE, correctly, and drawn where no listener
 *    could ever look (Stuart, 2026-09-04). Deleting the row means the next control cannot be
 *    added to the dead one by mistake. */

function buildControls() {
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
  mobileUi = initMobileControls({
    nudgeSteps: (n) => nudge(n * step),
    zoomBy:     (f) => { spec?.zoomBy(f); updateViewOverlays(); },
    freqHz:     () => spec?.frequency ?? null,
    freqText:   () => cardFreqText(),
    /* ★★★ DAB IS THE MODE WHILE IT IS ON. The card kept showing WFM — the demodulator the
     *  server is no longer running, because DAB replaces the whole chain rather than being an
     *  SDRMode. A readout naming a demodulator that is not running is the same fault as the
     *  decoder box saying RTTY: the panel describing the receiver has to describe THIS one. */
    mode:       () => dabOn ? 'DAB' : (spec?.mode ?? ''),
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
    /* ★★★ BLOCKED MODES WERE NEVER FILTERED HERE. isModeBlocked() was applied only in
     *  buildModeButtons — the DESKTOP bar's row — and that bar is retired and hidden, so an
     *  owner who switched a demodulator off still had it offered in the only picker anybody
     *  uses, and picking it was a no-op. AGENTS.md: never draw a control whose every use is a
     *  no-op. The list is read fresh on every open, so a mid-session change takes effect. */
    modes:      () => MODES.filter(m => !isModeBlocked(m)) as unknown as string[],
    setMode:    (m) => setMode(m as SDRMode, true),
    openMenu:      () => togglePanel('menu'),
    openAudio:     () => togglePanel('audioPanel'),
    openDecoders:  () => togglePanel('decodersPanel'),
    // ★★ The SAME dabCapable and the SAME toggle the desktop bar's button uses — not a second
    //    copy of the rule, which is how the two pickers came to disagree in the first place.
    dabCapable:    () => dabCapable,
    dabOn:         () => dabOn,
    toggleDab:     () => dabSetMode(!dabOn),
    openChat:      () => { togglePanel('chatPanel'); chatOpened(isPanelOpen('chatPanel')); },
  });
  initBw();
  initPanels();
  initRecorder();
  initSearch();
  // ★ DAB controls: the two multiplex buttons and the pinned pane toggle.
  { const dp = document.getElementById('dabPrev'); if (dp) (dp as HTMLElement).onclick = () => dabTune(-1);
    const dn = document.getElementById('dabNext'); if (dn) (dn as HTMLElement).onclick = () => dabTune(+1);
    const pb = document.getElementById('dabPane');
    if (pb) (pb as HTMLElement).onclick = () => dabSetPane(dabPane === 'stations' ? 'signal' : 'stations'); }
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
  /* ★★★ THE TUNER'S OWN FIGURE WHEN WE HAVE IT. spec.rfCenterHz() is DERIVED from the config's
   *     centre — the note above says it "tracked the dial and read 4.625 MHz on a receiver actually
   *     centred on 6.5" — and with VibeClarity moving the tuner underneath a stationary view, that
   *     derivation is wrong exactly when the marker matters most. hwinfo carries the real thing
   *     (rtlCenter plus the hardware offset), so prefer it and keep the derived one as the
   *     fallback for a server too old to send it.
   *  ★ Stuart saw the consequence directly: "in unlocked RF centre mode there are 2 RF centre
   *    VFO's that disagree with each other" — 106.360 from the derivation, 106.306 from the radio.
   *    They disagreed because one of them was guessing. */
  const rf = hwLockedCentre > 0 ? hwLockedCentre
           : (hwRfCentre > 0 ? hwRfCentre : spec.rfCenterHz());
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
let wfScroll: 'sharp' | 'default' | 'smooth' = 'default';
// Rows synthesised per received frame for each preset. DETAILED is 1 = no interpolation at all.
// ★ Each preset is a TARGET ROW RATE (rows/sec on screen), not a multiplier: this renderer works
//   out how many rows to synthesise per frame pair to hit it, which is why a slow feed and a fast
//   one scroll at the same speed. SHARP opts out entirely — one row per received frame.
const WF_PRESET: Record<string, { rate: number; note: string }> = {
  sharp:   { rate: 0,  note: 'One row per received frame — nothing invented, and it scrolls at the data rate.' },
  default: { rate: 20, note: 'A steady 20 rows/sec whatever the feed — a little interpolation on a slow one.' },
  smooth:  { rate: 30, note: 'A steady 30 rows/sec — continuous motion on a slow feed, less real detail.' },
};
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
  if (WF_PRESET[wfScroll]?.rate) wf?.setSpeed(WF_PRESET[wfScroll].rate);
  // The speed control belongs to SMOOTH: under SHARP the data rate decides, so showing it would be
  // a control whose every use is a no-op.
  // ★ The old 10/20/30 SPEED row is gone from the UI: the preset decides it. Kept hidden rather
  //   than deleted so a saved wfSpeed still loads without a migration.
  { const r = document.getElementById('rowWfSpeed');    if (r) r.hidden = true;
    const n = document.getElementById('wfSpeedNote');   if (n) n.hidden = true;
    const t = document.getElementById('wfScrollNote');
    if (t) t.textContent = WF_PRESET[wfScroll]?.note ?? '';
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
/** The last fps we actually asked the server for — see onHwInfo. */
let lastSentFps = -1;
/** Structure of the last hwinfo, so a gain move does not rebuild the controls. See onHwInfo. */
let lastHwSig = '';
/** What we should be asking for right now: our own choice, clamped to the server's ceiling. */
function wantedFps(): number {
  const want = throttled ? Math.min(IDLE_FPS, activeFps) : activeFps;
  return serverMaxFps > 0 ? Math.min(want, serverMaxFps) : want;
}

// ── "I'm over here" ──────────────────────────────────────────────────────────
/**
 * ★★★ ACCEPT AN ADMIN TICKET HANDED TO US IN THE URL.
 *
 *     The ticket exists to carry admin from one context into another — that is its entire job —
 *     but the browser only ever MINTED one after an in-page password unlock, and never READ one.
 *     So the app's ADMIN button opened `/?vs_admin_ticket=…`, this page saw an empty
 *     sessionStorage, and asked for the password again: the button led to a locked door while
 *     holding the key (Stuart, 2026-08-13: "it takes you to the landing screen where it wants you
 *     to enter the admin password again"). The SETUP page had read it from its own URL all along,
 *     which is why setup worked and admin did not.
 *
 * ★★ THE TICKET IS STRIPPED FROM THE URL IMMEDIATELY. It is a bearer credential: left in the
 *    address bar it lands in history, in a bookmark, and in the Referer of every outbound request
 *    the page makes. replaceState removes it without a reload, before anything else runs.
 *
 * ★ `vs_admin_ttl` is honoured when sent, and a short default is used otherwise — a ticket we
 *   over-estimate is one that fails later looking like a wrong password.
 */
function adoptAdminTicketFromUrl() {
  const q = new URLSearchParams(location.search);
  const t = q.get('vs_admin_ticket');
  if (!t) return;
  saveAdminTicket(t, Number(q.get('vs_admin_ttl')) || 600);
  q.delete('vs_admin_ticket');
  q.delete('vs_admin_ttl');
  const rest = q.toString();
  history.replaceState(null, '', location.pathname + (rest ? '?' + rest : '') + location.hash);
}
adoptAdminTicketFromUrl();

/** ★★ A NOTICE MUST GREET SOMEONE WHO ARRIVES AFTER IT WAS POSTED. The push only reaches sockets
 *  that were already open, and the person opening the page now is precisely the one about to judge
 *  a misbehaving receiver. Read from /vibeserver.json, which every client already fetches. */
async function loadOwnerNotice() {
  try {
    // ★★★ P(), NOT A ROOT PATH. Behind a front door every radio lives under /r/<id>/, and a bare
    //     "/vibeserver.json" reads the DOOR's file — a process that owns no radio, so it has no
    //     limit, no aerial and no countdown. The owner's notice is machine-level and came back
    //     correct either way, which is exactly why this went unseen: the one field that worked
    //     hid the ones that could not (Stuart, 2026-08-19: "I never saw the new time message").
    const r = await fetch(P('/vibeserver.json'), { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    if (typeof j?.notice === 'string' && j.notice) showOwnerNotice(j.notice);
    // ★★★ THE LANDING FIELDS COME FROM HERE TOO, AND THIS IS THE ONLY SOURCE A SIMPLE SERVER HAS.
    //     A one-radio machine never draws a picker, so it never fetches /vibeserver/radios — a
    //     field that only travelled with the directory would be invisible on most servers in the
    //     field. This file is served by the front door AND by every radio, so it always answers.
    // ★ showSplashRadios() renders the same message again from the directory when there is one;
    //   it is the same value, and whichever arrives first is correct.
    // ★★★ TELL THE LISTENER THE LIMIT IS A GUARANTEE, BEFORE IT MATTERS. Without this a soft
    //     server's countdown reaches zero, nothing happens, and the listener is left working out
    //     for themselves whether the receiver is broken or the rule is. Said ONCE, as a pill —
    //     they have not been refused anything, and a modal between a person and the radio to
    //     deliver good news would be absurd.
    // ★ Absent limitMode means hard, so this never fires on a server that has not chosen soft.
    softLimit = j?.limitMode === 'soft';
    if (softLimit && Number(j?.limitMin) > 0 && !softLimitTold) {
      softLimitTold = true;
      const mins = Number(j.limitMin);
      showPill(`This receiver is shared. It is yours for ${mins} minutes — after that you keep it `
             + `until somebody else wants it.`, 11000);
    }
    showLandingMessage(j?.landingMessage, j?.landingLinkUrl, j?.landingLinkLabel);
    const ant = typeof j?.antenna === 'string' ? j.antenna.trim() : '';
    const ael = document.getElementById('splashAntenna');
    if (ael) {
      // ★ innerHTML now that there is an SVG in it — so the description must be ESCAPED, which
      //   textContent used to do for free. Owner-authored text going into markup.
      ael.innerHTML = ant ? `${antIcon(j?.antennaIcon)}${escapeHtml(ant)}` : '';
      ael.style.display = ant ? '' : 'none';
    }
  } catch { /* an older server has no notice field — nothing to show */ }
}
void loadOwnerNotice();

/**
 * ★★★ OPEN THE ADMIN PAGE ON ARRIVAL, NOT AFTER CONNECTING. The `#admin` handler sat at the end of
 *     the post-CONNECT init — so tapping ADMIN in the app loaded the page, showed the splash with
 *     its radio cards, and waited for a connection that the owner had not asked for. What they saw
 *     was "the landing page", every time, with the admin panel never opening (Stuart, 2026-08-13,
 *     on build 99 — after the URL itself had already been corrected to point at the radio).
 * ★★ THE ADMIN PANEL NEEDS NO RADIO. It is HTTP against /vibeserver/admin/*, which is exactly why
 *     the front door serves it at all: it is how an owner gets in when every radio has died.
 *     Waiting for a spectrum socket was a dependency it never had.
 * ★★ `host` is derived from the LOCATION rather than from the connection, prefix included, so the
 *    panel asks the radio whose page this is — the same value the connected path would have set.
 * ★ Gated on holding a ticket, so the fragment alone can never prise the panel open.
 */
function openAdminIfAsked() {
  if (location.hash !== '#admin' || !inAdminMode()) return;
  const h = location.host + BASE_PATH;
  initAdmin(() => h, () => '');
  openAdmin(h, '');
}
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', openAdminIfAsked, { once: true });
} else {
  openAdminIfAsked();
}

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
/**
 * ★★★ THE OWNER'S NOTICE — "antenna maintenance in progress".
 *
 *     The situation it exists for: something is genuinely wrong with the aerial and the owner has
 *     two bad options — power the server down and leave a dead link on the website, or leave it up
 *     while listeners watch the spectrum jump about and conclude the receiver is rubbish (Stuart,
 *     2026-08-13). Neither is a fault in the RADIO; only the explanation is missing.
 *
 * ★★ DISMISSABLE, and stays dismissed for THIS message. Somebody who has read it should be able to
 *    watch their waterfall — but a NEW notice reappears, because the next one may say something
 *    else entirely.
 * ★ Amber, not red: this is the owner talking, not an error. A red banner over a working receiver
 *   teaches people to distrust red.
 */
let noticeDismissed = '';
function showOwnerNotice(text: string) {
  const id = 'ownerNotice';
  document.getElementById(id)?.remove();
  const t = (text || '').trim();
  if (!t || t === noticeDismissed) return;
  const el = document.createElement('div');
  el.id = id;
  // ★★★ IT SAT ON TOP OF THE WHOLE APP. z-index 9400 against a frequency panel at 51 — nearly two
  //     hundred layers above every control — so on a narrow screen a six-line notice covered the
  //     panel a listener had just opened (Stuart, 2026-08-19, iPhone SE: "it blocks most things in
  //     the UI").
  // ★★★ ONE VALUE CANNOT BE RIGHT FOR BOTH SCREENS. It must clear the SPLASH, which is 100, and
  //     sit under the panels, which are around 51 — so the layer depends on which screen you are
  //     on, and startApp() moves it when the screens swap. The two are never visible at once,
  //     which is what makes that safe.
  // ★★★ 48, NOT 45 — AND THE GAP IS ONLY THREE. The receiver's own overlays sit at 44, 45 and 46
  //     (the frequency scale among them) and the panels at 51, so there is exactly one usable slot
  //     between them. At 45 the scale drew straight over the notice and the box vanished, leaving
  //     text floating on the spectrum. Anything added to this file in the 47-50 range will collide
  //     with it.
  // ★★ AND IT NEEDS A VISIBLE WAY OUT. Clicking it has always dismissed it, but nothing said so —
  //    an affordance only the author knows about is not an affordance. The × is a real button with
  //    a real touch target, in the flex row rather than floated over the text, because a × that
  //    lands ON the words is unreadable at exactly the width where it matters most.
  // ★ Font and padding shrink with the viewport rather than at a breakpoint: the SE in Display
  //   Zoom is the narrowest layout this has to survive, and it is not a size anybody tests on.
  el.style.cssText =
    `position:fixed;left:50%;top:12px;transform:translateX(-50%);max-width:min(92vw,640px);` +
    `z-index:${document.getElementById('app')?.classList.contains('live') ? 48 : 101};` +
    // ★★★ FULLY OPAQUE. At 96% the waterfall and the frequency scale showed straight through and
    //     the text became hard to read against a moving picture — the one thing a notice must not
    //     be (Stuart, 2026-08-19). Nothing behind this needs to be visible THROUGH it; it is a
    //     message, and it can be dismissed.
    'background:#17100a;color:#ffd479;border:1px solid rgba(255,180,60,0.55);' +
    'border-radius:8px;padding:7px 8px 7px 12px;font:clamp(11px,3vw,13px) ui-monospace,monospace;' +
    'box-shadow:0 4px 18px rgba(0,0,0,0.55);display:flex;align-items:flex-start;gap:8px;' +
    'line-height:1.4';
  const msg = document.createElement('span');
  msg.textContent = t;
  msg.style.cssText = 'flex:1 1 auto;min-width:0';
  const x = document.createElement('button');
  x.type = 'button';
  x.textContent = '\u00D7';
  x.title = 'Dismiss this message';
  x.setAttribute('aria-label', 'Dismiss this message');
  // ★★ 28px, not a glyph with padding: this is the smallest thing on screen and the one people
  //    jab at with a thumb. ★ flex:none so it can never be squeezed to nothing by a long message —
  //    a clipped × is a TRAP, not untidiness (see the SE display-zoom note in AGENTS.md).
  x.style.cssText =
    'flex:none;width:28px;height:28px;margin:-3px -2px 0 0;padding:0;background:none;border:0;' +
    'color:#ffd479;font:18px/1 ui-monospace,monospace;cursor:pointer;opacity:.75;border-radius:6px';
  x.addEventListener('click', (e) => { e.stopPropagation(); noticeDismissed = t; el.remove(); });
  el.appendChild(msg);
  el.appendChild(x);
  // ★ The whole box still dismisses, as it always did — the × makes that discoverable rather than
  //   replacing it.
  el.style.cursor = 'pointer';
  el.addEventListener('click', () => { noticeDismissed = t; el.remove(); });
  document.body.appendChild(el);
}

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

/** The server is full. A full overlay, not a toast: nothing else on the page will work, and a
 *  dismissable banner would just invite the user to sit staring at a dead waterfall.
 *  ★★★ A REFUSAL WITH NO INFORMATION IS THE WORST SCREEN WE CAN SHOW. "Try again later" gives a
 *      person nothing to decide with, so they sit hammering reload — which our own cooldown then
 *      punishes. "Free in 4:12, you are 2nd in the queue" turns a dead end into a wait, and a wait
 *      is something someone will actually sit through (Stuart, 2026-08-04).
 *  ★ Called REPEATEDLY as our position changes — the server holds the socket open and re-sends.
 *    So this updates in place and must not stack overlays. */
function showBusy(q?: { queuePos?: number; queueLen?: number; freeIn?: number; queueFull?: boolean }) {
  const mmss = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  let body: string;
  if (q?.queueFull) {
    body = 'This server is full, and so is its waiting queue.<br><br>Please try again later.';
  } else {
    // ★★ ONLY PROMISE A TIME WE CAN HONOUR. freeIn < 0 means no session limit is set, so there is
    //    no honest number — the occupant may stay all day. A countdown we cannot keep is worse
    //    than none, because the user WILL sit and watch it.
    const when = (q?.freeIn !== undefined && q.freeIn >= 0)
      ? `A slot should free up in <b>${mmss(q.freeIn)}</b>.`
      : 'This server has no fixed session limit, so there is no reliable estimate.';
    const place = q?.queuePos
      ? (q.queuePos === 1
          ? 'You are <b>next in line</b> — keep this page open and you will be let in automatically.'
          : `You are <b>${q.queuePos} of ${q.queueLen ?? q.queuePos}</b> waiting. Keep this page open to hold your place.`)
      : '';
    body = `This server is serving as many listeners as it allows.<br><br>${when}<br><br>${place}`;
  }
  showRefusal('IN USE', body,
    // ★ The override box is offered ONLY on the IN USE screen, and only when the server says
    // it HAS an admin password. Offering it everywhere would invite listeners to try guessing
    // it, and offering it on a server with no password set is a puzzle with no answer.
    srvAdminProtected);
}

/** ★★ TAKE THE RADIO BACK. The owner's escape hatch: their own receiver is busy and they need
 *  it. Sends a nonce + HMAC on the connect URL, never the password — see resolveAdminOverride.
 *
 *  ★★★ AND A TICKET, BECAUSE THE NONCE ALONE CANNOT WORK ON A MULTI-RADIO SERVER. The challenge
 *      is answered by whichever process serves the page — on a V3 server that is the FRONT DOOR,
 *      which owns no radio, and the nonce store is per-PROCESS. So the radio we are trying to
 *      take over has never heard of the nonce we just proved, and refuses it: the password looks
 *      wrong however many times it is typed. `adminSignIn` learned this on the splash screen and
 *      this function was left behind, still doing what was right when every server was one
 *      process. Mint the ticket the same way it does — every radio on the machine accepts it,
 *      and `connect()` already folds it onto BOTH sockets.
 *
 *  ★ The nonce is still stashed as well, and deliberately: on a SINGLE-process server (a phone,
 *    a Mac, an older build) there is no /vibeserver/admin-ticket to mint from, and there the
 *    nonce is exactly right. Belt and braces, because the two cases are indistinguishable from
 *    here without another round trip.
 *
 *  ★ A reload is the honest retry: it re-runs the preflight and re-opens both sockets cleanly,
 *  carrying the override credentials this time. */
async function doAdminOverride(password: string, status: HTMLElement) {
  status.textContent = 'checking…';
  const q = await resolveAdminOverride(httpBase(location.host), password);
  if (!q) { status.textContent = 'this server cannot be overridden'; return; }
  // Stash for the reload to pick up. sessionStorage, not localStorage: an override is for THIS
  // visit, and a credential that outlives the tab is a credential left lying around.
  sessionStorage.setItem('vsAdminOverride', q);
  // ★ This IS the deliberate act — see the takeover flag in connect(). Set before the reload,
  //   because the reload is how the takeover actually happens.
  sessionStorage.setItem('vsTakeover', '1');
  // The ticket, for the multi-radio case. A server too old to mint one just 404s, and the nonce
  // above carries it — so this failing is not an error worth showing.
  try {
    const r = await fetch(`${httpBase(location.host)}/vibeserver/admin-ticket?${q}`, { cache: 'no-store' });
    if (r.ok) {
      const j = await r.json();
      saveAdminTicket(String(j.ticket || ''), Number(j.ttl) || 600);
      // ★ Keep the password for the admin PAGE, which signs its own requests — otherwise an owner
      //   who took over from this screen arrives unlocked and cannot open the admin page.
      adminPassword = password;
      setBookmarkAdminAuth(async () => resolveAdminOverride(httpBase(location.host), adminPassword));
    } else if (r.status === 401) {
      // ★ The one failure worth reporting. 401 means the password was wrong (or this address is
      //   in the brute-force lockout) — reloading would just show IN USE again with no clue why,
      //   which is the loop that made this look like "the browser cannot take it back".
      status.textContent = 'wrong password — or too many attempts; wait a moment';
      sessionStorage.removeItem('vsAdminOverride');
      sessionStorage.removeItem('vsTakeover');
      return;
    }
  } catch { /* no ticket endpoint, or offline — the nonce still rides the reload */ }
  location.reload();
}

/** ★★ Banned. Say it plainly and DO NOT offer a countdown or a retry — there is nothing to wait
 *  for, and a "try again later" here would be a lie that costs both ends a reconnect loop.
 *  ★ No reason is shown even though the server records one: the reason is the owner's note to
 *    themselves, written in the ban box, and it is quite often blunt. It belongs on the admin
 *    page, not quoted back at the person. */
function showBanned() { showRefusal('NO ACCESS',
  'This receiver\'s operator has blocked access from your address.<br><br>' +
  'If you believe that is a mistake, contact whoever runs this receiver.'); }

/** ★★★ THE IDLE RE-LOCK, as a PILL — not an overlay, and the difference is the whole point.
 *  Nothing has stopped: the audio is still playing, the decoder is still decoding, the session
 *  is still ours. An overlay would say "something has gone wrong and you must deal with it",
 *  which is exactly the wrong message for a quiet, expected safety step (Stuart, 2026-08-06:
 *  "simply lock the admin controls with a little pill pop up warning"). */
function showAdminRelocked(idleMin: number) {
  const mins = idleMin > 0 ? `${idleMin} minutes` : 'a while';
  showPill(`Admin controls locked after ${mins} idle — unlock again in the menu`);
}

/** ★ A small transient notice at the top of the screen. Replaces any pill already showing
 *  rather than stacking, and clears itself; there is no dismiss button because there is
 *  nothing here the user must acknowledge. */
let pillTimer = 0;
function showPill(text: string, ms = 9000) {
  let el = document.getElementById('vsPill');
  if (!el) {
    el = document.createElement('div');
    el.id = 'vsPill';
    document.body.appendChild(el);
  }
  el.textContent = text;
  // ★★★ SIT BELOW THE OWNER'S NOTICE, NOT UNDER IT. Both are fixed, centred and pinned to the top
  //     of the screen, and the notice's z-index is two orders above the pill's — so on a server
  //     carrying a maintenance notice the pill was drawn and then covered by it (Stuart,
  //     2026-08-19: "the pill is showing but it is hidden behind the maintenance message").
  // ★★ MEASURED, not a guessed constant: the notice wraps to two or three lines on a phone, and a
  //    fixed offset that clears it on this screen would overlap on a narrower one.
  // ★ Read at SHOW time, because the notice can be dismissed between one pill and the next.
  const notice = document.getElementById('ownerNotice');
  const below = notice ? Math.round(notice.getBoundingClientRect().bottom) + 10 : 14;
  el.style.top = `${below}px`;
  el.classList.add('show');
  clearTimeout(pillTimer);
  pillTimer = window.setTimeout(() => el?.classList.remove('show'), ms);
}

/** ★ The owner has taken their radio back. Not a fault, and worth saying so — being dropped
 *  with no explanation is what makes people assume the software broke. */
function showEvicted() { showRefusal('TAKEN OVER',
  'The owner of this receiver has taken control using the admin password.<br><br>' +
  'You can try again shortly.'); }

/** ★★ The session limit. Say WHY it ended and WHEN they may return — a bare disconnect on a
 *  public receiver reads as a crash, and the listener blames us rather than understanding they
 *  had a share of a shared radio. */
function showSessionEnded(cooldownSec: number, freshSec = 0) {
  const m = Math.max(1, Math.round(cooldownSec / 60));
  const f = Math.round(freshSec / 60);
  /* ★★★ TWO WINDOWS, AND WE QUOTED THE SHORT ONE. `cooldown` is how long this address is refused;
   *     a FULL turn only returns after the limit itself has passed. Come back between them and you
   *     are let in with no time left — which reads as a fault rather than as the rule it is.
   *  ★ Only said when the server stated it and it is genuinely longer. */
  showRefusal('TIME UP',
    'Your session on this shared receiver has ended, so someone else can have a turn.' +
    (f > m
      ? `<br><br>You can reconnect in about ${m} minute${m === 1 ? '' : 's'} — but a full turn `
        + `starts again ${f} minutes after your last one.`
      : `<br><br>You can reconnect in about ${m} minute${m === 1 ? '' : 's'}.`));
}

/** Refused because we came back before our cooldown finished. */
function showCooldown(secs: number) {
  const m = Math.max(1, Math.round(secs / 60));
  showRefusal('PLEASE WAIT',
    'You have just had a turn on this shared receiver.' +
    `<br><br>Try again in about ${m} minute${m === 1 ? '' : 's'}.`);
}

/** ★★★ "STILL LISTENING?" — the one prompt on this page that must be ANSWERED rather than read.
 *
 *  ★★ It clears itself when the server sees any control message, so the honest instruction is
 *     "touch anything". The button just sends a ping so somebody who is listening and does not
 *     want to change a thing can say so without retuning their radio.
 *  ★ Counts down visibly. A prompt with a deadline you cannot see is how a listener discovers the
 *    rule by losing their slot to it.
 */
let idleTimer: ReturnType<typeof setInterval> | null = null;
function showIdleCheck(secs: number) {
  const id = 'idleCheck';
  document.getElementById(id)?.remove();
  if (idleTimer) { clearInterval(idleTimer); idleTimer = null; }
  let left = Math.max(5, secs || 60);
  const el = document.createElement('div');
  el.id = id;
  el.style.cssText =
    'position:fixed;inset:0;z-index:9700;background:rgba(8,6,1,0.9);display:flex;'
  + 'align-items:center;justify-content:center;text-align:center;font:15px ui-monospace,monospace;color:#ffb833';
  const paint = () =>
    `<div style="max-width:340px;padding:24px">`
  + `<div style="font-size:20px;letter-spacing:4px;margin-bottom:12px">STILL LISTENING?</div>`
  + `<div style="opacity:.8;line-height:1.5">This receiver is shared, and nobody has used it for a `
  + `while. Tap anything to carry on &mdash; otherwise the radio goes back to the queue in `
  + `<b>${left}s</b>.</div>`
  + `<button id="idleYes" style="margin-top:18px;background:none;border:1px solid rgba(255,180,50,0.5);`
  + `color:#ffb833;border-radius:6px;padding:8px 18px;font:13px ui-monospace,monospace;cursor:pointer">`
  + `I'm still here</button></div>`;
  el.innerHTML = paint();
  const answer = () => {
    // ★ Any control message clears it server-side; ping is the smallest one that changes nothing.
    try { spec?.send({ type: 'ping' }); } catch {}
    if (idleTimer) { clearInterval(idleTimer); idleTimer = null; }
    el.remove();
  };
  el.addEventListener('click', answer);
  document.body.appendChild(el);
  document.getElementById('idleYes')?.addEventListener('click', answer);
  idleTimer = setInterval(() => {
    left--;
    if (left <= 0) { clearInterval(idleTimer!); idleTimer = null; return; }  // the server closes it
    const b = el.querySelector('b');
    if (b) b.textContent = `${left}s`;
  }, 1000);
}

function showRefusal(title: string, bodyHtml: string, offerOverride = false) {
  const id = 'busyOverlay';
  // ★★ UPDATE IN PLACE, do not bail. This used to return early if the overlay existed, which was
  //    right when every refusal was a one-shot. The queue re-sends our position and the countdown
  //    once a second, so bailing would freeze the display on whatever it said first — a countdown
  //    stuck at 4:12 is worse than no countdown, because the user trusts it and waits.
  const existing = document.getElementById(id);
  if (existing) {
    const t = existing.querySelector('[data-refusal-title]');
    const b = existing.querySelector('[data-refusal-body]');
    if (t) t.textContent = title;
    if (b) b.innerHTML = bodyHtml;
    return;
  }
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
    '<div style="max-width:340px;padding:24px"><div data-refusal-title style="font-size:20px;letter-spacing:4px;' +
    `margin-bottom:12px">${title}</div>` +
    `<div data-refusal-body style="opacity:0.8;line-height:1.5">${bodyHtml}</div>` +
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

/* ★★★ THE RESULT LIST SURVIVES THE STATION YOU JUST TRIED — AND ONLY FOR A WHILE.
 *
 *  A search for one broadcaster returns several frequencies (Voice of Korea is the example), most
 *  of them inaudible from any given place at any given hour, and you find the one that works by
 *  trying them. So tuning a result must not throw the list away.
 *
 *  ★ The two clients differed, in opposite directions: the APP cleared the query the instant you
 *    tuned a result, and THIS one kept the results in the closure for ever, so clicking back into
 *    the box an hour later reopened a list from a session you had forgotten. Same rule now in
 *    both — keep it, then let it go — because the same person uses both (Stuart: "this is both
 *    the app and the web client so the behaviour should be the same in both").
 *  ★ The window restarts each time you come back, so a long hunt never expires mid-hunt; only
 *    walking away ends it. Kept identical to FreqModal.SEARCH_STICKY_MS.
 */
const SEARCH_STICKY_MS = 90_000;

function initSearch() {
  const el = $<HTMLInputElement>('search');
  const list = $('searchResults');
  let results: SearchResult[] = [];
  let sel = -1;
  /** When the list was last USED. 0 = never, so a list nobody has tuned from is not on the clock. */
  let usedAt = 0;

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
      row.onclick = () => { usedAt = Date.now(); tuneTo(r); close(); };
      list.appendChild(row);
    });
    list.classList.add('open');
  };

  el.oninput = () => {
    /* ★★ 200, NOT 40 — the same ceiling the app uses (stations.searchStations). The browser was
       quietly returning a THIRD of the rows for the same query against the same receiver, so a
       broad search ("china" comes back with 62 in the app) looked truncated because it WAS.
       One number with two readers, and only one of them had been raised. */
    results = search(el.value, 200);
    sel = -1;
    usedAt = 0;                                // a new query is a new hunt, not a stale one
    render();
  };
  el.onfocus = () => {
    if (!results.length) return;
    // ★ Never tuned from this list yet? It is still the search you are typing — always reopen.
    if (usedAt && Date.now() - usedAt > SEARCH_STICKY_MS) { results = []; usedAt = 0; return; }
    if (usedAt) usedAt = Date.now();          // the window restarts on every return
    list.classList.add('open');
  };
  /* ★★★ TOUCHING THE LIST BLURS THE INPUT — so a scroll used to CLOSE it.
   *  On a touch screen, putting a finger on the results takes focus off the search box, and the
   *  blur handler then closed the list 150 ms later, mid-drag. It read as the list fighting you
   *  and snapping back (Stuart, 2026-09-03). The 150 ms delay was only ever there so a mouse
   *  click could land before the close; a touch drag needs the same protection and never had it.
   *  ★ So a pointer that goes DOWN on the list cancels the pending close outright. Tapping a row
   *    still closes it — that path calls close() itself, deliberately, after tuning. */
  let holdingList = false;
  list.addEventListener('pointerdown', () => { holdingList = true; });
  el.onblur = () => setTimeout(() => {
    if (holdingList) { holdingList = false; return; }
    close();
  }, 150);

  el.onkeydown = (e) => {
    if (e.key === 'Escape') { el.blur(); close(); return; }
    if (!results.length) return;
    if (e.key === 'ArrowDown') { sel = Math.min(results.length - 1, sel + 1); render(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { sel = Math.max(0, sel - 1); render(); e.preventDefault(); }
    else if (e.key === 'Enter') {
      usedAt = Date.now();
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
  // ★ The shim gates this on the ADMIN credential (vsAdminHttpOk). It used to say the PIN, "which
  //   is what becomes the admin credential when public servers arrive" — they arrived, it never
  //   moved, and with no PIN set (the public-receiver configuration) the write was open to every
  //   listener. Fixed 2026-08-07; this comment is the one that described the old behaviour.
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
          // ★ The PIN is not what gates this — writes are ADMIN-gated (vsAdminHttpOk). Blaming
          //   the PIN sent Stuart looking at the wrong credential while signed in as admin.
          : 'Nothing imported — the receiver refused it (are you signed in as admin?)';
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

  // ★★★ THREE DIFFERENT CLAIMS, THREE HEADINGS. These were one flat run distinguished only by a
  //     glyph and a tooltip, and the difference between them is not cosmetic: a bookmark somebody
  //     CHOSE, a station this aerial has actually HEARD, and an entry the receiver's owner saved
  //     for everyone are three different kinds of fact. Stuart, seeing his own CB channels
  //     interleaved with RDS-learned BBC services (2026-08-21): "maybe the bookmarks and learnt
  //     stations need a split to differentiate them."
  // ★★ AND IT IS ABOUT TO MATTER FAR MORE. DAB service discovery will add a whole multiplex at a
  //    time — a hundred entries from one scan — and dropping those into an undifferentiated list
  //    would bury the handful the user actually chose.
  // ★ Headings only appear when they have rows, so an ordinary receiver with nothing learned looks
  //   exactly as it always did.
  const sections: Array<{ title: string; hint: string; of: (r: Row) => boolean }> = [
    { title: 'YOUR BOOKMARKS', hint: 'Saved in this browser', of: r => r.local },
    { title: 'SAVED ON THE RECEIVER', hint: 'Saved by the owner, shared with everyone',
      of: r => !r.local && !r.heard },
    { title: 'HEARD BY THIS RECEIVER', hint: 'Found automatically over RDS — expires if it stops being heard',
      of: r => !r.local && !!r.heard },
  ];

  for (const sec of sections) {
    const mine = rows.filter(sec.of).sort((a, z) => a.frequency - z.frequency);
    if (!mine.length) continue;
    const head = document.createElement('div');
    head.className = 'bmHead';
    head.innerHTML = `<span class="bmHeadT">${sec.title}</span>` +
                     `<span class="bmHeadN">${mine.length}</span>` +
                     `<span class="bmHeadH">${escapeHtml(sec.hint)}</span>`;
    host.appendChild(head);
    renderBookmarkRows(host, mine);
  }
}

/** One section's worth of rows. Split out only so the sectioning above reads as sectioning. */
function renderBookmarkRows(host: HTMLElement, rows: Array<{
  name: string; frequency: number; mode?: string;
  local: boolean; heard?: boolean; bwLo?: number | null; bwHi?: number | null;
}>) {
  for (const b of rows) {
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
    void attachBookmarkLogo(row, b.name, undefined, b.frequency);
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

async function attachBookmarkLogo(row: HTMLElement, name: string, itu?: string, hz?: number) {
  // ★★★ IDENTITY FIRST, EXACTLY AS THE LIVE PANEL DOES. If this receiver has ever heard the
  //     station on this frequency and RadioDNS answered, that is the broadcaster's OWN file and it
  //     outranks any name match — the same ranking the live path enforces, applied to the list.
  if (hz && hz > 0) {
    const known = loadFreqLogos()[freqLogoKey(hz)];
    if (known && row.isConnected) {
      const img0 = document.createElement('img');
      img0.className = 'bmLogo';
      img0.src = known;
      img0.alt = '';
      row.insertBefore(img0, row.firstChild);
      return;
    }
  }
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
                'bookmarksPanel', 'freqPanel', 'chatPanel'];

function closePanels() {
  // ★ The chat's unread counter keys off whether its panel is open, and EVERY close route lands
  //   here — Escape, a click away, opening something else. Telling it from the button handlers
  //   alone would leave the count frozen at zero after any of the other three.
  if (isPanelOpen('chatPanel')) chatOpened(false);
  for (const id of PANELS) $(id).classList.remove('open');
}

function togglePanel(id: string) {
  const open = $(id).classList.contains('open');
  closePanels();
  if (!open) $(id).classList.add('open');
}

/** ★ Asked AFTER togglePanel, because "open" is the DOM's answer and not the caller's guess —
 *  closePanels() may have shut something else and a caller tracking it itself would drift. */
function isPanelOpen(id: string): boolean {
  return !!document.getElementById(id)?.classList.contains('open');
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
let listenerCount = 0, listenerMax = 0;
let sessionDeadline = 0;      // epoch ms, 0 = no limit
let sessionTicker: ReturnType<typeof setInterval> | null = null;

/** ★★ Does THIS server keep you past the limit? Read from /vibeserver.json at connect; absent
 *  means hard, which is what every server said before this existed. */
let softLimit = false;
/** ★ Once per page load. Repeating it on every poll would turn an explanation into nagging. */
let softLimitTold = false;

function setTimeLeft(secs: number) {
  sessionDeadline = Date.now() + secs * 1000;
  if (!sessionTicker) sessionTicker = setInterval(paintTimeLeft, 1000);
  paintTimeLeft();
}

/** ★★★ THE OWNER IS NOT ON THE CLOCK. The server already exempts an admin session from the
 *  session limit — enforceSessionLimit() returns early on adminOk, and occupantSecsLeft()
 *  answers -1 — so the countdown on screen was measuring a deadline that no longer existed. It
 *  ran down to "Your turn ends in 0:00" and then nothing happened, which is worse than either
 *  outcome on its own: the owner watches a threat that will not be carried out, and learns not to
 *  trust the readout (Stuart, 2026-08-07).
 *  ★ Called on EVERY grant of admin, whichever way it arrives — the menu password box, or
 *    connecting as admin from the splash — because both end in the same server-side state. */
function clearTimeLeft() {
  sessionDeadline = 0;
  if (sessionTicker) { clearInterval(sessionTicker); sessionTicker = null; }
  const el = document.getElementById('rxTimeLeft');
  if (!el) return;
  // ★★ SAY WHY THE CLOCK WENT, rather than emptying the slot. Hiding it is indistinguishable from
  //    "no limit on this server", so an admin could not tell an exemption from a receiver that
  //    never limited anyone — and the exemption is the thing worth knowing, because it is what a
  //    compromised password would also grant (Stuart, 2026-08-12).
  if (adminUnlocked) {
    el.hidden = false;
    el.textContent = '\u26bf Admin Mode';
    el.className = 'adminMode';
    el.title = 'Admin session — not subject to the listening time limit';
    return;
  }
  el.hidden = true; el.textContent = ''; el.className = ''; el.title = '';
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
    // ★★★ ON A SOFT SERVER THE CLOCK RUNNING OUT IS NOT THE END, AND MUST NOT LOOK LIKE IT. The
    //     listener stays connected past zero — so "Your turn ends in 0:00" sits there being wrong,
    //     and the reader is left wondering why they are still listening. That is the same fault
    //     clearTimeLeft() records for the admin exemption twenty lines up: a countdown that runs
    //     down to a threat nobody carries out teaches people not to trust the readout.
    // ★ Stuart, 2026-08-19: "users need to be advised that there is a soft limit so that when the
    //   time limit runs out they dont wonder why they are staying connected."
    if (softLimit) {
      // ★ SHORT. This is a one-line status slot in the corner under the receiver's name, not a
      //   place for a sentence — the explanation was already given as a pill on arrival, and all
      //   this has to do is say which state you are in now.
      el.textContent = 'Yours until someone else wants it';
      el.className = '';
      el.hidden = false;
      if (sessionTicker) { clearInterval(sessionTicker); sessionTicker = null; }
      return;
    }
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
  // ★★★ AND SAY THE RIGHT THING ABOUT IT. On a SOFT server this number is not a countdown to
  //     being disconnected — it is how long the radio is GUARANTEED to be yours, after which you
  //     keep it until somebody else turns up. "Your turn ends in 29:39" is simply false for the
  //     whole half hour, not merely at zero (Stuart, 2026-08-19: "their turn wont end but their
  //     guaranteed time will").
  // ★★ The COLOURS go with the meaning. Red and amber say "something is about to be taken from
  //    you", and on a soft server nothing is: the number running out changes what MAY happen, not
  //    what WILL. Dressing a guarantee as an alarm is how a listener learns to distrust it.
  el.textContent = softLimit
    ? `Yours for ${mm}:${String(ss).padStart(2, '0')}`
    : `Your turn ends in ${mm}:${String(ss).padStart(2, '0')}`;
  el.className = softLimit ? '' : (left <= 30 ? 'crit' : left <= 120 ? 'warn' : '');
  el.hidden = false;
  $('rxBadge').hidden = false;   // the badge may be empty if the owner set no name
}

async function loadServerLocation(host: string) {
  try {
    const r = await fetch(`${httpBase(host)}/location`, { cache: 'no-store' });
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
let activeDec: 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'rds' | 'time' | null = null;
/** True while the last thing written to the decoder panel was a replace-in-place progress line,
 *  which carries no trailing newline. See onText. */
let decProgressLine = false;
/** Which time station the TIME decoder is set to. A preset, exactly like RTTY's. */
let timeStation = 'msf';
/** ★ Each station implies its own FREQUENCY and its own MODE, and the mode matters as much as the
 *  frequency: these carry their code as carrier amplitude, so CW turns it into a beat note the
 *  decoder can follow. In AM a DC-blocking demodulator flattens a steady carrier to silence. */
const TIME_STATIONS: Record<string, { label: string; hint: string }> = {
  msf:   { label: 'MSF',   hint: 'MSF, Anthorn — tune <b>60.000 kHz</b> in <b>CW</b>.' },
  dcf77: { label: 'DCF77', hint: 'DCF77, Mainflingen — tune <b>77.500 kHz</b> in <b>CW</b>.' },
  wwvb:  { label: 'WWVB', hint: 'WWVB, Fort Collins — tune <b>60.000 kHz</b> in <b>CW</b>. '
                                 + 'Note MSF shares this frequency: which one you hear is geography.' },
  // ★★ AM, not CW, and the only station here that is: WWV's code rides a 100 Hz subcarrier on the
  //    AM signal rather than switching the carrier. Tuned in CW there is nothing to decode.
  // ★ The year is not shown from the air — see decodeWwv: its year field could not be identified
  //   against real signals, so the date is built from day-of-year and this machine's year.
  wwv:   { label: 'WWV',   hint: 'WWV, Fort Collins — <b>2.5 / 5 / 10 / 15 / 20 MHz</b> in <b>AM</b> '
                                 + '(not CW). Strongest in North America; from Europe try 15 MHz in '
                                 + 'the afternoon.' },
  rwm:   { label: 'RWM',   hint: 'RWM, Moscow — <b>4.996 / 9.996 / 14.996 MHz</b> in <b>CW</b>. '
                                 + 'Markers only: RWM transmits <b>no date or time code</b>, so none can be shown.' },
};

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
  if (mode === 'time') return { station: timeStation };
  return {};
}

function initDecoders(host: string, auth: AuthState) {
  decoders = new DecoderClient(host, auth, {
    onText: (t: string) => {
      const el = $('decText');
      // ★★ A LEADING \r MEANS "REPLACE THE LAST LINE", as a terminal would. The time decoders send
      //    a progress line once a SECOND — the fields filling in as they arrive — and appending
      //    those would scroll 59 near-identical lines past the reader every minute, burying the
      //    decoded times among them. Only the time decoders use it; everything else appends
      //    exactly as before.
      let text = el.textContent || '';
      // ★★★ ONLY THE TIME DECODERS SPEAK THE TERMINAL CONVENTION — RTTY's CARRIAGE RETURNS ARE
      //     DATA. CR is a character in the Baudot alphabet (see the tables in fsk_decoder.cpp) and
      //     stations really send it, usually as CR CR LF and sometimes bare. Treating those as
      //     "rewrite the last line" ate the history as it decoded: the box grew while everything
      //     above vanished (Stuart, 2026-08-12).
      //     ★★★ AND IT WAS WORSE THAN LOSING ONE LINE. The rewrite cuts back to the last '\n' —
      //         but when the buffer holds none yet, `lastIndexOf` returns -1 and the slice keeps
      //         NOTHING, so a single CR threw away everything decoded so far. RTTY is exactly the
      //         case with no newlines of its own, which is why it hit hardest there.
      // ★ For every other decoder a CR is just an end-of-line: fold a RUN of them, with or
      //   without a following LF, into ONE newline. RTTY convention is CR CR LF — two returns to
      //   give the carriage time to travel on a real teleprinter — so mapping each separately
      //   would print a blank line between every row of a transmission.
      const terminalCR = activeDec === 'time';
      if (!terminalCR) t = t.replace(/\r+\n?/g, '\n');
      for (const chunk of t.split(/(?=\r)/)) {
        if (chunk.startsWith('\r')) {
          const cut = text.lastIndexOf('\n');
          text = (cut >= 0 ? text.slice(0, cut + 1) : '') + chunk.slice(1);
          decProgressLine = true;
        } else {
          // ★★ CLOSE THE PROGRESS LINE FIRST. It is written WITHOUT a trailing newline so the next
          //    one can overwrite it — which means an ordinary line arriving next would run on the
          //    end of it: "second 59/59[MSF] locked — carrier +13 dB" (Stuart's screenshot,
          //    2026-08-11). Only after a progress line, because RTTY streams text character by
          //    character with no newlines of its own and must not be broken up.
          if (decProgressLine) { text += '\n'; decProgressLine = false; }
          text += chunk;
        }
      }
      el.textContent = text.slice(-8000);
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
      pushSpotsToMap();      // ★ keep an open map current — see pushSpotsToMap()
      setDecLive(true);
    },
  });
  decoders.connect();

  initRdsResize();
  initRspControls();
  initAhfControls();
  initHrfControls();
  $('decodersBtn').onclick = () => togglePanel('decodersPanel');
  $('decClose').onclick = () => closePanels();
  // ★ The panel owns the unread count only while it is OPEN — chatOpened(false) on close is what
  //   lets it start counting again, so a message that lands while you are reading is not "unread".
  $('chatBtn').onclick = () => { togglePanel('chatPanel'); chatOpened(isPanelOpen('chatPanel')); };
  $('chatClose').onclick = () => { closePanels(); chatOpened(false); };
  initChat({
    say: (id) => spec?.send({ type: 'say', id }),
    onUnread: (n) => {
      for (const id of ['chatUnread', 'mChatUnread']) {
        const el = document.getElementById(id);
        if (!el) continue;
        el.textContent = String(n);
        (el as HTMLElement).hidden = n <= 0;
      }
      // ★★ AND THE BUTTON ITSELF BREATHES — see .chatBreathing. The count says how many; the
      //    pulse is what makes somebody LOOK, which on a shared dial is the whole point: a
      //    message is usually a question waiting on an answer.
      //  ★ Both buttons, because the compact layout has its own and a message must not announce
      //    itself on one screen size and stay silent on another.
      for (const id of ['chatBtn', 'mChat']) {
        const b = document.getElementById(id);
        if (b) b.classList.toggle('chatBreathing', n > 0);
      }
    },
  });

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

  // ★ The time station is a preset: changing it re-attaches, because the shim builds the decoder
  //   from the attach parameters and cannot be re-pointed in place.
  segButtons('timeStation', 'station', (v) => {
    timeStation = v;
    const st = TIME_STATIONS[v];
    const hint = document.getElementById('timeHint');
    if (hint && st) {
      hint.innerHTML = st.hint + ' The fields fill in as they arrive'
        + (v === 'rwm' ? '.' : '; a reading is only shown once a second minute confirms it.');
    }
    reattachIf('time');
  });

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
  initDecHeadScroll();
}

/* ★★★ THE HEADER SCROLLS, AND UNTIL NOW NOTHING SAID SO.
 *
 *  On a narrow decoder box #decBoxHead becomes a horizontal scroller with the scrollbar hidden on
 *  purpose, so the only clue is a button sliced in half at the edge — which reads as a broken
 *  layout rather than "there is more this way". Stuart had the FT8 box open, could not find
 *  resize or minimise, and only reached them days later by REMEMBERING we had built the scroll.
 *
 * ★★ Per SIDE, and only when that side actually overflows — his spec exactly: right only, left
 *    only, or both. An arrow that points at nothing teaches people to ignore arrows, so the test
 *    is the real scroll position and not "is this a phone".
 * ★ Recomputed on scroll, on RESIZE of the strip, and on any change to its CHILDREN — the buttons
 *   come and go by decoder (PREV/SAVE are hidden for text modes, the spot filters only exist for
 *   spot decoders), so overflow changes without the box ever being resized or scrolled.
 * ★ A 1px tolerance because scrollWidth/clientWidth are fractional under a zoom or a device pixel
 *   ratio, and an exact comparison leaves an arrow permanently lit at a hard end.
 */
function initDecHeadScroll(): void {
  const wrap = document.getElementById('decHeadWrap');
  const strip = document.getElementById('decBoxHead');
  if (!wrap || !strip) return;

  const update = () => {
    const max = strip.scrollWidth - strip.clientWidth;
    wrap.classList.toggle('canL', strip.scrollLeft > 1);
    wrap.classList.toggle('canR', strip.scrollLeft < max - 1);
  };

  // ★ Most of a screenful, not all of it: a jump that leaves nothing in common is disorienting,
  //   and the overlap is what tells you it moved rather than replaced.
  const nudge = (dir: number) => {
    strip.scrollBy({ left: dir * Math.max(80, strip.clientWidth * 0.7), behavior: 'smooth' });
  };

  for (const el of Array.from(wrap.querySelectorAll<HTMLButtonElement>('.decHeadArr'))) {
    const dir = el.classList.contains('left') ? -1 : 1;
    el.addEventListener('click', (e) => { e.preventDefault(); e.stopPropagation(); nudge(dir); });
  }

  strip.addEventListener('scroll', update, { passive: true });
  try {
    new ResizeObserver(update).observe(strip);
    new MutationObserver(update).observe(strip, { childList: true, subtree: true,
                                                  attributes: true, attributeFilter: ['style'] });
  } catch { /* older browsers simply keep the arrows hidden — no worse than before */ }
  window.addEventListener('resize', update);
  update();
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
  $('timeSettings').hidden = activeDec !== 'time';
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
  // ★ Which decoders BIG should widen: the ones whose content is drawn at the box's width (SSTV,
  //   WEFAX) or written in long lines (the spot list). Text decoders get height instead.
  $('decBox').classList.toggle('wide', image || isSpots);
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
  // ★★ …AND FOR A TEXT DECODER IT MEANS TALLER, NOT WIDER. `.wide` is set in showDecBox for the
  //    decoders that actually draw at the box's width; RTTY, NAVTEX and the time signals get the
  //    height instead. See the CSS — a wider box for short lines is just more black.
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
/* ★★★ THE PANEL JOLTS WHILE YOU ARE READING IT, and the cure must not be truncation. These rows
 *     update several times a second and their verdicts change LENGTH — "IMS standing by · multipath
 *     not measurable at this S/N" is five lines where "clean" is one — so every change re-wraps the
 *     row and shoves everything below it. Stuart: "there is a lot of jitter when the info changes".
 *  ★★★ CLIPPING IS THE ONE FIX THAT IS FORBIDDEN HERE. `rdsFix` already tried it and the ellipsis
 *      landed exactly on the useful half — "5.5 kHz · …" instead of the verdict — which is recorded
 *      in the CSS above as TRUNCATION ATE THE VERDICT. So: reserve space, never remove text.
 *  ★★ A HIGH-WATER MARK, NOT A TABLE OF GUESSED HEIGHTS. The worst case per row depends on the
 *     string, the font, the container width and the reader's zoom, so any constant I wrote here
 *     would be wrong on somebody's screen. Each row simply remembers the tallest it has ever been
 *     and never returns below it: self-measuring, correct at any width, and it cannot hide a
 *     verdict because nothing is ever constrained smaller than its own content.
 *  ★ It costs one jump the first time a row hits a new longest value, and none afterwards. Reset
 *    on disconnect would only re-introduce the jump, so the mark is kept for the session. */
function lockRdsRowHeights() {
  if (!rdsPanelOpen()) return;          // never measure a hidden panel — it reads back 0
  const rows = document.querySelectorAll<HTMLElement>('.rdsF');
  for (let i = 0; i < rows.length; i++) {
    const el = rows[i];
    const h = el.offsetHeight;
    if (!h) continue;
    const prev = Number(el.dataset.maxH || 0);
    if (h > prev) { el.dataset.maxH = String(h); el.style.minHeight = h + 'px'; }
  }
}

function renderRds() {
  const dash = '—';
  // ★ The flag and logo move into the header while the bar is hidden, so the station keeps
  // the same visual identity it had on the VTS rather than becoming a table of numbers
  // (Stuart, 2026-07-26).
  $('decFlag').textContent = rdsName || rdsPi > 0 ? isoToFlag(rdsIso) : '';
  // ★ TWO COPIES, ON PURPOSE. The header badge is the identity you glance at while the panel
  // is minimised; the big one under the MPX fills space the column already leaves empty and is
  // the one you actually look at. Both are driven from the same URL so they cannot disagree.
  // ★★★ A DEAD FAVICON MUST HIDE, HERE TOO. The 404-handling was added to #vtsLogo and to NOTHING
  //     ELSE, so these two kept showing the browser's broken-image glyph in a reserved box —
  //     Classic FM, whose radio-browser favicon is dead, rendered a "?" tile where its artwork
  //     should be (Stuart, 2026-08-11). Same bug, same day, two lines away from the fix.
  //     ★ `show` is added only ON LOAD: an <img> is guilty until it has decoded, or the box
  //       flashes broken on every slow logo.
  //     ★ Clearing rdsLogoUrl stops this session retrying a link already known to be dead — and
  //       lets the next candidate name (see resolveRdsLogoBest) have its turn.
  for (const id of ['decLogo', 'rdsLogoBig']) {
    const el = document.getElementById(id) as HTMLImageElement | null;
    if (!el) continue;
    if (!rdsLogoUrl) { el.classList.remove('show'); continue; }
    if (el.src !== rdsLogoUrl) {
      const url = rdsLogoUrl;
      el.classList.remove('show');
      el.onload  = () => { el.classList.add('show'); };
      el.onerror = () => {
        el.classList.remove('show');
        if (rdsLogoUrl === url) rdsLogoUrl = '';
      };
      el.src = url;
    } else if (el.complete && el.naturalWidth > 0) {
      el.classList.add('show');
    }
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
  // ★★ MPX S/N, and WHAT IT DID. A number on its own invites "is 22 good?"; showing the corner
  //    the receiver chose because of it answers the question the listener actually has, which is
  //    "why does this sound the way it does". Only mentioned when it is actually acting — on a
  //    clean signal the treatment is bypassed entirely and saying "15.0 kHz" would imply a filter
  //    is in circuit when none is.
  const snrEl = $('rxMpxSnr');
  const snr = rdsExt?.mpxSnr ?? 0;
  const wspOff = !$('wspBtn').classList.contains('on');
  if (rdsExt && rdsExt.snrOk === false) {
    // ★★ NO PILOT, NO FIGURE. Printing 70 dB — on a meter whose leakage floor caps it near 34 —
    //    beside 580.9% multipath was the panel reporting arithmetic as measurement.
    snrEl.textContent = 'no pilot to measure';
    snrEl.style.color = '';
  } else if (rdsExt && snr > 0.5) {
    const lmr = rdsExt.hiCutLmr ?? 15000, aud = rdsExt.hiCutAud ?? 15000;
    const acting = lmr < 14000 || aud < 14000;
    // ★★ "BYPASSED" IS NOT "CLEAN". With the switch off nothing is acting either, but for a
    //    completely different reason — and printing "clean · no treatment" over a hissy signal
    //    because the LISTENER turned the treatment off would be the panel telling a flat lie
    //    (Stuart, 2026-08-14: "you could put a bypass label in to show it being bypassed").
    const how = wspOff ? 'bypassed'
              : !acting ? 'clean · no treatment'
              : aud < 14000 ? `blend ${(lmr / 1000).toFixed(1)}k · cut ${(aud / 1000).toFixed(1)}k`
              : `blend ${(lmr / 1000).toFixed(1)}k`;
    snrEl.textContent = `${snr.toFixed(0)} dB · ${how}`;
    // ★ Green only when nothing is being done. Amber is not a warning here — it means the
    //   receiver is working for its living, which on a difficult signal is the good outcome.
    snrEl.style.color = wspOff ? '' : acting ? '#ffd479' : '#7dff9a';
  } else { snrEl.textContent = dash; snrEl.style.color = ''; }

  // ★★★ MULTIPATH — and it is deliberately NOT described as bad news at low levels. Every real
  //     signal has some, and a DXer needs to know whether what they are hearing is a reflection
  //     (which an aerial rotation may fix) or noise (which it will not).
  const mpEl = $('rxMultipath');
  const mp = rdsExt?.multipath ?? 0;
  // ★★★ ORDERED SO THAT A TRUSTED READING ALWAYS PRINTS SOMETHING. The dash was a FOURTH state
  //     nobody meant to design: valid, trusted, but numerically ~0 after the noise correction, so
  //     it fell past every branch to the fallback. On a 27 dB station that made the field flicker
  //     between a figure and "—" (Stuart, 2026-08-14, second report of flashing — the first was a
  //     validity threshold, this one is a different cause with the same symptom).
  // ★★ "NONE DETECTED" IS A RESULT. Zero multipath on a signal we can measure is exactly the good
  //    news a DXer wants, and printing a dash for it says "no data" — the opposite meaning.
  if (rdsExt && rdsExt.snrOk === false) {
    mpEl.textContent = 'no pilot to measure';
    mpEl.style.color = '';
  } else if (rdsExt && rdsExt.multipathOk) {
    const pct = mp * 100;
    const label = pct < 0.5 ? 'none detected'
                : mp < 0.03 ? 'clean' : mp < 0.10 ? 'slight' : mp < 0.20 ? 'moderate' : 'severe';
    /* ★★ AND WHETHER IMS IS ACTING ON IT. Multipath is the fault IMS answers, so its action
     *    belongs beside the measurement that triggered it — the same measurement-and-action
     *    pairing IF NARROW and CEQ already use. Silent when IMS is not the reason for the blend,
     *    which is most of the time: CEQ takes strong signals, and below ~14 dB the noise curve
     *    has already blended further than IMS would ask for. */
    /* ★★★ AND WHY IT IS NOT, WHENEVER IT IS NOT — INCLUDING "nothing to suppress". I first left
     *      that case silent as clutter, reasoning that the percentage beside it already says so.
     *      That was wrong twice over: CEQ prints "standing by · nothing to correct" in the very
     *      same panel for the very same situation, so the silence read as a BROKEN readout rather
     *      than a calm one — Stuart went looking for it twice ("not seeing the IMS status", "still
     *      no IMS standby status"). A reader cannot tell "this control has nothing to say" from
     *      "this control is not reporting"; only the control can, and it must say which.
     *  ★ Silent only when the listener has switched IMS OFF, where the button already says it. */
    const base = pct < 0.5 ? label : `${pct.toFixed(1)}% · ${label}`;
    mpEl.textContent = base;
    mpEl.style.color = mp < 0.03 ? '#7dff9a' : mp < 0.10 ? '' : mp < 0.20 ? '#ffd479' : '#ff9a9a';
    mpHeld = mpEl.textContent;
  } else if (rdsExt && mpHeld) {
    // ★ Confidence dipped — dim the last good figure rather than replacing it. The eye is drawn to
    //   change, so a panel that swaps text is harder to read than one that fades.
    mpEl.textContent = `${mpHeld} · held`;
    mpEl.style.color = 'rgba(255,255,255,0.35)';
  } else if (rdsExt) {
    mpEl.textContent = 'too noisy to judge';
    mpEl.style.color = '';
  } else { mpEl.textContent = dash; mpEl.style.color = ''; }

  /* ★★★ APPENDED AFTER THE WHOLE CHAIN, NOT INSIDE ONE BRANCH — and that placement was the bug.
   *      The note lived in the "multipath reading is VALID" arm, so on the one station where the
   *      question is most pressing — 46.1% severe, tagged `held` because the meter cannot judge at
   *      6 dB — the row took the `held` arm instead and IMS said nothing at all. Stuart looked for
   *      it three times and then reasonably suspected the deploy: "still not seeing any of the IMS
   *      status ... which makes me think stale packages are being pushed". The package was right;
   *      the branch was wrong. (Verified by decoding the served bundle before changing anything —
   *      see memory/hermes_bundle_grep_utf16 for why guessing at that costs an hour.)
   *  ★ A control's status must not depend on whether SOME OTHER reading happened to be valid.
   *  ★ mpHeld deliberately does NOT carry this: it stores the last good MULTIPATH figure, and
   *    freezing a stale IMS verdict into it would outlive the state it describes. */
  {
    const imsHz = Number(rdsExt?.imsBlend ?? 0);
    const imsWhy = Number(rdsExt?.imsWhy ?? 1);
    const imsNote = imsHz > 0 ? `IMS blending L−R to ${(imsHz / 1000).toFixed(1)}k`
                  : imsWhy === 2 ? 'IMS standing by · CEQ has it'
                  : imsWhy === 3 ? 'IMS standing by · nothing to suppress'
                  : imsWhy === 4 ? 'IMS standing by · NR already blending further'
                  : imsWhy === 5 ? 'IMS standing by · multipath not measurable at this S/N'
                  : '';
    if (rdsExt && imsNote) mpEl.textContent = `${mpEl.textContent} · ${imsNote}`;
  }

  // ★★ THE BLANKER'S RATE, a DIAGNOSTIC before it is a control: it answers "is that crackle me or
  //    the station?", which an owner otherwise has no way to settle.
  const nbEl = $('rxNb');
  const nbR = (rdsExt?.nbRate ?? 0) * 100;
  if (rdsExt) {
    nbEl.textContent = nbR < 0.005 ? 'nothing to blank'
                     : `${nbR.toFixed(nbR < 1 ? 2 : 1)}% blanked`;
    // ★ Amber only once it is removing enough to matter — a trickle is normal and colouring it
    //   would cry wolf.
    nbEl.style.color = nbR > 0.5 ? '#ffd479' : nbR < 0.005 ? '#7dff9a' : '';
  } else { nbEl.textContent = dash; nbEl.style.color = ''; }

  // ★★ CEQ, SHOWN AS BEFORE -> AFTER, because "engaged" says nothing about whether it HELPED, and
  //    a blind equaliser quietly making things worse is the failure mode that matters.
  const ceqEl = $('rxCeq');
  if (rdsExt && rdsExt.ceqOn) {
    const before = (rdsExt.multipath ?? 0) * 100, after = (rdsExt.ceqAfter ?? 0) * 100;
    const gained = before > 0.5 && after < before * 0.8;
    ceqEl.textContent = `${before.toFixed(1)}% → ${after.toFixed(1)}%` + (gained ? '' : ' · little change');
    ceqEl.style.color = gained ? '#7dff9a' : '';
  } else if (rdsExt) {
    // ★★ AND WHY IT IS NOT RUNNING. "Standing by" alone left the owner unable to tell whether it
    //    had declined or failed — especially on a station being FOUGHT OVER, which reads as severe
    //    multipath while sitting well under the S/N the equaliser needs. Co-channel is not
    //    multipath: a reflection is your own signal arriving twice and can be inverted; a second
    //    TRANSMITTER cannot be, by anyone.
    const why = rdsExt.ceqWhy ?? 3;
    ceqEl.textContent = why === 1 ? 'off'
                      : why === 2 ? 'signal too weak to equalise'
                      : 'standing by · nothing to correct';
    ceqEl.style.color = '';
  } else { ceqEl.textContent = dash; ceqEl.style.color = ''; }

  // ★★ THE IF-NARROWING BENEFIT, read against the CURRENT state — the shadow always evaluates THE
  //    OTHER OPTION, so the sign flips the moment the filter engages and a bare number is
  //    genuinely ambiguous.
  const ifEl = $('rxIfGain');
  const ifG = rdsExt?.ifGain ?? 0;
  const ifC = rdsExt?.ifCand ?? 0;
  const ifBw = rdsExt?.ifBw ?? 0;
  /* ★★★ AN APPLIED WIDTH IS REPORTED WHATEVER ASKED FOR IT. This row used to be gated ENTIRELY on
   *      `ifC` — IMS's shadow candidate — so with IMS switched off it fell to a dash even while
   *      the filter was genuinely narrowed, which since auto bandwidth exists is a lie about the
   *      receiver. Stuart: "if i toggle off the IMS the auto bw readout ... is not showing any
   *      narrowing." Two requesters share this filter (see RxPipeline::setAutoBandwidth); the row
   *      reports the RESULT, so it cannot belong to either one of them.
   *  ★★ The "wide would gain/cost" half is IMS's measurement and is still only shown when IMS has
   *     actually measured it. Without it the width is stated plainly rather than dressed up with a
   *     benefit figure nobody computed. */
  const ifShadow = ifC > 0 && (rdsExt?.mpxSnr ?? 0) > 0.5;
  if (rdsExt && ifBw > 0) {
    ifEl.textContent = `${Math.round(ifBw / 1000)}k narrow` + (ifShadow
      ? ` · wide would ${ifG > 1.5 ? `gain ${ifG.toFixed(1)} dB` : `cost ${Math.abs(ifG).toFixed(1)} dB`}`
      : '');
    ifEl.style.color = '#7dff9a';
  } else if (rdsExt && ifShadow) {
    const kHz = Math.round(ifC / 1000);
    const verdict = ifG > 1.5 ? 'would help' : ifG < -1.5 ? 'would cost' : 'no real gain';
    ifEl.textContent = `wide · ${kHz}k ${verdict} (${ifG >= 0 ? '+' : ''}${ifG.toFixed(1)} dB)`;
    ifEl.style.color = ifG > 1.5 ? '#7dff9a' : '';
  } else { ifEl.textContent = dash; ifEl.style.color = ''; }

  const pdev = rdsExt?.pilotDev ?? 0;
  const rdev = rdsExt?.rdsDev ?? 0;
  const pEl = $('rxPilotDev'), rEl = $('rxRdsDev');
  if (pdev > 0.2) {
    const ok = pdev >= 6.0 && pdev <= 7.5;
    /* ★★ AND WHETHER THE PLL IS ACTUALLY LOCKED, which the deviation alone does not say: 5.2 kHz
     *    reads "low" against the 6.0-7.5 spec while the loop is locked and stereo is playing. The
     *    only other "lock" in this panel belongs to the RDS constellation, so without this the
     *    reader has one word covering two independent failures. */
    const plk = rdsExt?.pilotLock;
    const lockTxt = plk === undefined ? '' : plk ? ' · locked' : ' · NOT locked';
    pEl.textContent = `${pdev.toFixed(1)} kHz · ${ok ? 'nominal' : pdev < 6 ? 'low' : 'high'}${lockTxt}`;
    pEl.style.color = plk === false ? '#ffd479' : ok ? '#7dff9a' : '#ffd479';
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
  } else if (rdev >= 0) {
    // ★★ ALWAYS VISIBLE, EVEN AT ZERO. A dash cannot be told from a broken readout, and this one
    //    WAS broken — the server misspelled the field, so it had never populated at all. Someone
    //    measuring against a Pira needs to see the number is live and reading nothing, not an
    //    absence that might mean either (Stuart, 2026-08-11, for Hans).
    //    ★ -1 is the server's honest "no block sync, cannot measure", and stays a dash: that is a
    //      real absence rather than a real zero.
    rEl.textContent = `${rdev.toFixed(1)} kHz · no subcarrier`;
    rEl.style.color = '#ffd479';
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
  // ★★★ THE LIST WITH A TICK EACH, as the FM-DX Webserver shows it (Stuart, 2026-08-15). A bare
  //     "3 of 7" says entries are arriving damaged but not WHICH frequencies — and on a noisy
  //     station the unconfirmed ones are usually PHANTOMS manufactured by block errors rather than
  //     real alternatives. H F M, a Market Harborough community station running 22-27% block
  //     errors, was listing seven AFs across the band; a DXer needs to see which of those to
  //     believe. Confirmed entries are ticked and full-strength, unconfirmed are dimmed.
  // ★ Every entry stays TUNEABLE, confirmed or not: an unconfirmed AF is exactly the one you want
  //   to go and check by ear.
  const afAll = rdsExt?.afAll ?? [];
  if (!af.length && !afAll.length) {
    afEl.textContent = (rdsExt?.gtot ?? 0) > 0 ? 'none announced' : dash;
  } else if (afAll.length) {
    const seenSet = new Map<number, boolean>();
    for (const [khz, ok] of afAll) seenSet.set(khz, (seenSet.get(khz) ? true : false) || !!ok);
    const list = [...seenSet.entries()].sort((a, b) => a[0] - b[0]);
    afEl.innerHTML = list.map(([khz, ok]) =>
      `<a href="#" class="afLink${ok ? '' : ' afUnconf'}" data-khz="${khz}" title="${
        ok ? 'Confirmed — seen enough times to be believed'
           : 'Seen but NOT confirmed. On a noisy signal this is often a phantom created by block errors.'
      }">${(khz / 1000).toFixed(1)}${ok ? ' \u2713' : ''}</a>`).join('  ');
  } else {
    // Ascending, and de-duplicated: the same AF is re-announced constantly.
    const list = [...new Set(af)].sort((a, b) => a - b);
    afEl.innerHTML = list
      .map(khz => `<a href="#" class="afLink" data-khz="${khz}">${(khz / 1000).toFixed(1)}</a>`)
      .join('  ');
  }
  {
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
  lockRdsRowHeights();
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
  if (n < 8) return { text: 'RDS no lock', cls: 'bad' };
  const meanAbsX = sumAbsX / n;
  for (let i = 0; i < rx.length; i++) {
    const dx = Math.abs(rx[i]) - meanAbsX;
    sumXErr2 += dx * dx;
  }
  // Error energy is the scatter off the two ideal points; signal energy is the lobe offset.
  const err = Math.sqrt((sumY2 + sumXErr2) / n);
  if (meanAbsX < 1) return { text: 'RDS no lock', cls: 'bad' };
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
/** ★ ONE derivation of the map's points, used both to build the page and to push updates into
 *  it. Two copies would drift, and a live update that disagreed with the initial render is worse
 *  than no live update at all. */
function spotsMapPoints() {
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

  return pts;
}

/** The open map window, so new spots can be pushed to it. ★ Held rather than re-opened: a map
 *  that reopens itself would steal focus every time a spot arrived. */
let spotsMapWin: Window | null = null;

/** ★★★ PUSH NEW SPOTS TO AN OPEN MAP. The map baked its points into the page at open time, so it
 *  showed whatever had been decoded when you opened it and never changed — "it needs to live
 *  update or its a pointless map" (Stuart, 2026-08-05). FT8 decodes arrive every 15 seconds; a
 *  snapshot of them is a screenshot, not a map.
 *  ★ Silent and cheap when no map is open, because this runs on every spot. */
function pushSpotsToMap() {
  if (!spotsMapWin) return;
  if (spotsMapWin.closed) { spotsMapWin = null; return; }
  try {
    spotsMapWin.postMessage({ type: 'vibesdr-spots', spots: spotsMapPoints() }, '*');
  } catch { spotsMapWin = null; }      // window went away mid-push
}

function openSpotsMap() {
  const me = myPos();
  const pts = spotsMapPoints();

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
// ★★ MUTABLE, and everything that reads it lives in render(). The page used to be top-level
//    procedural code over a const, which is exactly why it could never update.
let spots = ${JSON.stringify(pts)};
const me = ${JSON.stringify(me)};
const COL = ${JSON.stringify(BAND_COLOUR)};

/* ★★★ preferCanvas IS AN AUDIO FIX, WHICH IS NOT WHERE ANYONE WOULD LOOK FOR ONE. This map is a
 *     SAME-ORIGIN window.open, so it shares its main thread with the page that is playing the
 *     radio — the audio WebSocket is received and Opus is decoded on that same event loop. Leaflet
 *     defaults circleMarker to SVG, one DOM node per spot, every one of them repositioned on every
 *     pan frame; on a busy band that is hundreds of nodes churning while the audio ring drains.
 *     The playout worklet has its own thread and keeps pulling, so what the listener hears is not
 *     the map stuttering, it is the AUDIO cutting in and out while they drag (Stuart, 2026-08-26,
 *     zooming in on an odd spot location).
 *  ★★ Canvas draws every spot into ONE element instead, which is the same picture for a fraction
 *     of the main-thread work. It does not make the window stop sharing the thread — that would
 *     mean moving audio receive and decode into a Worker — it makes the work small enough to fit
 *     between frames.
 *  ★ updateWhenZooming:false for the same reason and not for the tiles' sake: it stops Leaflet
 *    issuing tile work mid-animation that it is only going to throw away. */
const map = L.map('m', { worldCopyJump: true, preferCanvas: true })
  .setView(me ? [me.lat, me.lon] : [25, 5], me ? 4 : 3);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  { attribution: '&copy; OpenStreetMap', maxZoom: 14, updateWhenZooming: false }).addTo(map);

function radius(snr) {
  const s = Math.max(-24, Math.min(12, snr));
  return 4 + ((s + 24) / 36) * 9;
}
// Spot markers live in their own layer so a redraw can replace them without touching the
// receiver marker, the range rings or the tile layer.
const spotLayer = L.layerGroup().addTo(map);
function drawMarkers() {
  spotLayer.clearLayers();
  for (const s of spots) {
    L.circleMarker([s.lat, s.lon], {
      radius: radius(s.snr), color: '#00000066', weight: 1,
      fillColor: s.colour, fillOpacity: 0.85,
    }).addTo(spotLayer).bindPopup(
      '<div class="pop"><b>' + s.callsign + '</b><br>' +
      (s.country ? s.country + '<br>' : '') + s.grid +
      (s.km != null ? ' · ' + s.km + ' km' : '') + '<br>' +
      s.mode + ' · ' + s.band + ' · ' + (s.snr > 0 ? '+' : '') + s.snr + ' dB<br>' +
      (s.frequency / 1e6).toFixed(3) + ' MHz</div>');
  }
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
// ★★★ FIT ONCE, ON THE FIRST DRAW ONLY. Re-fitting on every update would yank the map away
//     from wherever the user had panned or zoomed, every fifteen seconds as the next FT8 cycle
//     lands — which would make live updating worse than the snapshot it replaces.
let fitted = false;
function fitOnce() {
  if (fitted) return;
  const all = spots.map(s => [s.lat, s.lon]);
  if (me) all.push([me.lat, me.lon]);
  if (all.length > 1) { map.fitBounds(all, { padding: [60, 60] }); fitted = true; }
}

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

// ★ Everything below reads the spots list, so it all has to live in here to be re-runnable.
// (No backticks in this comment: it sits inside a template literal and would end the string.)
function render() {
document.getElementById('count').textContent = String(spots.length);
document.getElementById('empty').textContent = spots.length
  ? '' : 'No spots with a grid yet — leave DIGITAL SPOTS running.';
drawMarkers();
fitOnce();
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

}
render();

// ★★★ LIVE. The opener pushes a fresh point list whenever a spot arrives.
addEventListener('message', (ev) => {
  if (!ev.data || ev.data.type !== 'vibesdr-spots') return;
  spots = ev.data.spots;
  render();
});

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
  spotsMapWin = w;
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
/** @param pushOnInit send the stored value to the server at wiring time. TRUE for a listener's own
 *  processing, which the server cannot know. **FALSE for anything SERVER-WIDE**: see below.
 *  ★★★ A CLIENT MUST NOT COMMAND A SHARED SETTING FROM ITS OWN STORAGE. For a control that belongs
 *      to the RADIO rather than to this listener, only the radio knows the truth — so a fresh page
 *      must READ it, never assert it. Pushing at wiring time makes a reload change the receiver
 *      instead of reading it, and on a shared server it does that to everyone else too.
 *  ★★★ IT HAPPENED, AND IT SURVIVED THE FIX THAT CAUSED IT. AUTO BW defaults ON at the server, yet
 *      Stuart's browser turned it off on every load: an earlier bug (setToggleTo overwriting the
 *      pref from a stale report) had written `autobw:false` into his storage, and this line then
 *      dutifully sent it. Fixing the writer did nothing for the value already written — "auto BW
 *      is not default on". A stale pref outlives the bug that made it, so the cure has to be that
 *      the stale pref is never authoritative in the first place. */
function toggle(id: string, apply: (on: boolean) => void, prefKey?: string, initial = false,
                pushOnInit = true) {
  const el = $<HTMLButtonElement>(id);
  const saved = prefKey ? prefs()[prefKey] : undefined;
  /* ★★★ A SERVER-AUTHORITATIVE CONTROL MUST NOT READ ITS STATE FROM STORAGE AT ALL — not even as a
   *      first guess. pushOnInit=false already stopped this browser COMMANDING the radio; this
   *      stops it DISPLAYING a value the radio never reported. The two are the same mistake seen
   *      from opposite ends, and fixing only the first left AUTO BW reading OFF for ever: the
   *      stored `autobw:false` (written by an earlier bug, long since fixed) still won the line
   *      below, so seeding `initial` from the server's report never got a look-in. Three separate
   *      fixes bounced off this one expression.
   *  ★ For those controls `initial` IS the server's last word (or the documented default), and a
   *    state report overwrites it moments later anyway. The pref is still SAVED on click, so a
   *    server that reports nothing keeps the listener's choice. */
  let on = (pushOnInit && typeof saved === 'boolean') ? saved : initial;
  const run = (push: boolean) => {
    el.classList.toggle('on', on);
    el.textContent = on ? 'ON' : 'OFF';
    if (push) apply(on);
  };
  el.onclick = () => {
    on = !on; recentPress.set(id, Date.now()); run(true);
    if (prefKey) savePref(prefKey, on);
  };
  run(pushOnInit);
}

/** ★★★ RENDER A TOGGLE TO THE STATE THE SERVER REPORTS — without firing its handler.
 *  These controls are sticky on the server and shared between listeners, so what a fresh page
 *  believes is irrelevant: only the radio knows. Calling click() would render it AND send the
 *  value straight back, so a client arriving with a stale pref would COMMAND the radio to match
 *  it — the reload would change the radio instead of reading it, and on a shared receiver it
 *  would do that to everyone else's audio.
 *  ★★ TWO TOGGLE STYLES IN THIS FILE and the difference is silent: `toggle()` writes ON/OFF into
 *     the button, the RSP toggles carry their own label ("RF NOTCH") and only take the class.
 *     Overwriting the label unconditionally would blank those buttons — so only rewrite text
 *     that IS an ON/OFF word.
 *  ★ The saved pref is updated too, or the next reconnect's pushSettingsToServer() would send
 *    the stale value and undo what we have just learned. */
/** ★★★ WHAT THE USER JUST PRESSED OUTRANKS A REPORT THAT LEFT BEFORE THEY PRESSED IT.
 *  A state report is a snapshot of the radio at the moment it was BUILT. Press a button and one is
 *  usually already in flight carrying the old value, so rendering it unconditionally flips the
 *  control straight back — and, far worse, setToggleTo also SAVES THE PREF, so the stale value is
 *  written to storage and pushed at the server on the next reconnect. The switch then turns itself
 *  off for real. Stuart: "the button wont stay active", and the Pi's log shows auto bandwidth
 *  running for forty seconds and then stopping on its own.
 *  ★★ THIS RACE IS AS OLD AS THE FUNCTION AND HAS ONLY JUST BECOME REACHABLE. Every caller passed
 *     an element id that does not exist ('wsp' for 'wspBtn'), so setToggleTo returned at the first
 *     line and did nothing at all — for NR, IMS, CEQ and NB alike. Fixing those ids switched on a
 *     path that had never run. A dead code path is not a tested one.
 *  ★ Four seconds covers a round trip on a slow tunnel and is far shorter than any interval at
 *    which somebody toggles a switch deliberately. */
const recentPress = new Map<string, number>();

/** ★★★ THE SERVER'S LAST WORD ON A SERVER-WIDE TOGGLE, KEPT FOR WHEN THE CONTROL IS WIRED LATER.
 *      The PROCESSING buttons are built when the audio menu is first opened — which is AFTER the
 *      state report has arrived and been rendered. So `toggle()` painted the button from this
 *      browser's stored value, and no further state report ever came to correct it: the server had
 *      AUTO BW on and adjusting (52 changes in four minutes in the log) while the button sat there
 *      reading OFF. Stuart, three rounds in: "the damned auto BW is not defaulting to the on
 *      position like it should." It WAS on; only the button disagreed.
 *  ★★ Not-yet-known stays `undefined` rather than `false`, so "no report seen" and "reported off"
 *     cannot be confused — that conflation is what made the stale pref look authoritative. */
let srvAutoBw: boolean | undefined;

function setToggleTo(id: string, on: boolean, prefKey?: string,
                     label?: (on: boolean) => string) {
  const el = document.getElementById(id);
  if (!el) return;
  if (Date.now() - (recentPress.get(id) || 0) < 4000) return;
  if (label) el.textContent = label(on);
  el.classList.toggle('on', on);
  const t = (el.textContent || '').trim();
  if (t === 'ON' || t === 'OFF') el.textContent = on ? 'ON' : 'OFF';
  if (prefKey) savePref(prefKey, on);
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

/** ★★★ THE SLIDER'S WHOLE TRAVEL SAT IN THE HALF OF THE RANGE THAT BARELY DOES ANYTHING, so it
 *  behaved like an on/off switch: "not changing in sound from 5% - 100%" (Stuart, 2026-08-26).
 *
 *  AudioNR has TWO controls and they are not side by side. `gmin` — the residual-noise floor —
 *  moves across strength 0..1, and it only decides how far bins that are ALREADY being suppressed
 *  get pushed down (-14 dB … -60 dB), which is a subtle change once the noise is gone. The control
 *  that alters how MUCH gets suppressed is the over-subtraction factor, and audio_nr.cpp guards it
 *  with `max(0, strength - 1)` — so it does nothing at all until strength passes 1.0.
 *
 *  ★★ The engine says who it was built for, right beside that line: "Strength >1 (slider 16..20)".
 *     It expects a TWENTY-STEP slider whose top quarter crosses into over-subtraction. This client
 *     sends a percentage as `pct/100`, which tops out at exactly 1.0 — the boundary — so the entire
 *     aggressive half was unreachable from the web client no matter where the slider was dragged.
 *  ★ So map the percentage onto the range the DSP was actually written for, keeping the same shape:
 *    0..80% covers the floor (0..1.0), and the last fifth crosses into over-subtraction (1.0..1.4,
 *    where setStrength() clamps). Same wire field, same units, nothing else has to know. */
function nrStrength(pct: number): number {
  const p = Math.max(0, Math.min(100, pct));
  return p <= 80 ? p / 80 : 1 + ((p - 80) / 20) * 0.4;
}

/** ★★★ THE EXACT INVERSE OF nrStrength(), AND IT MUST STAY EXACT. The server echoes the strength
 *  it is using and onDspState renders it back onto the slider — then DISPATCHES an `input` event,
 *  which sends the value again. That round trip is a feedback loop, so any disagreement between
 *  the two directions is not a small display error, it is a RATCHET: this read `strength * 100`
 *  against a forward map that divides by 80, so every echo multiplied the setting by 1.25 and it
 *  climbed on its own until it stuck at 100% (Stuart, 2026-08-26: "I set noise reduction to 30%
 *  but it has a mind of its own and increased to 100% by itself").
 *  ★★ I introduced this the same day by changing the forward map alone. **A wire value converted
 *     in two places must be changed in both, in the same edit** — the loop is what turns a units
 *     mismatch into a runaway instead of an off-by-a-bit. */
function nrPercent(strength: number): number {
  const s = Math.max(0, Math.min(1.4, strength));
  return s <= 1 ? s * 80 : 80 + ((s - 1) / 0.4) * 20;
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
      wfScroll = (b.dataset.wfscroll as typeof wfScroll) || 'default';
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
  { const v = prefs().wfScroll;   if (v === 'sharp' || v === 'default' || v === 'smooth') wfScroll = v; }
  { const d = prefs().wfDataRate; if (typeof d === 'number' && [0, 20, 10, 5].includes(d)) wfDataRate = d; }
  if (wfDataRate > 0 && wfSpeed < wfDataRate) wfSpeed = wfDataRate;
  applyWaterfallRates();

  toggle('idleSaver', (on) => {
    if (idleForced) return;         // owner-enforced: the control is locked, not merely ignored
    idleSaver = on;
    if (!on && throttled) { throttled = false; spec?.setFftRate(wantedFps()); updateStatus(); }
  }, 'idleSaver', true);

  toggle('biasT', (on) => spec!.setHwBiasT(on), 'biasT');
  // ★ READ, DO NOT ASSERT — see the pushOnInit note on toggle(). The AGC belongs to the radio and
  //   is persisted in its config; this button reports it and changes it, but must not command it
  //   from stale storage the moment a page loads.
  toggle('agc',   (on) => spec!.setHwAgc(on),   'agc', false, /*pushOnInit=*/false);
  segment('dsSeg', 'ds', (v) => spec!.setHwDirectSampling(v as 0 | 1 | 2), 'directSampling');

  const gainAuto = $<HTMLButtonElement>('gainAuto');
  gainAuto.onclick = () => {
    // ★★ THE OWNER MAY FIX IT ON. Locked means a listener can turn the AGC ON but not OFF — the
    //    same promise the RSP and Airspy have always kept, and now true on a dongle as well. The
    //    server refuses it regardless; refusing here too is what stops the button lying about
    //    having worked.
    const on = !gainAuto.classList.contains('on');
    // ★ Refused in place rather than with a message of its own: the button carries the reason in
    //   its title, and the server refuses it regardless. What must NOT happen is the button
    //   flipping to "off" and the radio ignoring it — a control that lies about having worked.
    if (!on && hwAgcLocked) return;
    gainAuto.classList.toggle('on', on);
    $<HTMLInputElement>('gain').disabled = on;
    if (on) spec!.setHwGain(0, true);
    else spec!.setHwGain(hwGains[Number($<HTMLInputElement>('gain').value)] ?? 0, false);
  };

  // ── Audio (server-side DSP in the shim) ──────────────────────────────────
  setupSquelchBar();

  slider('nr', 'nrVal',
    (v) => (v === 0 ? 'OFF' : `${v}%`),
    (v) => spec!.setNr(v > 0, nrStrength(v)),
    'nr');

  toggle('notch', (on) => spec!.setNotch(on), 'notch');
  toggle('stereoBtn', (on) => {
    // ★ "ST", not "ON". Beside NR / IMS / CEQ / NB a bare "ON" names nothing — the eye reads four
    //   labelled buttons and one that could belong to any of them, or to the row (Stuart).
    $('stereoBtn').textContent = on ? 'ST ON' : 'ST OFF';
    spec!.setStereo(on);
  }, 'stereo', true);
  // ★ Defaults ON (the `true`), like stereo itself: it only acts on a signal that needs it, so a
  //   listener who never opens this panel should get the better sound.
  toggle('wspBtn', (on) => {
    $('wspBtn').textContent = on ? 'NR ON' : 'NR OFF';
    spec!.setWeakProc(on);
  }, 'wsp', true);
  // ★ IMS is its OWN switch. NR works on noise, IMS works on a neighbour, and measured they want
  //   opposite actions — so one button for both would leave a listener unable to tell which of the
  //   two was helping (Stuart: "where is the IMS button to toggle it?").
  toggle('imsBtn', (on) => {
    $('imsBtn').textContent = on ? 'IMS ON' : 'IMS OFF';
    spec!.setIms(on);
  }, 'ims', true);
  toggle('ceqBtn', (on) => {
    $('ceqBtn').textContent = on ? 'CEQ ON' : 'CEQ OFF';
    spec!.setCeq(on);
  }, 'ceq', true);
  toggle('nbBtn', (on) => {
    $('nbBtn').textContent = on ? 'NB ON' : 'NB OFF';
    spec!.setNoiseBlanker(on);
  }, 'nb', true);
  // ★★ DEFAULTS ON, like the other four. It is the TEF6686's own behaviour and the reason Stuart
  //    went looking for it: "if you disable auto bandwidth it becomes really messy and noisy". It
  //    declines to act on anything that is not FM broadcast and on anything with no pilot, so it
  //    costs a listener nothing where it cannot help.
  // ★ SERVER-WIDE, so it is READ from the state report and never asserted from storage — see the
  //   pushOnInit note on toggle(). The button below is only its first paint.
  toggle('autoBwBtn', (on) => {
    $('autoBwBtn').textContent = on ? 'AUTO BW ON' : 'AUTO BW OFF';
    spec!.setAutoBw(on);
  }, 'autobw', srvAutoBw ?? true, /*pushOnInit=*/false);
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
    // ★ A BUTTON IS LABELLED WITH WHAT IT WILL DO, not with the state it is in. This read the
    //   state, so it said SHOW while the trace was plainly on screen (Stuart, 2026-08-06).
    showBtn.textContent = specOn ? 'HIDE' : 'SHOW';
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
  const nr = num('nr');             if (nr !== undefined) spec.setNr(nr > 0, nrStrength(nr));
  const notch = bool('notch');      if (notch !== undefined) spec.setNotch(notch);
  const stereo = bool('stereo');    if (stereo !== undefined) spec.setStereo(stereo);
  const wsp = bool('wsp');          if (wsp !== undefined) spec.setWeakProc(wsp);
  const ims = bool('ims');          if (ims !== undefined) spec.setIms(ims);
  const ceq = bool('ceq');          if (ceq !== undefined) spec.setCeq(ceq);
  const nb = bool('nb');            if (nb !== undefined) spec.setNoiseBlanker(nb);
  const deemph = num('deemph');     if (deemph !== undefined) spec.setDeemph(deemph * 1e-6);
  // ★★★ HARDWARE SETTINGS ARE THE OWNER'S, AND ONLY AN ADMIN MAY SEND THEM. Everything above this
  //     line is THIS listener's own processing — squelch, noise reduction, de-emphasis — and is
  //     rightly restored on every connect. PPM, bias-T, the tuner AGC and direct sampling are
  //     properties of somebody else's RADIO: pushing a value stored in this browser at a receiver
  //     we have no rights over is asserting a setting we cannot hold.
  //  ★★ The server refuses them, correctly — "refused bias-T — admin password required" appears
  //     in the log every time an ordinary listener connects (Stuart, 2026-08-22: "why is the
  //     iPhone asking for Bias-T though that is odd"). It is harmless and it is still wrong: it
  //     is a request that should never have been made, and it puts a refusal in the owner's log
  //     for a listener who did nothing.
  //  ★★★ AND IT WOULD NOT STAY HARMLESS. The refusal is the only thing standing between a stale
  //      local preference and a stranger's front end — bias-T feeds DC to an aerial. A client that
  //      only asks for what it may have does not depend on the server's refusal being perfect.
  //  ★ An ADMIN still restores them, because on their own receiver these ARE their settings.
  if (adminUnlocked) {
    const ppm = num('ppm');           if (ppm !== undefined) spec.setHwPpm(ppm);
    const biasT = bool('biasT');      if (biasT !== undefined) spec.setHwBiasT(biasT);
    /* ★★★ NOT THE AGC — THE SERVER PERSISTS THAT ONE ITSELF. `rtlAgc` is a config field, applied
     *     at startup (main.cpp), so it is the RADIO's setting and survives a restart. Re-asserting
     *     a value out of browser storage on every admin connect therefore does not restore a
     *     preference, it OVERRIDES the owner's saved configuration with whatever this tab last saw
     *     — which is why Stuart had to switch it back on "every time i connect". ppm, bias-T and
     *     direct sampling stay because they have no server-side home; the moment one of them gains
     *     a config field it should leave this list too. */
    const ds = num('directSampling'); if (ds !== undefined) spec.setHwDirectSampling(ds as 0 | 1 | 2);
  }

  // Re-assert the frame rate: the shim keeps whatever it was last set to, so a
  // reconnect could otherwise land in a stuck 5 fps with no way back.
  spec.setFftRate(wantedFps());

  // Gain and sample rate wait for hwinfo — we can't validate them until the
  // server has told us what this dongle actually supports.
}

/**
 * ★★★ BOUND THE SLIDER AT THE OWNER'S CEILING, AND MOVE IT DOWN IF IT IS ABOVE.
 *
 *     The server clamps regardless — it is the authority — but a slider left sitting above the
 *     cap shows a value the radio is not using, which reads as a broken control rather than as
 *     somebody's rule. Stuart, 2026-08-12: "we need to reduce the gain to the limit set
 *     automatically too, so the gain slider doesn't go into a prohibited range."
 * ★★ The slider is an INDEX into the discrete gain list, not a dB value, so the ceiling has to be
 *    translated into the highest index whose gain is within it. A cap that falls between two steps
 *    rounds DOWN — the whole point is not to exceed it.
 * ★ -1 = no ceiling: restore the full range rather than leaving yesterday's limit in place.
 */
/** The slider index whose step is closest to a gain the radio reports. The steps are the radio's
 *  own, so an exact match is normal; nearest covers a server reporting a value from a different
 *  table (a replugged dongle of another model). */
function nearestGainIdx(tenths: number): number {
  if (!hwGains.length) return -1;
  let best = 0, bestD = Infinity;
  for (let i = 0; i < hwGains.length; i++) {
    const d = Math.abs(hwGains[i] - tenths);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

function applyGainCap() {
  const g = document.getElementById('gain') as HTMLInputElement | null;
  if (!g || !hwGains.length) return;
  let maxIdx = hwGains.length - 1;
  if (hwGainCap >= 0) {
    maxIdx = -1;
    for (let i = 0; i < hwGains.length; i++) if (hwGains[i] <= hwGainCap) maxIdx = i;
    if (maxIdx < 0) maxIdx = 0;        // every step exceeds it — the lowest is the best we can do
  }
  g.max = String(maxIdx);
  if (Number(g.value) > maxIdx) {
    g.value = String(maxIdx);
    const tenths = hwGains[maxIdx] ?? 0;
    const lbl = document.getElementById('gainVal');
    if (lbl) lbl.textContent = `${(tenths / 10).toFixed(1)} dB`;
    savePref('gainIdx', maxIdx);
  }
  // ★ Say WHY it stops there. A slider that simply will not go further is indistinguishable from
  //   one that is broken; a note naming the owner's limit is a rule the listener can understand.
  g.title = hwGainCap >= 0
    ? `The owner has limited gain to ${(hwGainCap / 10).toFixed(1)} dB on this band`
    : '';
}

/** ★★★ A FIXED GAIN HAS NO CONTROLS AT ALL. Stuart, 2026-08-28: "on a gain locked server all the
 *  listener sees is no controls at all."
 *
 *  ★★ HIDDEN, NOT DISABLED, and that is the same rule applyRspLock already follows: a greyed
 *     slider still reads as an offer, and there is nothing here the listener can do to earn it.
 *     A cap leaves a working control with a lower ceiling; a lock leaves no control.
 *  ★ Called on every hwinfo, because the lock is reported PER BAND — it arrives and departs with
 *    the ceiling as the listener tunes, exactly as applyGainCap does. And called again from
 *    applyCaps, which rebuilds these rows when the radio announces itself and would otherwise
 *    un-hide them.
 *  ★ The RSP is not listed here: rspRestricted() already reads the flag, so applyRspLock does that
 *    radio's half. One rule, one reader. */
/** ★★ THE IF SLIDER'S CEILING. A GAIN position on the wire, and this slider is in REDUCTION dB —
 *  the two count opposite ways, exactly as the RF cap and the LNA state do, so the conversion
 *  happens once, here, and matches the server's (see the ifgr handler in local_sdr_shim.cpp).
 *  ★ Re-applied on every hwinfo because the ceiling is per band and changes as the listener tunes. */
function applyIfGainCap() {
  const gr = document.getElementById('rspIfGr') as HTMLInputElement | null;
  if (!gr) return;
  // ★ No conversion any more: the owner's figure and this slider are both a REDUCTION in dB, which
  //   is what Saber asked for — the number he sets is the number a listener sees the slider stop at.
  const floor = hwIfGrFloor >= 0
    ? Math.max(radioCaps?.ifGrMin ?? 20, Math.min(radioCaps?.ifGrMax ?? 59, hwIfGrFloor))
    : (radioCaps?.ifGrMin ?? 20);
  gr.min = String(floor);
  if (Number(gr.value) < floor) gr.value = String(floor);
  gr.title = hwIfGrFloor >= 0
    ? `The owner requires at least ${floor} dB of IF gain reduction on this band`
    : '';
}

function applyGainLocked() {
  const rowOf = (id: string) => document.getElementById(id)?.closest('.mrow') as HTMLElement | null;
  for (const id of ['gain', 'gainAuto']) {
    const r = rowOf(id);
    // ★ Only ever ADDS a reason to hide: applyCaps has already hidden these on the radios that
    //   have no single gain slider, and this must not un-hide them there.
    if (r && hwGainLocked) r.hidden = true;
  }
  // The HackRF's stages live in one block, which is the whole of its gain UI.
  const hrf = document.getElementById('hrfCtls') as HTMLElement | null;
  if (hrf && hwGainLocked) hrf.hidden = true;
  // ★ Guarded: applyRspLock reaches for the RSP panel's controls with `$`, which casts a missing
  //   element to non-null and would throw on a dongle. This function runs on EVERY hwinfo, so it
  //   cannot assume the panel the RSP paths can.
  if (document.getElementById('rspIfAgc')) applyRspLock();
}

/** The server tells us its real gain steps and sample rates (hwinfo) — the
 *  client can't query a remote dongle, so the controls are built from that. */
function populateHw() {
  if (hwGains.length) {
    const g = $<HTMLInputElement>('gain');
    g.min = '0';
    g.max = String(hwGains.length - 1);
    // ★★★ THE RADIO'S OWN GAIN WINS. This restored a REMEMBERED index and pushed it to the
    //     server on connect, which silently overrode the owner's resting gain — "I set the
    //     RTL-SDR on the server to return to 12.5db but when I opened it in the app it was at
    //     29.7db" (Stuart, 2026-08-15) — and on a SHARED receiver re-gained it under everyone
    //     already listening, from a preference they have never seen.
    // ★★ The old comment ("otherwise the slider shows a value the radio isn't using") had the
    //    right problem and the wrong end: the fix is for the server to SAY what the gain is and
    //    the slider to follow, not for every arriving client to impose its own.
    // ★ The stored preference is still the fallback for a server too old to tell us.
    const savedIdx = prefs().gainIdx;
    const nowIdx = hwGainNow >= 0 ? nearestGainIdx(hwGainNow) : -1;
    g.value = String(nowIdx >= 0 ? nowIdx
      : typeof savedIdx === 'number' ? Math.min(hwGains.length - 1, savedIdx)
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
    applyGainCap();   // ★ the saved index may be above a ceiling set since it was stored
    show();
    // ★★★ NOTHING IS PUSHED ON CONNECT. Arriving at a receiver is not a reason to change it.
    //     The slider now shows what the radio is set to; the listener moves it if they want to,
    //     and that is the only thing that writes a gain. The one exception is a server too old to
    //     report gainNow, where the remembered value is still better than a slider that lies.
    if (nowIdx < 0 && typeof savedIdx === 'number') {
      spec!.setHwGain(hwGains[Number(g.value)] ?? 0, false);
    }
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
    /* ★ THE IF FILTER, beside the rate because they are the same decision twice over: setting a
     *   sample rate re-derives this filter in librtlsdr, so the server re-asserts ours afterwards.
     *   NOT remembered in prefs — it is a property of the RADIO and it is shared, so a listener's
     *   saved preference must not silently re-narrow somebody else's front end on connect. */
    {
      const bw = document.getElementById('tunerBw') as HTMLSelectElement | null;
      if (bw) bw.onchange = () => spec!.setTunerBandwidth(Number(bw.value));
      // ★ VibeClarity. The server owns the state — this only asks, and the greying-out follows
      //   from what comes BACK on hwinfo, so the screen can never claim a mode the radio is not in.
    }

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
    /* ★★★ NOT WHILE DAB IS ON. This runs on every hwinfo, and it ENDS BY PUSHING the saved
     *  preference at the server — so a listener in DAB had 2.4 MS/s asserted at a receiver
     *  running an ensemble at 2.048, periodically, with nobody touching the control. Changing the
     *  rate rebuilds the source, so each push was a hole in the stream: clean, burst, clean. It
     *  looked like reception because nothing in the UI moved when it happened.
     *  ★ Show the truth instead. The server refuses this too — the client must not be the only
     *    thing standing between a live ensemble and its rate. */
    if (dabOn) {
      /* ★ And the option has to EXIST to be selectable — 2.048 is not in the dongle's advertised
       *  list, so setting .value alone leaves the <select> showing whatever was first and lying
       *  about the rate again, which is the fault this whole block was written to stop. Disabled,
       *  because in DAB it is not a choice: the server refuses any other value. */
      const o = document.createElement('option');
      o.value = '2048000';
      o.textContent = '2.048 MS/s (DAB)';
      r.appendChild(o);
      r.value = '2048000';
      r.disabled = true;
      return;
    }
    r.disabled = false;
    r.value = String(wanted);
    spec!.setHwSampleRate(wanted);
  }
}

function setMode(m: SDRMode, send: boolean) {
  if (!spec) return;
  if (send) spec.setMode(m);
  else { spec.mode = m; const bw = MODE_BANDWIDTHS[m]; spec.bandwidthLow = bw[0]; spec.bandwidthHigh = bw[1]; }
  // ★ The card's own readout (#mMode) is the one on screen; the bar's #modeLbl and its row of
  //   buttons went with the bar. See the note above buildControls.
  if (m !== 'wfm') {
    $('stereo').classList.remove('on');
    rdsName = ''; rdsText = ''; rdsIso = ''; rdsLogoUrl = ''; logoQuery = ''; logoDnsKey = ''; rdsLogoPi = -1;
  rdsLogoProvisional = false;
  logoFromIdentity = false;
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
/* ★★★ AND A CEILING SET BY THE RECEIVER, NOT BY US. Every sweep tick is a tune command on the
 *     spectrum socket and the server answers each one. At the 22/s ceiling that is a step every
 *     45 ms — so against a receiver answering in 1159 ms (a phone under a chroot, over a tunnel,
 *     2026-08-26) roughly twenty-five commands are in flight before the first is acknowledged.
 *     The dial then runs away from the radio and the whole receiver feels broken, while a SINGLE
 *     tap on the same server feels fine, because one round trip nobody notices. Stuart: "clicking
 *     to tune or tapping the buttons hides the lag lots."
 * ★★★ PACE, DO NOT SKIP. Waiting longer between steps keeps every step — the sweep simply runs at
 *     the speed the receiver can answer. Dropping intermediate steps instead would be the
 *     debouncing the note above rightly forbids, and would break what a sweep IS: a sweep that
 *     skips is not a faster sweep, it is a broken one.
 * ★★ ONE ROUND TRIP IN FLIGHT is the target, so the gap is the measured RTT. Capped, because a
 *    receiver having a genuinely terrible moment must still sweep at a usable rate rather than
 *    appear frozen — 400 ms is 2.5 steps/s, slow but plainly alive.
 * ★ TAPS ARE UNTOUCHED. This governs the auto-repeat tick only; ten fast taps are still ten
 *   steps, which is the structural guarantee the note above insists on. */
const SWEEP_MAX_GAP_MS = 400;   // never crawl slower than this, however bad the link
const SWEEP_RTT_FLOOR_MS = 60;  // below this the link is not the constraint — ignore it

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
        const ideal = 1000 / (SWEEP_LO + (SWEEP_HI - SWEEP_LO) * t);
        // ★ Read live: a link that degrades mid-sweep slows the sweep with it, and one that
        //   recovers speeds back up. See SWEEP_MAX_GAP_MS above for why this is paced, not skipped.
        const paced = rtt > SWEEP_RTT_FLOOR_MS ? Math.min(rtt, SWEEP_MAX_GAP_MS) : 0;
        tickT = window.setTimeout(tick, Math.max(ideal, paced));
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

/** The operator's locked window, or null when the dongle is free-running.
 *  ★ This is NOT `tuneRanges()`. Those are the RADIO's ranges — what the hardware can reach. This
 *    is what the OWNER has pinned it to, which is a much smaller box and the only one a listener
 *    on a shared receiver may move inside. */
function lockedWindow(): [number, number] | null {
  if (!(hwLockedCentre > 0)) return null;
  const fs = spec?.captureBandwidth();
  if (!fs) return null;
  return [hwLockedCentre - fs / 2, hwLockedCentre + fs / 2];
}

/** ★★ WHAT THE OWNER PERMITS, as opposed to what the hardware can reach. Absent unless they have
 *  set a limit, in which case it is always a subset of `ranges`. Kept separate so a listener who
 *  hits a wall can be told WHICH wall: "the operator does not allow this" and "this radio cannot
 *  hear it" call for completely different reactions, and telling somebody their radio is broken
 *  when it is policy is the worse mistake of the two. */
function allowedRanges(): Array<[number, number]> | null {
  const a = (radioCaps as any)?.allowed;
  return Array.isArray(a) && a.length ? a : null;
}

/** True when the hardware could reach this, so any refusal is the OPERATOR's doing. */
function hardwareCanReach(hz: number): boolean {
  const r = radioCaps?.ranges;
  if (!Array.isArray(r) || !r.length) return true;
  return r.some(([lo, hi]: [number, number]) => hz >= lo && hz <= hi);
}

function clampTune(hz: number): number {
  const want = Math.round(hz);
  // ★★★ THE LOCKED WINDOW WINS, AND IS CHECKED FIRST. The server clamps a request outside it
  //     ("retune … is outside the LOCKED window — clamped") but said nothing back, so the client
  //     went on displaying the frequency it had ASKED for: typing 99700 on a receiver locked to
  //     2.5-10.5 MHz left the readout sitting at 99.700 MHz while the radio was somewhere else
  //     entirely (Stuart, 2026-08-05). A readout that disagrees with the radio is worse than a
  //     refusal — it is the one thing on screen the user has no way to check.
  //     ★ Clamped rather than refused, matching the server: asking for 14 MHz on a 2.5-10.5 window
  //       gets you the edge, not silence, and the wall on the waterfall shows why.
  const win = lockedWindow();
  if (win) {
    gapNudgeDir = 0;
    return Math.max(win[0], Math.min(win[1], want));
  }
  // ★ The owner's limit wins where it exists; it is always inside the hardware's coverage, so
  //   using it alone cannot offer a frequency the radio cannot hear.
  const ranges = allowedRanges() ?? tuneRanges();
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
  const why = hardwareCanReach(want)
    ? 'the server operator does not allow tuning here'
    : 'this radio cannot receive here';
  showTuneGapMsg(Number.isFinite(other)
    ? `${(edge / 1e6).toFixed(3)} MHz — ${why}. Tune ${dir > 0 ? 'up' : 'down'} again to jump to ${(other / 1e6).toFixed(3)} MHz`
    : `${(edge / 1e6).toFixed(3)} MHz — ${why}`);
  return edge;
}

/** ★ Black out the part of the window the radio cannot tune, and say what it is.
 *  Only ever ONE region: the visible span is far narrower than any real gap, so a window can
 *  overlap at most one edge. Handling several would be code for a case that cannot occur. */
function updateRangeGap(centerHz: number, bwHz: number) {
  const el = document.getElementById('rangeGap');
  const note = document.getElementById('rangeGapNote');
  // ★ The same set the clamp uses, or the shading marks a wall the listener will not meet.
  const ranges = allowedRanges() ?? tuneRanges();
  if (!el || !note || !ranges || bwHz <= 0) { el?.classList.remove('show'); return; }

  const lo = centerHz - bwHz / 2, hi = centerHz + bwHz / 2;
  // The window's own edges, and the range that contains the middle of it.
  const inRange = ranges.find(([a, b]: [number, number]) => centerHz >= a && centerHz <= b);
  if (!inRange) { el.classList.remove('show'); return; }
  const [rLo, rHi] = inRange;

  let x0 = 0, x1 = 0, msg = '';
  if (hi > rHi) {                       // dead space on the RIGHT
    x0 = (rHi - lo) / bwHz; x1 = 1;
    const next = ranges.filter(([a]) => a > rHi).sort((p, q) => p[0] - q[0])[0];
    // ★ Say WHY, not just where. A wall the operator put up and a wall the hardware imposes look
    //   identical on a waterfall, and a listener told "this radio's range" when the truth is
    //   "the operator blocked it" goes away believing the receiver is faulty.
    const whyHi = hardwareCanReach(rHi + 1) ? 'the server operator allows no further'
                                            : 'this radio receives no higher';
    msg = next
      ? `${(rHi / 1e6).toFixed(3)} MHz — ${whyHi}.\nTune up again to jump to ${(next[0] / 1e6).toFixed(3)} MHz.`
      : `${(rHi / 1e6).toFixed(3)} MHz — ${whyHi}.`;
  } else if (lo < rLo) {                // dead space on the LEFT
    x0 = 0; x1 = (rLo - lo) / bwHz;
    const prev = ranges.filter(([, b]) => b < rLo).sort((p, q) => q[1] - p[1])[0];
    const whyLo = hardwareCanReach(rLo - 1) ? 'the server operator allows no lower'
                                           : 'this radio receives no lower';
    msg = prev
      ? `${(rLo / 1e6).toFixed(3)} MHz — ${whyLo}.\nTune down again to jump to ${(prev[1] / 1e6).toFixed(3)} MHz.`
      : `${(rLo / 1e6).toFixed(3)} MHz — ${whyLo}.`;
  } else { el.classList.remove('show'); return; }

  x0 = Math.max(0, Math.min(1, x0)); x1 = Math.max(0, Math.min(1, x1));
  if (x1 - x0 <= 0.005) { el.classList.remove('show'); return; }   // a sliver is just noise
  el.style.left  = `${x0 * 100}%`;
  el.style.width = `${(x1 - x0) * 100}%`;
  note.textContent = msg;
  el.classList.add('show');
  el.hidden = false;

  // ★★★ THE NOTE LIVES INSIDE THE DEAD SPACE, SO A NARROW GAP DESTROYS IT. Tune to the very edge
  //     of a band and the black region is a few percent of the width — the text then wraps to one
  //     word per line and runs off the side of the screen, which is how "the server operator
  //     allows no further" rendered as a clipped vertical sliver of syllables (Stuart, 2026-08-22,
  //     landscape iPhone).
  // ★★ Below the threshold the SHADING stays — it is the honest picture of what cannot be tuned —
  //    and only the caption goes. The message is not lost: showTuneGapMsg() puts the same words in
  //    the centred toast, which is sized for reading rather than for fitting in a gap.
  // ★ Measured in pixels, not percent: the same 6% is comfortable on a desktop and unusable on a
  //   phone, and it is the pixels the text has to fit into.
  const NOTE_MIN_PX = 170;
  note.hidden = el.getBoundingClientRect().width < NOTE_MIN_PX;
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
  /* ★★ THE CARD'S INPUT FOLLOWS THE RADIO TOO. It was filled once when the panel opened and never
     again, so tuning from the search list left the box showing where you USED to be — 96.600 while
     the receiver sat on 93.000. Harmless until the list started surviving the tune, which is
     exactly when you stay on this card and work down a list of frequencies: the one field naming
     a frequency then disagrees with the radio for the whole hunt.
     ★ Never while it has focus — that is somebody typing, and overwriting it mid-entry is worse
       than the staleness this fixes. */
  const fi = document.getElementById('freqInput') as HTMLInputElement | null;
  if (fi && document.activeElement !== fi) {
    fi.value = (hz / UNIT_DIV[freqUnit]).toFixed(UNIT_DP[freqUnit]);
  }
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
    const asked = v * UNIT_DIV[freqUnit];
    const got = clampTune(asked);
    // ★★ SAY SO WHEN IT IS OUT OF RANGE, and do NOT close the panel. A silent clamp is barely
    //    better than the wrong readout it replaces: the user typed a number, something else
    //    happened, and nothing on screen connects the two. Tell them what this receiver covers
    //    and leave the box open so they can correct it.
    const win = lockedWindow();
    if (win && Math.abs(got - asked) > 1) {
      $('freqMsg').textContent =
        `This receiver covers ${(win[0] / 1e6).toFixed(3)}–${(win[1] / 1e6).toFixed(3)} MHz`;
      return;
    }
    spec!.tune(got, undefined, { recenter: true, retarget: true });
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
  const base = `${httpBase(currentHost)}/`;
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


// ── HackRF One (EXPERIMENTAL) ────────────────────────────────────────────────
// ★★★ THREE STAGES AND NO AGC. libhackrf exposes no automatic mode at all, so there is no AUTO
//     button here and no VibeAGC either — see hackrf_source.h for why the loop stays away from a
//     radio nobody here can hear.
// ★★★ THE AMP AND THE BIAS-T ARE OWNER-ONLY, AND THAT MIRRORS THE SERVER (`adminGate` in the
//     hackrf_control handler). If these two rules drift apart the panel lies: it offers something
//     the server will silently refuse. The gain stages themselves follow `sharedGate`, the same
//     rule the RSP's gains follow — one listener moving them moves them for everyone.
const HRF_PREFS = { hrfLna: 'hrf_lna', hrfVga: 'hrf_vga' } as const;

function hrfSend(msg: Record<string, unknown>) { spec?.send({ type: 'hackrf_control', ...msg }); }

/** ★★★ THE STAGES ONLY. The amp and the bias-T are NEVER pushed from a saved preference — not on
 *  reconnect, not on a server restart, not on a fresh page.
 *  Stuart: "the hackrf MUST DEFAULT TO 0 GAIN AND PREAMP, those things have a bad habit of
 *  blowing up their preamps." A restored preference is exactly how a switch that was safe on one
 *  aerial gets re-applied on a different one, and on this radio the amp is the U14 MGA-81563 —
 *  the part that most commonly dies, unprotected, with -5 dBm between it and a repair. It is a
 *  deliberate act every time, or it is not switched at all.
 *  ★ The gains are pushed, because the server rebuilds its state on a restart and would otherwise
 *    open at zero while the panel still showed what the owner chose — the same contract
 *    pushAllRspSettings has. Zero is the safe end, so re-asserting them cannot surprise anyone. */
function pushAllHrfSettings() {
  if (radioCaps?.driver !== 'hackrf') return;
  hrfSend({
    lna: Number($<HTMLInputElement>('hrfLna').value),
    vga: Number($<HTMLInputElement>('hrfVga').value),
  });
}

/** ★★★ THE OWNER'S GAIN LIMIT ON A RADIO THAT NEEDS IT MOST. Stuart: "the hack RF needs gain
 *  locks more than the others it seems." An RSP has its own AGC to back off with and an HF+ is
 *  effectively 18-bit; a HackRF is 8 bits with no automatic gain at all, so nothing in the radio
 *  catches an overload.
 *
 *  ★★★ THE CAP IS ON THE TOTAL, so the two sliders share one budget: each one's ceiling is the cap
 *  MINUS whatever the other is using. Both stages sit after the mixer (the only pre-mixer stage is
 *  the amp, which is owner-only anyway), so LNA + VGA together is what drives the converter —
 *  capping them separately would permit twice the limit the owner wrote.
 *
 *  ★★ THE SERVER ENFORCES THIS TOO, in the hackrf_control handler, and the two must agree: if they
 *  drift the panel either refuses gain the server would allow or offers gain it will silently
 *  clamp. Both read as a broken control. This is the same contract rspRestricted() has. */
function applyHrfGainCap() {
  if (radioCaps?.driver !== 'hackrf') return;
  const lna = document.getElementById('hrfLna') as HTMLInputElement | null;
  const vga = document.getElementById('hrfVga') as HTMLInputElement | null;
  if (!lna || !vga) return;
  const capDb = hwGainCap >= 0 ? Math.floor(hwGainCap / 10) : -1;
  const HW_LNA_MAX = 40, HW_VGA_MAX = 62;
  if (capDb < 0) {
    lna.max = String(HW_LNA_MAX);
    vga.max = String(HW_VGA_MAX);
    lna.title = ''; vga.title = '';
    return;
  }
  // ★ Clamp the VALUES first, then derive each ceiling from the other — otherwise a pair that is
  //   already over the limit (a saved setting, or a retune into a tighter band) would each be
  //   measured against the other's illegal value and neither would come down.
  let l = Number(lna.value), g = Number(vga.value);
  if (l + g > capDb) {
    // ★ VGA first: it is the cheaper decibel to lose. The LNA is nearer the front end, so gain
    //   taken there costs the least noise figure — the same order the server uses on a retune.
    g = Math.max(0, capDb - l);
    if (l + g > capDb) l = Math.max(0, capDb - g);
    if (String(l) !== lna.value) { lna.value = String(l); hrfSend({ lna: l }); savePref('hrf_lna', l); }
    if (String(g) !== vga.value) { vga.value = String(g); hrfSend({ vga: g }); savePref('hrf_vga', g); }
    renderHrfVals();
  }
  lna.max = String(Math.min(HW_LNA_MAX, Math.max(0, capDb - g)));
  vga.max = String(Math.min(HW_VGA_MAX, Math.max(0, capDb - l)));
  // ★ Say WHY it stops there — a slider that simply will not move further is indistinguishable
  //   from a broken one. Same rule as the dongle's cap note.
  const why = `The owner has limited this receiver to ${capDb} dB of total gain on this band `
            + `(LNA + VGA). Lower one to raise the other.`;
  lna.title = why; vga.title = why;
}

function renderHrfVals() {
  const lna = Number($<HTMLInputElement>('hrfLna').value);
  const vga = Number($<HTMLInputElement>('hrfVga').value);
  $('hrfLnaVal').textContent = `${lna} dB${lna === 0 ? ' · none' : ''}`;
  $('hrfVgaVal').textContent = `${vga} dB${vga === 0 ? ' · none' : ''}`;
}

/** Gain is shared hardware, so it follows the SAME rule as the RSP's: free on a personal
 *  receiver, admin-only on one with a locked centre. Disabled rather than hidden — they are
 *  still the right controls, just not yours to set at that moment. */
function applyHrfLock() {
  const restricted = rspRestricted();
  for (const id of ['hrfLna', 'hrfVga']) {
    const el = $<HTMLInputElement>(id);
    el.disabled = restricted;
    const row = el.closest('.mrow') as HTMLElement | null;
    if (row) row.style.opacity = restricted ? '0.45' : '1';
  }
}

function applyHrfCaps(caps: import('./spectrum').RadioCaps | null) {
  if (caps?.driver !== 'hackrf') return;
  /* ★★★ THE RADIO'S OWN STATE WINS OVER THE PANEL'S, for the two switches. The server opens
   *     every HackRF with the amp and the bias-T OFF and reports what they actually are, so the
   *     buttons are set from the CAPS, never from a preference. A button that said ON because a
   *     previous session left the class there — on a radio that came up off — is the "you cannot
   *     tell by looking" failure the HF+ preamp had, and here it points at the dangerous end. */
  setHrfToggle('hrfAmp',   !!caps.amp);
  setHrfToggle('hrfBiasT', !!caps.biast);
  const lna = $<HTMLInputElement>('hrfLna'), vga = $<HTMLInputElement>('hrfVga');
  if (typeof caps.lna === 'number') lna.value = String(caps.lna);
  if (typeof caps.vga === 'number') vga.value = String(caps.vga);
  renderHrfVals();
  applyHrfLock();
  applyHrfGainCap();   // ★ before the push, so a saved pair above the ceiling is never sent
  if (!rspRestricted()) pushAllHrfSettings();
}

function setHrfToggle(id: string, on: boolean) {
  const el = $(id);
  el.classList.toggle('on', on);
  el.textContent = on ? 'ON' : 'OFF';   // ★ label AND class, or the button lies
}

function initHrfControls() {
  const p = prefs();
  for (const [id, key] of Object.entries(HRF_PREFS)) {
    const v = p[key];
    if (typeof v === 'number') $<HTMLInputElement>(id).value = String(v);
  }
  // ★ No preference is read for the amp or the bias-T, and none is written. See pushAllHrfSettings.
  $('hrfAmp').onclick = () => {
    const on = !$('hrfAmp').classList.contains('on');
    // ★★ ASK BEFORE THE DANGEROUS DIRECTION ONLY. Turning it OFF is always safe and never
    //    interrupted; turning it ON is the click that can cost somebody a radio.
    if (on && !confirm('Switch the HackRF RF amp ON?\n\n+14 dB in front of everything, on a radio '
                     + 'whose front end is unprotected. Maximum safe input is -5 dBm — a strong '
                     + 'signal or a static discharge can destroy the amplifier.\n\nOnly do this if '
                     + 'you know what is on the aerial.')) return;
    setHrfToggle('hrfAmp', on);
    hrfSend({ amp: on ? 1 : 0 });
  };
  $('hrfBiasT').onclick = () => {
    const on = !$('hrfBiasT').classList.contains('on');
    setHrfToggle('hrfBiasT', on);
    hrfSend({ biast: on ? 1 : 0 });
  };
  $<HTMLInputElement>('hrfLna').oninput = () => {
    renderHrfVals();
    const v = Number($<HTMLInputElement>('hrfLna').value);
    hrfSend({ lna: v }); savePref('hrf_lna', v);
    applyHrfGainCap();   // ★ the OTHER slider's ceiling just moved — they share one budget
  };
  $<HTMLInputElement>('hrfVga').oninput = () => {
    renderHrfVals();
    const v = Number($<HTMLInputElement>('hrfVga').value);
    hrfSend({ vga: v }); savePref('hrf_vga', v);
    applyHrfGainCap();
  };
}


function applyRadioCaps(caps: import('./spectrum').RadioCaps | null) {
  radioCaps = caps;
  // ★★★ A NEW RADIO IS A NEW CONNECTION, so the once-per-connection notices are armed again.
  //     The comment on gainMinShown said this already happened; it did not — the flag was set
  //     once and never cleared, so the notice was really once per PAGE LOAD and a listener who
  //     switched receivers was told nothing about the second one. ("WRITTEN AND NEVER READ", the
  //     other way round: a reset that was described but never implemented.)
  gainMinShown = false;
  rtlAutoExplained = false;
  maybeExplainRtlAutomation();
  const isRsp = caps?.driver === 'sdrplay';
  const isAhf = caps?.driver === 'airspyhf';
  const isHrf = caps?.driver === 'hackrf';
  $<HTMLElement>('rspCtls').hidden = !isRsp;
  $<HTMLElement>('ahfCtls').hidden = !isAhf;
  $<HTMLElement>('hrfCtls').hidden = !isHrf;
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
    el.hidden = isRsp || isAhf || isHrf;
  $('radioName').textContent = caps?.model
    ? (isRsp ? `SDRplay ${caps.model}` : caps.model)
    : (caps?.driver === 'rtl' ? 'RTL-SDR' : '—');
  // The dongle's single gain slider is meaningless on an RSP, and on an HF+ there is no
  // variable gain stage at all — hide it rather than leave a control whose label lies.
  // ★ A HackRF hides it too, and for the opposite reason: it has THREE gain stages and no single
  //   "gain" at all. One slider standing for three is a control whose label lies just as surely
  //   as one standing for none — the panel above draws the stages the radio actually has.
  const gainRow = $('gain').closest('.mrow') as HTMLElement | null;
  if (gainRow) gainRow.hidden = isRsp || isAhf || isHrf;
  const autoRow = $('gainAuto').closest('.mrow') as HTMLElement | null;
  if (autoRow) autoRow.hidden = isRsp || isAhf || isHrf;
  // ★ THE PROTECTED CONTROLS ARE BUILT HERE, so the lock has to be re-applied here. The
  // Airspy and RSP panels only exist once the radio has announced itself, which happens AFTER
  // the admin state is first resolved — so applying it only at connect left the per-radio
  // controls (calibration especially) enabled on a protected server (Stuart, 2026-07-27).
  refreshAdminRow();
  // ★ The rows above have just been rebuilt from the radio's capabilities, which un-hides anything
  //   the LOCK had hidden — so the lock is re-applied here, on every path out of this function.
  if (isAhf) { applyAhfCaps(caps); refreshAdminRow(); applyGainLocked(); return; }
  if (isHrf) { applyHrfCaps(caps); refreshAdminRow(); applyGainLocked(); return; }
  if (!isRsp) { applyGainLocked(); return; }

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
  applyRspLock();   // the panel has only just been built; nothing has applied the lock to it yet
  // ★ The radio has just told us what it is — which is also the moment to tell it what the
  // user last chose. Covers a server restart, a reconnect, and a fresh page load alike.
  // ★ ...unless we are not allowed to: on a locked receiver a listener pushing their saved
  //   gains would be refused by the server anyway, and on a SHARED one it would be rude —
  //   every other listener's front end moved to suit whoever reconnected last.
  if (!rspRestricted()) pushAllRspSettings();
  applyGainLocked();
  // ★ AFTER the gr.min above, which has just been reset to the radio's own floor — the OWNER's
  //   ceiling is a second, tighter floor and must be re-applied or the panel rebuild loses it.
  applyIfGainCap();
}

function rspSend(msg: Record<string, unknown>) {
  spec?.send({ type: 'rsp_control', ...msg });
}

/** ★★★ WHO MAY TOUCH THE FRONT END — and this MIRRORS THE SERVER'S RULE EXACTLY (`sharedGate`
 *  in local_sdr_shim.cpp). Gain is SHARED hardware: one listener moving the LNA moves it for
 *  everyone, so on a receiver with a LOCKED CENTRE it lives behind the admin password. On a
 *  personal receiver it is the owner's own radio and stays free.
 *  ★ If these two rules ever drift apart the controls lie: either they refuse something the
 *    server would have allowed, or they offer something it will silently reject. */
function rspRestricted(): boolean {
  // ★★ TWO INDEPENDENT REASONS, ONE ANSWER. The first is WHO you are (a shared front end behind
  //    the admin password); the second is that the owner has fixed the gain on this band, where
  //    there is nothing to unlock — the password would not help, because the server refuses the
  //    message from anybody. Both end in the same place: hide the controls rather than offer a
  //    control whose every use is a no-op.
  return (hwLockedCentre > 0 && srvAdminProtected && !adminUnlocked) || hwGainLocked;
}

/** ★★★ THE IF SLIDER'S READ-ONLY STATE, IN ONE PLACE, CALLED FROM EVERYWHERE THAT CHANGES IT.
 *
 *  It is read-only when the AGC owns the register (the hardware refuses a manual write then —
 *  see setIfGainReduction) or when this listener may not touch the front end at all.
 *
 *  ★★ IT USED TO BE APPLIED ONLY WHEN AN `rspstat` ARRIVED, and that was the bug Stuart hit:
 *  on a --zoom-spectrum server no stat was ever sent, so the class that had been added by the
 *  one stat received before zooming was never removed — an admin could turn the AGC off and the
 *  slider STILL could not be dragged (2026-08-03). UI state must never depend on a telemetry
 *  message arriving; telemetry moves the thumb, it does not decide who owns it. */
function applyRspLock() {
  const restricted = rspRestricted();
  const agcOn = $('rspIfAgc').classList.contains('on');
  const gr = $<HTMLInputElement>('rspIfGr');
  gr.classList.toggle('agc', agcOn || restricted);

  // ★ HIDDEN, not greyed, for a listener who cannot use them — a disabled control still reads
  //   as an offer, and there is nothing here for them to unlock without the password.
  //   ★ THE TWO EXCEPTIONS ARE DELIBERATE AND ARE THE WHOLE POINT OF THE PANEL FOR A LISTENER:
  //     SYSTEM GAIN (what the radio actually has) stays visible, and the IF slider stays visible
  //     but read-only BECAUSE IT MOVES — watching the AGC work is how you can tell it is working
  //     at all. A hidden slider and a frozen one look identical: broken.
  const rowOf = (id: string) => document.getElementById(id)?.closest('.mrow') as HTMLElement | null;
  for (const id of ['rspLna', 'rspIfAgc']) {
    const r = rowOf(id); if (r) r.hidden = restricted;
  }
  for (const id of ['rowAgcSet', 'rowRspNotch']) {
    const r = document.getElementById(id); if (r) r.hidden = restricted;
  }
  const bias = document.getElementById('rspBiasT')?.closest('.mrow') as HTMLElement | null;
  if (bias) bias.hidden = restricted;
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
      // ★ AND UNLOCK THE SLIDER RIGHT NOW, rather than waiting for an rspstat to say so. That
      //   wait was the bug: on a server whose stats never arrive, the AGC could be turned off
      //   and the slider stayed read-only for ever (Stuart, 2026-08-03).
      if (key === 'ifagc') applyRspLock();
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

// ★ Draw as soon as the page exists, then refresh while the visitor is still on the splash — a
//   new row lands every second for the first five minutes, so a server that has just been set up
//   visibly fills in while somebody reads the page.
drawSplashSpectrogram();
showSplashListeners();
showSplashConditions();
refreshBandConditions();
// ★ Slow on purpose: this is a one-minute rolling median on the server, so polling faster would
//   re-fetch a figure that has not moved. It also keeps working after the splash is dismissed,
//   which is when the VTS needs it.
setInterval(refreshBandConditions, 60000);
setInterval(() => {
  const sp = document.getElementById('splash');
  if (sp && !sp.classList.contains('hidden')) drawSplashSpectrogram();
}, 15000);

/**
 * ★★★ THE RADIO LIST UPDATES ITSELF. It was drawn once, so "FREE" or "IN USE" was only ever true
 *     at the moment the page loaded — someone watching the landing page waiting for a radio to come
 *     free had to keep reloading to find out, and the `FREE IN 2:34` countdowns sat frozen at
 *     whatever they said on arrival. A frozen countdown is worse than none, because people watch it
 *     (Stuart, 2026-08-17).
 *
 * ★★ NOT WHILE THE TAB IS HIDDEN. A landing page left open in a background tab all afternoon would
 *    otherwise poll a Pi that is also running the DSP, for nobody's benefit — the same reasoning as
 *    the idle saver and the frame-rate ceilings. `visibilityState` is checked every tick rather
 *    than subscribed to, so returning to the tab picks straight back up.
 *
 * ★★ UPDATED IN PLACE, NOT REDRAWN. Replacing innerHTML on a timer can swallow a click that lands
 *    in the same instant, and this list IS the thing people click. Only the state text changes on a
 *    normal tick; a FULL redraw happens solely when reachability flips, which is the moment the
 *    card must become clickable (or stop being) and its handlers have to be rebuilt anyway.
 *
 * ★ 10 s: this changes when somebody arrives or leaves, not sub-second, and three radios is three
 *   requests a tick. Under that starts to be real traffic on a Pi for no more information.
 */
async function refreshSplashRadios(): Promise<void> {
  const sp = document.getElementById('splash');
  if (!sp || sp.classList.contains('hidden')) return;
  if (document.visibilityState !== 'visible') return;
  const host = document.getElementById('splashRadios');
  if (!host) return;
  const cards = Array.from(host.querySelectorAll('[data-serial]')) as HTMLElement[];
  if (!cards.length) return;

  let dir: any;
  try {
    const r = await fetch(P('/vibeserver/radios'), { cache: 'no-store' });
    if (!r.ok) return;
    dir = await r.json();
  } catch { return; }
  const radios: any[] = Array.isArray(dir?.radios) ? dir.radios : [];
  // ★ The directory itself changed (a radio added or removed) — the rows are wrong, not just their
  //   text. Hand it back to the full renderer.
  if (radios.length !== cards.length) { void showSplashRadios(); return; }

  const live = await Promise.all(radios.map(async (r) => {
    try {
      const rid = encodeURIComponent((r as any).id || r.serial);
      const resp = await fetch(`${location.origin}/r/${rid}/vibeserver.json`, { cache: 'no-store' });
      if (!resp.ok) { latestRadioStatus.delete(r.serial); return null; }
      const j = await resp.json();
      // ★ The click handler reads THIS, not the object the card was built from — see the map.
      latestRadioStatus.set(r.serial, j);
      return j;
    } catch { return null; }
  }));

  let needsRedraw = false;
  radios.forEach((r, i) => {
    const card = cards.find((c) => c.dataset.serial === r.serial);
    if (!card) { needsRedraw = true; return; }
    const { state, blocked } = radioCardState(r, live[i]);
    const span = card.querySelector('.rcState');
    if (span && span.textContent !== state) span.textContent = state;
    // A blocked card is a <div>, a reachable one an <a> — the tag itself has to change.
    const wasBlocked = card.tagName.toLowerCase() !== 'a';
    if (wasBlocked !== blocked) needsRedraw = true;
  });
  if (needsRedraw) void showSplashRadios();
}
setInterval(() => { void refreshSplashRadios(); }, 10000);
addEventListener('resize', () => {
  const sp = document.getElementById('splash');
  if (sp && !sp.classList.contains('hidden')) drawSplashSpectrogram();
});
