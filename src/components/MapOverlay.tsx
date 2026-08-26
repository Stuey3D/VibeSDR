/**
 * MapOverlay — full-screen Leaflet maps for HFDL / Digital spots / CW spots.
 * Ported from skin v6.3.2 (lsv-hfdl / lsv-digmap / lsv-cwmap overlays).
 *
 * Architecture: react-native-webview running a self-contained HTML document.
 * ALL chrome lives inside the WebView (skin layout parity) — one slim topbar
 * `← SDR | TITLE | (SNAP) | count` padded by env(safe-area-inset-*), so
 * landscape wastes no vertical space. The back button posts 'close' to RN.
 *
 * The WebView talks to the UberSDR server directly:
 *   HFDL  — polls {base}/addon/hfdl/aircraft (5s) + /groundstations (piggy-backed)
 *   DIGI/CW — own /ws/dxcluster socket. The server REQUIRES the registered
 *     session UUID (validated against /connection + User-Agent map), so the
 *     app's live sessionUuid is injected — random IDs are rejected with 400.
 *   RX position — {base}/api/description → receiver.gps
 *
 * Skin parity per map:
 *   HFDL (amber): lattice-tower GS icons, AC table popups (Alt row dropped —
 *     never populates upstream — replaced by Tracked = tracked_km), latest-
 *     flight toast, pulse glow + expanding rings, SNAP, ⓘ legend.
 *   DIGI/CW (green): band-coloured circle markers sized by SNR, filters row,
 *     ⓘ scrolling legend with arrows, 📊 stats sheet (range/countries/bands/
 *     modes with bar charts), new-spot toast, country abbreviations.
 *
 * GPU acceleration: WKWebView composites Leaflet's translate3d pan/zoom;
 * HFDL markers glide between polls on a CSS transform transition; pulse and
 * rings are pure CSS keyframes.
 */

import React, { useMemo, useRef, useEffect, useState } from 'react';
import { Modal, StyleSheet, View, Text, TouchableOpacity } from 'react-native';
import { useRepeatingKeys } from './PanelNav';
import { WebView } from 'react-native-webview';
import { CABBR } from '../assets/countryAbbr';
import { type SpotRow } from '../services/DecoderClient';

export type MapKind = 'hfdl' | 'digi' | 'cw';

interface MapOverlayProps {
  visible: boolean;
  kind:    MapKind | null;
  baseUrl: string;          // http(s)://host — used for fetch + ws URLs
  /** Registered session UUID — required by /ws/dxcluster validation. */
  sessionUuid: string;
  onClose: () => void;
  /** On-device-decoder mode (Local/Kiwi): spots come from the localhost shim's
   *  /ws/dxcluster carrying a Maidenhead grid (not lat/lon, no /api/description),
   *  so the WebView converts grid→lat/lon and the RX position is injected. */
  local?:   boolean;
  /** ws://127.0.0.1:<port> of the local shim/sidecar (local mode). */
  wsBaseOverride?: string | null;
  /** Injected receiver position (Kiwi gps / device GPS / picked city). 0,0 =
   *  Null Island = location unknown → the map offers a "set location" button. */
  rxLat?:   number | null;
  rxLon?:   number | null;
  /** local mode: user tapped "set location" (no GPS) → open the RN city picker. */
  onPickCity?: () => void;
  /** Server (Kiwi) dropped/kicked us while the map is open → show a warning over
   *  the map with Back-to-instances / Retry / Ignore (stay on the map). */
  disconnected?: boolean;
  onBackToList?: () => void;
  onRetry?: () => void;
}

// ── HTML document builder ─────────────────────────────────────────────────────

function buildHtml(
  kind: MapKind, baseUrl: string, uuid: string,
  opts?: { local?: boolean; wsBase?: string | null; rxLat?: number | null; rxLon?: number | null },
): string {
  const local = !!opts?.local;
  const base = baseUrl.replace(/\/+$/, '');
  const wsBase = local && opts?.wsBase ? opts.wsBase.replace(/\/+$/, '') : base.replace(/^http/, 'ws');
  const rxLat = opts?.rxLat ?? 0;
  const rxLon = opts?.rxLon ?? 0;
  const isHfdl = kind === 'hfdl';
  // Theme: HFDL amber (255,160,0), spots maps green (80,200,80) — skin parity.
  const T = isHfdl
    ? { bg: '#090602', a: '255,160,0', hi: '255,200,80', txt: '255,180,60' }
    : { bg: '#060906', a: '80,200,80', hi: '120,240,120', txt: '120,240,120' };
  const TITLE = isHfdl ? 'HFDL AIRCRAFT MAP' : kind === 'digi' ? 'DIGITAL SPOTS MAP' : 'CW SPOTS MAP';
  const CNT_ICON = isHfdl
    ? `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 26 26" style="vertical-align:middle;opacity:0.75"><path d="M13,1 L15,8 L23,11 L23,13 L15,11 L16,20 L19,21 L19,23 L13,21 L7,23 L7,21 L10,20 L11,11 L3,13 L3,11 L11,8 Z" fill="rgba(255,190,40,0.9)"/></svg>`
    : kind === 'cw'
    ? `<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11" viewBox="0 0 13 11" style="vertical-align:middle;margin-right:2px;opacity:0.8;color:rgba(${T.hi},0.9)"><rect x="1" y="1" width="11" height="4" rx="1" fill="currentColor" opacity="0.9"/><line x1="6.5" y1="5" x2="6.5" y2="8" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><rect x="3" y="8" width="7" height="2" rx="1" fill="currentColor" opacity="0.7"/></svg>`
    : '&#128225;';

  return `<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
  html,body{margin:0;padding:0;height:100%;background:${T.bg};overflow:hidden;}
  body{display:flex;flex-direction:column;font-family:'Courier New',monospace;}
  /* ── topbar — skin lsv-hfdl-topbar / lsv-smap-topbar (single slim row) ── */
  #topbar{display:flex;align-items:center;gap:8px;padding:6px 10px;
    padding-left:max(10px,env(safe-area-inset-left));padding-right:max(10px,env(safe-area-inset-right));
    padding-top:max(8px,env(safe-area-inset-top));
    background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.97);border-bottom:1px solid rgba(${T.a},0.18);flex-shrink:0;z-index:2;}
  #back{background:rgba(${T.a},0.08);border:1px solid rgba(${T.a},0.30);border-radius:8px;
    color:rgba(${T.a},0.85);font-family:inherit;font-size:12px;letter-spacing:1px;padding:5px 10px;
    cursor:pointer;flex-shrink:0;-webkit-tap-highlight-color:transparent;touch-action:manipulation;}
  #back:active{background:rgba(${T.a},0.18);}
  #title{font-size:13px;letter-spacing:2px;color:rgba(${T.a},0.80);flex:1;text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
  #cntwrap{display:flex;align-items:center;gap:5px;font-size:11px;letter-spacing:1px;color:rgba(${T.a},0.60);flex-shrink:0;}
  #cnt{color:rgba(${T.hi},0.90);font-size:14px;}
  #snap{background:rgba(255,160,0,0.08);border:1px solid rgba(255,160,0,0.20);border-radius:8px;
    color:rgba(255,160,0,0.45);font-size:10px;letter-spacing:0.5px;padding:5px 7px;cursor:pointer;
    font-family:inherit;-webkit-tap-highlight-color:transparent;touch-action:manipulation;line-height:1.2;text-align:center;flex-shrink:0;}
  #snap.on{border-color:rgba(255,160,0,0.50);color:rgba(255,190,60,0.90);background:rgba(255,160,0,0.10);}
  /* ── filters row (digi/cw) — skin lsv-smap-filters ── */
  #filters{display:flex;align-items:center;gap:6px;padding:4px 10px;
    padding-left:max(10px,env(safe-area-inset-left));padding-right:max(10px,env(safe-area-inset-right));
    background:rgba(6,9,6,0.97);border-bottom:1px solid rgba(${T.a},0.10);flex-shrink:0;flex-wrap:wrap;}
  select{background:rgba(${T.a},0.06);border:1px solid rgba(${T.a},0.25);border-radius:6px;
    color:rgba(${T.a},0.85);font-family:inherit;font-size:10px;letter-spacing:0.5px;padding:4px 20px 4px 7px;cursor:pointer;
    appearance:none;-webkit-appearance:none;
    background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='6' viewBox='0 0 10 6'%3E%3Cpath fill='rgba(${T.a},0.6)' d='M0 0l5 6 5-6z'/%3E%3C/svg%3E");
    background-repeat:no-repeat;background-position:right 6px center;}
  .flabel{font-size:9px;letter-spacing:1px;color:rgba(${T.a},0.35);}
  #map{flex:1;position:relative;overflow:hidden;background:${T.bg};}
  #lmap{position:absolute;inset:0;background:${T.bg};}
  /* ── GPU hints — markers glide between updates on the compositor ── */
  .leaflet-pane{will-change:transform;}
  .leaflet-marker-icon.glide{transition:transform 2.2s linear;will-change:transform;}
  body.nog .leaflet-marker-icon.glide{transition:none;}
  .leaflet-fade-anim .leaflet-tile{will-change:opacity;}
  /* skin map theming */
  .leaflet-tile{filter:brightness(0.65) saturate(0.7);}
  .leaflet-popup-content-wrapper{background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.96);border:1px solid rgba(${T.a},0.30);color:rgba(${T.txt},0.90);font-family:'Courier New',monospace;font-size:11px;border-radius:8px;}
  .leaflet-popup-tip{background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.96);}
  .leaflet-popup-close-button{color:rgba(${T.a},0.50)!important;}
  .leaflet-popup-content{margin:10px 12px;font-family:'Courier New',monospace;}
  .leaflet-control-zoom a{background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.90);color:rgba(${T.a},0.70);border-color:rgba(${T.a},0.20);}
  .leaflet-bottom{padding-bottom:max(8px,env(safe-area-inset-bottom));}
  .leaflet-control-attribution{background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.70);color:rgba(${T.a},0.25);font-size:8px;}
  /* skin lsv-hfdl pulse + rings (verbatim keyframes) */
  @keyframes pulseac{0%,100%{filter:drop-shadow(0 0 3px rgba(255,255,80,0.5));}50%{filter:drop-shadow(0 0 10px rgba(255,255,80,1)) drop-shadow(0 0 18px rgba(255,200,0,0.7));}}
  @keyframes pulsegs{0%,100%{filter:drop-shadow(0 0 3px rgba(80,200,255,0.5));}50%{filter:drop-shadow(0 0 10px rgba(80,220,255,1)) drop-shadow(0 0 18px rgba(0,180,255,0.7));}}
  .pulse-ac{animation:pulseac 1.2s ease-in-out infinite;}
  .pulse-gs{animation:pulsegs 1.2s ease-in-out infinite;}
  @keyframes ring{0%{transform:translate(-50%,-50%) scale(1);opacity:0.8;}100%{transform:translate(-50%,-50%) scale(3);opacity:0;}}
  .ring{position:absolute;pointer-events:none;z-index:650;width:26px;height:26px;border-radius:50%;border:2px solid;transform:translate(-50%,-50%);animation:ring 1.4s ease-out 3;will-change:transform,opacity;}
  .ring-ac{border-color:rgba(255,255,100,0.85);}
  .ring-gs{border-color:rgba(80,220,255,0.85);}
  /* ── toast — skin lsv-hfdl-toast / lsv-smap-toast ── */
  #toast{position:absolute;bottom:max(10px,env(safe-area-inset-bottom));left:50%;transform:translateX(-50%);
    z-index:1000;background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.93);border:1px solid rgba(${T.a},0.30);border-radius:20px;
    padding:6px 14px;font-size:11px;letter-spacing:1px;
    color:rgba(${T.hi},0.90);white-space:nowrap;opacity:0;transition:opacity 0.4s;max-width:90vw;
    overflow:hidden;text-overflow:ellipsis;cursor:pointer;}
  #toast.on{opacity:1;}
  .tf{color:#ffe566;font-size:12px;}
  .tg{color:rgba(255,160,0,0.55);}
  .ta{color:rgba(255,160,0,0.40);font-size:10px;}
  /* ── legend — skin lsv-smap-legend / lsv-hfdl-legend ── */
  #legend{position:absolute;bottom:max(44px,calc(env(safe-area-inset-bottom) + 34px));left:max(10px,env(safe-area-inset-left));
    z-index:1000;font-size:10px;letter-spacing:0.8px;color:rgba(${T.hi},0.80);pointer-events:auto;}
  #legbtn{display:flex;align-items:center;justify-content:center;width:28px;height:28px;
    background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.88);border:1px solid rgba(${T.a},0.30);border-radius:8px;cursor:pointer;
    font-size:14px;line-height:1;-webkit-tap-highlight-color:transparent;touch-action:manipulation;color:rgba(${T.hi},0.80);}
  #legbtn:active{background:rgba(${T.a},0.15);}
  #legbody{display:none;position:absolute;bottom:0;left:34px;background:rgba(${isHfdl ? '9,6,2' : '6,9,6'},0.92);
    border:1px solid rgba(${T.a},0.18);border-radius:8px;padding:7px 9px;white-space:nowrap;
    max-height:min(320px,calc(100vh - 200px));overflow-y:scroll;overflow-x:hidden;
    -webkit-overflow-scrolling:touch;overscroll-behavior:contain;touch-action:pan-y;}
  #legend.open #legbody{display:block;}
  #legbody::-webkit-scrollbar{width:4px;}
  #legbody::-webkit-scrollbar-track{background:transparent;}
  #legbody::-webkit-scrollbar-thumb{background:rgba(${T.a},0.40);border-radius:2px;}
  .lg-row{display:flex;align-items:center;gap:8px;margin:3px 0;font-size:10px;}
  .lg-dot{display:inline-block;width:10px;height:10px;border-radius:50%;flex-shrink:0;}
  .lg-sec{font-size:9px;letter-spacing:1.5px;color:rgba(${T.a},0.40);margin:6px 0 3px;}
  .scroll-arrow{display:none;position:sticky;left:0;right:0;text-align:center;font-size:11px;line-height:1;padding:2px 0;pointer-events:none;color:rgba(${T.hi},0.90);}
  .scroll-arrow-up{top:0;}
  .scroll-arrow-dn{bottom:0;}
  .scroll-arrow.arr-on{display:block;}
  /* ── stats (digi/cw) — skin lsv-st ── */
  #statsbtnwrap{position:absolute;bottom:max(120px,calc(env(safe-area-inset-bottom) + 110px));right:max(10px,env(safe-area-inset-right));
    z-index:1000;pointer-events:auto;}
  #statsbtn{display:flex;align-items:center;justify-content:center;width:28px;height:28px;
    background:rgba(6,9,6,0.88);border:1px solid rgba(${T.a},0.30);border-radius:8px;cursor:pointer;
    font-size:13px;line-height:1;-webkit-tap-highlight-color:transparent;touch-action:manipulation;color:rgba(${T.hi},0.80);}
  #statsbtn:active{background:rgba(${T.a},0.15);}
  #stsheet{display:none;position:absolute;inset:0;z-index:1500;background:rgba(4,12,4,0.82);
    color:rgba(${T.hi},0.80);font-size:10px;letter-spacing:0.8px;
    backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);
    overflow-y:scroll;overflow-x:hidden;-webkit-overflow-scrolling:touch;overscroll-behavior:contain;touch-action:pan-y;
    padding:16px max(16px,env(safe-area-inset-right)) max(16px,env(safe-area-inset-bottom)) max(16px,env(safe-area-inset-left));
    scrollbar-width:thin;scrollbar-color:rgba(${T.a},0.50) rgba(${T.a},0.10);}
  #stsheet::-webkit-scrollbar{width:5px;}
  #stsheet::-webkit-scrollbar-track{background:rgba(${T.a},0.08);border-radius:3px;}
  #stsheet::-webkit-scrollbar-thumb{background:rgba(${T.a},0.50);border-radius:3px;min-height:40px;}
  #stsheet.open{display:block;}
  .st-close{position:sticky;top:0;float:right;background:rgba(${T.a},0.12);border:1px solid rgba(${T.a},0.35);
    border-radius:8px;color:rgba(${T.hi},0.85);font-family:inherit;font-size:11px;letter-spacing:1px;padding:5px 10px;
    cursor:pointer;-webkit-tap-highlight-color:transparent;touch-action:manipulation;margin-bottom:8px;z-index:10;}
  .st-title{font-size:13px;letter-spacing:2px;color:rgba(${T.a},0.70);margin-bottom:12px;padding-top:2px;}
  .st-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;max-width:700px;margin:0 auto;}
  @media(max-width:480px){.st-grid{grid-template-columns:1fr;}}
  .st-card{background:rgba(6,9,6,0.60);border:1px solid rgba(${T.a},0.12);border-radius:8px;padding:10px 12px;}
  .st-sect{font-size:9px;letter-spacing:1.5px;color:rgba(${T.a},0.45);margin:0 0 6px;}
  .st-row{display:flex;align-items:baseline;justify-content:space-between;gap:8px;margin:4px 0;font-size:11px;}
  .st-val{color:rgba(${T.hi},0.95);font-size:12px;font-weight:bold;}
  .st-sub{font-size:9px;color:rgba(${T.a},0.45);letter-spacing:0.5px;margin:-2px 0 6px;}
  .st-bar{height:3px;background:rgba(${T.a},0.12);border-radius:2px;margin:2px 0 6px;}
  .st-bf{height:3px;background:rgba(${T.a},0.65);border-radius:2px;transition:width 0.4s;}
</style>
</head><body>
<div id="topbar">
  <button id="back">&#8592; SDR</button>
  <div id="title">${TITLE}</div>
  ${isHfdl ? `<button id="snap" title="Toggle auto-snap to latest aircraft">&#9881;<br>SNAP</button>` : ''}
  <div id="cntwrap">${CNT_ICON}<span id="cnt">&#8230;</span></div>
</div>
${!isHfdl ? `
<div id="filters">
  <span class="flabel">FILTER</span>
  ${kind === 'digi' ? `<select id="fMode"><option value="ALL">All</option><option>FT8</option><option>FT4</option><option>FT2</option><option>WSPR</option><option>JS8</option></select>` : ''}
  <select id="fBand"><option value="ALL">All</option><option>160m</option><option>80m</option><option>60m</option><option>40m</option><option>30m</option><option>20m</option><option>17m</option><option>15m</option><option>12m</option><option>10m</option></select>
  <select id="fAge">${kind === 'digi'
    ? `<option value="ALL">All</option><option value="2m" selected>2 min</option><option value="5m">5 min</option><option value="15m">15 min</option><option value="30m">30 min</option><option value="1h">1 hour</option>`
    : `<option value="ALL">All</option><option value="15m" selected>15 min</option><option value="30m">30 min</option><option value="1h">1 hour</option><option value="3h">3 hours</option>`}</select>
</div>` : ''}
<div id="map">
  <div id="lmap"></div>
  <div id="legend">
    <div id="legbtn" title="Legend">&#9432;</div>
    <div id="legbody">
      <div class="scroll-arrow scroll-arrow-up" id="arr-up">&#9650;</div>
      ${isHfdl ? `
      <div class="lg-sec">AIRCRAFT AGE</div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(80,255,120,0.92);"></span><span>&lt; 2 min</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(180,255,80,0.88);"></span><span>&lt; 5 min</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(255,190,40,0.85);"></span><span>&lt; 15 min</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(255,80,60,0.75);"></span><span>older</span></div>
      <div class="lg-sec">GND STATION</div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(60,220,80,0.92);"></span><span>strong</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(220,200,40,0.92);"></span><span>medium</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(255,100,60,0.85);"></span><span>weak</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(120,120,140,0.60);"></span><span>no sig</span></div>
      ` : `
      <div class="lg-sec">BAND COLOUR</div>
      <div class="lg-row"><span class="lg-dot" style="background:#9b30d9;"></span><span>2200m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#c71585;"></span><span>630m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#e8001e;"></span><span>160m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ff5500;"></span><span>80m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ff8c00;"></span><span>60m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ffd700;"></span><span>40m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#aacc00;"></span><span>30m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#00cc44;"></span><span>20m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#00ccaa;"></span><span>17m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#00aaff;"></span><span>15m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#0055ff;"></span><span>12m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#6600ff;"></span><span>11m</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#cc00cc;"></span><span>10m</span></div>
      <div class="lg-sec">SNR / SIZE</div>
      <div class="lg-row"><span class="lg-dot" style="width:14px;height:14px;background:rgba(255,255,255,0.5);box-shadow:0 0 6px 3px rgba(255,255,255,0.4);"></span><span>Strong (&ge;0 dB)</span></div>
      <div class="lg-row"><span class="lg-dot" style="width:9px;height:9px;background:rgba(255,255,255,0.5);"></span><span>Medium (-20 to 0)</span></div>
      <div class="lg-row"><span class="lg-dot" style="width:6px;height:6px;background:rgba(255,255,255,0.3);"></span><span>Weak (&lt; -20 dB)</span></div>
      `}
      <div class="scroll-arrow scroll-arrow-dn" id="arr-dn">&#9660;</div>
    </div>
  </div>
  ${!isHfdl ? `
  <div id="statsbtnwrap"><div id="statsbtn" title="Stats">&#128202;</div></div>
  <div id="stsheet">
    <button class="st-close" id="stclose">&#10005; CLOSE</button>
    <div class="st-title">${kind === 'digi' ? 'DIGITAL' : 'CW'} SPOTS &mdash; STATISTICS</div>
    <div class="st-grid">
      <div class="st-card">
        <div class="st-sect">RANGE</div>
        <div class="st-row"><span>Closest</span><span class="st-val" id="s-close">&mdash;</span></div>
        <div class="st-sub" id="s-close-sub"></div>
        <div class="st-row"><span>Furthest</span><span class="st-val" id="s-far">&mdash;</span></div>
        <div class="st-sub" id="s-far-sub"></div>
      </div>
      <div class="st-card">
        <div class="st-sect">TOP COUNTRIES</div>
        <div id="s-countries"></div>
      </div>
      <div class="st-card">
        <div class="st-sect">SPOTS BY BAND</div>
        <div id="s-bands"></div>
      </div>
      ${kind === 'digi' ? `
      <div class="st-card">
        <div class="st-sect">SPOTS BY MODE</div>
        <div id="s-modes"></div>
      </div>` : ''}
    </div>
  </div>` : ''}
  <div id="toast"></div>
</div>
<script>
var BASE='${base}', WSBASE='${wsBase}', KIND='${kind}', UUID='${uuid}';
var LOCAL=${local ? 1 : 0};
var RX_LAT=${rxLat || 0}, RX_LON=${rxLon || 0};
// Maidenhead locator → square-centre lat/lon (on-device FT8 spots carry a grid,
// not lat/lon). 4- or 6-char; returns null if invalid.
function gridLL(g){
  if(!g)return null; g=String(g).trim().toUpperCase();
  if(!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(g))return null;
  var lon=(g.charCodeAt(0)-65)*20-180, lat=(g.charCodeAt(1)-65)*10-90;
  lon+=(g.charCodeAt(2)-48)*2; lat+=(g.charCodeAt(3)-48);
  if(g.length===6){lon+=(g.charCodeAt(4)-65)*(2/24)+(1/24);lat+=(g.charCodeAt(5)-65)*(1/24)+(0.5/24);}
  else{lon+=1;lat+=0.5;}
  return{lat:lat,lon:lon};
}
var CABBR=${JSON.stringify(CABBR)};
function abbr(c){if(!c)return'';var s=String(c).trim();if(CABBR[s])return CABBR[s];return s.length>10?s.substring(0,9)+'\\u2026':s;}
// preferCanvas: the SPOT layer is circleMarkers, one SVG node each, all repositioned on every
// pan frame — and unlike the browser this WebView is on the phone doing the viewing, which is
// the device least able to afford it. Canvas draws them into one element for the same picture.
// ★ It changes VECTOR layers only: the aircraft and ground-station divIcons stay DOM, which is
//   what they must be — they carry the CSS glide and pulse animations.
var map=L.map('lmap',{zoomControl:false,attributionControl:true,fadeAnimation:true,zoomAnimation:true,markerZoomAnimation:true,preferCanvas:true})
  .setView([30,0],2);
L.control.zoom({position:'bottomright'}).addTo(map);
// keepBuffer holds a wider skirt of tiles so an ordinary drag never reaches bare canvas;
// updateWhenZooming stops Leaflet firing requests it will throw away mid-animation.
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  {attribution:'&copy; OSM',maxZoom:14,keepBuffer:4,updateWhenZooming:false}).addTo(map);
var cnt=document.getElementById('cnt');
var toast=document.getElementById('toast');

document.getElementById('back').addEventListener('click',function(){
  if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage('close');
});

// Legend toggle + scroll arrows (skin lsvScrollArrows)
var legEl=document.getElementById('legend'), legBody=document.getElementById('legbody');
function updArrows(el,upId,dnId){
  var atTop=el.scrollTop<4, atBot=el.scrollTop+el.clientHeight>=el.scrollHeight-4;
  var u=document.getElementById(upId), d=document.getElementById(dnId);
  if(u)u.classList.toggle('arr-on',!atTop);
  if(d)d.classList.toggle('arr-on',!atBot);
}
legBody.addEventListener('scroll',function(){updArrows(legBody,'arr-up','arr-dn');},{passive:true});
legBody.addEventListener('touchstart',function(e){e.stopPropagation();},{passive:true});
legBody.addEventListener('touchmove',function(e){e.stopPropagation();},{passive:true});
document.getElementById('legbtn').addEventListener('click',function(e){
  e.stopPropagation();
  legEl.classList.toggle('open');
  if(legEl.classList.contains('open'))setTimeout(function(){updArrows(legBody,'arr-up','arr-dn');},20);
});

// Marker glide must not fight Leaflet's zoom repositioning
map.on('zoomstart',function(){document.body.classList.add('nog');});
map.on('zoomend',function(){setTimeout(function(){document.body.classList.remove('nog');},60);});
// Orientation change → relayout
window.addEventListener('resize',function(){setTimeout(function(){try{map.invalidateSize();}catch(e){}},120);});

function age(ms){var s=Math.floor((Date.now()-ms)/1000);if(s<5)return'just now';if(s<60)return s+'s ago';var m=Math.floor(s/60);return m<60?m+'m ago':Math.floor(m/60)+'h ago';}
function distKm(lat,lon){if(!RX_LAT&&!RX_LON)return null;var R=6371,r=Math.PI/180;var dLat=(lat-RX_LAT)*r,dLon=(lon-RX_LON)*r;var a=Math.sin(dLat/2)*Math.sin(dLat/2)+Math.cos(RX_LAT*r)*Math.cos(lat*r)*Math.sin(dLon/2)*Math.sin(dLon/2);return Math.round(R*2*Math.atan2(Math.sqrt(a),Math.sqrt(1-a)));}

var RX_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 22 22"><polygon points="11,1 21,10 17,10 17,21 13,21 13,15 9,15 9,21 5,21 5,10 1,10" fill="rgba(255,200,60,0.85)" stroke="rgba(0,0,0,0.5)" stroke-width="1"/></svg>';
function addRx(){if(RX_LAT||RX_LON){L.marker([RX_LAT,RX_LON],{icon:L.divIcon({html:RX_SVG,className:'',iconSize:[22,22],iconAnchor:[11,22]}),zIndexOffset:1000}).addTo(map).bindPopup('<div style="font-size:11px;letter-spacing:1px;">&#127968; Your receiver</div>');map.setView([RX_LAT,RX_LON],KIND==='hfdl'?4:3);}}

// Local/Kiwi (on-device decoder): RX position is injected (no /api/description).
// 0,0 = Null Island = unknown → offer a "set location" button that asks RN for a
// city so distances are at least roughly meaningful.
function showSetLoc(){
  var b=document.createElement('button');
  b.textContent='\\uD83D\\uDCCD SET LOCATION';
  b.style.cssText='position:absolute;left:50%;bottom:18px;transform:translateX(-50%);z-index:900;background:rgba(6,9,6,0.92);color:rgba('+'${T.hi}'+',0.95);border:1px solid rgba('+'${T.a}'+',0.5);border-radius:8px;padding:9px 16px;font-family:inherit;font-size:12px;letter-spacing:1px;';
  b.addEventListener('click',function(){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage('pickCity');});
  document.getElementById('map').appendChild(b);
}
if(LOCAL){
  if(RX_LAT||RX_LON)addRx(); else showSetLoc();
}else{
  fetch(BASE+'/api/description').then(function(r){return r.json();}).then(function(d){
    var gps=d&&d.receiver&&d.receiver.gps;
    if(gps&&(gps.lat||gps.lon)){RX_LAT=gps.lat;RX_LON=gps.lon;}
    addRx();
  }).catch(function(){});
}

// ════════ HFDL (skin lsv-hfdl parity) ════════
if(KIND==='hfdl'){
  var gsM={}, acM={}, gsNames={};
  var glowGS=null, glowAC=null, lastFlight=null, pendingFit=null;
  var ageTimer=null;
  var snapBtn=document.getElementById('snap');
  var snapOn=(function(){try{var v=localStorage.getItem('lsv_hfdl_snap');return v===null?true:v==='1';}catch(e){return true;}})();
  function updSnap(){snapBtn.classList.toggle('on',snapOn);}
  updSnap();
  snapBtn.addEventListener('click',function(){snapOn=!snapOn;try{localStorage.setItem('lsv_hfdl_snap',snapOn?'1':'0');}catch(e){}updSnap();});

  function acColour(lastSeen,glow){if(glow)return'rgba(255,240,60,1)';var a=Math.floor(Date.now()/1000)-lastSeen;
    if(a<120)return'rgba(80,255,120,0.92)';if(a<300)return'rgba(180,255,80,0.88)';
    if(a<900)return'rgba(255,190,40,0.85)';return'rgba(255,80,60,0.75)';}

  // skin _acSVG: fuselage ellipse + swept wings + tail, rotated, glow shadow
  function acSVG(glow,bearing,lastSeen){
    var c=acColour(lastSeen||0,glow);
    var f=glow?'filter:drop-shadow(0 0 5px rgba(255,240,60,0.85));':'';
    return'<svg xmlns="http://www.w3.org/2000/svg" width="26" height="26" viewBox="0 0 26 26" style="'+f+'">'
    +'<g transform="rotate('+(bearing||0)+',13,13)">'
    +'<ellipse cx="13" cy="13" rx="2.5" ry="9" fill="'+c+'" stroke="rgba(0,0,0,0.3)" stroke-width="0.5"/>'
    +'<polygon points="13,10 24,17 24,19 13,15 2,19 2,17" fill="'+c+'" stroke="rgba(0,0,0,0.3)" stroke-width="0.5"/>'
    +'<polygon points="13,20 17,24 17,25 13,23 9,25 9,24" fill="'+c+'" stroke="rgba(0,0,0,0.3)" stroke-width="0.5"/>'
    +'</g></svg>';
  }

  // skin _gsSVG verbatim: lattice radio tower (mast + legs + cross braces +
  // ground line) with radiating wave arcs off the antenna tip, ✕ when silent
  function gsSVG(glow,sig){
    var c,xOver=false;
    if(glow){c='rgba(255,240,60,1)';}
    else if(sig==null||sig===0){c='rgba(120,120,140,0.60)';xOver=true;}
    else if(sig>-40){c='rgba(60,220,80,0.92)';}
    else if(sig>-55){c='rgba(220,200,40,0.92)';}
    else{c='rgba(255,100,60,0.85)';}
    var f=glow?'filter:drop-shadow(0 0 5px rgba(255,240,60,0.85));':'';
    var svg='<svg xmlns="http://www.w3.org/2000/svg" width="30" height="36" viewBox="0 0 30 36" style="'+f+'">'
    +'<path d="M10,11 A7,7 0 0,0 10,1" fill="none" stroke="'+c+'" stroke-width="2" stroke-linecap="round"/>'
    +'<path d="M20,11 A7,7 0 0,1 20,1" fill="none" stroke="'+c+'" stroke-width="2" stroke-linecap="round"/>'
    +'<path d="M7,14 A11,11 0 0,0 7,-2" fill="none" stroke="'+c+'" stroke-width="1.3" stroke-linecap="round" opacity="0.55"/>'
    +'<path d="M23,14 A11,11 0 0,1 23,-2" fill="none" stroke="'+c+'" stroke-width="1.3" stroke-linecap="round" opacity="0.55"/>'
    +'<circle cx="15" cy="6" r="2.8" fill="'+c+'"/>'
    +'<line x1="15" y1="6" x2="15" y2="30" stroke="'+c+'" stroke-width="1.8" stroke-linecap="round"/>'
    +'<line x1="15" y1="8" x2="4" y2="30" stroke="'+c+'" stroke-width="1.8" stroke-linecap="round"/>'
    +'<line x1="15" y1="8" x2="26" y2="30" stroke="'+c+'" stroke-width="1.8" stroke-linecap="round"/>'
    +'<line x1="8" y1="19" x2="22" y2="19" stroke="'+c+'" stroke-width="1.4" stroke-linecap="round"/>'
    +'<line x1="5" y1="26" x2="25" y2="26" stroke="'+c+'" stroke-width="1.4" stroke-linecap="round"/>'
    +'<line x1="8" y1="19" x2="15" y2="26" stroke="'+c+'" stroke-width="1" stroke-linecap="round" opacity="0.7"/>'
    +'<line x1="22" y1="19" x2="15" y2="26" stroke="'+c+'" stroke-width="1" stroke-linecap="round" opacity="0.7"/>'
    +'<line x1="5" y1="26" x2="15" y2="30" stroke="'+c+'" stroke-width="1" stroke-linecap="round" opacity="0.7"/>'
    +'<line x1="25" y1="26" x2="15" y2="30" stroke="'+c+'" stroke-width="1" stroke-linecap="round" opacity="0.7"/>'
    +'<line x1="1" y1="30" x2="29" y2="30" stroke="'+c+'" stroke-width="2.2" stroke-linecap="round"/>'
    +'<line x1="4" y1="30" x2="2" y2="34" stroke="'+c+'" stroke-width="1.8" stroke-linecap="round"/>'
    +'<line x1="26" y1="30" x2="28" y2="34" stroke="'+c+'" stroke-width="1.8" stroke-linecap="round"/>';
    if(xOver){
      svg+='<circle cx="24" cy="30" r="6" fill="rgba(9,6,2,0.90)" stroke="rgba(200,40,40,0.90)" stroke-width="1.2"/>'
      +'<line x1="20.5" y1="26.5" x2="27.5" y2="33.5" stroke="rgba(220,50,50,0.95)" stroke-width="1.8" stroke-linecap="round"/>'
      +'<line x1="27.5" y1="26.5" x2="20.5" y2="33.5" stroke="rgba(220,50,50,0.95)" stroke-width="1.8" stroke-linecap="round"/>';
    }
    return svg+'</svg>';
  }
  function icon(svg,w,h,ax,ay){return L.divIcon({html:svg,className:'glide',iconSize:[w,h],iconAnchor:[ax,ay],popupAnchor:[0,-ay]});}

  // skin _sigMeterHTML: 3 signal bars
  function sigBars(dbfs){if(dbfs==null||dbfs===0)return 0;if(dbfs>-40)return 3;if(dbfs>-55)return 2;return 1;}
  function sigMeterHTML(dbfs){
    var bars=sigBars(dbfs);
    if(bars===0)return'<span style="font-size:9px;color:rgba(255,60,60,0.8);letter-spacing:0.5px;">&#10005; NO SIG</span>';
    var colours=['rgba(255,60,60,0.85)','rgba(255,200,40,0.85)','rgba(60,220,80,0.85)'];
    var heights=[6,9,12];
    var h='<span style="display:inline-flex;align-items:flex-end;gap:2px;margin-left:2px;">';
    for(var i=0;i<3;i++){
      var col=i<bars?colours[bars-1]:'rgba(255,255,255,0.12)';
      h+='<span style="display:inline-block;width:5px;height:'+heights[i]+'px;background:'+col+';border-radius:1px;"></span>';
    }
    return h+'</span>';
  }

  // skin rings — overlay-pane divs, CSS animation, self-removing
  var ringAC=null, ringGS=null;
  function placeRing(latlng,cls){
    var pane=map.getPane('overlayPane');if(!pane)return null;
    var el=document.createElement('div');
    el.className='ring '+cls;
    pane.appendChild(el);
    try{var p=map.latLngToLayerPoint(latlng);el.style.left=p.x+'px';el.style.top=p.y+'px';}catch(e){}
    el._tid=setTimeout(function(){if(el.parentNode)el.parentNode.removeChild(el);},4400);
    return el;
  }
  function removeRing(el){if(!el)return;clearTimeout(el._tid);if(el.parentNode)el.parentNode.removeChild(el);}

  /* ★★★ WARM THE DESTINATION TILES BEFORE THE FLIGHT, NOT AFTER IT. Leaflet asks for tiles when
   *     it ARRIVES, so a flyTo lands on bare canvas and fills in square by square — the animation
   *     is smooth and the map underneath it is not. Nothing here changes the animation; it just
   *     makes sure the pictures are already in the browser cache when it gets there.
   * ★★ IT MUST NEVER BLOCK THE FLIGHT. The wait is capped, and the fly starts on whichever comes
   *    first — tiles ready, or the cap. A slow or absent network therefore costs the old behaviour
   *    and nothing worse; it can delay the animation, never prevent it. 'fired' guards the cap and
   *    the last image racing each other.
   * ★ flyTo arcs OUT and back IN, so the lower zooms are on the path too — they are a handful of
   *   tiles each (a whole level up is 4x the ground per tile) and cheap to fetch. */
  function preloadTiles(centre,zoom,done){
    zoom=Math.max(0,Math.min(14,Math.round(zoom)));
    var size=map.getSize(),pending=0,fired=false;
    function fin(){if(fired)return;fired=true;clearTimeout(cap);done();}
    var cap=setTimeout(fin,600);
    function grab(c,z){
      z=Math.max(0,Math.round(z));
      var n=Math.pow(2,z),la=c.lat*Math.PI/180;
      var cx=(c.lng+180)/360*n;
      var cy=(1-Math.log(Math.tan(la)+1/Math.cos(la))/Math.PI)/2*n;
      var cols=Math.min(6,Math.ceil(size.x/256)+2),rows=Math.min(6,Math.ceil(size.y/256)+2);
      for(var dx=-Math.floor(cols/2);dx<=Math.floor(cols/2);dx++){
        for(var dy=-Math.floor(rows/2);dy<=Math.floor(rows/2);dy++){
          var y=Math.floor(cy)+dy;if(y<0||y>=n)continue;
          var x=Math.floor(cx)+dx;x=((x%n)+n)%n;
          pending++;
          var im=new Image();
          im.onload=im.onerror=function(){if(--pending<=0)fin();};
          im.src='https://'+'abc'.charAt((x+y)%3)+'.tile.openstreetmap.org/'+z+'/'+x+'/'+y+'.png';
        }
      }
    }
    var here=map.getCenter();
    grab(centre,zoom);grab(centre,zoom-1);grab(centre,zoom-2);
    // the wide part of the arc, over whatever sits between here and there
    grab(L.latLng((here.lat+centre.lat)/2,(here.lng+centre.lng)/2),Math.max(0,zoom-3));
    if(pending===0)fin();
  }

  // skin _fitToLatest: flyToBounds AC+GS with 30% padding, rings on arrival
  function fitToLatest(acKey,gsid){
    var acLL=acKey&&acM[acKey]?acM[acKey].getLatLng():null;
    var gsLL=gsid!=null&&gsM[gsid]?gsM[gsid].getLatLng():null;
    if(!acLL)return;
    removeRing(ringAC);ringAC=null;removeRing(ringGS);ringGS=null;
    map.once('moveend',function(){
      if(acKey&&acM[acKey])ringAC=placeRing(acM[acKey].getLatLng(),'ring-ac');
      if(gsid!=null&&gsM[gsid])ringGS=placeRing(gsM[gsid].getLatLng(),'ring-gs');
    });
    if(gsLL){
      var b=L.latLngBounds([acLL,gsLL]);
      var sw=b.getSouthWest(),ne=b.getNorthEast();
      var latPad=Math.max((ne.lat-sw.lat)*0.3,4),lngPad=Math.max((ne.lng-sw.lng)*0.3,4);
      var fb=L.latLngBounds([sw.lat-latPad,sw.lng-lngPad],[ne.lat+latPad,ne.lng+lngPad]);
      var fo={maxZoom:6,animate:true,duration:0.9,paddingTopLeft:L.point(60,60),paddingBottomRight:L.point(60,60)};
      // ★ Public API on purpose: getBoundsZoom/getCenter rather than the private _getBoundsCenterZoom
      //   flyToBounds uses internally. Being a tile out only means one tile arrives late, which is
      //   the very thing this is already tolerating — not worth depending on Leaflet's internals.
      preloadTiles(fb.getCenter(),Math.min(6,map.getBoundsZoom(fb,false,L.point(60,60))),function(){
        map.flyToBounds(fb,fo);
      });
    }else{
      preloadTiles(acLL,5,function(){map.flyTo(acLL,5,{animate:true,duration:0.8});});
    }
  }

  function glowGSfn(gsid){
    if(glowGS!=null&&gsM[glowGS]){
      gsM[glowGS].setIcon(icon(gsSVG(false,gsM[glowGS]._sig),30,36,15,36));
      try{gsM[glowGS].getElement().classList.remove('pulse-gs');}catch(e){}
    }
    glowGS=gsid;
    if(gsid!=null&&gsM[gsid]){
      gsM[gsid].setIcon(icon(gsSVG(true,gsM[gsid]._sig),30,36,15,36));
      try{gsM[gsid].getElement().classList.add('pulse-gs');}catch(e){}
    }
  }
  function glowACfn(key){
    if(glowAC&&acM[glowAC]){
      var d=acM[glowAC]._d||{};
      acM[glowAC].setIcon(icon(acSVG(false,d.bearing||0,d.last_seen||0),26,26,13,13));
      acM[glowAC]._sigr='';
      try{acM[glowAC].getElement().classList.remove('pulse-ac');}catch(e){}
    }
    glowAC=key;
    if(key&&acM[key]){
      var d=acM[key]._d||{};
      acM[key].setIcon(icon(acSVG(true,d.bearing||0,d.last_seen||0),26,26,13,13));
      acM[key]._sigr='';
      try{acM[key].getElement().classList.add('pulse-ac');}catch(e){}
    }
  }

  function showToast(){
    if(!lastFlight)return;
    toast.innerHTML='<span class="tf">'+(lastFlight.cs||'???')+'</span>'
      +(lastFlight.gs?' <span class="tg">via '+lastFlight.gs+'</span>':'')
      +' <span class="ta">'+age(lastFlight.t)+'</span>';
    toast.classList.add('on');
    if(ageTimer)clearInterval(ageTimer);
    ageTimer=setInterval(function(){
      if(lastFlight){var el=toast.querySelector('.ta');if(el)el.textContent=age(lastFlight.t);}
    },15000);
  }
  toast.addEventListener('click',function(){
    if(lastFlight&&lastFlight.key)fitToLatest(lastFlight.key,lastFlight.gsid);
  });

  // skin _buildACPopup table — Alt row dropped (never populates upstream),
  // Tracked (tracked_km accumulated path) in its place per design review.
  function buildACPopup(a,lat,lon,hdg){
    var fl=a.flight||a.reg||a.key;
    var dist=distKm(lat,lon);
    var spd=a.gnd_spd_kts?Math.round(a.gnd_spd_kts)+' kts':'';
    var gsN=gsNames[a.gs_id]||'';
    var freq=a.freq_khz?(a.freq_khz/1000).toFixed(3)+' MHz':'';
    var sig=a.sig_level?a.sig_level.toFixed(1)+' dBFS':'';
    var tracked=a.tracked_km?Math.round(a.tracked_km).toLocaleString()+' km'+(spd?' &bull; '+spd:''):spd;
    var row=function(label,val){return val?'<tr><td style="color:rgba(255,160,0,0.5);padding-right:8px;white-space:nowrap;">'+label+'</td><td style="color:rgba(255,210,80,0.9);">'+val+'</td></tr>':'';};
    return '<div style="font-size:11px;letter-spacing:0.8px;">'
      +'<div style="font-size:13px;font-weight:600;color:rgba(255,210,60,1);margin-bottom:6px;">'+fl+(a.icao&&a.icao!==fl?' <span style="font-size:10px;color:rgba(255,180,60,0.55);">'+a.icao+'</span>':'')+'</div>'
      +'<table style="border-collapse:collapse;">'
      +row('Freq',freq)
      +row('Via',gsN)
      +row('Signal',sig)
      +row('Distance',dist?dist+' km':'')
      +row('Tracked',tracked)
      +row('Msgs',a.msg_count||'')
      +row('Seen',age(a.last_seen*1000))
      +'</table></div>';
  }

  // skin _fetchGS — fields: gs_id, location, frequencies[{freq_khz,enabled}],
  // last_sig_level, last_heard
  function fetchGS(){
    fetch(BASE+'/addon/hfdl/groundstations').then(function(r){if(!r.ok)throw 0;return r.json();}).then(function(data){
      (Array.isArray(data)?data:[]).forEach(function(s){
        var id=s.gs_id;
        var name=s.location||('GS '+id);
        if(s.lat==null||s.lon==null)return;
        gsNames[id]=name;
        var freqs=s.frequencies||[];
        var sig=(s.last_sig_level&&s.last_sig_level!==0)?s.last_sig_level:null;
        var lastHeard=s.last_heard||0;
        var sigLine;
        if(sig){
          sigLine=sigMeterHTML(sig)+'<span style="font-size:9px;color:rgba(255,160,0,0.40);margin-left:3px;">'+sig.toFixed(1)+' dBFS</span>';
        }else if(lastHeard>0){
          var el=Math.floor(Date.now()/1000-lastHeard),elStr;
          if(el<60)elStr=el+'s ago';else if(el<3600)elStr=Math.floor(el/60)+'m ago';
          else if(el<86400)elStr=Math.floor(el/3600)+'h '+Math.floor((el%3600)/60)+'m ago';
          else elStr=Math.floor(el/86400)+'d ago';
          sigLine='<span style="font-size:9px;color:rgba(255,80,60,0.70);">&#10005; No sig &mdash; last '+elStr+'</span>';
        }else{
          sigLine='<span style="font-size:9px;color:rgba(180,80,60,0.55);">&#10005; Never heard</span>';
        }
        var ph='<div style="font-size:12px;letter-spacing:1px;color:rgba(255,200,80,0.9)">'+name+'</div>'
          +'<div style="margin-top:4px;display:flex;align-items:center;gap:6px;">'
          +'<span style="font-size:10px;color:rgba(255,160,0,0.55);">Signal:</span>'+sigLine+'</div>';
        var d=distKm(s.lat,s.lon);
        if(d)ph+='<div style="margin-top:3px;font-size:10px;color:rgba(255,160,0,0.5);">'+d+' km from receiver</div>';
        if(freqs.length){
          ph+='<div style="margin-top:6px;">';
          freqs.forEach(function(f){
            var k=f.freq_khz||0,en=f.enabled!==false;
            ph+='<div style="margin:3px 0;display:flex;align-items:center;gap:5px;">'
            +'<span style="color:'+(en?'rgba(255,180,60,0.9)':'rgba(180,120,30,0.45)')+';min-width:66px;font-size:11px;">'+(k?(k/1000).toFixed(3)+' MHz':'?')+'</span>'
            +(en?'<span style="color:rgba(80,220,80,0.7);font-size:9px;">&#9679; ON</span>':'<span style="color:rgba(255,80,80,0.35);font-size:9px;">&#9675; off</span>')
            +'</div>';
          });
          ph+='</div>';
        }else{
          ph+='<div style="color:rgba(255,160,0,0.35);font-size:10px;margin-top:4px;">No frequencies listed</div>';
        }
        if(gsM[id]){
          gsM[id].setPopupContent(ph);
          if(gsM[id]._sig!==sig){
            gsM[id]._sig=sig;
            gsM[id].setIcon(icon(gsSVG(glowGS===id,sig),30,36,15,36));
            if(glowGS===id){try{gsM[id].getElement().classList.add('pulse-gs');}catch(e){}}
          }
        }else{
          var mk=L.marker([s.lat,s.lon],{icon:icon(gsSVG(glowGS===id,sig),30,36,15,36),zIndexOffset:500});
          mk._sig=sig;
          mk.addTo(map).bindPopup(ph,{maxWidth:240});
          gsM[id]=mk;
        }
      });
      if(pendingFit){var pf=pendingFit;pendingFit=null;fitToLatest(pf.acKey,pf.gsid);}
    }).catch(function(){});
  }

  // skin _fetchAC — latest flight detection → toast + glow + snap
  function fetchAC(){
    fetch(BASE+'/addon/hfdl/aircraft').then(function(r){if(!r.ok)throw 0;return r.json();}).then(function(data){
      var ac=Array.isArray(data)?data:[];
      cnt.textContent=ac.length;
      var latest=null;
      ac.forEach(function(a){
        if(a.lat==null||a.lon==null)return;
        if(!latest||a.last_seen>latest.last_seen)latest=a;
      });
      if(latest){
        var cs=latest.flight||latest.reg||latest.key||'???';
        var tsMs=latest.last_seen*1000;
        if(!lastFlight||tsMs>lastFlight.t){
          lastFlight={cs:cs,gs:gsNames[latest.gs_id]||'',t:tsMs,key:latest.key,
                      gsid:latest.gs_id!=null?latest.gs_id:null};
          showToast();
          glowGSfn(latest.gs_id!=null?latest.gs_id:null);
          glowACfn(latest.key);
          if(snapOn)pendingFit={acKey:latest.key,gsid:latest.gs_id!=null?latest.gs_id:null};
        }
      }
      var cur={};
      ac.forEach(function(a){
        if(!a.key||a.lat==null||a.lon==null)return;
        cur[a.key]=1;
        var hdg=(a.true_trk_valid&&a.true_trk_deg!=null)?a.true_trk_deg:(a.bearing||0);
        var pop=buildACPopup(a,a.lat,a.lon,hdg);
        // icon signature: only swap the icon when its rendering changes —
        // otherwise setLatLng alone rides the CSS transition (GPU glide)
        var sig=acColour(a.last_seen,glowAC===a.key)+'|'+Math.round(hdg/3);
        if(acM[a.key]){
          acM[a.key].setLatLng([a.lat,a.lon]);
          acM[a.key]._d={bearing:hdg,last_seen:a.last_seen};
          if(acM[a.key]._sigr!==sig){
            acM[a.key]._sigr=sig;
            acM[a.key].setIcon(icon(acSVG(glowAC===a.key,hdg,a.last_seen),26,26,13,13));
            if(glowAC===a.key){try{acM[a.key].getElement().classList.add('pulse-ac');}catch(e){}}
          }
          if(acM[a.key].getPopup())acM[a.key].setPopupContent(pop);
        }else{
          var mk=L.marker([a.lat,a.lon],{icon:icon(acSVG(glowAC===a.key,hdg,a.last_seen),26,26,13,13)});
          mk._d={bearing:hdg,last_seen:a.last_seen};
          mk._sigr=sig;
          mk.addTo(map).bindPopup(pop,{maxWidth:220});
          acM[a.key]=mk;
        }
      });
      Object.keys(acM).forEach(function(k){
        if(!cur[k]){try{map.removeLayer(acM[k]);}catch(e){}delete acM[k];if(glowAC===k)glowAC=null;}
      });
      fetchGS();
    }).catch(function(){cnt.textContent='\\u2715';});
  }
  fetchAC();
  setInterval(fetchAC,5000);
}

// ════════ DIGI / CW spots (skin lsv-digmap / lsv-cwmap parity) ════════
if(KIND==='digi'||KIND==='cw'){
  var isCW=(KIND==='cw');
  var spots=[], markers={}, MAXS=500;
  var fMode=document.getElementById('fMode'), fBand=document.getElementById('fBand'), fAge=document.getElementById('fAge');

  // skin BAND_COLOUR — markers coloured by band, sized by SNR
  var BAND_COLOUR={'2200m':'#9b30d9','630m':'#c71585','160m':'#e8001e','80m':'#ff5500','60m':'#ff8c00',
    '40m':'#ffd700','30m':'#aacc00','20m':'#00cc44','17m':'#00ccaa','15m':'#00aaff','12m':'#0055ff','11m':'#6600ff','10m':'#cc00cc'};
  var BAND_RANGES={'2200m':[135700,137800],'630m':[472000,479000],'160m':[1800000,2000000],'80m':[3500000,4000000],
    '60m':[5250000,5450000],'40m':[7000000,7300000],'30m':[10100000,10150000],'20m':[14000000,14350000],
    '17m':[18068000,18168000],'15m':[21000000,21450000],'12m':[24890000,24990000],'11m':[26965000,27405000],'10m':[28000000,29700000]};
  var BAND_ORDER=['2200m','630m','160m','80m','60m','40m','30m','20m','17m','15m','12m','11m','10m'];

  function spotHz(f){if(!f)return 0;return f<1000?Math.round(f*1e6):Math.round(f);}
  function bandFromHz(hz){for(var b in BAND_RANGES){var r=BAND_RANGES[b];if(hz>=r[0]&&hz<=r[1])return b;}return null;}
  function spotBand(s){return s.band||bandFromHz(spotHz(s.freq))||null;}
  function key(s){return (s.call||'?')+'|'+(s.band||'')+'|'+(s.mode||'');}
  // skin _snrStyle: size + glow from SNR
  function snrStyle(snr){
    var v=(snr===undefined||snr===null||isNaN(parseFloat(snr)))?-99:parseFloat(snr);
    if(v>=0)  return{r:8,op:0.92,glow:true};
    if(v>=-20)return{r:6,op:0.80,glow:false};
    return      {r:4,op:0.55,glow:false};
  }
  function passes(s){
    if(!isCW&&fMode&&fMode.value!=='ALL'&&s.mode!==fMode.value)return false;
    if(fBand&&fBand.value!=='ALL'&&spotBand(s)!==fBand.value)return false;
    var fa=fAge?fAge.value:'ALL';
    if(fa!=='ALL'){
      var ms={'2m':120000,'5m':300000,'15m':900000,'30m':1800000,'1h':3600000,'3h':10800000}[fa]||Infinity;
      if(Date.now()-s.t>ms)return false;
    }
    return true;
  }
  function style(s){
    var col=BAND_COLOUR[spotBand(s)]||'#aaaaaa';
    var ss=snrStyle(s.snr);
    return{radius:ss.r,fillColor:col,color:ss.glow?col:'rgba(0,0,0,0.4)',weight:ss.glow?2:1,opacity:ss.glow?0.9:0.5,fillOpacity:ss.op};
  }
  // skin spot popup: call / mode·band·freq / SNR·country / age (+distance)
  function popup(s){
    var hz=spotHz(s.freq);
    var band=spotBand(s);
    return'<div style="font-size:12px;color:rgba(120,240,120,0.95);letter-spacing:1px;">'+s.call+'</div>'
    +'<div style="margin-top:4px;font-size:10px;color:rgba(80,200,80,0.70);">'
    +(s.mode?'<span style="margin-right:6px;">'+s.mode+'</span>':'')
    +(band?'<span style="margin-right:6px;">'+band+'</span>':'')
    +(hz?'<span>'+(hz/1e6).toFixed(4)+' MHz</span>':'')+'</div>'
    +'<div style="margin-top:3px;font-size:10px;color:rgba(80,200,80,0.55);">'
    +(s.snr!==undefined?'SNR: '+s.snr+' dB':'')+(s.wpm?' &bull; '+Math.round(s.wpm)+' wpm':'')
    +(s.country?' &bull; '+abbr(s.country):'')+'</div>'
    +'<div style="margin-top:2px;font-size:9px;color:rgba(80,200,80,0.35);">'+age(s.t)
    +(s.distKm?' &bull; '+Math.round(s.distKm)+' km':'')+'</div>';
  }
  function upsert(s){
    if(!passes(s))return;
    var k=key(s);
    if(markers[k]){markers[k].setStyle(style(s)).setLatLng([s.lat,s.lon]);markers[k].setPopupContent(popup(s));}
    else{markers[k]=L.circleMarker([s.lat,s.lon],style(s)).bindPopup(popup(s),{maxWidth:200}).addTo(map);}
  }
  function updateCount(){cnt.textContent=Object.keys(markers).length;}
  function rebuild(){
    Object.keys(markers).forEach(function(k){try{map.removeLayer(markers[k]);}catch(e){}});
    markers={};
    spots.forEach(function(s){upsert(s);});
    updateCount();
    if(statsOpen)updateStats();
  }
  [fMode,fBand,fAge].forEach(function(el){if(el)el.onchange=rebuild;});
  setInterval(rebuild,30000); // age filter refresh + prune display

  // ── stats sheet (skin lsvSmapStats) ──
  var statsOpen=false;
  var stSheet=document.getElementById('stsheet');
  function barRow(label,n,max){
    return'<div class="st-row"><span>'+label+'</span><span class="st-val">'+n+'</span></div>'
    +'<div class="st-bar"><div class="st-bf" style="width:'+Math.round(n/max*100)+'%"></div></div>';
  }
  function setH(id,v){var e=document.getElementById(id);if(e)e.innerHTML=v||'';}
  function setT(id,v){var e=document.getElementById(id);if(e)e.textContent=v||'';}
  function updateStats(){
    var vis=spots.filter(passes);
    if(!vis.length){
      setT('s-close','—');setT('s-far','—');setT('s-close-sub','');setT('s-far-sub','');
      setH('s-countries','<div style="font-size:9px;color:rgba(80,200,80,0.35)">No data</div>');
      setH('s-bands','');if(!isCW)setH('s-modes','');
      return;
    }
    function callSub(s){return(s.call||'')+(s.country?' \\u2022 '+abbr(s.country):'')+(s.mode&&!isCW?' \\u2022 '+s.mode:'');}
    var wd=vis.filter(function(s){return s.distKm>0;});
    if(wd.length){
      wd.sort(function(a,b){return a.distKm-b.distKm;});
      var cl=wd[0],fa=wd[wd.length-1];
      setT('s-close',Math.round(cl.distKm)+' km');setT('s-close-sub',callSub(cl));
      setT('s-far',Math.round(fa.distKm)+' km');setT('s-far-sub',callSub(fa));
    }else{
      setT('s-close','—');setT('s-far','—');setT('s-close-sub','');setT('s-far-sub','');
    }
    var cm={};vis.forEach(function(s){var c=abbr(s.country)||'Unknown';cm[c]=(cm[c]||0)+1;});
    var ct=Object.keys(cm).sort(function(a,b){return cm[b]-cm[a];}).slice(0,3);
    setH('s-countries',ct.length?ct.map(function(c,i){return barRow((i+1)+'. '+c,cm[c],cm[ct[0]]);}).join(''):'<div style="font-size:9px;color:rgba(80,200,80,0.35)">No country data</div>');
    var bm={};vis.forEach(function(s){var b=spotBand(s)||'?';bm[b]=(bm[b]||0)+1;});
    var bx=Math.max.apply(null,Object.keys(bm).map(function(b){return bm[b];}).concat([1]));
    setH('s-bands',BAND_ORDER.filter(function(b){return bm[b];}).map(function(b){return barRow(b,bm[b],bx);}).join('')||'&mdash;');
    if(!isCW){
      var mm={};vis.forEach(function(s){var m=(s.mode||'?').toUpperCase();mm[m]=(mm[m]||0)+1;});
      var mx=Math.max.apply(null,Object.keys(mm).map(function(m){return mm[m];}).concat([1]));
      setH('s-modes',Object.keys(mm).sort(function(a,b){return mm[b]-mm[a];}).map(function(m){return barRow(m,mm[m],mx);}).join('')||'&mdash;');
    }
  }
  document.getElementById('statsbtn').addEventListener('click',function(e){
    e.stopPropagation();
    statsOpen=!statsOpen;
    stSheet.classList.toggle('open',statsOpen);
    if(statsOpen)updateStats();
  });
  document.getElementById('stclose').addEventListener('click',function(e){
    e.stopPropagation();statsOpen=false;stSheet.classList.remove('open');
  });
  stSheet.addEventListener('touchstart',function(e){e.stopPropagation();},{passive:true});
  stSheet.addEventListener('touchmove',function(e){e.stopPropagation();},{passive:true});

  function norm(d,cw){
    var lat=d.latitude, lon=d.longitude;
    // On-device FT8 spots (Local/Kiwi) carry a Maidenhead grid instead of lat/lon.
    if((lat==null||lon==null)&&d.grid){var ll=gridLL(d.grid);if(ll){lat=ll.lat;lon=ll.lon;}}
    if(lat==null||lon==null)return null;
    return{
      lat:lat, lon:lon,
      mode:cw?'CW':String(d.mode||'').toUpperCase(),
      band:String(d.band||''),
      call:String(cw?(d.dx_call||''):(d.callsign||'')),
      snr:typeof d.snr==='number'?d.snr:undefined,
      wpm:typeof d.wpm==='number'?d.wpm:undefined,
      freq:d.frequency, country:String(d.country||''),
      distKm:(function(){var v=parseFloat(d.distance_km);return isNaN(v)?(lat!=null?distKm(lat,lon):0):v;})(),
      t:(function(ts){var x=new Date(ts).getTime();return isNaN(x)?Date.now():x;})(cw?d.time:d.timestamp)
    };
  }
  function showSpotToast(s){
    var band=spotBand(s)||'';
    if(!s.call)return;
    toast.textContent=s.call+(s.mode?' '+s.mode:'')+(band?' '+band:'');
    toast.classList.add('on');
    clearTimeout(toast._tid);
    toast._tid=setTimeout(function(){toast.classList.remove('on');},3000);
  }
  function ingest(d){
    var s=norm(d||{},isCW);
    if(!s)return;
    spots.unshift(s);
    if(spots.length>MAXS)spots.length=MAXS;
    upsert(s);
    updateCount();
    showSpotToast(s);
    if(statsOpen)updateStats();
  }
  function connect(){
    // Server validates this UUID against the registered /connection session —
    // random IDs are rejected with 400 before upgrade.
    var ws=new WebSocket(WSBASE+'/ws/dxcluster?user_session_id='+UUID);
    ws.onopen=function(){
      ws.send(JSON.stringify({type:isCW?'subscribe_cw_spots':'subscribe_digital_spots'}));
    };
    ws.onmessage=function(e){
      if(typeof e.data!=='string')return;
      try{
        var m=JSON.parse(e.data);
        if((m.type==='digital_spot'&&!isCW)||(m.type==='cw_spot'&&isCW))ingest(m.data||{});
      }catch(x){}
    };
    ws.onclose=function(){setTimeout(connect,3000);};
  }
  // Local/Kiwi (on-device decoder): the sidecar sends spots to only one
  // /ws/dxcluster client, so a second socket here would steal the decoder
  // panel's stream. Instead RN feeds us the spots it already has (each carries a
  // grid) via injected __digiPush — no extra connection, no conflict.
  if(LOCAL){
    window.__digiPush=function(json){try{var a=JSON.parse(json);for(var i=0;i<a.length;i++)ingest(a[i]);}catch(e){}};
    if(window.__pendingSpots){window.__digiPush(window.__pendingSpots);window.__pendingSpots=null;}
  }else{
    connect();
  }
}
</script>
</body></html>`;
}

// ── Component ─────────────────────────────────────────────────────────────────

/** SpotRow → the digital_spot `data` shape the WebView's norm() expects. */
function spotToData(s: SpotRow) {
  return {
    grid: s.grid, mode: s.mode, band: s.band, callsign: s.call,
    snr: s.snr, frequency: s.freqHz, distance_km: s.distKm, timestamp: s.time,
    country: s.country,
  };
}

export default function MapOverlay(
  { visible, kind, baseUrl, sessionUuid, onClose, local, wsBaseOverride, rxLat, rxLon, onPickCity, spots,
    disconnected, onBackToList, onRetry }:
    MapOverlayProps & { spots?: SpotRow[] },
) {
  const webRef = useRef<WebView>(null);
  const [ignored, setIgnored] = useState(false);

  // ★ Esc closes the map. It is OUR page, so shortcuts are not suppressed here — but nothing
  // was listening for a way out, and once inside a map a keyboard user was stuck.
  const closeRef = useRef(onClose); closeRef.current = onClose;
  useRepeatingKeys(visible, (k: string) => { if (k === 'Escape') closeRef.current(); }, []);
  useEffect(() => { if (!disconnected) setIgnored(false); }, [disconnected]);
  const injected = useRef<Set<string>>(new Set());
  const html = useMemo(
    () => (kind ? buildHtml(kind, baseUrl, sessionUuid,
      { local, wsBase: wsBaseOverride, rxLat, rxLon }) : ''),
    [kind, baseUrl, sessionUuid, local, wsBaseOverride, rxLat, rxLon],
  );

  // Inject any spots not yet pushed (newest-first), so markers/stats don't double
  // count. Local mode only — UberSDR maps own their WS.
  const pushSpots = (rows?: SpotRow[]) => {
    if (!local || !rows || rows.length === 0) return;
    const fresh = rows.filter(s => {
      const k = `${s.time}-${s.call}-${s.freqHz}`;
      if (injected.current.has(k)) return false;
      injected.current.add(k); return true;
    });
    if (!fresh.length) return;
    const json = JSON.stringify(fresh.map(spotToData));
    webRef.current?.injectJavaScript(
      `(window.__digiPush?window.__digiPush(${JSON.stringify(json)}):(window.__pendingSpots=${JSON.stringify(json)}));true;`,
    );
  };
  // Live updates push only the new ones.
  useEffect(() => { if (visible) pushSpots(spots); }, [local, visible, spots]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!visible || !kind) return null;

  return (
    <Modal
      visible
      transparent={false}
      animationType="slide"
      onRequestClose={onClose}
      supportedOrientations={['portrait', 'portrait-upside-down', 'landscape-left', 'landscape-right']}
    >
      <View style={mo.root}>
        <WebView
          ref={webRef}
          // baseUrl gives the document the INSTANCE's origin so in-page
          // fetches (/addon/hfdl/*, /api/*) are same-origin — CORS is a
          // per-instance ubersdr config flag (Server.EnableCORS) and most
          // instances have it off, which silently killed the HFDL polls
          // (digi/CW maps survived because WebSockets bypass CORS).
          source={{ html, baseUrl: baseUrl.replace(/\/+$/, '') + '/' }}
          originWhitelist={['*']}
          style={mo.web}
          javaScriptEnabled
          domStorageEnabled
          allowsInlineMediaPlayback
          setSupportMultipleWindows={false}
          onLoadEnd={() => { injected.current = new Set(); pushSpots(spots); }}
          onMessage={(e) => {
            const d = e.nativeEvent.data;
            if (d === 'close') onClose();
            else if (d === 'pickCity') onPickCity?.();
          }}
        />
        {disconnected && !ignored && (
          <View style={mo.discWrap} pointerEvents="box-none">
            <View style={mo.discCard}>
              <Text style={mo.discTitle}>Receiver disconnected</Text>
              <Text style={mo.discBody}>The server dropped the connection — spots have stopped. You can stay on the map (already-plotted spots remain) or reconnect.</Text>
              <View style={mo.discRow}>
                <TouchableOpacity style={[mo.discBtn, mo.discBtnAlt]} onPress={() => onBackToList?.()} activeOpacity={0.85}>
                  <Text style={mo.discBtnAltTxt}>Back to list</Text>
                </TouchableOpacity>
                <TouchableOpacity style={mo.discBtn} onPress={() => { setIgnored(true); onRetry?.(); }} activeOpacity={0.85}>
                  <Text style={mo.discBtnTxt}>Retry</Text>
                </TouchableOpacity>
                <TouchableOpacity style={[mo.discBtn, mo.discBtnAlt]} onPress={() => setIgnored(true)} activeOpacity={0.85}>
                  <Text style={mo.discBtnAltTxt}>Ignore</Text>
                </TouchableOpacity>
              </View>
            </View>
          </View>
        )}
      </View>
    </Modal>
  );
}

const mo = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0a0804' },
  web:  { flex: 1, backgroundColor: '#0a0804' },
  discWrap:  { position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', padding: 24 },
  discCard:  { backgroundColor: 'rgba(6,9,6,0.97)', borderWidth: 1, borderColor: 'rgba(80,200,80,0.45)',
               borderRadius: 12, padding: 18, maxWidth: 420 },
  discTitle: { color: 'rgba(120,240,120,0.95)', fontSize: 15, letterSpacing: 1, fontWeight: '600' },
  discBody:  { color: 'rgba(80,200,80,0.7)', fontSize: 12, lineHeight: 17, marginTop: 8 },
  discRow:   { flexDirection: 'row', marginTop: 16, gap: 8, justifyContent: 'flex-end', flexWrap: 'wrap' },
  discBtn:   { backgroundColor: 'rgba(80,200,80,0.9)', borderRadius: 8, paddingHorizontal: 16, paddingVertical: 9 },
  discBtnTxt:{ color: '#031003', fontSize: 13, fontWeight: '600', letterSpacing: 0.5 },
  discBtnAlt:{ backgroundColor: 'transparent', borderWidth: 1, borderColor: 'rgba(80,200,80,0.55)' },
  discBtnAltTxt: { color: 'rgba(120,240,120,0.9)', fontSize: 13, letterSpacing: 0.5 },
});
