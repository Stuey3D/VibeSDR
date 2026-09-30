/**
 * adminLazy.ts — the admin panel's code, fetched when an admin opens the panel and not before.
 *
 * ★★★ WHY. admin.ts is ~41 KB of minified script (plus the map renderer it can pull in) that no
 *     listener ever runs — and every listener used to download it, on every link, because the page
 *     was one bundle (Stuart, 2026-09-30: "optimise the page first — that is our biggest issue for
 *     users on wank internet connections"). build-web.mjs splits each import() into its own file,
 *     so the panel now costs only the owner who opens it.
 * ★★ THE SAME FOUR CALLS main.ts ALWAYS MADE, with the same meaning — this is a doorway, not a
 *    second implementation:
 *      initAdmin      records the getters and wires the ONE control that exists before the code
 *                     does (the menu's SERVER ADMIN button); admin.ts's own initAdmin wires the
 *                     rest the moment it arrives, with these same getters.
 *      openAdmin      fetches, then opens — unless closeAdmin came in between (a re-lock while the
 *                     file was on its way must not open the panel afterwards).
 *      closeAdmin     closes it if it was ever loaded; there is nothing to close otherwise.
 *      startAdminTicketRenewal  fetches, then starts renewing.
 * ★ A fetch that fails (a dropped link) is said in the console and forgotten, so the next press
 *   tries again rather than being stuck with the failure.
 */
import { inAdminMode } from './adminticket';

type AdminModule = typeof import('./admin');
let mod: AdminModule | null = null;
let loading: Promise<AdminModule> | null = null;
let getters: [() => string, () => string] | null = null;
/** Set by openAdmin, cleared by closeAdmin: what the panel should be once the code has arrived. */
let wantOpen = false;

function load(): Promise<AdminModule> {
  if (mod) return Promise.resolve(mod);
  loading ??= import('./admin').then((m) => {
    mod = m;
    if (getters) m.initAdmin(getters[0], getters[1]);
    return m;
  }).catch((e) => {
    loading = null;
    console.error('[admin] the admin panel did not load — press again to retry', e);
    throw e;
  });
  return loading;
}

export function initAdmin(getHost: () => string, getPassword: () => string): void {
  if (getters) return;          // same guard as admin.ts: the first entry point wins
  getters = [getHost, getPassword];
  if (mod) { mod.initAdmin(getHost, getPassword); return; }
  // ★ Only until the code is here: admin.ts wires this same button itself once it has loaded, and
  //   two handlers would open the panel twice.
  document.getElementById('btnServerAdmin')?.addEventListener('click', () => {
    if (mod) return;
    const pw = getPassword();
    // ★ A ticket is enough on its own. Only refuse when there is NEITHER (as admin.ts does).
    if (!pw && !inAdminMode()) {
      alert('Enter the admin password in the menu first.');
      return;
    }
    document.getElementById('menu')?.classList.remove('show');
    openAdmin(getHost(), pw);
  });
}

export function openAdmin(currentHost: string, adminPassword: string): void {
  wantOpen = true;
  load().then((m) => { if (wantOpen) m.openAdmin(currentHost, adminPassword); }).catch(() => { /* said in load() */ });
}

export function closeAdmin(): void {
  wantOpen = false;
  mod?.closeAdmin();
}

export function startAdminTicketRenewal(): void {
  load().then((m) => m.startAdminTicketRenewal()).catch(() => { /* said in load() */ });
}
