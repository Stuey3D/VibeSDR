/**
 * admin.ts — the SERVER ADMIN page.
 *
 * Reached from the button at the bottom of the menu, which appears only once this session is
 * unlocked as admin. Talks to /vibeserver/admin/* and nothing else.
 *
 * ★★★ IT IS A CLIENT OF A DOCUMENTED API, exactly as the setup page is a client of the config
 * API. Every rule lives on the server; this file only draws. That is what lets the VibeSDR app
 * grow its own admin screen later with no server-side work — and it is what stops a second,
 * drifting copy of the rules appearing in JavaScript.
 *
 * ★★ POLLING STARTS WHEN THE PAGE OPENS AND STOPS WHEN IT CLOSES. A monitoring endpoint polled
 * once a second by every listener's browser is a load problem we would have inflicted on
 * ourselves, on the very machine the page exists to keep an eye on.
 */
import { fetchAuthChallenge, vibeAuthToken } from './auth';
// ★ The client already knows how to turn an ISO code into a flag — it does it for RDS station
//   countries. Reuse it rather than growing a second copy that renders a different set.
import { isoToFlag } from '../../../src/services/rdsCountry';

let host = '';
/** Set by initAdmin so closeAdmin can stop the maintenance-log poll too. */
let stopFollowing: () => void = () => {};
let password = '';
let timer = 0;
let open = false;

const $ = (id: string) => document.getElementById(id)!;
const base = () => `http://${host}`;

/** ★ A FRESH NONCE PER REQUEST. The nonce is single-use on the server, so caching one would
 *  work exactly once and then fail in a way that looks like a wrong password. */
async function q(): Promise<string> {
  const { nonce } = await fetchAuthChallenge(base());
  if (!nonce) throw new Error('this server did not offer a challenge');
  return `vs_admin_nonce=${encodeURIComponent(nonce)}&vs_admin_auth=${vibeAuthToken(password, nonce)}`;
}
async function get(path: string): Promise<any> {
  const r = await fetch(`${base()}/vibeserver/admin/${path}?${await q()}`, { cache: 'no-store' });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}
async function post(path: string, body: unknown): Promise<any> {
  const r = await fetch(`${base()}/vibeserver/admin/${path}?${await q()}`,
                        { method: 'POST', body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

// ── Formatting ────────────────────────────────────────────────────────────────────────────────

const esc = (s: string) => s.replace(/[&<>"]/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!));

/** ★ Durations read as durations, not as seconds. "4327" is a number nobody can picture;
 *  "1h 12m" is a fact about an evening. */
function dur(sec: number): string {
  if (!isFinite(sec) || sec < 0) return '—';
  const d = Math.floor(sec / 86400), h = Math.floor(sec / 3600) % 24;
  const m = Math.floor(sec / 60) % 60, s = Math.floor(sec) % 60;
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${String(m).padStart(2, '0')}m`;
  if (m) return `${m}m ${String(s).padStart(2, '0')}s`;
  return `${s}s`;
}
function when(epoch: number): string {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000), now = Date.now() / 1000;
  const hhmm = d.toTimeString().slice(0, 5);
  // Within the day, the clock time is what an owner correlates against ("that was when I was
  // out"). Older than that and the date matters more than the minute.
  return now - epoch < 86400 ? hhmm : `${d.toISOString().slice(5, 10)} ${hhmm}`;
}
/** ★★ A flag, or NOTHING. An unknown country must render as absence — never as a globe, a
 *  question mark or a placeholder flag. The lookup is honest about not knowing (private
 *  addresses, unallocated space, a server that has not downloaded the data yet), and the UI must
 *  not paper over that with a symbol people will read as a real answer. */
const flag = (cc?: string) => (cc ? isoToFlag(cc) : '');
const withFlag = (cc: string | undefined, text: string) => {
  const f = flag(cc);
  return f ? `<span class="cc" title="${esc(cc!)}">${f}</span> ${esc(text)}` : esc(text);
};

const mhz = (hz: number) => hz > 0 ? `${(hz / 1e6).toFixed(3)} MHz` : '—';

// ── Health cards ──────────────────────────────────────────────────────────────────────────────

/** @param status one of the server's own "ok"/"warning"/"critical"/"unknown" verdicts.
 *  ★★ THE SERVER DECIDES, NOT THIS PAGE. The thresholds are relative to core count and to the
 *  Pi's own throttling points; duplicating that arithmetic here is how two answers to one
 *  question appear, and the one on screen is the one nobody can debug. */
function card(k: string, v: string, sub: string, status = 'ok'): string {
  const cls = status === 'critical' ? 'crit' : status === 'warning' ? 'warn'
            : status === 'unknown' ? 'none' : '';
  return `<div class="aCard ${cls}"><div class="k">${esc(k)}</div>`
       + `<div class="v">${esc(v)}</div><div class="s">${esc(sub)}</div></div>`;
}

function renderHealth(st: any) {
  const sys = st.sys ?? {};
  const out: string[] = [];

  // ★★★ USAGE FIRST, LOAD SECOND. Stuart, 2026-08-06: "I prefer and understand the usage more
  //     than load" — and he is right that a percentage is the number people actually read. Load
  //     stays because it is the one that catches a machine thrashing on I/O, where usage looks
  //     calm; but it is demoted to the subtitle where it belongs, with the core count beside it
  //     because "2.4" is meaningless without knowing there are 4 cores.
  //     ★ The CARD'S STATUS still comes from the 15-minute load, not from usage — see
  //       loadStatus() in vibe_admin.h. Usage is instantaneous and spikes to 100% every time the
  //       FFT runs, so alarming on it would light this card up permanently.
  out.push(typeof sys.cpuPct === 'number'
    ? card('CPU USAGE', `${sys.cpuPct.toFixed(0)}%`,
           sys.loadStatus === 'unknown'
             ? `${sys.cores ?? '?'} cores`
             : `load ${(sys.load1 ?? 0).toFixed(2)} / ${(sys.load5 ?? 0).toFixed(2)} / `
               + `${(sys.load15 ?? 0).toFixed(2)} · ${sys.cores} cores`,
           sys.loadStatus)
    : sys.loadStatus === 'unknown'
    ? card('CPU USAGE', 'not available', `${sys.cores ?? '?'} cores`, 'unknown')
    // ★ cpuPct needs two samples, so the very first poll after a restart has none. Fall back to
    //   load rather than showing "0%", which would be a confident lie about a busy machine.
    : card('CPU LOAD', `${(sys.load1 ?? 0).toFixed(2)}`,
           `${(sys.load5 ?? 0).toFixed(2)} / ${(sys.load15 ?? 0).toFixed(2)} · ${sys.cores} cores`,
           sys.loadStatus));

  // ★ ABSENT, not zero. A Mac and a container have no thermal zone, and "0 °C" would be a lie
  //   an owner might act on.
  out.push(sys.tempStatus === 'unknown'
    ? card('CPU TEMP', 'not available', 'no sensor on this machine', 'unknown')
    : card('CPU TEMP', `${sys.tempC.toFixed(1)}°C`,
           sys.tempStatus === 'ok' ? 'normal'
             : sys.tempStatus === 'warning' ? 'warm — check airflow'
             : 'throttling — check cooling', sys.tempStatus));

  if (sys.memTotalKB) {
    const usedPct = 100 * (1 - (sys.memAvailKB ?? 0) / sys.memTotalKB);
    out.push(card('MEMORY', `${usedPct.toFixed(0)}%`,
      `${(sys.memAvailKB / 1048576).toFixed(1)} GB free of ${(sys.memTotalKB / 1048576).toFixed(1)} GB`,
      usedPct > 92 ? 'critical' : usedPct > 80 ? 'warning' : 'ok'));
  }

  out.push(card('LISTENERS', `${st.listeners ?? 0}${st.maxUsers > 1 ? ` / ${st.maxUsers}` : ''}`,
    st.waiting ? `${st.waiting} waiting` : 'nobody waiting'));

  out.push(card('UPLINK', `${(st.txKbps ?? 0).toFixed(0)} kbps`,
    `${st.uniqueHour ?? 0} addresses this hour`));

  const radio = st.radio ?? {};
  out.push(card('RADIO', radio.present ? String(radio.driver).toUpperCase() : 'MISSING',
    radio.present ? `${mhz(radio.centreHz)} · ${(radio.spanHz / 1e6).toFixed(1)} MHz span`
                  : 'the receiver was unplugged or has failed',
    radio.present ? 'ok' : 'critical'));

  // ★★ THE FRONT END, because a gain problem is invisible on a waterfall — auto-contrast
  //    normalises it away, so the picture looks identical whether the radio is wide open or
  //    backed right off. This is the panel to look at when a decoder is misbehaving on a signal
  //    that "looks strong".
  //    ★ Not shown on radios that have no such controls (a plain dongle, an HF+): a card that
  //      says "—" for everything is worse than no card. Same rule as the gain sliders.
  // ★ Nested under `radio`, because that is whose front end it is. It is also where the server
  //   actually puts it — I first read it as `st.frontEnd` and the card silently never appeared,
  //   which is precisely the "drawn but inert" failure this project keeps re-learning. Caught
  //   only by cross-checking the API against what the receiver's own menu was showing.
  const fe = st.radio?.frontEnd;
  if (fe) {
    out.push(card('FRONT END', `${(fe.sysGainDb ?? 0).toFixed(1)} dB`,
      `LNA ${fe.lna}/${fe.lnaStates ?? '?'} · IF GR ${fe.ifGrDb} dB · `
      + `AGC ${fe.ifAgc ? 'auto' : 'fixed'}`,
      // ★ Overload is the only genuinely bad state here. A LOW gain is not a fault — it is the
      //   AGC doing its job against a strong band, and colouring it red would send an owner
      //   chasing a problem that is not there. I made exactly that mistake reading a log.
      fe.overload ? 'critical' : 'ok'));
  }

  if (sys.uptimeSec) out.push(card('UPTIME', dur(sys.uptimeSec),
    sys.governor ? `governor: ${sys.governor}` : ''));

  out.push(card('VISITORS', String(st.uniqueDay ?? 0), 'distinct addresses today'));

  $('adminHealth').innerHTML = out.join('');
}

// ── Graphs ────────────────────────────────────────────────────────────────────────────────────

/** ★ Inline SVG sparklines rather than a charting library. Three lines of history do not
 *  justify a dependency, and this page has to load on a phone over 4G from a field. */
function spark(rows: number[][], col: number, label: string, fmt: (n: number) => string): string {
  const vals = rows.map((r) => r[col]).filter((n) => isFinite(n));
  if (vals.length < 2) return `<figure><figcaption><span>${esc(label)}</span><span>—</span></figcaption>`
                            + `<svg viewBox="0 0 100 30" preserveAspectRatio="none"></svg></figure>`;
  const max = Math.max(...vals, 0.0001), min = Math.min(...vals, 0);
  const span = max - min || 1;
  const pts = vals.map((v, i) =>
    `${(i / (vals.length - 1)) * 100},${30 - ((v - min) / span) * 28}`).join(' ');
  const last = vals[vals.length - 1];
  return `<figure><figcaption><span>${esc(label)}</span><span>${esc(fmt(last))}</span></figcaption>`
       + `<svg viewBox="0 0 100 30" preserveAspectRatio="none">`
       + `<polyline points="${pts}" fill="none" stroke="var(--amber)" stroke-width="1"`
       + ` vector-effect="non-scaling-stroke"/></svg></figure>`;
}

function renderGraphs(h: any) {
  const rows: number[][] = h.rows ?? [];
  // fields: at, load1, tempC, listeners, kbps
  const parts = [
    spark(rows, 1, 'CPU LOAD', (n) => n.toFixed(2)),
    spark(rows, 3, 'LISTENERS', (n) => String(Math.round(n))),
    spark(rows, 4, 'UPLINK kbps', (n) => n.toFixed(0)),
  ];
  // Only draw temperature where there is one — an empty box is worse than no box.
  if (rows.some((r) => r[2] > 0)) parts.splice(1, 0, spark(rows, 2, 'CPU TEMP °C', (n) => n.toFixed(1)));
  $('adminGraphs').innerHTML = parts.join('');
}

// ── Where listeners come from ────────────────────────────────────────────────────────────────

/** ★★ A BAR PER COUNTRY, counted by DISTINCT ADDRESS over the last day (the server does the
 *  counting — see ConnLog::topCountriesJson for why connections would be the wrong unit).
 *  ★ Plain divs rather than a chart library: eight labelled bars do not justify a dependency,
 *    and this page may be opened over 4G from a field. */
function renderCountries(list: any[]) {
  const wrap = $('adminCountries');
  const none = $('adminNoCountries');
  if (!list.length) {
    // ★★ SAY WHY IT IS EMPTY. On a fresh install the country data has not downloaded yet, and an
    //    empty chart with no explanation reads as "nobody has ever connected" — a different and
    //    much more alarming statement than "we do not know yet".
    wrap.innerHTML = '';
    none.hidden = false;
    return;
  }
  none.hidden = true;
  const max = Math.max(...list.map((c) => c.n), 1);
  wrap.innerHTML = list.map((c) => `
    <div class="ccRow">
      <span class="ccFlag">${flag(c.cc)}</span>
      <span class="ccName">${esc(c.cc)}</span>
      <span class="ccBar"><i style="width:${(100 * c.n / max).toFixed(1)}%"></i></span>
      <span class="ccN">${c.n}</span>
    </div>`).join('');
}

// ── Listeners ─────────────────────────────────────────────────────────────────────────────────

function renderSessions(s: any) {
  const list: any[] = s.sessions ?? [];
  const tb = $('adminSessions').querySelector('tbody')!;
  $('adminNoSessions').hidden = list.length > 0;
  ($('adminSessions').parentElement as HTMLElement).hidden = list.length === 0;
  $('adminListenerCount').textContent = list.length ? `— ${list.length}` : '';
  tb.innerHTML = list.map((c) => `<tr>
    <td>${withFlag(c.cc, c.ip || '—')}</td>
    <td>${esc(mhz(c.vfoHz))}</td>
    <td>${esc(String(c.mode || '').toUpperCase())}${c.zoomed ? ' <span class="dim">zoom</span>' : ''}</td>
    <td>${esc(c.secs ? dur(c.secs) : '—')}</td>
    <td>${c.dropped > 0 ? esc(String(c.dropped)) : '<span class="dim">0</span>'}</td>
    <td class="agent">${c.net ? esc(c.net) : '<span class="dim">unknown</span>'}</td>
    <td>
      <button class="btn" data-kick="${esc(c.session)}">DISCONNECT</button>
      <button class="btn" data-ban="${esc(c.ip)}">BLOCK IP</button>
      ${c.net ? `<button class="btn" data-banasn="${esc(c.net.split(' ')[0])}"
                         data-netname="${esc(c.net)}">BLOCK NETWORK</button>` : ''}
    </td></tr>`).join('');
}

// ── Bans ──────────────────────────────────────────────────────────────────────────────────────

function renderBans(bans: any[]) {
  const tb = $('adminBans').querySelector('tbody')!;
  $('adminNoBans').hidden = bans.length > 0;
  ($('adminBans').parentElement as HTMLElement).hidden = bans.length === 0;
  tb.innerHTML = bans.map((b) => `<tr>
    <td>${esc(b.cidr)}${b.asn ? ' <span class="dim">whole network</span>' : ''}${b.valid === false
        ? ' <span class="dim">(not a valid address or AS number — never matches)</span>' : ''}</td>
    <td class="agent">${esc(b.reason || '—')}</td>
    <td>${esc(when(b.at))}</td>
    <td>${b.until ? esc(when(b.until)) : '<span class="dim">forever</span>'}</td>
    <td><button class="btn" data-unban="${esc(b.cidr)}">REMOVE</button></td>
  </tr>`).join('');
}

// ── Connection history ────────────────────────────────────────────────────────────────────────

/** ★★ The `Ended` column is the reason this table is worth keeping. "127 connections yesterday"
 *  tells an owner nothing; "41 of them ended in `banned`, all from one range" tells them what to
 *  do next. */
function renderConns(list: any[]) {
  const tb = $('adminConns').querySelector('tbody')!;
  $('adminNoConns').hidden = list.length > 0;
  ($('adminConns').parentElement as HTMLElement).hidden = list.length === 0;
  tb.innerHTML = list.map((c) => {
    const live = !c.end;
    return `<tr>
      <td>${esc(when(c.at))}</td>
      <td>${withFlag(c.cc, c.ip || '—')}</td>
      <td>${live ? '<span class="dim">now</span>' : esc(dur(c.end - c.at))}</td>
      <td class="why-${esc(c.reason || '')}">${live ? '<span class="dim">connected</span>'
                                                    : esc(c.reason || '—')}</td>
      <td class="agent">${esc((c.agent || '').slice(0, 60) || '—')}</td>
      <td><button class="btn" data-ban="${esc(c.ip)}">BLOCK</button></td>
    </tr>`;
  }).join('');
}

// ── The refresh cycle ─────────────────────────────────────────────────────────────────────────

let failures = 0;
async function refresh() {
  if (!open) return;
  try {
    const [st, ses, conns, hist] = await Promise.all([
      get('status'), get('sessions'), get('connections'), get('history'),
    ]);
    failures = 0;
    renderHealth(st);
    renderGraphs(hist);
    renderSessions(ses);
    renderBans(st.bans ?? []);
    renderCountries(st.countries ?? []);
    renderConns(conns.connections ?? []);
    $('adminHost').textContent = host;
  } catch (e) {
    // ★ SAY SO IN PLACE rather than silently freezing. A monitoring page that stops updating
    //   without a word is indistinguishable from a machine that has stopped having problems —
    //   which is the single most dangerous thing this screen could imply.
    if (++failures >= 2) {
      $('adminHost').textContent = `${host} — not responding (${(e as Error).message})`;
    }
  }
}

// ── Open / close ──────────────────────────────────────────────────────────────────────────────

export function openAdmin(currentHost: string, adminPassword: string) {
  host = currentHost;
  password = adminPassword;
  open = true;
  failures = 0;
  $('adminPanel').hidden = false;
  $('adminHost').textContent = host;
  void refresh();
  window.clearInterval(timer);
  // ★ 2 s, not 1. This page's whole job is to avoid loading the machine it is watching, and
  //   nothing here changes meaningfully faster than that — the load average itself is a
  //   one-minute mean.
  timer = window.setInterval(refresh, 2000);
}

export function closeAdmin() {
  open = false;
  stopFollowing();
  window.clearInterval(timer);
  timer = 0;
  $('adminPanel').hidden = true;
}

export function isAdminOpen() { return open; }

/** Wire the buttons once. The tables are re-rendered constantly, so their buttons are handled by
 *  DELEGATION — per-row listeners would leak on every refresh. */
export function initAdmin(getHost: () => string, getPassword: () => string) {
  $('btnServerAdmin')?.addEventListener('click', () => {
    const pw = getPassword();
    if (!pw) {
      // ★ Should not happen — the button only exists once unlocked — but the page needs the
      //   password itself to sign its own requests, so say why rather than opening a dead panel.
      alert('Enter the admin password in the menu first.');
      return;
    }
    document.getElementById('menu')?.classList.remove('show');
    openAdmin(getHost(), pw);
  });
  $('adminPanelClose')?.addEventListener('click', closeAdmin);
  // Escape closes it, like every other panel here.
  document.addEventListener('keydown', (e) => {
    if (open && e.key === 'Escape') closeAdmin();
  });

  const msg = (id: string, text: string) => { const el = $(id); el.textContent = text; };

  // ── Blocking ────────────────────────────────────────────────────────────────────────────
  $('banAdd')?.addEventListener('click', async () => {
    const cidr = ($('banCidr') as HTMLInputElement).value.trim();
    const reason = ($('banReason') as HTMLInputElement).value.trim();
    const minutes = Number(($('banFor') as HTMLSelectElement).value) || 0;
    if (!cidr) { msg('banMsg', 'Enter an address or a range.'); return; }
    try {
      const r = await post('ban', { cidr, reason, minutes });
      // ★ REPORT THE KICK. A ban that leaves the banned listener connected is not a ban, and
      //   saying "blocked, and disconnected 1 listener" is what proves it took effect.
      msg('banMsg', r.kicked > 0
        ? `Blocked ${cidr} and disconnected ${r.kicked} listener${r.kicked === 1 ? '' : 's'}.`
        : `Blocked ${cidr}. Nobody was connected from it.`);
      ($('banCidr') as HTMLInputElement).value = '';
      ($('banReason') as HTMLInputElement).value = '';
      void refresh();
    } catch (e) {
      msg('banMsg', (e as Error).message);
    }
  });

  // Delegated row actions, for both tables.
  document.getElementById('adminPanelBody')?.addEventListener('click', async (ev) => {
    const t = (ev.target as HTMLElement).closest('button');
    if (!t) return;
    const kick   = t.getAttribute('data-kick');
    const ban    = t.getAttribute('data-ban');
    const unban  = t.getAttribute('data-unban');
    const banAsn = t.getAttribute('data-banasn');
    try {
      if (kick) { await post('kick', { session: kick }); void refresh(); }
      else if (unban) { await post('unban', { cidr: unban }); msg('banMsg', `Removed ${unban}.`); void refresh(); }
      else if (banAsn) {
        // ★★★ THE BIGGEST BUTTON ON THIS PAGE, so it names what it will actually do. Blocking a
        //     network can block a whole ISP and everyone behind it — for a residential provider
        //     that is a town. The confirmation states the network by NAME, not just its number,
        //     because "AS5089" means nothing and "AS5089 Virgin Media" means everything.
        const name = t.getAttribute('data-netname') || banAsn;
        if (!confirm(`Block the entire network ${name}?\n\n`
                   + `This blocks EVERY address belonging to that network, not just this one. `
                   + `If it is a home internet provider, that may be a great many innocent `
                   + `listeners.\n\nUse it for hosting and proxy networks; think twice for an ISP.`)) return;
        const r = await post('ban', { cidr: banAsn, reason: `network blocked: ${name}`, minutes: 0 });
        msg('banMsg', `Blocked ${name}${r.kicked ? `, disconnected ${r.kicked}` : ''}.`);
        void refresh();
      }
      else if (ban) {
        // ★ CONFIRM, and say what it will do. This button sits in a table row next to a
        //   DISCONNECT, and the two are one careless click apart — but only one of them is
        //   remembered forever.
        if (!confirm(`Block ${ban} from this receiver?\n\n`
                   + `They will be disconnected now and refused in future, until you remove the `
                   + `block on this page.`)) return;
        const r = await post('ban', { cidr: ban, reason: 'blocked from the admin page', minutes: 0 });
        msg('banMsg', `Blocked ${ban}${r.kicked ? `, disconnected ${r.kicked}` : ''}.`);
        void refresh();
      }
    } catch (e) {
      msg('banMsg', (e as Error).message);
    }
  });

  // ── Maintenance ─────────────────────────────────────────────────────────────────────────
  // ★★ THE DESTRUCTIVE ONES ASK FIRST, AND NAME THE COST IN LISTENERS. "Reboot?" is a question
  //    about a machine; "reboot, disconnecting 6 listeners?" is a question about people, and it
  //    is the one the owner actually wants to be asked.
  /** ★★ FOLLOW THE ACTION'S OUTPUT until the server says it has finished.
   *  ★ Polled SEPARATELY from the main refresh, and faster: an apt run prints in bursts and the
   *    2 s page poll would make it look frozen between them. It stops itself when the action
   *    ends, so nothing keeps polling once there is nothing to watch. */
  let logTimer = 0;
  stopFollowing = () => { window.clearInterval(logTimer); logTimer = 0; };
  const followLog = () => {
    const box = $('actLog');
    window.clearInterval(logTimer);
    let idleRounds = 0;
    const tick = async () => {
      let j: any;
      try { j = await get('maintenance-log'); } catch { return; }
      const text = String(j.text ?? '');
      if (text) {
        box.hidden = false;
        const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;
        box.textContent = text;
        // ★ Only auto-scroll if the reader was already at the bottom — yanking the view while
        //   somebody is reading back through an error is worse than not scrolling at all.
        if (atBottom) box.scrollTop = box.scrollHeight;
      }
      if (j.running) { idleRounds = 0; return; }
      // ★★ Do not stop on the FIRST not-running reply. The helper is triggered by systemd
      //    noticing a file, so for a moment after the request there is no log and nothing
      //    running — stopping there would abandon the action just before it started.
      if (++idleRounds < 4 && !text) return;
      window.clearInterval(logTimer);
      logTimer = 0;
      const failed = Number(j.exitCode) !== 0;
      box.classList.toggle('failed', failed && !!text);
      box.classList.toggle('done', !failed);
      if (text) msg('actMsg', failed ? `Finished with errors (exit ${j.exitCode}).` : 'Finished.');
    };
    void tick();
    logTimer = window.setInterval(tick, 900);
  };

  const act = async (action: string, confirmText?: string) => {
    if (confirmText) {
      let n = 0;
      try { n = (await get('status')).listeners ?? 0; } catch { /* ask anyway */ }
      const who = n > 0 ? `\n\nThis will disconnect ${n} listener${n === 1 ? '' : 's'}.` : '';
      if (!confirm(confirmText + who)) return;
    }
    msg('actMsg', 'working…');
    const box = $('actLog');
    box.textContent = '';
    box.hidden = true;
    box.classList.remove('done', 'failed');
    try {
      await post('action', { action });
      followLog();
      // ★ Reboot and shutdown take the server away mid-action, so the follower will simply stop
      //   getting answers. That is not a failure and must not be reported as one.
      if (action === 'reboot' || action === 'shutdown') { window.clearInterval(logTimer); logTimer = 0; }
      msg('actMsg', action === 'reboot'
        ? 'Rebooting. This page will stop responding for a minute or two.'
        : action === 'restart'
        ? 'Restarting VibeServer. Reload this page in a few seconds.'
        : action === 'update'
        ? 'Installing updates. VibeServer will restart itself if a new version is fitted.'
        : 'Checking for updates.');
    } catch (e) {
      msg('actMsg', (e as Error).message);
    }
  };
  $('actUpdateCheck')?.addEventListener('click', () => act('update-check'));
  $('actUpdate')?.addEventListener('click', () => act('update', 'Install available updates?'));
  $('actRestart')?.addEventListener('click', () => act('restart', 'Restart VibeServer?'));
  $('actReboot')?.addEventListener('click', () => act('reboot', 'Reboot the whole machine?'));
}
