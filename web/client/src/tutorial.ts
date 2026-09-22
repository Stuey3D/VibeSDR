/**
 * tutorial.ts — the web client's first-run tour.
 *
 * ★★★ THE REAL CONTROLS LIGHT UP; THE WORDS SIT IN A PANEL. Captions pinned over the buttons
 *     themselves were the obvious design and the wrong one: the control card rearranges with the
 *     window (wide, the tune and zoom pads flank the frequency; narrow, they flow under it — see
 *     #mcard in index.html), so an anchored bubble has no stable place to be and would have to be
 *     re-measured on every reflow. Making the CONTROL breathe and putting the text in one fixed
 *     panel means the layout can do whatever it likes and the tour still points at the right
 *     thing (Stuart's call: "a box on screen a bit like the decoder box ... easier than trying to
 *     put captions over the controls themselves due to varying screen sizes and layouts").
 *
 * ★★★ IT NEVER RUNS ITSELF. It is reached only from "View tutorial" on the start screen, and that
 *     button is there on every visit — having seen it once must not take it away, because the one
 *     person who wants it again is the one who half-read it the first time. localStorage records
 *     that it has been seen; nothing in here consults that record to decide whether to run.
 *
 * ★★ A STEP WITH NOTHING TO POINT AT IS SKIPPED, NOT SHOWN. #mChat is deliberately GREYED rather
 *    than hidden on a receiver with no shared dial (index.html keeps the stack symmetrical), so
 *    "this is CHAT" on such a server would be a tour of a dead button. Same rule as AGENTS.md's
 *    "a control that only works on one radio should not be there": do not describe what is not
 *    usable here.
 */

/** ★ Recorded, but never consulted to gate the tour — see the header. It exists so a future
 *  "you have not seen the tutorial" nudge has something to read, and so the button can say
 *  "View tutorial again" to somebody who has. */
const SEEN_KEY = 'vibe.webTutorialSeen';

export function tutorialSeen(): boolean {
  try { return localStorage.getItem(SEEN_KEY) === '1'; } catch { return false; }
}
function markSeen() {
  try { localStorage.setItem(SEEN_KEY, '1'); } catch { /* private mode; the tour still ran */ }
}

type Step = {
  /** Element ids of every control this step lights up. The FIRST one that exists decides
   *  whether the step is shown at all; the rest are lit if they are there. */
  ids: string[];
  /** Names the control, so the panel and the glow cannot be read as belonging to different
   *  things — the tour is text in one corner and motion in another, and only the title joins
   *  them up. */
  title: string;
  body: string;
};

// ★ Ids are the card's (#mcard), not the retired desktop bar's. index.html: "This and the desktop
//   bar used to swap at a breakpoint ... The card is now simply THE controls; width changes only
//   the ARRANGEMENT." Pointing at #tuneDown/#stepBtn would light up a bar nobody can see.
const STEPS: Step[] = [
  {
    ids: ['mVfoDown', 'mVfoUp', 'mStep', 'mZoomOut', 'mZoomIn'],
    title: 'Tuning and zoom — ‹ ›, the step button, − and +',
    body: 'These are your main controls. Use [[‹]] and [[›]] to move the dial by the amount shown '
        + 'on the step button [[@mStep]] — press it to change the step. Use [[−]] and [[+]] to zoom '
        + 'the spectrum and waterfall.',
  },
  {
    // ★ The demodulator "button" is the MODE readout inside the pill — mobile.ts binds
    //   $('mMode').onclick to openModePicker(), and the decoders were folded into that same
    //   picker when the DECODERS button became CHAT. It is the control, so it is the target.
    ids: ['mMode'],
    title: 'The demodulator button',
    body: 'The demodulator button [[@mMode]] opens the audio modes and the decoders — how you listen, '
        + 'plus things like RTTY and weather fax.',
  },
  {
    ids: ['mMenu'],
    title: 'MENU',
    body: "[[MENU]] opens the display settings and the receiver's hardware controls.",
  },
  {
    ids: ['mAudio'],
    title: 'The speaker button',
    body: 'The speaker button [[@mAudio]] holds the audio tools: noise reduction, notch filters and '
        + 'squelch — and raw IQ output where the receiver offers it.',
  },
  {
    ids: ['mChat'],
    title: 'CHAT',
    body: '[[CHAT]] is for receivers with a shared tuner — use it to ask before you tune.',
  },
  {
    // ★★ THE WHOLE ROW, NOT THE TEXT ALONE. #status (the KB/s · fps · ping · buf string) lives
    //    INSIDE #linkStats, which also carries the link bars and the SQL / SETTLING / OVERLOAD
    //    chips — and it is the ROW as a block that reads as "technical" to somebody who has just
    //    arrived. Lighting the container lights the thing they are actually looking at.
    // ★ mobile.ts MOVES the real #linkStats node into the card's status line rather than copying
    //   it, so there is exactly one of these to point at whatever the layout is doing.
    ids: ['linkStats'],
    title: 'The connection line',
    body: 'The line at the bottom is how your connection is doing. Ping is how quickly the '
        + 'receiver answers, and buf is how much audio is held in hand to ride out the bumps. '
        + 'Lower is better, but VibeSDR buffers so a weaker connection still plays smoothly.',
  },
];

/** ★ Visible AND usable. `offsetParent` is null for a `display:none` control, and a disabled
 *  button is present but dead — #mChat is disabled on a receiver with no chat, which is exactly
 *  the case the brief says to skip rather than point at. */
function usable(id: string): HTMLElement | null {
  const el = document.getElementById(id);
  if (!el) return null;
  if ((el as HTMLButtonElement).disabled) return null;
  if (!el.getClientRects().length) return null;          // display:none, or collapsed by the layout
  return el;
}

function stepAvailable(s: Step): boolean {
  return s.ids.some((id) => !!usable(id));
}

// ── State ────────────────────────────────────────────────────────────────────

let idx = -1;                       // -1 = not running
let panel: HTMLDivElement | null = null;
let onKey: ((e: KeyboardEvent) => void) | null = null;
let onOutside: ((e: Event) => void) | null = null;
let onResize: (() => void) | null = null;

// ── The breathing idiom ──────────────────────────────────────────────────────
// ★★ THE SAME VISUAL LANGUAGE AS .chatBreathing (index.html): an INSET box-shadow, not a border.
//    A real border changes the button's size, which would nudge every control beside it twice
//    every cycle — the wobble that rule was written for. Amber rather than the chat's blue,
//    because this is the page's own voice, not a message from another listener.
// ★ The style is injected from here rather than added to index.html because the tour lives
//   entirely in src/ — one file to delete if it is ever withdrawn.
const STYLE_ID = 'tutStyle';
function installStyle() {
  if (document.getElementById(STYLE_ID)) return;
  const st = document.createElement('style');
  st.id = STYLE_ID;
  st.textContent = `
@keyframes tutBreathe {
  from { box-shadow: inset 0 0 0 1px rgba(255,176,0,0), 0 0 0 rgba(255,176,0,0); }
  to   { box-shadow: inset 0 0 0 1px rgba(255,176,0,1), 0 0 10px rgba(255,176,0,.55); }
}
.tutBreathing { animation: tutBreathe 1.8s ease-in-out infinite alternate; }
@media (prefers-reduced-motion: reduce) {
  /* ★ Motion is a preference; being SHOWN which button is meant is not. Somebody who has asked
     for less movement still gets the glow, just held still. */
  .tutBreathing { animation: none; box-shadow: inset 0 0 0 1px rgba(255,176,0,.9), 0 0 10px rgba(255,176,0,.45); }
}
#tutBox {
  /* Absolute inside #wfWrap and riding on --decBoxBottom, exactly as #decBox does, so the panel
     clears the control card and the VTS bar without measuring either of them itself. */
  position: absolute; left: 14px; bottom: var(--decBoxBottom, 14px); z-index: 55;
  width: min(94vw, max(46vw, 380px), 560px);
  max-height: calc(100dvh - var(--decBoxBottom, 14px) - 24px);
  display: flex; flex-direction: column; overflow: hidden;
  background: rgba(8,6,2,0.96); border: 1px solid var(--btn-border, #ffa000);
  border-radius: 10px; box-shadow: 0 12px 40px rgba(0,0,0,0.6);
  font-family: var(--mono, ui-monospace, monospace); color: var(--text, #e8e8e8);
}
#tutBox .tutHead {
  display: flex; align-items: baseline; gap: 8px; padding: 8px 10px;
  border-bottom: 1px solid rgba(255,160,0,.25);
}
#tutBox .tutTitle { color: var(--amber, #ffb000); font-size: 12px; letter-spacing: .06em; }
#tutBox .tutCount { margin-left: auto; font-size: 11px; opacity: .6; white-space: nowrap; }
#tutBox .tutBody { padding: 10px; font-size: 12px; line-height: 1.9; overflow-y: auto; }
/* ★★★ THE BUTTON ITSELF, IN THE SENTENCE. Naming a control in prose asks the reader to hold a
   description in their head and then go hunting for something that matches it — and the glyphs
   are the worst case: a bare ‹ in a paragraph is punctuation, not a button (Stuart, 2026-09-22:
   "in the decoder box can we have visual representations of the actual buttons themselves").
   ★★ Drawn from the same tokens as the real control, so it is a picture of THAT button and not a
      generic key cap: --btn-bg, --btn-border, --btn-text, and .mBtn's 9px radius scaled down.
      If the receiver's palette changes, these change with it.
   ★ line-height above is raised to 1.9 so the chips do not crowd the lines they sit in. */
#tutBox .tutKey {
  display: inline-block; padding: 1px 7px; margin: 0 1px;
  background: var(--btn-bg, #1a1206); color: var(--btn-text, var(--amber, #ffb000));
  border: 1px solid var(--btn-border, var(--amber, #ffb000)); border-radius: 6px;
  font-family: inherit; font-size: 11px; line-height: 1.5; white-space: nowrap;
  vertical-align: baseline;
}
/* ★ A cloned glyph is sized for a 44px touch target; inside a sentence it takes the line's size. */
#tutBox .tutKey svg { width: 1.1em; height: 1.1em; vertical-align: -0.15em; display: inline-block; }
#tutBox .tutFoot {
  display: flex; gap: 8px; padding: 8px 10px; border-top: 1px solid rgba(255,160,0,.25);
  flex-wrap: wrap;
}
#tutBox .tutFoot button {
  font: 11px/1 var(--mono, ui-monospace, monospace); letter-spacing: .08em;
  padding: 8px 14px; border-radius: 7px; cursor: pointer;
  color: var(--amber, #ffb000); background: rgba(0,0,0,.5);
  border: 1px solid var(--btn-border, #ffa000);
}
#tutBox .tutFoot button[disabled] { opacity: .35; cursor: default; }
#tutBox .tutFoot .tutSkip { margin-left: auto; opacity: .75; }
`;
  document.head.appendChild(st);
}

/** ★★★ CLEAR BY SELECTOR, NOT BY A REMEMBERED LIST. A list of elements goes stale the moment
 *  something re-renders the card, and a control left breathing for the rest of the session is
 *  the exact "stuck glowing" fault this has to be immune to. Asking the DOM what is currently
 *  marked cannot miss one. */
function clearGlow() {
  for (const el of Array.from(document.querySelectorAll('.tutBreathing'))) {
    el.classList.remove('tutBreathing');
  }
}

function applyGlow(s: Step) {
  clearGlow();
  for (const id of s.ids) usable(id)?.classList.add('tutBreathing');
}

// ── The panel ────────────────────────────────────────────────────────────────

function render() {
  const s = STEPS[idx];
  if (!panel || !s) return;
  applyGlow(s);

  panel.textContent = '';

  const head = document.createElement('div');
  head.className = 'tutHead';
  const title = document.createElement('span');
  title.className = 'tutTitle';
  title.textContent = s.title;
  const count = document.createElement('span');
  count.className = 'tutCount';
  // ★ Counted over the steps that are ACTUALLY being shown on this receiver, not over all five —
  //   "3 of 5" on a tour that stops at four is a promise of a step that never comes.
  const shown = STEPS.filter(stepAvailable);
  count.textContent = `${shown.indexOf(s) + 1} of ${shown.length}`;
  head.append(title, count);

  const body = document.createElement('div');
  body.className = 'tutBody';
  /* ★★ BUILT, NOT PARSED. `[[…]]` in the copy becomes a chip, and every other character is set as
   *  TEXT — so the panel never takes a string as markup and there is nothing for a station name or
   *  a receiver's own wording to inject through. */
  for (const part of s.body.split(/(\[\[[^\]]+\]\])/)) {
    if (!part) continue;
    if (part.startsWith('[[') && part.endsWith(']]')) {
      const tok = part.slice(2, -2);
      const key = document.createElement('span');
      key.className = 'tutKey';
      /* ★★★ `@id` TAKES THE PICTURE FROM THE BUTTON ITSELF rather than describing it twice. The
       *  speaker is an inline SVG and the mode button's caption is whatever this listener is
       *  actually in (NFM, WFM, DAB…), so a literal in the copy would be a drawing of a button
       *  that may not be the one on screen — the "one rule, two readers" shape, where the copy
       *  and the control drift apart and only the copy is wrong.
       *  ★ A DEEP CLONE of the live node, so it cannot be dragged out of the card it belongs to,
       *    and only from OUR OWN document — never from anything a receiver or a station sent.
       *  ★ Falls back to the token as text when the control is not on this page, so a chip is
       *    always drawn and the sentence never loses a word. */
      if (tok.startsWith('@')) {
        const src = document.getElementById(tok.slice(1));
        const svg = src?.querySelector('svg');
        if (svg) key.appendChild(svg.cloneNode(true));
        else key.textContent = (src?.textContent || '').trim() || tok.slice(1);
      } else {
        key.textContent = tok;
      }
      body.appendChild(key);
    } else {
      body.appendChild(document.createTextNode(part));
    }
  }

  const foot = document.createElement('div');
  foot.className = 'tutFoot';
  const back = document.createElement('button');
  back.type = 'button';
  back.textContent = 'BACK';
  const prev = find(idx - 1, -1);
  back.disabled = prev < 0;
  back.onclick = () => { const p = find(idx - 1, -1); if (p >= 0) { idx = p; render(); } };

  const next = document.createElement('button');
  next.type = 'button';
  const after = find(idx + 1, 1);
  next.textContent = after < 0 ? 'FINISH' : 'NEXT';
  next.onclick = () => {
    const n = find(idx + 1, 1);
    if (n < 0) endTutorial();
    else { idx = n; render(); }
  };

  const skip = document.createElement('button');
  skip.type = 'button';
  skip.className = 'tutSkip';
  skip.textContent = 'SKIP';
  skip.onclick = () => endTutorial();

  foot.append(back, next, skip);
  panel.append(head, body, foot);
}

/** First step from `from` in direction `dir` that has something to point at; -1 if none. */
function find(from: number, dir: 1 | -1): number {
  for (let i = from; i >= 0 && i < STEPS.length; i += dir) {
    if (stepAvailable(STEPS[i])) return i;
  }
  return -1;
}

export function tutorialRunning(): boolean { return idx >= 0; }

export function endTutorial() {
  // ★ Safe to call on a tour that is not running — the audio gate does exactly that every time it
  //   reappears. clearGlow() still runs regardless, because the one thing that must never survive
  //   is a control left breathing; only "seen" is conditional, so a stray call cannot rewrite it.
  const wasRunning = idx >= 0;
  idx = -1;
  clearGlow();
  panel?.remove();
  panel = null;
  if (onKey) { window.removeEventListener('keydown', onKey, true); onKey = null; }
  if (onOutside) { window.removeEventListener('pointerdown', onOutside, true); onOutside = null; }
  if (onResize) { window.removeEventListener('resize', onResize); onResize = null; }
  if (wasRunning) markSeen();
}

export function startTutorial() {
  // ★ Re-entrant on purpose: pressing the button while a tour is running restarts it rather than
  //   stacking a second panel on the first.
  if (idx >= 0) endTutorial();
  const first = find(0, 1);
  if (first < 0) return;                 // nothing on this layout to show — say nothing
  installStyle();
  markSeen();

  panel = document.createElement('div');
  panel.id = 'tutBox';
  // ★ Inside #wfWrap so --decBoxBottom means the same thing here as it does for #decBox; body is
  //   the fallback for any page that has not built the waterfall yet.
  (document.getElementById('wfWrap') || document.body).appendChild(panel);

  idx = first;
  render();

  onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') { e.stopPropagation(); endTutorial(); } };
  window.addEventListener('keydown', onKey, true);

  // ★★ THE LAYOUT CAN CHANGE UNDER THE TOUR. Rotating a phone, or dragging a desktop window
  //    narrow, rearranges the card — and a control that goes away while lit would keep the class
  //    on a node nobody can see. Re-applying on resize both moves the glow to whatever is on
  //    screen now and, if the whole step has become unavailable, walks on to one that is not.
  onResize = () => {
    if (idx < 0) return;
    if (!stepAvailable(STEPS[idx])) {
      const n = find(idx + 1, 1);
      const p = n >= 0 ? n : find(idx - 1, -1);
      if (p < 0) { endTutorial(); return; }
      idx = p;
    }
    render();
  };
  window.addEventListener('resize', onResize);

  // ★★ A TAP OUTSIDE ENDS IT, but not the tap that STARTED it: the tour opens from inside the
  //    start screen's own gesture, and arming this immediately let that gesture's own pointerdown
  //    close the panel before it was drawn. Armed on the next tick, after the gesture is over.
  setTimeout(() => {
    if (idx < 0) return;
    onOutside = (e: Event) => {
      const t = e.target as Node | null;
      if (t && panel && panel.contains(t)) return;
      endTutorial();
    };
    window.addEventListener('pointerdown', onOutside, true);
  }, 0);

  // ★ Nothing may survive the page. A pagehide with a control still breathing is only visible
  //   after a bfcache restore, which is precisely when nobody would guess why.
  window.addEventListener('pagehide', () => { if (idx >= 0) endTutorial(); }, { once: true });
}
