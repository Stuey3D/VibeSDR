/**
 * ★★★ THE SERVER HEALTH PILL TUCKS INTO THE EDGE — the web's port of the app's EdgeChip (src/components/EdgeChip.tsx,
 * Stuart, 2026-10-03; asked for on the web 2026-11-12: "the health pill on the app can be flicked off screen to a tiny
 * arrow which then pops out when something is struggling").
 *
 *   · Flick or drag the pill to the right, or click the › on its left edge: it slides off into a small tab on the
 *     screen edge — a + and a ‹, in the pill's own health colour. Click the tab (or drag it left) and the pill comes
 *     back. Pointer events, so a mouse drag, a finger flick and a pen all do the same thing.
 *   · 'auto'   — out, unless a decoder box is open (and not minimised) and the server has nothing urgent to say;
 *     'tucked' — the listener put it away; 'shown' — the listener brought it back.
 *   · CRITICAL (level 3) brings it out even from 'tucked', and puts it back on 'auto' — "if the server is practically
 *     melting show it". Opening a box puts a pill the listener had brought out back on 'auto', so it tucks for the box.
 *  ★ The same rules, word for word, as SDRScreen's chipPref — chipOut() below is the app's chipOut().
 *  ★ The session TIME is not a separate pill on the web (it is a line in the receiver badge, #rxTimeLeft), so only
 *    the health pill tucks here.
 *  ★ Not remembered across page loads, as in the app: a tucked pill is a "not now", not a setting.
 */

export type ChipPref = 'auto' | 'shown' | 'tucked';

/** ★ The app's chipOut(), as data: is the card out? */
export function chipOut(pref: ChipPref, boxOpen: boolean, hot: boolean): boolean {
  return pref === 'shown' || (pref === 'auto' && (!boxOpen || hot));
}

/** The preference after an event — the app's two effects (hot rising → auto; a box opening → 'shown' becomes auto). */
export function nextPref(pref: ChipPref, ev: 'hotRise' | 'boxOpen' | 'userTuck' | 'userShow'): ChipPref {
  switch (ev) {
    case 'hotRise':  return 'auto';
    case 'boxOpen':  return pref === 'shown' ? 'auto' : pref;
    case 'userTuck': return 'tucked';
    case 'userShow': return 'shown';
  }
}

let pref: ChipPref = 'auto';
let hot = false, boxOpen = false;
let dock: HTMLElement | null = null;
let observer: MutationObserver | null = null;

/** ★★★ beta96 THE EDGE DOCK, shared (Stuart 11-27, the map's chips: "collapse to the side of the screen exactly as the
 *  SERVER HEALTH chips do"): out (the card) or tucked (the tab on the edge) — one rule for the health pill and the map's
 *  TUNE / LAYERS / MAP / LIST chips (tracks.ts). The CSS is index.html's `.edgeDock` (`.left` mirrors it). */
export function edgeDockSet(d: HTMLElement, out: boolean): void {
  d.classList.toggle('tucked', !out);
  const card = d.querySelector<HTMLElement>(':scope > .hCard'), tab = d.querySelector<HTMLElement>(':scope > .hTab');
  // ★ The hidden half leaves the tab order and the accessibility tree, so a keyboard never lands on a control that is
  //   off the screen.
  card?.toggleAttribute('inert', !out);
  if (card) card.setAttribute('aria-hidden', String(!out));
  // ★★ beta97 a map's short landscape keeps the tab IN the row while its card is out (the card drops below it, and the
  //    tab tucks it again) — there the tab stays live (edgeTabLive)
  const live = !out || (!!d.closest('.trkView[data-short]') && !d.classList.contains('mDock'));
  tab?.toggleAttribute('inert', !live);
  if (tab) { tab.setAttribute('aria-hidden', String(!live)); tab.tabIndex = live ? 0 : -1; }
}
/** ★★ beta97 the short-landscape switch: every edge dock's tab live again (or not) by the rule above. */
export function edgeTabsLive(root: HTMLElement): void {
  for (const d of Array.from(root.querySelectorAll<HTMLElement>('.edgeDock:not(.mDock)'))) edgeDockSet(d, !d.classList.contains('tucked'));
}

/** ★★ Stuart 15:09: lent to a full-screen map, the pill is a TAB until asked for — the map is centre stage, so neither
 *  'auto' nor a hot battery holds it out there; its › tucks it and its tab brings it back, as the map's own chips. */
let onMap = false, mapPref: 'tucked' | 'shown' = 'tucked';
export function healthTuckOnMap(on: boolean): void { onMap = on; if (on) mapPref = 'tucked'; apply(); }
function apply(): void {
  if (!dock) return;
  const out = onMap ? mapPref === 'shown' : chipOut(pref, boxOpen, hot);
  edgeDockSet(dock, out);
  // ★ the tab sits on the right edge — the receiver badge (name, locator, time) steps left of it (Stuart, 2026-11-12:
  //   "when collapsed the card slightly fouls the server name")
  document.documentElement.classList.toggle('hTucked', !out);
  dock.dataset.pref = pref;
}

function userTuck(): void { if (onMap) { mapPref = 'tucked'; apply(); return; } pref = nextPref(pref, 'userTuck'); apply(); }
function userShow(): void { if (onMap) { mapPref = 'shown'; apply(); return; } pref = nextPref(pref, 'userShow'); apply(); }

/** Read the decoder box: open and not minimised is what the app calls a box being open. */
function readBox(): void {
  const box = document.getElementById('decBox');
  const open = !!box && box.classList.contains('open') && !box.classList.contains('min');
  if (open && !boxOpen) pref = nextPref(pref, 'boxOpen');
  boxOpen = open;
  apply();
}

/** ★ A flick: further than 50 px, or faster than 0.5 px/ms (the app's thresholds). */
function wireDrag(card: HTMLElement, d: HTMLElement | null = dock, signOf: number | (() => number) = 1, onTuck: () => void = userTuck, axis: 'x' | 'y' = 'x'): void {
  // ★ the sign is read at each gesture: a map chip can change edge as its window is resized
  let sign = 1;
  let id = -1, x0 = 0, y0 = 0, dragging = false, lastX = 0, lastT = 0, vx = 0, swallowClick = false;
  // ★ axis 'y': a BOTTOM-edge dock (the map's LIST) — the same gesture, turned through 90°
  const along = (e: PointerEvent) => (axis === 'x' ? e.clientX : e.clientY), across = (e: PointerEvent) => (axis === 'x' ? e.clientY : e.clientX);
  card.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || (d ?? dock)?.classList.contains('tucked')) return;
    id = e.pointerId; x0 = lastX = along(e); y0 = across(e); lastT = e.timeStamp; vx = 0; dragging = false;
    sign = typeof signOf === 'function' ? signOf() : signOf;
  });
  card.addEventListener('pointermove', (e) => {
    if (e.pointerId !== id) return;
    // ★ sign: +1 a right-edge dock (drag right to tuck), −1 a left-edge one (drag left) — the same thresholds
    const dx = (along(e) - x0) * sign, dy = across(e) - y0;
    if (!dragging) {
      if (!(dx > 6 && Math.abs(dx) > Math.abs(dy))) return;
      dragging = true;
      try { card.setPointerCapture(id); } catch { /* the pointer may already be gone */ }
      card.classList.add('dragging');
    }
    const dt = Math.max(1, e.timeStamp - lastT);
    vx = (along(e) - lastX) * sign / dt; lastX = along(e); lastT = e.timeStamp;
    card.style.transform = `translate${axis === 'x' ? 'X' : 'Y'}(${Math.max(0, dx) * sign}px)`;
    e.preventDefault();
  });
  const end = (e: PointerEvent, cancelled: boolean) => {
    if (e.pointerId !== id) return;
    id = -1;
    if (!dragging) return;
    dragging = false;
    swallowClick = true; setTimeout(() => { swallowClick = false; }, 0);
    const dx = (along(e) - x0) * sign;
    // ★ Restore the transition BEFORE clearing the inline transform, in the same frame: the browser then animates
    //   from where the finger let go to wherever the class says — off the edge, or back home.
    card.classList.remove('dragging');
    card.style.transform = '';
    if (!cancelled && (dx > 50 || vx > 0.5)) onTuck();
  };
  card.addEventListener('pointerup', (e) => end(e, false));
  card.addEventListener('pointercancel', (e) => end(e, true));
  // A drag that started on the › must not also count as a click on it.
  card.addEventListener('click', (e) => { if (swallowClick) { e.stopPropagation(); e.preventDefault(); } }, true);
}

function wireTab(tab: HTMLElement, signOf: number | (() => number) = 1, onShow: () => void = userShow, axis: 'x' | 'y' = 'x'): void {
  const sg = () => (typeof signOf === 'function' ? signOf() : signOf);
  tab.addEventListener('click', () => onShow());
  // ★ The app's tab also comes out on a drag away from its edge.
  let id = -1, x0 = 0;
  const along = (e: PointerEvent) => (axis === 'x' ? e.clientX : e.clientY);
  tab.addEventListener('pointerdown', (e) => { id = e.pointerId; x0 = along(e); });
  tab.addEventListener('pointerup', (e) => { if (e.pointerId === id && (along(e) - x0) * sg() < -20) onShow(); id = -1; });
}

/** ★★★ beta96 an edge dock of the map's (tracks.ts): the same drag, flick, › and tab as the health pill's — `side` the
 *  screen edge it tucks into. */
export function wireEdgeDock(d: HTMLElement, side: 'left' | 'right' | 'bottom', onTuck: () => void, onShow: () => void): void {
  // ★ the edge is read from the dock at each gesture (data-side), so a chip moved to the other edge still slides the right way
  const sign = () => ((d.dataset.side || side) === 'left' ? -1 : 1), axis = side === 'bottom' ? 'y' : 'x';
  const card = d.querySelector<HTMLElement>(':scope > .hCard'), tab = d.querySelector<HTMLElement>(':scope > .hTab');
  d.querySelector<HTMLElement>('.hTuck')?.addEventListener('click', (e) => { e.stopPropagation(); onTuck(); });
  if (card) wireDrag(card, d, sign, onTuck, axis);
  if (tab) wireTab(tab, sign, onShow, axis);
}

/** ★ Build (once) the card, its › and the tab inside #srvHealth, and return the element the pill's contents go in. */
export function healthTuckBody(el: HTMLElement): HTMLElement {
  let body = el.querySelector<HTMLElement>('.hBody');
  if (body) return body;
  dock = el;
  el.classList.add('edgeDock');   // ★ beta96 the shared edge-dock CSS (index.html), also the map's chips'
  el.dataset.gpskip = '';   // ★ pointer-only: off the pad / keyboard route (gamepad.ts candidates)
  el.innerHTML =
      `<div class="hCard"><button type="button" class="hTuck" aria-label="Hide server health" title="Tuck away">›</button>`
    + `<div class="hBody" role="group"></div></div>`
    + `<button type="button" class="hTab" aria-label="Show server health" title="Server health">`
    + `<span class="hTabPlus" aria-hidden="true">+</span><span class="hTabChev" aria-hidden="true">‹</span></button>`;
  body = el.querySelector<HTMLElement>('.hBody')!;
  const card = el.querySelector<HTMLElement>('.hCard')!;
  el.querySelector<HTMLElement>('.hTuck')!.addEventListener('click', () => userTuck());
  wireDrag(card);
  wireTab(el.querySelector<HTMLElement>('.hTab')!);
  const box = document.getElementById('decBox');
  observer?.disconnect();
  if (box) {
    observer = new MutationObserver(readBox);
    observer.observe(box, { attributes: true, attributeFilter: ['class'] });
  }
  readBox();
  return body;
}

/** Each health report: is it critical, and the colour the tab and the › wear. */
export function healthTuckLevel(worst: number, colour: string): void {
  const isHot = worst >= 3;
  if (isHot && !hot) pref = nextPref(pref, 'hotRise');
  hot = isHot;
  dock?.style.setProperty('--hc', colour);
  apply();
}

/** For the tests: the state as a string. */
export function healthTuckState(): string { return `${pref}/${boxOpen ? 'box' : 'nobox'}/${hot ? 'hot' : 'cool'}`; }
