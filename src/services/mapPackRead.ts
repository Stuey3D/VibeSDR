/**
 * mapPackRead.ts — the map page's reader of the BUNDLED tile packs over the React Native bridge, with retries.
 *
 * ★★★ THE BUG (Mac Ha, beta96, the AIS map on weak WiFi): ships on the map and NO LAND. beta96's app reads the basic pack
 *     by byte range over the bridge (MapOverlay.tsx `packRange` → mapglPack.ts readPackRange), each piece with a 10 s
 *     limit. The FIRST read (vibemapgl.js probes the pack's 7-byte magic) came back after the limit — the bridge was busy
 *     (the JS thread answering a flood of AIS reports and reconnects) — so the basic pack was judged missing, the GPU
 *     map's `ready` rejected, and the only trace was a console line nobody sees. Every AIS reopen rebuilt the page, so it
 *     came back every time.
 * ★★ NOW: a piece that times out or answers nothing is asked again — a few tries, each waiting LONGER than the last (a
 *    busy bridge is slow, not gone), with a short pause before each retry so we never pile requests onto it. An earlier
 *    try that answers late still counts: the first bytes back win. Only after every try has failed is the piece a
 *    failure — and then the page tells the app (MapOverlay `glfail`), which rebuilds the map on Leaflet and SAYS so.
 *
 * ★ The page is one big template literal (MapOverlay buildHtml), so this is plain ES5 SOURCE in a string, embedded into
 *   it as an expression — and evaluated directly by scripts/test_map_pack_read.ts. No backticks in it, ever (they would
 *   end the page's literal — check-map-overlay.mjs).
 *
 * Usage in the page:  var read = (PACK_READ_JS)(post, opts);  read(name, off, len) -> Promise<ArrayBuffer|null>
 *   post(name, off, len, cb) sends one request; cb(buffer | null) is its answer (null = it failed). It may never come.
 *   opts: { timeouts: [ms per try], pauses: [ms before try 2, 3, …], setTimeout, onRetry(name, tryNo, why) }
 */
export const PACK_READ_TIMEOUTS_MS = [10000, 15000, 20000];
export const PACK_READ_PAUSES_MS = [500, 1500];

export const PACK_READ_JS = [
  '(function(post,opts){',
  '  opts=opts||{};',
  '  var T=opts.timeouts||' + JSON.stringify(PACK_READ_TIMEOUTS_MS) + ';',
  '  var P=opts.pauses||' + JSON.stringify(PACK_READ_PAUSES_MS) + ';',
  '  var st=opts.setTimeout||function(f,ms){return setTimeout(f,ms);};',
  '  var onRetry=opts.onRetry||function(){};',
  '  return function(name,off,len){return new Promise(function(res){',
  '    var done=false,n=0;',
  '    function finish(v){if(done)return;done=true;res(v);}',
  '    function attempt(){',
  '      if(done)return;',
  '      var k=n++,moved=false;',
  '      function next(why){',
  '        if(moved||done)return;moved=true;',
  '        if(n>=T.length){if(k===n-1)finish(null);return;}',
  '        onRetry(name,n+1,why);',
  '        st(attempt,P[Math.min(n-1,P.length-1)]||0);',
  '      }',
  '      try{post(name,off,len,function(buf){',
  '        if(buf&&buf.byteLength>0){finish(buf);return;}',
  '        next(\'no bytes\');',
  '      });}catch(e){next(\'send failed\');return;}',
  '      st(function(){next(\'no answer in \'+Math.round(T[k]/1000)+\' s\');},T[k]);',
  '    }',
  '    attempt();',
  '  });};',
  '})',
].join('\n');
