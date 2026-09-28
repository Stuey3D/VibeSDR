/**
 * ★★★ PORTABLE VIEW SETTINGS AND BOOKMARKS across every *.vibeserver.vibesdr.net server (Stuart, 2026-09-19:
 *     "every new server that comes online I have to set Sonar Green 40% spectrum 5 brightness 5 sharpness
 *     ... colour to orange").
 *
 * THREE LAYERS, highest first:
 *   1. OVERRIDE — this server's own storage (LS_PREFS in main.ts). A server's vanity address never changes,
 *      so its origin's storage is a stable per-server place, and display tweaks already save there.
 *   2. MASTER  — one store in the DIRECTORY's origin (vibeserver.vibesdr.net), reached through a hidden
 *      iframe (/store.html) by postMessage. A page cannot read another origin's storage directly; every
 *      *.vibeserver.vibesdr.net address is one SITE, so Chromium and Firefox keep that one store shared
 *      between them. ★★ SAFARI DOES NOT: it keys the frame's localStorage by the page around it, so the
 *      store also keeps a COOKIE, which WebKit does share between same-site pages (store.html has the
 *      measurement, 2026-09-28). "Save view settings for all" writes it.
 *   3. The built-in defaults.
 *
 * ★ Nothing here leaves the browser: no cookie, nothing sent with a request, nothing stored by us. Only VIEW
 *   settings travel — never gain, hardware, where you were tuned, or any credential.
 * ★ RESET lives on the directory page. It clears MASTER and stamps a reset time; a server page that loads
 *   with overrides older than that stamp drops them, so a reset reaches every server, not just the master.
 * ★★ "SAVE FOR ALL" STAMPS ITS OWN TIME (viewAt) FOR THE SAME REASON. Writing MASTER is not enough on its
 *    own: layer 1 beats layer 2, and any server the listener has already tweaked is holding a layer-1
 *    override that would win forever. The stamp is what makes those servers let go of theirs.
 * ★ Only on *.vibeserver.vibesdr.net. A LAN, port-forwarded or raw tunnel address behaves exactly as before.
 */

/** The settings that travel — what the Display menu's Reset already groups, plus the VFO look and a few
 *  personal ones. Hardware, tuning, squelch, DAB block and the like stay with the radio. */
export const VIEW_KEYS = [
  'palette', 'autoContrast', 'minDb', 'maxDb', 'wfBrightness', 'wfContrast', 'wfSharpness', 'wfCoarse',
  'wfSpeed', 'wfScroll', 'smoothingFrames', 'spatialSmooth', 'peakHold', 'specFloor', 'specPeakScale',
  'specAlpha', 'specRatio', 'specShow', 'vfoColor', 'vfoIntensity', 'vfoFrost', 'freqUnit', 'volume',
] as const;

const DIRECTORY = 'https://vibeserver.vibesdr.net';
const EPOCH_KEY = 'vsPortableEpoch';          // this origin's copy of the last reset it has honoured
/* ★★★ THE VIEW EPOCH IS ITS OWN, AND MUST STAY THAT WAY. "Save view settings for all" has to reach
 *     servers this browser is not currently on, and the only way it can is by making them drop their
 *     own overrides next time they load — the same trick RESET uses. It cannot BORROW resetAt to do
 *     it: search.ts clears the listener's BOOKMARKS when resetAt advances, so a colour change would
 *     have silently binned their bookmarks on every other server. Two readers, two epochs. */
const VIEW_EPOCH_KEY = 'vsPortableViewEpoch';
/* ★★★ THIS SERVER'S COPY OF THE LAST MASTER IT SAW — AND WITHOUT IT "SAVE FOR ALL" MADE THINGS WORSE.
 *     Stuart, 2026-09-27, on the Pi 500 that had carried his view since day one, loading at the
 *     defaults: "I got that save view for all vibeservers setting to reduce the amount of times I had
 *     to set up the view to my liking, no INCREASE it!"
 *     Save-for-all DELETES the server's own overrides so that it follows master — which left every
 *     server holding NOTHING of its own, and reading its whole view from a hidden iframe on another
 *     origin at every load, with 2 s for the frame and 1.5 s for the answer. Any load where that did
 *     not come back in time (a cold tunnel, a slow directory) — or where the directory's storage had
 *     been cleared — fell straight through to the BUILT-IN DEFAULTS, silently. One round-trip stood
 *     between the listener and their view, and it was allowed to fail to "factory".
 *     ★ So each origin keeps the last master view it was given, and uses it whenever the store does
 *       not answer, or answers EMPTY without a reset having been done. A deliberate RESET (resetAt
 *       moved on) still clears it — that is the one time empty is the answer. */
const MASTER_COPY_KEY = 'vsPortableMasterCopy';
interface MasterCopy { view: Record<string, unknown>; resetAt: number; viewAt: number }
function readCopy(): MasterCopy | null {
  try {
    const c = JSON.parse(localStorage.getItem(MASTER_COPY_KEY) || 'null');
    return c && c.view && typeof c.view === 'object' && Object.keys(c.view).length ? c : null;
  } catch { return null; }
}
function writeCopy(c: MasterCopy | null) {
  try {
    if (c && Object.keys(c.view).length) localStorage.setItem(MASTER_COPY_KEY, JSON.stringify(c));
    else localStorage.removeItem(MASTER_COPY_KEY);
  } catch { /* private mode */ }
}

export interface Portable { view: Record<string, unknown>; bookmarks: unknown[] | null; resetAt: number; viewAt: number }
let master: Portable = { view: {}, bookmarks: null, resetAt: 0, viewAt: 0 };
/** ★ Decided ONCE, when the store loads, before anything can write the epoch: was the directory reset since
 *  this origin last looked? Settings (honourReset) and bookmarks (search.ts) both act on this one answer —
 *  deciding it twice raced, and the second reader saw the epoch the first had already written. */
let resetPending = false;
/** ★ Decided once alongside resetPending: has "save for all" run somewhere since this origin looked?
 *  Drops this server's VIEW overrides only — bookmarks are not involved. */
let viewPending = false;
let frame: HTMLIFrameElement | null = null;
let ready: Promise<boolean> | null = null;
let seq = 0;
const waiting = new Map<number, (v: any) => void>();

export function onVibeDomain(): boolean {
  const h = location.hostname;
  return h.endsWith('.vibeserver.vibesdr.net') && location.protocol === 'https:';
}

function ask(msg: Record<string, unknown>, timeoutMs = 1500): Promise<any> {
  return new Promise((resolve) => {
    if (!frame?.contentWindow) return resolve(null);
    const id = ++seq;
    const t = setTimeout(() => { waiting.delete(id); resolve(null); }, timeoutMs);
    waiting.set(id, (v) => { clearTimeout(t); resolve(v); });
    frame.contentWindow.postMessage({ ...msg, id }, DIRECTORY);
  });
}

/** Load MASTER once. Resolves false (and changes nothing) off the VibeSDR domain, or if the store does not
 *  answer in time — a blocked or slow store must never hold up the radio. */
export function portableReady(): Promise<boolean> {
  if (ready) return ready;
  ready = (async () => {
    if (!onVibeDomain()) return false;
    /* ★ Until (and unless) the store answers, this origin's copy IS master. Set first, so a store that
     *  never answers leaves the page on the listener's view, not on the defaults. No epochs move: a
     *  copy is only what we already knew, so it must never drop anything. */
    const copy = readCopy();
    if (copy) master = { view: copy.view, bookmarks: null, resetAt: copy.resetAt, viewAt: copy.viewAt };
    window.addEventListener('message', (e) => {
      if (e.origin !== DIRECTORY || !e.data || typeof e.data.id !== 'number') return;
      const cb = waiting.get(e.data.id); if (cb) { waiting.delete(e.data.id); cb(e.data); }
    });
    frame = document.createElement('iframe');
    frame.src = `${DIRECTORY}/store`;   // ★ the assets binding serves store.html here (a .html path 307s to it)
    frame.style.display = 'none';
    frame.setAttribute('aria-hidden', 'true');
    const loaded = new Promise<void>((r) => { frame!.onload = () => r(); });
    document.body.appendChild(frame);
    await Promise.race([loaded, new Promise((r) => setTimeout(r, 2000))]);
    const got = await ask({ op: 'get' });
    if (!got || !got.data) {
      if (copy) console.warn('portable settings: store did not answer; using this server\'s copy of the master view');
      return false;
    }
    const fresh: Portable = { view: got.data.view || {}, bookmarks: got.data.bookmarks ?? null,
                              resetAt: Number(got.data.resetAt) || 0, viewAt: Number(got.data.viewAt) || 0 };
    const storeEmpty = !Object.keys(fresh.view).length;
    if (storeEmpty && copy && fresh.resetAt <= copy.resetAt) {
      /* ★★ THE STORE LOST ITS VIEW WITHOUT A RESET — browser storage cleared, a new profile. That is
       *  not the listener asking for defaults; keep their view. (Not written back: this page does not
       *  get to stamp viewAt on every other server's behalf.) */
      console.warn('portable settings: store answered empty with no reset; using this server\'s copy');
      fresh.view = copy.view;
      fresh.viewAt = copy.viewAt;
    }
    master = fresh;
    writeCopy(storeEmpty && fresh.resetAt > (copy?.resetAt ?? 0) ? null
              : { view: master.view, resetAt: master.resetAt, viewAt: master.viewAt });
    let seen = 0, seenView = 0;
    try { seen = Number(localStorage.getItem(EPOCH_KEY)) || 0; } catch { /* private mode */ }
    try { seenView = Number(localStorage.getItem(VIEW_EPOCH_KEY)) || 0; } catch { /* private mode */ }
    resetPending = master.resetAt > seen;
    viewPending  = master.viewAt  > seenView;
    if (resetPending) try { localStorage.setItem(EPOCH_KEY, String(master.resetAt)); } catch { /* private mode */ }
    if (viewPending)  try { localStorage.setItem(VIEW_EPOCH_KEY, String(master.viewAt)); } catch { /* private mode */ }
    return true;
  })();
  return ready;
}

/** MASTER's view settings, for prefs() to lay under this server's own. */
export function masterView(): Record<string, unknown> { return master.view; }
export function masterBookmarks(): unknown[] | null { return master.bookmarks; }

/** Drop this server's VIEW overrides if either epoch has moved since this origin last looked — a RESET on
 *  the directory page, or a "save for all" done on some other server. Returns true if anything was dropped.
 *  `local` is this origin's prefs object; the caller writes it back. */
export function honourReset(local: Record<string, unknown>): boolean {
  if (!resetPending && !viewPending) return false;
  let dropped = false;
  for (const k of VIEW_KEYS) if (k in local) { delete local[k]; dropped = true; }
  return dropped;
}
/** Whether the directory was reset since this server's page last looked — bookmarks honour it too. */
export function portableWasReset(): boolean { return resetPending; }

/** "Save view settings for all VibeSDR.net servers": the given values become MASTER. */
export async function saveViewForAll(values: Record<string, unknown>): Promise<boolean> {
  const view: Record<string, unknown> = {};
  for (const k of VIEW_KEYS) if (values[k] !== undefined) view[k] = values[k];
  const r = await ask({ op: 'setView', view });
  if (r?.ok) {
    master.view = view;
    writeCopy({ view, resetAt: master.resetAt, viewAt: Date.now() });
    /* ★ This page has just deleted its own overrides, so it is already in step with the master it
     *  wrote; record the stamp it caused so its NEXT load does not treat its own save as news. */
    try { localStorage.setItem(VIEW_EPOCH_KEY, String(Date.now())); } catch { /* private mode */ }
  }
  return !!r?.ok;
}

/** Keep the portable bookmark list in step with this page's own. */
export async function saveBookmarks(list: unknown[]): Promise<void> {
  if (!frame) return;
  const r = await ask({ op: 'setBookmarks', bookmarks: list });
  if (r?.ok) master.bookmarks = list;
}
