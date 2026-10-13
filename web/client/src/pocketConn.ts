/**
 * ★★★ PORTABLE WI-FI on the served page (Stuart, 2026-10-10) — the menu's Network | Own Wi-Fi choice, the link's
 * figures beside it, and the "weak Wi-Fi" notice while listening. Words: src/services/pocketConnection.ts (shared with
 * the app). Whether the link is weak: the server (one rule).
 *
 * ★ Costs nothing anywhere else: ONE request at start. Unless the answer is "a pocket box, and you are on its own
 *   network", nothing is drawn and nothing is polled. On a pocket box: one small GET every 30 s while the page is visible.
 */
import {
  pocketHere, pocketWifiShown, pocketOnOwn, pocketOnNet, pocketLinkLine, pocketSwitchText,
  POCKET_WEAK_TITLE, POCKET_WEAK_BODY, POCKET_WEAK_SNOOZE_MS, POCKET_TRY_SEC,
  type PocketConn, type PocketChoice,
} from '../../../src/services/pocketConnection';

const URL_ = '/vibeserver/pocket/connection';
let conn: PocketConn | null = null;
let snoozedUntil = 0;
let switching = false;

const $ = (id: string) => document.getElementById(id);
const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!));

async function fetchConn(): Promise<PocketConn | null> {
  try {
    const r = await fetch(URL_, { cache: 'no-store' });
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

function render(): void {
  const row = $('pocketConn');
  if (!row || !conn) return;
  // ★★★ No Wi-Fi on the box, or the owner switched it off (a wired box, 2026-10-13): no chooser — every press would be
  //     refused. Still polled, so the row comes back the moment Wi-Fi is switched on again.
  if (!pocketWifiShown(conn)) { row.hidden = true; $('pocketWeak')?.remove(); return; }
  row.hidden = false;
  const own = pocketOnOwn(conn);
  // ★ The saved networks BY NAME (Stuart, 2026-10-11: "Stuey3D's iPhone | Meow! | Own WiFi"); an older server sends
  //   none, and keeps the NETWORK key.
  const nets = conn.nets ?? [];
  const holder = $('pcNets');
  if (holder) {
    const key = JSON.stringify(nets);
    if (holder.dataset.key !== key) {
      holder.dataset.key = key;
      holder.innerHTML = '';
      for (const n of nets) {
        const b = document.createElement('button');
        b.className = 'btn';
        b.textContent = n.ssid.toUpperCase();
        b.dataset.ssid = n.ssid;
        b.onclick = () => { void choose(n); };
        holder.appendChild(b);
      }
    }
    holder.querySelectorAll('button').forEach((b) => {
      const n = nets.find((x) => x.ssid === (b as HTMLElement).dataset.ssid);
      b.classList.toggle('on', !!n && (pocketOnNet(conn!, n) || conn!.trying === n.ssid));
    });
  }
  const netBtn = $('pcNet') as HTMLButtonElement | null;
  if (netBtn) netBtn.hidden = nets.length > 0;
  $('pcNet')?.classList.toggle('on', !own);
  $('pcOwn')?.classList.toggle('on', own);
  const ownBtn = $('pcOwn') as HTMLButtonElement | null;
  // ★ Never a dead control: with no hotspot of its own set up there is nothing to switch to — say so instead.
  if (ownBtn) ownBtn.hidden = !conn.apSet;
  const line = $('pcLine');
  if (line) line.textContent = pocketLinkLine(conn);
  const note = $('pcNote');
  if (note && !switching) {
    note.hidden = !(conn.advice === 'weak' || !conn.apSet);
    note.textContent = !conn.apSet
      ? "Set up this box's own hotspot (setup → Wi-Fi) to be able to switch to it."
      : 'Weak link — switching to Own Wi-Fi is recommended.';
  }
  weakNotice();
}

/** The notice while listening. Once per weak spell; "Not now" quietens it for 30 minutes. */
function weakNotice(): void {
  const id = 'pocketWeak';
  const show = !!conn && conn.advice === 'weak' && conn.apSet && !pocketOnOwn(conn) && !switching && Date.now() > snoozedUntil;
  const el = $(id);
  if (!show) { el?.remove(); return; }
  if (el) return;
  const b = document.createElement('div');
  b.id = id;
  b.style.cssText =
    'position:fixed;left:50%;top:16px;transform:translateX(-50%);z-index:9400;max-width:min(92vw,460px);' +
    'background:var(--ov-warn,rgba(40,10,0,0.94));color:#ffb833;border:1px solid rgba(255,120,0,0.6);' +
    'border-radius:8px;padding:10px 14px;font:13px ui-monospace,monospace;text-align:center;' +
    'box-shadow:0 4px 18px rgba(0,0,0,0.6)';
  b.innerHTML = `<b>${esc(POCKET_WEAK_TITLE)}</b><br><span style="opacity:0.8;font-size:12px">${esc(POCKET_WEAK_BODY)}</span>` +
    '<div style="margin-top:8px;display:flex;gap:8px;justify-content:center">' +
    '<button class="btn" id="pocketWeakGo">SWITCH TO OWN WI-FI</button><button class="btn" id="pocketWeakLater">NOT NOW</button></div>';
  document.body.appendChild(b);
  $('pocketWeakGo')!.onclick = () => { b.remove(); void choose('own'); };
  $('pocketWeakLater')!.onclick = () => { snoozedUntil = Date.now() + POCKET_WEAK_SNOOZE_MS; b.remove(); };
}

async function choose(to: PocketChoice): Promise<void> {
  if (!conn) return;
  const net = typeof to === 'object' ? to : null;
  if (net ? pocketOnNet(conn, net) : (to === 'own') === pocketOnOwn(conn)) return;   // already there
  const t = pocketSwitchText(conn, to);
  if (!window.confirm(`${t.title}\n\n${t.body}`)) return;
  const note = $('pcNote');
  try {
    const r = await fetch(URL_, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(net ? { to: 'net', rank: net.rank } : { to }) });
    if (!r.ok) {
      const j = await r.json().catch(() => ({} as any));
      if (note) { note.hidden = false; note.textContent = `Not switched — ${j.error || 'the box refused (' + r.status + ')'}.`; }
      return;
    }
    switching = true;
    $('pocketWeak')?.remove();
    if (note) {
      note.hidden = false;
      note.textContent = net
        ? `Looking for “${net.ssid}” for ${POCKET_TRY_SEC} s — turn it on now if it is a hotspot, then open this page `
          + 'again on a device connected to it.'
        : to === 'own'
        ? `Switching… join “${conn.apSsid || 'its own Wi-Fi'}” on this device, then open this page again.`
        : 'Switching… join your network on this device, then open this page again.';
    }
  } catch {
    if (note) { note.hidden = false; note.textContent = 'Not switched — the box did not answer.'; }
  }
}

export async function initPocketConn(): Promise<void> {
  conn = await fetchConn();
  if (!pocketHere(conn)) { conn = null; return; }           // ★ not a pocket box, or not on its network: nothing more
  $('pcNet')!.onclick = () => { void choose('network'); };
  $('pcOwn')!.onclick = () => { void choose('own'); };
  render();
  setInterval(async () => {
    if (document.hidden || switching) return;
    const c = await fetchConn();
    if (pocketHere(c)) { conn = c; render(); }
  }, 30000);
}
