/**
 * digiMapStats.ts — ★★★ beta92 THE DIGI MODES MAP's STATISTICS, LEGEND and SUMMARY, ONE COPY for the web's map window
 * (web/client/src/main.ts spotsMapHtml) and the app's (src/components/MapOverlay.tsx kind 'digi').
 *
 * Stuart, 2026-11-23: the app's map "is missing stuff" next to the web's, and "even with the toggle pressed it seems to lack
 * info compared to the web client" — the app's statistics sheet had RANGE, the top 3 countries, band and mode (and split
 * FST4W-1 from FST4W-3), the web's had DISTANCE bands, BEARING sectors, RANGE, the top 10 countries, mode by FAMILY, band,
 * TOTALS, a LEGEND with counts and a SUMMARY strip. Both were written out by hand in each page, so they drifted. Now the
 * numbers AND the words are worked out here, once:
 *   · digiStats(items, me)      — the numbers (distance bands, bearing sectors, closest / farthest, countries, modes by
 *                                 family, bands, latest, rarest country);
 *   · digiStatsHtml(st, o)      — the three panels' HTML (the web's class names: sect / row / bar / sub / lrow / sw mk /
 *                                 k / v; each page styles them in its own colours).
 * ★ AS A STRING (DIGI_FIT_JS's reason): both map pages are their own documents and embed it as is; the app's is a WebView,
 *   and Hermes cannot hand a function's source over. scripts/test_digi_map_stats.ts runs this very text. Plain ES5, no
 *   backticks and no backslashes, no closing script tag.
 * ★ Each page maps its own spot shape to an ITEM first: { call, grid, country, band, mode, fam, snr, km, lat, lon, t }
 *   — fam is the mode's family (src/utils/digiModes.ts digiModeFamily: PSK31 / PSK63 → PSK, FST4W-3 → FST4W), km rounded
 *   to a whole km (null = no receiver position), lat / lon the locator square's CENTRE (not the spread marker), t in ms.
 * ★ Ties are broken by name, so the same spots give the same list in either page, whatever order they arrived in.
 */

/** One station as the statistics read it (each page builds these from its own spot shape). */
export interface DigiStatItem {
  call: string; grid: string; country: string; band: string; mode: string; fam: string;
  snr: number | null; km: number | null; lat: number; lon: number; t: number;
}
/** What digiStats answers. */
export interface DigiStats {
  n: number;
  /** stations with a distance */
  withKm: number;
  /** four bands between the closest and the farthest (UberSDR's) — empty without distances */
  dist: { lo: number; hi: number; n: number; pct: number }[];
  /** eight sectors from the receiver, N first — null without a receiver position */
  bearing: { d: string; n: number; pct: number }[] | null;
  closest: DigiStatItem | null;
  farthest: DigiStatItem | null;
  countries: [string, number][];
  modes: [string, number][];
  bands: [string, number][];
  modeN: Record<string, number>;
  nCountries: number;
  nBands: number;
  latest: DigiStatItem | null;
  rarest: [string, number] | null;
}

export const DIGI_STATS_JS = `
var DIGI_DIRS=['N','NE','E','SE','S','SW','W','NW'];
function digiBearing(a,b){
  var r=Math.PI/180,dLon=(b.lon-a.lon)*r;
  var y=Math.sin(dLon)*Math.cos(b.lat*r);
  var x=Math.cos(a.lat*r)*Math.sin(b.lat*r)-Math.sin(a.lat*r)*Math.cos(b.lat*r)*Math.cos(dLon);
  return (Math.atan2(y,x)/r+360)%360;
}
function digiSorted(o,asc){
  return Object.keys(o).map(function(k){return [k,o[k]];}).sort(function(a,b){
    var d=asc?a[1]-b[1]:b[1]-a[1];return d||(a[0]<b[0]?-1:a[0]>b[0]?1:0);});
}
function digiStats(items,me){
  var list=(items||[]).filter(function(s){return s&&typeof s==='object';});
  var st={n:list.length,withKm:0,dist:[],bearing:null,closest:null,farthest:null,countries:[],modes:[],bands:[],modeN:{},
          nCountries:0,nBands:0,latest:null,rarest:null};
  var wk=list.filter(function(s){return s.km!=null&&isFinite(Number(s.km));}).sort(function(a,b){
    return a.km-b.km||(a.call<b.call?-1:a.call>b.call?1:0);});
  st.withKm=wk.length;
  if(wk.length){
    var min=wk[0].km,max=wk[wk.length-1].km,step=Math.max(1,(max-min)/4);
    for(var i=0;i<4;i++){
      var lo=min+i*step,hi=min+(i+1)*step;
      var n=wk.filter(function(s){return s.km>=lo&&(i===3?s.km<=hi:s.km<hi);}).length;
      st.dist.push({lo:Math.round(lo),hi:Math.round(hi),n:n,pct:Math.round(n/wk.length*100)});
    }
    st.closest=wk[0];st.farthest=wk[wk.length-1];
    if(me&&isFinite(Number(me.lat))&&isFinite(Number(me.lon))){
      var c=[0,0,0,0,0,0,0,0];
      list.forEach(function(s){if(isFinite(Number(s.lat))&&isFinite(Number(s.lon)))c[Math.round(digiBearing(me,s)/45)%8]++;});
      var tot=c.reduce(function(a,b){return a+b;},0)||1;
      st.bearing=DIGI_DIRS.map(function(d,i){return {d:d,n:c[i],pct:Math.round(c[i]/tot*100)};});
    }
  }
  var co={},mo={},bo={};
  list.forEach(function(s){
    if(s.country)co[s.country]=(co[s.country]||0)+1;
    var f=s.fam||s.mode;if(f)mo[f]=(mo[f]||0)+1;
    if(s.band)bo[s.band]=(bo[s.band]||0)+1;
  });
  st.countries=digiSorted(co,false);st.modes=digiSorted(mo,false);st.bands=digiSorted(bo,false);
  st.modes.forEach(function(x){st.modeN[x[0]]=x[1];});
  st.nCountries=st.countries.length;st.nBands=st.bands.length;
  st.rarest=st.countries.length?digiSorted(co,true)[0]:null;
  list.forEach(function(s){var l=st.latest;
    if(!l||(s.t||0)>(l.t||0)||((s.t||0)===(l.t||0)&&s.call<l.call))st.latest=s;});
  return st;
}
function digiEsc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function digiCol(v){return /^#[0-9a-fA-F]{3,8}$/.test(String(v))?String(v):'#aaaaaa';}
function digiNum(n){return Number(n||0).toLocaleString('en-GB');}
function digiBars(e,colourFor,limit){
  e=e.slice(0,limit||8);
  if(!e.length)return '<div class="sub">Nothing yet</div>';
  var max=e[0][1]||1;
  return e.map(function(x){var c=colourFor?colourFor(x[0]):null;
    return '<div class="row"><span>'+digiEsc(x[0])+'</span><b>'+digiEsc(digiNum(x[1]))+'</b></div>'+
      '<div class="bar"><i style="width:'+Math.round(x[1]/max*100)+'%'+(c?';background:'+digiCol(c):'')+'"></i></div>';}).join('');
}
function digiRangeSub(s){
  return '<div class="sub">'+digiEsc(s.call)+(s.grid?' ('+digiEsc(s.grid)+')':'')+(s.country?' · '+digiEsc(s.country):'')+
    (s.band?' · '+digiEsc(s.band):'')+(s.snr!=null&&isFinite(Number(s.snr))?' · '+digiEsc(s.snr)+'dB':'')+'</div>';
}
function digiCell(k,v,sub){
  return '<div><div class="k">'+digiEsc(k)+'</div><div class="v">'+digiEsc(v)+'</div>'+(sub?'<div class="k">'+digiEsc(sub)+'</div>':'')+'</div>';
}
/* o: { modes:[{m,colour,ink,letter}], modeColour(m), rx:'<svg…>' or '', aprs:{n,on,far:{km,call}|null,leg:[{svg,words}],only},
 *      svgOk(s), overlay:{label,colour}|null, tot:{srvOk,winWords,drawn,total,trunc,unplaced,decodesToday,falseToday,
 *      notKeptToday,spotsSent} } */
function digiStatsHtml(st,o){
  o=o||{};var tot=o.tot||{},ap=o.aprs||{n:0,on:false,far:null,leg:[],only:false};
  var h='';
  if(st.dist.length){
    h+='<div class="sect">DISTANCE</div>';
    st.dist.forEach(function(d){h+='<div class="row"><span>'+d.lo+'–'+d.hi+' km</span><b>'+digiNum(d.n)+' ('+d.pct+'%)</b></div>'+
      '<div class="bar"><i style="width:'+d.pct+'%"></i></div>';});
  }
  if(st.bearing){
    h+='<div class="sect">BEARING</div><div class="rose">'+st.bearing.map(function(b){
      return '<div><span class="n">'+b.d+'</span> <b>'+digiNum(b.n)+'</b><br><span class="n">'+b.pct+'%</span></div>';}).join('')+'</div>';
  }
  if(st.closest){
    h+='<div class="sect">RANGE</div><div class="row"><span>Closest</span><b>'+digiEsc(digiNum(st.closest.km))+' km</b></div>'+digiRangeSub(st.closest)+
       '<div class="row"><span>Farthest</span><b>'+digiEsc(digiNum(st.farthest.km))+' km</b></div>'+digiRangeSub(st.farthest);
  }else if(st.n){
    h+='<div class="sect">RANGE</div><div class="sub">No receiver location published — distances unavailable.</div>';
  }
  h+='<div class="sect">TOP COUNTRIES</div>'+digiBars(st.countries,null,10);
  h+='<div class="sect">BY MODE</div>'+digiBars(st.modes,o.modeColour||null,10);
  h+='<div class="sect">BY BAND</div>'+digiBars(st.bands,null,12);
  if(ap.on&&ap.n){
    h+='<div class="sect">APRS</div><div class="row"><span>Stations</span><b>'+digiNum(ap.n)+'</b></div>'+
      (ap.far?'<div class="row"><span>Farthest</span><b>'+digiEsc(digiNum(ap.far.km))+' km</b></div><div class="sub">'+digiEsc(ap.far.call)+'</div>':'');
  }
  h+='<div class="sect">TOTALS</div>'+
    (tot.srvOk?'<div class="sub">'+digiEsc(tot.winWords||'')+' · background decoding and every listener</div>':'')+
    '<div class="row"><span>Stations</span><b>'+digiNum(st.n)+'</b></div>'+
    (tot.srvOk&&tot.trunc&&tot.total>tot.drawn?'<div class="sub">the newest '+digiNum(tot.drawn)+' of '+digiNum(tot.total)+' in this window are drawn</div>':'')+
    (tot.unplaced?'<div class="row"><span>Heard without a locator</span><b>'+digiNum(tot.unplaced)+'</b></div>':'')+
    (tot.srvOk&&tot.decodesToday>=0?'<div class="row"><span>Decodes today (UTC)</span><b>'+digiNum(tot.decodesToday)+'</b></div>':'')+
    (tot.srvOk&&tot.falseToday>0?'<div class="row"><span>Set aside as false decodes</span><b>'+digiNum(tot.falseToday)+'</b></div><div class="sub">today (UTC): calls no country was allocated — decoded out of noise, never kept or reported. A real call at a locator its country cannot be at is kept, without the locator.</div>':'')+
    (tot.srvOk&&tot.notKeptToday>0?'<div class="row"><span>Not kept today</span><b>'+digiNum(tot.notKeptToday)+'</b></div><div class="sub">the day’s list was full — these were decoded (and sent to PSK Reporter when it is on) but are not on the map</div>':'')+
    '<div class="row"><span>Countries</span><b>'+digiNum(st.nCountries)+'</b></div>'+
    '<div class="row"><span>Bands</span><b>'+digiNum(st.nBands)+'</b></div>';
  var rows=(o.modes||[]).slice();
  Object.keys(st.modeN).sort().forEach(function(m){if(!rows.some(function(x){return x.m===m;}))rows.push({m:m,colour:'#9E9E9E',ink:'#000000',letter:m.charAt(0)});});
  var svgOk=o.svgOk||function(){return '';};
  var lg=(o.rx?'<div class="lrow"><span class="rx">'+o.rx+'</span>Receiver</div>':'')+
    rows.map(function(x){return '<div class="lrow" data-mode="'+digiEsc(x.m)+'"><span class="sw mk" style="background:'+digiCol(x.colour)+
      ';color:'+digiCol(x.ink)+'">'+digiEsc(x.letter)+'</span>'+digiEsc(x.m)+'<span class="lc">'+digiNum(st.modeN[x.m]||0)+'</span></div>';}).join('')+
    (ap.n||ap.on?'<div class="lrow"><span class="sw mk" style="background:#88CCEE;color:#000">A</span>APRS (its symbol where it sends one)<span class="lc">'+digiNum(ap.n)+'</span></div>'+
      (ap.on?(ap.leg||[]).map(function(r){return '<div class="lrow"><span class="asym">'+svgOk(r.svg)+'</span>'+digiEsc(r.words)+'</div>';}).join(''):''):'')+
    (o.overlay?'<div class="lrow"><span class="sw" style="background:'+digiCol(o.overlay.colour)+'"></span>'+digiEsc(o.overlay.label)+'</div>':'')+
    '<div class="lrow foot">Colour + letter = mode · size = SNR</div>';
  var sm;
  if(ap.only)sm=digiCell('APRS',ap.n+' station'+(ap.n===1?'':'s'),ap.n&&ap.newest?'newest '+ap.newest:'none with a position yet');
  else if(st.n){
    var l=st.latest;
    sm=digiCell('LATEST',(l.country||l.call)+' · '+l.band,l.call+' · '+l.mode+(l.snr!=null&&isFinite(Number(l.snr))?' · '+l.snr+'dB':''))+
      (st.rarest?digiCell('RAREST COUNTRY',st.rarest[0]+' ('+st.rarest[1]+')',''):'')+
      (tot.spotsSent>0?digiCell('SPOTS',digiNum(tot.spotsSent),'from '+digiNum(st.n)+' transmitters · '+st.nCountries+' countries')
                      :digiCell('TRANSMITTERS',digiNum(st.n),st.nCountries+' countries'))+
      (ap.n?digiCell('APRS',digiNum(ap.n)+' station'+(ap.n===1?'':'s'),ap.newest?'newest '+ap.newest:''):'');
  /* ★ beta93 (Stuart 11-24: an APRS station on the map and the card said "no spots yet") — APRS stations are not digital
   *  spots, but they are not nothing: the card says so instead of WAITING. */
  }else if(ap.n)sm=digiCell('APRS',digiNum(ap.n)+' station'+(ap.n===1?'':'s'),ap.newest?'newest '+ap.newest:'none with a position yet')+
      digiCell('DIGITAL SPOTS','none yet','');
  else sm=digiCell('WAITING','no spots yet','');
  return {stats:h,legend:lg,summary:sm};
}
`;

/** The panels' keys — the web's checkboxes, the app's keys. */
export const DIGI_PANELS: { id: 'stats' | 'summary' | 'legend'; label: string; words: string }[] = [
  { id: 'stats', label: 'STATS', words: 'Statistics: distance, bearing, range, countries, modes, bands and totals' },
  { id: 'summary', label: 'SUMMARY', words: 'Summary: the latest station, the rarest country and the totals' },
  { id: 'legend', label: 'LEGEND', words: 'Legend: each mode, its colour and letter, and how many' },
];
/** ★ LAYOUT FOLLOWS THE WINDOW: at or above this width the panels sit beside the map (the web's layout); below it they
 *  are keys that open a sheet over part of the map. */
export const DIGI_WIDE_PX = 700;
export type DigiPanelsShown = { stats: boolean; summary: boolean; legend: boolean };
/** What opens with no choice remembered: everything on a wide window (as the web); on a narrow one only the summary
 *  strip — the map is the point, the statistics are a key away. */
export function digiPanelsDefault(wide: boolean): DigiPanelsShown {
  return wide ? { stats: true, summary: true, legend: true } : { stats: false, summary: true, legend: false };
}
/** A remembered choice read back (anything broken → the default for that width). */
export function digiPanelsOf(v: unknown, wide: boolean): DigiPanelsShown {
  const d = digiPanelsDefault(wide);
  let o: any = null;
  try { o = typeof v === 'string' ? JSON.parse(v) : v; } catch { o = null; }
  if (!o || typeof o !== 'object') return d;
  return { stats: typeof o.stats === 'boolean' ? o.stats : d.stats, summary: typeof o.summary === 'boolean' ? o.summary : d.summary,
           legend: typeof o.legend === 'boolean' ? o.legend : d.legend };
}
