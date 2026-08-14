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
import { httpBase } from './origin';
import { adminTicketQuery, inAdminMode, saveAdminTicket } from './adminticket';

let host = '';
/** Set by initAdmin so closeAdmin can stop the maintenance-log poll too. */
let stopFollowing: () => void = () => {};
let password = '';
let timer = 0;
let open = false;

const $ = (id: string) => document.getElementById(id)!;
const base = () => httpBase(host);

/** ★★★ THE MACHINE, NOT THIS RADIO. `host` carries the radio's own path prefix when the panel is
 *  opened from a receiver — the header reads "vibeserver.local:48000/r/DD52B980BE4946DA" — so
 *  building the fan-out on top of it produced /r/<serial>/r/<serial>/… , which 404s. Every merged
 *  list came back empty while the single-radio health cards carried on working, so the page said
 *  "Nobody is listening" directly beneath a LISTENERS card reading 1 (Stuart, 2026-08-08).
 *  ★ Strips one trailing /r/<serial>; on the front door there is none and this is a no-op. */
const machineBase = () => httpBase(host).replace(/\/r\/[^/]+\/?$/, '');

/** ★ A FRESH NONCE PER REQUEST. The nonce is single-use on the server, so caching one would
 *  work exactly once and then fail in a way that looks like a wrong password. */
async function q(): Promise<string> {
  // ★★★ A TICKET IS ADMIN PROOF TOO, and on this page it is often the ONLY proof there is. An
  //     owner who signed in at the landing page arrived here through a fresh page load, so the
  //     password they typed is long gone — it lived in a variable on a page that no longer exists.
  //     Every hardware control was correctly unlocked (the server had the ticket) while this panel
  //     insisted on a password nothing could supply, and the menu hides its password box once
  //     unlocked, so there was no way back in either (Stuart, 2026-08-08: "unlocked the radio
  //     controls but didnt unlock the admin controls ... no way of entering the password").
  const t = adminTicketQuery();
  if (t && !password) return t;
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

function renderHealth(st: any, perRadio: Array<{ radio: string; data: any }> = []) {
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
           // ★ Say what the number MEANS, and never blame cooling for something else. A warm chip
           //   is not a fault: the machine throttles at 80 °C and that is where the advice starts.
           sys.tempStatus === 'ok' ? 'normal — throttles at 80°C'
             : sys.tempStatus === 'warning' ? 'warm — approaching the 80°C throttle'
             : 'throttling at 80°C — check cooling and airflow', sys.tempStatus));

  if (sys.memTotalKB) {
    const usedPct = 100 * (1 - (sys.memAvailKB ?? 0) / sys.memTotalKB);
    out.push(card('MEMORY', `${usedPct.toFixed(0)}%`,
      `${(sys.memAvailKB / 1048576).toFixed(1)} GB free of ${(sys.memTotalKB / 1048576).toFixed(1)} GB`,
      usedPct > 92 ? 'critical' : usedPct > 80 ? 'warning' : 'ok'));
  }

  // ★★★ POWER, AND IT EARNS A CARD OF ITS OWN. An under-volting Pi does not announce itself: USB
  //     devices drop out, radio streams die, SD cards corrupt — and every one of those looks like a
  //     different fault. It cost an evening here, with three SDRs against a 600 mA default budget
  //     producing 66 over-current events, and the only record was dmesg, which nobody reads on an
  //     appliance in a cupboard (Stuart, 2026-08-08: "maybe put the under voltage warning in the
  //     admin page too").
  // ★★ SHOWN WHEN IT HAS EVER HAPPENED, not only while it is happening. By the time anyone opens
  //    this page the voltage has usually recovered, and "all fine" would be true of this instant
  //    and false about the machine.
  if (sys.underVoltage !== undefined) {
    const now = !!sys.underVoltage, ever = !!sys.underVoltageEver;
    out.push(card('POWER',
      now ? 'UNDER-VOLTAGE' : ever ? 'DIPPED' : 'OK',
      now  ? 'the supply cannot keep up — expect USB devices to drop out'
           : ever ? ('the supply has dipped since boot — suspect it before anything else'
                     // ★ Only raised when there is a problem to explain. `performance` is the
                     //   default for a good reason (ondemand costs ~25% of throughput, because the
                     //   DSP spreads over every core so each looks idle and the chip clocks down)
                     //   — but it also holds the clock high, and on a marginal supply that is a
                     //   lever the owner has. Saying so unprompted would just be noise.
                     + (sys.governor === 'performance'
                        ? ' · the CPU is pinned to performance, which draws more' : ''))
                  : 'supply voltage has stayed in range',
      now ? 'critical' : ever ? 'warning' : 'ok'));
  }

  // ★ The governor is already shown under UPTIME; the CLOCK is the number that says what the
  //   governor is actually DOING right now.
  if (st.sys?.cpuKHz > 0)
    out.push(card('CPU CLOCK', `${Math.round(st.sys.cpuKHz / 1000)} MHz`,
                  st.sys.governor ? `governor: ${st.sys.governor}` : ''));
  // ★★★ ONE CARD PER RADIO, NOT ONE CARD FOR "THE" RADIO. Health above this line is about the
  //     MACHINE — one CPU, one supply, one clock — but listeners, uplink and the radio itself
  //     belong to a process each. Read from a single status they were not merely incomplete but
  //     WRONG: the front door showed one radio's name beside another's listener limit, because
  //     the numbers came from whichever process happened to answer (Stuart, 2026-08-08: "you have
  //     the RTL stats at the top but not the SDRPlay or Airspy").
  // ★ Falls back to the single status when there is only one radio, so a one-radio server's page
  //   is exactly what it always was.
  const radios = perRadio.length ? perRadio : [{ radio: '', data: st }];

  // ★★ THE MACHINE'S TOTAL, not the biggest radio's. Reading one status made this card show
  //    whichever radio answered — "0 / 30" on a server that can actually take 32, because the
  //    Airspy and the dongle contribute one slot each (Stuart, 2026-08-08). Capacity is a property
  //    of the whole machine, and it is the number an owner sizes their uplink against.
  {
    const tot = radios.reduce((a, r) => ({
      now:  a.now  + (Number(r.data?.listeners) || 0),
      max:  a.max  + (Number(r.data?.maxUsers)  || 0),
      wait: a.wait + (Number(r.data?.waiting)   || 0),
      kbps: a.kbps + (Number(r.data?.txKbps)    || 0),
    }), { now: 0, max: 0, wait: 0, kbps: 0 });
    out.push(card('LISTENERS', `${tot.now}${tot.max ? ` / ${tot.max}` : ''}`,
      tot.wait ? `${tot.wait} waiting` : 'nobody waiting'));
    out.push(card('UPLINK', `${tot.kbps.toFixed(0)} kbps`,
      `${st.uniqueHour ?? 0} addresses this hour`));
  }
  for (const r of radios) {
    const d = r.data ?? {};
    const radio = d.radio ?? {};
    const name = r.radio || (radio.present ? String(radio.driver).toUpperCase() : 'RADIO');
    out.push(card(name,
      radio.present ? `${d.listeners ?? 0}${d.maxUsers > 1 ? ` / ${d.maxUsers}` : ''} listening`
                    : 'MISSING',
      radio.present
        ? `${mhz(radio.centreHz)} · ${(radio.spanHz / 1e6).toFixed(1)} MHz span`
          + ` · ${(d.txKbps ?? 0).toFixed(0)} kbps`
          + (d.waiting ? ` · ${d.waiting} waiting` : '')
        : 'the receiver was unplugged or has failed',
      radio.present ? 'ok' : 'critical'));
  }

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
  // fields: at, load1, tempC, listeners, kbps, mhz
  const parts = [
    spark(rows, 1, 'CPU LOAD', (n) => n.toFixed(2)),
    spark(rows, 3, 'LISTENERS', (n) => String(Math.round(n))),
    spark(rows, 4, 'UPLINK kbps', (n) => n.toFixed(0)),
  ];
  // Only draw temperature where there is one — an empty box is worse than no box.
  if (rows.some((r) => r[2] > 0)) parts.splice(1, 0, spark(rows, 2, 'CPU TEMP °C', (n) => n.toFixed(1)));
  // ★★ THE CLOCK IS WHERE A SAGGING SUPPLY BECOMES VISIBLE. Temperature stays fine and load looks
  //    ordinary while the governor quietly runs the machine slower, so the dip only shows here.
  //    Drawn next to the load so the two can be read together.
  //    ★ Older servers send five columns; index 5 is simply absent and no box is drawn.
  if (rows.some((r) => r.length > 5 && r[5] > 0))
    parts.splice(1, 0, spark(rows, 5, 'CPU CLOCK MHz', (n) => n.toFixed(0)));
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


/**
 * What connected, in two words. The raw User-Agent is long and mostly boilerplate, and an owner
 * scanning a listener list wants "is this a person, and which of my apps" — not a version string.
 * ★ VibeSDR Jr MUST be tested first: its agent ("VibeSDR Jr/1.0 (watchOS)") contains "VibeSDR",
 *   so the obvious order silently counts every watch as a phone.
 * ★ An empty agent is normal, not suspicious: a browser cannot set one on a WebSocket.
 */
export function clientKind(agent: string | undefined): string {
  const a = (agent || '').trim();
  if (!a) return 'browser';
  if (/vibesdr\s*jr/i.test(a)) return 'VibeSDR Jr';
  if (/vibesdr/i.test(a)) return 'VibeSDR';
  if (/mozilla|safari|chrome|firefox|edg\//i.test(a)) return 'browser';
  // ★ OUR OWN PROBES, named so they are not mistaken for visitors. tools/vibeserver-probes send
  //   this; before they did, Node's bare `User-Agent: node` filed them under "other" and they
  //   quietly inflated the connection history on the public demo.
  if (/vibesdr-probe/i.test(a)) return 'probe';
  if (/bot|crawl|spider|curl|wget|python|scan|^node$|undici/i.test(a)) return 'bot';
  return 'other';
}

function clientLabel(agent: string | undefined): string {
  const k = clientKind(agent);
  const raw = (agent || '').slice(0, 90);
  // ★ The full agent stays available on hover — the short label is for scanning, not for hiding.
  return k === 'browser' && !raw
    ? '<span class="dim">browser</span>'
    : `<span title="${esc(raw)}">${esc(k)}</span>`;
}


// ── The whole machine, not one radio ─────────────────────────────────────────────────────────
//
// ★★★ AN OWNER ASKS ABOUT THE SERVER, NOT ABOUT A PROCESS. With a radio per process, opening the
//     admin page from the RTL showed only the RTL — "no point me being on the RTL and not seeing
//     what is going on with the airspy or SDRplay" (Stuart, 2026-08-08). Every radio's admin API
//     is reachable through the front door at /r/<serial>/…, and the admin TICKET is valid across
//     processes, so this needs no new server plumbing: ask each one and merge.
// ★★ One slow or dead radio must not blank the page. Each is fetched independently and a failure
//    contributes nothing rather than throwing the whole refresh away.
let radioList: Array<{ serial: string; label: string }> = [];

async function discoverRadios(): Promise<void> {
  try {
    const r = await fetch(`${machineBase()}/vibeserver/radios`, { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    radioList = (Array.isArray(j?.radios) ? j.radios : [])
      .map((x: any) => ({ serial: String(x.serial || ''), label: String(x.label || x.serial || '') }))
      .filter((x: { serial: string }) => x.serial);
  } catch { /* one radio, or an older server: fall back to just this process */ }
}

/** Ask every radio the same question at once, tagging each answer with the radio it came from. */
async function fromEveryRadio(path: string): Promise<Array<{ radio: string; data: any }>> {
  if (!radioList.length) {
    try { return [{ radio: '', data: await get(path) }]; } catch { return []; }
  }
  const q = await qCached();
  const out = await Promise.all(radioList.map(async (r) => {
    try {
      const resp = await fetch(`${machineBase()}/r/${encodeURIComponent(r.serial)}/vibeserver/admin/${path}?${q}`,
                               { cache: 'no-store' });
      if (!resp.ok) return null;
      return { radio: r.label, data: await resp.json() };
    } catch { return null; }
  }));
  return out.filter(Boolean) as Array<{ radio: string; data: any }>;
}

/**
 * ★★★ THE FAN-OUT MUST CARRY A TICKET, NEVER A NONCE. q() fetches a nonce from THIS process and
 *     signs it; every other radio is a different process with its own nonce store and rejects it
 *     out of hand. So a panel opened from a radio with the menu password could talk to that radio
 *     and to nothing else — the merged view would be silently one radio wide.
 * ★ If there is no ticket yet (opened with a password rather than from the landing page), mint one
 *   here: the admin-ticket endpoint takes ordinary admin proof, and what it returns every process
 *   on the machine accepts. One credential, one poll.
 */
async function qCached(): Promise<string> {
  const have = adminTicketQuery();
  if (have) return have;
  try {
    const r = await fetch(`${base()}/vibeserver/admin-ticket?${await q()}`, { cache: 'no-store' });
    if (r.ok) {
      const j = await r.json();
      saveAdminTicket(String(j.ticket || ''), Number(j.ttl) || 600);
      const t = adminTicketQuery();
      if (t) return t;
    }
  } catch { /* fall through — a single-radio server needs no ticket at all */ }
  return q();
}

// ── Listeners ─────────────────────────────────────────────────────────────────────────────────

function renderQueue(list: any[]) {
  const el = document.getElementById('adminQueue');
  if (!el) return;
  if (!list.length) { el.innerHTML = ''; el.hidden = true; return; }
  el.hidden = false;
  // ★ Ordered by position, because the order IS the promise the server makes to them.
  el.innerHTML = '<div class="qHead">WAITING</div>' + list
    .slice()
    .sort((a, b) => (a.pos || 0) - (b.pos || 0))
    .map((w) => `<div class="qRow"><b>${esc(String(w.pos ?? '?'))}</b> `
              + `${withFlag(w.cc, w.ip || '—')}`
              + `${w.radio ? ` <span class="dim">for ${esc(w.radio)}</span>` : ''}`
              + ` <span class="dim">waiting ${esc(dur(w.secs || 0))}</span></div>`).join('');
}

function renderVisitors(list: any[], busyIps: Set<string>) {
  const el = document.getElementById('adminVisitors');
  if (!el) return;
  // ★ Somebody LISTENING is not "sitting on the landing page" — their own tab may still be
  //   polling it. Showing them twice would inflate what the owner reads as demand.
  const idle = list.filter((v) => !busyIps.has(String(v.ip)));
  if (!idle.length) { el.innerHTML = ''; el.hidden = true; return; }
  el.hidden = false;
  el.innerHTML = '<div class="qHead">ON THE LANDING PAGE</div>' + idle
    .map((v) => `<div class="qRow">${withFlag(v.cc, v.ip || '—')} `
              + `<span class="dim">choosing a radio · seen ${esc(dur(v.secs || 0))} ago</span></div>`)
    .join('');
}

function renderSessions(s: any) {
  renderQueue(s.queue ?? []);
  renderVisitors(s.visitors ?? [],
                 new Set<string>((s.sessions ?? []).map((c: any) => String(c.ip))));
  const list: any[] = s.sessions ?? [];
  const tb = $('adminSessions').querySelector('tbody')!;
  $('adminNoSessions').hidden = list.length > 0;
  ($('adminSessions').parentElement as HTMLElement).hidden = list.length === 0;
  $('adminListenerCount').textContent = list.length ? `— ${list.length}` : '';
  // ★ Costs are a rate from two samples, so the FIRST poll after connecting has nothing to show.
  //   A dash is the honest answer there; a 0 would read as "this listener is free".
  const rate = (v: any, unit: string) =>
    typeof v === 'number' && v >= 0 ? `${esc(String(v))}<span class="dim">${unit}</span>`
                                    : '<span class="dim">—</span>';
  tb.innerHTML = list.map((c) => `<tr>
    <td>${withFlag(c.cc, c.ip || '—')}</td>
    <td>${c.radio ? esc(c.radio) : '<span class="dim">—</span>'}</td>
    <td>${esc(mhz(c.vfoHz))}</td>
    <td>${esc(String(c.mode || '').toUpperCase())}${c.zoomed ? ' <span class="dim">zoom</span>' : ''}</td>
    <td>${c.decoder ? esc(String(c.decoder).toUpperCase()) : '<span class="dim">—</span>'}</td>
    <td>${esc(c.secs ? dur(c.secs) : '—')}</td>
    <td title="Share of ONE core: 100% = a core fully busy keeping up with this listener's stream. Not a share of the whole machine — the HEALTH card above is that.">${rate(c.cpu, '% core')}</td>
    <td>${rate(c.kbps, 'k')}</td>
    <td>${c.dropped > 0 ? esc(String(c.dropped)) : '<span class="dim">0</span>'}</td>
    <td class="agent">${clientLabel(c.agent)}${c.admin
      // ★ Named, not coloured-only: an owner scanning this table for a session that should not be
      //   admin needs a word, and colour alone does not survive a screenshot in a bug report.
      ? ' <span class="adminTag" title="This session has unlocked the admin password — exempt from the session limit and able to change the radio">ADMIN</span>'
      : ''}</td>
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


/**
 * ★★ WHICH CLIENTS PEOPLE ACTUALLY USE. Stuart, 2026-08-08: "be interesting to know how many
 *    people connect with VibeSDR and VibeSDR Jr". Counted over the connection history the page
 *    already has, and by DISTINCT ADDRESS rather than by connection — one person who reconnects
 *    forty times through a flaky link is one user of that client, and counting sessions would say
 *    they were forty.
 * ★ The sample size is stated, because this is "the last N connections on record", not all time,
 *   and a percentage with no denominator invites exactly the wrong conclusion.
 */
function renderClientMix(list: any[]) {
  const el = document.getElementById('adminClientMix');
  if (!el) return;
  if (!list.length) { el.textContent = ''; return; }
  const seen = new Map<string, Set<string>>();
  for (const c of list) {
    const k = clientKind(c.agent);
    if (!seen.has(k)) seen.set(k, new Set());
    seen.get(k)!.add(String(c.ip || '?'));
  }
  const rows = [...seen.entries()]
    .map(([k, ips]) => ({ k, n: ips.size }))
    .sort((a, b) => b.n - a.n);
  const people = rows.reduce((t, r) => t + r.n, 0);
  el.innerHTML = rows.map((r) =>
    `<span class="mixItem"><b>${esc(String(r.n))}</b> ${esc(r.k)}</span>`).join('')
    + `<span class="dim"> — distinct addresses across the last ${esc(String(list.length))} `
    + `connection${list.length === 1 ? '' : 's'} on record`
    + (people ? '' : '') + `</span>`;
}

// ── Connection history ────────────────────────────────────────────────────────────────────────

/** ★★ The `Ended` column is the reason this table is worth keeping. "127 connections yesterday"
 *  tells an owner nothing; "41 of them ended in `banned`, all from one range" tells them what to
 *  do next. */
/** ★★ A PAGE AT A TIME. The log keeps hundreds of rows — 406 on this Pi after an afternoon — and
 *  rendering them all put the maintenance buttons a very long scroll below the thing an owner came
 *  to look at (Stuart, 2026-08-08: "maybe make pages here so that we dont end up scrolling for
 *  ages"). The newest are what matter, so page 1 is the newest.
 *  ★ The page number survives a refresh: this panel repaints every two seconds, and a list that
 *    jumped back to page 1 under you would be unusable for reading anything but the top. */
let connPage = 0;
const CONNS_PER_PAGE = 100;

/** ★★★ ONE ROW PER VISIT, LISTING EVERY RADIO THEY TRIED.
 *
 *  A visitor who looks at two receivers is ONE person having a look round, not two connections —
 *  but each radio is its own process and logs its own row, so the table showed them twice. Now
 *  that a visit carries one session id across radios (see visitSessionId in main.ts), those rows
 *  can be folded back into the single event they describe.
 *
 *  Stuart, 2026-08-11: "if I go into the Airspy and then use the back button and then switch to
 *  the SDRPlay that should only count 1 session… also if they do use multiple radios then show
 *  both."
 *
 *  ★★ SPAN, NOT SUM. The duration shown is first-open to last-close, because that is how long the
 *     person was here; adding the per-radio durations would double-count the moment they overlap
 *     and would read as longer than the visit actually was.
 *  ★★ ROWS WITH NO SESSION ID ARE NEVER GROUPED. Those are refusals logged before a session
 *     existed, and two of them from one address are two events — that is what a scan looks like,
 *     and folding them together would hide exactly what the log is for.
 *  ★ Radios are listed in the order they were VISITED, deduped: "Airspy HF+ → SDRplay RSP1B"
 *    tells a story that a set of labels does not.
 */
/**
 * ★★★ A SESSION ID IS NOT A VISIT, AND A GAP IS WHAT SEPARATES THEM.
 *
 *     This merged every leg sharing a session id however far apart they were, and the visit's
 *     duration is the SPAN from first open to last close — so a browser tab left open, reconnecting
 *     for a minute every few minutes, became ONE visit whose clock ran across all the idle time
 *     between. Measured on the demo, 2026-08-12: 195.180.34.4 logged twelve legs of 20-61 seconds
 *     over an hour and forty; about NINE MINUTES on the radio, displayed as 1h39 and still rising
 *     while every leg read "closed". Stuart saw 2h15 on a server with a 30-MINUTE LIMIT, which
 *     makes the limit itself look broken — and it was working perfectly.
 *     ★ It also explains rows "disappearing and reappearing": each new leg rewrites the visit it
 *       is folded into, so a row moves and its times change under you.
 *
 * ★★ THE SPAN IS STILL RIGHT INSIDE A VISIT — do not be tempted to sum the legs. A visit is
 *    normally TWO CONCURRENT SOCKETS (spectrum and audio); summing them would double every
 *    duration, which is the bug this rule was written to avoid in the first place.
 *
 * ★ So: same session AND resuming within kVisitGapSec of the last leg ending. A reconnect across a
 *   network blip comes back in seconds; two minutes of nothing is someone who left and returned.
 *   The server's own idle grace is 300 s, so this is deliberately tighter — it describes a PERSON
 *   coming back, not a radio being released.
 */
const VISIT_GAP_SEC = 120;

function groupVisits(list: any[]): any[] {
  const bySession = new Map<string, any>();
  const out: any[] = [];
  for (const c of list) {
    const sess = String(c.session || '');
    if (!sess) { out.push({ ...c, radioLegs: c.radio ? [{ radio: c.radio, at: c.at || 0 }] : [] }); continue; }
    const had = bySession.get(sess);
    // ★★★ TESTED IN BOTH DIRECTIONS, BECAUSE THE LOG ARRIVES NEWEST FIRST. My first cut only asked
    //     "does this leg START long after the visit ENDED" — which is never true when the list runs
    //     backwards in time, so the rule silently did nothing and the 2h32 row survived the fix
    //     (Stuart: "nope, rebooted and reloaded"). ConnLog::json() says "Newest first, capped" and
    //     it means it. The min/max merge below is order-agnostic; the gap test has to be too.
    // ★ `!had.end` is a LIVE leg — never split from it: the visit is still happening.
    const gapped = had && (
         (!!had.end && (c.at || 0) - had.end > VISIT_GAP_SEC)     // arrives long after it ended
      || (!!c.end && (had.at || 0) - c.end > VISIT_GAP_SEC));     // ended long before it began
    if (!had || gapped) {
      const v = { ...c, radioLegs: c.radio ? [{ radio: c.radio, at: c.at || 0 }] : [] };
      bySession.set(sess, v);
      out.push(v);
      continue;
    }
    had.at = Math.min(had.at || 0, c.at || 0);
    // ★ A still-open leg (end 0) wins: the visit is LIVE, however many closed legs precede it.
    had.end = (!had.end || !c.end) ? 0 : Math.max(had.end, c.end);
    had.bytes = (had.bytes || 0) + (c.bytes || 0);
    // The most recent ending is the one worth showing — "banned" after two clean legs is the news.
    if (c.end && c.end >= (had.end || 0)) had.reason = c.reason;
    // ★★★ IN THE ORDER IT HAPPENED, NOT THE ORDER IT ARRIVED. The log is NEWEST FIRST, so pushing
    //     as we iterate built the chain BACKWARDS: a visit that went RSP1B → Airspy was shown as
    //     "Airspy HF+ → SDRplay RSP1B" (Stuart, 2026-08-13). The same newest-first trap as the gap
    //     test twenty lines above, in the next statement along — which is exactly why the fix
    //     there is written to be order-agnostic rather than to assume a direction.
    // ★ Each hop keeps its own timestamp and the chain is sorted at render, so it reads correctly
    //   whichever way the server hands us the legs.
    if (c.radio && !had.radioLegs.some((x: any) => x.radio === c.radio)) {
      had.radioLegs.push({ radio: c.radio, at: c.at || 0 });
    }
    if (!had.agent && c.agent) had.agent = c.agent;
    if (!had.cc && c.cc) had.cc = c.cc;
  }
  return out;
}

function renderConns(raw: any[]) {
  const list = groupVisits(raw);
  renderClientMix(list);
  const tb = $('adminConns').querySelector('tbody')!;
  $('adminNoConns').hidden = list.length > 0;
  ($('adminConns').parentElement as HTMLElement).hidden = list.length === 0;

  const pages = Math.max(1, Math.ceil(list.length / CONNS_PER_PAGE));
  if (connPage >= pages) connPage = pages - 1;      // the log shrank under us
  const from = connPage * CONNS_PER_PAGE;
  const shown = list.slice(from, from + CONNS_PER_PAGE);
  const pager = document.getElementById('adminConnsPager');
  if (pager) {
    pager.hidden = pages < 2;
    if (pages > 1) {
      pager.innerHTML =
        `<button class="btn" data-conn="prev"${connPage === 0 ? ' disabled' : ''}>◀ newer</button>`
        + `<span class="dim"> ${from + 1}–${Math.min(from + CONNS_PER_PAGE, list.length)} `
        + `of ${list.length} </span>`
        + `<button class="btn" data-conn="next"${connPage >= pages - 1 ? ' disabled' : ''}>older ▶</button>`;
      Array.from(pager.querySelectorAll('button')).forEach((b) => {
        (b as HTMLButtonElement).onclick = () => {
          connPage += b.getAttribute('data-conn') === 'next' ? 1 : -1;
          connPage = Math.max(0, Math.min(pages - 1, connPage));
          // ★ RE-RENDER FROM THE RAW LIST, not the grouped one. Grouping is not idempotent —
          //   a second pass would reset each visit's `radios` to the single `radio` it was built
          //   from, so paging through the log would quietly lose the second receiver.
          renderConns(raw);
        };
      });
    }
  }

  tb.innerHTML = shown.map((c) => {
    const live = !c.end;
    return `<tr>
      <td>${esc(when(c.at))}</td>
      <!-- ★★ THE NETWORK, which the LIVE table has always shown and this one never did — because
           it was never RECORDED. The country was, as a SNAPSHOT taken when the connection opened,
           so a row kept whatever GeoIP said at that moment even after the database knew better:
           a work laptop stayed logged in the US while the live table had it right (Stuart,
           2026-08-14). Both are derived at RENDER time now, from the stored address, exactly as
           the live table derives them. One derivation, two tables.
           ★ Dimmed and on its own line: it is context for the address above it, not a column
             anyone scans. A blank means the ASN database has no entry for that range — which is
             itself worth seeing, since it usually means a VPN or corporate egress. -->
      <td>${withFlag(c.cc, c.ip || '—')}${c.admin ? ' <span class="adminTag" title="This session used the admin password">ADMIN</span>' : ''}${
        c.net ? `<div class="dim cNet" title="Network this address belongs to. A corporate VPN or cloud egress will show its provider rather than a consumer ISP — and that is usually why a country looks wrong.">${esc(String(c.net).slice(0, 40))}</div>` : ''}</td>
      <!-- ★ WHICH RECEIVER THEY CHOSE. The fan-out already tagged every record with the radio it
           came from; it was meaningless while each radio answered with the whole machine's history
           (see the per-radio log path in main.cpp) and is worth showing now that it is true.
           ★ Stuart wanted it to see which radio is the most popular — so it is a plain label, not
             an id: a column of hex would answer nothing at a glance. -->
      <td class="cRadio">${esc(
        (c.radioLegs && c.radioLegs.length
          ? [...c.radioLegs].sort((a: any, b: any) => (a.at || 0) - (b.at || 0))
              .map((x: any) => x.radio).join(' \u2192 ')
          : c.radio) || '—')}</td>
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
let lastRenew = 0;

/**
 * Keep the admin lease alive for as long as the tab holds one — REGARDLESS OF THIS PANEL.
 *
 * ★★★ RENEWAL USED TO LIVE INSIDE refresh(), WHICH RETURNS IMMEDIATELY WHEN THE PANEL IS CLOSED.
 *     So the ticket lapsed after ten minutes of ordinary listening, and the owner met the exact
 *     symptom Stuart reported: the menu says UNLOCKED — correctly, because the SOCKET unlock is
 *     still valid — while the admin button demands a password, and there is no box to type it in
 *     because being "unlocked" hid it. Only a page refresh recovered it.
 * ★★★ TWO CREDENTIALS, TWO LIFETIMES, AND THE UI ASSUMED ONE IMPLIED THE OTHER. The socket unlock
 *     lasts until the idle re-lock; the PAGE ticket lasts ten minutes. They are proved against
 *     different processes and they expire independently — see adminticket.ts.
 * ★★ A ticket renews ITSELF (the route gates on adminOkFor(), which accepts one as proof), so this
 *    needs no password and no second challenge. Four minutes against a ten-minute lease: two
 *    misses still leave a margin.
 */
export function startAdminTicketRenewal(): void {
  setInterval(() => {
    const q = adminTicketQuery();
    if (!q || Date.now() - lastRenew < 4 * 60 * 1000) return;
    lastRenew = Date.now();
    // ★ base(), not a reconstructed URL: this module already knows where the admin routes live
    //   (initAdmin sets the host), and a second derivation is a second thing to drift.
    void fetch(`${base()}/vibeserver/admin-ticket?${q}`, { cache: 'no-store' })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { if (j?.ticket) saveAdminTicket(String(j.ticket), Number(j.ttl) || 600); })
      .catch(() => { /* a blip must not end the session */ });
  }, 30_000);
}

async function refresh() {
  if (!open) return;
  try {
    if (!radioList.length) await discoverRadios();
    const [st, hist, sesAll, connsAll] = await Promise.all([
      get('status'), get('history'), fromEveryRadio('sessions'), fromEveryRadio('connections'),
    ]);
    // ★ The machine's health comes from whichever process we asked (they all read the same /proc);
    //   the per-RADIO numbers have to come from each radio.
    const statAll = await fromEveryRadio('status');
    // Merge, tagging every row with the radio it belongs to.
    const ses = {
      sessions: sesAll.flatMap((r) => (r.data?.sessions ?? []).map((x: any) => ({ ...x, radio: r.radio }))),
      queue:    sesAll.flatMap((r) => (r.data?.queue ?? []).map((x: any) => ({ ...x, radio: r.radio }))),
      // ★ Visitors are a property of the MACHINE (they are on the front door's landing page), not
      //   of a radio — so dedupe by address rather than showing the same person once per radio.
      visitors: Object.values(sesAll.flatMap((r) => r.data?.visitors ?? [])
        .reduce((acc: any, v: any) => { const k = String(v.ip);
          if (!acc[k] || v.secs < acc[k].secs) acc[k] = v; return acc; }, {})),
    };
    const conns = {
      connections: connsAll
        .flatMap((r) => (r.data?.connections ?? []).map((x: any) => ({ ...x, radio: r.radio })))
        .sort((a: any, b: any) => (b.at || 0) - (a.at || 0)),
    };
    failures = 0;
    renderHealth(st, statAll);
    renderGraphs(hist);
    // ★★★ SIMPLE vs FULL. The gated panels are about MANAGING STRANGERS — who is connected,
    //     blocking them, where they came from. On a household receiver they are noise, and
    //     plug-and-play is the thing people already like about VibeServer (Stuart, 2026-08-07:
    //     "Simple mode — recommended for local sharing only").
    //     ★★ HIDDEN, NOT SWITCHED OFF. The server keeps logging and keeps enforcing bans either
    //        way, so turning Full on later arrives with history already there instead of starting
    //        from nothing. That is a display decision; not collecting would be a different
    //        product — and would make the switch feel broken.
    //     ★ HEALTH AND MAINTENANCE STAY IN BOTH. "Your Pi is at 82 °C" matters whether one person
    //       is listening or thirty, and updating from a browser beats SSH more for a simple user,
    //       not less. Session limits and the queue also stay: they exist today, and taking away
    //       something people already have is far worse than never adding it.
    const full = st.publicSharing === true;
    for (const id of ['secListeners', 'secBlocking', 'secHistory', 'secCountries']) {
      const el = document.getElementById(id);
      if (el) el.hidden = !full;
    }
    // ★★★ ONLY DRAW WHAT THIS PLATFORM CAN ACTUALLY DO. The server advertises its maintenance
    //     actions; anything not listed is not shown. On macOS a reboot stops at the FileVault
    //     login and needs someone physically present, and on Android the USB radio is not
    //     re-detected until it is replugged — so on those platforms these buttons would not just
    //     be inert, they would STRAND the receiver (Stuart, 2026-08-07).
    //     ★ Per-BUTTON, not just per-section, so a platform can offer "restart" without "reboot".
    {
      const offered = String(st.maintenance ?? '').split(',').filter(Boolean);
      const sec = document.getElementById('secMaintenance');
      if (sec) sec.hidden = offered.length === 0;
      for (const [id, act] of [['actUpdateCheck', 'update-check'], ['actUpdate', 'update'],
                               ['actUpdateAll', 'update-all'],
                               ['actRestart', 'restart'], ['actReboot', 'reboot']] as const) {
        const el = document.getElementById(id);
        if (el) el.hidden = !offered.includes(act);
      }
      // ★ The scheduler is only meaningful where an update action exists to schedule.
      const sched = document.getElementById('updSchedRow');
      const schedAll = document.getElementById('updSchedAllRow');
      const snote = document.getElementById('updNote');
      const canUpdate = offered.includes('update') || offered.includes('update-all');
      if (sched) sched.hidden = !offered.includes('update');
      if (schedAll) schedAll.hidden = !offered.includes('update-all');
      if (snote) snote.hidden = !canUpdate;

    }

    const note = document.getElementById('adminSimpleNote');
    if (note) note.hidden = full;

    // ★ Only render what is on screen. Not an optimisation for its own sake — rendering into a
    //   hidden panel is how a stale table survives a mode change and reappears wrong.
    if (full) {
      renderSessions(ses);
      renderBans(st.bans ?? []);
      renderCountries(st.countries ?? []);
      renderConns(conns.connections ?? []);
    }
    // ★ Always, in both modes: the header names which server you are looking at, and an early
    //   return past it would leave it stale — which on an admin page is the one thing that must
    //   never be wrong.
    // ★ Show what is SET, not what was last sent — the same rule as the CPU governor and the
    //   front end. A control that displays the user's last click rather than the server's state
    //   will eventually disagree with it, silently.
    {
      const show = (daySel: string, hourSel: string, hour: any, day: any) => {
        const d = document.getElementById(daySel) as HTMLSelectElement | null;
        const h = document.getElementById(hourSel) as HTMLSelectElement | null;
        if (!d || !h) return;
        // ★ Never yank a dropdown the user is currently using.
        if (document.activeElement === d || document.activeElement === h) return;
        if (typeof hour !== 'number') return;
        d.value = hour < 0 ? '-2' : String(day);
        if (hour >= 0) h.value = String(hour);
      };
      show('updSrvDay', 'updSrvHour', st.updateSrvHour, st.updateSrvDay);
      show('updAllDay', 'updAllHour', st.updateAllHour, st.updateAllDay);
    }
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

/** What is showing right now, in the owner's own words — read from the server, never assumed. */
async function refreshNotice() {
  const el = document.getElementById('noticeNow');
  if (!el) return;
  let j: any = null;
  try { j = await get('notice'); } catch { el.textContent = ''; return; }
  const text = String(j?.text ?? '');
  const left = Number(j?.secondsLeft ?? 0);
  if (!text) { el.textContent = 'Nothing showing.'; return; }
  // ★ -1 is "no end date", and it must NOT read as though it were about to expire.
  el.textContent = left < 0
    ? `Showing until you clear it: “${text}”`
    : `Showing for another ${Math.max(1, Math.round(left / 60))} min: “${text}”`;
  const box = document.getElementById('noticeText') as HTMLInputElement | null;
  if (box && !box.value) box.value = text;
}

export function openAdmin(currentHost: string, adminPassword: string) {
  host = currentHost;
  password = adminPassword;
  open = true;
  failures = 0;
  $('adminPanel').hidden = false;
  // ★ What is showing RIGHT NOW, asked of the server — not what this page last sent.
  void refreshNotice();
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
let wired = false;

/** Wire the panel once, from whichever entry point gets there first.
 *  ★★★ IT USED TO BE CALLED ONLY FROM startApp(), which runs only after connecting to a RADIO. So
 *      on the landing page — where an owner opens this precisely BECAUSE they do not want to take
 *      a radio — nothing was wired: every maintenance button was inert and the update-schedule
 *      selects were empty boxes, because the options are built in here too (Stuart, 2026-08-08:
 *      "these buttons at the bottom also dont work on this combined screen").
 *  ★ Guarded rather than moved: both entry points are legitimate, and wiring twice would give
 *    every button two handlers — one press, two reboots. */
export function initAdmin(getHost: () => string, getPassword: () => string) {
  if (wired) return;
  wired = true;
  $('btnServerAdmin')?.addEventListener('click', () => {
    const pw = getPassword();
    // ★ A ticket is enough on its own — see q(). Only refuse when there is NEITHER.
    if (!pw && !inAdminMode()) {
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
      // ★★★ A 401 MEANS THE SESSION HAS GONE, NOT THAT IT SHOULD BE RETRIED. Re-polling an expired
      //     ticket every two seconds was scored server-side as a wrong password each time, and the
      //     brute-force backoff then refused the OWNER's correct password in the menu — the page
      //     locked its own operator out of the machine it is for (Stuart, 2026-08-08). Stop, say
      //     so, and let them sign in again.
      if (/HTTP 401/.test((e as Error).message)) {
        window.clearInterval(timer); timer = 0;
        $('adminHost').textContent = `${host} — admin session expired, sign in again`;
        return;
      }

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
  // ── Scheduled updates ───────────────────────────────────────────────────────────────────
  {
    const DAYS = [['-2', 'never'], ['-1', 'every day'], ['0', 'Sundays'], ['1', 'Mondays'],
                  ['2', 'Tuesdays'], ['3', 'Wednesdays'], ['4', 'Thursdays'], ['5', 'Fridays'],
                  ['6', 'Saturdays']];
    const fill = (daySel: string, hourSel: string, defDay: string, defHour: string) => {
      const d = $(daySel) as HTMLSelectElement, h = $(hourSel) as HTMLSelectElement;
      for (const [v, label] of DAYS) {
        const o = document.createElement('option'); o.value = v; o.textContent = label; d.appendChild(o);
      }
      for (let i = 0; i < 24; i++) {
        const o = document.createElement('option');
        o.value = String(i); o.textContent = `${String(i).padStart(2, '0')}:00`;
        h.appendChild(o);
      }
      d.value = defDay; h.value = defHour;
    };
    // ★ Small hours by default — least likely to interrupt someone listening.
    fill('updSrvDay', 'updSrvHour', '-2', '4');
    fill('updAllDay', 'updAllHour', '-2', '4');

    $('updSave')?.addEventListener('click', async () => {
      // ★ "never" is day -2 in the UI; the SERVER's definition of off is hour -1. Converting here
      //   keeps one definition of "off" on the server rather than two that must agree.
      const read = (daySel: string, hourSel: string) => {
        const d = Number(($(daySel) as HTMLSelectElement).value);
        const h = Number(($(hourSel) as HTMLSelectElement).value);
        return d === -2 ? { hour: -1, day: -1 } : { hour: h, day: d };
      };
      const srv = read('updSrvDay', 'updSrvHour');
      const all = read('updAllDay', 'updAllHour');
      try {
        await post('schedule', { updateSrvHour: srv.hour, updateSrvDay: srv.day,
                                 updateAllHour: all.hour, updateAllDay: all.day });
        const parts: string[] = [];
        if (srv.hour >= 0) parts.push(`VibeServer at ${String(srv.hour).padStart(2, '0')}:00`);
        if (all.hour >= 0) parts.push(`all packages at ${String(all.hour).padStart(2, '0')}:00`);
        msg('actMsg', parts.length ? `Saved — ${parts.join(', ')}.` : 'Automatic updates turned off.');
      } catch (e) { msg('actMsg', (e as Error).message); }
    });
  }

  // ── ★★ THE NOTICE TO LISTENERS ────────────────────────────────────────────────────────────
  const postNotice = async (text: string, mins: number) => {
    try {
      await post('notice', { text, minutes: mins });
      // ★ Re-read rather than assume: the server decides what is actually showing (it may have
      //   trimmed the text), and an admin page that displays what it SENT rather than what is
      //   live is the same fault as the governor and the mDNS name.
      await refreshNotice();
      msg('actMsg', text ? 'Notice posted.' : 'Notice cleared.');
    } catch (e) { msg('actMsg', (e as Error).message); }
  };
  $('noticePost')?.addEventListener('click', () => {
    const t = ($('noticeText') as HTMLInputElement | null)?.value.trim() ?? '';
    if (!t) { msg('actMsg', 'Type the message first.'); return; }
    const m = Number(($('noticeMins') as HTMLSelectElement | null)?.value ?? '30') || 0;
    void postNotice(t, m);
  });
  $('noticeClear')?.addEventListener('click', () => {
    const el = $('noticeText') as HTMLInputElement | null;
    if (el) el.value = '';
    void postNotice('', 0);
  });

  $('actUpdateCheck')?.addEventListener('click', () => act('update-check'));
  $('actUpdate')?.addEventListener('click', () => act('update', 'Update VibeServer to the latest version?'));
  $('actUpdateAll')?.addEventListener('click', () => act('update-all',
    'Upgrade EVERY package on this machine?\n\n'
    + 'This is a full system upgrade, running unattended. It keeps security fixes current, but an '
    + 'OS upgrade can occasionally need attention and nobody will be watching.'));
  $('actRestart')?.addEventListener('click', () => act('restart', 'Restart VibeServer?'));
  $('actReboot')?.addEventListener('click', () => act('reboot', 'Reboot the whole machine?'));
}
