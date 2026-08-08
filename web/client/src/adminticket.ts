/**
 * adminticket.ts — the browser's half of the cross-process admin ticket.
 *
 * ★★★ WHY IT IS NOT THE PASSWORD. The landing page runs on the FRONT DOOR, which owns no radio;
 *     every radio is a separate process with its own nonce store, so a credential proved at the
 *     door is meaningless at a radio (that is why the admin box could only ever answer "Server
 *     refused the connection"). The server mints a short-lived ticket instead — see
 *     vibe_admin_ticket.h — and the browser carries that from the door into whichever radio the
 *     owner picks.
 *
 * ★★ sessionStorage, NOT localStorage. This is a bearer credential: whoever holds it is admin
 *    until it expires. sessionStorage is scoped to this tab and this origin and is gone when the
 *    tab closes, while localStorage would leave an admin key on the machine indefinitely. It
 *    survives the one thing that has to work — navigating from the landing page into /r/<serial>/
 *    — because that is a same-origin, same-tab navigation.
 *
 * ★ Kept, not consumed. The PIN-style override is removed the moment it is used, which is right
 *   for a one-shot takeover; an admin SESSION has to survive moving between radios and reloading
 *   the page, so this one lives until it expires or the tab closes.
 */

const KEY = 'vsAdminTicket';

interface Stored { t: string; exp: number }

/** Store a freshly minted ticket. `ttlSec` comes from the server, not from us. */
export function saveAdminTicket(ticket: string, ttlSec: number): void {
  if (!ticket) return;
  // ★ Expire slightly EARLY. A ticket that is valid for another two seconds when we send it may
  //   be expired by the time the server checks it, and the failure looks like a wrong password.
  const exp = Date.now() + Math.max(0, ttlSec - 15) * 1000;
  try { sessionStorage.setItem(KEY, JSON.stringify({ t: ticket, exp } as Stored)); } catch { /* private mode */ }
}

/** The current ticket, or '' if there is none or it has expired. */
export function getAdminTicket(): string {
  try {
    const raw = sessionStorage.getItem(KEY);
    if (!raw) return '';
    const s = JSON.parse(raw) as Stored;
    if (!s?.t || typeof s.exp !== 'number') return '';
    if (Date.now() >= s.exp) { sessionStorage.removeItem(KEY); return ''; }
    return s.t;
  } catch { return ''; }
}

export function clearAdminTicket(): void {
  try { sessionStorage.removeItem(KEY); } catch { /* ignore */ }
}

/** True while this tab holds a usable admin ticket — i.e. ADMIN MODE. */
export function inAdminMode(): boolean { return getAdminTicket() !== ''; }

/** The query fragment to append to a URL, or '' when not admin. */
export function adminTicketQuery(): string {
  const t = getAdminTicket();
  return t ? `vs_admin_ticket=${encodeURIComponent(t)}` : '';
}
