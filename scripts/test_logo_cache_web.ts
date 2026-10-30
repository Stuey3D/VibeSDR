// The web client's logo index storage (web/client/src/logoStoreWeb.ts): the old DAB store migrated at the right ranks,
// the key round trip the station list and bookmarks rely on, air bytes replacing only on change, a dead picture
// forgotten and never re-filed this visit.
let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => { const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++; if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`); };
const mem: Record<string, string> = {
  'vibe.dabLogos.v1': JSON.stringify({ '225|49614|50386': { u: 'https://x/heart.png', t: 1 }, '225|49614|50387': { u: 'data:image/png;base64,AAAA', t: 2 } }),
};
(globalThis as any).localStorage = {
  getItem: (k: string) => (k in mem ? mem[k] : null), setItem: (k: string, v: string) => { mem[k] = v; }, removeItem: (k: string) => { delete mem[k]; },
};
const W = await import('../web/client/src/logoStoreWeb.ts');
eq('web key → index key', W.dabIdxKey('225|49614|50386'), 'dab:E1|C1CE|C4D2');
eq('…and back', W.dabWebKey('dab:E1|C1CE|C4D2'), '225|49614|50386');
eq('unknown ECC round-trips', W.dabWebKey(W.dabIdxKey('-1|49614|50386')), '-1|49614|50386');
eq('migrated URL filed as a name match (identity may replace it)', W.webLogos.get('dab:E1|C1CE|C4D2')?.src, 'name');
eq('migrated data: URL filed as a slide (an air logo may replace it)', W.webLogos.get('dab:E1|C1CE|C4D3')?.src, 'slide');
const k = 'dab:E1|C1CE|C4D2';
eq('RadioDNS replaces the migrated name match', W.webLogoOffer(k, 'radiodns', 'https://x/rdns.png'), 'https://x/rdns.png');
eq('a name match cannot replace it', W.webLogoOffer(k, 'name', 'https://x/other.png'), 'https://x/rdns.png');
eq('the multiplex takes over', W.webLogoOffer(k, 'air', '/vibeserver/dablogoair?sid=50386', { sig: 'data:image/png;base64,A1', show: 'data:image/png;base64,A1' }), 'data:image/png;base64,A1');
const saved = W.webLogos.get(k)!.savedAt;
eq('the same air picture again: kept, not re-stored', [W.webLogoOffer(k, 'air', '/vibeserver/dablogoair?sid=50386', { sig: 'data:image/png;base64,A1', show: 'data:image/png;base64,A1' }), W.webLogos.get(k)!.savedAt], ['data:image/png;base64,A1', saved]);
eq('a new air picture replaces it', W.webLogoOffer(k, 'air', '/vibeserver/dablogoair?sid=50386', { sig: 'data:image/png;base64,A2', show: 'data:image/png;base64,A2' }), 'data:image/png;base64,A2');
eq('RadioDNS is outranked by air', W.webLogoOutranked(k, 'radiodns'), true);
const k2 = 'fm:GB|C203|9660';
W.webLogoOffer(k2, 'radiodns', 'https://dead/logo.png');
eq('a dead picture is forgotten', [W.webLogoForget('https://dead/logo.png'), W.webLogos.get(k2)], [[k2], undefined]);
eq('…and never re-filed this visit', W.webLogoOffer(k2, 'radiodns', 'https://dead/logo.png'), '');
eq('…but the key is retried later (backoff), not blanked for good', W.webLogos.missOf(k2)?.fails, 1);
await new Promise(r => setTimeout(r, 1100));
eq('persisted to the v2 record; the v1 record removed', [Object.keys(JSON.parse(mem['vibe.logos.v2'] || '{}')).includes(k), 'vibe.dabLogos.v1' in mem], [true, false]);
console.log(`logo cache (web storage): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
