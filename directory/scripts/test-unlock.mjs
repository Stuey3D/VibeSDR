// Unit-test the directory's tryServerPin() against a faked network, covering the shapes real
// hardware cannot easily be put into — especially the one that shipped a hole:
// a FRONT DOOR WITH NO MASTER PIN, where /vibeserver/auth/verify answers 200 to anybody.
import { readFileSync } from 'node:fs';
import { createHmac } from 'node:crypto';

const html = readFileSync(new URL('../public/index.html', import.meta.url), 'utf8');
const grab = (name) => {
  let i = html.indexOf(`function ${name}(`);
  if (i < 0) throw new Error('not found: ' + name);
  // ★ keep the `async ` prefix — dropping it turns an async body into a syntax error
  if (html.slice(i - 6, i) === 'async ') i -= 6;
  // walk braces from the first { after the signature
  let j = html.indexOf('{', i), d = 0;
  for (let k = j; k < html.length; k++) {
    if (html[k] === '{') d++;
    else if (html[k] === '}') { d--; if (!d) return html.slice(i, k + 1); }
  }
  throw new Error('unbalanced: ' + name);
};
const src = ['pinStoreKey', 'pinRestore', 'pinRemember', 'pinToken', 'tryServerPin',
             'refreshUnlockedRadios'].map(grab).join('\n');

const REAL_PIN = '4321';
const hex = (pin, nonce) => createHmac('sha256', pin).update(nonce).digest('hex');

/** @param shape 'frontdoor-radiopin' | 'frontdoor-nomaster' | 'single-serverpin' | 'single-nopin' */
function makeFetch(shape, log) {
  return async (url) => {
    const u = new URL(url);
    log.push(u.pathname);
    const q = u.searchParams;
    if (u.pathname === '/vibeserver/auth') {
      const required = shape === 'single-serverpin';
      return { json: async () => ({ required, nonce: 'n'.repeat(32), lockedFor: 0 }) };
    }
    if (u.pathname === '/vibeserver/unlock') {
      // only a front door answers; a radio PIN opens its own radio
      if (shape === 'frontdoor-radiopin')
        return { json: async () => ({ radios: q.get('vs_auth') === hex(REAL_PIN, q.get('vs_nonce')) ? ['r1'] : [] }) };
      if (shape === 'frontdoor-nomaster') return { json: async () => ({ radios: [] }) };
      return { status: 404, json: async () => { throw new Error('not json'); } };
    }
    if (u.pathname === '/vibeserver/auth/verify') {
      // ★ THE TRAP: a door with no PIN says 200 to anything.
      if (shape === 'frontdoor-nomaster' || shape === 'single-nopin') return { status: 200 };
      return { status: q.get('vs_auth') === hex(REAL_PIN, q.get('vs_nonce')) ? 200 : 401 };
    }
    if (u.pathname === '/vibeserver/radios') return { json: async () => ({ radios: [] }) };
    return { status: 404, json: async () => ({}) };
  };
}

const store = new Map();
const sessionStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, v),
  clear: () => store.clear(),
};

const cases = [
  ['front door, radio PIN, RIGHT pin', 'frontdoor-radiopin', { pin: false }, REAL_PIN, 1],
  ['front door, radio PIN, WRONG pin', 'frontdoor-radiopin', { pin: false }, '1111',   0],
  ['front door, NO master, any pin  ', 'frontdoor-nomaster', { pin: false }, '1111',   0],
  ['single radio + server PIN, RIGHT', 'single-serverpin',   { pin: true  }, REAL_PIN, 1],
  ['single radio + server PIN, WRONG', 'single-serverpin',   { pin: true  }, '1111',   0],
  ['single radio, NO pin at all     ', 'single-nopin',       { pin: false }, '1111',   0],
];

let pass = 0;
for (const [name, shape, flags, pin, want] of cases) {
  store.clear();
  const log = [];
  const ALL_LOCKED = '*';
  const PIN_OPEN = new Map();
  const s = { address: 'x.example', pin: flags.pin, radios: [{ id: 'r1', pinLocked: true }] };
  const fn = new Function('PIN_OPEN', 'ALL_LOCKED', 'sessionStorage', 'fetch', 'crypto',
    src + '; return tryServerPin;')(PIN_OPEN, ALL_LOCKED, sessionStorage, makeFetch(shape, log), globalThis.crypto);
  let got = 0;
  try { got = await fn(s, pin); } catch (e) { got = 'threw: ' + e.message; }
  const ok = got === want;
  if (ok) pass++;
  console.log((ok ? '  ok   ' : '  FAIL ') + name + ' -> opened=' + got
              + (ok ? '' : ' (wanted ' + want + ')') + '   [' + log.join(' ') + ']');
}
console.log(`\n${pass}/${cases.length} pass`);
process.exit(pass === cases.length ? 0 : 1);
