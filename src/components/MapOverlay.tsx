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
import { DIGI_MODE_STYLE, DIGI_MARKER_RIM, digiModeStyle } from '../utils/digiModes';
import { DSC_PIN_COLOURS } from '../services/dsc';   // ★ V12 DSC's map legend
import { AppState, Linking, Modal, StyleSheet, View, Text, TouchableOpacity, useWindowDimensions } from 'react-native';
import { useOverlay } from './PanelNav';
import { NavCard, CardKey } from './NavCard';
import { useCoversScreen } from '../hooks/useScreenCovered';
import { WebView } from 'react-native-webview';
import { CABBR } from '../assets/countryAbbr';
import { type SpotRow } from '../services/DecoderClient';
import { ensureMapPack, writeMapPage } from '../services/mapPack';
import { ensureMapglPack } from '../services/mapglPack';
import { detailInstalled, readDetailRange } from '../services/mapglDetail';
import { jsStringLiteral, isAllowedHostFetchPath } from '../utils/pageSafety';
import { wsOriginFor } from '../utils/safeUrl';
// ★ The GPU map's style — embedded into the page, because the page creates its map synchronously.
import VIBEMAP_STYLE from '../../assets/mapgl/vibemap-style.json';
/* ★★★ THE SHARED BASEMAP RENDERER, AS A STRING. The app has no file server, so the only way
 *     the WebView can run the SAME map as the directory page and the server's web client is to
 *     carry the source and inject it. ★★ It CONTAINS BACKTICKS, so it may only ever be
 *     INTERPOLATED into the page template (a runtime value) — never pasted in, which would close
 *     the template literal and report the error at some unrelated comment line.
 *     Regenerate with: node scripts/gen-vibemap-source.mjs */
import { VIBEMAP_JS } from '../generated/vibemapSource';
/* ★★★ AND THE BASEMAP'S DATA, BECAUSE THE APP IS THE MAP SOURCE. The renderer asks for
 *     /mapdata/v1/<file>, which resolves against the INSTANCE (the WebView's baseUrl) — fine on a
 *     VibeServer, a 404 on an UberSDR or a KiwiSDR, which is why the HFDL map drew aircraft over a
 *     void. tier0+tier1 are embedded and served over the bridge when the instance has none.
 *     ★★ Each entry is a getter: the bytes stay in Hermes's mmapped string table until the map is
 *     actually opened, so carrying them costs install size and not memory.
 *     Regenerate with: node scripts/gen-mapdata-source.mjs */
import { MAPDATA_FILES } from '../generated/mapdataBundle';
/* ★★★ V12: THE SHARED ADS-B LAYER, AS A STRING, the same way and for the same reason as VIBEMAP_JS — it may only
 *     be INTERPOLATED into the page. Regenerate with: node scripts/gen-adsblayer-source.mjs (gen-vibemap-source runs
 *     it too). */
import { ADSB_LAYER_JS } from '../generated/adsbLayerSource';
/* ★★ V12 NDB: the shared beacon layer (web/mapkit/ndblayer.js), the same string rule — it uses the ADS-B layer's rings,
 *  so it is injected AFTER it. Regenerate with: node scripts/gen-ndblayer-source.mjs */
import { NDB_LAYER_JS } from '../generated/ndbLayerSource';
import NdbLiveList, { type NdbOverlay } from './NdbLiveList';
import { ndbMapFeed, parseNdbMapPrefs, type NdbMapPrefs } from '../services/ndb';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { parseAdsbMapPrefs, type AdsbAircraftRaw, type AdsbMapPrefs, type AdsbGainView } from '../services/adsbTypes';
import { noteFault } from '../services/faultLog';
import type { Aircraft } from '../services/SDRBackend';
import AdsbLiveList from './AdsbLiveList';
/* ★★★ V12 AIS / APRS: THE SHARED SHIP AND STATION LAYER, AS A STRING (web/mapkit/tracklayer.js — the web client's same
 *     file), interpolated only, for VIBEMAP_JS's reason. Regenerate with: node scripts/gen-tracklayer-source.mjs. What
 *     each item LOOKS like is worked out in React Native (src/services/tracks.ts) and handed in with it. */
import { TRACK_LAYER_JS } from '../generated/trackLayerSource';
import { TRACK_CARD_CSS, AIS_COLOURS, type TrackItem, type TrackRow } from '../services/tracks';
import TrackLiveList from './TrackLiveList';
import { useFaceplate } from '../contexts/FaceplateContext';
import { controlsText } from '../constants/decoderTokens';
import { FMT_SPOT_TIME_JS } from '../utils/spotTime';   // ★ the spot card's HH:MM:SS UTC (the slot's start)

/* ★ 'adsb' (V12) = VibeServer's own ADS-B 1090. HOSTED EXACTLY AS 'hfdl' IS — the same GPU path, the 'aero'
 *  profile (airports, runways), the same receiver marker, chrome and transitions — so opening it feels like
 *  opening the HFDL map (Stuart: "ASMR grade sublime"). Only the FEED differs: React Native pushes each report
 *  in (like the local spots map's pushSpots), and the shared layer draws the aircraft.
 * ★★★ AND IT IS THE MODE'S SCREEN, NOT A VIEW OF IT (Stuart, 2026-10-24: "when ADS-B is selected the waterfall drops
 *  and the full-screen map is on, an Exit ADS-B button is at the top of the screen, same as our HFDL map exit").
 *  So where HFDL's key reads "← SDR" and only puts the map away, this one reads EXIT ADS-B and its onClose LEAVES THE
 *  MODE (SDRScreen exitAdsb) — the key, Android back, and a pad's or keyboard's Back alike. The live list in the
 *  bottom-left (AdsbLiveList) is React Native's, over the page. */
/* ★★ 'ndb' (V12) = VibeServer's NDB skimmer — ADS-B's pattern exactly: the mode's screen (EXIT NDB leaves the mode), the
 *  aero look and basemap, the shared layer fed by React Native (the beacons at their published positions, each with a
 *  3-bar meter; ADS-B's NM rings), and an RN live list in the bottom-left (NdbLiveList). */
/* ★★ 'ais' (V12) = VibeServer's AIS MODE — ADS-B's shape exactly: the map IS the mode's screen, its key reads EXIT AIS and
 *  leaves the mode (SDRScreen exitAis). 'aprs' = the APRS decoder's station map (the box's MAP key); CLOSE MAP goes back to
 *  the box and the decoder carries on. Both draw with the shared track layer, fed by React Native (no socket of the
 *  page's own), with React Native's live list bottom-left (TrackLiveList). */
/* ★ 'dsc' (V12) = the DSC box's MAP: the positions sent in DSC calls, on the same track layer (pins coloured by level, the
 *  call's own line as the card); CLOSE MAP goes back to the box and the decoder carries on. */
export type MapKind = 'hfdl' | 'digi' | 'cw' | 'adsb' | 'ais' | 'aprs' | 'ndb' | 'dsc';

interface MapOverlayProps {
  visible: boolean;
  kind:    MapKind | null;
  baseUrl: string;          // http(s)://host — used for fetch + ws URLs
  /** ★★ The RADIO's own address (http(s)://host/r/<id> behind a front door) — ADS-B trails and the owner's logos live
   *  on the radio, and the door answers /adsb/… with its own page (2026-10-25). Absent = baseUrl. */
  radioUrl?: string;
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
  /** ★ kind 'adsb': the latest report's aircraft, in the SERVER's field names (the shared layer's input). */
  adsbAircraft?: AdsbAircraftRaw[] | null;
  /** ★ kind 'adsb': the receiver is dropping samples (adsbTypes adsbCaptureWarning), or null. */
  adsbCapture?: { short: string; rest: string } | null;
  /** ★★ kind 'adsb': the server's feed warning (AdsbReport.feedWarn — full gain and hearing little), or empty. */
  adsbFeedWarn?: string;
  /** ★ kind 'adsb': the same report in the app's shape, ranged from the receiver — the live list's rows. */
  adsbList?: Aircraft[];
  /** kind 'adsb': a report has arrived (the list then says "no aircraft yet", not "taking the receiver to 1090"). */
  adsbStarted?: boolean;
  /** ★★ kind 'adsb': the GAIN key (adsbTypes adsbGainView) — null/absent = no key (no gain list, owner-fixed, or unknown). */
  adsbGain?: { view: AdsbGainView | null; steps: readonly number[]; onAuto: () => void; onSet: (tenthDb: number) => void } | null;
  /** ★★ kind 'ndb' (V12): the report, the readings kept, Find / stats and their answers (NdbLiveList). */
  ndb?: NdbOverlay | null;
  /** ★ kinds 'ais' / 'aprs': the items on the map (each with its icon, label and card — tracks.ts aisItems / aprsItems),
   *  the live list's rows, its status line and empty text, and the AIS safety strip. */
  trk?: { items: TrackItem[]; rows: TrackRow[]; status: string; empty: string; safety?: string[];
          /** One item's track for its card: [lat, lon] oldest first ([] = none). */
          trail: (key: string) => Promise<[number, number][]>;
          /** Find this item when the map opens (a row tapped in the APRS box); '' = none. */
          focus?: string } | null;
  /** ★ The screen's safe-area insets, MEASURED BY THE PARENT — the hook reads 0 inside a Modal (BrowserOverlay). */
  safeInsets?: { top: number; bottom: number; left: number; right: number };
}

// ── HTML document builder ─────────────────────────────────────────────────────

function buildHtml(
  kind: MapKind, baseUrl: string, uuid: string,
  opts?: { local?: boolean; wsBase?: string | null; rxLat?: number | null; rxLon?: number | null; gl?: boolean; detail?: boolean; glBase?: string;
          /** kind 'adsb': the in-map toggles as this device last left them. */
          adsbPrefs?: AdsbMapPrefs; radioUrl?: string; ndbPrefs?: NdbMapPrefs;
          /** ★ V12 kind 'digi': the modes THIS server decodes (vibeserver.json spotModes) — the filter and the legend. */
          spotModes?: string[] },
  /** ★★ The user's colours as rgb TRIPLETS (2026-10-12): `x` text (lifted for contrast), `k` controls, `kt` controls as
   *  a legend. The page is rebuilt when they change — and they can only change from the menus, after leaving this
   *  screen (Stuart: "to change the colour of the map means youd have to exit its screen anyway"). */
  ink: { x: string; k: string; kt: string } = { x: '255,180,60', k: '255,160,0', kt: '255,200,80' },
): string {
  const local = !!opts?.local;
  /* ★ V12: the spots map's filter and legend list the modes THIS server decodes (vibeserver.json spotModes) — never a mode
   *  it cannot spot (a dead filter choice). Not said (UberSDR, on-device FT8): the style table's. */
  const esc = (v: string) => v.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
  const legendModes: string[] = opts?.spotModes && opts.spotModes.length ? opts.spotModes : Object.keys(DIGI_MODE_STYLE);
  /* ★★★ THE GPU MAP (vibemapgl.js, route B): MapLibre GL JS in this same WebView, drawing our own
   *  tiles from <documents>/mapgl/. The page's HFDL / spots code is unchanged — vibemapgl-compat.js
   *  answers its Leaflet calls with the GPU map underneath. Off (gl false) = today's Leaflet page. */
  const gl = !!opts?.gl;
  /* ★ Where the GPU map's files are, relative to this page: <documents>/mapgl/ on iOS, the APK's unpacked
   *  bundle (mapgl-bundled/) on Android — see ensureMapglPack. Only ever our own fixed folder names. */
  const glBase = /^[a-z-]+\/$/.test(opts?.glBase ?? '') ? opts!.glBase! : 'mapgl/';
  const base = baseUrl.replace(/\/+$/, '');
  const wsBase = local && opts?.wsBase ? opts.wsBase.replace(/\/+$/, '') : base.replace(/^http/, 'ws');
  const rxLat = Number.isFinite(opts?.rxLat as number) ? (opts!.rxLat as number) : 0;
  const rxLon = Number.isFinite(opts?.rxLon as number) ? (opts!.rxLon as number) : 0;
  /* ★★ CONTENT SECURITY POLICY. This page is a file:// document that may read its sibling files, and it draws
   *  text from servers. If anything ever slips past esc(), the policy keeps an injected script from loading code
   *  or sending what it reads anywhere: scripts only inline / beside the page / Leaflet's CDN; connections only
   *  to files beside it and to THIS receiver's spot socket. MapLibre needs blob: for its worker and decoded
   *  images. ★ Every source the page uses today is listed — check here first if a map layer goes missing. */
  const wsOrigin = wsOriginFor(wsBase);
  const CSP = [
    "default-src 'none'",
    "script-src 'unsafe-inline' file: blob: https://unpkg.com",
    "style-src 'unsafe-inline' file: https://unpkg.com",
    'img-src file: data: blob: https://unpkg.com',
    'font-src file: data:',
    `connect-src file: data: blob:${wsOrigin ? ' ' + wsOrigin : ''}`,
    'worker-src blob: file:',
    'child-src blob:',
    "base-uri 'none'",
    "form-action 'none'",
  ].join('; ');
  const isHfdl = kind === 'hfdl';
  const isAdsb = kind === 'adsb';
  const isNdb = kind === 'ndb';   // ★ V12 NDB — the aero look, its own layer, RN's live list (as ADS-B)
  const isTrk = kind === 'ais' || kind === 'aprs' || kind === 'dsc';   // ★ V12 AIS / APRS / DSC — the shared track layer
  /* ★★ THE AIRCRAFT MAPS SHARE ONE LOOK: ADS-B wears HFDL's amber-black page, its 'aero' basemap and its chrome.
   *  `aero` is the look; `isHfdl` stays for what is HFDL's alone (SNAP, the ground stations, the flight toast). */
  const aero = isHfdl || isAdsb || isNdb;
  const isSpots = kind === 'digi' || kind === 'cw';
  /* ★★★ THE MAP'S MENUS FOLLOW THE USER'S COLOURS (Stuart, 2026-10-12: "yes colours apply everywhere"; the map:
   *  "to change the colour of the map means youd have to exit its screen anyway"). They were HFDL amber and spots
   *  green. Now TEXT (titles, counts, labels, popups, stats) = the text colour (`T.x`); KEYS, selectors, borders,
   *  scrollbars = the controls colour (`T.k`, legends `T.kt`). ★ KEPT, because they MEAN something: the spot-age and
   *  signal-strength dots, the band colours, the aircraft's yellow selection ring and its signal meter. The
   *  near-black page tint per map (amber-black HFDL, green-black spots) is a background, kept. */
  const T = { bg: aero ? '#090602' : '#060906', x: ink.x, k: ink.k, kt: ink.kt };
  const TITLE = isHfdl ? 'HFDL AIRCRAFT MAP' : isAdsb ? 'ADS-B AIRCRAFT MAP' : isNdb ? 'NDB BEACONS' : kind === 'ais' ? 'AIS SHIP MAP'
    : kind === 'aprs' ? 'APRS STATION MAP' : kind === 'dsc' ? 'DSC POSITIONS' : kind === 'digi' ? 'DIGITAL SPOTS MAP' : 'CW SPOTS MAP';
  const CNT_ICON = isNdb
    // ★ V12 NDB: a beacon mast with its radiating arcs — the count beside it is the beacons IDENTIFIED
    ? `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" style="vertical-align:middle;opacity:0.8"><path d="M12 9 L8 22 M12 9 L16 22 M9.5 17 L14.5 17" stroke="rgba(255,190,40,0.9)" stroke-width="1.6" fill="none" stroke-linecap="round"/><circle cx="12" cy="7" r="1.8" fill="rgba(255,190,40,0.9)"/><path d="M7.5 3.5 A6 6 0 0 0 7.5 10.5 M16.5 3.5 A6 6 0 0 1 16.5 10.5" stroke="rgba(255,190,40,0.7)" stroke-width="1.4" fill="none" stroke-linecap="round"/></svg>`
    : kind === 'ais' || kind === 'dsc' ? '&#9875;' : kind === 'aprs' ? '&#128225;' : aero
    ? `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 26 26" style="vertical-align:middle;opacity:0.75"><path d="M13,1 L15,8 L23,11 L23,13 L15,11 L16,20 L19,21 L19,23 L13,21 L7,23 L7,21 L10,20 L11,11 L3,13 L3,11 L11,8 Z" fill="rgba(255,190,40,0.9)"/></svg>`
    : kind === 'cw'
    ? `<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11" viewBox="0 0 13 11" style="vertical-align:middle;margin-right:2px;opacity:0.8;color:rgba(${T.x},0.9)"><rect x="1" y="1" width="11" height="4" rx="1" fill="currentColor" opacity="0.9"/><line x1="6.5" y1="5" x2="6.5" y2="8" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><rect x="3" y="8" width="7" height="2" rx="1" fill="currentColor" opacity="0.7"/></svg>`
    : '&#128225;';

  return `<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="${CSP}">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>${VIBEMAP_JS}</script>
${isAdsb || isNdb ? `<script>${ADSB_LAYER_JS}</script>` : ''}
${isNdb ? `<script>${NDB_LAYER_JS}</script>` : ''}
${isTrk ? `<script>${TRACK_LAYER_JS}</script>` : ''}
${gl ? `<link rel="stylesheet" href="${glBase}vendor/maplibre-gl.css"><script src="${glBase}vendor/maplibre-gl.js"></script><script src="${glBase}vendor/pmtiles.js"></script><script src="${glBase}vendor/vibemapgl.js"></script><script src="${glBase}vendor/vibemapgl-compat.js"></script>` : ''}
<style>
  html,body{margin:0;padding:0;height:100%;background:${T.bg};overflow:hidden;}
  /* ★ Every size here went up 2 px on 2026-09-27 (Stuart: the legend was a strain, "all fonts could be
     a little bigger"). On a Mac the iPad app is shown at 77 %, so 10 px was landing near 7.7. */
  /* ★★★ NOTHING ON THIS PAGE IS SELECTABLE OR DRAGGABLE. On a Mac the iPad app gets mouse presses as
     TOUCHES; a press held a moment is a long-press, which selected the page's text and LIFTED it — so
     grabbing the map dragged a ghost of the count and the zoom buttons instead (Stuart, 2026-09-27,
     build 344). Leaflet cancelled every touchstart, so that path never opened; MapLibre does not. */
  *{-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;-webkit-user-drag:none;}
  select{-webkit-user-select:auto;user-select:auto;}
  body{display:flex;flex-direction:column;font-family:'Courier New',monospace;}
  /* ── topbar — skin lsv-hfdl-topbar / lsv-smap-topbar (single slim row) ── */
  #topbar{display:flex;align-items:center;gap:8px;padding:6px 10px;
    padding-left:max(10px,env(safe-area-inset-left));padding-right:max(10px,env(safe-area-inset-right));
    padding-top:max(8px,env(safe-area-inset-top));
    background:rgba(${aero ? '9,6,2' : '6,9,6'},0.97);border-bottom:1px solid rgba(${T.k},0.18);flex-shrink:0;z-index:2;}
  #back{background:rgba(${T.k},0.08);border:1px solid rgba(${T.k},0.30);border-radius:8px;
    color:rgba(${T.kt},0.85);font-family:inherit;font-size:14px;letter-spacing:1px;padding:5px 10px;
    cursor:pointer;flex-shrink:0;-webkit-tap-highlight-color:transparent;touch-action:manipulation;}
  #back:active{background:rgba(${T.k},0.18);}
  #title{font-size:15px;letter-spacing:2px;color:rgba(${T.x},0.80);flex:1;text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
  #cntwrap{display:flex;align-items:center;gap:5px;font-size:13px;letter-spacing:1px;color:rgba(${T.x},0.60);flex-shrink:0;}
  #cnt{color:rgba(${T.x},0.90);font-size:16px;}
  /* ★★ ADS-B: the receiver dropping samples (adsbCaptureWarning) — red, before the count; a phone keeps only "capture N %". */
  #capwarn{flex:0 1 auto;min-width:0;font-size:11px;letter-spacing:0.5px;color:#ff6b5e;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
  @media (max-width:520px){#capwarn .w{display:none;}}
  /* ★★ The feed warning (AdsbReport.feedWarn): red-edged, at the top of the map, wraps; × hides it until it changes. */
  #feedwarn{position:absolute;left:10px;right:10px;top:8px;z-index:900;max-width:640px;margin:0 auto;display:flex;gap:10px;align-items:flex-start;
    padding:8px 10px 8px 12px;border-radius:8px;background:rgba(8,6,2,0.96);border:1px solid #ff6b5e;font-size:12px;letter-spacing:0.3px;
    line-height:1.4;color:rgba(${T.x},0.92);}
  #feedwarn b{color:#ff6b5e;white-space:nowrap;letter-spacing:0.8px;}
  #feedwarn span{flex:1 1 auto;min-width:0;}
  #feedwarn button{flex:0 0 auto;background:none;border:0;color:rgba(${T.x},0.6);font-size:18px;line-height:1;padding:0 2px;font-family:inherit;}
  @media (max-width:1000px){#feedwarn{top:100px;}}
  @media (max-width:520px){#feedwarn{flex-wrap:wrap;}#feedwarn b{flex:1 0 auto;order:0;}#feedwarn button{order:1;}#feedwarn span{order:2;flex-basis:100%;}}
  #snap{background:rgba(${T.k},0.08);border:1px solid rgba(${T.k},0.30);border-radius:8px;
    color:rgba(${T.kt},0.60);font-size:12px;letter-spacing:0.5px;padding:5px 7px;cursor:pointer;
    font-family:inherit;-webkit-tap-highlight-color:transparent;touch-action:manipulation;line-height:1.2;text-align:center;flex-shrink:0;}
  #snap.on{border-color:rgba(${T.k},0.60);color:rgba(${T.kt},1);background:rgba(${T.k},0.14);}
  /* ── filters row (digi/cw) — skin lsv-smap-filters ── */
  #filters{display:flex;align-items:center;gap:6px;padding:4px 10px;
    padding-left:max(10px,env(safe-area-inset-left));padding-right:max(10px,env(safe-area-inset-right));
    background:rgba(6,9,6,0.97);border-bottom:1px solid rgba(${T.k},0.10);flex-shrink:0;flex-wrap:wrap;}
  select{background:rgba(${T.k},0.06);border:1px solid rgba(${T.k},0.25);border-radius:6px;
    color:rgba(${T.kt},0.85);font-family:inherit;font-size:12px;letter-spacing:0.5px;padding:4px 20px 4px 7px;cursor:pointer;
    appearance:none;-webkit-appearance:none;
    background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='6' viewBox='0 0 10 6'%3E%3Cpath fill='rgba(${T.k},0.6)' d='M0 0l5 6 5-6z'/%3E%3C/svg%3E");
    background-repeat:no-repeat;background-position:right 6px center;}
  .flabel{font-size:11px;letter-spacing:1px;color:rgba(${T.x},0.35);}
  #map{flex:1;position:relative;overflow:hidden;background:${T.bg};}
  /* ★★★ THE MAP'S BACKDROP IS THE BASEMAP'S COLOUR, NOT THE APP'S. This was ${T.bg} — near
     black — behind a LIGHT basemap, so every tile that had not arrived yet showed as a black
     hole. Stuart, 2026-09-26: "there were black tiles and it looked unsightly", and later "it
     still renders in with the black boxes anyway".
     ★★ THAT IS WHAT THE PREFETCHING WAS FOR, AND IT NEVER WORKED — a flyTo arcs through
     positions and zooms that no fixed warm-up can predict, so the gaps appeared regardless. We
     were breaching OpenStreetMap's usage policy to paper over a CSS colour, and it got us
     blocked (2026-09-26). A tile that has not loaded now looks like unrendered land instead of
     a hole, which costs one line and works whatever the tiles are doing.
     ★ #f2efe9 is the standard OSM land colour, so a gap reads as map rather than as damage. */
  #lmap{position:absolute;inset:0;background:#f2efe9;}
  /* ── GPU hints — markers glide between updates on the compositor ── */
  .leaflet-pane{will-change:transform;}
  /* ★★★ NO PER-MARKER will-change. It promotes EVERY marker to its own compositor layer, and
     on a busy receiver that is 600+ layers — far past any browser's budget. The symptom is
     unmistakable once you know it: Stuart, 2026-09-27, "the map only updates one aircraft at a
     time". That is not the code updating them in sequence; it is the browser only managing one at
     a time, each setLatLng forcing its own style recalculation and composite, so a single pass
     takes seconds and you watch them move in single file.
     ★★ The transition stays — that is the gliding Stuart wants — but the compositor decides what
     to promote, which is what it is good at. The pane above still gets the hint, once, for all of
     them together.
     ★ body.nog kills the transition outright while zooming, and now also when there are simply
     too many aircraft to animate honestly — see AC_GLIDE_MAX. */
  .leaflet-marker-icon.glide{transition:transform 2.2s linear;}
  body.nog .leaflet-marker-icon.glide{transition:none;}
  body.nogmany .leaflet-marker-icon.glide{transition:none;}
  .leaflet-fade-anim .leaflet-tile{will-change:opacity;}
  /* skin map theming */
  .leaflet-tile{filter:brightness(0.65) saturate(0.7);}
  .leaflet-popup-content-wrapper{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);border:1px solid rgba(${T.k},0.30);color:rgba(${T.x},0.90);font-family:'Courier New',monospace;font-size:13px;border-radius:8px;}
  .leaflet-popup-tip{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);}
  .leaflet-popup-close-button{color:rgba(${T.kt},0.50)!important;}
  .leaflet-popup-content{margin:10px 12px;font-family:'Courier New',monospace;}
  .leaflet-control-zoom a{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.90);color:rgba(${T.kt},0.70);border-color:rgba(${T.k},0.20);}
  .leaflet-bottom{padding-bottom:max(8px,env(safe-area-inset-bottom));}
  .leaflet-control-attribution{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.70);color:rgba(${T.x},0.25);font-size:10px;}
  /* ★ THE GPU MAP'S CONTROLS WEAR THE SAME CLOTHES as the Leaflet ones above — same colours, font and
   *  sizes — so switching renderer does not restyle the page. */
  .maplibregl-popup-content{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);border:1px solid rgba(${T.k},0.30);color:rgba(${T.x},0.90);font-family:'Courier New',monospace;font-size:13px;border-radius:8px;padding:10px 12px;}
  .maplibregl-popup-anchor-bottom .maplibregl-popup-tip,.maplibregl-popup-anchor-bottom-left .maplibregl-popup-tip,.maplibregl-popup-anchor-bottom-right .maplibregl-popup-tip{border-top-color:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);}
  .maplibregl-popup-anchor-top .maplibregl-popup-tip,.maplibregl-popup-anchor-top-left .maplibregl-popup-tip,.maplibregl-popup-anchor-top-right .maplibregl-popup-tip{border-bottom-color:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);}
  .maplibregl-popup-anchor-left .maplibregl-popup-tip{border-right-color:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);}
  .maplibregl-popup-anchor-right .maplibregl-popup-tip{border-left-color:rgba(${aero ? '9,6,2' : '6,9,6'},0.96);}
  .maplibregl-popup-close-button{color:rgba(${T.kt},0.50);font-size:16px;}
  .maplibregl-ctrl-group{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.90);border:1px solid rgba(${T.k},0.20);}
  .maplibregl-ctrl-group button+button{border-top:1px solid rgba(${T.k},0.20);}
  .maplibregl-ctrl-group button .maplibregl-ctrl-icon{filter:invert(0.7) sepia(1) saturate(3) hue-rotate(-10deg);opacity:0.7;}
  .maplibregl-ctrl-attrib,.maplibregl-ctrl-attrib.maplibregl-compact{background:rgba(${aero ? '9,6,2' : '6,9,6'},0.70);color:rgba(${T.x},0.45);font-size:10px;}
  .maplibregl-ctrl-attrib a{color:rgba(${T.x},0.55);}
  .maplibregl-ctrl-attrib-button{filter:invert(0.7) sepia(1) saturate(3) hue-rotate(-10deg);opacity:0.55;}
  /* skin lsv-hfdl pulse + rings (verbatim keyframes) */
  /* ★★★ THE PULSE WEARS THE MARKER'S OWN COLOUR. It was a fixed yellow (and a cyan ring), and the
     station only took its signal-strength colour once the NEXT flight moved the glow on — which on
     a busy HFDL server is after the map has already flown somewhere else, so the colour that
     answers "how well did I hear it?" was never seen (Stuart, 2026-09-29). --pc is set on the
     marker element to the SAME rgb the icon is drawn in (gsTone / acTone), so the pulse, the ring
     and the settled icon are one colour from the first frame. */
  /* ★★★ THE PULSE ONLY FADES — IT NEVER REPAINTS (2026-10-13, Moto G35 HFDL map). It used to animate
     filter:drop-shadow on the marker itself: a filter cannot be composited, so every frame re-rasterised
     the glow AND every aircraft sharing its layer. Two pulses (the newest flight and its station) halved
     the whole map's frame rate — 27 fps idle with ~220 aircraft, 53 with the pulses off (measured in the
     WebView). Now the full glow is drawn ONCE on a copy of the icon behind it (.pglow, added by setPulse)
     and only that copy's OPACITY animates — the compositor's job, no repaint. Same colour, same 1.2 s
     breathing: faint at the ends, full at the middle.
     ✗ No filter on the marker element itself: a parent filter would flatten the fading child back into
       a repaint every frame. */
  @keyframes pglow{0%,100%{opacity:0.15;}50%{opacity:1;}}
  .pglow{position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none;z-index:-1;
    filter:drop-shadow(0 0 10px rgba(var(--pc,255,240,60),1)) drop-shadow(0 0 18px rgba(var(--pc,255,240,60),0.7));
    animation:pglow 1.2s ease-in-out infinite;will-change:opacity;}
  @keyframes ring{0%{transform:translate(-50%,-50%) scale(1);opacity:0.8;}100%{transform:translate(-50%,-50%) scale(3);opacity:0;}}
  .ring{position:absolute;pointer-events:none;z-index:650;width:26px;height:26px;border-radius:50%;border:2px solid;transform:translate(-50%,-50%);animation:ring 1.4s ease-out 3;will-change:transform,opacity;}
  .ring-ac,.ring-gs{border-color:rgba(255,240,60,0.85);}
  /* ── toast — skin lsv-hfdl-toast / lsv-smap-toast ── */
  /* ★★★ THE PILL LIVES IN A LANE THAT STOPS SHORT OF THE ZOOM STACK. It was centred with
     left:50% + translateX(-50%) and max-width:90vw, so a long ground station ("AMX020 via
     Riverhead, New York, USA just now") ran full width UNDER the +/- buttons (Stuart, 2026-09-29,
     iPhone, VibeSDR 11 B2). Now left/right define a lane — the right edge clears the zoom control
     (Leaflet: 10 px margin + 34 px wide; MapLibre's is 29 px) plus an 8 px gap — and auto margins
     centre the pill inside it, sized to its content.
     ★★ The three parts are FLEX ITEMS so only the STATION gives way: callsign and age never
     shrink, the station ellipsises — and showToast first drops it to its first part ("Riverhead")
     when the whole name will not fit. ★ The ⓘ legend sits ABOVE this lane (bottom 44 px), so the
     left side needs only the ordinary 10 px margin. */
  #toast{position:absolute;bottom:max(10px,env(safe-area-inset-bottom));
    left:max(10px,env(safe-area-inset-left));right:calc(52px + env(safe-area-inset-right));
    width:fit-content;max-width:calc(100% - 62px - env(safe-area-inset-left) - env(safe-area-inset-right));
    margin:0 auto;box-sizing:border-box;display:flex;align-items:baseline;gap:0.6em;
    z-index:1000;background:rgba(${aero ? '9,6,2' : '6,9,6'},0.93);border:1px solid rgba(${T.k},0.30);border-radius:20px;
    padding:6px 14px;font-size:13px;letter-spacing:1px;
    color:rgba(${T.x},0.90);white-space:nowrap;opacity:0;transition:opacity 0.4s;
    overflow:hidden;cursor:pointer;pointer-events:none;}
  /* ★ An invisible pill no longer swallows taps meant for the map beneath it. */
  #toast.on{opacity:1;pointer-events:auto;}
  .tf,.ta{flex:none;}
  .tg,.tx{flex:0 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;}
  .tf{color:rgba(${T.x},1);font-size:14px;}
  .tg{color:rgba(${T.x},0.70);}
  .ta{color:rgba(${T.x},0.60);font-size:12px;}
  /* ★ iPhone SE with Display Zoom (320 pt) is the narrowest layout the app has to survive. */
  @media (max-width:400px){#toast{padding:5px 11px;letter-spacing:0.4px;gap:0.5em;}}
  /* ── legend — skin lsv-smap-legend / lsv-hfdl-legend ── */
  #legend{position:absolute;bottom:max(44px,calc(env(safe-area-inset-bottom) + 34px));left:max(10px,env(safe-area-inset-left));
    z-index:1000;font-size:12px;letter-spacing:0.8px;color:rgba(${T.x},0.80);pointer-events:auto;}
  #legbtn{display:flex;align-items:center;justify-content:center;width:28px;height:28px;
    background:rgba(${aero ? '9,6,2' : '6,9,6'},0.88);border:1px solid rgba(${T.k},0.30);border-radius:8px;cursor:pointer;
    font-size:16px;line-height:1;-webkit-tap-highlight-color:transparent;touch-action:manipulation;color:rgba(${T.kt},0.80);}
  #legbtn:active{background:rgba(${T.k},0.15);}
  #legbody{display:none;position:absolute;bottom:0;left:34px;background:rgba(${aero ? '9,6,2' : '6,9,6'},0.92);
    border:1px solid rgba(${T.k},0.18);border-radius:8px;padding:7px 9px;white-space:nowrap;
    max-height:min(320px,calc(100vh - 200px));overflow-y:scroll;overflow-x:hidden;
    -webkit-overflow-scrolling:touch;overscroll-behavior:contain;touch-action:pan-y;}
  #legend.open #legbody{display:block;}
  #legbody::-webkit-scrollbar{width:4px;}
  #legbody::-webkit-scrollbar-track{background:transparent;}
  #legbody::-webkit-scrollbar-thumb{background:rgba(${T.k},0.40);border-radius:2px;}
  .lg-row{display:flex;align-items:center;gap:8px;margin:3px 0;font-size:12px;}
  .lg-dot{display:inline-block;width:12px;height:12px;border-radius:50%;flex-shrink:0;}
  .lg-mk{width:15px;height:15px;font:bold 9px/15px monospace;text-align:center;box-shadow:0 0 0 1.2px ${DIGI_MARKER_RIM};}
  .lg-sec{font-size:11px;letter-spacing:1.5px;color:rgba(${T.x},0.40);margin:6px 0 3px;}
  .scroll-arrow{display:none;position:sticky;left:0;right:0;text-align:center;font-size:13px;line-height:1;padding:2px 0;pointer-events:none;color:rgba(${T.x},0.90);}
  .scroll-arrow-up{top:0;}
  .scroll-arrow-dn{bottom:0;}
  .scroll-arrow.arr-on{display:block;}
  /* ── stats (digi/cw) — skin lsv-st ── */
  /* ★ Above the zoom stack, not on it: at 120 px the stats button sat over the GPU map's "+" (its
     control spans roughly 74-134 px from the bottom with the attribution beneath it). 2026-09-29. */
  #statsbtnwrap{position:absolute;bottom:max(146px,calc(env(safe-area-inset-bottom) + 136px));right:max(10px,env(safe-area-inset-right));
    z-index:1000;pointer-events:auto;}
  #statsbtn{display:flex;align-items:center;justify-content:center;width:28px;height:28px;
    background:rgba(6,9,6,0.88);border:1px solid rgba(${T.k},0.30);border-radius:8px;cursor:pointer;
    font-size:15px;line-height:1;-webkit-tap-highlight-color:transparent;touch-action:manipulation;color:rgba(${T.kt},0.80);}
  #statsbtn:active{background:rgba(${T.k},0.15);}
  #stsheet{display:none;position:absolute;inset:0;z-index:1500;background:rgba(4,12,4,0.82);
    color:rgba(${T.x},0.80);font-size:12px;letter-spacing:0.8px;
    backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);
    overflow-y:scroll;overflow-x:hidden;-webkit-overflow-scrolling:touch;overscroll-behavior:contain;touch-action:pan-y;
    padding:16px max(16px,env(safe-area-inset-right)) max(16px,env(safe-area-inset-bottom)) max(16px,env(safe-area-inset-left));
    scrollbar-width:thin;scrollbar-color:rgba(${T.k},0.50) rgba(${T.k},0.10);}
  #stsheet::-webkit-scrollbar{width:5px;}
  #stsheet::-webkit-scrollbar-track{background:rgba(${T.k},0.08);border-radius:3px;}
  #stsheet::-webkit-scrollbar-thumb{background:rgba(${T.k},0.50);border-radius:3px;min-height:40px;}
  #stsheet.open{display:block;}
  .st-close{position:sticky;top:0;float:right;background:rgba(${T.k},0.12);border:1px solid rgba(${T.k},0.35);
    border-radius:8px;color:rgba(${T.kt},0.85);font-family:inherit;font-size:13px;letter-spacing:1px;padding:5px 10px;
    cursor:pointer;-webkit-tap-highlight-color:transparent;touch-action:manipulation;margin-bottom:8px;z-index:10;}
  .st-title{font-size:15px;letter-spacing:2px;color:rgba(${T.x},0.70);margin-bottom:12px;padding-top:2px;}
  .st-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;max-width:700px;margin:0 auto;}
  @media(max-width:480px){.st-grid{grid-template-columns:1fr;}}
  .st-card{background:rgba(6,9,6,0.60);border:1px solid rgba(${T.k},0.12);border-radius:8px;padding:10px 12px;}
  .st-sect{font-size:11px;letter-spacing:1.5px;color:rgba(${T.x},0.45);margin:0 0 6px;}
  .st-row{display:flex;align-items:baseline;justify-content:space-between;gap:8px;margin:4px 0;font-size:13px;}
  .st-val{color:rgba(${T.x},0.95);font-size:14px;font-weight:bold;}
  .st-sub{font-size:11px;color:rgba(${T.x},0.45);letter-spacing:0.5px;margin:-2px 0 6px;}
  .st-bar{height:3px;background:rgba(${T.x},0.12);border-radius:2px;margin:2px 0 6px;}
  .st-bf{height:3px;background:rgba(${T.x},0.65);border-radius:2px;transition:width 0.4s;}
  ${isTrk ? TRACK_CARD_CSS : ''}
  ${isAdsb || isNdb || isTrk ? `
  /* ★★ ADS-B (and V12 NDB, AIS / APRS): THE BOTTOM-LEFT CORNER IS THE LIVE LIST'S (React Native draws it over this page — AdsbLiveList), so the
     legend's key moves to the top-left, the one corner nothing else uses (the layer's toggles are top-right), and opens
     downwards from there. */
  #legend{top:10px;bottom:auto;}
  #legbody{top:0;bottom:auto;max-height:min(320px,calc(100vh - 140px));}
  /* ★★ THE ATTRIBUTION STAYS READABLE BESIDE THE LIST: its text is narrowed to the room right of the list (the list
     reports its width, __adsbListW; 0 as a pill) and wraps upwards instead of running underneath it. */
  .maplibregl-ctrl-bottom-right .maplibregl-ctrl-attrib,.leaflet-bottom.leaflet-right .leaflet-control-attribution{
    max-width:calc(100vw - var(--adsbListW,0px) - 40px - env(safe-area-inset-left) - env(safe-area-inset-right));white-space:normal;}` : ''}
</style>
</head><body>
<div id="topbar">
  ${isAdsb
    ? '<button id="back" aria-label="Exit ADS-B and give the receiver back">EXIT ADS-B</button>'
    : isNdb
    ? '<button id="back" aria-label="Exit NDB and give the receiver back">EXIT NDB</button>'
    : kind === 'ais' ? '<button id="back" aria-label="Exit AIS and give the receiver back">EXIT AIS</button>'
    : kind === 'aprs' ? '<button id="back" aria-label="Close the map — the APRS decoder carries on">CLOSE MAP</button>'
    : kind === 'dsc' ? '<button id="back" aria-label="Close the map — the DSC decoder carries on">CLOSE MAP</button>'
    : '<button id="back">&#8592; SDR</button>'}
  <div id="title">${TITLE}</div>
  ${isHfdl ? `<button id="snap" title="Toggle auto-snap to latest aircraft">&#9881;<br>SNAP</button>` : ''}
  ${isTrk ? `<button id="snap" class="trkfit" title="Fit every ${kind === 'ais' ? 'ship' : kind === 'dsc' ? 'position' : 'station'} on the map">FIT</button>` : ''}
  ${isAdsb ? '<span id="capwarn" style="display:none"><b></b><span class="w"></span></span>' : ''}
  <div id="cntwrap">${CNT_ICON}<span id="cnt">&#8230;</span></div>
</div>
${isSpots ? `
<div id="filters">
  <span class="flabel">FILTER</span>
  ${kind === 'digi' ? `<select id="fMode"><option value="ALL">All</option>${legendModes.map((m) => `<option>${esc(m)}</option>`).join('')}</select>` : ''}
  <select id="fBand"><option value="ALL">All</option>${['2200m', '630m', '160m', '80m', '60m', '40m', '30m', '20m', '17m', '15m', '12m', '10m', '6m', '4m', '2m', '70cm'].map((b) => `<option>${b}</option>`).join('')}</select>
  <select id="fAge">${kind === 'digi'
    ? `<option value="ALL">All</option><option value="2m" selected>2 min</option><option value="5m">5 min</option><option value="15m">15 min</option><option value="30m">30 min</option><option value="1h">1 hour</option>`
    : `<option value="ALL">All</option><option value="15m" selected>15 min</option><option value="30m">30 min</option><option value="1h">1 hour</option><option value="3h">3 hours</option>`}</select>
</div>` : ''}
<div id="map">
  <div id="lmap"></div>
  ${isAdsb ? '<div id="feedwarn" role="alert" style="display:none"><b>CHECK THE ANTENNA</b><span></span><button type="button" aria-label="Hide">&times;</button></div>' : ''}
  <div id="legend">
    <div id="legbtn" title="Legend">&#9432;</div>
    <div id="legbody">
      <div class="scroll-arrow scroll-arrow-up" id="arr-up">&#9650;</div>
      ${isNdb ? `
      <div class="lg-sec">NDB BEACONS</div>
      <div class="lg-row"><span>&#8962;</span><span>Your receiver</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ffb000;"></span><span>&#10003; identified, at its published place</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ffe066;"></span><span>&#8776; read one letter off the published ident</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:rgba(255,184,51,0.55);"></span><span>? not read yet &mdash; the likeliest beacon</span></div>
      <div class="lg-row"><span class="lg-dot" style="width:5px;height:5px;background:rgba(170,170,170,0.6);"></span><span>published, not heard</span></div>
      <div class="lg-sec">SIGNAL</div>
      <div class="lg-row"><span style="color:#e04040">&#9646;</span><span>&lt; 10 dB</span></div>
      <div class="lg-row"><span style="color:#e0b020">&#9646;&#9646;</span><span>&lt; 20 dB</span></div>
      <div class="lg-row"><span style="color:#33cc44">&#9646;&#9646;&#9646;</span><span>20 dB and over &middot; dim = not heard this minute</span></div>
      <div class="lg-row"><span>&#9711;</span><span>Range rings in nautical miles</span></div>
      ` : kind === 'ais' ? `
      <div class="lg-sec">AIS SHIPS</div>
      ${Object.entries(AIS_COLOURS).map(([f, c]) => `<div class="lg-row"><span class="lg-dot" style="background:${c};"></span><span>${f === 'hsc' ? 'high-speed craft' : f}</span></div>`).join('')}
      <div class="lg-row"><span class="lg-dot" style="background:#ff3b3b;"></span><span>distress beacon</span></div>
      <div class="lg-row"><span>&#9670;</span><span>Aid to navigation (dashed = virtual)</span></div>
      <div class="lg-row"><span>&#9679;</span><span>A dot = still; a hull points along its heading</span></div>
      ` : kind === 'aprs' ? `
      <div class="lg-sec">APRS STATIONS</div>
      <div class="lg-row"><span class="lg-dot" style="background:#59c26b;"></span><span>station</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#4fa3ff;"></span><span>moving</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#39d0d8;"></span><span>weather</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#ffad42;"></span><span>object / item</span></div>
      <div class="lg-row"><span class="lg-dot" style="background:#c58cff;"></span><span>digipeater / gateway</span></div>
      <div class="lg-row"><span>&#9711;</span><span>Dashed ring = alternate symbol table</span></div>
      ` : kind === 'dsc' ? `
      <div class="lg-sec">DSC POSITIONS</div>
      ${DSC_PIN_COLOURS.map((c, i) => `<div class="lg-row"><span class="lg-dot" style="background:${c};"></span><span>${['routine', 'safety', 'urgency', 'distress'][i]}</span></div>`).reverse().join('')}
      <div class="lg-row"><span>&#8962;</span><span>Your receiver</span></div>
      ` : isAdsb ? `
      <div class="lg-sec">ADS-B 1090</div>
      <div class="lg-row"><span>&#8962;</span><span>Your receiver</span></div>
      <div class="lg-row"><span>&#9992;</span><span>Icon = aircraft type, turned to its track</span></div>
      <div class="lg-row"><span>&#9711;</span><span>Range rings in nautical miles</span></div>
      <div class="lg-sec">ON THE MAP</div>
      <div class="lg-row"><span>Auto-zoom &middot; Fit &middot; Rings</span></div>
      ` : isHfdl ? `
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
      ` : kind === 'digi' ? `
      <div class="lg-sec">MODE</div>
      ${legendModes.map((m) => { const st = digiModeStyle(m); return `<div class="lg-row"><span class="lg-dot lg-mk" style="background:${st.colour};color:${st.ink};">${esc(st.letter)}</span><span>${esc(m)}</span></div>`; }).join('')}
      <div class="lg-row"><span>Colour + letter = mode</span></div>
      <div class="lg-sec">SNR / SIZE</div>
      <div class="lg-row"><span class="lg-dot" style="width:14px;height:14px;background:rgba(255,255,255,0.5);box-shadow:0 0 6px 3px rgba(255,255,255,0.4);"></span><span>Strong (&ge;0 dB)</span></div>
      <div class="lg-row"><span class="lg-dot" style="width:9px;height:9px;background:rgba(255,255,255,0.5);"></span><span>Medium (-20 to 0)</span></div>
      <div class="lg-row"><span class="lg-dot" style="width:6px;height:6px;background:rgba(255,255,255,0.3);"></span><span>Weak (&lt; -20 dB)</span></div>
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
  ${isSpots ? `
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
var BASE=${jsStringLiteral(base)}, WSBASE=${jsStringLiteral(wsBase)}, KIND=${jsStringLiteral(kind)}, UUID=${jsStringLiteral(uuid)};
/* ★★★ EVERY SERVER STRING THAT GOES INTO HTML GOES THROUGH esc(). Popups and the stats sheet are built as HTML;
 *  a callsign, flight or station name is text from someone else's server and must never become markup. */
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function numOr(v,d){var n=typeof v==='number'?v:parseFloat(v);return isFinite(n)?n:d;}
var LOCAL=${local ? 1 : 0};
var GL_OK=${gl ? 1 : 0};
var DETAIL_OK=${gl && opts?.detail ? 1 : 0};
var VM_STYLE=${gl ? JSON.stringify(VIBEMAP_STYLE) : 'null'};
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
/* ★★★ THE SWITCH. With WebGL 2 and the map files on disk, the GPU map is created here and
 *  window.L becomes the adapter, so every L.marker / flyToBounds below lands on the GPU map. Anything
 *  else — no WebGL 2, files missing, an exception — leaves Leaflet exactly as it was. */
var GLVM=null;
function glLoad(path,kind){return new Promise(function(res){try{var x=new XMLHttpRequest();x.open('GET',path);
  x.responseType=kind==='text'?'text':'arraybuffer';
  x.onload=function(){res((x.status===0||x.status===200)&&x.response?x.response:null);};
  x.onerror=function(){res(null);};x.send();}catch(e){res(null);}});}
if(GL_OK&&window.VibeMapGL&&window.VibeMapGLCompat&&VM_STYLE){
  try{
    /* ★ The high-detail pack, when installed, is read by byte range over the bridge (mapglDetail.ts):
     *  the page asks, React Native answers with exactly those bytes, base64. */
    window.__mgWait={};window.__mgSeq=0;
    window.__mgDeliver=function(id,b64){var w=window.__mgWait[id];if(!w)return;delete window.__mgWait[id];
      if(!b64){w(null);return;}try{var bin=atob(b64),u=new Uint8Array(bin.length);for(var i=0;i<bin.length;i++)u[i]=bin.charCodeAt(i);w(u.buffer);}catch(e){w(null);}};
    var detailRange=DETAIL_OK&&window.ReactNativeWebView?function(off,len){return new Promise(function(res){
      var id='mg'+(++window.__mgSeq);window.__mgWait[id]=res;
      window.ReactNativeWebView.postMessage(JSON.stringify({t:'mgrange',id:id,off:off,len:len}));
      setTimeout(function(){if(window.__mgWait[id]){delete window.__mgWait[id];res(null);}},10000);});}:null;
    GLVM=VibeMapGL.createNow(document.getElementById('lmap'),{style:VM_STYLE,load:glLoad,base:'${glBase}',detailRange:detailRange,
      profile:((KIND==='hfdl'||KIND==='adsb'||KIND==='ndb')?'aero':'spots'),center:[0,30],zoom:1});
    if(GLVM){window.L=VibeMapGLCompat.install(GLVM);window.__GL=true;
      // ★ Belt to the CSS braces above: no native selection or drag ever starts on this page.
      ['selectstart','dragstart'].forEach(function(t){document.addEventListener(t,function(e){e.preventDefault();},{capture:true});});
      GLVM.ready.catch(function(e){console.error('GPU map: '+e);});
      // ★ The greyline on EVERY map (Stuart): HFDL 2026-09-27 — how many aircraft fly in the dark — and the
      //   digital / CW spots 2026-09-28, where it is propagation information, as in the web client.
      GLVM.setNight(true);}
  }catch(e){console.error('GPU map unavailable: '+e);GLVM=null;}
}
var map=L.map('lmap',{zoomControl:false,attributionControl:true,fadeAnimation:true,zoomAnimation:true,markerZoomAnimation:true,preferCanvas:true})
  .setView([30,0],2);
L.control.zoom({position:'bottomright'}).addTo(map);
/* ★★★ A TILE CACHE, BECAUSE THE FLYOVER IS WHAT MAKES THIS MAP DIFFERENT. Leaflet's keepBuffer
 *  holds a skirt around the CURRENT view and nothing else: zoom in and the world tiles are
 *  destroyed, zoom back out and they are fetched again over the network, which is the black you
 *  see. Every other map in this product is panned by a human and never notices. This one snaps
 *  to each new aircraft, so it crosses the same ground over and over.
 *  ★★★ Stuart, 2026-09-26: "it went from zoomed in to a zoomed out full world view and it went
 *  black and it shouldnt have as we already had the full world view previously" — and that is
 *  exactly right, we HAD fetched them.
 *  ★★ THIS IS WHAT WAS ASKED FOR ALL ALONG, AND THE CODE DID THE OPPOSITE. What was built was a
 *  PREFETCH: 21 tiles pulled from OpenStreetMap on every map open, guessing at what a flight
 *  might cross. It could not work — a flyTo arcs through zooms and positions no fixed warm-up
 *  predicts — and prefetching a free service is the one thing their usage policy forbids.
 *  CACHING WHAT WE ACTUALLY FETCHED IS THE OPPOSITE: it is explicitly fine, and it makes us a
 *  LIGHTER user of OSM, not a heavier one.
 *  ★ Held as object URLs rather than trusting the HTTP cache: this document is loaded from an
 *    HTML STRING (see the WebView's source={{html}}), and a WebView's cache for such a document
 *    is not something to build a feature on. */
/* ★★★ THE TILE CACHE IS GONE, AND SO IS THE SYNTAX ERROR IT WAS HIDING. ~150 lines of LRU cache
 *  and a CachedTileLayer were built on 2026-09-26 to stop OSM tiles going black during the HFDL
 *  flyover, then made irrelevant the same afternoon when the basemap became bundled VECTORS —
 *  nothing to fetch means nothing to cache. It was never instantiated after that, and a
 *  DUPLICATED "return img; } });" sat at the end of it: a bare "return" at top level, which is a
 *  SyntaxError that kills the ENTIRE inline page script — basemap, markers and every handler.
 *  ★★ It never shipped (no APK was built that day), but it had been COMMITTED, and it survived
 *  because nothing parses this page at build time: the literal is just a string to TypeScript.
 *  ★ Dead code is not free. This is the second time today a fault hid inside something nobody
 *  was reading any more. The page's JS is now parse-checked — see the note on VIBEMAP_JS. */
/* ===== THE BASEMAP IS THE SHARED RENDERER =============================================
 * ★★★ ONE RENDERER, THREE READERS. This file used to carry its own crude copy: three tiers of
 *  country rings, a place dot and an airport glyph, and nothing else. Every improvement — relief
 *  tiles, landcover, the label ladder and its collision placer, world wrapping, the profiles —
 *  was paid for on the directory page and none of it reached the app. web/mapkit/vibemap.js is
 *  now the only copy; this page LOADS it. ✗ Do not re-implement a basemap here.
 * ★★ AND IT FIXES TWO FAULTS THE OLD COPY HAD: its tier-2 list asked for tier2-lakes.json and
 *  tier2-admin1.json, which are SHARDED on disk (tier2-lakes.s0.json …) and so 404'd every time;
 *  and MAP_DATA_BASE was referenced before it was assigned for half a day, throwing a
 *  ReferenceError into a catch, so the map drew nothing and said nothing. The shared renderer
 *  reads the "sharded" map in index.json, and it logs every draw failure.
 *  ★ No backtick may appear anywhere in this page, comments included: it would close the template
 *  literal, and the build error points at an unrelated line. That just cost another go.
 * ★★ PROFILES ARE SUBTRACTIONS. A profile decides which FEATURES earn screen space, never the
 *  palette: HFDL is aircraft, so airfields and runways matter and sea ports are noise; the spot
 *  maps ask WHERE someone is, so towns and the locator grid matter and airfields are noise.
 * ✗ STALE COMMENT, CORRECTED 2026-09-27: dataBase is RELATIVE ('mapdata/v1/'), not root-relative.
 *   This line went on claiming the opposite after the value changed, and the fetch shim's MARK —
 *   the one place that has to agree with it — was left matching the old spelling. That cost three
 *   builds of blue sea. Whatever this says, the shim now matches WITHOUT a leading slash so both
 *   spellings work; keep it that way rather than re-tightening it.
 * ★ (Historical) dataBase was ROOT-RELATIVE — the document is loaded from the instance origin, so it
 *  reaches the VibeServer the user is actually listening to, including a local dongle (a loopback
 *  VibeServer) and a Pi in hotspot mode with no uplink.
 * ★★★ AND WHEN THE INSTANCE IS NOT A VIBESERVER, THE APP ANSWERS INSTEAD. An UberSDR serves
 *  /addon/hfdl/aircraft and does NOT serve /mapdata/v1/ (200 vs 404), so the HFDL map used to draw
 *  aircraft over a void. Our own vector maps exist precisely so the app stops depending on anyone
 *  else for a basemap — openstreetmap.org blocked us for hammering their tiles — so the APP is the
 *  map source for every engine it drives: UberSDR, KiwiSDR, a local dongle. OWRX is the one that
 *  uses its own maps.
 * ★★ THE SHIM IS AT fetch(), NOT IN THE RENDERER. vibemap.js reaches for its data in four different
 *  places (the index probe, fetchJson, the shards, tier0-countrylabels); wrapping fetch catches all
 *  of them and needs no renderer change, which keeps ONE renderer. ✗ Do not add an app-only data
 *  path inside vibemap.js.
 * ★★ NETWORK FIRST, AND THAT IS DELIBERATE: a VibeServer may hold the FULL pack (tier2 shards, the
 *  relief rasters) and the embedded tier0/tier1 is the FLOOR, not a replacement. One failed
 *  index.json latches localOnly, so a non-VibeServer costs exactly ONE 404 and never one per layer.
 * ★ The bytes come over the bridge from RN (see onMessage/__mdDeliver): the data cannot live in this
 *  page, because the page is a string and 14 MB of it would be built on every open. */
/* ★★★ THE INSTANCE IS FETCHED BY REACT NATIVE, NOT BY THIS PAGE — AND THAT IS THE WHOLE POINT.
 *  The page used to be given the INSTANCE as its origin so /addon/hfdl/* was same-origin (most
 *  UberSDRs have CORS off). That forced the MAP DATA over the RN bridge instead, which is exactly
 *  backwards: ~15 MB of static map on the slow path, a few KB of aircraft every 5 s on the fast
 *  one. Stuart: "that is what I want — maps stored on device so animations are smooth."
 *  ★★ So the page now loads from a LOCAL origin with the map pack beside it on disk, and the only
 *     thing crossing the bridge is the small, frequent aircraft feed. RN's fetch is not bound by
 *     browser CORS, so an instance with CORS off still works — the very reason the instance origin
 *     was chosen in the first place, now served better by not using it.
 *  ★ Returns parsed JSON or null; every caller already treats a failure as "no data this tick". */
window.__hfWait = {};
window.__hfSeq = 0;
window.__hfDeliver = function(id, body){
  var w = window.__hfWait[id];
  if(w){ delete window.__hfWait[id]; w(body); }
};
function hostFetch(path){
  return new Promise(function(res){
    if(!window.ReactNativeWebView){ res(null); return; }
    var id = 'hf' + (++window.__hfSeq);
    window.__hfWait[id] = res;
    window.ReactNativeWebView.postMessage(JSON.stringify({ t:'hostfetch', id:id, path:path }));
    /* ★ Never wait for ever: a lost reply must read as "nothing this tick", which every caller
     *  already handles, rather than stalling the poll loop that feeds the animation. */
    setTimeout(function(){ if(window.__hfWait[id]){ delete window.__hfWait[id]; res(null); } }, 12000);
  });
}
window.__mdWait = {};
window.__mdSeq = 0;
window.__mdDeliver = function(id, body){
  var w = window.__mdWait[id];
  if(w){ delete window.__mdWait[id]; w(body); }
};
/* ★★★ BIG FILES ARRIVE IN PIECES, BECAUSE ONE INJECTION CANNOT CARRY THEM.
 *  Three builds showed the same pattern and I kept reading it as a permissions problem: the small
 *  label files arrived and every megabyte layer — tier0-cover, tier1-cover, the coastlines — did
 *  not, so the map drew sea, continent labels and nothing else. Stuart saw it three times: "no
 *  map, just sea", "I see labels for continents".
 *  ★★ SIZE WAS THE ONLY VARIABLE THAT EVER CORRELATED. injectJavaScript hands WKWebView a script
 *     SOURCE STRING, and a 2.2 MB one does not survive the trip. Splitting it needs no permission
 *     from anybody, which is the point: it removes the guess rather than making a better one.
 *  ★ Reassembled in order and only resolved when every part is in, so a short delivery fails as
 *    "no data" — the renderer's own coarse-map path — rather than as half a continent. */
window.__mdParts = {};
window.__mdChunk = function(id, i, n, part){
  var b = window.__mdParts[id] || (window.__mdParts[id] = { n: n, got: 0, buf: [] });
  b.buf[i] = part; b.got++;
  if (b.got >= b.n) {
    delete window.__mdParts[id];
    var w = window.__mdWait[id];
    if (!w) return;
    delete window.__mdWait[id];
    /* ✗✗✗ THE JOINED TEXT IS STILL JSON-ENCODED, AND I SHIPPED IT RAW IN 340.
     *     The direct path interpolates the payload as a JavaScript STRING LITERAL, so the page
     *     is handed the DECODED text. The chunked path slices that same encoded form, outer
     *     quotes and all — so joining the pieces reproduces the ENCODED text, not the string.
     *     Every chunked layer arrived still wearing its backslash-escaped quotes, and parsed as
     *     nothing.
     *  ★ One decode here puts the two paths back on the same contract. Splitting the ESCAPED
     *    text is still right (a boundary cannot then cut a multi-byte character in half) — it
     *    just has to be UNescaped once it is whole again. */
    var out = null;
    try { out = JSON.parse(b.buf.join('')); } catch (e) { out = null; }
    w(out);
  }
};
(function(){
  var localOnly = false;
  var real = window.fetch.bind(window);
  /* ✗✗✗ THE MARK HAD A LEADING SLASH AND dataBase DID NOT, SO THE SHIM NEVER FIRED.
   *     dataBase is 'mapdata/v1/' — I made it relative earlier today to fix a different symptom
   *     — and this tested indexOf('/mapdata/v1/'). A request for 'mapdata/v1/tier0-cover.json'
   *     does not contain that string, so indexOf returned -1, the shim handed it to the REAL
   *     fetch, it resolved against file://.../vibemap.html, found nothing, and failed. No
   *     exception, no console output, no land: Stuart attached Safari's inspector to the page and
   *     the console was COMPLETELY EMPTY, which is what finally pinned it.
   *  ★★ AN EMPTY CONSOLE WAS THE FINDING, not a dead end. The page was not crashing, it was
   *     running perfectly and being handed nothing — and every layer fails to a quiet null by
   *     design, so silence is exactly what a delivery miss looks like from inside the page.
   *  ★★ ONE RULE, TWO READERS, and I wrote both halves. The comment on dataBase three hundred
   *     lines down STILL said "root-relative on purpose" after I made it relative. When a value
   *     and the thing that matches it live apart, changing one is changing half.
   *  ★ Matching without the leading slash accepts both spellings, so neither a relative nor a
   *    root-relative dataBase can break it again. */
  var MARK = 'mapdata/v1/';
  function ask(file){
    return new Promise(function(res){
      if(!window.ReactNativeWebView){ res(null); return; }
      var id = 'md' + (++window.__mdSeq);
      window.__mdWait[id] = res;
      window.ReactNativeWebView.postMessage(JSON.stringify({ t:'mapdata', id:id, file:file }));
      // ★ Never leave the renderer awaiting forever: a lost message must read as "no data",
      //   which it already handles (it draws the coarser map), not as a hung basemap.
      setTimeout(function(){ if(window.__mdWait[id]){ delete window.__mdWait[id]; res(null); } }, 10000);
    });
  }
  function reply(body){
    if(body == null) return new Response('', { status: 404 });
    return new Response(body, { status: 200, headers: { 'Content-Type': 'application/json' } });
  }
  window.fetch = function(u, opt){
    var s = (typeof u === 'string') ? u : ((u && u.url) || '');
    var i = s.indexOf(MARK);
    if(i < 0) return real(u, opt);
    var file = s.slice(i + MARK.length);
    if(localOnly) return ask(file).then(reply);
    var miss = function(){
      if(file.indexOf('index.json') === 0) localOnly = true;
      return ask(file).then(reply);
    };
    /* ★★★ READ IT OFF THE DISK — AND A file:// READ SUCCEEDS WITH STATUS 0, NOT 200.
     *  This raced fetch() against a 2.5 s timer and accepted the answer only if 'r.ok'. A local
     *  file is never 'ok': it has no HTTP status, so it reports 0. MEASURED in WKWebView with this
     *  WebView's exact file-access settings (2026-09-27, the same WebKit as iOS): fetch() of the
     *  2.2 MB tier1-cover returned ALL 2,263,506 bytes in 54 ms — with ok:false. So every
     *  successful disk read was thrown away and the layer went over the React Native bridge
     *  instead, a JSON-encoded, chunked, injectJavaScript round trip. Worse, the first file to be
     *  "refused" was index.json, which latched localOnly: from then on EVERY file skipped the
     *  disk entirely. That is the sluggish, streaming map Stuart described ("it appears that we
     *  cannot read from super fast SSD storage that quickly") — the SSD was never asked.
     *  ✗ The old note here said a file:// fetch "can simply never settle". Measured, it settles;
     *    it was being rejected, not hung.
     *  ★★ XMLHttpRequest, not fetch: same file, 5 ms against 54 ms. A body with status 0 or 200
     *     is success; anything else, or no answer inside the guard, falls to the bridge. */
    return new Promise(function(resolve){
      var done = false;
      var finish = function(body){
        if(done) return; done = true; clearTimeout(t);
        resolve(body != null ? reply(body) : miss());
      };
      var t = setTimeout(function(){ finish(null); }, 2500);
      try {
        var x = new XMLHttpRequest();
        x.open('GET', s);
        x.onload = function(){
          var ok = (x.status === 0 || x.status === 200) && x.responseText && x.responseText.length > 0;
          finish(ok ? x.responseText : null);
        };
        x.onerror = function(){ finish(null); };
        x.send();
      } catch(e){ finish(null); }
    });
  };
})();
var VM = window.__GL ? null : VibeMap.attach(map, {
  /* ★★★ RELATIVE, NOT ROOT-RELATIVE, AND THE SLASH IS THE WHOLE DIFFERENCE. The page now lives on
   *  disk at <documents>/vibemap.html with the pack unpacked to <documents>/mapdata/v1/, so
   *  'mapdata/v1/x.json' resolves to the file beside it. A LEADING slash on a file:// document
   *  resolves to the FILESYSTEM ROOT — /mapdata/v1/... — which exists nowhere and fails silently,
   *  one layer at a time, exactly the quiet blank this rework exists to end. */
  dataBase: 'mapdata/v1/',
  profile: (KIND === 'hfdl' || KIND === 'adsb' || KIND === 'ndb') ? 'aero' : 'spots'
});
/* ★★★ THE STARTUP TILE WARM-UP IS GONE. It fetched 21 tiles from OpenStreetMap on every
 *  map open to hide the black gaps during a flyTo — and it did not hide them, because the arc
 *  passes through zooms and positions no fixed warm-up can anticipate. What it did do was make
 *  us look like an app bulk-fetching a free service, which their policy forbids and which got
 *  our tiles refused. The gaps are now invisible because #lmap is the basemap's own colour.
 *  ✗ Do not reintroduce prefetching against somebody else's tile server. */
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
/* ★★★ PAST THIS MANY AIRCRAFT, STOP PRETENDING TO ANIMATE. A 2.2 s transition on 600 markers
 *  is not a smooth glide, it is a queue — and the queue is what makes the whole map feel broken.
 *  Below the threshold the animation is the thing Stuart wants ("that animation when it works is
 *  awesome"); above it, honest jumps beat a stutter.
 *  ★ Re-evaluated on every update, so a receiver that quietens down starts gliding again. */
/* ★ MOVES per poll, not markers on screen. 220 aircraft all moving at once (ADS-B) is a
 *  queue; 220 of 600 sitting still (HFDL) costs nothing to glide. */
var AC_GLIDE_MAX = 220;
window.__acGlideCheck = function(n){
  document.body.classList.toggle('nogmany', n > AC_GLIDE_MAX);
};
map.on('zoomstart',function(){document.body.classList.add('nog');});
map.on('zoomend',function(){setTimeout(function(){document.body.classList.remove('nog');},60);});
// Orientation change → relayout
window.addEventListener('resize',function(){setTimeout(function(){try{map.invalidateSize();}catch(e){}},120);});

${FMT_SPOT_TIME_JS}
function age(ms){var s=Math.floor((Date.now()-ms)/1000);if(s<5)return'just now';if(s<60)return s+'s ago';var m=Math.floor(s/60);return m<60?m+'m ago':Math.floor(m/60)+'h ago';}
function distKm(lat,lon){if(!RX_LAT&&!RX_LON)return null;var R=6371,r=Math.PI/180;var dLat=(lat-RX_LAT)*r,dLon=(lon-RX_LON)*r;var a=Math.sin(dLat/2)*Math.sin(dLat/2)+Math.cos(RX_LAT*r)*Math.cos(lat*r)*Math.sin(dLon/2)*Math.sin(dLon/2);return Math.round(R*2*Math.atan2(Math.sqrt(a),Math.sqrt(1-a)));}

var RX_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 22 22"><polygon points="11,1 21,10 17,10 17,21 13,21 13,15 9,15 9,21 5,21 5,10 1,10" fill="rgba(255,200,60,0.85)" stroke="rgba(0,0,0,0.5)" stroke-width="1"/></svg>';
function addRx(){if(RX_LAT||RX_LON){L.marker([RX_LAT,RX_LON],{icon:L.divIcon({html:RX_SVG,className:'',iconSize:[22,22],iconAnchor:[11,22]}),zIndexOffset:1000}).addTo(map).bindPopup('<div style="font-size:13px;letter-spacing:1px;">&#127968; Your receiver</div>');map.setView([RX_LAT,RX_LON],KIND==='hfdl'?4:KIND==='adsb'?7:3);}}

// Local/Kiwi (on-device decoder): RX position is injected (no /api/description).
// 0,0 = Null Island = unknown → offer a "set location" button that asks RN for a
// city so distances are at least roughly meaningful.
function showSetLoc(){
  var b=document.createElement('button');
  b.textContent='\\uD83D\\uDCCD SET LOCATION';
  b.style.cssText='position:absolute;left:50%;bottom:18px;transform:translateX(-50%);z-index:900;background:rgba(6,9,6,0.92);color:rgba('+'${T.x}'+',0.95);border:1px solid rgba('+'${T.k}'+',0.5);border-radius:8px;padding:9px 16px;font-family:inherit;font-size:14px;letter-spacing:1px;';
  b.addEventListener('click',function(){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage('pickCity');});
  document.getElementById('map').appendChild(b);
}
if(LOCAL){
  if(RX_LAT||RX_LON)addRx(); else showSetLoc();
}else if(KIND==='adsb'){
  /* ★ ADS-B: the receiver is the SERVER's own /location, which React Native already holds and injects (a VibeServer
   *  has no UberSDR /api/description). Unknown = no marker; the aircraft still draw, without range. */
  if(RX_LAT||RX_LON)addRx();
}else if(KIND==='ndb'){
  /* ★ V12 NDB: the beacon layer draws the receiver's house itself (ndblayer.js) — only the view is set here. */
  if(RX_LAT||RX_LON)map.setView([RX_LAT,RX_LON],7);
}else if(KIND==='ais'||KIND==='aprs'||KIND==='dsc'){
  /* ★ V12 AIS / APRS: the server's /location, injected; the track layer draws the receiver's house itself (setReceiver). */
  if(RX_LAT||RX_LON)map.setView([RX_LAT,RX_LON],8);
}else{
  hostFetch('/api/description').then(function(d){ if(!d) throw 0; return d; }).then(function(d){
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

  /* ★★ ONE COLOUR PER MARKER, glowing or not — the pulse no longer swaps in yellow (see .pglow).
   *  Tones return the rgb triple separately so the pulse and ring can use it at their own alpha. */
  function acTone(lastSeen){var a=Math.floor(Date.now()/1000)-lastSeen;
    if(a<120)return{rgb:'80,255,120',a:0.92};if(a<300)return{rgb:'180,255,80',a:0.88};
    if(a<900)return{rgb:'255,190,40',a:0.85};return{rgb:'255,80,60',a:0.75};}
  function acColour(lastSeen){var t=acTone(lastSeen);return'rgba('+t.rgb+','+t.a+')';}
  function gsTone(sig){
    if(sig==null||sig===0)return{rgb:'120,120,140',a:0.60,x:true};
    if(sig>-40)return{rgb:'60,220,80',a:0.92};
    if(sig>-55)return{rgb:'220,200,40',a:0.92};
    return{rgb:'255,100,60',a:0.85};
  }
  /* ★ Start (rgb) or stop (null) a marker's pulse in its own colour. The custom property sits on the
   *  same element as the class, because that is the element the animation runs on. */
  function setPulse(mk,cls,rgb){
    try{
      if(mk.setPulse){mk.setPulse(rgb);return;}   // ★ a GPU marker draws its own (vibemapgl-compat.js)
      var el=mk.getElement();if(!el)return;
      var old=el.querySelector('.pglow');if(old)old.remove();
      if(rgb){
        el.style.setProperty('--pc',rgb);el.classList.add(cls);
        // ★ The glow layer: a copy of the icon, glowing, behind it — see .pglow (only its opacity animates).
        var svg=el.querySelector('svg');
        if(svg){var g=document.createElement('div');g.className='pglow';g.appendChild(svg.cloneNode(true));el.insertBefore(g,el.firstChild);}
      }
      else{el.classList.remove(cls);el.style.removeProperty('--pc');}
    }catch(e){console.error('HFDL: could not set the marker pulse',e);}
  }

  // skin _acSVG: fuselage ellipse + swept wings + tail, rotated, glow shadow
  function acSVG(glow,bearing,lastSeen){
    var t=acTone(lastSeen||0), c='rgba('+t.rgb+','+t.a+')';
    var f=glow?'filter:drop-shadow(0 0 5px rgba('+t.rgb+',0.85));':'';
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
    /* ★★ The signal-strength colour WHILE it glows too — it used to be yellow until the glow moved
     *  on. The same thresholds as sigBars and the legend. */
    var t=gsTone(sig), c='rgba('+t.rgb+','+t.a+')', xOver=!!t.x;
    var f=glow?'filter:drop-shadow(0 0 5px rgba('+t.rgb+',0.85));':'';
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
  /* ★★★ ON THE GPU MAP THE AIRCRAFT AND STATIONS ARE GPU SYMBOLS, NOT ELEMENTS (2026-10-13). As DOM markers MapLibre
   *  moved every one of them on every frame of a pan or a fly — 9 fps flying over WESSEX's ~650 aircraft on a
   *  Moto G35, 35 with them hidden. L.gpuMarker (vibemapgl-compat.js) draws them all in one GPU pass. Its icons are
   *  drawn UN-rotated with the heading as a property (4 colours = 4 images, not one per 3°), and the glow is baked
   *  into the image instead of a CSS filter. The Leaflet fallback keeps its DOM icons exactly as before. */
  var GPU=!!(window.__GL&&L.gpuMarker);
  function acIcon(glow,hdg,ls){
    if(GPU)return{svg:acSVG(false,0,ls),w:26,h:26,ax:13,ay:13,rot:hdg||0,glow:glow?acTone(ls||0).rgb:null};
    return icon(acSVG(glow,hdg,ls),26,26,13,13);
  }
  function gsIcon(glow,sig){
    if(GPU)return{svg:gsSVG(false,sig),w:30,h:36,ax:15,ay:36,rot:0,glow:glow?gsTone(sig).rgb:null};
    return icon(gsSVG(glow,sig),30,36,15,36);
  }
  function newMarker(ll,ic,o){return GPU?L.gpuMarker(ll,ic,o):L.marker(ll,Object.assign({icon:ic},o||{}));}

  // skin _sigMeterHTML: 3 signal bars
  function sigBars(dbfs){if(dbfs==null||dbfs===0)return 0;if(dbfs>-40)return 3;if(dbfs>-55)return 2;return 1;}
  function sigMeterHTML(dbfs){
    var bars=sigBars(dbfs);
    if(bars===0)return'<span style="font-size:11px;color:rgba(255,60,60,0.8);letter-spacing:0.5px;">&#10005; NO SIG</span>';
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
  function placeRing(latlng,cls,rgb){
    /* ★ On the GPU map a ring is a zero-size marker MapLibre keeps in place, with the ring INSIDE it:
     *  the ring animates its own transform (scale + centring), which MapLibre would overwrite if it
     *  were the marker element itself. */
    if(window.__GL){
      var wrap=document.createElement('div');wrap.style.width='0';wrap.style.height='0';
      var ring=document.createElement('div');ring.className='ring '+cls;ring.style.left='0';ring.style.top='0';
      if(rgb)ring.style.borderColor='rgba('+rgb+',0.85)';
      wrap.appendChild(ring);
      var gm=new maplibregl.Marker({element:wrap,anchor:'center'}).setLngLat([latlng.lng,latlng.lat]).addTo(map._gl);
      wrap._mk=gm;wrap._tid=setTimeout(function(){gm.remove();},4400);
      return wrap;
    }
    var pane=map.getPane('overlayPane');if(!pane)return null;
    var el=document.createElement('div');
    el.className='ring '+cls;
    if(rgb)el.style.borderColor='rgba('+rgb+',0.85)';
    pane.appendChild(el);
    try{var p=map.latLngToLayerPoint(latlng);el.style.left=p.x+'px';el.style.top=p.y+'px';}catch(e){}
    el._tid=setTimeout(function(){if(el.parentNode)el.parentNode.removeChild(el);},4400);
    return el;
  }
  function removeRing(el){if(!el)return;clearTimeout(el._tid);if(el._mk){el._mk.remove();return;}if(el.parentNode)el.parentNode.removeChild(el);}

  /* ★★★ THE TILE PREWARM IS GONE, AND REMOVING IT IS THE FIX FOR WHAT IT EXISTED TO HIDE.
   *     It fetched the destination's OSM tiles before a flyTo, because Leaflet asks for tiles when
   *     it ARRIVES and the map filled in square by square behind a smooth animation (Stuart,
   *     2026-09-04: "still buffering in the tiles as it animates different zoom levels").
   *  ★★★ THERE ARE NO TILES ANY MORE — this map draws our own vector data. So it was:
   *       • dead by construction: nothing it fetched could ever be displayed;
   *       • the LAST openstreetmap.org fetch in the app, and hammering that service from three
   *         clients at once is what got us BLOCKED on 2026-09-26;
   *       • worst of all, a DELAY — every flight waited up to 600 ms for images nobody would draw,
   *         so removing it makes the animation start sooner rather than merely cost less.
   *  ★ Callers fly immediately. Keeping a (centre, zoom, done) wrapper whose only job was to call
   *    its callback would leave something for the next reader to misunderstand. */

  // skin _fitToLatest: flyToBounds AC+GS with 30% padding, rings on arrival
  function fitToLatest(acKey,gsid){
    var acLL=acKey&&acM[acKey]?acM[acKey].getLatLng():null;
    var gsLL=gsid!=null&&gsM[gsid]?gsM[gsid].getLatLng():null;
    if(!acLL)return;
    removeRing(ringAC);ringAC=null;removeRing(ringGS);ringGS=null;
    map.once('moveend',function(){
      // ★ Rings in the marker's own colour, like its pulse (see .pglow).
      if(acKey&&acM[acKey])ringAC=placeRing(acM[acKey].getLatLng(),'ring-ac',acTone((acM[acKey]._d||{}).last_seen||0).rgb);
      if(gsid!=null&&gsM[gsid])ringGS=placeRing(gsM[gsid].getLatLng(),'ring-gs',gsTone(gsM[gsid]._sig).rgb);
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
      map.flyToBounds(fb,fo);
    }else{
      map.flyTo(acLL,5,{animate:true,duration:0.8});
    }
  }

  function glowGSfn(gsid){
    if(glowGS!=null&&gsM[glowGS]){
      gsM[glowGS].setIcon(gsIcon(false,gsM[glowGS]._sig));
      setPulse(gsM[glowGS],'pulse-gs',null);
    }
    glowGS=gsid;
    if(gsid!=null&&gsM[gsid]){
      gsM[gsid].setIcon(gsIcon(true,gsM[gsid]._sig));
      setPulse(gsM[gsid],'pulse-gs',gsTone(gsM[gsid]._sig).rgb);
    }
  }
  function glowACfn(key){
    if(glowAC&&acM[glowAC]){
      var d=acM[glowAC]._d||{};
      acM[glowAC].setIcon(acIcon(false,d.bearing||0,d.last_seen||0));
      acM[glowAC]._sigr='';
      setPulse(acM[glowAC],'pulse-ac',null);
    }
    glowAC=key;
    if(key&&acM[key]){
      var d=acM[key]._d||{};
      acM[key].setIcon(acIcon(true,d.bearing||0,d.last_seen||0));
      acM[key]._sigr='';
      setPulse(acM[key],'pulse-ac',acTone(d.last_seen||0).rgb);
    }
  }

  /* ★★ A LONG STATION SHORTENS TO ITS FIRST PART BEFORE IT ELLIPSISES. "via Riverhead, New York,
   *  USA" at 320 pt would otherwise read "via Riverhead, Ne…"; "via Riverhead" says the same thing
   *  whole. The full name is kept in the span's title and in the aircraft card. Re-run on resize,
   *  because a rotation to landscape gives the full name room again. */
  function fitToastStation(){
    var tg=toast.querySelector('.tg');
    if(!tg||!lastFlight||!lastFlight.gs)return;
    tg.textContent='via '+lastFlight.gs;
    if(tg.scrollWidth>tg.clientWidth+1){
      var first=String(lastFlight.gs).split(',')[0].trim();
      if(first)tg.textContent='via '+first;
    }
  }
  window.addEventListener('resize',function(){setTimeout(fitToastStation,150);});
  function showToast(){
    if(!lastFlight)return;
    /* ★ Built from text nodes, not innerHTML: the station name comes from the server. */
    toast.textContent='';
    var tf=document.createElement('span');tf.className='tf';tf.textContent=lastFlight.cs||'???';toast.appendChild(tf);
    if(lastFlight.gs){
      var tg=document.createElement('span');tg.className='tg';tg.textContent='via '+lastFlight.gs;
      tg.title=lastFlight.gs;toast.appendChild(tg);
    }
    var ta=document.createElement('span');ta.className='ta';ta.textContent=age(lastFlight.t);toast.appendChild(ta);
    fitToastStation();
    toast.classList.add('on');
    if(ageTimer)clearInterval(ageTimer);
    ageTimer=setInterval(function(){
      if(lastFlight){var el=toast.querySelector('.ta');if(el)el.textContent=age(lastFlight.t);}
    },15000);
  }
  toast.addEventListener('click',function(){
    if(!lastFlight||!lastFlight.key)return;
    fitToLatest(lastFlight.key,lastFlight.gsid);
    /* ★ AND OPEN ITS CARD (Stuart, 2026-09-28: tapping the "flight received" pill should show the
     *  pop-up on the plane — the pointer turned to a hand and nothing opened). Opened NOW, not on
     *  'moveend': the card is pinned to the marker and rides the fly, and an aircraft already in view
     *  produces no move at all, so a moveend-only open would silently never happen. */
    var m=acM[lastFlight.key];
    if(m&&m.openPopup){try{m.openPopup();}catch(e){console.error('HFDL: could not open the aircraft card',e);}}
  });

  // skin _buildACPopup table — Alt row dropped (never populates upstream),
  // Tracked (tracked_km accumulated path) in its place per design review.
  function buildACPopup(a,lat,lon,hdg){
    var fl=a.flight||a.reg||a.key;
    var dist=distKm(lat,lon);
    var spd=numOr(a.gnd_spd_kts,0)?Math.round(numOr(a.gnd_spd_kts,0))+' kts':'';
    var gsN=esc(gsNames[a.gs_id]||'');
    var fk=numOr(a.freq_khz,0);
    var freq=fk?(fk/1000).toFixed(3)+' MHz':'';
    var sl=numOr(a.sig_level,0);
    // ★ The 3-bar meter beside the figure (Stuart, 2026-09-28: "I dont know what constitutes a good signal
    //   or bad one") — the SAME sigBars thresholds as the ground-station popups, so the two never disagree.
    var sig=sl?sigMeterHTML(sl)+' <span style="margin-left:4px;">'+sl.toFixed(1)+' dBFS</span>':'';
    var tk=numOr(a.tracked_km,0);
    var tracked=tk?Math.round(tk).toLocaleString()+' km'+(spd?' &bull; '+spd:''):spd;
    var row=function(label,val){return val?'<tr><td style="color:rgba(${T.x},0.65);padding-right:8px;white-space:nowrap;">'+label+'</td><td style="color:rgba(${T.x},0.95);">'+val+'</td></tr>':'';};
    return '<div style="font-size:13px;letter-spacing:0.8px;">'
      +'<div style="font-size:15px;font-weight:600;color:rgba(${T.x},1);margin-bottom:6px;">'+esc(fl)+(a.icao&&a.icao!==fl?' <span style="font-size:12px;color:rgba(${T.x},0.65);">'+esc(a.icao)+'</span>':'')+'</div>'
      +'<table style="border-collapse:collapse;">'
      +row('Freq',freq)
      +row('Via',gsN)
      +row('Signal',sig)
      +row('Distance',dist?dist+' km':'')
      +row('Tracked',tracked)
      +row('Msgs',numOr(a.msg_count,0)||'')
      +row('Seen',age(numOr(a.last_seen,0)*1000))
      +'</table></div>';
  }

  // skin _fetchGS — fields: gs_id, location, frequencies[{freq_khz,enabled}],
  // last_sig_level, last_heard
  function fetchGS(){
    hostFetch('/addon/hfdl/groundstations').then(function(data){ if(!data) throw 0; return data; }).then(function(data){
      (Array.isArray(data)?data:[]).forEach(function(s){
        var id=s.gs_id;
        var name=s.location||('GS '+id);
        if(s.lat==null||s.lon==null)return;
        gsNames[id]=name;
        var freqs=Array.isArray(s.frequencies)?s.frequencies:[];
        var sig=numOr(s.last_sig_level,0)||null;
        var lastHeard=numOr(s.last_heard,0);
        var sigLine;
        if(sig){
          sigLine=sigMeterHTML(sig)+'<span style="font-size:11px;color:rgba(${T.x},0.60);margin-left:3px;">'+sig.toFixed(1)+' dBFS</span>';
        }else if(lastHeard>0){
          var el=Math.floor(Date.now()/1000-lastHeard),elStr;
          if(el<60)elStr=el+'s ago';else if(el<3600)elStr=Math.floor(el/60)+'m ago';
          else if(el<86400)elStr=Math.floor(el/3600)+'h '+Math.floor((el%3600)/60)+'m ago';
          else elStr=Math.floor(el/86400)+'d ago';
          sigLine='<span style="font-size:11px;color:rgba(255,80,60,0.70);">&#10005; No sig &mdash; last '+elStr+'</span>';
        }else{
          sigLine='<span style="font-size:11px;color:rgba(180,80,60,0.55);">&#10005; Never heard</span>';
        }
        var ph='<div style="font-size:14px;letter-spacing:1px;color:rgba(${T.x},0.95)">'+esc(name)+'</div>'
          +'<div style="margin-top:4px;display:flex;align-items:center;gap:6px;">'
          +'<span style="font-size:12px;color:rgba(${T.x},0.70);">Signal:</span>'+sigLine+'</div>';
        var d=distKm(s.lat,s.lon);
        if(d)ph+='<div style="margin-top:3px;font-size:12px;color:rgba(${T.x},0.65);">'+d+' km from receiver</div>';
        if(freqs.length){
          ph+='<div style="margin-top:6px;">';
          freqs.forEach(function(f){
            var k=numOr(f&&f.freq_khz,0),en=!f||f.enabled!==false;
            ph+='<div style="margin:3px 0;display:flex;align-items:center;gap:5px;">'
            +'<span style="color:'+(en?'rgba(${T.x},0.9)':'rgba(${T.x},0.40)')+';min-width:66px;font-size:13px;">'+(k?(k/1000).toFixed(3)+' MHz':'?')+'</span>'
            +(en?'<span style="color:rgba(80,220,80,0.7);font-size:11px;">&#9679; ON</span>':'<span style="color:rgba(255,80,80,0.35);font-size:11px;">&#9675; off</span>')
            +'</div>';
          });
          ph+='</div>';
        }else{
          ph+='<div style="color:rgba(${T.x},0.55);font-size:12px;margin-top:4px;">No frequencies listed</div>';
        }
        if(gsM[id]){
          gsM[id].setPopupContent(ph);
          if(gsM[id]._sig!==sig){
            gsM[id]._sig=sig;
            gsM[id].setIcon(gsIcon(glowGS===id,sig));
            if(glowGS===id)setPulse(gsM[id],'pulse-gs',gsTone(sig).rgb);
          }
        }else{
          var mk=newMarker([s.lat,s.lon],gsIcon(glowGS===id,sig),{zIndexOffset:500});
          mk._sig=sig;
          mk.addTo(map).bindPopup(ph,{maxWidth:240});
          /* ★ A station that is ALREADY the glowing one when its marker is first made (the first
           *  flight arrives before the station list) pulses from birth, not from the next flight. */
          if(glowGS===id)setPulse(mk,'pulse-gs',gsTone(sig).rgb);
          gsM[id]=mk;
        }
      });
      if(pendingFit){var pf=pendingFit;pendingFit=null;fitToLatest(pf.acKey,pf.gsid);}
    }).catch(function(){});
  }

  // skin _fetchAC — latest flight detection → toast + glow + snap
  function fetchAC(){
    hostFetch('/addon/hfdl/aircraft').then(function(data){ if(!data) throw 0; return data; }).then(function(data){
      var ac=Array.isArray(data)?data:[];
      cnt.textContent=ac.length;
      /* ★ Decide whether gliding is honest at this many aircraft BEFORE moving any of them, so
       *  the class is already right for this pass rather than one poll behind. */
      var _moved=0;
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
        var sig=acColour(a.last_seen)+'|'+(glowAC===a.key?1:0)+'|'+Math.round(hdg/3);
        if(acM[a.key]){
          /* ★★★ COUNT WHAT ACTUALLY MOVES, NOT WHAT EXISTS. I first gated the glide on the
           *  NUMBER OF AIRCRAFT, and Stuart corrected it: "with HFDL not all aircraft update at
           *  once... ADSB you'd have a point." Quite right — HFDL position reports arrive
           *  sporadically, so a receiver holding 600 aircraft may move a handful in a poll.
           *  Gliding those is nearly free, and switching the animation off at 600 would have
           *  killed it on exactly the busy receivers where it looks best.
           *  ★ ADS-B is the case the gate is really for: everything moving, every second. Counting
           *    MOVES rather than markers serves both without knowing which protocol is feeding it. */
          var _p=acM[a.key].getLatLng();
          if(!_p||Math.abs(_p.lat-a.lat)>1e-6||Math.abs(_p.lng-a.lon)>1e-6)_moved++;
          acM[a.key].setLatLng([a.lat,a.lon]);
          acM[a.key]._d={bearing:hdg,last_seen:a.last_seen};
          if(acM[a.key]._sigr!==sig){
            acM[a.key]._sigr=sig;
            acM[a.key].setIcon(acIcon(glowAC===a.key,hdg,a.last_seen));
            if(glowAC===a.key)setPulse(acM[a.key],'pulse-ac',acTone(a.last_seen).rgb);
          }
          if(acM[a.key].getPopup())acM[a.key].setPopupContent(pop);
        }else{
          var mk=newMarker([a.lat,a.lon],acIcon(glowAC===a.key,hdg,a.last_seen));
          mk._d={bearing:hdg,last_seen:a.last_seen};
          mk._sigr=sig;
          mk.addTo(map).bindPopup(pop,{maxWidth:220});
          // ★ Same for the aircraft: on the first poll the glow is chosen before its marker exists.
          if(glowAC===a.key)setPulse(mk,'pulse-ac',acTone(a.last_seen).rgb);
          acM[a.key]=mk;
        }
      });
      Object.keys(acM).forEach(function(k){
        if(!cur[k]){try{map.removeLayer(acM[k]);}catch(e){}delete acM[k];if(glowAC===k)glowAC=null;}
      });
      fetchGS();
      /* ★ Decided from the moves this poll actually made — see the note where _moved is
       *  counted. Re-evaluated every time, so a burst switches the glide off and a quiet
       *  minute brings it straight back. */
      if(window.__acGlideCheck)window.__acGlideCheck(_moved);
    }).catch(function(){cnt.textContent='\\u2715';});
  }
  fetchAC();
  setInterval(fetchAC,5000);
}

// ════════ ADS-B 1090 (V12) — the shared layer, fed by React Native ════════
/* ★★★ THE LAYER DRAWS THE AIRCRAFT — icons by category, the glide, the breathe (never on the first batch), auto-zoom,
 *  fit and range rings with their in-map toggles (web/mapkit/adsblayer.js, the web client's same file). This page only
 *  hosts it on HFDL's map: the receiver marker above, the count in the top bar, and the prefs handed back to React
 *  Native to keep per device. ★ Reports arrive by injection (__adsbPush), never a socket of the page's own: the app's
 *  spectrum socket already carries them, and a second would be a second listener on somebody's radio. */
if(KIND==='adsb'){
  var ADSB_PREFS=${JSON.stringify(opts?.adsbPrefs ?? null)};
  var adsbLayer=null;
  function adsbMake(){
    if(adsbLayer||!window.VibeAdsbLayer)return adsbLayer;
    try{
      adsbLayer=VibeAdsbLayer.create(map,{
        receiver:(RX_LAT||RX_LON)?{lat:RX_LAT,lon:RX_LON}:null,
        prefs:ADSB_PREFS,
        onPrefs:function(p){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage(JSON.stringify({t:'adsbPrefs',prefs:p}));},
        // ★ an icon tapped on the map (or its card closed): the live list's row follows
        onSelect:function(h){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage(JSON.stringify({t:'adsbSel',hex:h||null}));},
        theme:'dark',
        // ★ The page draws HFDL's receiver marker itself (addRx) — the layer is asked not to add a second.
        drawReceiver:false,
        // ★ V12: the owner's logos / silhouettes on this server (the card shows them when the server names one). Photos
        //   stay OFF until the Planespotters terms are confirmed — the TODO in adsblayer.js.
        imageBase:${JSON.stringify(opts?.local ? null : (opts?.radioUrl || baseUrl).replace(/\/+$/, '') + '/adsb/')}
      });
    }catch(e){console.error('ADS-B layer could not start',e);adsbLayer=null;}
    return adsbLayer;
  }
  window.__adsbPush=function(json){
    try{
      var a=JSON.parse(json);if(!Array.isArray(a))return;
      var placed=0;for(var i=0;i<a.length;i++)if(a[i]&&a[i].lat!=null&&a[i].lon!=null)placed++;
      cnt.textContent=String(placed);
      var L2=adsbMake();if(L2)L2.update(a);
    }catch(e){console.error('ADS-B: bad report',e);}
  };
  /* ★ FURTHEST (with NEWEST and LONGEST TRACKED) is the live list's header now — AdsbLiveList, adsbList.ts adsbChips —
   *  as on the web, whose top bar crowded on a portrait iPhone (d48dfe43). */
  /* ★ The capture warning, as text nodes (never HTML): [short, rest], or null to hide it. */
  window.__adsbCapture=function(w){
    var el=document.getElementById('capwarn');if(!el)return;
    if(!w||!w.length){el.style.display='none';return;}
    el.firstChild.textContent=String(w[0]);el.lastChild.textContent=String(w[1]||'');el.title=String(w[0])+String(w[1]||'');
    el.style.display='';
  };
  /* ★★ The feed warning, as a text node (never HTML): a string, or '' to hide it. × hides it until the wording changes. */
  window.__adsbFeedWarn=function(w){
    var el=document.getElementById('feedwarn');if(!el)return;
    w=String(w||'');
    if(!w||el.getAttribute('data-x')===w){el.style.display='none';return;}
    el.children[1].textContent=w;el.style.display='';
    el.children[2].onclick=function(){el.setAttribute('data-x',w);el.style.display='none';};
  };
  window.__adsbRx=function(lat,lon){RX_LAT=lat;RX_LON=lon;if(adsbLayer&&adsbLayer.setReceiver)adsbLayer.setReceiver(lat,lon);};
  /* ★ The live list (React Native, over this page): a row chosen = the layer's own focus (select, open the card, pan only
   *  if it is off screen); and the list's width, so the attribution keeps clear of it (the CSS above). */
  window.__adsbLegend=function(){try{var L5=adsbMake();if(L5&&L5.toggleLegend)L5.toggleLegend();}catch(e){console.error('ADS-B legend',e);}};
  window.__adsbFocus=function(h){try{var L3=adsbMake();if(L3&&L3.focus)L3.focus(String(h));}catch(e){console.error('ADS-B focus',e);}};
  window.__adsbListW=function(w,h){document.documentElement.style.setProperty('--adsbListW',(Math.max(0,Number(w)||0))+'px');
    /* ★ and the layer keeps aircraft out from under it: the list sits 10 px in from the bottom-left corner (EDGE) */
    try{var L4=adsbMake();if(L4&&L4.setObstacle)L4.setObstacle(w>0&&h>0?{corner:'bottom-left',w:Number(w)+20,h:Number(h)+20}:null);}catch(e){console.error('ADS-B obstacle',e);}};
  if(window.__pendingAdsb){window.__adsbPush(window.__pendingAdsb);window.__pendingAdsb=null;}
}

// ════════ NDB (V12) — the shared beacon layer, fed by React Native ════════
/* ★★ ADS-B'S PATTERN: web/mapkit/ndblayer.js draws every beacon at its published position with a 3-bar meter, the faint
 *  unheard ones, the receiver and a selected beacon's line to it, and ADS-B's own NM rings (VibeAdsbLayer.shared — which
 *  is why the ADS-B layer is injected first). Reports arrive by injection (__ndbPush): the app's spectrum socket already
 *  carries them. The layer takes the VibeMapGL instance (GLVM) on the GPU map, else the Leaflet map — never the compat
 *  adapter, which it does not know. A marker tapped is posted back (ndbSel) so the live list's row follows. */
if(KIND==='ndb'){
  var NDB_PREFS=${JSON.stringify(opts?.ndbPrefs ?? null)};
  var ndbLayer=null;
  function ndbMake(){
    if(ndbLayer||!window.VibeNdbLayer)return ndbLayer;
    try{
      ndbLayer=VibeNdbLayer.create(GLVM||map,{
        receiver:(RX_LAT||RX_LON)?{lat:RX_LAT,lon:RX_LON}:null,
        theme:'dark',prefs:NDB_PREFS||undefined,
        onSelect:function(k){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage(JSON.stringify({t:'ndbSel',key:k||null}));}
      });
    }catch(e){console.error('NDB layer could not start',e);ndbLayer=null;}
    return ndbLayer;
  }
  window.__ndbPush=function(json){
    try{
      var d=JSON.parse(json);if(!d||!Array.isArray(d.beacons))return;
      cnt.textContent=String(d.identified|0);
      var L2=ndbMake();if(L2)L2.update(d.beacons,Array.isArray(d.unheard)?d.unheard:[]);
    }catch(e){console.error('NDB: bad report',e);}
  };
  window.__ndbFocus=function(k){try{var L3=ndbMake();if(L3)L3.focus(String(k));}catch(e){console.error('NDB focus',e);}};
  window.__ndbSelect=function(k){try{var L3=ndbMake();if(L3)L3.select(String(k||''));}catch(e){console.error('NDB select',e);}};
  window.__ndbPrefs=function(p){try{var L4=ndbMake();if(L4){L4.setPrefs(p);if(p&&p.autofit)L4.fit();}}catch(e){console.error('NDB prefs',e);}};
  window.__ndbListW=function(w){document.documentElement.style.setProperty('--adsbListW',(Math.max(0,Number(w)||0))+'px');};
  if(window.__pendingNdb){window.__ndbPush(window.__pendingNdb);window.__pendingNdb=null;}
}
// ════════ V12 AIS / APRS — the shared track layer, fed by React Native ════════
/* ★★★ THE LAYER DRAWS THE SHIPS AND STATIONS (web/mapkit/tracklayer.js, the web client's same file): the glide, the fade,
 *  a tapped one's track and its card. Each item arrives with its icon, label and card already made (src/services/tracks.ts
 *  — one rule, two readers with the web), so this page holds no copy of what a ship looks like. A track is asked of React
 *  Native (AIS: the server's ais_trail; APRS: the list's own), answered through __trkTrail. */
if(KIND==='ais'||KIND==='aprs'||KIND==='dsc'){
  var trkLayer=null,trkWait={},trkWantFocus=null;
  function trkPost(o){if(window.ReactNativeWebView)window.ReactNativeWebView.postMessage(JSON.stringify(o));}
  function trkMake(){
    if(trkLayer||!window.VibeTrackLayer)return trkLayer;
    try{
      trkLayer=VibeTrackLayer.create(map,L,{
        receiver:(RX_LAT||RX_LON)?{lat:RX_LAT,lon:RX_LON}:null,
        icon:function(it){return it&&it._ic?it._ic:{html:'',w:20,h:20};},
        label:function(it){return it&&it._lb?String(it._lb):'';},
        card:function(it){return it&&it._cd?String(it._cd):'';},
        trail:function(key,done){
          trkWait[key]=done;trkPost({t:'trkTrail',key:String(key)});
          setTimeout(function(){if(trkWait[key]===done){delete trkWait[key];done([]);}},8000);
        },
        onSelect:function(k){trkPost({t:'trkSel',key:k==null?null:String(k)});}
      });
    }catch(e){console.error('track layer could not start',e);trkLayer=null;}
    return trkLayer;
  }
  window.__trkPush=function(json){
    try{
      var a=JSON.parse(json);if(!Array.isArray(a))return;
      cnt.textContent=String(a.length);
      var l=trkMake();if(!l)return;l.update(a);
      if(trkWantFocus){var k=trkWantFocus;trkWantFocus=null;setTimeout(function(){try{l.focus(k);}catch(e){}},80);}
    }catch(e){console.error('tracks: bad report',e);}
  };
  window.__trkTrail=function(key,pts){var d=trkWait[key];if(!d)return;delete trkWait[key];d(Array.isArray(pts)?pts:[]);};
  /* ★ A row chosen (the live list, or the APRS box before the map was up): before the first items, when they come. */
  window.__trkFocus=function(k){try{var l=trkMake();if(!l||!l.focus(String(k)))trkWantFocus=String(k);}catch(e){console.error('tracks focus',e);}};
  window.__trkListW=function(w,h){document.documentElement.style.setProperty('--adsbListW',(Math.max(0,Number(w)||0))+'px');};
  var fitBtn=document.getElementById('snap');
  if(fitBtn)fitBtn.addEventListener('click',function(){try{var l=trkMake();if(l)l.fit();}catch(e){console.error('tracks fit',e);}});
  if(window.__pendingTrk){window.__trkPush(window.__pendingTrk);window.__pendingTrk=null;}
}

// ════════ DIGI / CW spots (skin lsv-digmap / lsv-cwmap parity) ════════
if(KIND==='digi'||KIND==='cw'){
  var isCW=(KIND==='cw');
  /* ★★★ NO COUNT CAP. This held the newest 500 spot REPORTS, so with every filter on All the map
   *  topped out near 500 and then dropped older stations as new decodes arrived (Stuart, 2026-09-28:
   *  "a user said they wanted to see all spots"). The map draws one marker per call|band|mode, so
   *  that is all we keep: the newest report per station replaces its older one. Memory now grows with
   *  the number of distinct stations heard, not with time, and the markers are GPU circles. */
  var spots=[], byKey={}, markers={};
  var fMode=document.getElementById('fMode'), fBand=document.getElementById('fBand'), fAge=document.getElementById('fAge');

  // skin BAND_COLOUR — markers coloured by band, sized by SNR
  var BAND_COLOUR={'2200m':'#9b30d9','630m':'#c71585','160m':'#e8001e','80m':'#ff5500','60m':'#ff8c00',
    '40m':'#ffd700','30m':'#aacc00','20m':'#00cc44','17m':'#00ccaa','15m':'#00aaff','12m':'#0055ff','11m':'#6600ff','10m':'#cc00cc'};
  var BAND_RANGES={'2200m':[135700,137800],'630m':[472000,479000],'160m':[1800000,2000000],'80m':[3500000,4000000],
    '60m':[5250000,5450000],'40m':[7000000,7300000],'30m':[10100000,10150000],'20m':[14000000,14350000],
    '17m':[18068000,18168000],'15m':[21000000,21450000],'12m':[24890000,24990000],'11m':[26965000,27405000],'10m':[28000000,29700000],
    '6m':[50000000,54000000],'4m':[70000000,70500000],'2m':[144000000,148000000],'70cm':[420000000,450000000]};
  var BAND_ORDER=['2200m','630m','160m','80m','60m','40m','30m','20m','17m','15m','12m','11m','10m','6m','4m','2m','70cm'];

  function spotHz(f){f=numOr(f,0);if(!f)return 0;return f<1000?Math.round(f*1e6):Math.round(f);}
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
    if(!isCW&&fMode&&fMode.value!=='ALL'&&famOf(s.mode,FAMS)!==String(fMode.value).toUpperCase())return false;
    if(fBand&&fBand.value!=='ALL'&&spotBand(s)!==fBand.value)return false;
    var fa=fAge?fAge.value:'ALL';
    if(fa!=='ALL'){
      var ms={'2m':120000,'5m':300000,'15m':900000,'30m':1800000,'1h':3600000,'3h':10800000}[fa]||Infinity;
      if(Date.now()-s.t>ms)return false;
    }
    return true;
  }
  /* ★★ V12 (2026-10-26): DIGITAL SPOTS ARE COLOURED BY MODE, NOT BAND (Stuart: "most of our users will be using one SDR at
   *  a time so only able to cover one band at a time") — the table in src/utils/digiModes.ts, shared with the web client.
   *  A dark rim keeps the pale fills readable, and on the GPU map each circle carries its mode's LETTER (colour is never
   *  the only cue). Bigger circles than the band dots were, so the letter fits. CW's map keeps its band colours. */
  var MODE_STYLE=${JSON.stringify(DIGI_MODE_STYLE).replace(/</g, '\\u003c')}, RIM=${JSON.stringify(DIGI_MARKER_RIM)};
  /* ★ A mode's FAMILY (src/utils/digiModes.ts digiModeFamily — PSK31 / PSK63 → PSK, Q65-60A → Q65): the colour and the
   *  filter both go by it, so PSK63 spots are PSK's purple and a PSK filter shows them. */
  var FAMS=${JSON.stringify(legendModes.map((m) => m.toUpperCase())).replace(/</g, '\\u003c')};
  function famOf(m,list){m=String(m||'').toUpperCase();if(list.indexOf(m)>=0)return m;
    for(var i=0;i<list.length;i++){var k=list[i];if(m.indexOf(k+'-')===0||(m.indexOf(k)===0&&/^[0-9]+$/.test(m.slice(k.length))))return k;}return m;}
  function modeStyle(m){m=String(m||'').toUpperCase();if(MODE_STYLE[m])return MODE_STYLE[m];
    var f=famOf(m,Object.keys(MODE_STYLE));if(MODE_STYLE[f])return MODE_STYLE[f];
    return{colour:'#9E9E9E',ink:'#000000',letter:(m.match(/[A-Z0-9]/)||['?'])[0]};}
  function style(s){
    var ss=snrStyle(s.snr);
    if(!isCW){
      var ms=modeStyle(s.mode);
      return{radius:ss.r+2,fillColor:ms.colour,color:ss.glow?ms.colour:RIM,weight:ss.glow?2:1.2,opacity:0.9,fillOpacity:Math.max(0.8,ss.op),
             label:ms.letter,labelColor:ms.ink,labelSize:ss.r>=8?11:9};
    }
    var col=BAND_COLOUR[spotBand(s)]||'#aaaaaa';
    return{radius:ss.r,fillColor:col,color:ss.glow?col:'rgba(0,0,0,0.4)',weight:ss.glow?2:1,opacity:ss.glow?0.9:0.5,fillOpacity:ss.op};
  }
  // skin spot popup: call / mode·band·freq / SNR·country / age (+distance)
  function popup(s){
    var hz=spotHz(s.freq);
    var band=spotBand(s);
    return'<div style="font-size:14px;color:rgba(120,240,120,0.95);letter-spacing:1px;">'+esc(s.call)+'</div>'
    +'<div style="margin-top:4px;font-size:12px;color:rgba(80,200,80,0.70);">'
    +(s.mode?'<span style="margin-right:6px;">'+esc(s.mode)+'</span>':'')
    +(band?'<span style="margin-right:6px;">'+esc(band)+'</span>':'')
    +(hz?'<span>'+(hz/1e6).toFixed(4)+' MHz</span>':'')+'</div>'
    +'<div style="margin-top:3px;font-size:12px;color:rgba(80,200,80,0.55);">'
    +(s.snr!==undefined?'SNR: '+s.snr+' dB':'')+(s.wpm?' &bull; '+Math.round(s.wpm)+' wpm':'')
    +(s.country?' &bull; '+esc(abbr(s.country)):'')+'</div>'
    +'<div style="margin-top:2px;font-size:11px;color:rgba(80,200,80,0.35);">'+fmtSpotTime(s.t)+' UTC &bull; '+age(s.t)
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
    return'<div class="st-row"><span>'+esc(label)+'</span><span class="st-val">'+esc(n)+'</span></div>'
    +'<div class="st-bar"><div class="st-bf" style="width:'+Math.round(n/max*100)+'%"></div></div>';
  }
  function setH(id,v){var e=document.getElementById(id);if(e)e.innerHTML=v||'';}
  function setT(id,v){var e=document.getElementById(id);if(e)e.textContent=v||'';}
  function updateStats(){
    var vis=spots.filter(passes);
    if(!vis.length){
      setT('s-close','—');setT('s-far','—');setT('s-close-sub','');setT('s-far-sub','');
      setH('s-countries','<div style="font-size:11px;color:rgba(80,200,80,0.35)">No data</div>');
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
    setH('s-countries',ct.length?ct.map(function(c,i){return barRow((i+1)+'. '+c,cm[c],cm[ct[0]]);}).join(''):'<div style="font-size:11px;color:rgba(80,200,80,0.35)">No country data</div>');
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
  /* ★ THE PILL IS A WAY TO THE SPOT (Stuart, 2026-09-28: it showed a hand cursor and did nothing, as the
   *  HFDL one did). Tapping it flies to the newest spot and opens its card, as a tap on the circle would. */
  var lastSpot=null;
  toast.addEventListener('click',function(){
    if(!lastSpot)return;
    var m=markers[key(lastSpot)];
    try{
      map.flyTo([lastSpot.lat,lastSpot.lon],Math.max(map.getZoom(),4),{animate:true,duration:0.8});
      if(m&&m.openPopup)m.openPopup();
    }catch(e){console.error('spots: could not show the spot from the pill',e);}
  });
  function showSpotToast(s){
    var band=spotBand(s)||'';
    if(!s.call)return;
    lastSpot=s;
    // ★ In a span, so it ellipsises inside the lane that clears the zoom stack (see #toast).
    toast.textContent='';
    var tx=document.createElement('span');tx.className='tx';
    tx.textContent=s.call+(s.mode?' '+s.mode:'')+(band?' '+band:'');
    toast.appendChild(tx);
    toast.classList.add('on');
    clearTimeout(toast._tid);
    toast._tid=setTimeout(function(){toast.classList.remove('on');},3000);
  }
  function ingest(d){
    var s=norm(d||{},isCW);
    if(!s)return;
    var k=key(s), old=byKey[k];
    if(old){var i=spots.indexOf(old);if(i>=0)spots.splice(i,1);}
    byKey[k]=s;
    spots.unshift(s);
    upsert(s);
    updateCount();
    showSpotToast(s);
    if(statsOpen)updateStats();
  }
  function connect(){
    // Server validates this UUID against the registered /connection session —
    // random IDs are rejected with 400 before upgrade.
    var ws=new WebSocket(WSBASE+'/ws/dxcluster?user_session_id='+encodeURIComponent(UUID));
    ws.onopen=function(){
      ws.send(JSON.stringify({type:isCW?'subscribe_cw_spots':'subscribe_digital_spots'}));
    };
    ws.onmessage=function(e){
      if(typeof e.data!=='string')return;
      try{
        var m=JSON.parse(e.data);
        if((m.type==='digital_spot'&&!isCW)||(m.type==='cw_spot'&&isCW))ingest(m.data||{});
      }catch(x){
        // One bad spot is dropped, never silently: counted, and logged at most every 10 s.
        window.__badSpots=(window.__badSpots||0)+1;
        var nw=Date.now();if(!window.__badSpotLog||nw-window.__badSpotLog>10000){window.__badSpotLog=nw;
          console.error('[fault] map dxcluster: dropped bad spot ('+window.__badSpots+' total)',x);}
      }
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

/** ★ The ADS-B map's in-map toggles, per device (not per server: they are how this person likes the map). */
const ADSB_PREFS_KEY = 'lsv_adsb_map_prefs';
/** ★ V12 NDB: the beacon map's toggles (RINGS · UNHEARD · FIT), per device — the web's vibesdr.ndbMapPrefs. */
const NDB_PREFS_KEY = 'lsv_ndb_map_prefs';

/** SpotRow → the digital_spot `data` shape the WebView's norm() expects. */
function spotToData(s: SpotRow) {
  return {
    grid: s.grid, mode: s.mode, band: s.band, callsign: s.call,
    snr: s.snr, frequency: s.freqHz, distance_km: s.distKm, timestamp: s.time,
    country: s.country,
  };
}

export default function MapOverlay(
  { visible, kind, baseUrl, radioUrl, sessionUuid, onClose, local, wsBaseOverride, rxLat, rxLon, onPickCity, spots,
    disconnected, onBackToList, onRetry, adsbAircraft, adsbList, adsbStarted, adsbCapture, adsbFeedWarn, adsbGain, ndb, trk, safeInsets, spotModes }:
    MapOverlayProps & { spots?: SpotRow[]; spotModes?: string[] },
) {
  const webRef = useRef<WebView>(null);
  const { width: winW, height: winH } = useWindowDimensions();
  /* ★★ An OPAQUE full-screen modal over the receiver: the waterfall shader and the meters under it
   *  stop drawing while it is up (useScreenCovered) — otherwise the phone runs two GPU pictures, the
   *  map's and an invisible waterfall's. */
  useCoversScreen(visible && !!kind);
  /* ★★★ THE OS SAYS MEMORY IS TIGHT: THE MAP LETS GO FIRST. The renderer holds a RAM budget sized
   *  from the device (vibemap.js MEMORY BANDS) — preloaded layers, a data cache and canvas drawn
   *  past the screen edge so a drag never shows sea. On a memory warning it drops straight to the
   *  floor band: a narrower margin costs a moment of sea while dragging; being killed costs the
   *  whole map. Stuart: "use what we can without being intrusive". */
  useEffect(() => {
    const sub = AppState.addEventListener('memoryWarning', () => {
      webRef.current?.injectJavaScript('try{ if (window.VM && VM.shed) VM.shed(); }catch(e){} true;');
    });
    return () => sub.remove();
  }, []);
  const [ignored, setIgnored] = useState(false);

  // ★ Back (Backspace / Escape) closes the map. It is OUR page, so keys are not suppressed here — but nothing
  // was listening for a way out, and once inside a map a keyboard user was stuck.
  // ★★ AN OVERLAY (controller audit group B, 2026-10-20): it was not in the receiver screen's yield list, so the arrows
  //    and Enter pressed the receiver's controls BEHIND the map. Now the screen yields while it is up, and Back closes
  //    it only when it is on top — the city picker opened from it takes Back first (it used to close the MAP and leave
  //    the picker up).
  const closeRef = useRef(onClose); closeRef.current = onClose;
  useOverlay(visible, { name: 'map', onBack: () => closeRef.current() });
  useEffect(() => { if (!disconnected) setIgnored(false); }, [disconnected]);
  const injected = useRef<Set<string>>(new Set());
  /* ★★ THE GPU MAP'S FILES FIRST: copied into <documents>/mapgl/ once (mapglPack.ts). Until that
   *  answers, no page is built; if it FAILS, the page is built for Leaflet — a map, never a hole. */
  const [glOk, setGlOk] = useState<boolean | null>(null);
  const [glBase, setGlBase] = useState('mapgl/');
  useEffect(() => {
    if (!visible || glOk !== null) return;
    let dead = false;
    ensureMapglPack().then((b) => { if (!dead) { setGlBase(b); setGlOk(true); } })
      .catch((e) => { console.warn('GPU map files unavailable, using Leaflet:', e); if (!dead) setGlOk(false); });
    return () => { dead = true; };
  }, [visible, glOk]);
  // ★★ The user's colours go INTO the page (2026-10-12): text colour (neon reading under Nixie, lifted for contrast as
  //   every surface lifts it), controls colour for keys. A change rebuilds the page — which only happens after leaving
  //   this screen for the menus (Stuart: "to change the colour of the map means youd have to exit its screen anyway").
  const fp = useFaceplate();
  const textRgb = fp.settings.display === 'nixie' ? '255,154,85' : fp.text.rgb;
  const ink = useMemo(() => ({ x: controlsText(textRgb), k: fp.controls.rgb, kt: controlsText(fp.controls.rgb) }),
                      [textRgb, fp.controls.rgb]);
  /* ★★ THE ADS-B MAP'S TOGGLES ARE REMEMBERED PER DEVICE (auto-zoom, fit, rings — drawn by the shared layer, which
   *  hands every change back as an `adsbPrefs` message). Read once before the page is built and passed IN; a change
   *  is written to storage and kept in a ref, never put back into the page's source — that would rebuild the map
   *  under the user's finger for a toggle it has already applied. */
  const adsbPrefsRef = useRef<AdsbMapPrefs | null>(null);
  const [adsbPrefsReady, setAdsbPrefsReady] = useState(false);
  useEffect(() => {
    if (kind !== 'adsb' || adsbPrefsReady) return;
    let dead = false;
    AsyncStorage.getItem(ADSB_PREFS_KEY)
      .then((v) => { adsbPrefsRef.current = parseAdsbMapPrefs(v); })
      .catch(() => { adsbPrefsRef.current = parseAdsbMapPrefs(null); })
      .finally(() => { if (!dead) setAdsbPrefsReady(true); });
    return () => { dead = true; };
  }, [kind, adsbPrefsReady]);
  /* ★★ V12 NDB: its toggles (RINGS · UNHEARD · FIT) are remembered per device the same way — read once before the page
   *  is built and passed IN; a change goes to the layer by injection and to storage, never back into the page's source. */
  const [ndbPrefs, setNdbPrefs] = useState<NdbMapPrefs | null>(null);
  const ndbPrefsAtBuild = useRef<NdbMapPrefs | null>(null);
  useEffect(() => {
    if (kind !== 'ndb' || ndbPrefs) return;
    let dead = false;
    AsyncStorage.getItem(NDB_PREFS_KEY)
      .then((v) => { if (!dead) setNdbPrefs(parseNdbMapPrefs(v)); })
      .catch(() => { if (!dead) setNdbPrefs(parseNdbMapPrefs(null)); });
    return () => { dead = true; };
  }, [kind, ndbPrefs]);
  if (ndbPrefs && !ndbPrefsAtBuild.current) ndbPrefsAtBuild.current = ndbPrefs;
  const ndbPrefsReady = !!ndbPrefs;
  const html = useMemo(
    () => (kind && glOk !== null && (kind !== 'adsb' || adsbPrefsReady) && (kind !== 'ndb' || ndbPrefsReady) ? buildHtml(kind, baseUrl, sessionUuid,
      { local, wsBase: wsBaseOverride, rxLat, rxLon, gl: glOk, detail: glOk && detailInstalled(), glBase,
        adsbPrefs: adsbPrefsRef.current ?? undefined, radioUrl, ndbPrefs: ndbPrefsAtBuild.current ?? undefined, spotModes }, ink) : ''),
    // ★ adsbPrefsRef / ndbPrefsAtBuild are read at build time only (see above) — deliberately not dependencies.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [kind, baseUrl, radioUrl, sessionUuid, local, wsBaseOverride, rxLat, rxLon, glOk, glBase, ink, adsbPrefsReady, ndbPrefsReady, spotModes?.join(',')],
  );
  /* ★★★ THE PAGE IS WRITTEN TO DISK BESIDE THE MAP PACK AND LOADED FROM THERE.
   *
   *  ★★★ WHY, AND IT IS THE WHOLE POINT OF THIS REWORK. The page used to be handed to the WebView
   *      as an HTML STRING with the INSTANCE as its baseUrl, so that /addon/hfdl/* was same-origin
   *      (most UberSDRs send no CORS headers). That left the map data with nowhere to come from
   *      but the React Native bridge — ~15 MB of static geometry, several megabytes per layer,
   *      fetched one round trip at a time. The animation juddered and the map sometimes drew
   *      blank. Stuart: "that is what I want — maps stored on device so animations are smooth.
   *      That animation when it works is awesome."
   *  ★★ SO THE TWO WERE SWAPPED. The page now lives on a LOCAL origin with the pack unpacked
   *     beside it, and reads layers straight off the filesystem. The only thing left on the bridge
   *     is the aircraft feed — a few KB every five seconds, which is what a bridge is good at (see
   *     hostFetch). RN's own fetch is not bound by browser CORS, so an instance that sends none
   *     still works: the very thing the instance origin was there to buy.
   *  ★ Rewritten on every open. It is a few hundred KB and it must track the code that generates
   *    it — a cached page from a previous build is the kind of ghost nobody thinks to look for. */
  const [pageUri, setPageUri] = useState<string | null>(null);
  const [packError, setPackError] = useState<string | null>(null);
  useEffect(() => {
    if (!visible || !kind || !html) return;
    let dead = false;
    (async () => {
      try {
        await ensureMapPack();
        const uri = await writeMapPage(html);
        if (!dead) { setPageUri(uri); setPackError(null); }
      } catch (e) {
        /* ★ Say so rather than showing an empty map for ever. A failure here means no basemap at
         *  all, which is exactly the silent-blank case this rework exists to end. */
        if (!dead) setPackError(String((e as Error)?.message || e));
      }
    })();
    return () => { dead = true; };
  }, [visible, kind, html]);

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

  /* ★ ADS-B: the whole table each report (about once a second) — it REPLACES, it does not accumulate (the server drops
   *  an aircraft after 60 s), so there is nothing to de-duplicate. Before the page's handler exists the latest is
   *  parked in __pendingAdsb and picked up on start. ★★ Double-encoded: the JSON travels as a JS STRING literal, so a
   *  callsign can never become source (and U+2028/2029 are escaped, the mapdata rule below). */
  const pushAdsb = (list?: AdsbAircraftRaw[] | null) => {
    if (kind !== 'adsb' || !list) return;
    const lit = JSON.stringify(JSON.stringify(list))
      .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
      .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
    webRef.current?.injectJavaScript(
      `(window.__adsbPush?window.__adsbPush(${lit}):(window.__pendingAdsb=${lit}));true;`,
    );
  };
  useEffect(() => { if (visible) pushAdsb(adsbAircraft); }, [kind, visible, adsbAircraft]); // eslint-disable-line react-hooks/exhaustive-deps
  // ★★ The capture warning in the top bar (adsbCaptureWarning) — JSON literal, never source; only on a change.
  const capKey = adsbCapture ? adsbCapture.short + adsbCapture.rest : '';
  const pushCap = () => {
    if (kind !== 'adsb') return;
    webRef.current?.injectJavaScript(
      `window.__adsbCapture&&window.__adsbCapture(${JSON.stringify(adsbCapture ? [adsbCapture.short, adsbCapture.rest] : null)});true;`);
  };
  useEffect(() => { if (visible) pushCap(); }, [kind, visible, capKey]); // eslint-disable-line react-hooks/exhaustive-deps
  // ★★ The feed warning (AdsbReport.feedWarn) — JSON literal, never source; only on a change.
  const pushFeedWarn = () => {
    if (kind !== 'adsb') return;
    webRef.current?.injectJavaScript(`window.__adsbFeedWarn&&window.__adsbFeedWarn(${JSON.stringify(adsbFeedWarn ?? '')});true;`);
  };
  useEffect(() => { if (visible) pushFeedWarn(); }, [kind, visible, adsbFeedWarn]); // eslint-disable-line react-hooks/exhaustive-deps
  /* ★ The live list's two calls into the page. The hex is the report's cleaned address (/^~?[0-9a-f]{6}$/, adsbTypes),
   *  and JSON.stringify makes it a string literal whatever it is — never source. The width is kept, so a page that
   *  (re)loads after the list measured itself still gets it (onLoadEnd). */
  const adsbListW = useRef(0), adsbListH = useRef(0);
  const adsbLegend = () => webRef.current?.injectJavaScript('window.__adsbLegend&&window.__adsbLegend();true;');
  const [adsbSel, setAdsbSel] = useState<string | null>(null);
  const adsbFocus = (hex: string) =>
    webRef.current?.injectJavaScript(`window.__adsbFocus&&window.__adsbFocus(${JSON.stringify(String(hex))});true;`);
  const adsbFootprint = (w: number, h: number) => {
    adsbListW.current = w; adsbListH.current = h;
    webRef.current?.injectJavaScript(`window.__adsbListW&&window.__adsbListW(${Math.round(w) || 0},${Math.round(h) || 0});true;`);
  };

  /* ★★ V12 NDB: the layer's feed (ndbMapFeed — the web's renderMap) each report, double-encoded as ADS-B's is; the
   *  selected beacon (a marker tapped, or a row) is kept here so the list and the card follow. */
  const [ndbSel, setNdbSel] = useState(0);
  const ndbFeedLit = useMemo(() => {
    if (kind !== 'ndb' || !ndb?.report) return null;
    const f = ndbMapFeed(ndb.report, ndb.found);
    return JSON.stringify(JSON.stringify({ identified: ndb.report.identified, beacons: f.beacons, unheard: f.unheard }))
      .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
      .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
  }, [kind, ndb?.report, ndb?.found]);   // eslint-disable-line react-hooks/exhaustive-deps
  const pushNdb = () => {
    if (kind !== 'ndb' || !ndbFeedLit) return;
    webRef.current?.injectJavaScript(`(window.__ndbPush?window.__ndbPush(${ndbFeedLit}):(window.__pendingNdb=${ndbFeedLit}));true;`);
  };
  useEffect(() => { if (visible) pushNdb(); }, [visible, ndbFeedLit]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { if (kind !== 'ndb') setNdbSel(0); }, [kind]);
  const ndbJs = (fn: string, arg: unknown) =>
    webRef.current?.injectJavaScript(`window.${fn}&&window.${fn}(${JSON.stringify(arg)});true;`);
  const ndbChoose = (id: number, fromList: boolean) => {
    setNdbSel(id);
    if (!fromList) return;
    if (id) ndbJs('__ndbFocus', String(id)); else ndbJs('__ndbSelect', '');
  };
  const ndbSetPrefs = (p: Partial<NdbMapPrefs>) => {
    const next = { ...(ndbPrefs ?? parseNdbMapPrefs(null)), ...p };
    setNdbPrefs(next);
    AsyncStorage.setItem(NDB_PREFS_KEY, JSON.stringify(next)).catch(() => {});
    ndbJs('__ndbPrefs', p);
  };
  /* ★ V12 AIS / APRS: the items each report (they REPLACE — the server's list is the truth), double-encoded as ADS-B's are
   *  (a ship's name can never become source). Parked in __pendingTrk until the page's handler exists. */
  const isTrkKind = kind === 'ais' || kind === 'aprs' || kind === 'dsc';
  const trkRef = useRef(trk); trkRef.current = trk;
  const pushTrk = (items?: TrackItem[]) => {
    if (!isTrkKind || !items) return;
    const lit = JSON.stringify(JSON.stringify(items))
      .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
      .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
    webRef.current?.injectJavaScript(`(window.__trkPush?window.__trkPush(${lit}):(window.__pendingTrk=${lit}));true;`);
  };
  useEffect(() => { if (visible) pushTrk(trk?.items); }, [kind, visible, trk?.items]); // eslint-disable-line react-hooks/exhaustive-deps
  const [trkSel, setTrkSel] = useState<string | null>(null);
  const trkFocus = (key: string) => {
    setTrkSel(key);
    webRef.current?.injectJavaScript(`window.__trkFocus&&window.__trkFocus(${JSON.stringify(String(key))});true;`);
  };
  const trkListW = useRef(0), trkListH = useRef(0);
  const trkFootprint = (w: number, h: number) => {
    trkListW.current = w; trkListH.current = h;
    webRef.current?.injectJavaScript(`window.__trkListW&&window.__trkListW(${Math.round(w) || 0},${Math.round(h) || 0});true;`);
  };
  // ★ the APRS box's row: found once the page has its items (onLoadEnd), and again if it changes while open
  useEffect(() => { if (visible && isTrkKind && trk?.focus) trkFocus(trk.focus); }, [visible, kind, trk?.focus]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { if (!visible) setTrkSel(null); }, [visible, kind]);

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
        {/* ★ Until the pack is unpacked and the page written there is nothing to show. A WebView
            pointed at a file that does not exist yet renders a blank white document, which is
            indistinguishable from the map being broken. */}
        {packError ? (
          <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24 }}>
            <Text style={{ color: '#ffb0b0', fontSize: 12, textAlign: 'center' }}>
              {'The map could not be prepared on this device.\n' + packError}
            </Text>
          </View>
        ) : !pageUri ? null : (
        <WebView
          ref={webRef}
          // baseUrl gives the document the INSTANCE's origin so in-page
          // fetches (/addon/hfdl/*, /api/*) are same-origin — CORS is a
          // per-instance ubersdr config flag (Server.EnableCORS) and most
          // instances have it off, which silently killed the HFDL polls
          // (digi/CW maps survived because WebSockets bypass CORS).
          /* ★★★ A FILE ON DISK, NOT AN HTML STRING WITH A REMOTE BASE. See the note beside
           *  pageUri: this is what lets the renderer read its layers off the filesystem instead
           *  of pulling them a round trip at a time over the bridge. The instance is still
           *  reached — by React Native, through hostFetch — so nothing is lost by leaving its
           *  origin behind, and CORS stops applying at all.
           *  ★ allowFileAccess / allowFileAccessFromFileURLs are what let a file:// document read
           *    its siblings; without them the page loads and every layer 404s silently, which is
           *    the failure this whole rework is trying to stop happening quietly. */
          source={{ uri: pageUri as string }}
          /* ★★★ allowingReadAccessToURL IS THE iOS HALF, AND I SHIPPED WITHOUT IT. The three
           *  props below are ANDROID settings; on iOS WKWebView will load the file you point it
           *  at and refuse to read its SIBLINGS unless the enclosing directory is granted here.
           *  So the page came up, the sea drew, the small label files squeaked through on the
           *  bridge fallback and every large layer — tier0-cover, tier1-cover, megabytes each —
           *  died on the fallback's timeout. Stuart saw exactly that: "no map, just sea", then
           *  "I see labels for continents".
           *  ★ The grant is the page's own directory, nothing wider: the pack sits beside it. */
          allowingReadAccessToURL={(pageUri || '').replace(/\/[^/]*$/, '/')}
          allowFileAccess
          allowFileAccessFromFileURLs
          /* ★★★ allowUniversalAccessFromFileURLs IS GONE (security, 2026-10-03). It let this file:// page read
           *  ANY origin without CORS — nothing here needs it: the layers are siblings on disk (the file-URL
           *  grant above), the instance is fetched by React Native (hostFetch), and the spot WebSocket is not
           *  subject to CORS at all. allowFileAccess + allowFileAccessFromFileURLs STAY: without them every
           *  sibling layer read fails (see the note above).
           *  ★ originWhitelist stays '*' ON PURPOSE: react-native-webview hands any URL that fails the whitelist
           *    straight to the OS (Linking.openURL) — tel:, sms:, other apps' schemes. The filter is
           *    onShouldStartLoadWithRequest instead: the page itself (file:) loads, a tapped http(s) link (the
           *    map attribution) opens in the browser, and everything else is refused. */
          originWhitelist={['*']}
          onShouldStartLoadWithRequest={(req) => {
            const u = String(req?.url || '');
            if (/^(file:|about:blank|blob:|data:)/i.test(u)) return true;
            if (/^https?:\/\//i.test(u)) { Linking.openURL(u).catch(() => {}); }
            return false;
          }}
          style={mo.web}
          javaScriptEnabled
          /* ★★★ INSPECTABLE, BECAUSE FOUR BUILDS WENT ON GUESSES ABOUT WHAT THIS WEBVIEW WAS DOING.
           *  Permissions, a relative dataBase, a read-access URL, chunked delivery — each shipped,
           *  each plausible, none confirmed, because nothing could SEE the page. Meanwhile the one
           *  attempt at visibility (an on-screen error box in 335) wrapped console.error and logged
           *  per redraw, hard-locking the app: the diagnostic cost more than the bug.
           *  ★★ This is the cheap version of the same thing. It costs nothing at runtime, shows the
           *     user nothing, and lets Safari's Develop menu attach to the WebView on macOS and iOS
           *     — real console, real network, real answers instead of another plausible theory.
           *  ★★ DEV BUILDS ONLY (security, 2026-10-03): inspectable in a release build lets anyone holding
           *    the device attach to a page that can read files. A dev build still gets the whole console. */
          webviewDebuggingEnabled={__DEV__}
          /* ★★ IF THE OS KILLS THE PAGE ANYWAY, BRING IT BACK. WKWebView's content process can be
           *  reclaimed under memory pressure (and Android's renderer likewise), which leaves a blank
           *  white map that nothing ever redraws. Reloading restarts it on the floor it can afford. */
          onContentProcessDidTerminate={() => webRef.current?.reload()}
          onRenderProcessGone={() => { webRef.current?.reload(); }}
          domStorageEnabled
          allowsInlineMediaPlayback
          setSupportMultipleWindows={false}
          onLoadEnd={() => { injected.current = new Set(); pushSpots(spots); pushAdsb(adsbAircraft); pushCap(); pushFeedWarn(); pushNdb();
                             if (kind === 'adsb') adsbFootprint(adsbListW.current, adsbListH.current);
                             if (kind === 'ndb') ndbJs('__ndbListW', adsbListW.current);
                             if (isTrkKind) { pushTrk(trkRef.current?.items); trkFootprint(trkListW.current, trkListH.current);
                                              if (trkRef.current?.focus) trkFocus(trkRef.current.focus); } }}
          onMessage={(e) => {
            const d = e.nativeEvent.data;
            if (d === 'close') return onClose();
            if (d === 'pickCity') return onPickCity?.();
            /* ★★ THE PAGE ASKING US FOR BASEMAP DATA. Anything that is not one of the two legacy
             *  bare-string messages is JSON; an unparseable message is ignored rather than thrown,
             *  because a map that stops talking is worse than a map missing one layer. */
            let m: { t?: string; id?: string; file?: string } | null = null;
            try { m = JSON.parse(d); }
            catch (err) { noteFault('map-webview', 'bad-json', err, `len=${String(d).length}`); return; }
            /* ★ The ADS-B layer's toggles changed — remember them for this device (see adsbPrefsRef). Re-parsed, so
             *  only three booleans ever reach storage whatever the page sent. */
            // ★ V12 NDB: a marker tapped (or the selection cleared) — the list's row and the card follow
            if (m && (m as any).t === 'ndbSel') {
              const k = Number((m as any).key);
              ndbChoose(Number.isFinite(k) && k > 0 ? Math.floor(k) : 0, false);
              return;
            }
            if (m && (m as any).t === 'adsbSel') {
              const h = (m as any).hex;
              setAdsbSel(typeof h === 'string' && /^~?[0-9a-f]{6}$/.test(h) ? h : null);
              return;
            }
            /* ★ V12 AIS / APRS: the map's selection (the list's row follows), and a track asked for — answered with
             *  [lat, lon] numbers only, whatever the asker returns. The key is checked against the items we sent. */
            if (m && (m as any).t === 'trkSel') {
              const k = (m as any).key;
              setTrkSel(typeof k === 'string' && k.length <= 40 ? k : null);
              return;
            }
            if (m && (m as any).t === 'trkTrail') {
              const k = String((m as any).key ?? '').slice(0, 40);
              const answer = (pts: [number, number][]) => {
                const clean = (Array.isArray(pts) ? pts : []).filter((q) => Array.isArray(q) && Number.isFinite(q[0]) && Number.isFinite(q[1]))
                  .slice(-400).map((q) => [Number(q[0]), Number(q[1])]);
                webRef.current?.injectJavaScript(`window.__trkTrail&&window.__trkTrail(${JSON.stringify(k)},${JSON.stringify(clean)});true;`);
              };
              const t = trkRef.current;
              if (!t || !t.items.some((i) => i.key === k)) { answer([]); return; }
              t.trail(k).then(answer, () => answer([]));
              return;
            }
            if (m && (m as any).t === 'adsbPrefs') {
              const pr = parseAdsbMapPrefs(JSON.stringify((m as any).prefs ?? null));
              adsbPrefsRef.current = pr;
              AsyncStorage.setItem(ADSB_PREFS_KEY, JSON.stringify(pr)).catch(() => {});
              return;
            }
            /* ★★★ THE INSTANCE FETCH, DONE HERE INSTEAD OF IN THE PAGE. See hostFetch: RN is not
             *  bound by browser CORS, so this reaches an UberSDR that sends no CORS headers —
             *  which is what the instance-origin trick was for, and it frees the page to live on
             *  a local origin with the map pack on disk beside it.
             *  ★ A failure answers null rather than throwing: every caller in the page already
             *    treats that as "no data this tick", which is what a poll loop wants. */
            /* ★★ A BYTE RANGE OF THE HIGH-DETAIL PACK (mapglDetail.ts): the WebView cannot read part of a
             *  file, React Native can. A few KB per close-zoom tile; null = coarse map there. */
            if (m && (m as any).t === 'mgrange' && (m as any).id) {
              const id = String((m as any).id);
              const b64 = readDetailRange(Number((m as any).off) || 0, Number((m as any).len) || 0);
              webRef.current?.injectJavaScript(
                `window.__mgDeliver&&window.__mgDeliver(${JSON.stringify(id)},${b64 ? JSON.stringify(b64) : 'null'});true;`);
              return;
            }
            if (m && (m as any).t === 'hostfetch' && (m as any).id) {
              const id = String((m as any).id);
              const path = String((m as any).path || '');
              const deliver = (body: string) =>
                webRef.current?.injectJavaScript(
                  `window.__hfDeliver&&window.__hfDeliver(${JSON.stringify(id)},${body});true;`);
              /* ★★★ ONLY THE PATHS THE PAGE ACTUALLY ASKS FOR. This is React Native's fetch — no CORS, and on
               *  the instance's own origin — so an injected script must not be able to steer it anywhere else:
               *  '@' or '//' would change the HOST once appended to the base, '..' or '\\' would leave the path. */
              if (!isAllowedHostFetchPath(path)) { deliver('null'); return; }
              const url = baseUrl.replace(/\/+$/, '') + path;
              fetch(url, { signal: AbortSignal.timeout(10000) })
                .then(r => (r.ok ? r.text() : null))
                .then(t => {
                  /* ★ Parsed here so a malformed body becomes null rather than a syntax error
                   *  injected into the page — the RDS lesson: one bad payload must not take the
                   *  whole thing down. */
                  if (t === null) { deliver('null'); return; }
                  try { JSON.parse(t); } catch { deliver('null'); return; }
                  // ★ Valid JSON is not yet valid JS SOURCE: U+2028/2029 end a line there (see mapdata below).
                  deliver(t.replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
                           .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029'));
                })
                .catch(() => deliver('null'));
              return;
            }
            if (!m || m.t !== 'mapdata' || !m.id) return;
            const get = m.file ? MAPDATA_FILES[m.file] : undefined;
            /* ★ A file we do not carry (a tier2 shard, a relief tile) is answered with null, i.e.
             *  a 404 — the renderer's own "pack not installed" path, which draws a coarser map.
             *  ✗ Never leave the request unanswered: the page would wait out its timeout. */
            const body = get ? get() : null;
            /* ★★★ U+2028 AND U+2029 ARE LEGAL IN JSON AND ILLEGAL RAW IN JAVASCRIPT SOURCE, and
             *  this string is injected AS SOURCE. JSON.stringify does not escape them, so a single
             *  place name carrying one would produce a syntax error inside injectJavaScript —
             *  silently, since there is no console here — and exactly one map layer would vanish
             *  with nothing to find.
             *  ★ Today's pack contains none (checked, 27 files, 0 occurrences). That is luck about
             *  the DATA, not a property of the mechanism, and the next regeneration can change it. */
            const js = body === null ? 'null'
              : JSON.stringify(body)
                  .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
                  .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
            /* ★★★ CHUNKED ABOVE 256 KB. See __mdChunk: a single injection cannot carry a megabyte
             *  layer, which is why the map had sea and labels and no land for three builds.
             *  ★ The whole body is still JSON-escaped ONCE, then the escaped text is split — so a
             *    multi-byte character can never be cut in half by the boundary. */
            const CHUNK = 256 * 1024;
            if (js === 'null' || js.length <= CHUNK) {
              webRef.current?.injectJavaScript(
                `window.__mdDeliver&&window.__mdDeliver(${JSON.stringify(m.id)},${js});true;`,
              );
            } else {
              const n = Math.ceil(js.length / CHUNK);
              for (let i = 0; i < n; i++) {
                const part = js.slice(i * CHUNK, (i + 1) * CHUNK);
                webRef.current?.injectJavaScript(
                  `window.__mdChunk&&window.__mdChunk(${JSON.stringify(m.id)},${i},${n},${
                    JSON.stringify(part)});true;`,
                );
              }
            }
          }}
        />
        )}
        {/* ★★ V12 THE ADS-B LIVE LIST, bottom-left over the map — RN-drawn so the pad and keyboard reach it
            (AdsbLiveList's note). Only once the page is there: before that there is no map to find anything on. */}
        {kind === 'adsb' && !!pageUri && !packError && (
          <AdsbLiveList aircraft={adsbList ?? []} started={!!adsbStarted} onFocus={adsbFocus} selected={adsbSel} onFootprint={adsbFootprint} onLegend={adsbLegend}
                        raw={adsbAircraft ?? null}
                        gain={adsbGain?.view ?? null} gainSteps={adsbGain?.steps}
                        onGainAuto={adsbGain?.onAuto} onGainSet={adsbGain?.onSet}
                        receiver={Number.isFinite(rxLat as number) && Number.isFinite(rxLon as number) && (rxLat || rxLon)
                          ? { lat: rxLat as number, lon: rxLon as number } : null}
                        logoBase={local ? null : baseUrl.replace(/\/+$/, '') + '/adsb/'}
                        ink={ink} areaW={winW} areaH={winH}
                        insets={safeInsets ?? { top: 0, bottom: 0, left: 0, right: 0 }} />
        )}
        {/* ★★ V12 THE NDB LIVE LIST, bottom-left over the map, and the beacon card and dialogs — RN-drawn so the pad and
            keyboard reach them (AdsbLiveList's reason). */}
        {kind === 'ndb' && !!pageUri && !packError && !!ndb && (
          <NdbLiveList ndb={ndb} selected={ndbSel} onSelect={(id) => ndbChoose(id, true)}
                       prefs={ndbPrefs ?? parseNdbMapPrefs(null)} onPrefs={ndbSetPrefs}
                       onFootprint={(w) => { adsbListW.current = w; ndbJs('__ndbListW', w); }}
                       ink={ink} areaW={winW} areaH={winH}
                       insets={safeInsets ?? { top: 0, bottom: 0, left: 0, right: 0 }} />
        )}
        {/* ★★ V12 THE AIS / APRS LIVE LIST — AdsbLiveList's corner and reasons (TrackLiveList). */}
        {isTrkKind && !!trk && !!pageUri && !packError && (
          <TrackLiveList title={kind === 'ais' ? 'SHIPS' : kind === 'dsc' ? 'CALLS' : 'STATIONS'} rows={trk.rows} status={trk.status} empty={trk.empty}
                         safety={trk.safety} onFocus={trkFocus} selected={trkSel} onFootprint={trkFootprint}
                         ink={ink} areaW={winW} areaH={winH}
                         insets={safeInsets ?? { top: 0, bottom: 0, left: 0, right: 0 }} />
        )}
        {disconnected && !ignored && (
          <View style={mo.discWrap} pointerEvents="box-none">
            <NavCard name="map disconnected" onBack={() => setIgnored(true)}>
            <View style={mo.discCard}>
              <Text style={mo.discTitle}>Receiver disconnected</Text>
              <Text style={mo.discBody}>The server dropped the connection — spots have stopped. You can stay on the map (already-plotted spots remain) or reconnect.</Text>
              <View style={mo.discRow}>
                <CardKey ringColor="#ffffff" style={[mo.discBtn, mo.discBtnAlt]} onPress={() => onBackToList?.()} activeOpacity={0.85}>
                  <Text style={mo.discBtnAltTxt}>Back to list</Text>
                </CardKey>
                <CardKey preferred ringColor="#ffffff" style={mo.discBtn} onPress={() => { setIgnored(true); onRetry?.(); }} activeOpacity={0.85}>
                  <Text style={mo.discBtnTxt}>Retry</Text>
                </CardKey>
                <CardKey ringColor="#ffffff" style={[mo.discBtn, mo.discBtnAlt]} onPress={() => setIgnored(true)} activeOpacity={0.85}>
                  <Text style={mo.discBtnAltTxt}>Ignore</Text>
                </CardKey>
              </View>
            </View>
            </NavCard>
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
