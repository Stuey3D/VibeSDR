// ── Mobile control card ──────────────────────────────────────────────────────
// An HTML port of the phone app's control layout (src/components/ControlsBar.tsx), used at
// every width — the web client's only control surface. CSS handles the arrangement (pads flank
// the pill when wide, flow under it when narrow); this module only wires behaviour.
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
  /** ★★ The RECEIVER's clock: its UTC offset (minutes) and zone abbreviation, null until the server says
   *  (an older build) — then the deck shows the browser's time as before. */
  serverClock?: () => { offsetMin: number; abbr: string } | null;
  /** Tune by a signed number of STEPS (not Hz) — the caller owns step size and clamping. */
  nudgeSteps: (steps: number) => void;
  /** ★ Tap = one step, hold = main.ts's accelerating sweep, paced on the receiver's own answer to
   *  each tune (attachHoldSweep). Owned there so the pacing has ONE implementation. */
  holdSweep: (el: HTMLElement, tap: () => void) => void;
  /** spec.zoomBy — a MAGNIFICATION factor, not a span multiplier: >1 zooms IN (narrower
   *  span, more detail), <1 zooms OUT. Reading it the other way round is what once got the
   *  − and + buttons swapped. */
  zoomBy: (factor: number) => void;
  /** Current dial frequency in Hz, or null before the first tune. */
  freqHz: () => number | null;
  /** ★★ THE UNIT THE USER CHOSE in the frequency panel, and the readout it produces. The card
   *  used to decide for itself (kHz below 10 MHz, MHz above), so picking kHz in the entry popup
   *  changed the popup and nothing else. The rule: "the unit chosen in the entry popup also drives
   *  the readout, so the two always agree — a dial reading MHz while you type kHz is how people
   *  mis-tune by a factor of a thousand." */
  /** `chan`: the airband channel's small line (spacing / true frequency / name) — absent elsewhere. */
  freqText: () => { main: string; fine: string; unit: string; chan?: string } | null;
  mode: () => string;
  /** Formatted step label for the step button, e.g. "1k". */
  stepLabel: () => string;
  /** Open the step ladder as a popup, anchored to the element the user tapped. */
  openStepMenu: (anchor: HTMLElement) => void;
  /** 0..1 level for the pill's gradient, plus the three readings the meter can show. */
  signal: () => { level: number; snr: number; dbfs: number; sUnit: string;
                  sqlNorm: number; sqlClosed: boolean };
  openFreqEntry: () => void;
  /** The demodulators this client offers, and a setter. Drives the mode picker. */
  modes: () => string[];
  setMode: (m: string) => void;
  openMenu: () => void;
  openAudio: () => void;
  openDecoders: () => void;
  /** ★ The one decoder left when the owner has blocked all the others (and the spots) — drawn
   *  here in place of DECODERS… so a single button does not hide behind a door. See main.ts. */
  soleDecoder: () => { label: string; on: boolean; press: () => void } | null;
  /** ★ False when the owner has switched off every decoder and the spot list — the DECODERS…
   *  entry is then left out rather than opening an empty panel. */
  anyDecoderLeft: () => boolean;
  /** ★★★ DAB, WHICH IS NOT AN SDRMode AND SO CANNOT COME THROUGH modes(). It replaces the whole
   *  chain rather than filtering a channel out of it, so it is offered here as its own entry —
   *  and ONLY where the server says the receiver can reach a multiplex.
   *  ★ It was once added to the retired desktop bar's mode row alone, where nobody could see it
   *    (Stuart, 2026-09-04: "cant see the dab button"). There is one picker now: this one. */
  dabCapable: () => boolean;
  dabOn: () => boolean;
  toggleDab: () => void;
  /** ★ On a shared-dial receiver only — see chat.ts. The button that opens it is hidden
   *  everywhere else, because on an ordinary receiver there is nobody to talk to. */
  openChat: () => void;
};

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

/** Press-and-hold repeat for the zoom pad's − / +. (The tune pad uses deps.holdSweep.) */
function attachRepeat(btn: HTMLElement, fire: () => void) {
  let hold = 0, rep = 0;
  const stop = () => {
    if (hold) { clearTimeout(hold); hold = 0; }
    if (rep) { clearInterval(rep); rep = 0; }
  };
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    // ★★★ CAPTURE THE POINTER. Without this the release is delivered to whatever the finger is
    //     over — and on iOS a long press raises the selection callout, which swallows it
    //     entirely: the repeat kept firing and the button stayed visually stuck down, because
    //     the press that started it never ended as far as this element was concerned. With
    //     capture, pointerup/pointercancel ALWAYS come back here, so the hold can always end.
    try { btn.setPointerCapture(e.pointerId); } catch { /* mouse on an old engine */ }
    fire();
    hold = window.setTimeout(() => { rep = window.setInterval(fire, 70); }, 380);
  });
  // ★ `lostpointercapture` matters as much as the rest: if the system takes the pointer away
  //   (a callout, a gesture, a phone call) that is the ONLY event we get, and without it the
  //   repeat runs on with nothing left to stop it.
  for (const ev of ['pointerup', 'pointercancel', 'pointerleave', 'lostpointercapture']) {
    btn.addEventListener(ev, stop);
  }
  window.addEventListener('pointerup', stop);
  // A last resort for the same class of failure: if the page is hidden or loses focus
  // mid-hold, nothing above necessarily fires.
  window.addEventListener('blur', stop);
  document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
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

  // ── Tune / zoom pads ───────────────────────────────────────────────────────
  // ★★★ BUTTONS, NOT A DRAGGABLE DRUM. On glass the drum's inertia is the whole point — it is
  //     what makes it feel like a weighted dial. On a TRACKPAD it was far too twitchy and the
  //     coast fought the user: every attempt to fine-tune ended with the momentum carrying the
  //     dial past the frequency they were settling on (Stuart, 2026-08-01). Overshooting on the
  //     last and most precise step of a task is worse than having no control at all.
  // ★ The drag implementation is not carried here as dead code — it is in git history if the
  //   card is ever driven by a real touchscreen, where `pointer: coarse` would be the honest
  //   test for turning it back on.
  // ★★ TUNING sweeps through deps.holdSweep (accelerating, and paced on the receiver's answer — a
  //    flat-rate repeat outruns a slow server). ZOOM keeps the plain repeat below, unchanged — the
  //    sweep fixes were made for, and measured on, tuning.
  deps.holdSweep($('mVfoDown'), () => deps.nudgeSteps(-1));
  deps.holdSweep($('mVfoUp'),   () => deps.nudgeSteps(1));
  // ★ IN magnifies, OUT widens. These were once the wrong way round: − zoomed in and + zoomed out.
  attachRepeat($('mZoomIn'),  () => deps.zoomBy(2));
  attachRepeat($('mZoomOut'), () => deps.zoomBy(0.5));

  // ── Buttons — the app's order: step, audio, menu, [decoders] ────────────────
  // ★ A POPUP, NOT A CYCLER (Stuart). Cycling makes you tap through every step to reach the
  //   one you want and gives no sight of the ladder — and on a phone that is several taps of
  //   a control that is already small. It opens the step ladder as a menu anchored to the button.
  $('mStep').onclick = () => deps.openStepMenu($('mStep'));
  $('mAudio').onclick = () => deps.openAudio();
  $('mMenu').onclick  = () => deps.openMenu();
  $('mChat').onclick  = () => deps.openChat();
  // ★★ TWO SEPARATE TARGETS, exactly as the app's pill has: the FREQUENCY opens frequency
  //    entry (onFreqTap) and the MODE opens the demodulator picker (onModeTap). Each is its
  //    own boxed button and the PILL ITSELF IS NOT CLICKABLE — an earlier version put the
  //    frequency handler on the whole pill, so tapping the mode bubbled up and produced a
  //    number pad, and the gradient behind them was an invisible third button.
  $('mFreqBox').onclick = () => deps.openFreqEntry();
  $('mMode').onclick    = () => openModePicker();
  // ★★★ A MENU, NOT A CYCLER — the same ruling this file already applies to the STEP button, and
  //     for the same reason, now confirmed on this control too: "I have to click multiple times
  //     and it sometimes jumps past the digits I want, I want to view the SNR but keeps jumping
  //     past it" (Stuart, 2026-08-14). A three-state cycler has no way back: overshoot the one you
  //     want and the only route to it is all the way round again, and any click that double-fires
  //     or is missed leaves you somewhere you did not choose. It also shows nothing of the ladder,
  //     so there is no way to know what is coming next.
  // ★★ AND THE TARGET WAS A BARE <span>, which is very likely the "multiple times" half: a near
  //    miss landed on the pill behind it, which has no handler by design, so the click did nothing
  //    at all. A menu makes every press either open it or dismiss it — never silently nothing.
  const openMeterMenu = (anchor: HTMLElement) => {
    document.getElementById('mMeterMenu')?.remove();
    const r = anchor.getBoundingClientRect();
    const m = document.createElement('div');
    m.id = 'mMeterMenu';
    m.style.cssText = 'position:fixed;z-index:9998;background:#0d0d0d;border:1px solid #ffa000;'
      + 'border-radius:8px;padding:4px;display:flex;flex-direction:column;gap:2px;'
      + 'font:12px/1.4 var(--mono,monospace);box-shadow:0 6px 24px rgba(0,0,0,.6)';
    const label: Record<MeterMode, string> = {
      snr: 'SNR', smeter: 'S-meter', dbfs: 'dBFS',
    };
    for (const v of METER_MODES) {
      const b = document.createElement('button');
      b.textContent = label[v];
      // ★ The current one is marked, so the menu also ANSWERS "which am I looking at?" — the
      //   question a cycler forces you to work out from the reading itself.
      b.style.cssText = 'background:none;border:0;color:' + (v === meterMode ? '#ffe566' : '#ddd')
        + ';padding:7px 14px;text-align:left;cursor:pointer;border-radius:5px;font:inherit';
      b.onmouseenter = () => { b.style.background = 'rgba(255,160,0,.18)'; };
      b.onmouseleave = () => { b.style.background = 'none'; };
      b.onclick = () => {
        meterMode = v;
        try { localStorage.setItem(METER_PREF, meterMode); } catch { /* private mode */ }
        close();
        refresh();
        paintSignal();      // the reading itself is painted here, not by refresh()
      };
      m.appendChild(b);
    }
    document.body.appendChild(m);
    // Above when there is no room below — this pill lives at the bottom of the window.
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
  };
  $('mSnr').onclick = () => openMeterMenu($('mSnr'));
  $('mSnr').title = 'Signal reading: SNR / S-meter / dBFS';

  // ★★ THE VTS SITS IN THE SAME CORNER AS THIS CARD (#vts is bottom:14px), so without an
  //    offset the station strip draws straight over the controls. Publish the card's MEASURED
  //    height and let the stylesheet lift the VTS clear of it. A fixed number would be wrong
  //    at most widths: the height changes with the breakpoint, with whether the drums are
  //    stacked, and with the font-size clamp.
  const publishHeight = () => {
    document.documentElement.style.setProperty('--mcard-h', `${Math.round(card.offsetHeight)}px`);
    // ★ The decoder box measures the card directly, so nudge it whenever the card resizes —
    //   otherwise it only re-places itself when the VTS appears, and a card that grew (a
    //   wider window, a longer status line) would be overlapped until something else moved.
    (window as unknown as { _vibeSetDecBoxOffset?: () => void })._vibeSetDecBoxOffset?.();
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
    const grid = document.createElement('div');
    grid.className = 'mModeGrid';
    const cur = deps.mode().toLowerCase();
    for (const m of deps.modes()) {
      const b = document.createElement('button');
      b.textContent = m.toUpperCase();
      b.className = 'mModeOpt' + (m.toLowerCase() === cur ? ' on' : '');
      b.onclick = () => { deps.setMode(m); close(); refresh(); };
      grid.appendChild(b);
    }
    menu.appendChild(grid);
    // ★★★ THE BANDWIDTH ROW COMES WITH THE DEMODULATORS (Stuart): the mode and the width you
    //     listen at are one decision — pick USB and the first thing you reach for is how wide.
    // ★★ MOVED, NOT REBUILT. Two mirrored sliders with a SYNC toggle is real behaviour; a
    //    second copy would drift. The row is parked in #bwHome (hidden) and the NODE is moved in
    //    here, which keeps every listener bound to it; close() puts it back.
    const bw = document.querySelector('#bwHome .bwRow') as HTMLElement | null;
    const bwHome = bw?.parentElement ?? null;
    if (bw) {
      const sep = document.createElement('div');
      sep.className = 'mBwSep';
      sep.textContent = 'BANDWIDTH';
      menu.appendChild(sep);
      menu.appendChild(bw);
    }

    // ★★★ AND THE DECODERS COME WITH THEM, for the same reason the bandwidth row does: what you
    //     demodulate and what you decode out of it are one decision (USB then RTTY, CW then TIME),
    //     which is how the app's ModeSelector has always been laid out. Moving them here is also
    //     what freed the bar's second button for the chat (Stuart, 2026-08-20).
    // ★ A door, not a copy: the decoders panel is a screenful of settings, image views and spot
    //   lists. Rebuilding it inside a popup anchored to the pill would be a second implementation
    //   of a thing that already works.
    /* ★ Before DECODERS, because it is a demodulator choice and they are not. Labelled 'DAB'
     *  like every other entry here; the EXPERIMENTAL warning rides on the multiplex bar, where
     *  it is unmissable the moment you are actually in DAB and cannot squeeze any layout. */
    if (deps.dabCapable()) {
      const dabRow = document.createElement('button');
      dabRow.className = 'mModeOpt' + (deps.dabOn() ? ' on' : '');
      dabRow.textContent = 'DAB';
      dabRow.title = 'DAB (Experimental) — digital radio, Band III';
      dabRow.onclick = () => { deps.toggleDab(); close(); refresh(); };
      grid.appendChild(dabRow);
    }

    const sole = deps.soleDecoder();
    if (sole || deps.anyDecoderLeft()) {
      const decRow = document.createElement('button');
      if (sole) {
        // ★ One survivor: it stands where the door stood, and toggles exactly as it would inside.
        decRow.className = 'mModeOpt' + (sole.on ? ' on' : '');
        decRow.textContent = sole.label;
        decRow.onclick = () => { sole.press(); close(); refresh(); };
      } else {
        decRow.className = 'mModeOpt';
        decRow.textContent = 'DECODERS…';
        decRow.onclick = () => { close(); deps.openDecoders(); };
      }
      grid.appendChild(decRow);
    }

    // ★★ AN EXPLICIT WAY OUT. Dismiss-by-clicking-away is fine when the backdrop is inert; here
    //    the backdrop is the WATERFALL, and a click on it TUNES. So the only obvious way to leave
    //    this menu was also the way to lose the frequency you opened it to listen to (Stuart,
    //    2026-08-05: "I went into it to adjust the bandwidth and clicked the waterfall to exit
    //    which retuned me"). The button is the fix he asked for; `away` swallowing the click below
    //    is the other half, because people will keep clicking away out of habit.
    const closeRow = document.createElement('button');
    closeRow.className = 'mModeClose';
    closeRow.textContent = 'CLOSE';
    closeRow.onclick = () => close();
    menu.appendChild(closeRow);

    document.body.appendChild(menu);
    // Anchored to the pill, and flipped above it when there is no room below — on a
    // phone the pill sits near the bottom, so "below" is almost never where it fits.
    const r = $('mPill').getBoundingClientRect();
    const h = menu.offsetHeight;
    const top = r.top - h - 8 > 0 ? r.top - h - 8 : Math.min(r.bottom + 8, innerHeight - h - 8);
    menu.style.left = `${Math.max(8, Math.min(r.left, innerWidth - menu.offsetWidth - 8))}px`;
    menu.style.top = `${Math.max(8, top)}px`;

    const close = () => {
      // ★ Put the bandwidth row back before the menu is destroyed, or it is removed with it
      //   and the control is gone until the page reloads.
      if (bw && bwHome) bwHome.appendChild(bw);
      menu.remove();
      document.removeEventListener('pointerdown', away, true);
      document.removeEventListener('keydown', esc, true);
    };
    // ★★★ THE DISMISS CLICK IS CONSUMED, NOT PASSED ON. This runs in the CAPTURE phase, so the
    //     same pointerdown that closed the menu went on to reach the waterfall and RETUNE — the
    //     dismiss gesture and the tune gesture being the same event. Standard popover behaviour is
    //     that the first click outside only dismisses; here it is not a nicety, because the action
    //     it was falling through to is destructive of the thing the user was working on.
    const away = (ev: Event) => {
      if (menu.contains(ev.target as Node)) return;
      ev.preventDefault();
      ev.stopPropagation();
      close();
    };
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
  // ★ WRITE ONLY ON CHANGE. This runs 4×/s over elements the user is trying to click, and
  //   replacing textContent destroys and rebuilds the text node under the pointer — churn
  //   that costs nothing to avoid and can only help a mid-click update.
  // ★★ NULL-SAFE ON PURPOSE. refresh() runs 4x/s and touches a dozen elements by id; when one
  //    was renamed out of the markup, put() threw and aborted the REST of the tick — the
  //    frequency, meter, squelch line and mute state all stopped updating, with a console
  //    error as the only clue. A missing element should cost that one readout, not all of them.
  const put = (el: HTMLElement | null, v: string) => {
    if (el && el.textContent !== v) el.textContent = v;
  };

  function refresh() {
    const ft = deps.freqText();
    const fEl = $('mFreq'), uEl = $('mUnit'), fineEl = $('mFreqFine');
    if (!ft) {
      put(fEl, '\u2014'); put(fineEl, '');
    } else {
      put(fEl, ft.main);
      // ★★ THE FINE DIGITS ARE DIMMED, NOT DROPPED. The step ladder goes down to 10 Hz, and MHz
      //    at three decimals resolves only to 1 kHz — so pressing a 500 Hz or 100 Hz step moved
      //    the radio and the display sat there unchanged, which reads as a control that does not
      //    work (Stuart). Showing them at full weight would bury the digits people actually scan
      //    for; a dimmed tail keeps `14.229` legible at a glance with `50` there when you look.
      put(fineEl, ft.fine);
      put(uEl, ft.unit);
    }
    // ★ The airband channel line (src/utils/airband.ts) — hidden, and so taking no room, elsewhere.
    const chEl = document.getElementById('mChan');
    if (chEl) { chEl.hidden = !ft?.chan; put(chEl, ft?.chan ?? ''); }
    put($('mMode'), deps.mode().toUpperCase());
    put($('mStep'), deps.stepLabel());
    // ★ The stereo light, mute, LOCK/FREE and the recorder are NOT polled here: main.ts has ONE
    //   writer for each (syncStereoLight, setMuted, the lock button, initRecorder/updateRecTime)
    //   and it writes the card's own elements. They used to be mirrored from hidden desktop-bar
    //   controls that acted as state holders — two copies of one fact, read four times a second.
    // ★ Throughput, fps, rtt and the link bars live in the card's status row (#linkStats).

    paintSignal();
  }

  /** ★★★ THE METER IS NOT PART OF THE 4 Hz POLL. Everything else in refresh() describes something
   *  a human changes — the frequency, the mode, whether we are recording — and 250 ms is plenty
   *  for those. The signal meter describes the BAND, which changes far faster than a thumb, and
   *  painting it four times a second is a visible lag no amount of work upstream can undo: the
   *  server was sending 20 readings a second and nineteen of them were being thrown away
   *  (Stuart, 2026-08-05, having noticed the squelch meter — drawn on the spectrum frame — was
   *  quicker than this one, which is what pointed straight at the draw rate rather than the data).
   *  ★ Called from the spectrum frame handler, so it paints exactly as often as there is
   *    something new to paint, and never more. */
  function paintSignal() {
    const sig = deps.signal();
    // Clamp: a level outside 0..1 would paint the gradient past the pill or invert it.
    const w = `${Math.max(0, Math.min(1, sig.level)) * 100}%`;
    const mSig = $('mSig');
    if (mSig.style.width !== w) mSig.style.width = w;   // ★ write on change — see setStyle in main.ts
    // ★ While the gate is shut the pill says SQL instead of a number — see the CSS.
    const snrEl = $('mSnr');
    if (snrEl.classList.contains('sql') !== sig.sqlClosed) snrEl.classList.toggle('sql', sig.sqlClosed);
    put(snrEl, sig.sqlClosed ? 'SQL' : meterText(sig));
    const sql = $('mSqlLine');
    if (sig.sqlNorm >= 0) { if (sql.hidden) sql.hidden = false; const l = `${sig.sqlNorm * 100}%`; if (sql.style.left !== l) sql.style.left = l; }
    else if (!sql.hidden) sql.hidden = true;
  }

  // ★ UTC first, then local — the order every band plan, schedule and logbook uses, so
  //   the reading a listener needs is the one they see first.
  /* ★★★ AND "LOCAL" IS THE RECEIVER'S, NOT YOURS — as the app shows it, and as this client's own pop-out
   *  already did. The deck kept formatting the BROWSER's time, so on Kiko's receiver in Brazil it read
   *  "18:41 UTC · 19:41" (Stuart's UK clock) while the app read "15:23 -03" (Stuart, 2026-10-02). The
   *  label is the zone abbreviation, or the signed offset, so it is plainly not your own clock. */
  function clock() {
    const d = new Date();
    const p = (n: number) => String(n).padStart(2, '0');
    const utc = `${p(d.getUTCHours())}:${p(d.getUTCMinutes())} UTC`;
    const srv = deps.serverClock?.() ?? null;
    let loc: string;
    if (srv && Number.isFinite(srv.offsetMin)) {
      // Shift UTC by the receiver's offset and read it back in UTC — no tz database needed.
      const at = new Date(d.getTime() + srv.offsetMin * 60_000);
      const off = srv.offsetMin, a = Math.abs(off);
      const label = srv.abbr || (off === 0 ? 'UTC'
        : `${off > 0 ? '+' : '-'}${p(Math.floor(a / 60))}${a % 60 ? ':' + p(a % 60) : ''}`);
      loc = `${p(at.getUTCHours())}:${p(at.getUTCMinutes())} ${label}`;
    } else {
      loc = `${p(d.getHours())}:${p(d.getMinutes())}`;
    }
    put($('mClock'), `${utc} · ${loc}`);
  }

  refresh();
  clock();
  // 4 Hz is enough for a frequency that changes as fast as a thumb can drag, and cheap
  // enough to leave running on a phone.
  setInterval(refresh, 250);
  setInterval(clock, 10_000);
  return { refresh, paintSignal };
}
