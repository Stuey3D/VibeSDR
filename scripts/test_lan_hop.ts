// V12 SAME NETWORK → GO DIRECT — the web page's hop (web/client/src/lanHop.ts), with a fake page, permission API and
// LAN. Silent: no browser, no audio.
//
//   npx tsx scripts/test_lan_hop.ts
//
// ★★★ WHAT MUST HOLD: the page moves ONLY with an offer, on https, with Local Network Access already granted, and
//     only after the box proves itself; it never raises the browser's prompt unasked; the move keeps the path, query
//     and #fragment (a radio's /r/<id>/ and a directory PIN signature); a browser that cannot probe gets a plain link.
import { createHmac } from 'node:crypto';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

const KEY = 'k'.repeat(64), NONCE = 'lan0123456789abcdef', PROOF = createHmac('sha256', KEY).update(NONCE).digest('hex');
const META = `192.168.86.13:48001|${NONCE}|${PROOF}`;

// ── A fake page ─────────────────────────────────────────────────────────────────────────────────────────────────
type El = { tag: string; children: El[]; textContent: string; hidden: boolean; href?: string; disabled?: boolean;
            type?: string; className?: string; title?: string; listeners: Record<string, Function>;
            appendChild(c: El): El; remove(): void; addEventListener(k: string, f: Function): void };
const el = (tag: string): El => {
  const e: El = { tag, children: [], textContent: '', hidden: true, listeners: {},
    appendChild(c) { e.children.push(c); return c; }, remove() { /* detached */ },
    addEventListener(k, f) { e.listeners[k] = f; } };
  return e;
};
const page: { meta: string | null; protocol: string; replaced: string | null; perm: string | null; splashLan: El;
              answer: 'proof' | 'wrong' | 'none'; fetches: { url: string; init: any }[] } =
  { meta: META, protocol: 'https:', replaced: null, perm: 'granted', splashLan: el('div'), answer: 'proof', fetches: [] };
const g = globalThis as any;
g.document = {
  querySelector: (sel: string) => sel === 'meta[name="vibe-lan"]' && page.meta != null ? { getAttribute: () => page.meta } : null,
  getElementById: (id: string) => (id === 'splashLan' ? page.splashLan : null),
  createElement: (t: string) => el(t),
  createTextNode: (t: string) => { const e = el('#text'); e.textContent = t; return e; },
};
g.location = {
  get protocol() { return page.protocol; }, pathname: '/r/rsp1a/', search: '?x=1', hash: '#vs_nonce=abc&vs_auth=def',
  replace: (u: string) => { page.replaced = u; },
};
Object.defineProperty(g, 'navigator', { configurable: true, value: { permissions: { query: async ({ name }: { name: string }) => {
  if (page.perm == null || name !== 'local-network-access') throw new TypeError('unknown permission');
  return { state: page.perm };
} } } });
g.fetch = async (url: string, init: any) => {
  page.fetches.push({ url, init });
  if (page.answer === 'none') throw new TypeError('Failed to fetch');
  const proof = page.answer === 'proof' ? PROOF : createHmac('sha256', 'other').update(NONCE).digest('hex');
  return { ok: true, json: async () => ({ dirProof: proof }) };
};
const reset = (o: Partial<typeof page>) => { Object.assign(page, { replaced: null, fetches: [], splashLan: el('div'), ...o }); };

(async () => {
  const { lanOfferFromPage, lanHopAtBoot, offerLanOnSplash, hopUrl, lnaState } = await import('../web/client/src/lanHop');
  console.log('test_lan_hop');

  ok(hopUrl('192.168.86.13:48001', { pathname: '/r/a/', search: '?q', hash: '#h' }) === 'http://192.168.86.13:48001/r/a/?q#h',
     'the LAN URL keeps path, query and fragment');

  reset({ meta: null });
  ok(lanOfferFromPage() === null, 'no meta (a stranger, or an old directory) → no offer, nothing happens');
  reset({ meta: META, protocol: 'http:' });
  ok(lanOfferFromPage() === null, 'already on plain http (the LAN page itself) → no offer');
  reset({ meta: 'evil.example:80|x|y', protocol: 'https:' });
  ok(lanOfferFromPage() === null, 'a junk meta → no offer');

  reset({ meta: META, protocol: 'https:', perm: 'granted', answer: 'proof' });
  const offer = lanOfferFromPage();
  ok(!!offer, 'https + meta → an offer');
  ok(await lanHopAtBoot(offer!) === true, '★★★ granted + the server proves → the page moves');
  ok(page.replaced === 'http://192.168.86.13:48001/r/rsp1a/?x=1#vs_nonce=abc&vs_auth=def',
     '…to the same radio, query and PIN signature on the LAN address');
  ok(page.fetches.length === 1 && page.fetches[0].url === `http://192.168.86.13:48001/vibeserver.json?dirNonce=${NONCE}`
     && page.fetches[0].init.targetAddressSpace === 'local', 'the probe carries the nonce and the LNA hint');

  reset({ perm: 'granted', answer: 'wrong' });
  ok(await lanHopAtBoot(offer!) === false && page.replaced === null, '★★★ a box that cannot prove → no move (tunnel)');
  reset({ perm: 'granted', answer: 'none' });
  ok(await lanHopAtBoot(offer!) === false && page.replaced === null, 'nothing there / blocked → no move');

  reset({ perm: 'prompt', answer: 'proof' });
  ok(await lanHopAtBoot(offer!) === false && page.fetches.length === 0, '★★ not yet granted → no probe at boot (no unasked prompt)');
  await offerLanOnSplash(offer);
  const btn = page.splashLan.children.find((c) => c.tag === 'button');
  ok(!page.splashLan.hidden && !!btn && btn.textContent === 'CONNECT DIRECTLY', '…the landing page offers CONNECT DIRECTLY');
  await btn!.listeners.click();
  ok(page.replaced === 'http://192.168.86.13:48001/r/rsp1a/?x=1#vs_nonce=abc&vs_auth=def', 'the press proves and moves');

  reset({ perm: 'prompt', answer: 'wrong' });
  await offerLanOnSplash(offer);
  await page.splashLan.children.find((c) => c.tag === 'button')!.listeners.click();
  ok(page.replaced === null && page.splashLan.children.some((c) => /COULD NOT REACH/.test(c.textContent)),
     'a press that cannot prove stays put and says so');

  reset({ perm: null });
  ok(await lnaState() === 'unsupported', 'a browser without Local Network Access → unsupported');
  ok(await lanHopAtBoot(offer!) === false && page.fetches.length === 0, '…no probe');
  await offerLanOnSplash(offer);
  const a = page.splashLan.children.find((c) => c.tag === 'a');
  ok(!!a && a.href === 'http://192.168.86.13:48001/r/rsp1a/?x=1#vs_nonce=abc&vs_auth=def' && /192\.168\.86\.13:48001/.test(a.textContent),
     '…the landing page offers a plain OPEN DIRECTLY link, naming the address');

  reset({ perm: 'denied' });
  await offerLanOnSplash(offer);
  ok(page.splashLan.hidden && page.splashLan.children.length === 0, 'denied → nothing offered');
  reset({ perm: 'prompt' });
  await offerLanOnSplash(null);
  ok(page.splashLan.hidden, 'no offer → nothing drawn');

  process.exit(fails ? 1 : 0);
})();
