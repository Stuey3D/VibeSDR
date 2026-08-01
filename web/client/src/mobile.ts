// ── Mobile control card ──────────────────────────────────────────────────────
// An HTML port of the phone app's control layout (src/components/ControlsBar.tsx) for
// narrow windows. Shown by CSS at ≤1280px; this module only wires behaviour.
//
// ★★ WHAT CAME FROM WHERE. The CONTROL SET and their ORDER come from ControlsBar.tsx as
// it is today — not from screenshots/ and not from the PocketUberSDR skin, both of which
// are older than the app and show a different set. What DID carry over from the skin is
// the FEEL of the drums: weighted, draggable, and they coast. That is Stuart's own design
// (PocketUberSDR, MIT) and the thing users recognise; the skin itself was UberSDR-only, so
// none of its UberSDR-specific controls (chat, share, VTS) are ported.
//
// ★ The drums are the protected part of the design — see the app's README: "Two large
// weighted drums with real inertia. Spin them, flick them, let them coast. It feels like
// tuning a radio, because that's what it's modelled on."

export type MobileDeps = {
  /** Tune by a signed number of STEPS (not Hz) — the caller owns step size and clamping. */
  nudgeSteps: (steps: number) => void;
  /** Multiply the view span. >1 zooms out, <1 zooms in, matching spec.zoomBy. */
  zoomBy: (factor: number) => void;
  /** Current dial frequency in Hz, or null before the first tune. */
  freqHz: () => number | null;
  mode: () => string;
  /** Formatted step label for the step button, e.g. "1k". */
  stepLabel: () => string;
  /** Open the step ladder as a popup, anchored to the element the user tapped. */
  openStepMenu: (anchor: HTMLElement) => void;
  /** 0..1 level for the pill's gradient, plus the three readings the meter can show. */
  signal: () => { level: number; snr: number; dbfs: number; sUnit: string };
  openFreqEntry: () => void;
  /** The demodulators this client offers, and a setter. Drives the mode picker. */
  modes: () => string[];
  setMode: (m: string) => void;
  openMenu: () => void;
  openAudio: () => void;
  openDecoders: () => void;
};

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

// ★★ PIXELS PER DETENT. Low enough that a short drag does something, high enough that a
// clumsy thumb does not fling you across the band. 14 px matches the app's feel on a
// 3x-density phone; it is deliberately NOT scaled by DPR — a finger is the same size on
// every screen, so the useful unit is CSS pixels, not device pixels.
const PX_PER_DETENT = 14;
// ★ Below this the coast is imperceptible and just burns frames — stop rather than crawl.
const MIN_COAST_VELOCITY = 0.02;
// ★ Per-frame velocity decay. 0.94 gives roughly a second of coast from a hard flick,
// which reads as a weighted dial; 0.98 felt like ice and overshot constantly.
const FRICTION = 0.94;

/**
 * Make one drum face draggable. `onDetent` fires once per PX_PER_DETENT of travel, with
 * the direction, during BOTH the drag and the coast that follows it.
 */
function attachDrum(face: HTMLElement, onDetent: (dir: number) => void) {
  let dragging = false;
  let lastX = 0;
  let accum = 0;          // sub-detent travel carried between events
  let offset = 0;         // background scroll, purely cosmetic
  let velocity = 0;       // px per frame, for the coast
  let raf = 0;

  const paint = () => { face.style.backgroundPosition = `${offset}px 0`; };

  const travel = (dx: number) => {
    offset += dx;
    accum += dx;
    // ★ A LOOP, NOT AN `if`. A fast flick can cross several detents inside one pointermove
    // (or one coast frame); handling only the first would silently swallow the rest and
    // make the drum feel like it was slipping.
    while (Math.abs(accum) >= PX_PER_DETENT) {
      const dir = accum > 0 ? 1 : -1;
      accum -= dir * PX_PER_DETENT;
      onDetent(dir);
    }
    paint();
  };

  const stopCoast = () => {
    if (raf) { cancelAnimationFrame(raf); raf = 0; }
    velocity = 0;
    face.classList.remove('coasting');
  };

  const coast = () => {
    velocity *= FRICTION;
    if (Math.abs(velocity) < MIN_COAST_VELOCITY) { stopCoast(); return; }
    travel(velocity);
    raf = requestAnimationFrame(coast);
  };

  face.addEventListener('pointerdown', (e) => {
    // ★ TOUCHING A COASTING DRUM STOPS IT. That is how a real dial behaves and it is the
    // only way to catch a flick that went too far without fighting it.
    stopCoast();
    dragging = true;
    lastX = e.clientX;
    accum = 0;
    face.setPointerCapture(e.pointerId);
    e.preventDefault();
  });

  face.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const dx = e.clientX - lastX;
    lastX = e.clientX;
    // ★ Blend rather than replace, so one jittery sample cannot define the throw.
    velocity = velocity * 0.6 + dx * 0.4;
    travel(dx);
  });

  const release = (e: PointerEvent) => {
    if (!dragging) return;
    dragging = false;
    try { face.releasePointerCapture(e.pointerId); } catch { /* already gone */ }
    if (Math.abs(velocity) > 0.5) {
      face.classList.add('coasting');
      raf = requestAnimationFrame(coast);
    } else {
      velocity = 0;
    }
  };
  face.addEventListener('pointerup', release);
  face.addEventListener('pointercancel', release);
}

/** Press-and-hold repeat for the drums' − / + ends, for people who would rather tap. */
function attachRepeat(btn: HTMLElement, fire: () => void) {
  let hold = 0, rep = 0;
  const stop = () => {
    if (hold) { clearTimeout(hold); hold = 0; }
    if (rep) { clearInterval(rep); rep = 0; }
  };
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    fire();
    hold = window.setTimeout(() => { rep = window.setInterval(fire, 70); }, 380);
  });
  for (const ev of ['pointerup', 'pointercancel', 'pointerleave']) {
    btn.addEventListener(ev, stop);
  }
  window.addEventListener('pointerup', stop);
}

// ★★ THE SAME THREE READINGS THE APP OFFERS — `signalMode: 'snr' | 'smeter' | 'dbfs'` in
//    ControlsBar.tsx, formatted by its meterText(). Three names for one measurement, and which
//    one is useful depends entirely on what you are doing: SNR for judging a decode, S-units
//    for a signal report, dBFS for setting gain. Cycling beats choosing for people in settings.
type MeterMode = 'snr' | 'smeter' | 'dbfs';
const METER_MODES: MeterMode[] = ['snr', 'smeter', 'dbfs'];
const METER_PREF = 'vibe.mMeterMode';

export function initMobileControls(deps: MobileDeps) {
  const card = document.getElementById('mcard');
  if (!card) return;

  // ★ Remembered: someone who wants S-units wants them every session, and re-cycling from
  //   SNR on every page load would be a small daily annoyance.
  let meterMode: MeterMode = 'snr';
  try {
    const saved = localStorage.getItem(METER_PREF) as MeterMode | null;
    if (saved && METER_MODES.includes(saved)) meterMode = saved;
  } catch { /* private mode — the default is fine */ }

  const meterText = (s: { snr: number; dbfs: number; sUnit: string }) => {
    if (meterMode === 'smeter') return s.sUnit.trim();
    if (meterMode === 'dbfs')   return `${Math.round(s.dbfs)} dBFS`;
    return isFinite(s.snr) ? `SNR ${Math.round(s.snr)} dB` : '—';
  };

  // ── Drums ──────────────────────────────────────────────────────────────────
  attachDrum($('mVfoFace'), (dir) => deps.nudgeSteps(dir));
  // ★ A QUARTER-OCTAVE PER DETENT, matching the desktop bar's hold-sweep rate. A full
  //   factor of 2 per detent made the zoom drum unusable — one flick and the whole band
  //   was gone.
  const ZOOM_DETENT = Math.pow(2, 0.25);
  attachDrum($('mZoomFace'), (dir) => deps.zoomBy(dir > 0 ? ZOOM_DETENT : 1 / ZOOM_DETENT));

  attachRepeat($('mVfoDown'), () => deps.nudgeSteps(-1));
  attachRepeat($('mVfoUp'),   () => deps.nudgeSteps(1));
  attachRepeat($('mZoomOut'), () => deps.zoomBy(2));
  attachRepeat($('mZoomIn'),  () => deps.zoomBy(0.5));

  // ── Buttons — the app's order: step, audio, menu, [decoders] ────────────────
  // ★ A POPUP, NOT A CYCLER (Stuart). Cycling makes you tap through every step to reach the
  //   one you want and gives no sight of the ladder — and on a phone that is several taps of
  //   a control that is already small. The desktop button opens a menu; so does this one, and
  //   it is the SAME menu, anchored to whichever button was tapped.
  $('mStep').onclick = () => deps.openStepMenu($('mStep'));
  $('mAudio').onclick = () => deps.openAudio();
  $('mMenu').onclick  = () => deps.openMenu();
  $('mDec').onclick   = () => deps.openDecoders();
  // ★★ TWO SEPARATE TARGETS, exactly as the app's pill has: the FREQUENCY opens frequency
  //    entry (onFreqTap) and the MODE opens the demodulator picker (onModeTap). Each is its
  //    own boxed button and the PILL ITSELF IS NOT CLICKABLE — an earlier version put the
  //    frequency handler on the whole pill, so tapping the mode bubbled up and produced a
  //    number pad, and the gradient behind them was an invisible third button.
  $('mFreqBox').onclick = () => deps.openFreqEntry();
  $('mMode').onclick    = () => openModePicker();
  // ★ Tap the reading to change WHICH reading it is: SNR → S-units → dBFS.
  $('mSnr').onclick = () => {
    meterMode = METER_MODES[(METER_MODES.indexOf(meterMode) + 1) % METER_MODES.length];
    try { localStorage.setItem(METER_PREF, meterMode); } catch { /* non-fatal */ }
    refresh();
  };
  $('mSnr').title = 'Tap to switch: SNR / S-meter / dBFS';

  // ── Controls the desktop bar owns, mirrored where they BELONG ───────────────
  // ★★★ CLICK THE ORIGINAL, DO NOT REIMPLEMENT IT. Every one of these drives the existing
  //     bar control, so behaviour, state and any future change live in exactly one place.
  //     A second implementation of REC or LOCK would drift the first time either is touched.
  const mirror = (fromId: string, toId: string) => {
    const from = document.getElementById(fromId);
    const to = document.getElementById(toId);
    if (from && to) from.onclick = () => to.click();
  };
  mirror('mBookmarks', 'bookmarksBtn');
  mirror('mPanelRec', 'recBtn');
  mirror('mPanelRecs', 'recordingsBtn');
  mirror('mPanelMute', 'muteBtn');
  mirror('mPanelLock', 'lockBtn');
  mirror('mPanelCentre', 'centreBtn');
  mirror('mPanelFit', 'zoomReset');

  // ★★★ MOVE THE SEARCH BOX, DO NOT COPY IT. #searchWrap lives in the desktop bar; when that
  //     bar is hidden the input inside it cannot be focused or typed into, so anything
  //     pointing at it is dead. Relocating the NODE keeps every listener — they are bound to
  //     the element, not to its position in the tree — so there is still exactly one search
  //     implementation. It goes back when the bar returns, or a user who widens the window
  //     would find the bar missing its search box.
  const wrap = document.getElementById('searchWrap');
  const host = document.getElementById('mSearchHost');
  const home = wrap?.parentElement ?? null;
  const nextSibling = wrap?.nextElementSibling ?? null;
  const narrow = window.matchMedia('(max-width: 1280px)');
  const placeSearch = () => {
    if (!wrap || !host || !home) return;
    if (narrow.matches) {
      if (wrap.parentElement !== host) host.appendChild(wrap);
    } else if (wrap.parentElement !== home) {
      // Back to its exact slot, not just appended — #findRow puts BOOKMARKS before it.
      home.insertBefore(wrap, nextSibling);
    }
  };
  placeSearch();
  narrow.addEventListener('change', placeSearch);

  // ★★ THE VOLUME SLIDER IS A VALUE, NOT AN ACTION — mirroring a click would do nothing.
  //    Copy the value across and fire `input`, which is the event the audio path listens on;
  //    seed from the original so the mobile slider opens where the audio actually is rather
  //    than snapping it to a default the moment the panel is opened.
  const vol = document.getElementById('vol') as HTMLInputElement | null;
  const mVol = document.getElementById('mPanelVol') as HTMLInputElement | null;
  if (vol && mVol) {
    mVol.min = vol.min; mVol.max = vol.max; mVol.step = vol.step;
    mVol.value = vol.value;
    mVol.oninput = () => {
      vol.value = mVol.value;
      vol.dispatchEvent(new Event('input', { bubbles: true }));
    };
    // Keep in step if the desktop slider moves (a keyboard shortcut, a restored pref).
    vol.addEventListener('input', () => { mVol.value = vol.value; });
  }

  // ★★ THE VTS SITS IN THE SAME CORNER AS THIS CARD (#vts is bottom:14px), so without an
  //    offset the station strip draws straight over the controls. Publish the card's MEASURED
  //    height and let the stylesheet lift the VTS clear of it. A fixed number would be wrong
  //    at most widths: the height changes with the breakpoint, with whether the drums are
  //    stacked, and with the font-size clamp.
  const publishHeight = () => {
    document.documentElement.style.setProperty('--mcard-h', `${Math.round(card.offsetHeight)}px`);
  };
  publishHeight();
  if (typeof ResizeObserver !== 'undefined') new ResizeObserver(publishHeight).observe(card);
  else window.addEventListener('resize', publishHeight);

  // ★ A popup rather than a row of buttons: seven demodulators across a phone would leave
  //   each one below a thumb's width, and the card has no room for a second row without
  //   eating the waterfall it exists to control.
  function openModePicker() {
    document.getElementById('mModeMenu')?.remove();
    const menu = document.createElement('div');
    menu.id = 'mModeMenu';
    const cur = deps.mode().toLowerCase();
    for (const m of deps.modes()) {
      const b = document.createElement('button');
      b.textContent = m.toUpperCase();
      b.className = 'mModeOpt' + (m.toLowerCase() === cur ? ' on' : '');
      b.onclick = () => { deps.setMode(m); close(); refresh(); };
      menu.appendChild(b);
    }
    document.body.appendChild(menu);
    // Anchored to the pill, and flipped above it when there is no room below — on a
    // phone the pill sits near the bottom, so "below" is almost never where it fits.
    const r = $('mPill').getBoundingClientRect();
    const h = menu.offsetHeight;
    const top = r.top - h - 8 > 0 ? r.top - h - 8 : Math.min(r.bottom + 8, innerHeight - h - 8);
    menu.style.left = `${Math.max(8, Math.min(r.left, innerWidth - menu.offsetWidth - 8))}px`;
    menu.style.top = `${Math.max(8, top)}px`;

    const close = () => {
      menu.remove();
      document.removeEventListener('pointerdown', away, true);
      document.removeEventListener('keydown', esc, true);
    };
    const away = (ev: Event) => { if (!menu.contains(ev.target as Node)) close(); };
    const esc = (ev: KeyboardEvent) => { if (ev.key === 'Escape') { ev.stopPropagation(); close(); } };
    // Deferred, or the click that OPENED the menu immediately closes it again.
    setTimeout(() => {
      document.addEventListener('pointerdown', away, true);
      document.addEventListener('keydown', esc, true);
    }, 0);
  }

  // ── Readout ────────────────────────────────────────────────────────────────
  // ★ The pill shows MHz or kHz on the same rule the app uses: below 10 MHz a kHz
  //   reading has more useful digits, above it MHz does. Switching unit is not cosmetic —
  //   it is what keeps the significant figures on screen at every band.
  function refresh() {
    const hz = deps.freqHz();
    const fEl = $('mFreq'), uEl = $('mUnit');
    if (hz == null) {
      fEl.textContent = '—';
    } else if (hz < 10_000_000) {
      fEl.textContent = (hz / 1e3).toFixed(3);
      uEl.textContent = 'kHz';
    } else {
      fEl.textContent = (hz / 1e6).toFixed(3);
      uEl.textContent = 'MHz';
    }
    $('mMode').textContent = deps.mode().toUpperCase();
    $('mStep').textContent = deps.stepLabel();

    // ★★ MIRROR THE DESKTOP READOUTS, DO NOT RECOMPUTE THEM. Throughput, fps, rtt and link
    //    quality are all derived in updateStatus()/updateLink(); a second derivation here
    //    would disagree with the first the moment either changed, and two contradictory
    //    status readouts is worse than one.
    const st = document.getElementById('status');
    const bars = document.getElementById('linkBars');
    if (st) $('mNetTxt').textContent = st.textContent ?? '';
    if (bars) $('mBars').className = bars.className;

    const sig = deps.signal();
    // Clamp: a level outside 0..1 would paint the gradient past the pill or invert it.
    $('mSig').style.width = `${Math.max(0, Math.min(1, sig.level)) * 100}%`;
    $('mSnr').textContent = meterText(sig);
  }

  // ★ UTC first, then local — the order every band plan, schedule and logbook uses, so
  //   the reading a listener needs is the one they see first.
  function clock() {
    const d = new Date();
    const p = (n: number) => String(n).padStart(2, '0');
    const utc = `${p(d.getUTCHours())}:${p(d.getUTCMinutes())} UTC`;
    const loc = `${p(d.getHours())}:${p(d.getMinutes())}`;
    $('mClock').textContent = `${utc} · ${loc}`;
  }

  refresh();
  clock();
  // 4 Hz is enough for a frequency that changes as fast as a thumb can drag, and cheap
  // enough to leave running on a phone.
  setInterval(refresh, 250);
  setInterval(clock, 10_000);
  return { refresh };
}
