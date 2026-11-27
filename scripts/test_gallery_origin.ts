// test_gallery_origin.ts — WHO a gallery item came from, as the list's badge and the open item's line say it
// (src/utils/galleryOrigin.ts — the ONE rule the web gallery (web/client/src/main.ts galOriginTag) and the app's
// GalleryOverlay both draw with).
// ★★ beta97 (Stuart, Sony beta96, METEOR filter): every Meteor picture read LISTENER, the unattended 02:44 UTC pass too.
//   The server now files an unattended satellite pass's pictures as "o":"background","p":"Satellite pass"
//   (vibe_gallery.h stampStartOrigin; vibeserver/test-lrpt-host.cpp proves the server half). This proves the clients'
//   half: those index lines read BACKGROUND, a listener's still LISTENER.
import { galleryOriginTag, galleryOriginWords } from '../src/utils/galleryOrigin';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// index lines exactly as vibe_gallery.h indexLine writes them (the fields the badge reads)
const passLine = JSON.parse('{"f":"x.png","k":"lrpt","o":"background","t":1795920240,"hz":137900000,"m":"lrpt","d":"lrpt","p":"Satellite pass","r":"","title":"METEOR LRPT · RGB 221 false colour"}');
const listenerLine = JSON.parse('{"f":"y.png","k":"lrpt","o":"live","t":1795920240,"hz":137900000,"m":"lrpt","d":"lrpt","p":"","r":"","title":"METEOR LRPT · channel 1 visible"}');
const fcPassLine = JSON.parse('{"f":"z.csv","k":"fctlm","o":"background","t":1795920240,"hz":145935000,"m":"usb","d":"fctlm","p":"Satellite pass","r":"","title":"AO-73"}');
const orbPassLine = JSON.parse('{"f":"o.png","k":"orbcomm","o":"background","t":1795920240,"hz":137500000,"m":"nfm","d":"orbcomm","p":"Satellite pass","r":"","title":"ORBCOMM"}');

console.log('an unattended satellite pass');
ok(galleryOriginTag(passLine) === 'BACKGROUND', `METEOR pass badge: ${galleryOriginTag(passLine)}`);
ok(galleryOriginWords(passLine) === 'background decoding (Satellite pass)', `open line: ${galleryOriginWords(passLine)}`);
ok(galleryOriginTag(fcPassLine) === 'BACKGROUND', 'FUNcube pass badge: BACKGROUND');
ok(galleryOriginTag(orbPassLine) === 'BACKGROUND', 'ORBCOMM pass badge: BACKGROUND');
console.log('a listener\'s decoder');
ok(galleryOriginTag(listenerLine) === 'LISTENER', `METEOR listener badge: ${galleryOriginTag(listenerLine)}`);
ok(galleryOriginWords(listenerLine) === "a listener's decoder", `open line: ${galleryOriginWords(listenerLine)}`);

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
