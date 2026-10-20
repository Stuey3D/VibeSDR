/**
 * ★★★ THE CONTROLLER GUIDE on the web page (Stuart approved the prototype 2026-10-19: "controller guides look fine").
 * The drawings and every word come from src/utils/padGuide.ts — the app draws the same data — so this file only turns
 * them into SVG markup and page text inside #padGuide (index.html), and keeps the leader lines true.
 *
 *  • Opened from the menu (CONTROLLER GUIDE) or by SELECT on a game controller, which also closes it (gamepad.ts);
 *    back closes it, as it closes every panel (it is one of main.ts's PANELS — Escape and a click away too).
 *  • While it is open the D-pad left / right moves between the two tabs (gamepad.ts); it opens on the layout in use and
 *    follows a HOLD START switch while open (setPadGuideLayout).
 * ★★ THE LAYOUT FOLLOWS THE WINDOW — the GUIDE'S OWN WIDTH, not the device: three columns with leader lines when there
 *    is room, the pad then a numbered list (two columns, then one) when there is not (guideColumns). A ResizeObserver,
 *    not a media query, because the panel is narrower than the window and old WebViews have no container queries.
 * ★ Leader lines are worked out from where things ARE on screen, every time the box changes size and when the panel's
 *   opening animation ends (it is scaled while it opens) — never fixed points.
 * ★ Colours are the page's tokens where it has them (--amber for the title and BACK, the panel glass and border, the
 *   menu's own tab keys); the drawing's warm greys and the action green are the prototype's, which sit on the
 *   amber-on-black page as designed.
 */
import { PAD_GUIDES, guideColumns, leaderPath, type ArtRole, type ArtShape, type PadLayout } from '../../../src/utils/padGuide';

const ROLE: Record<ArtRole, string> = {
  body: 'var(--pg-body)', edge: 'var(--pg-edge)', key: 'var(--pg-key)', keyEdge: 'var(--pg-keye)',
  well: 'var(--pg-well)', action: 'var(--pg-grn)', actionBg: 'var(--pg-grnbg)', none: 'none',
};

let layout: PadLayout = 'sticks';
/** The web page's addition to the shared typing lines: a computer has a mouse and a keyboard as well as a touchscreen. */
const WEB_KB_LINE = 'On a computer, click the box or just type on the keyboard — the on-screen keyboard steps aside, keeping what you typed';
let built = false;

const $ = (id: string) => document.getElementById(id);

function esc(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

/** One shape as SVG markup. ★ Colours as a style (var() is not allowed in a presentation attribute). */
function shapeSvg(sh: ArtShape): string {
  const paint = `fill:${ROLE[sh.fill]}` + (sh.stroke ? `;stroke:${ROLE[sh.stroke]};stroke-width:${sh.sw ?? 1}` : '');
  if (sh.k === 'rect') {
    const rot = sh.rot ? ` transform="rotate(${sh.rot[0]} ${sh.rot[1]} ${sh.rot[2]})"` : '';
    return `<rect x="${sh.x}" y="${sh.y}" width="${sh.w}" height="${sh.h}" rx="${sh.rx}" style="${paint}"${rot}/>`;
  }
  if (sh.k === 'circle') return `<circle cx="${sh.cx}" cy="${sh.cy}" r="${sh.r}" style="${paint}"/>`;
  return `<path d="${sh.d}" style="${paint}"/>`;
}

export function padGuideOpen(): boolean {
  return !!$('padGuide')?.classList.contains('open');
}

export function padGuideLayout(): PadLayout { return layout; }

function render() {
  const g = PAD_GUIDES[layout];
  $('pgTitle')!.textContent = g.title;
  $('pgSub')!.textContent = g.sub;
  $('pgFoot')!.textContent = g.foot;
  // ★ TYPING — the controller keyboard's controls (padKeyboard.ts), the app's words, plus the page's own hand-over line.
  $('pgKb')!.innerHTML = [...g.keyboard, WEB_KB_LINE].map((t) => `<p>${esc(t)}</p>`).join('');
  const marks = g.items.map((it, i) =>
    `<g><circle cx="${it.at[0]}" cy="${it.at[1]}" r="13" style="fill:var(--pg-grnbg);stroke:var(--pg-grn);stroke-width:2.5"/>` +
    `<text x="${it.at[0]}" y="${it.at[1] + 6}" font-family="Helvetica Neue, Arial, sans-serif" font-size="17" font-weight="700" ` +
    `text-anchor="middle" style="fill:var(--pg-grn)">${i + 1}</text></g>`).join('');
  $('pgPad')!.innerHTML = `<svg viewBox="${g.view.join(' ')}" preserveAspectRatio="xMidYMid meet" role="img" ` +
    `aria-label="${esc(g.title)}">${g.art.map(shapeSvg).join('')}${marks}</svg>`;
  for (const side of ['left', 'right'] as const) {
    $(side === 'left' ? 'pgLeft' : 'pgRight')!.innerHTML = g.items.map((it, i) => it.side !== side ? '' :
      `<div class="pgLab ${it.kind}" data-i="${i}"><span class="pgNum">${i + 1}</span><span><b>${esc(it.t)}</b>` +
      `${it.s ? `<span class="pgS">${esc(it.s)}</span>` : ''}</span></div>`).join('');
  }
  document.querySelectorAll<HTMLButtonElement>('#pgTabs button').forEach((b) => {
    const on = b.dataset.g === layout;
    b.classList.toggle('on', on);
    b.setAttribute('aria-pressed', String(on));
  });
  requestAnimationFrame(drawLines);
}

/** The guide's own width decides its layout (guideColumns) — set as a class the CSS draws. */
function fit() {
  const host = $('pgGuide');
  if (!host) return;
  const n = guideColumns(host.clientWidth);
  host.classList.toggle('pgCols3', n === 3);
  host.classList.toggle('pgCols2', n === 2);
  host.classList.toggle('pgCols1', n === 1);
}

function drawLines() {
  const lines = $('pgLines') as unknown as SVGSVGElement | null;
  const host = $('pgGuide');
  const svg = document.querySelector<SVGSVGElement>('#pgPad svg');
  if (!lines || !host || !svg) return;
  if (!host.classList.contains('pgCols3') || !padGuideOpen()) { lines.innerHTML = ''; return; }
  const hr = host.getBoundingClientRect();
  const m = svg.getScreenCTM();
  if (!m || !hr.width) { lines.innerHTML = ''; return; }
  // ★ getBoundingClientRect includes the panel's opening scale; dividing by it draws in the host's own units.
  const k = host.offsetWidth ? hr.width / host.offsetWidth : 1;
  const pt = svg.createSVGPoint();
  const g = PAD_GUIDES[layout];
  let d = '';
  host.querySelectorAll<HTMLElement>('.pgLab').forEach((el) => {
    const it = g.items[+(el.dataset.i ?? -1)];
    const num = el.querySelector('.pgNum');
    if (!it || !num) return;
    pt.x = it.at[0]; pt.y = it.at[1];
    const p = pt.matrixTransform(m);
    const n = num.getBoundingClientRect();
    const x1 = it.side === 'left' ? n.right + 6 : n.left - 6;
    d += leaderPath((p.x - hr.left) / k, (p.y - hr.top) / k, (x1 - hr.left) / k, (n.top + n.height / 2 - hr.top) / k, it.side);
  });
  lines.innerHTML = `<path d="${d}"/>`;
}

function build() {
  if (built) return;
  built = true;
  document.querySelectorAll<HTMLButtonElement>('#pgTabs button').forEach((b) =>
    b.addEventListener('click', () => { layout = b.dataset.g === 'buttons' ? 'buttons' : 'sticks'; render(); }));
  const host = $('pgGuide')!;
  const redo = () => { fit(); requestAnimationFrame(drawLines); };
  if (typeof ResizeObserver !== 'undefined') new ResizeObserver(redo).observe(host);
  window.addEventListener('resize', redo);
  // The panel opens with a short scale-in; the lines are measured again once it has landed.
  $('padGuide')!.addEventListener('transitionend', (e) => { if (e.target === $('padGuide')) redo(); });
}

/** Show the guide on `l` — the caller closes any other panel first (main.ts togglePadGuide). */
export function showPadGuide(l: PadLayout) {
  build();
  layout = l;
  const panel = $('padGuide')!;
  panel.classList.add('open');
  panel.scrollTop = 0;
  fit();
  render();
}

/** HOLD START switched the controls, or the D-pad stepped a tab: show that layout if the guide is open. */
export function setPadGuideLayout(l: PadLayout) {
  if (!padGuideOpen() || l === layout) return;
  layout = l;
  render();
}

/** The D-pad up / down while the guide is open: scroll it (a phone shows the list below the pad). */
export function scrollPadGuide(dir: 1 | -1) {
  const panel = $('padGuide');
  if (panel) panel.scrollTop += dir * 90;
}
