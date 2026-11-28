/**
 * mapLegend.ts — ★★★ beta97 THE KEY INSIDE THE MAP CARD (Stuart, build 586, a public UberSDR's HFDL map on the Mac: "the key
 * isn't showing properly" — the page drew its own legend panel at its old top-left place, UNDER the MAP card). The web keeps
 * the key as a fold in MAP CONTROLS; so does the app now: the page READS its legend (whatever map drew it — the track maps'
 * #legbody, the ADS-B layer's key, the digital map's legend panel) into plain items and posts them; React Native draws them
 * in the card's KEY fold. The page's legend panel never opens on a card map.
 *
 * Two halves: LEGEND_READ_JS (the page's reader, a string — ES5, it runs in the WebView) and legendClean (React Native's
 * side: everything from the page is checked, bounded and typed; nothing it sends is trusted).
 */

export type LegendItem =
  | { k: 'h'; text: string }
  | { k: 'row'; text: string; n?: string; dot?: string; ink?: string; letter?: string; size?: number; sq?: boolean; glyph?: string; gc?: string; svg?: string }
  | { k: 'bar'; stops: string[] }
  | { k: 'axis'; labels: string[] }
  | { k: 'check'; text: string; on: boolean; i: number };

export const LEGEND_MAX_ITEMS = 120;
const SVG_MAX = 6000;

/** A CSS colour the page computed — rgb()/rgba()/hsl()/hsla()/#hex only; anything else is dropped. */
export function legendColour(v: unknown): string | undefined {
  if (typeof v !== 'string') return undefined;
  const s = v.trim();
  if (/^#[0-9a-f]{3,8}$/i.test(s)) return s;
  if (/^(rgba?|hsla?)\([0-9.,%\s/-]{3,60}\)$/i.test(s)) return /rgba?\(0,\s*0,\s*0,\s*0\)/i.test(s) ? undefined : s;   // transparent = none
  return undefined;
}
const str = (v: unknown, max: number) => (typeof v === 'string' ? v.replace(/\s+/g, ' ').trim().slice(0, max) : '');
/** An SVG the page drew (the APRS symbols, the ship / aircraft icons): an <svg> element only, no script, no events, no links. */
export function legendSvg(v: unknown): string | undefined {
  if (typeof v !== 'string' || v.length > SVG_MAX) return undefined;
  const s = v.trim();
  if (!/^<svg[\s>]/i.test(s) || !/<\/svg>$/i.test(s)) return undefined;
  if (/<script|<foreignObject|\son[a-z]+\s*=|javascript:|href\s*=\s*["']?(?!#)/i.test(s)) return undefined;
  return s;
}

/** React Native's side: the page's items, checked. */
export function legendClean(raw: unknown): LegendItem[] {
  if (!Array.isArray(raw)) return [];
  const out: LegendItem[] = [];
  for (const x of raw.slice(0, LEGEND_MAX_ITEMS)) {
    if (!x || typeof x !== 'object') continue;
    const o = x as Record<string, unknown>;
    if (o.k === 'h') { const t = str(o.text, 60); if (t) out.push({ k: 'h', text: t }); }
    else if (o.k === 'row') {
      const it: LegendItem & { k: 'row' } = { k: 'row', text: str(o.text, 160) };
      const n = str(o.n, 12); if (n) it.n = n;
      const svg = legendSvg(o.svg);
      if (svg) { it.svg = svg; const gc = legendColour(o.gc); if (gc) it.gc = gc; }
      else if (legendColour(o.dot)) {
        it.dot = legendColour(o.dot); const ink = legendColour(o.ink); if (ink) it.ink = ink;
        const l = str(o.letter, 2); if (l) it.letter = l;
        const sz = Number(o.size); if (Number.isFinite(sz) && sz >= 4 && sz <= 24) it.size = sz;
        if (o.sq === true) it.sq = true;
      } else { const g = str(o.glyph, 3); if (g) { it.glyph = g; const gc = legendColour(o.gc); if (gc) it.gc = gc; } }
      if (it.text || it.svg || it.dot) out.push(it);
    } else if (o.k === 'bar' && Array.isArray(o.stops)) {
      const stops = o.stops.slice(0, 24).map(legendColour).filter((c): c is string => !!c);
      if (stops.length >= 2) out.push({ k: 'bar', stops });
    } else if (o.k === 'axis' && Array.isArray(o.labels)) {
      const labels = o.labels.slice(0, 8).map((l) => str(l, 10));
      if (labels.length >= 2) out.push({ k: 'axis', labels });
    } else if (o.k === 'check') {
      const i = Number(o.i), t = str(o.text, 100);
      if (Number.isInteger(i) && i >= 0 && i < 16 && t) out.push({ k: 'check', text: t, on: o.on === true, i });
    }
  }
  return out;
}

/** ★ The page's reader (ES5, in the WebView). legItems(roots) walks each root's legend markup — every map's: the track
 *  maps' .lg-sec / .lg-row, the ADS-B layer's .t / .r / .bar / .ax, the digital map's .sect / .lrow (its count .lc) — and
 *  returns the items; hidden parts are skipped; a checkbox row is a 'check' (React Native presses it back: __legCheck). */
export const LEGEND_READ_JS = `
function legItems(roots){var out=[],inputs=[];
  function cs(el,p){try{return getComputedStyle(el)[p]||'';}catch(e){return '';}}
  function txt(el){return (el&&el.textContent||'').replace(/\\s+/g,' ').trim();}
  function has(el,c){if(!el.classList)return false;for(var i=0;i<c.length;i++)if(el.classList.contains(c[i]))return true;return false;}
  function hid(el){return el.hidden||(el.style&&el.style.display==='none')||has(el,['scroll-arrow']);}
  function row(el){
    var inp=el.querySelector('input[type=checkbox]');
    if(inp){inputs.push(inp);out.push({k:'check',text:txt(el),on:!!inp.checked,i:inputs.length-1});return;}
    var lc=el.querySelector('.lc'),n=lc?txt(lc):'',sw=el.firstElementChild,it={k:'row',text:''};
    if(sw&&sw!==lc&&sw.tagName==='SPAN'&&sw.nextSibling){
      var svg=sw.querySelector('svg');
      if(svg){it.svg=svg.outerHTML;it.gc=cs(sw,'color');}
      else if(has(sw,['lg-dot','sw','mk'])){it.dot=cs(sw,'backgroundColor');it.ink=cs(sw,'color');it.letter=txt(sw).slice(0,2);
        it.size=parseFloat(sw.style.width)||0;it.sq=has(sw,['mk'])||/^[1-4]px/.test(sw.style.borderRadius||'');}
      else{it.glyph=txt(sw).slice(0,3);it.gc=cs(sw,'color');}
      var f=txt(el),s=txt(sw);it.text=(s&&f.indexOf(s)===0)?f.slice(s.length).trim():f;
    }else it.text=txt(el);
    if(n){it.n=n;if(it.text.slice(-n.length)===n)it.text=it.text.slice(0,-n.length).trim();}
    if(it.text||it.svg||it.dot)out.push(it);
  }
  function walk(el){for(var c=el.firstElementChild;c;c=c.nextElementSibling){if(hid(c))continue;
    if(has(c,['lg-sec','t','sect']))out.push({k:'h',text:txt(c)});
    else if(has(c,['lg-row','r','lrow']))row(c);
    else if(has(c,['bar'])){var m=String(c.style.background||c.style.backgroundImage||'').match(/(hsla?|rgba?)\\([^)]*\\)|#[0-9a-f]{3,8}/gi);if(m&&m.length>1)out.push({k:'bar',stops:m.slice(0,24)});}
    else if(has(c,['ax'])){var a=[];for(var q=c.firstElementChild;q;q=q.nextElementSibling)a.push(txt(q));out.push({k:'axis',labels:a});}
    else walk(c);}}
  for(var i=0;i<roots.length;i++)if(roots[i])walk(roots[i]);
  window.__legInputs=inputs;return out.slice(0,${LEGEND_MAX_ITEMS});}
var legOn=false,legSig='',legT=null;
function legTell(roots,post,force){try{var it=legItems(roots),sig=JSON.stringify(it);if(!force&&sig===legSig)return;legSig=sig;post({t:'legend',items:it});}catch(e){}}
window.__legCheck=function(i){try{var b=(window.__legInputs||[])[i];if(b){b.click();}}catch(e){}};
`;
