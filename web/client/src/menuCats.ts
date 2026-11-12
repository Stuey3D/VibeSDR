/**
 * menuCats.ts — the MENU's folding categories (Stuart, 2026-11-12: "the main menu has gotten very long on the web
 * client so maybe have collapsable catagories like we do with the r36s … all the options default collapsed so the menu
 * is not overwhelming").
 *
 * ★★★ THE R36S's MENU, ON THE WEB (handheld-ui/src/app_impl.h drawMenu): one header per category, ONE OPEN AT A TIME
 *     (opening one folds the others), and back — B on a pad, Escape / Backspace on a keyboard — folds the open one
 *     before it closes the menu, leaving the highlight on its header.
 * ★★ The headers and bodies are index.html's own markup (`button.mCat[data-cat]` + `.mCatBody[data-cat]`): every row
 *    keeps its id and its listeners, nothing is copied. A folded body is `hidden`, so nothing inside it is a pad /
 *    keyboard stop (gamepad.ts visible()); a header is a <button>, so it is one, and enter on it opens it.
 * ★ REMEMBERED PER BROWSER: which one is open (or none), in localStorage, every access in try/catch — a private window
 *   or blocked storage simply starts folded. A first visit starts with everything folded.
 * ★ ATTENTION OPENS IT: revealMenuCat(id) for something inside a folded category that the listener must act on.
 * ★★ THE PASSWORD IS IN SERVER (Stuart, 2026-11-12: one owner password box, in SERVER at the bottom). SERVER is not a
 *    folding category — it is two rows — so revealServerUnlock() folds the open category (the box sits right under the
 *    headers then), scrolls to it and puts the cursor in it; with the menu shut (the idle re-lock, a refused password)
 *    it does that the next time the menu opens. SDR HARDWARE's "locked — unlock under SERVER below" line calls it.
 * ★ The handheld page (handheld.ts) adds its own R36S HARDWARE category with addMenuCat and listens with onMenuCatOpen.
 */
import { pushBack } from './gamepad';

const LS_OPEN = 'vibesdr_menu_cat_v1';

interface Cat { id: string; btn: HTMLButtonElement; body: HTMLElement }
const cats: Cat[] = [];
const listeners: Array<(id: string) => void> = [];
let menuEl: HTMLElement | null = null;
let openId = '';
let unback: (() => void) | null = null;
/** The password box is wanted the next time the menu opens (revealServerUnlock with the menu shut). */
let wantServer = false;

function readSaved(): string {
  try { return localStorage.getItem(LS_OPEN) || ''; } catch { return ''; }
}
function writeSaved(id: string) {
  try { if (id) localStorage.setItem(LS_OPEN, id); else localStorage.removeItem(LS_OPEN); } catch { /* private mode */ }
}

const menuOpen = () => !!menuEl?.classList.contains('open');

/** ★ Back folds the open category — but only while the menu is up. A fold registered while the menu is shut would
 *  make the next back on the radio fold something nobody can see, instead of doing what back does there. */
function syncBack() {
  if (wantServer && menuOpen()) { wantServer = false; landOnServer(); }
  const want = !!openId && menuOpen();
  if (want && !unback) {
    unback = pushBack(() => {
      unback = null;
      const c = cats.find((x) => x.id === openId);
      setOpen('', true);
      return c?.btn;            // ★ the highlight goes back to the header it came from (gamepad.ts back)
    });
  } else if (!want && unback) { unback(); unback = null; }
}

function paint() {
  for (const c of cats) {
    const o = c.id === openId;
    c.btn.setAttribute('aria-expanded', String(o));
    c.body.hidden = !o;
  }
}

function setOpen(id: string, remember: boolean) {
  const prev = openId;
  openId = cats.some((c) => c.id === id) ? id : '';
  paint();
  if (remember) writeSaved(openId);
  syncBack();
  if (openId && openId !== prev) for (const f of listeners) f(openId);
}

function wire(c: Cat) {
  c.btn.addEventListener('click', () => {
    const opening = openId !== c.id;
    setOpen(opening ? c.id : '', true);
    // ★ Opening a category below an open one folds that one away above it, and the header jumps up the panel: keep
    //   the header where the eye (and the highlight) is.
    if (opening) c.btn.scrollIntoView({ block: 'nearest' });
  });
}

/** Wire the menu's categories (once). Starts on what this browser last had open, or all folded. */
export function initMenuCats(menu: HTMLElement) {
  if (menuEl) return;
  menuEl = menu;
  for (const btn of Array.from(menu.querySelectorAll<HTMLButtonElement>('button.mCat[data-cat]'))) {
    const id = btn.dataset.cat || '';
    const body = menu.querySelector<HTMLElement>(`.mCatBody[data-cat="${id}"]`);
    if (!id || !body) continue;
    const c = { id, btn, body };
    cats.push(c);
    wire(c);
  }
  openId = '';
  const saved = readSaved();
  if (cats.some((c) => c.id === saved)) openId = saved;
  paint();
  // ★ The menu opens and closes by its .open class from several places (the MENU key, CLOSE, back, a click outside):
  //   watch the class rather than every one of them.
  new MutationObserver(syncBack).observe(menu, { attributes: true, attributeFilter: ['class'] });
  syncBack();
}

/** A category made by script (the handheld's R36S HARDWARE), placed before `before`. Returns its body. */
export function addMenuCat(id: string, title: string, before: Node | null): HTMLElement {
  const menu = menuEl!;
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'btn mCat';
  btn.dataset.cat = id;
  btn.setAttribute('data-navwide', '');   // ★ straight ahead of whatever is above or below it (gamepad.ts navBox)
  btn.id = 'mCat-' + id;
  btn.setAttribute('aria-expanded', 'false');
  btn.textContent = title;
  const body = document.createElement('div');
  body.className = 'mCatBody';
  body.dataset.cat = id;
  body.setAttribute('role', 'region');
  body.setAttribute('aria-labelledby', btn.id);
  body.hidden = true;
  menu.insertBefore(btn, before);
  menu.insertBefore(body, before);
  const c = { id, btn, body };
  // ★ In page order, so nothing depends on the order the categories were registered in.
  cats.push(c);
  cats.sort((a, b) => (a.btn.compareDocumentPosition(b.btn) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1));
  wire(c);
  if (readSaved() === id) setOpen(id, false); else paint();
  return body;
}

/** The body of category `id` (null if there is none). */
export function menuCatBody(id: string): HTMLElement | null { return cats.find((c) => c.id === id)?.body ?? null; }

/** Which category is open ('' = none). */
export function openMenuCat(): string { return openId; }

/** Open category `id` (folding the others), or fold everything with ''. Remembered like a press. */
export function setMenuCat(id: string) { setOpen(id, true); }

/** ★ Something inside category `id` needs the listener (a password wanted again): open it, unless it is already. */
export function revealMenuCat(id: string) { if (openId !== id) setOpen(id, true); }

/** Called with the id whenever a category opens. */
export function onMenuCatOpen(f: (id: string) => void) { listeners.push(f); }

/** ★ Scroll SERVER into view and put the cursor in its password box (a pad driving gets the highlight instead —
 *  gamepad.ts focusin). Waits a frame so a category folded this instant has given its height back first. */
function landOnServer() {
  requestAnimationFrame(() => {
    const blk = document.getElementById('srvBlock');
    const inp = document.getElementById('adminPwInput') as HTMLInputElement | null;
    if (!blk || blk.hidden) return;
    blk.scrollIntoView({ block: 'nearest' });
    if (inp && !inp.closest('[hidden]')) inp.focus({ preventScroll: true });
  });
}

/** ★★ The password is wanted (SDR HARDWARE's locked line, the idle re-lock, a refused password): fold the open
 *  category so SERVER sits right under the headers, and land on its password box — now if the menu is up, else the
 *  next time it opens. */
export function revealServerUnlock() {
  setOpen('', true);
  if (menuOpen()) landOnServer(); else wantServer = true;
}
