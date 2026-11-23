/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * mapAutoFit.ts — ★★★ ONE AUTO FIT FOR EVERY MAP (Stuart, 2026-11-23: "i want all maps to have an auto fit that gently
 * glides the zoom in and out like the adsb maps" … and on the spots map's fit: "include the receiver").
 *
 * THE ADS-B MAP IS THE REFERENCE (web/mapkit/adsblayer.js — its AUTO key, cameraStep, AutoZoom, cameraFor). This is
 * its camera, lifted out for every other map — the digi / CW spots maps, the shipping map (AIS · DSC · NAVTEX), APRS,
 * ISM, radiosondes, the aviation map's ground stations, NDB — in the app's map WebView (MapOverlay.tsx) and on the
 * web page (tracks.ts, the spots window in main.ts, the DSC window in dsc.ts, ndblayer.js through its hosts):
 *
 *   • AUTO, ON WHEN THE MAP OPENS. The view frames everything the map SHOWS and the receiver (the bounding box of both,
 *     centred on the box — ADS-B's "FIT" framing), and keeps re-framing as things come and go.
 *   • A GLIDE, NEVER A SNAP: MapLibre easeTo with ADS-B's cubic ease-in-out, CAMERA_MS (2.2 s — ADS-B's CAMERA_MS =
 *     GLIDE_MS); Leaflet flyTo over the same 2.2 s at easeLinearity 0.25. The very first framing, if it comes as the
 *     map opens, is a jump (as ADS-B's is): there is no view yet to glide from.
 *   • HYSTERESIS — ADS-B's AutoZoom numbers exactly: OUT at once when something leaves the view, to a little further
 *     than needed (slack 0.3 of a zoom level); IN only when the view is too wide by more than 0.6 of a level for 4 s;
 *     re-centre only when the box's centre has wandered 12 % of the screen for 3 s; never twice inside 1.5 s, never
 *     while a glide is running. A station appearing inside the view, or ageing out, moves nothing at once.
 *   • A PERSON WHO MOVES THE MAP HAS TAKEN IT: a drag, a pinch, the wheel, the ± keys (a move carrying a gesture, or
 *     within a second of a touch / click / wheel / key on the map) PAUSES it — the key shows striped, as ADS-B's AUTO
 *     does. Tapping the key: paused → resume and glide; on → off; off → on and glide. A focus (a row chosen) pauses it
 *     too: that is where the person wants the map.
 *   • NEVER STREET LEVEL: at most AUTOFIT_MAX_ZOOM (Leaflet zoom 11 — ADS-B's own ZMAX); a map may ask for less (the
 *     digi map: DIGI_FIT_MAX_ZOOM, 9). Nothing heard yet = the map stays where it opened (the receiver alone is not a
 *     view to zoom into).
 *
 * ★ AS A STRING because every map page is its own document (the app's WebView, the web's popup windows, the web's map
 *   view) — the digiMapShared.ts DIGI_FIT_JS pattern. Plain ES5: no backticks, no backslashes, no "</" — it is
 *   interpolated into template literals and inline <script>s. scripts/test_map_autofit.ts runs this very text and
 *   checks its numbers against adsblayer.js's.
 *
 *   var af = VibeAutoFit.attach(map, {           // a Leaflet map, the GPU compat map (map._gl), a VibeMapGL vm or MapLibre
 *     points:   function () { return [[lat, lon], …]; },   // what the map SHOWS now (filters applied)
 *     receiver: function () { return { lat, lon } | null; },
 *     button:   el | null,                     // the FIT key: on / paused / off, as ADS-B's AUTO
 *     what:     'station',                     // its title's words ("every station")
 *     maxZoom:  11,                            // Leaflet zoom; never closer
 *     obstacle: function () { return { corner, w, h } | null; },   // a list over a corner (ADS-B's setObstacle)
 *     onState:  function (s) {} });            // { on, paused } after any change
 *   af.poke()     // the points changed — consider a move now (the 1 s tick does the rest)
 *   af.key()      // a tap on a FIT key that lives elsewhere (the app's React Native chip) — as the button's own click
 *   af.fitNow() · af.pause() · af.resume() · af.set(on) · af.state() · af.target() · af.destroy()
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */

/** ADS-B's CAMERA_MS (adsblayer.js: CAMERA_MS = GLIDE_MS = 2200) — the glide's length. */
export const AUTOFIT_CAMERA_MS = 2200;
/** The closest an auto fit ever goes, in Leaflet zoom — ADS-B's ZMAX on Leaflet (11; 10 on the GPU map, one below). */
export const AUTOFIT_MAX_ZOOM = 11;
/** ADS-B's AutoZoom defaults (adsblayer.js AutoZoom) — the hysteresis. */
export const AUTOFIT_HYST = { slack: 0.3, inHyst: 0.6, inHold: 4000, centreFrac: 0.12, centreHold: 3000, minGap: 1500 } as const;

export const MAP_AUTOFIT_JS = `
var VibeAutoFit=(function(){
  var CAMERA_MS=${AUTOFIT_CAMERA_MS},MAX_ZOOM=${AUTOFIT_MAX_ZOOM},MIN_ZOOM=2.5,D2R=Math.PI/180,FIRST_JUMP_MS=1500,TICK_MS=1000;
  var HYST={slack:${AUTOFIT_HYST.slack},inHyst:${AUTOFIT_HYST.inHyst},inHold:${AUTOFIT_HYST.inHold},centreFrac:${AUTOFIT_HYST.centreFrac},centreHold:${AUTOFIT_HYST.centreHold},minGap:${AUTOFIT_HYST.minGap}};
  function nowMs(){return(typeof performance!=='undefined'&&performance.now)?performance.now():Date.now();}
  function wrapLon(lon,ref){var d=lon-(ref||0);d=((d+180)%360+360)%360-180;return(ref||0)+d;}
  function merc(lat,lon){var s=Math.sin(Math.max(-85.05,Math.min(85.05,lat))*D2R);return{x:(lon+180)/360,y:0.5-Math.log((1+s)/(1-s))/(4*Math.PI)};}
  function unmerc(x,y){return{lat:Math.atan(Math.sinh(Math.PI*(1-2*y)))/D2R,lon:x*360-180};}
  function easeInOut(k){return k<0.5?4*k*k*k:1-Math.pow(-2*k+2,3)/2;}
  /* ADS-B's cameraFor, fit:true — the box of the receiver AND the points, centred on the box */
  function cameraFor(o){
    var ts=o.tileSize||512,pad=o.pad!=null?o.pad:48;
    var minZ=o.minZoom!=null?o.minZoom:2,maxZ=o.maxZoom!=null?o.maxZoom:10;
    var pts=o.points||[],rx=o.rx||null;
    function clampZ(z){return Math.max(minZ,Math.min(maxZ,z));}
    if(!pts.length)return null;
    var ref=rx?rx.lon:pts[0].lon;
    var ms=pts.map(function(p){return merc(p.lat,wrapLon(p.lon,ref));});
    var availW=Math.max(40,o.w-2*pad),availH=Math.max(40,o.h-2*pad);
    if(rx)ms.push(merc(rx.lat,rx.lon));
    var x0=Infinity,x1=-Infinity,y0=Infinity,y1=-Infinity;
    ms.forEach(function(m){x0=Math.min(x0,m.x);x1=Math.max(x1,m.x);y0=Math.min(y0,m.y);y1=Math.max(y1,m.y);});
    var dx=x1-x0,dy=y1-y0;
    var zx=dx>0?Math.log(availW/(dx*ts))/Math.LN2:Infinity,zy=dy>0?Math.log(availH/(dy*ts))/Math.LN2:Infinity;
    var c=unmerc((x0+x1)/2,(y0+y1)/2);
    return{center:{lat:c.lat,lon:wrapLon(c.lon,ref)},zoom:clampZ(Math.min(zx,zy))};
  }
  function screenOffset(cam,lat,lon,ts){var ws=(ts||512)*Math.pow(2,cam.zoom);var c=merc(cam.center.lat,cam.center.lon),p=merc(lat,wrapLon(lon,cam.center.lon));return{x:(p.x-c.x)*ws,y:(p.y-c.y)*ws};}
  function anyOutside(points,cam,w,h,pad,ts){var hw=w/2-pad,hh=h/2-pad;for(var i=0;i<points.length;i++){var s=screenOffset(cam,points[i].lat,points[i].lon,ts);if(Math.abs(s.x)>hw||Math.abs(s.y)>hh)return true;}return false;}
  function insetFor(obs,w,h){var z={l:0,r:0,t:0,b:0};if(!obs||!(obs.w>0)||!(obs.h>0)||!(w>0)||!(h>0))return z;
    var c=String(obs.corner||'bottom-left');var side=c.indexOf('right')>=0?'r':'l',end=c.indexOf('top')>=0?'t':'b';
    if(obs.w/w<=obs.h/h)z[side]=Math.min(obs.w,w*0.6);else z[end]=Math.min(obs.h,h*0.6);return z;}
  /* ADS-B's AutoZoom — the decisions, calm by construction */
  function AutoZoom(o){o=o||{};var k;for(k in HYST)this[k]=o[k]!=null?o[k]:HYST[k];this.reset();}
  AutoZoom.prototype.reset=function(){this.inSince=null;this.centreSince=null;this.lastMoveAt=-Infinity;};
  AutoZoom.prototype.decide=function(now,cur,tgt,outside,w,h,ts,busy){
    if(!tgt||!cur)return null;
    if(busy||now-this.lastMoveAt<this.minGap)return null;
    var self=this,goal={center:tgt.center,zoom:tgt.zoom-this.slack};
    function go(g){self.lastMoveAt=now;self.inSince=null;self.centreSince=null;return g;}
    if(outside||tgt.zoom<cur.zoom-0.02)return go(goal);
    if(tgt.zoom>cur.zoom+this.inHyst){if(this.inSince==null)this.inSince=now;if(now-this.inSince>=this.inHold)return go(goal);}
    else this.inSince=null;
    var off=screenOffset(cur,tgt.center.lat,tgt.center.lon,ts);
    if(Math.hypot(off.x,off.y)>this.centreFrac*Math.min(w,h)){if(this.centreSince==null)this.centreSince=now;if(now-this.centreSince>=this.centreHold)return go({center:tgt.center,zoom:cur.zoom});}
    else this.centreSince=null;
    return null;
  };
  var cssDone=false;
  function css(){if(cssDone||typeof document==='undefined')return;cssDone=true;try{var st=document.createElement('style');
    st.textContent='.vaf-key.paused{background-image:repeating-linear-gradient(135deg,rgba(160,160,160,0.22) 0 4px,transparent 4px 8px)!important;opacity:0.9}';
    (document.head||document.documentElement).appendChild(st);}catch(e){}}
  function llOf(p){if(!p)return null;var la=Number(Array.isArray(p)?p[0]:p.lat),lo=Number(Array.isArray(p)?p[1]:(p.lon!=null?p.lon:p.lng));
    if(!isFinite(la)||!isFinite(lo)||la<-90||la>90||lo<-540||lo>540)return null;return{lat:la,lon:lo};}

  function attach(mapIn,o){
    o=o||{};
    var gl=null,lm=null,DZ=0;
    if(mapIn&&mapIn._gl&&typeof mapIn._gl.easeTo==='function'){gl=mapIn._gl;try{DZ=Math.round(mapIn.getZoom()-gl.getZoom());}catch(e){DZ=1;}if(!isFinite(DZ))DZ=1;}
    else if(mapIn&&mapIn.map&&typeof mapIn.map.easeTo==='function'){gl=mapIn.map;DZ=1;}
    else if(mapIn&&typeof mapIn.easeTo==='function'&&typeof mapIn.project==='function'){gl=mapIn;DZ=1;}
    else if(mapIn&&typeof mapIn.getCenter==='function'&&typeof mapIn.flyTo==='function'){lm=mapIn;}
    else throw new Error('autofit: not a map');
    css();
    var TILE=gl?512:256;
    var maxZ=(o.maxZoom!=null?Number(o.maxZoom):MAX_ZOOM)-DZ,minZ=MIN_ZOOM-DZ;
    var container=gl?gl.getContainer():lm.getContainer();
    var btn=o.button||null,what=String(o.what||'station');
    var on=o.on!==false,paused=false,destroyed=false,ourMove=0,owning=false,firstCamera=true,attachedAt=nowMs(),lastInputAt=-Infinity;
    var az=new AutoZoom(o.hyst);
    function viewSize(){return{w:container.clientWidth||600,h:container.clientHeight||400};}
    function curCam(){var c=gl?gl.getCenter():lm.getCenter();return{center:{lat:c.lat,lon:c.lng},zoom:gl?gl.getZoom():lm.getZoom()};}
    function rxNow(){var r=null;try{r=o.receiver?llOf(o.receiver()):null;}catch(e){r=null;}if(r&&r.lat===0&&r.lon===0)r=null;return r;}
    function ptsNow(){var out=[];try{var a=o.points?o.points():[];for(var i=0;i<(a?a.length:0);i++){var p=llOf(a[i]);if(p)out.push(p);}}catch(e){console.error('autofit points',e);}return out;}
    function padPx(){var v=viewSize();return Math.max(36,Math.min(64,Math.min(v.w,v.h)*0.08));}
    function clearRect(){var v=viewSize(),obs=null;try{obs=o.obstacle?o.obstacle():null;}catch(e){obs=null;}var i=insetFor(obs,v.w,v.h);
      return{w:v.w-i.l-i.r,h:v.h-i.t-i.b,dx:(i.l-i.r)/2,dy:(i.t-i.b)/2};}
    function shiftCam(cam,dx,dy){if(!cam||(!dx&&!dy))return cam;var ws=TILE*Math.pow(2,cam.zoom),c=merc(cam.center.lat,cam.center.lon);
      var m=unmerc(c.x-dx/ws,c.y-dy/ws);return{center:{lat:m.lat,lon:wrapLon(m.lon,cam.center.lon)},zoom:cam.zoom};}
    function target(){var pts=ptsNow();if(!pts.length)return null;var r=clearRect();
      var cam=cameraFor({points:pts,rx:rxNow(),w:r.w,h:r.h,pad:padPx(),tileSize:TILE,minZoom:minZ,maxZoom:maxZ});
      return shiftCam(cam,r.dx,r.dy);}
    function moveCamera(cam,ms){
      ourMove++;var fin=false,tid=0;
      function done(){if(fin)return;fin=true;clearTimeout(tid);ourMove=Math.max(0,ourMove-1);}
      if(ms>0)tid=setTimeout(done,ms+600);
      owning=true;
      try{
        if(gl){
          if(ms>0){gl.once('moveend',done);gl.easeTo({center:[cam.center.lon,cam.center.lat],zoom:cam.zoom,duration:ms,easing:easeInOut},{vsfit:true});}
          else{gl.jumpTo({center:[cam.center.lon,cam.center.lat],zoom:cam.zoom},{vsfit:true});done();}
        }else{
          if(ms>0){lm.once('moveend',done);lm.flyTo([cam.center.lat,cam.center.lon],cam.zoom,{duration:ms/1000,easeLinearity:0.25});}
          else{lm.setView([cam.center.lat,cam.center.lon],cam.zoom,{animate:false});done();}
        }
      }catch(e){done();console.error('autofit: camera move failed',e);}
      owning=false;
    }
    function busy(){return ourMove>0||(gl&&gl.isMoving?gl.isMoving():false);}
    /* Leaflet snaps a zoom to whole levels — floor, so the box is never cut by rounding up */
    function snapZ(z){if(lm&&!(lm.options&&lm.options.zoomSnap===0)){var s=(lm.options&&lm.options.zoomSnap)||1;return Math.floor(z/s+0.001)*s;}return z;}
    function same(a,b){if(!a||!b)return false;if(Math.abs(a.zoom-b.zoom)>0.01)return false;var off=screenOffset(a,b.center.lat,b.center.lon,TILE);return Math.hypot(off.x,off.y)<2;}
    /* force = a person asked (the key, resume): go now */
    function step(force){
      if(destroyed)return;
      if(!force&&(!on||paused))return;
      var tgt=target();if(!tgt)return;
      var cur;try{cur=curCam();}catch(e){return;}
      if(firstCamera){
        firstCamera=false;
        var g0={center:tgt.center,zoom:snapZ(tgt.zoom-az.slack)};
        az.lastMoveAt=nowMs();
        if(!same(cur,g0))moveCamera(g0,nowMs()-attachedAt<FIRST_JUMP_MS?0:CAMERA_MS);
        return;
      }
      if(force){az.reset();az.lastMoveAt=nowMs();var gf={center:tgt.center,zoom:snapZ(tgt.zoom-az.slack)};if(!same(cur,gf))moveCamera(gf,CAMERA_MS);return;}
      var v=viewSize(),r=clearRect();
      var out=anyOutside(ptsNow().concat(rxNow()?[rxNow()]:[]),shiftCam(cur,-r.dx,-r.dy),r.w,r.h,padPx()*0.5,TILE);
      var last=az.lastMoveAt;
      var g=az.decide(nowMs(),cur,tgt,out,v.w,v.h,TILE,busy());
      if(g){g={center:g.center,zoom:snapZ(g.zoom)};
        /* a decision that snaps to where the map already is (Leaflet's whole zooms) is not a move: it must not start
         *  the 1.5 s gap and hold back a real one */
        if(same(cur,g))az.lastMoveAt=last;else moveCamera(g,CAMERA_MS);}
    }
    function paint(){
      if(btn){
        btn.classList.add('vaf-key');
        btn.classList.toggle('on',on&&!paused);
        btn.classList.toggle('paused',on&&paused);
        btn.setAttribute('aria-pressed',String(on&&!paused));
        btn.title=!on?'Auto fit is off — tap to keep every '+what+' and the receiver in view'
          :paused?'Auto fit paused because you moved the map — tap to resume'
          :'Auto fit on: every '+what+' and the receiver stay in view — tap to turn it off';
      }
      if(o.onState){try{o.onState({on:on,paused:paused});}catch(e){console.error('autofit onState',e);}}
    }
    function onClick(){
      if(on&&paused){paused=false;az.reset();step(true);}
      else{on=!on;paused=false;az.reset();if(on)step(true);}
      paint();
    }
    if(btn)btn.addEventListener('click',onClick);
    function onInput(e){if(btn&&e&&e.target&&btn.contains&&btn.contains(e.target))return;lastInputAt=nowMs();}
    var EVS=['pointerdown','touchstart','wheel','keydown'];
    EVS.forEach(function(ev){container.addEventListener(ev,onInput,{passive:true,capture:true});});
    function onMoveStart(e){
      if(owning||(e&&e.vsfit))return;
      var person=!!(e&&e.originalEvent)||nowMs()-lastInputAt<1000;
      if(!person)return;
      if(ourMove>0)ourMove=0;
      if(on&&!paused){paused=true;paint();}
    }
    if(gl)gl.on('movestart',onMoveStart);else lm.on('movestart',onMoveStart);
    var tick=setInterval(function(){step(false);},TICK_MS);
    paint();
    return{
      poke:function(){step(false);},
      key:onClick,
      fitNow:function(){if(!on)on=true;paused=false;az.reset();step(true);paint();},
      pause:function(){if(on&&!paused){paused=true;paint();}},
      resume:function(){if(!on||paused){on=true;paused=false;az.reset();step(true);paint();}},
      set:function(v){v=!!v;if(v===on&&!paused)return;on=v;paused=false;az.reset();if(on)step(true);paint();},
      state:function(){return{on:on,paused:paused};},
      target:target,
      destroy:function(){destroyed=true;clearInterval(tick);if(btn)btn.removeEventListener('click',onClick);
        EVS.forEach(function(ev){try{container.removeEventListener(ev,onInput,{capture:true});}catch(e){}});
        try{if(gl)gl.off('movestart',onMoveStart);else lm.off('movestart',onMoveStart);}catch(e){}}
    };
  }
  return{attach:attach,maths:{CAMERA_MS:CAMERA_MS,MAX_ZOOM:MAX_ZOOM,MIN_ZOOM:MIN_ZOOM,HYST:HYST,cameraFor:cameraFor,AutoZoom:AutoZoom,easeInOut:easeInOut,anyOutside:anyOutside,screenOffset:screenOffset,merc:merc,unmerc:unmerc,wrapLon:wrapLon}};
})();
if(typeof window!=='undefined')window.VibeAutoFit=VibeAutoFit;
`;
