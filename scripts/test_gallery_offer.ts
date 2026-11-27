/* test_gallery_offer.ts — ★★ V12 beta96 THE GALLERY ON THE LANDING (src/services/galleryOffer.ts): the button's gate and
 * caption, shared by the web landing and the app's radio chooser.
 *   · one read, the exact path (offset=0&limit=1&since=86400) — never more;
 *   · something today → "N today · newest METEOR 3 h ago"; only older items → "340 kept · nothing new today";
 *   · nothing / no gallery / private (an error answer) / refused (null — a server PIN not given) → no button;
 *   · an older server that ignores since= still gets a button with the newest's words.
 * Silent, no network. */
import { galleryOffer, galleryAgeWords, galleryKindWord } from '../src/services/galleryOffer';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

async function main() {
  const now = 1790000000;
  const asked: string[] = [];
  const mk = (ans: any) => async (p: string) => { asked.push(p); return ans; };

  const meteor = await galleryOffer(mk({ enabled: true, since: 86400, total: 12, count: 340, items: [{ k: 'lrpt', t: now - 3 * 3600 - 60, f: 'x.png' }] }), now);
  ok(asked.length === 1 && asked[0] === '/vibeserver/gallery.json?offset=0&limit=1&since=86400', `one small read, the newest of the last 24 h (${asked.join(' , ')})`);
  ok(meteor.show && meteor.line === '12 today · newest METEOR 3 h ago', `today's caption: "${meteor.line}"`);
  ok(meteor.today === 12 && meteor.count === 340 && meteor.newest?.k === 'lrpt', 'its counts and newest');

  const old = await galleryOffer(mk({ enabled: true, since: 86400, total: 0, count: 340, items: [] }), now);
  ok(old.show && old.line === '340 kept · nothing new today', `only older items: "${old.line}"`);

  const empty = await galleryOffer(mk({ enabled: true, since: 86400, total: 0, count: 0, items: [] }), now);
  ok(!empty.show, 'an empty gallery: no button');
  ok(!(await galleryOffer(mk({ enabled: false, items: [] }), now)).show, 'a server that keeps no gallery: no button');
  ok(!(await galleryOffer(mk({ items: [], enabled: false, error: "the owner has made this server's gallery private" }), now)).show, 'a private gallery: no button');
  ok(!(await galleryOffer(mk(null), now)).show, 'refused (a server PIN not given) or unreachable: no button, no count');
  ok(!(await galleryOffer(async () => { throw new Error('x'); }, now)).show, 'a getter that throws: no button');

  const older = await galleryOffer(mk({ enabled: true, total: 900, count: 900, items: [{ k: 'navtex', t: now - 600 }] }), now);
  ok(older.show && older.line === 'newest NAVTEX 10 min ago', `an older server (no since=): "${older.line}"`);

  ok(galleryAgeWords(now - 30, now) === 'just now' && galleryAgeWords(now - 2 * 86400, now) === '2 days ago', 'the age words');
  ok(galleryKindWord('ft8') === 'DIGITAL SPOTS' && galleryKindWord('newkind') === 'NEWKIND', 'the kind words (the filter chips’ own)');

  console.log('── the app\'s radio chooser (src/screens/SDRScreen.tsx) — the wiring ──');
  {
    const fs = await import('node:fs');
    const src = fs.readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
    const gal = fs.readFileSync(new URL('../src/components/GalleryOverlay.tsx', import.meta.url), 'utf8');
    ok(/void galleryOffer\(async \(path\) => \{\s*try \{ const r = await fetch\(withReadAuth\(`\$\{baseUrl/.test(src), 'the chooser asks galleryOffer through the read auth (the server PIN the picker took), on the server\'s base');
    ok(/chooserItems\(pickRadios, pickShowPin, !!asOffer, !!galOffer\)/.test(src), 'GALLERY is in the chooser\'s pad order only when there is something to show');
    ok(/case 'gallery': openGalleryViewer\(\); return;/.test(src), 'Enter on it opens the gallery');
    ok(/nav-registered: the chooser list \(pickItems 'gallery'\)/.test(src) && /NavRing on=\{pickItems\[pickFocus\]\?\.k === 'gallery'\}/.test(src), 'the row shows the pad\'s ring');
    ok(/const openGalleryViewer = useCallback\(\(\) => \{ setGalleryViewer\(true\); setGalleryOpen\(true\); \}, \[\]\)/.test(src), 'it opens the gallery overlay — no radio chosen, no connect');
    for (const cb of ['onNodesMap', 'onNavtexMap', 'onShipMap', 'onDigiMap'])
      ok(new RegExp(`${cb}=\\{IS_APPLE_TV[^?]*\\|\\| galleryViewer \\? null`).test(src), `in the viewer ${cb} is null (no MAP key — it needs a live map)`);
    ok(/pickOn && !galleryOpen,/.test(src), 'the chooser list stops listening while the gallery is over it');
    ok(/onClose=\{\(\) => \{ setGalleryOpen\(false\); setGalleryViewer\(false\); \}\}/.test(src), 'closing it ends the viewer');
    // ★ GalleryOverlay needs no session: what it reads comes from its props (a base and the read auth)
    ok(/export default function GalleryOverlay\(\{ visible, base, readAuth = '', translateTo = '', onClose,/.test(gal) && !/^import .*(SDRScreen|VibeClient|useSession|sdrClient)/m.test(gal), 'GalleryOverlay reads only its base + read auth (no session)');
  }

  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
}
void main();
