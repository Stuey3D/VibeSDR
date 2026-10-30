// V12 SAME NETWORK → GO DIRECT — the client half (src/utils/lanDirect.ts), shared by the app's picker and the web
// page's hop (web/client/src/lanHop.ts). Pure: a fake fetch stands in for the LAN.
//
//   npx tsx scripts/test_lan_direct.ts
//
// ★★★ WHAT MUST HOLD: an address is used ONLY when the box there answers the nonce with the server's own HMAC.
//     A wrong proof, no proof, silence or an error → null, and the caller keeps the tunnel. A junk offer never
//     produces a request at all.
import { createHmac } from 'node:crypto';
import { parseLanOffer, parseLanMeta, proveLan, isLanHost, lanProbeUrl, type LanFetch } from '../src/utils/lanDirect';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

const KEY = 'k'.repeat(64), NONCE = 'lan0123456789abcdef', PROOF = createHmac('sha256', KEY).update(NONCE).digest('hex');
const offer = (hosts: string[]) => parseLanOffer({ hosts, nonce: NONCE, proof: PROOF });

console.log('test_lan_direct');
ok(isLanHost('192.168.86.13:48001') && isLanHost('10.1.2.3:80') && isLanHost('172.31.0.1:8073'), 'RFC 1918 + port accepted');
ok(!isLanHost('8.8.8.8:80') && !isLanHost('127.0.0.1:48001') && !isLanHost('100.64.0.1:80') && !isLanHost('192.168.1.1')
   && !isLanHost('evil.example:80') && !isLanHost('192.168.1.1:0') && !isLanHost('192.168.1.1:99999') && !isLanHost(5),
   'public, loopback, CGNAT, portless, hostnames and bad ports refused');
ok(parseLanOffer({ hosts: ['192.168.86.13:48001'], nonce: NONCE, proof: PROOF })?.hosts.length === 1, 'a good offer parses');
ok(parseLanOffer({ hosts: ['8.8.8.8:80'], nonce: NONCE, proof: PROOF }) === null, 'an offer with no LAN host is no offer');
ok(parseLanOffer({ hosts: ['192.168.86.13:48001'], nonce: 'x', proof: PROOF }) === null
   && parseLanOffer({ hosts: ['192.168.86.13:48001'], nonce: NONCE, proof: 'zz' }) === null
   && parseLanOffer(null) === null && parseLanOffer('x') === null, 'a bad nonce / proof / shape is refused');
ok(parseLanMeta(`192.168.86.13:48001,10.0.0.2:48001|${NONCE}|${PROOF}`)?.hosts.join() === '192.168.86.13:48001,10.0.0.2:48001',
   'the page meta parses (two hosts)');
ok(parseLanMeta('') === null && parseLanMeta(`javascript:alert(1)|${NONCE}|${PROOF}`) === null, 'a junk meta is no offer');
ok(lanProbeUrl('192.168.86.13:48001', NONCE) === `http://192.168.86.13:48001/vibeserver.json?dirNonce=${NONCE}`, 'the probe URL');

// A fake LAN: each host answers (or not) after a delay.
type Ans = { proof?: string; delay?: number; throws?: boolean; silent?: boolean };
const lan = (m: Record<string, Ans>): { f: LanFetch; asked: string[] } => {
  const asked: string[] = [];
  const f: LanFetch = (url, timeoutMs) => {
    asked.push(url);
    const host = new URL(url).host, a = m[host];
    return new Promise((res, rej) => {
      if (!a || a.silent) { setTimeout(() => res(null), timeoutMs); return; }   // as an aborted fetch resolves
      setTimeout(() => (a.throws ? rej(new Error('net')) : res(a.proof ? { dirProof: a.proof } : { name: 'x' })), a.delay ?? 5);
    });
  };
  return { f, asked };
};

(async () => {
  {
    const { f } = lan({ '192.168.86.13:48001': { proof: PROOF } });
    ok(await proveLan(offer(['192.168.86.13:48001']), f) === '192.168.86.13:48001', '★ the real server proves → its address');
  }
  {
    const { f } = lan({ '192.168.86.13:48001': { proof: PROOF.toUpperCase() } });
    ok(await proveLan(offer(['192.168.86.13:48001']), f) === '192.168.86.13:48001', 'the proof compares case-blind (hex)');
  }
  {
    const { f } = lan({ '192.168.86.13:48001': { proof: createHmac('sha256', 'other').update(NONCE).digest('hex') } });
    ok(await proveLan(offer(['192.168.86.13:48001']), f) === null, '★★★ a DIFFERENT VibeServer at that address → null (tunnel)');
  }
  {
    const { f } = lan({ '192.168.86.13:48001': {} });
    ok(await proveLan(offer(['192.168.86.13:48001']), f) === null, '★★ something that answers without a proof → null');
  }
  {
    const { f } = lan({ '192.168.86.13:48001': { throws: true } });
    ok(await proveLan(offer(['192.168.86.13:48001']), f) === null, 'a network error → null');
  }
  {
    const t0 = Date.now();
    const { f } = lan({ '192.168.86.13:48001': { silent: true } });
    const r = await proveLan(offer(['192.168.86.13:48001']), f, 300);
    ok(r === null && Date.now() - t0 < 1000, '★ nothing there (another network) → null within the timeout');
  }
  {
    const t0 = Date.now();
    const never: LanFetch = () => new Promise(() => {});   // a fetch that ignores its own timeout
    ok(await proveLan(offer(['192.168.86.13:48001']), never, 200) === null && Date.now() - t0 < 1000,
       'even a fetch that never settles cannot hold the caller past the timeout');
  }
  {
    const { f, asked } = lan({ '10.0.0.2:48001': { proof: PROOF, delay: 30 }, '172.17.0.1:48001': { throws: true } });
    ok(await proveLan(offer(['172.17.0.1:48001', '192.168.86.13:48001', '10.0.0.2:48001']), f, 300) === '10.0.0.2:48001',
       'several addresses: asked together, the one that proves wins');
    ok(asked.length === 3, 'every offered address was asked once');
  }
  {
    const { f, asked } = lan({});
    ok(await proveLan(null, f) === null && asked.length === 0, 'no offer → no request at all');
  }
  process.exit(fails ? 1 : 0);
})();
