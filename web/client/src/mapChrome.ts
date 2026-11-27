/**
 * mapChrome.ts — ★★★★ THE MAP CARD SYSTEM (V12 beta96–97, Stuart's design, signed off 11-27): the full-screen map view
 * with its chips, cards, LIST drawer (and the live waterfall in it), the short-landscape top row and the safe rectangle,
 * RATE, HELP, STATS, the shared-tuner pill and the bottom-right group. Every web map mounts THIS: the air & sea maps, the
 * ISM maps, and (ported onto it) the amateur / spots map and the directory's community map.
 *
 *   ★ THE STABLE SURFACE is `mountMapChrome()` / `MapChrome` (mapChromeApi.ts; docs/v12/MAP-CONTROL-CARDS.md "The
 *     module"). `TrackView` is the implementation; the existing maps still drive it directly.
 *
 * ★★ MADE ON ENTRY, DESTROYED ON EXIT — a MapLibre map holds a WebGL context and tens of MB of tiles; a listener who
 *    leaves for the evening must not carry it (the 1 GB phones).
 * ★ Everything shown came off the air or a server: every string is escaped where it meets HTML (esc).
 */
import { wireIslandSide } from './islandSide';
import { padHighlight, activePadLayout } from './gamepad';
import { padFace } from './padFace';
import { probeMapGL, loadMapGLScripts, mapglLoad } from './mapgl';
import { loadLeafletInto } from './leafletAsset';
import { dataUrl } from './origin';
import { padRehighlight } from './gamepad';
import { loadVibemapSource } from './chunkCache';
import { MAP_AUTOFIT_JS } from '../../../src/utils/mapAutoFit';
import { MESH_ROUTE_JS } from '../../../src/utils/meshRoute';   // ★★ beta96 a mesh node's route, drawn hop by hop   // ★ beta92 ONE auto fit for every map (the ADS-B map's)
import { CARD_IDS, CARD_NARROW_W, CARD_W, CARD_W_SNUG, CARD_ROOMY_W, cardPlace, oneSide, MAP_HELP, MAP_HELP_NOTES, cardOpenRule, STICKY_CARDS, CARD_IDLE_MS, cardDefaults, cardLimit as cardLimitFor, cardToggle, trimOutliers, type CardId, shortMap } from '../../../src/utils/mapCards';
import { modeIconSvg } from '../../../src/utils/modeIcons';
import { wireEdgeDock as wireEdgeDockT, edgeDockSet as edgeDockSetT } from './healthTuck';
import { edgeDockSet, wireEdgeDock } from './healthTuck';
import { TUNE_ICON_PATH } from '../../../src/services/airSeaTune';

/** ★★ b96-rangefeed `approx`: words for a position that is not exact (the locator square's centre — rxPosition.ts) */
export type LatLon = { lat: number; lon: number; approx?: string };
export function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}
export function ago(s: number): string {
  const v = Math.max(0, Math.round(s));
  return v < 5 ? 'now' : v < 60 ? `${v} s` : v < 3600 ? `${Math.round(v / 60)} min` : `${Math.floor(v / 3600)} h ${Math.round((v % 3600) / 60)} min`;
}

// ── the overlay ───────────────────────────────────────────────────────────────────────────────────────────────────
const CSS = `
.trkView{position:fixed;inset:0;min-width:100vw;z-index:600;background:#0b1014;color:#dfe7ef;display:flex;flex-direction:column;
  font:13px/1.35 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.trkView[hidden]{display:none}
.trkTop{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:8px 10px;padding-top:max(8px,var(--sai-t));
  background:#121a22;border-bottom:1px solid #22303d;flex-wrap:wrap}
.trkTop .btn{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:6px;padding:6px 10px;font:600 12px/1 system-ui,sans-serif;cursor:pointer}
.trkTop .btn:hover,.trkTop .btn:focus-visible{background:#2a3a4a;outline:none}
.trkTop .btn.exit{background:#5a1f1f;border-color:#8a3434}
.trkTop .ttl{font:700 13px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em}
.trkTop .st{color:#9fb0c2;font-size:12px;flex:1 1 200px;min-width:0}
.trkTop .slot-share{display:inline-flex;align-items:center;gap:6px;flex:none}
.trkTop .slot-share:empty,.trkTop .slot-share>[hidden]{display:none}
.trkTop .mapShLine{background:rgba(255,190,80,.10);color:#ffcf7a;border-color:rgba(255,190,80,.45);font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;white-space:nowrap}
.trkTop .mapShLine.alone{color:#7bd88f;border-color:rgba(123,216,143,.45);background:rgba(123,216,143,.08)}
.trkTop .mapShLine.locked{color:#ff9d8a;border-color:rgba(255,120,100,.55);background:rgba(255,90,70,.10)}
.trkTop .btn.dialLocked{opacity:.4;filter:grayscale(.7);cursor:not-allowed}
.tunePop{position:fixed;z-index:950;max-height:min(70vh,520px);overflow:auto;background:#101820;border:1px solid #2c3e52;border-radius:10px;box-shadow:0 10px 30px rgba(0,0,0,.55);padding:6px}
.tunePop[hidden]{display:none}
.tunePop .tpG{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7;padding:8px 8px 4px}
.tunePop .tpRow{display:flex;align-items:center;gap:9px;width:100%;text-align:left;background:none;border:0;border-radius:6px;color:#e8eef5;padding:8px;font:600 13px/1.2 system-ui,sans-serif;cursor:pointer}
.tunePop .tpRow:hover,.tunePop .tpRow:focus-visible{background:#1e2a36;outline:none}
.tunePop .tpRow svg{flex:none;color:#9fc6e6}
.tunePop .tpRow .nm{flex:none}
.tunePop .tpRow .fq{flex:1 1 auto;color:#9fb0c2;font:12px/1.2 ui-monospace,Menlo,monospace;text-align:right}

.tunePop .tpRow.live{background:rgba(91,227,107,.10)}
.tunePop .tpRow[aria-disabled="true"]{opacity:.42;cursor:not-allowed}
.trkKey{position:absolute;right:10px;bottom:30px;z-index:900;background:rgba(11,20,32,.92);border:1px solid #2c3e52;border-radius:8px;color:#d7dee6;font:12px/1.3 inherit;max-height:60%;overflow:auto}
.trkKey .kb{padding:2px 10px 8px}
.trkKey .kr{display:flex;align-items:center;gap:8px;padding:2px 0;white-space:nowrap}
.trkKey .kr svg{flex:none}
@keyframes trkLive{50%{opacity:.35}}
.trkList .hd .tabs{display:inline-flex;gap:2px;flex-wrap:wrap}
.trkList .hd .tab{background:none;border:0;border-bottom:2px solid transparent;color:#8fa2b5;font:700 11px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.05em;padding:2px 5px;cursor:pointer}
.trkList .hd .tab[aria-selected="true"]{color:#e8eef5;border-bottom-color:#ffd34d}
.trkList .hd .tab .tn{color:#ffd34d;margin-left:4px}
.trkList .hd .tab:focus-visible,.trkList .hd .srt button:focus-visible{outline:2px solid #ffd34d}
.trkList .hd .srt{display:inline-flex;margin-left:4px}
.trkList .hd .srt button{background:#1e2a36;color:#cfd9e3;border:1px solid #33475a;font:700 9.5px/1 ui-monospace,Menlo,monospace;padding:3px 5px;cursor:pointer;margin-left:-1px}
.trkList .hd .srt button:first-child{border-radius:4px 0 0 4px;margin-left:0}.trkList .hd .srt button:last-child{border-radius:0 4px 4px 0}
.trkList .hd .srt button[aria-pressed="true"]{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkList .hd .srt[hidden],.trkList .hd .tabs[hidden],.trkList .hd .lt[hidden],.trkList .filt[hidden]{display:none}
.trkList .filt{display:flex;align-items:center;gap:6px;padding:4px 10px;font-size:11px;color:#cfd9e3;background:#132030;border-bottom:1px solid #22303d}
.trkList .filt button{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:4px;font:600 10px/1 system-ui,sans-serif;padding:3px 6px;cursor:pointer}
.trkMsg .lbl{color:#7fc8ff}
.trkCard .nvTr{background:#1d2a36;color:#e3eaf1;border:1px solid #3c5266;border-radius:5px;font:600 11px ui-monospace,Menlo,monospace;padding:3px 8px;cursor:pointer}
.trkCard .nvTr.on{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard .nvTr:focus-visible{outline:2px solid #ffd34d}
.trkList .sec{padding:4px 10px;font:700 10px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.06em;color:#7f93a7;background:#0d151c;border-bottom:1px solid #151f29}
.trkMapHost{position:relative;flex:1 1 auto;min-height:0}
.trkMapHost .trkNote{position:absolute;left:50%;top:12px;transform:translateX(-50%);background:rgba(10,14,20,.9);padding:6px 10px;border-radius:6px;z-index:800;font-size:12px}
.trkList{position:absolute;left:10px;bottom:calc(10px + var(--sai-b));z-index:700;width:min(430px,calc(100% - 20px));
  max-height:min(46%,420px);display:flex;flex-direction:column;background:rgba(10,14,20,.93);border:1px solid #2c3a4a;border-radius:8px;overflow:hidden}
.trkList .hd{display:flex;align-items:center;gap:8px;padding:6px 10px;font:700 11px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.05em;
  color:#b9c7d5;cursor:pointer;user-select:none;border-bottom:1px solid #22303d}
.trkList .hd .n{color:#ffd34d}
.trkList .rows{overflow:auto;flex:1 1 auto;min-height:0}
.trkList .safety{padding:6px 10px;background:#3a2a10;color:#ffe2a8;font-size:12px;border-bottom:1px solid #5a4316}
.trkRow{display:grid;grid-template-columns:minmax(0,1fr) auto auto auto;gap:2px 10px;align-items:center;padding:5px 10px;cursor:pointer;border-bottom:1px solid #151f29}
.trkRow:hover,.trkRow:focus-visible,.trkRow.sel{background:#1a2632;outline:none}
.trkRow.sel{box-shadow:inset 3px 0 0 #ffd34d}
.trkRow.nopos{opacity:.65}
.trkRow.em{background:#4a1515}
.trkRow .id{min-width:0}
.trkRow .id b{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:600}
.trkRow .id small{display:block;color:#93a3b4;font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.trkRow .sw{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:1px}
.trkRow .sp,.trkRow .rg,.trkRow .ag{font:12px/1.2 ui-monospace,Menlo,monospace;color:#cfd9e3;white-space:nowrap;text-align:right}
.trkRow .ag{color:#8596a7;min-width:2.6em}
.trkRow .ag .agS{display:none}
.trkRow.stale{opacity:.4}
.trkList .hd .lPause{all:unset;box-sizing:border-box;flex:none;display:inline-flex;align-items:center;gap:3px;padding:3px 7px;border-radius:6px;background:#3a2c0a;border:1px solid #ffd34d;color:#ffd34d;font:700 11px/1 ui-monospace,Menlo,monospace;cursor:pointer}
.trkList .hd .lPause[hidden]{display:none}
.trkList .hd .lPause:focus-visible{outline:2px solid #ffd34d;outline-offset:1px}
.trkRow.kvRow{grid-template-columns:minmax(0,1fr) auto auto!important}
.trkRow .id b .kv{display:inline-flex;align-items:stretch;flex:none;margin-left:6px;border:1px solid #33475a;border-radius:4px;font:600 9.5px/14px ui-monospace,Menlo,monospace;color:#b8c6d4;white-space:nowrap;vertical-align:1px}
.trkRow .id b .kv span{padding:0 4px}
.trkRow .id b .kv i{width:1px;background:#33475a}
@media (max-width:359px){.trkRow .id b .kv .kt{display:none}}
.trkRow .ag{text-align:right}
.trkView[data-narrow] .trkRow .ag .agL{display:none}.trkView[data-narrow] .trkRow .ag .agS{display:inline}.trkView[data-narrow] .trkRow .brg{display:none}
.trkView[data-narrow] .trkRow{grid-template-columns:minmax(0,1fr) auto auto auto;column-gap:7px}
.trkRow .brg,.trkRow .dim{color:#8596a7}
.trkRow.dx{box-shadow:inset 3px 0 0 #ffd34d}.trkRow.dx.sel{box-shadow:inset 3px 0 0 #ffd34d}
.trkRow .snrChip{margin:0 5px 0 0}
.trkRow .dxTag{display:inline-block;margin-right:5px;padding:0 4px;border-radius:3px;background:#ffd34d;color:#111;font:700 9.5px/14px ui-monospace,Menlo,monospace;letter-spacing:.6px;vertical-align:1px}
.trkView i.up{color:#6dd36d;font-style:normal}
.trkView i.dn{color:#ff8a6b;font-style:normal}
.trkEmpty{padding:12px;color:#9fb0c2;font-size:12px}
.trkCard .t{font-size:13px;margin:0 18px 6px 0;color:#fff}
.trkCard .r{display:grid;grid-template-columns:7.2em minmax(0,1fr);gap:8px;padding:1px 0}
.trkCard .k{color:#8596a7}
.trkCard .v{color:#e3eaf1;word-break:break-word}
.trkCard .raw{margin-top:6px;padding:5px 6px;background:#0d151c;border-radius:4px;color:#9fb0c2;font-size:11px;word-break:break-all}
.trkView .flag{font-size:14px}
.trkView .avCredit{padding:6px 10px 8px;font:10px/1.35 ui-monospace,Menlo,monospace;color:#8a9aaa;border-top:1px solid #151f29}
.trkView .avCredit a{color:#9ccfff}
.trkView .cc{font:700 10px/1 ui-monospace,monospace;background:#2a3a4a;border-radius:3px;padding:1px 3px}
@media (max-width:520px){
  .trkTop .scr{display:flex;align-items:center;gap:8px;flex:1 1 0;min-width:0;overflow-x:auto;overscroll-behavior-x:contain;scrollbar-width:none;
    -webkit-mask-image:linear-gradient(to right,#000 calc(100% - 14px),transparent 100%);mask-image:linear-gradient(to right,#000 calc(100% - 14px),transparent 100%)}
  .trkTop .scr::-webkit-scrollbar{display:none}
  .trkTop .scr>*{flex:0 0 auto}
  .trkTop .scr .ttl{white-space:nowrap}
  .trkTop .st{flex:1 0 100%}
  .trkList{left:6px;right:6px;width:auto;max-height:34%}
  /* ★ Stuart 13:08 ("the time received is missing"): the age stays at every width, compact ("13s", "2m"); the bearing
   *  letters drop first, the subtitle always truncates first */
  .trkRow{grid-template-columns:minmax(0,1fr) auto auto auto;column-gap:7px}
  .trkRow .brg{display:none}
  .trkRow .ag .agL{display:none}
  .trkRow .ag .agS{display:inline}
  .vtl-card{top:6px !important;right:6px !important;left:52px;max-width:none !important;max-height:30% !important;font-size:11px !important}
}
/* ★★★ beta96 THE MAP CONTROL CARDS (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md): the map takes centre stage. ONE bar with
 *  only what must always be there — the way out, the shared tuner line + CHAT, the row of small card buttons, and the live
 *  message rate top right; everything else in CARDS that pop out of those buttons and shrink back (the health chips'
 *  and the app's SERVERS button's shape). How many may be open follows the WINDOW (cardLimit). */
.trkTop{flex-wrap:nowrap;gap:6px;min-height:40px;box-sizing:border-box}
.trkTop .st{position:absolute!important;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap;flex:none}
.trkTop .slot-top{display:inline-flex;align-items:center;gap:6px;flex:none}
.trkTop .slot-top:empty{display:none}
.trkCards{position:absolute;right:var(--trk-rail-w,84px);top:8px;bottom:8px;z-index:760;display:flex;flex-direction:row-reverse;gap:8px;align-items:flex-start;pointer-events:none;max-width:calc(100% - var(--trk-rail-w,84px) - 8px)}
.trkCard2{pointer-events:auto;display:flex;flex-direction:column;min-height:0;flex:0 1 auto;background:rgba(10,14,20,.94);border:1px solid #2c3a4a;
  border-radius:10px;box-shadow:0 8px 24px rgba(0,0,0,.45);overflow:hidden;transform-origin:top right;animation:trkCardIn .16s ease-out}
@keyframes trkCardIn{from{opacity:0;transform:scale(.92) translateX(8px)}to{opacity:1;transform:none}}
@media (prefers-reduced-motion:reduce){.trkCard2{animation:none}}
.trkCard2[hidden]{display:none}
.trkCard2>.ch{display:flex;align-items:center;gap:8px;padding:6px 6px 6px 10px;border-bottom:1px solid #22303d;flex:none;
  font:800 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#9fb0c2}
.trkCard2>.ch .x{margin-left:auto;background:none;border:1px solid transparent;border-radius:6px;color:#9fb0c2;font:600 15px/1 system-ui,sans-serif;padding:3px 7px;cursor:pointer}
.trkCard2>.ch .x:hover,.trkCard2>.ch .x:focus-visible{border-color:#7fc8ff;color:#fff;outline:none}
.trkCard2>.cb{overflow:auto;min-height:0;flex:1 1 auto;overscroll-behavior:contain;touch-action:pan-y;-webkit-overflow-scrolling:touch}
/* ★ Stuart 18:1x ("the CHAT card doesn't scroll"): every card's body and the chat's own log scroll by touch — pan-y on the
 *  scroller itself, and a card never taller than its column (min-height 0 lets it shrink, the body takes the rest) */
.trkCards .trkCard2{min-height:0;max-height:100%}
.trkCard2[data-card="chat"] .chatBody #chatLog{touch-action:pan-y;-webkit-overflow-scrolling:touch;overscroll-behavior:contain}
.trkCard2 .facts{padding:6px 10px;color:#9fb0c2;font-size:12px;border-bottom:1px solid #151f29}
.trkCard2 .facts:empty{display:none}
.trkCard2 .sect{padding:8px 10px;border-bottom:1px solid #151f29}
.trkCard2 .sect:last-child{border-bottom:0}
.trkCard2 .sect[hidden]{display:none}
.trkCard2 .sh{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7;margin:0 0 6px}
.trkCard2 .row{display:flex;flex-wrap:wrap;gap:5px;align-items:center}
.trkCard2 .btn{background:#1e2a36;color:#e8eef5;border:1px solid #33475a;border-radius:6px;padding:6px 10px;font:600 12px/1 system-ui,sans-serif;cursor:pointer}
.trkCard2 .btn:hover,.trkCard2 .btn:focus-visible{background:#2a3a4a;outline:none;border-color:#7fc8ff}
.trkCard2 .btn[aria-pressed="true"]{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard2 .btn.on{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard2 .btn.dialLocked{opacity:.4;filter:grayscale(.7);cursor:not-allowed}
.trkCard2 .rng{display:inline-flex;flex-wrap:wrap;gap:0}
.trkCard2 .rng[hidden],.trkCard2 .tgl[hidden],.trkCard2 .scope[hidden]{display:none}
.trkCard2 .rng:not(.tgl):not(.scope) .btn{border-radius:0;margin-left:-1px;padding:6px 8px}
.trkCard2 .rng:not(.tgl):not(.scope) .btn:first-child{border-radius:6px 0 0 6px;margin-left:0}
.trkCard2 .rng:not(.tgl):not(.scope) .btn:last-child{border-radius:0 6px 6px 0}
.trkCard2 .tgl,.trkCard2 .scope{display:flex;flex-wrap:wrap;gap:5px;align-items:center}
.trkCard2 .tgl .lbl{display:none}
.trkCard2 .tgl .grp{flex:1 0 100%;font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;color:#7f93a7;margin:4px 0 0}
.trkCard2 .tgl .grp:first-child{margin-top:0}
.trkCard2 .btn.chip{border-radius:999px;display:inline-flex;align-items:center;gap:5px;padding:5px 9px}
.trkCard2 .btn.chip .dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.trkCard2 .btn.chip[aria-pressed="true"]{background:#1e2a36;color:#e8eef5;border-color:#7fc8ff}
.trkCard2 .btn.chip[aria-pressed="false"]{opacity:.45}
.trkCard2 .btn.chip[aria-pressed="false"] .dot{background:transparent!important;border:1px solid currentColor}
.trkCard2 .btn.chip .n{font-size:10px;opacity:.75;font-variant-numeric:tabular-nums}
.trkCard2 .btn.chip .n:empty{display:none}
/* ★ Stuart 15:24 ("what is the red here all about … it looks like an error"): a layer this radio is decoding NOW wears the
 *  small LIVE tag — the TUNE card's own, in the house green, still. No red on anything that is not an error. */
.trkCard2 .btn.chip .lv,.tunePop .tpRow .lv{flex:none;font:800 9px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;color:#08140b;background:#5BE36B;border:1px solid #5BE36B;border-radius:3px;padding:2px 4px;margin-left:6px;max-width:5.5em;overflow:hidden;white-space:nowrap}
.trkCard2 .btn.chip .lv.o{background:transparent;color:#8fd79a;border-color:#4f8f5a}
.trkCard2 .tunePop{position:static;max-height:none;box-shadow:none;border:0;border-radius:0;background:none;padding:4px}
.trkCard2 .tunePop[hidden]{display:block}
.trkCard2 .slot-tune:empty{display:none}
.trkCard2 .slot-tune{display:flex;flex-wrap:wrap;gap:6px;align-items:center;padding:8px 10px;border-bottom:1px solid #151f29}
.trkCard2 .slot-ctl .btn{border-radius:999px;padding:5px 11px;font:700 11px/1 ui-monospace,Menlo,monospace}
.trkCard2 .slot-ctl .btn[aria-pressed="true"],.trkCard2 .slot-ctl .btn.on{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard2 .fitSect .btn.fit.on{background:#ffd34d;color:#111;border-color:#ffd34d}
.trkCard2 .fitSect .btn{border-radius:999px;padding:5px 12px;font:700 11px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em}
.trkCard2 .slot-ctl:empty{display:none}
.trkCard2 .slot-ctl .acKeys{display:flex;flex-wrap:wrap;gap:5px}
.trkCard2 .slot-ctl .acKeys .btn[disabled]{opacity:.45;cursor:not-allowed}
.trkCard2 .fitSel{display:flex;flex-wrap:wrap;gap:5px}
.trkCard2 .note{color:#8596a7;font-size:11px;margin-top:6px}
.trkCard2 .note:empty{display:none}
.trkCard2 .kb{padding:8px 10px;color:#d7dee6;font-size:12px;line-height:1.35}
.trkCard2 .trkKey{position:static;max-height:none;overflow:visible;border:0;border-radius:0;background:none;z-index:auto}
.trkCard2 .trkKey .kr{white-space:normal}
.trkCard2 .kb .kr{display:flex;align-items:center;gap:8px;padding:2px 0}
/* ★★★ ONE ROW STYLE for the cards' toggle rows (Stuart 11:59): the LAYERS rows and the SOURCES rows — a dark row, a coloured
 *  dot, the name (a muted line under it), the count on the right, an ON / OFF tag; a hidden one dimmed with OFF */
.trkCard2 .mRow{all:unset;box-sizing:border-box;display:flex;align-items:center;gap:10px;width:100%;padding:10px 12px;cursor:pointer;
  border-bottom:1px solid #151f29;font:600 13px/1.2 system-ui,sans-serif;color:#e8eef5;text-align:left}
.trkCard2 .mRow:hover,.trkCard2 .mRow:focus-visible{background:#1a2632;outline:none}
.trkCard2 .mRow:focus-visible{box-shadow:inset 3px 0 0 #ffd34d}
.trkCard2 .mRow .dot{width:10px;height:10px;border-radius:50%;flex:none}
.trkCard2 .mRow .nm{display:flex;flex-direction:column;gap:2px;min-width:0;flex:1 1 auto}
.trkCard2 .mRow .nm b{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.trkCard2 .mRow .nm small{font:500 11px/1.25 system-ui,sans-serif;color:#93a3b4}
.trkCard2 .mRow .n{margin-left:auto;font:700 12.5px/1 ui-monospace,Menlo,monospace;color:#ffd34d;white-space:nowrap}
.trkCard2 .mRow::after{content:"ON";flex:none;box-sizing:border-box;width:30px;text-align:center;font:800 9px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;padding:3px 5px;border-radius:4px;background:#1e3a26;color:#7ee787}
.trkCard2 .mRow[aria-pressed="false"]{color:#7f93a7}
.trkCard2 .mRow[aria-pressed="false"] .n{color:#7f93a7}
.trkCard2 .mRow[aria-pressed="false"] .dot{background:transparent!important;border:1px solid currentColor}
.trkCard2 .mRow[aria-pressed="false"]::after{content:"OFF";background:#2a1d1d;color:#ff8a6b}
.trkCard2 .mRow.down .n{color:#ff8a6b}
.trkCard2 .srcSect{padding:0}
.trkCard2 .srcSect>.sh{padding:8px 10px 4px;margin:0}
.trkCard2[data-card="list"] .trkList{position:static;width:auto;max-height:none;border:0;border-radius:0;background:none;flex:1 1 auto;min-height:0}
.trkCard2[data-card="list"]{flex:1 1 auto}
.trkCard2[data-card="list"] .trkList .hd{cursor:default}
.trkTop .slot-share{min-width:0;flex:0 1 auto}
.trkTop .slot-share .mapShLine{min-width:0;overflow:hidden;text-overflow:ellipsis;flex:0 1 auto;text-transform:none;letter-spacing:.02em}
.trkTop .slot-share>span{min-width:0}
/* ★ a phone: the bar may WRAP the shared line (never reorder it) — EXIT, the line, CHAT, the rate */
.trkView[data-narrow] .trkTop{flex-wrap:wrap;row-gap:6px}
.trkView[data-narrow] .trkTop .slot-share>span{flex-wrap:wrap}
.trkView[data-narrow] .trkTop .slot-share .mapShLine{white-space:normal;text-align:left;line-height:1.25}
.trkView[data-narrow] .trkCards{left:6px;right:var(--trk-rail-w,76px);top:6px;bottom:6px;max-width:none}
.trkView[data-narrow] .trkCard2{max-height:58%}
/* ★★★ TWEAK 3+ (Stuart 11-27): the chips are EDGE DOCKS (index.html .edgeDock — the SERVER HEALTH pill's own look and
 *  slide): TUNE and LAYERS down the LEFT edge, MAP CONTROLS top RIGHT, LIST bottom RIGHT (the health pill's side). */
/* ★★★ V12 SAFE AREAS (Stuart 11-27, iPhone landscape): the map runs edge to edge (viewport-fit=cover while a map is up —
 *  TrackView.show) and every control is padded in by the inset on ITS side only (the app's islandSide rule). One set of
 *  vars, so a driver can simulate a notch (scripts/test-web-map-landscape.mjs). */
/* ★ Stuart 19:xx ("tone down the green border, it looks highlighted by a controller"): the RESTING rim of every chip and
 *  card is the chips' own muted green (index.html .edgeDock .hCard); the bright green is focus (:focus-visible / gpFocus)
 *  and a chip's own lit / open state only */
.trkView{--chip-rim:rgba(120,200,120,.45)}
.trkCard2[data-card="help"] .hPad:empty{display:none}
.trkCard2[data-card="help"] .hPad{padding:2px 0 8px;margin-bottom:8px;border-bottom:1px solid #22303d;font-size:12px}
.trkCard2[data-card="help"] .hPadH{font:700 10px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;color:#8fa2b5;margin:2px 0 6px}
.trkCard2[data-card="help"] .hPadR{display:flex;gap:8px;align-items:baseline;padding:2px 0}
.trkCard2[data-card="help"] .hPadR .k{flex:none;min-width:84px;color:var(--hc,#5BE36B);font-weight:700}
/* health / time's tabs: the muted rim like every tab; their colour lives in the ICON (the tab's text colour), as the pill's */
.trkView .edgeDock:not(.crit) .hTab{border-color:var(--chip-rim)!important}
.trkView{--sai-l:env(safe-area-inset-left,0px);--sai-r:env(safe-area-inset-right,0px);--sai-t:env(safe-area-inset-top,0px);--sai-b:env(safe-area-inset-bottom,0px)}
.mChips{position:absolute;z-index:790;display:flex;flex-direction:column;gap:8px;pointer-events:none}
/* ★ tweak 4: SERVERS top-left where the app has it, then TUNE, LAYERS; the zoom ± step over to the right, under MAP */
.mChipsL{left:0;top:8px}
.trkMapHost .maplibregl-ctrl-top-left,.trkMapHost .leaflet-top.leaflet-left{left:auto!important;right:0;top:var(--trk-zoom-top,72px)}
.mChipsR{right:0;top:8px}
.mChipsRB{right:0;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.mDock{position:relative}
.mDock[hidden]{display:none}
.mDock .mchip{all:unset;box-sizing:border-box;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;cursor:pointer;padding:0 4px;color:#dfe7ef;border-radius:8px}
.mDock .mchip:focus-visible{outline:2px solid var(--hc);outline-offset:1px}
.mDock .mchip .cap{font:700 9px/1 system-ui,sans-serif;letter-spacing:.16em;color:#B9C0B4;text-align:center}
.mDock.open .mchip .cap{color:#fff}
/* ★ Stuart 19:xx: OPEN / lit is not the focus green — a gold accent on a warmer ground, the rim staying muted (bright green
 *  rims on the map = focus only: :focus-visible / gpFocus) */
.mDock:not([data-dock="list"]).open .hCard{border-color:var(--chip-rim)!important;background:rgba(255,211,77,.16)!important}
.mDock:not([data-dock="list"]).open .mchip{color:#ffd34d!important}
.mDock .mchip .ic{display:flex;align-items:center;gap:5px;color:var(--hc);font:700 12px/1 ui-monospace,Menlo,monospace}
.mDock .mchip .ic .cn{color:#e8eef5}
.mDock .mchip .ic .cn:empty{display:none}
.mDock .mIco{width:22px;height:22px;display:block}
.mDock .lsum{display:flex;align-items:center;gap:9px}
.mDock .lsum:empty::after{content:"—";color:#7f93a7}
.mDock .le{display:inline-flex;align-items:center;gap:3px;font:700 12px/1 ui-monospace,Menlo,monospace;color:#e8eef5;font-variant-numeric:tabular-nums}
.mDock .lg{display:inline-flex}
.mDock .lg svg{display:block}
/* ★ a phone-width window: the LAYERS chip is a slim column — each glyph over its count (the one-card breakpoint) */
.trkView[data-narrow] .mDock .lsum{flex-direction:column;gap:5px}
.trkView[data-narrow] .mDock .le{flex-direction:column;gap:2px;font-size:11px}
/* ★ the ask shield's look: the TUNE chip fades while this listener must ask first (a press nudges — setDialLock) */
.mDock.askFaded .hCard{opacity:.4;filter:grayscale(.7)}
.trkCard2 .srvPick{all:unset;box-sizing:border-box;display:flex;flex-direction:column;gap:2px;width:100%;padding:8px 10px;cursor:pointer;border-bottom:1px solid #151f29}
.trkCard2 .srvPick b{font:600 13px/1.25 system-ui,sans-serif;color:#e8eef5}
.trkCard2 .srvPick small{font-size:11px;color:#93a3b4}
.trkCard2 .srvPick:hover,.trkCard2 .srvPick:focus-visible{background:#1a2632;outline:none}
.trkView .trkCards{position:absolute;inset:0;left:0;right:0;top:0;bottom:0;max-width:none;display:block;z-index:795;pointer-events:none}   /* ★ an open card above the other chips */
.trkCards .cSide{position:absolute;display:flex;flex-direction:column;gap:8px;width:var(--trk-card-w,340px);pointer-events:none;min-height:0}
.trkCards .cSide>.trkCard2{pointer-events:auto}
.trkCards .sideL{left:var(--trk-chip-l,70px);top:84px;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.trkCards .sideR{right:var(--trk-chip-r,70px);top:8px;bottom:calc(var(--trk-attrib-h,0px) + 8px)}
.trkCards .sideB{right:var(--trk-chip-rb,70px);top:40%;bottom:calc(var(--trk-attrib-h,0px) + 8px);justify-content:flex-end}
.trkCards .cSide>.trkCard2{max-height:100%}
.trkCards .sideB>.trkCard2{flex:0 1 auto}
.trkView[data-narrow] .trkCards .sideL{left:var(--trk-chip-l,64px);right:var(--trk-chip-r,64px);width:auto;top:6px}
.trkView[data-narrow] .trkCards .sideR,.trkView[data-narrow] .trkCards .sideB{left:var(--trk-chip-l,64px);right:var(--trk-chip-r,64px);width:auto}
.trkView[data-narrow] .trkCards .sideB{right:var(--trk-chip-rb,64px);top:30%}
.trkView[data-narrow] .trkCard2{max-height:100%}
/* ★★★ TWEAK 4 (Stuart 11-27): a press on a chip EXPANDS it in place into its card (flush to the same edge, the dock's
 *  rim); its header shrinks it back. The tuck › stays the separate, deliberate action — smaller, set off by a rule. */
.mDock.open{visibility:hidden}
/* ★ the AIR & SEA list's own tags (acTags.ts) — the live ADS-B list's box */
.trkRow .id b .tag{display:inline-block;margin-left:6px;padding:0 5px;border-radius:3px;font:700 8.5px/14px system-ui,sans-serif;letter-spacing:.06em;vertical-align:1px;flex:none}
.trkRow .tag.em{background:#e04040;color:#fff}
.trkRow .id b:has(.nmT){display:flex;align-items:center;min-width:0}
.trkRow .id b .nmT{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.trkRow .tag.mil{background:rgba(90,170,255,.85);color:#06121f}
/* ★★★ Stuart 12:15 / 12:18: every card OPAQUE (nothing reads through); the LIST a true drawer — flush to the left and the
 *  bottom, no rim on the edge sides, a square corner (the window's own corner clips it), the safe area inside it; the
 *  controller's green focus ring drawn INSIDE (no clipping at the edge, no layout shift) */
.trkCards .trkCard2{background:#0b110d!important}
/* ★ Stuart 13:04 ("remove the green border"): the drawer's rim is the cards' quiet grey; green is only ever the pad's focus */
.trkCards .sideB>.trkCard2[data-card="list"]{border-color:#2c3a4a!important}
.trkCard2[data-card="list"] .lMax{color:#2c3a4a!important}
.trkCard2[data-card="list"] .lMax svg{color:var(--hc,#5BE36B)}
/* ★ …and the chip over the last row: the row fades out under it (a gradient off the chip's plate, no layout) */
.mDock[data-dock="list"].open .hCard{position:relative}

.mDock[data-dock="list"].open .hCard{box-shadow:none!important;border-color:transparent!important}
/* a row's long title is a mouse's tooltip, never a touch's */
@media (hover:none){.trkRow{-webkit-touch-callout:none}}
.mHelp,.mChipsScale{transition:transform .2s ease}
.trkCard2[data-card="tune"] .mapGainHost{display:flex;flex-direction:column;gap:4px;padding:4px 10px 8px;border-bottom:1px solid #1b2733}
.trkCard2[data-card="tune"] .mapGainHost:empty{display:none}
.trkCard2[data-card="tune"] .mapGainHost .mrow{margin:0}
.mapGainHost .gRow{display:flex;align-items:center;gap:8px;min-height:34px}
.mapGainHost .gRow[hidden]{display:none}
.mapGainHost .gSl{flex:1 1 auto;min-width:80px;accent-color:#ffd34d;-webkit-appearance:none;appearance:none;background:transparent;height:24px;margin:0;cursor:pointer}
/* ★ the house slider, never the browser's grey ("the sources look like windows 95", Stuart 11:59): a dark rail, the gold thumb */
.mapGainHost .gSl::-webkit-slider-runnable-track{height:4px;border-radius:2px;background:#33475a}
.mapGainHost .gSl::-moz-range-track{height:4px;border-radius:2px;background:#33475a}
.mapGainHost .gSl::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:16px;height:16px;margin-top:-6px;border-radius:50%;background:#ffd34d;border:2px solid #0b110d;box-shadow:0 0 0 1px #ffd34d}
.mapGainHost .gSl::-moz-range-thumb{width:14px;height:14px;border-radius:50%;background:#ffd34d;border:2px solid #0b110d}
.mapGainHost .gSl:focus-visible{outline:2px solid #ffd34d;outline-offset:2px}
.mapGainHost .gVal{font:700 13px/1 ui-monospace,"SF Mono",Menlo,Consolas,monospace;color:#e8eef5;font-variant-numeric:tabular-nums;white-space:nowrap;flex:none}
.mapGainHost .gAgc{all:unset;cursor:pointer;flex:none;font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.08em;padding:6px 9px;border-radius:7px;border:1px solid #33475a;color:#9fb0c2;background:#141c24}
.mapGainHost .gAgc.on{background:#ffd34d;border-color:#ffd34d;color:#111}
.mapGainHost .gLock,.mapGainHost .gMoreB{all:unset;cursor:pointer;flex:none;font-size:15px;line-height:1;padding:4px 6px;border-radius:6px;color:#cfd9e3}
.mapGainHost .gMoreB{margin-left:auto;font-size:16px;color:#ffd34d}
.mapGainHost .gMoreB[aria-expanded="true"]{transform:rotate(180deg)}
.mapGainHost .gAgc:focus-visible,.mapGainHost .gLock:focus-visible,.mapGainHost .gMoreB:focus-visible{outline:2px solid #ffd34d;outline-offset:1px}
.mapGainHost .gPw{display:flex;gap:6px;align-items:center;padding:4px 0}
.mapGainHost .gPw[hidden]{display:none}
.mapGainHost .gPwIn{flex:1 1 auto;min-width:0;background:#0f1720;color:#e8eef5;border:1px solid #33475a;border-radius:6px;padding:6px 8px;font:13px ui-monospace,Menlo,monospace}
.mapGainHost .gPwGo{all:unset;cursor:pointer;padding:6px 9px;border-radius:6px;background:#ffd34d;color:#111;font:700 10.5px/1 ui-monospace,Menlo,monospace}
.mapGainHost .gPwMsg{font-size:11px;color:#ff8a7d}
.mapGainHost .gPw.shake{animation:gShake .35s ease 1}
@keyframes gShake{0%,100%{transform:translateX(0)}25%{transform:translateX(-6px)}75%{transform:translateX(6px)}}
body.askLocked .mapGainHost .gRow{opacity:.45}
/* ★★ the ASK PULSE (main.ts mapAskPulse): a breath and a radar ring, three times, transform + opacity only */
.trkView .askPulse{position:relative;overflow:visible!important;animation:trkAskBreathe .8s ease-in-out 3}
.trkView .askPulse::after{content:"";position:absolute;inset:-2px;border-radius:inherit;border:2px solid #ffcc88;pointer-events:none;animation:trkAskRing .8s ease-out 3;opacity:0;z-index:-1}
.trkView .askPulse{z-index:0}
.trkView .trkTop{z-index:789!important}   /* ★ the bar and its ring under the edge chips (790): a ring never sits over a chip's words */
@keyframes trkAskBreathe{0%,100%{opacity:1}50%{opacity:.72}}
@keyframes trkAskRing{0%{transform:scale(1);opacity:.9}100%{transform:scale(1.6);opacity:0}}
@media (prefers-reduced-motion:reduce){.trkView .askPulse{animation:trkAskFlash .6s ease-out 1}.trkView .askPulse::after{animation:none}}
@keyframes trkAskFlash{0%{opacity:.5}100%{opacity:1}}
/* ★★ the radio page's SERVER HEALTH chip, lent to the map's right column (main.ts mapChromeAdopt) — in the flow, not fixed */
.trkView .mChipsR #srvHealth{position:relative!important;top:auto!important;right:auto!important;left:auto!important;bottom:auto!important;transform:none!important;pointer-events:auto;align-self:flex-end}
.trkView .mChipsR #rxBadge{position:relative!important;top:auto!important;right:auto!important;align-self:flex-end;padding-right:12px;margin-top:-4px}
/* ★ Stuart 15:09: TIME is a slim tab on the edge ("Guaranteed time 6:53", vertical); a tap pops a small card — the time,
 *  and what the limit means — that folds itself after 9 s or on a second tap */
/* ★ the TIME EdgeChip: SessionClock's caption and number (Atkinson where the page has it), the frame in the stage's colour */
.trkView .mChipsR .mTime{position:relative;align-self:flex-end;pointer-events:auto;--tc:rgba(255,160,0,.45);--tn:#ffb833;--tcap:rgba(255,160,0,.65)}
.trkView .mChipsR .mTime[data-stage="1"]{--tc:rgba(255,225,77,.7);--tn:#ffe14d;--tcap:rgba(255,225,77,.9)}
.trkView .mChipsR .mTime[data-stage="2"]{--tc:rgba(255,90,90,.75);--tn:#ff6b6b;--tcap:rgba(255,140,140,.95)}
.mTime .hCard{border-color:var(--tc)!important}
.trkView .mChipsR>.edgeDock:not(.mDock) .hCard{border-right-width:1.5px!important;margin-right:-1.5px}
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock) .hCard{margin-right:0}
.mTime .hTab{color:var(--tn);border-color:var(--tc)!important}
.mTime .tClock{display:flex;flex-direction:column;align-items:center;gap:2px;padding:2px 6px 2px 2px}
.mTime .tCap{font:700 9px/1.2 'Atkinson Hyperlegible',system-ui,sans-serif;letter-spacing:.08em;color:var(--tcap)}
.mTime .tNum{font:700 20px/1.1 'Atkinson Hyperlegible',ui-monospace,Menlo,monospace;color:var(--tn);font-variant-numeric:tabular-nums}
.mTime .tNum.soft{font-size:11px;line-height:15px;max-width:150px;text-align:center;font-weight:400}
.mTime .tIcon{display:inline-flex}
.mTime.warn .tl{color:#ffd479}.mTime.crit .tl{color:#ff8a7d}
.trkCard2[data-card="stats"] .stTabs{display:flex;gap:4px;padding:6px 8px 2px;flex-wrap:wrap}
.trkCard2[data-card="stats"] .stTabs[hidden]{display:none}
.trkCard2[data-card="stats"] .stTab{all:unset;cursor:pointer;font:700 10.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em;color:#8fa2b5;padding:5px 8px;border-radius:999px;border:1px solid #2c3a4a}
.trkCard2[data-card="stats"] .stTab[aria-selected="true"]{color:#111;background:#ffd34d;border-color:#ffd34d}
.trkCard2[data-card="stats"] .stTab:focus-visible,.trkCard2[data-card="stats"] .stRow:focus-visible{outline:2px solid #ffd34d;outline-offset:-2px}
.trkCard2[data-card="stats"] .stBody{display:flex;flex-direction:column;padding:4px 6px 8px}
.trkCard2[data-card="stats"] .stRow{all:unset;cursor:pointer;display:grid;grid-template-columns:auto auto 1fr;gap:8px;align-items:baseline;padding:8px 8px;border-radius:8px;border:1px solid transparent}
.trkCard2[data-card="stats"] .stRow:hover{background:#15202b;border-color:#2c3a4a}
.trkCard2[data-card="stats"] .stRow .k{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.08em;color:#8fa2b5}
.trkCard2[data-card="stats"] .stRow .v{font:700 12px/1 ui-monospace,Menlo,monospace;color:#ffd34d;white-space:nowrap}
.trkCard2[data-card="stats"] .stRow .n{font:600 12px/1.2 system-ui,sans-serif;color:#e8eef5;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.trkList .hd .srt .srtT{display:inline-flex;align-items:center;gap:4px;border-radius:6px!important;margin:0!important;padding:3px 6px!important;background:#1e2a36;color:#ffd34d;border:1px solid #33475a}
.trkList .hd .srt .srtT .sIco{width:14px;height:14px}
.trkList .hd .srt .srtT .sw{font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.06em}
.trkView[data-narrow] .trkList .hd .srt .srtT .sw{display:none}
/* ★ Stuart 12:58: the "heard but not on the map" sentence gone from the LIST on every width (those rows say "no position") */
.trkCard2[data-card="list"]>.cb>.facts{display:none!important}
/* the callsign and its tags never truncate: the subtitle gives way first */
.trkRow .id b .nmT{flex:0 0 auto!important;overflow:visible!important}
/* ★★★ Stuart 13:00 ("on the larger wider screen the list is super skinny"): the drawer's width from the viewport —
 *  30 % of it, 340 to 640 px; a phone's is the full width */
.trkView:not([data-narrow]) .trkCards .sideB{width:clamp(420px,30vw,640px)!important;max-width:calc(100% - 12px)}
.trkCards .sideB>.trkCard2[data-card="list"]{width:100%!important;max-width:none!important}
.trkView[data-narrow] .trkCards .sideB{right:0!important;width:auto!important}
.trkView.stackAway .mHelp,.trkView.stackAway .mChipsScale{visibility:hidden}
.trkView.scaleAway .mChipsScale{visibility:hidden}
/* ★★★ Stuart 12:50–12:56 NO TOP BAR — the map runs to the top. The bar's pieces float over it: the big round X top-left
 *  (red-tinted, never an edge tab), the shared-tuner pill top-centre (tap = chat), RATE then CHAT down the right edge, the
 *  zoom below them. The old bar element stays for the drivers (its status line is off-screen), out of the flow. */
/* ★ Stuart 14:12: nothing under a full-screen map keeps the page composing — the radio page's breathing dots, marquees
 *  and pulses pause while a map is up (body.trkMapUp, set by show / hide) */
body.trkMapUp > :not(.trkView) *,body.trkMapUp > :not(.trkView){animation-play-state:paused!important}
.trkView .trkTop{position:absolute!important;left:0;right:0;top:0;z-index:801;background:none!important;border:0!important;box-shadow:none!important;min-height:0!important;
  padding:calc(var(--sai-t) + 10px) var(--trk-pill-pad,72px) 0!important;display:flex!important;flex-wrap:wrap!important;justify-content:center;align-items:flex-start;gap:6px;pointer-events:none}
.trkView .trkTop>*{pointer-events:auto}
.trkView .trkTop .ttl,.trkView .trkTop .rate,.trkView .trkTop .spacer{display:none!important}
.trkView .trkTop .st{position:absolute!important;width:1px!important;height:1px!important;overflow:hidden!important;clip:rect(0 0 0 0)!important;flex:none!important}
.trkView .trkTop .exit{position:fixed;left:0;top:var(--sai-t);width:calc(52px + var(--sai-l));height:52px;border-radius:0 0 14px 0!important;padding:0 0 0 var(--sai-l)!important;box-sizing:border-box;
  font-size:0!important;color:#fff;background:#5e1818!important;border:1px solid #d0554e!important;border-top-width:0!important;border-left-width:0!important;box-shadow:2px 2px 10px rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:2}
.trkView .trkTop .exit::before{content:"";width:18px;height:18px;background:linear-gradient(45deg,transparent 43%,#fff 43%,#fff 57%,transparent 57%),linear-gradient(-45deg,transparent 43%,#fff 43%,#fff 57%,transparent 57%)}
.trkView .trkTop .exit:hover,.trkView .trkTop .exit:focus-visible{background:rgba(160,36,36,.96)!important;outline:2px solid #ffd34d;outline-offset:2px}
.trkView .mChipsL{top:calc(var(--sai-t) + 52px + 12px)}
.trkView .mChipsR{top:var(--sai-t);align-items:flex-end}   /* ★ each chip its own width — CHAT is not as wide as RATE */
.trkView .mChipsR>.mDock{align-self:flex-end}
.mDock[data-dock="rate"] .hCard{border-top-width:0!important;border-right-width:0!important;border-radius:0 0 0 12px!important;padding-right:var(--sai-r)}
.trkView.rateOpen .trkCards .sideR{right:0!important;left:auto!important;top:var(--trk-rate-bottom,40px)!important}
.trkView.rateOpen .trkCards .sideR>.trkCard2[data-card="rate"]{margin:-1.5px -1.5px 0 0;border-radius:0 0 0 12px!important;width:max(var(--trk-rate-w,160px),240px);transform-origin:top right}
/* ★ no percentage max-width on the pill's boxes: in this shrink-to-fit row it resolved against the pill's own width and held
 *  it at the width of an EARLIER text — "ASK IN CHAT" overflowed a box with room to spare (map-cards e2e) */
/* ★★★ Stuart 14:42: the shared-tuner message MIRRORS THE CONTROL DECK — attached to the top edge, centred, the deck
 *  upside down (square top, rounded bottom), the deck's own material (--ctl-card-bg / --bar-border, so every skin and
 *  the transparency setting follow) and its font; one line, shortened (main.ts) before it could reach the ✕ or RATE */
.trkView .trkTop{padding-top:0!important}
.trkView .trkTop .slot-share .mapShLine{margin-top:0;border-radius:0 0 12px 12px!important;border:1px solid var(--bar-border,rgba(255,255,255,.18))!important;border-top:0!important;
  padding:calc(var(--sai-t) + 7px) 14px 7px!important;box-shadow:0 8px 28px rgba(0,0,0,.6)!important;background:var(--ctl-card-bg,rgba(10,10,10,.92))!important;
  font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace!important;font-size:12px!important;letter-spacing:.12em!important;color:#ffcc88!important;
  white-space:nowrap!important;overflow:hidden;text-overflow:ellipsis;text-align:center;line-height:1.2}
.trkView .trkTop .slot-share .mapShLine.alone{color:#7bd88f!important}
.trkView .trkTop .slot-share .mapShLine.locked{color:#ff9d8a!important}
.trkView[data-narrow] .trkTop .slot-share .mapShLine{white-space:nowrap!important;font-size:11px!important;letter-spacing:.06em!important}
/* the RATE chip: a reading; a ⌄ only with several sources */
.mDock[data-dock="rate"] .rateIc{flex-direction:row!important;align-items:center;gap:4px;font:700 11px/1 ui-monospace,Menlo,monospace;color:#dfe7ef;white-space:nowrap}
.mDock[data-dock="rate"] .ru{color:#ffd34d}
.mDock[data-dock="rate"] .rmore{color:#9fb0c2;font-size:10px}
.mDock[data-dock="rate"] .rmore:empty{display:none}
.mDock[data-dock="rate"] .arw{width:10px;height:10px;flex:none;transition:transform .2s ease}
.mDock[data-dock="rate"].static .arw{display:none}
.mDock[data-dock="rate"].static .mchip{cursor:default}
.mDock[data-dock="rate"].open .arw{transform:rotate(180deg)}
.mDock[data-dock="rate"] .cap{display:none}
.trkView[data-narrow] .mDock[data-dock="rate"] .rn{display:none}
.mDock[data-dock="rate"] .mchip{flex-direction:row!important;min-height:34px!important;padding:0 8px!important}
.mDock[data-dock="rate"].open{visibility:visible}
.mDock[data-dock="rate"].open .hCard{border-radius:0!important}
.trkCard2[data-card="rate"] .rateRow{display:grid;grid-template-columns:10px 1fr auto;align-items:center;gap:8px;padding:7px 10px;font:600 12px/1.2 system-ui,sans-serif}
.trkCard2[data-card="rate"] .rateRow .dot{width:8px;height:8px;border-radius:50%;background:#5BE36B}
.trkCard2[data-card="rate"] .rateRow.down .dot{background:#e0a040}
.trkCard2[data-card="rate"] .rateRow:not(.mine) .dot{background:transparent;border:1.5px solid #5BE36B;box-sizing:border-box}
.trkCard2[data-card="rate"] .rateRow.down:not(.mine) .dot{border-color:#e0a040}
.trkCard2[data-card="rate"] .rateRow .v{font:700 12px/1 ui-monospace,Menlo,monospace;color:#ffd34d;font-variant-numeric:tabular-nums}
.trkCard2[data-card="rate"] .rateRow.down .v{color:#e0a040}
/* the CHAT chip: a glyph and the unread badge; a single gentle pulse on a new message */
.mDock[data-dock="chat"] .ic{position:relative}
.mDock[data-dock="chat"] .unr{position:absolute;top:-6px;right:-9px;min-width:15px;height:15px;border-radius:8px;background:#e04040;color:#fff;font:700 9.5px/15px system-ui,sans-serif;text-align:center;padding:0 3px;box-sizing:border-box}
.mDock[data-dock="chat"] .unr[hidden]{display:none}
.mDock[data-dock="chat"].off{opacity:.45}
.mDock[data-dock="chat"].pulse .hCard{animation:trkChatPulse 1.2s ease-out 1}
@keyframes trkChatPulse{0%{transform:scale(1)}25%{transform:scale(1.12)}100%{transform:scale(1)}}
@media (prefers-reduced-motion:reduce){.mDock[data-dock="chat"].pulse .hCard{animation:none}}
.trkCard2[data-card="chat"] .chatBody{display:flex;flex-direction:column;gap:6px;padding:6px 8px 8px;min-width:0}
.trkCard2[data-card="chat"] .chatBody #chatLog{max-height:9.5em;overflow:auto;font-size:12px}
.trkView[data-narrow] .trkCard2[data-card="chat"] .chatBody #chatLog{max-height:6.5em}
.trkCard2[data-card="chat"] .chatBody #chatPhrases{display:flex;flex-wrap:wrap;gap:5px}
/* ★★★ Stuart 12:44 ("empty space where it says LIST then a back button, why doesn't that go"): the drawer has NO title row —
 *  it opens straight onto its tabs and rows. Its CHIP is the toggle, always in the corner: the list glyph, the count and an
 *  arrow — up (open it) while shut, down (fold it) while open. The chip sits in the drawer's own bottom strip, so no row
 *  ever hides under it. */
.trkCard2[data-card="list"]>.ch{display:none!important}
.trkCards .sideB>.trkCard2[data-card="list"]{padding-bottom:var(--sai-b)!important}
/* ★ Stuart 12:55 ("the collapse button can simply cut in over the data in that corner"): no strip — the rows run to the
 *  drawer's bottom; the chip floats over the corner on its own opaque plate; a spacer after the last row lets it scroll clear */
.trkCard2[data-card="list"] .rows::after{content:none}
/* ★★★ Stuart 13:20 ("the row at the top … moves next to the collapse button at the bottom, no covering of the lines at
 *  all"): ONE slim bar at the drawer's foot — the chip (in the corner, as ever), the tab scroller, ‖ +N, the sort — and
 *  the rows above it from the drawer's top edge; nothing ever covers a row */
.trkCard2[data-card="list"] .trkList .hd{order:9;min-height:var(--trk-lchip-h,31px);box-sizing:border-box;padding:0 8px 0 calc(var(--trk-lchip-w,76px) + 4px - var(--sai-l,0px))!important;border-top:1px solid #1b2733;border-bottom:0!important;flex:none}
.trkCard2[data-card="list"] .trkList .rows{order:1}
.trkCard2[data-card="list"] .trkList .safety,.trkCard2[data-card="list"] .trkList .filt{order:2}
.trkView .mChipsBL{z-index:800}
.mDock[data-dock="list"].open{visibility:visible}
.mDock[data-dock="list"].open .hCard{background:#0b110d!important}
.mDock[data-dock="list"] .arw{width:11px;height:11px;flex:none;margin-left:2px;opacity:.85;transition:transform .2s ease}
.mDock[data-dock="list"].open .arw{transform:rotate(180deg)}
/* the LIST chip on ONE line at every width — glyph, count, arrow — a slim strip under the drawer's rows */
.mChipsBL .mDock[data-dock="list"] .mchip{flex-direction:row!important;min-height:30px!important;padding-top:3px!important;padding-bottom:3px!important}
.mChipsBL .mDock[data-dock="list"] .hCard{padding-top:0!important;padding-bottom:0!important;min-height:0!important}
/* ★★★ Stuart 12:46 / 12:48: a small TAB grows out of the drawer's top edge near its right corner — the drawer's own
 *  background and rim, rounded top corners, concave fillets where it meets the edge (a folder tab) — ⌃ maximise, ⌄ restore */
.trkCards .trkCard2[data-card="list"]{overflow:visible!important}
.trkCards .trkCard2[data-card="list"]>.cb{border-radius:inherit}
.trkCard2[data-card="list"] .lMax{all:unset;box-sizing:border-box;position:absolute;top:-19px;right:22px;width:44px;height:19px;display:flex;align-items:flex-start;justify-content:center;padding-top:3px;
  background:#0b110d;border:1px solid;border-bottom:0;border-color:inherit;border-radius:9px 9px 0 0;color:var(--hc,#5BE36B);cursor:pointer;z-index:2}
.trkCard2[data-card="list"] .lMax svg{width:12px;height:12px;transition:transform .2s ease}
.trkCard2[data-card="list"] .lMax[aria-pressed="true"] svg{transform:rotate(180deg)}
.trkCard2[data-card="list"] .lMax::before,.trkCard2[data-card="list"] .lMax::after{content:"";position:absolute;bottom:-1px;width:9px;height:9px;pointer-events:none}
.trkCard2[data-card="list"] .lMax::before{left:-9px;background:radial-gradient(circle at 0 0,transparent 7.5px,currentColor 8px,currentColor 8.6px,#0b110d 9.2px)}
.trkCard2[data-card="list"] .lMax::after{right:-9px;background:radial-gradient(circle at 100% 0,transparent 7.5px,currentColor 8px,currentColor 8.6px,#0b110d 9.2px)}
.trkCards .trkCard2[data-card="list"]{position:relative}
.trkCard2[data-card="list"] .lMax:focus-visible{outline:2px solid #ffd34d;outline-offset:-3px}
.trkCards .trkCard2[data-card="list"][hidden] .lMax,.trkCards .trkCard2[data-card="list"].stepAside .lMax{display:none}
.trkView.lMax .trkCards .sideB{top:var(--trk-cb-top,0px)!important}
/* ★★★ V12 the LIST ↔ WATERFALL tab: the maximise tab's mirror at the drawer's top-LEFT (same size, shape, material) */
.trkCard2[data-card="list"] .lSwap{all:unset;box-sizing:border-box;position:absolute;top:-19px;left:22px;width:44px;height:19px;display:flex;align-items:flex-start;justify-content:center;padding-top:3px;
  background:#0b110d;border:1px solid #2c3a4a;border-bottom:0;border-radius:9px 9px 0 0;color:#2c3a4a;cursor:pointer;z-index:2}
.trkCard2[data-card="list"] .lSwap[hidden]{display:none}
.trkCard2[data-card="list"] .lSwap svg{width:13px;height:13px;color:var(--hc,#5BE36B)}
.trkCard2[data-card="list"] .lSwap::before,.trkCard2[data-card="list"] .lSwap::after{content:"";position:absolute;bottom:-1px;width:9px;height:9px;pointer-events:none}
.trkCard2[data-card="list"] .lSwap::before{left:-9px;background:radial-gradient(circle at 0 0,transparent 7.5px,currentColor 8px,currentColor 8.6px,#0b110d 9.2px)}
.trkCard2[data-card="list"] .lSwap::after{right:-9px;background:radial-gradient(circle at 100% 0,transparent 7.5px,currentColor 8px,currentColor 8.6px,#0b110d 9.2px)}
.trkCard2[data-card="list"] .lSwap:focus-visible{outline:2px solid #ffd34d;outline-offset:-3px}
.trkCards .trkCard2[data-card="list"][hidden] .lSwap,.trkCards .trkCard2[data-card="list"].stepAside .lSwap{display:none}
/* the collapsed chip's own ≋ tab (opens the drawer on the waterfall); room above the chip for it */
.mDock[data-dock="list"] .hCard{position:relative}
.mDock[data-dock="list"] .lSwapC{all:unset;box-sizing:border-box;display:flex;align-items:center;justify-content:center;align-self:stretch;flex:none;
  padding:0 8px 0 9px;margin-left:2px;border-left:1px solid rgba(255,255,255,.16);color:var(--hc,#5BE36B);cursor:pointer}
.mDock[data-dock="list"] .lSwapC svg{width:17px;height:17px}
.mDock[data-dock="list"] .lSwapC[hidden],.mDock[data-dock="list"].open .lSwapC,.mDock[data-dock="list"].tucked .lSwapC{display:none}
.mDock[data-dock="list"] .lSwapC:focus-visible{outline:2px solid #ffd34d;outline-offset:-3px}
/* the waterfall's view: the list's rows and its bar's tools give way to the picture and the dial in words */
.trkList .wfPane{display:none}
.trkList[data-pane="wf"] .wfPane{display:flex;flex-direction:column;order:1;flex:1 1 auto;min-height:0;height:0;position:relative;overflow:hidden;contain:size layout paint}
.trkList[data-pane="wf"] .rows,.trkList[data-pane="wf"] .safety,.trkList[data-pane="wf"] .filt{display:none!important}
.trkList[data-pane="wf"] .hd>:not(.wfInfo){display:none!important}
.trkCard2[data-card="list"][data-pane="wf"] .cb>.facts{display:none}
.trkList .hd .wfInfo{display:none}
.trkList[data-pane="wf"] .hd .wfInfo{display:block;color:#cfd9e3;font:700 11px/1.2 ui-monospace,Menlo,monospace;letter-spacing:.04em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.trkCard2[data-card="help"] .hCredit{margin-top:8px;padding-top:6px;border-top:1px solid #22303d;font:10px/1.4 ui-monospace,Menlo,monospace;color:#8fa2b5}
.trkCard2[data-card="help"] .hCredit a{color:#cfd9e3}
.mapWf{position:relative;flex:1 1 auto;min-height:0;height:0;background:#000;overflow:hidden;contain:size layout paint;pointer-events:none}
.mapWf canvas{position:absolute;inset:0;width:100%;height:100%;display:block}
.mapWf .mwMarks{position:absolute;inset:0}
.mapWf .mwLine{position:absolute;top:0;height:var(--mw-spec,30%);width:1px;margin-left:-.5px;background:rgba(255,211,77,.22)}
.mapWf .mwLine.on{background:rgba(255,160,40,.4)}   /* ★ Stuart 16:25: faint — a burst on its channel stays readable */
.mapWf .mwBand{position:absolute;top:0;height:var(--mw-spec,30%);background:rgba(255,160,40,.13)}
.mapWf .mwLbl{position:absolute;transform:translateX(-50%);padding:0 3px;border-radius:3px;background:rgba(0,0,0,.62);color:#cfd9e3;font:600 9.5px/12px ui-monospace,Menlo,monospace;white-space:nowrap}
.mapWf .mwLbl.on{color:#ffb347}
.mChipsBL .mDock[data-dock="list"] .mchip .ic{display:inline-flex!important;flex-direction:row!important;align-items:center;gap:6px}
.trkView.listOverAttrib .maplibregl-ctrl-attrib,.trkView.listOverAttrib .leaflet-control-attribution{visibility:hidden}
/* ★ Stuart 12:34 ("no gap on mac but there is on iphone"): an old phone rule inset the cards' layer 6 px — the layer is the
 *  whole map on every width; only the drawer's CONTENT takes the safe area */
.trkView[data-narrow] .trkCards{left:0!important;right:0!important;top:0!important;bottom:0!important}
.trkCards .sideB>.trkCard2[data-card="list"]{border-radius:0 12px 0 0!important;
  padding-left:var(--sai-l);padding-bottom:var(--sai-b);box-sizing:border-box}
.trkCards .trkCard2.gpFocus,.trkCards .trkCard2 .gpFocus{outline-offset:-3px!important}
/* ★★★★ SHORT LANDSCAPE (mapCards shortMap: the map under 480 px and wider than tall) — STUART'S SAFE RECTANGLE (11-27):
 *  "the map takes the full width, but we build the chip system around the safe zones, so the ✕ joins the top row".
 *  The MAP runs edge to edge under the insets (fitViewport); ALL chrome sits inside the safe rectangle — x from the left
 *  inset to the width less the right inset, above the bottom inset. ONE top row hanging from the top edge (no inset there),
 *  native boxes, one height (--trk-tab-h) and one width (--trk-tab-w, both measured by shortGeom), 8 px gaps:
 *  ✕ (the first tab, red) · SERVERS · TUNE · LAYERS · STATS · MAP … CHAT · HEALTH · TIME · RATE (the last; wider for
 *  its words). LAYERS keeps its full counts (its own width). Every card drops from under the row. The LIST / waterfall
 *  panel from the safe left under the row, its swap / maximise tabs on its right edge; the shared-tuner pill and the
 *  bottom-right group (credits, ?, zoom − +, a horizontal scale — shortGeom) above the bottom inset. Insets 0: full
 *  width. Portrait is untouched: every rule here is [data-short]. */
.trkView[data-short]{--trk-gap:8px}
.trkView[data-short] .trkTop .exit{left:var(--sai-l)!important;top:0!important;width:var(--trk-tab-w,44px)!important;height:var(--trk-tab-h,48px)!important;padding:0!important;
  border:1px solid #d0554e!important;border-top-width:0!important;border-radius:0 0 12px 12px!important;box-shadow:none!important}
.trkView[data-short] .mChipsL{top:0!important;bottom:auto!important;left:calc(var(--sai-l) + var(--trk-tab-w,44px) + var(--trk-gap))!important;flex-direction:row!important;align-items:flex-start!important;gap:var(--trk-gap)!important}
.trkView[data-short] .mChipsR{top:0!important;right:var(--sai-r)!important;flex-direction:row-reverse!important;align-items:flex-start!important;gap:var(--trk-gap)!important}
.trkView[data-short] .mChipsR>[data-dock="rate"]{order:0}
.trkView[data-short] .mChipsR>.mTime{order:1}
.trkView[data-short] .mChipsR>#srvHealth{order:2}
.trkView[data-short] .mChipsR>[data-dock="chat"]{order:3}
/* the tabs: hanging from the top edge — flat on it, rounded below */
.trkView[data-short] .mChipsL .mDock .hCard,.trkView[data-short] .mChipsR .mDock:not([data-dock="rate"]) .hCard{width:var(--trk-tab-w,44px);height:var(--trk-tab-h,48px);min-width:0!important;box-sizing:border-box;
  justify-content:center;align-items:center;border-width:0 1px 1px 1px!important;border-radius:0 0 12px 12px!important;padding:4px!important}
.trkView[data-short] .mChipsL .mDock[data-dock="layers"] .hCard{width:auto;min-width:var(--trk-tab-w,44px)!important}
.trkView[data-short] .mChipsL .mDock .mchip,.trkView[data-short] .mChipsR .mDock:not([data-dock="rate"]) .mchip{flex-direction:column!important;align-items:center;min-height:40px!important}
.trkView[data-short] .mChipsL .mDock .mchip .cap,.trkView[data-short] .mChipsR .mDock:not([data-dock="rate"]) .mchip .cap{display:none}
.trkView[data-short] .mChipsL .mDock .lsum{flex-direction:row!important;flex-wrap:nowrap!important;white-space:nowrap}
.trkView[data-short].famCompact .mChipsL .mDock .lsum .ln,.trkView[data-short].famCompact .mChipsL .mDock .mchip .cn{display:none!important}
.trkView[data-short] .mDock[data-dock="rate"] .hCard{height:var(--trk-tab-h,48px)!important;box-sizing:border-box;border:1px solid var(--bar-border,#2c3a4a)!important;border-top-width:0!important;
  border-radius:0 0 12px 12px!important;padding:0 8px!important;align-items:center}
/* health and time: their tab (tucked) or their card (out), both hanging from the top; the arrows turned with the edge */
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock){position:relative!important;top:auto!important;right:auto!important;margin:0!important;align-self:flex-start!important;transform:none!important;
  width:var(--trk-tab-w,44px);height:var(--trk-tab-h,48px);flex:none}
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock) .hTab{top:0!important;right:0;left:auto;margin:0!important;width:var(--trk-tab-w,44px)!important;height:var(--trk-tab-h,48px)!important;padding:0 0 4px!important;
  flex-direction:column;align-items:center!important;justify-content:center!important;gap:2px;border:1.5px solid var(--chip-rim)!important;border-top-width:0!important;border-radius:0 0 12px 12px!important;transform:none!important}
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock) .hTab>*{flex:none;margin:0!important}
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock) .hCard{position:absolute;top:calc(var(--trk-tab-h,48px) + 4px);right:0;border-width:1.5px!important;border-radius:12px!important;
  transform:none!important;padding:4px 8px!important;z-index:3;white-space:nowrap}
/* ★ Stuart 19:xx (iPhone: the TIME card opened sideways IN the row, HEALTH's tab on top of its digits, CHAT hidden under it):
 *  out, the card DROPS below its own tab like every card here; the tab stays in the row at its width, lit, and tucks it */
.trkView[data-short] .mChipsR>.edgeDock:not(.mDock):not(.tucked) .hTab{visibility:visible!important;pointer-events:auto!important;background:var(--bar-bg-hi,#1d2a38)!important}
/* the cards drop from under the row */
.trkView[data-short] .trkCards .sideL{left:var(--trk-cl-left,96px)!important;right:auto!important;top:calc(var(--trk-tab-h,48px) + 4px)!important;bottom:calc(var(--sai-b) + 8px)!important;
  max-height:none;justify-content:flex-start;width:min(320px,calc(100% - var(--trk-cl-left,96px) - 8px))!important}
.trkView[data-short] .trkCards .cSide>.trkCard2{margin:0!important;border-radius:12px!important}
.trkView[data-short] .trkCards .sideL>.trkCard2{transform-origin:top left!important}
.trkView[data-short] .trkCards .sideR{top:calc(var(--trk-tab-h,48px) + 4px)!important;right:calc(var(--sai-r) + 8px)!important;bottom:calc(var(--sai-b) + 8px)!important;justify-content:flex-start}
.trkView[data-short] .trkCards .sideR>.trkCard2{transform-origin:top right!important}
/* the LIST / waterfall panel and its chip: from the SAFE left, under the row, above the bottom inset */
.trkView[data-short] .trkCards .sideB{left:var(--sai-l)!important;right:auto!important;top:calc(var(--trk-tab-h,48px) + var(--trk-gap))!important;bottom:var(--sai-b)!important;width:clamp(300px,34vw,360px)!important;max-width:none!important}
.trkView[data-short].lMax .trkCards .sideB{width:calc(100% - var(--sai-l) - var(--sai-r) - 16px)!important}
.trkView[data-short] .trkCards .sideB>.trkCard2[data-card="list"]{padding-left:0!important;padding-bottom:0!important;height:100%!important}
.trkView[data-short] .trkCard2[data-card="list"] .trkList .hd{padding-left:calc(var(--trk-lchip-w,76px) + 4px)!important}
.trkView[data-short] .trkCard2[data-card="list"] .lMax,.trkView[data-short] .trkCard2[data-card="list"] .lSwap{top:22px;right:-19px;left:auto;width:19px;height:44px;border:1px solid #2c3a4a;border-left:0;border-radius:0 9px 9px 0;padding:0 0 0 2px;align-items:center;justify-content:flex-start}
.trkView[data-short] .trkCard2[data-card="list"] .lSwap{top:74px}
.trkView[data-short] .trkCard2[data-card="list"] .lMax::before,.trkView[data-short] .trkCard2[data-card="list"] .lMax::after,
.trkView[data-short] .trkCard2[data-card="list"] .lSwap::before,.trkView[data-short] .trkCard2[data-card="list"] .lSwap::after{display:none}
.trkView[data-short] .trkCard2[data-card="list"] .lMax svg{transform:rotate(90deg)}
.trkView[data-short] .trkCard2[data-card="list"] .lMax[aria-pressed="true"] svg{transform:rotate(-90deg)}
.trkView[data-short] .mChipsBL{left:var(--sai-l)!important;bottom:var(--sai-b)!important}
.trkView[data-short] .mChipsBL .mDock[data-dock="list"] .hCard{padding-left:6px!important;padding-bottom:0!important}
/* the shared-tuner pill: the bottom centre, above the inset, in the span shortGeom leaves it (--trk-pill-pl / -pr) */
.trkView[data-short] .trkTop{top:auto!important;bottom:var(--sai-b)!important;min-height:0!important;height:auto!important;padding:0 var(--trk-pill-pr,140px) 0 var(--trk-pill-pl,140px)!important;align-items:flex-end!important}
.trkView[data-short] .trkTop .slot-share,.trkView[data-short] .trkTop .slot-share>span{min-height:0!important;height:auto!important;align-items:flex-end!important}
.trkView[data-short] .trkTop .slot-share .mapShLine{min-height:0!important;height:auto!important;line-height:1.2!important;white-space:nowrap!important;margin:0!important;
  padding:6px 14px!important;border:1px solid var(--bar-border,rgba(255,255,255,.18))!important;border-radius:12px!important}
/* the bottom-right group's pieces, laid by shortGeom: the zoom a horizontal pair; no stray margins; Leaflet's credit a short line that opens */
.trkView[data-short] .trkMapHost .maplibregl-ctrl-top-left .maplibregl-ctrl-group,.trkView[data-short] .trkMapHost .leaflet-top.leaflet-left .leaflet-bar{display:flex!important;flex-direction:row-reverse;margin:0!important}
.trkView[data-short] .trkMapHost .maplibregl-ctrl-top-left .maplibregl-ctrl-group button+button{border-top:0!important;border-right:1px solid #ddd}
.trkView[data-short] .trkMapHost .maplibregl-ctrl-bottom-right .maplibregl-ctrl,.trkView[data-short] .trkMapHost .leaflet-bottom.leaflet-right .leaflet-control{margin:0!important}
.trkView[data-short] .trkMapHost .leaflet-control-attribution{max-width:26px;overflow:hidden;white-space:nowrap;text-overflow:clip;cursor:pointer}
.trkView[data-short] .trkMapHost .leaflet-control-attribution:hover,.trkView[data-short] .trkMapHost .leaflet-control-attribution:focus-within{max-width:min(70vw,520px)}
/* the scale lies down in the group (shortGeom adds scaleH): the line across, its words beside it */
.trkView.scaleH .mScaleBody{flex-direction:row;align-items:center}
.trkView.scaleH .mScaleBody .vsLine{height:6px;border-width:0 2px 2px 2px}
.trkView.scaleH .mScaleBody .vsLbl{flex-direction:row}
.trkView.scaleH .mChipsScale .hCard{padding:4px 6px!important}
.trkCard2[data-card="list"] .trkList .hd{flex-wrap:nowrap!important;white-space:nowrap}   /* ★ the bar is ONE line at every size (Stuart 15:51) */
/* ★★★ V12 SAFE AREAS: each edge control stands IN from its edge by that edge's inset (the drawer's and cards' plates may run
 *  under it — their content is padded); the ✕ off the notch side, the chips, the ? / scale stack, zoom and credits */
.trkView .trkTop .exit{left:0!important;width:calc(52px + var(--sai-l))!important;padding:0 0 0 var(--sai-l)!important}
/* ★ Stuart 16:0x ("health and time tabs appear twice, one off the edge"): a TUCKED card's own › sat past the edge beside its
 *  tab — only the tab shows while tucked */
.trkView .edgeDock.tucked .hTuck,.trkView #srvHealth.tucked .hTuck{visibility:hidden}
/* ★★ Stuart 17:0x (iPhone, island left: "health and time appear TWICE, one set past the edge"): a tucked card slides out by
 *  its own width and an untucked tab hides the same way — with the chips stood IN from the edge by the safe area, the slid
 *  piece landed in the inset, on screen. Each half is invisible while it is away, so only one of card / tab ever shows. */
.trkView .edgeDock.tucked .hCard{visibility:hidden;transition:transform .2s ease,visibility 0s linear .2s}
.trkView .edgeDock:not(.tucked) .hTab{visibility:hidden;transition:transform .2s ease,visibility 0s linear .2s}
.trkView .edgeDock.tucked .hTab,.trkView .edgeDock:not(.tucked) .hCard{transition:transform .2s ease,visibility 0s}
/* ★★★ an EDGE piece (portrait's side chips and tabs, the ✕ corner, the LIST drawer) sits on the PHYSICAL edge and pads its
 *  CONTENT in by that edge's inset; free-standing controls (the ? and scale, zoom, credits) stand IN by it. */
.trkView .mChipsR .mDock .hCard{padding-right:calc(4px + var(--sai-r))!important}
.trkView:not([data-short]) .mChipsR #srvHealth .hCard,.trkView:not([data-short]) .mChipsR .mTime .hCard{padding-right:calc(8px + var(--sai-r))!important}
.trkView .mDock[data-dock="rate"] .hCard{padding-right:var(--sai-r)!important}
.trkView .mChipsL .mDock .hCard{padding-left:calc(4px + var(--sai-l))!important}
.trkView .mChipsBL .mDock .hCard{padding-left:calc(6px + var(--sai-l))!important}
.trkView .edgeDock .hTab{width:calc(26px + var(--sai-r));padding-right:var(--sai-r)}
.trkView .edgeDock.left .hTab{width:calc(26px + var(--sai-l));padding-left:var(--sai-l);padding-right:0}
/* ★ every layout: the collapsed LIST chip is an edge piece — its frame on the physical bottom, its content above the inset
 *  (Stuart's portrait check: ☰ n ⌃ sat in the 34 px gesture inset) */
.trkView .mChipsBL .mDock[data-dock="list"]:not(.open) .hCard{padding-bottom:var(--sai-b)!important}
/* ★ Stuart 19:xx: the collapsed LIST chip is ONE rounded rectangle with a full border — [☰ n ⌃ | ≋] (the waterfall beside
 *  the list, live maps over a radio only). Portrait / desktop: it sits its border + corner radius PAST the left and bottom
 *  edges (the joined-to-the-edge look, as the cards); short landscape: all four corners inside the safe rectangle. */
.trkView .mChipsBL .mDock[data-dock="list"]:not(.open) .hCard{flex-direction:row!important;align-items:stretch!important;border:1.5px solid var(--chip-rim)!important;border-radius:12px!important;
  margin:0 0 -13.5px -13.5px;padding-left:calc(6px + 13.5px + var(--sai-l))!important;padding-bottom:calc(13.5px + var(--sai-b))!important}
.trkView[data-short] .mChipsBL .mDock[data-dock="list"]:not(.open) .hCard{margin:0;padding-left:6px!important;padding-bottom:0!important}
.trkView .mChipsScale{right:var(--sai-r);margin-bottom:var(--sai-b)}
.trkView .mHelp{right:calc(10px + var(--sai-r));margin-bottom:var(--sai-b)}
.trkMapHost .maplibregl-ctrl-top-left,.trkMapHost .leaflet-top.leaflet-left{right:var(--sai-r)!important}
.trkMapHost .maplibregl-ctrl-bottom-right,.trkMapHost .leaflet-bottom.leaflet-right{right:var(--sai-r)!important;bottom:var(--sai-b)!important}
.trkMapHost .maplibregl-ctrl-bottom-left,.trkMapHost .leaflet-bottom.leaflet-left{left:var(--sai-l)!important;bottom:var(--sai-b)!important}
.trkCards .trkCard2[data-card="list"].stepAside{visibility:hidden;transform:translateY(105%);transition:transform .2s ease,visibility 0s linear .2s}
.trkCards .trkCard2[data-card="list"]{transition:transform .2s ease}
.mChipsBL .mDock.aside{visibility:visible!important;opacity:.55}
/* ★ HELP grows out of its ? — its bottom-right corner ON the button */
.trkCards .sideH{right:var(--trk-help-r,10px)!important;bottom:var(--trk-help-b,40px)!important;top:12px!important}
.trkCards .sideH>.trkCard2{max-height:100%;transform-origin:bottom right}
/* ★★★ tweak 5 (Stuart 11-27: "other cards can cover the list with a drop shadow to show they are the top layer"): ONE rule —
 *  the LIST's side is the bottom layer, every transient card's side above it (whatever the order they opened in), with a
 *  shadow in the map chrome's own dark. The LIST's top stops under the left chips (--trk-cb-top), so they stay pressable. */
.trkCards .sideB{z-index:1;top:max(30%,var(--trk-cb-top,0px))!important}
.trkCards .sideL,.trkCards .sideR,.trkCards .sideH{z-index:2}
.trkCards .sideL>.trkCard2,.trkCards .sideR>.trkCard2,.trkCards .sideH>.trkCard2{box-shadow:0 10px 28px rgba(0,0,0,.6),0 2px 6px rgba(0,0,0,.45)}
/* ★★★ TWEAK 5 (Stuart 11-27): at rest every chip is the SLIM SIDE TAB — the health pill's tucked tab (26 px of edge, its
 *  rim, its rounding), wearing only its own icon (LAYERS: each family's glyph over its count; LIST: the icon over its
 *  count). One press grows the card straight out of it; its heading sends it back. */
.mDock .hCard{padding:6px 4px 6px 4px!important;min-width:30px;justify-content:center}
.mDock .mchip{padding:0!important;min-height:44px!important;gap:5px}
.mDock .mchip .cap{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0)}
.mDock .mchip .ic{flex-direction:column;gap:2px}
.mDock .mIco{width:20px!important;height:20px!important}
.mDock .lsum{flex-direction:column!important;gap:6px!important}
.mDock .le{flex-direction:column!important;gap:2px!important;font-size:10.5px!important}
.mDock .lg svg{width:15px;height:15px}
/* ★ the SCALE chip: right edge, bottom, above the ? (and clear of the zoom ± up top) */
.mChipsScale{right:0;bottom:calc(var(--trk-attrib-h,0px) + 48px)}   /* the bottom-right stack: (i) · 8 px · ? (30) · 10 px · the scale */
.mScale .hCard{cursor:default!important;padding:6px 5px!important;min-width:0!important}
.mScale[hidden]{display:none}
.mScaleBody{display:flex;align-items:flex-start;gap:4px}
.saiProbe{position:absolute;width:0;height:0;overflow:hidden;visibility:hidden;pointer-events:none;padding:var(--sai-t) var(--sai-r) var(--sai-b) var(--sai-l)}
.mScaleBody .vsLine{display:block;width:6px;box-sizing:border-box;border:solid #e8eef5;border-width:2px 0 2px 2px}
.mScaleBody .vsLbl{display:flex;flex-direction:column;align-items:center;font:700 10px/1.05 ui-monospace,Menlo,monospace;color:#e8eef5}
.mDock .zst{position:relative;display:block;width:18px;height:18px;color:#7ee787}
.mDock .zst:empty{display:none}
.mDock .zst svg{position:absolute;inset:0;width:18px;height:18px}
.mDock[data-zoom="paused"] .zst{color:#ffd34d}
.mDock[data-zoom="off"] .zst{color:#9fb0c2}
.mDock .zst .zNo{color:#ff5a4d}
.trkCard2 .fitSect .btn.fit[data-zoom="paused"]{background:#3a2e0e;color:#ffd34d;border-color:#ffd34d}
.trkCard2 .fitSect .btn.fit[data-zoom="paused"]::after{content:" ‖"}
.trkCard2 .fitSect .btn.fit[data-zoom="off"]{background:#2a1d1d;color:#ff8a6b;border-color:#8a3434}
.trkCard2 .fitSect .btn.fit[data-zoom="off"]::after{content:" OFF"}
.lgI{width:16px;height:16px;display:block}
.trkList .hd .tab .tg{display:inline-flex;vertical-align:-3px;margin-right:4px}
.trkList .hd .tab .tg svg{width:14px;height:14px}
.mChipsBL .mDock .mchip{flex-direction:column!important;min-height:44px!important}
.mChipsBL .mDock .hCard{border-radius:12px 12px 0 0}
/* ★★★ the ? HELP button and its card, bottom-right above the attribution */
.mHelp{all:unset;box-sizing:border-box;position:absolute;right:10px;bottom:calc(var(--trk-attrib-h,0px) + 8px);z-index:790;width:30px;height:30px;
  border-radius:50%;display:flex;align-items:center;justify-content:center;cursor:pointer;font:800 15px/1 system-ui,sans-serif;color:var(--hc,#5BE36B);
  background:rgba(8,12,8,.86);border:1.5px solid rgba(120,200,120,.45);box-shadow:0 2px 8px rgba(0,0,0,.45)}
.mHelp:focus-visible{border-color:var(--hc,#5BE36B);outline:none;background:#16231a}
.mHelp:hover{background:#16231a}
.mHelp.on{background:rgba(255,211,77,.16);color:#ffd34d}
.trkCards .sideH{right:10px;bottom:calc(var(--trk-attrib-h,0px) + 48px);top:20%;justify-content:flex-end;width:min(340px,calc(100% - 20px))}
.trkCards .sideH>.trkCard2{border:1.5px solid var(--chip-rim);border-radius:12px;transform-origin:bottom right;background:rgba(8,12,8,.94)}
.helpBody{padding:6px 10px 10px}
.hRowH{display:flex;gap:10px;align-items:flex-start;padding:6px 0;font-size:12.5px;line-height:1.35;color:#dfe7ef;border-bottom:1px solid #151f29}
.hRowH b{font:700 10.5px/1 system-ui,sans-serif;letter-spacing:.12em;color:#fff}
.hRowH .hi{flex:none;color:var(--hc,#5BE36B);display:flex}
.hRowH .hi svg{width:18px;height:18px;display:block}
.hNoteH{padding:6px 0 0;font-size:11.5px;color:#9fb0c2;line-height:1.35}
/* ★★★ the LAYERS card is a LIST: one row per mode — its colour, its name, its count — a press shows / hides it */
.trkCard2[data-card="layers"] .sect{padding:0}
.trkCard2[data-card="layers"] .tgl{display:flex;flex-direction:column;align-items:stretch;gap:0}
.trkCard2[data-card="layers"] .tgl .grp{flex:none;margin:0;padding:8px 10px 4px;font:700 9.5px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip{display:flex;align-items:center;gap:10px;width:100%;box-sizing:border-box;border:0;border-bottom:1px solid #151f29;
  border-radius:0;background:none;padding:10px 12px;font:600 13px/1.2 system-ui,sans-serif;color:#e8eef5;opacity:1;text-align:left}
.trkCard2[data-card="layers"] .tgl .btn.chip:hover,.trkCard2[data-card="layers"] .tgl .btn.chip:focus-visible{background:#1a2632;outline:none}
.trkCard2[data-card="layers"] .tgl .btn.chip .dot{width:10px;height:10px;flex:none}
.trkCard2[data-card="layers"] .tgl .btn.chip .n{margin-left:auto;font:700 13px/1 ui-monospace,Menlo,monospace;opacity:1;color:#ffd34d;display:inline-block!important;min-width:4.5ch;text-align:right}
/* ★ Stuart 12:00 ("the on buttons are misaligned when no numbers are there"): one grid for every row — the count a fixed,
 *  right-aligned column (empty when none), the ON / OFF tag a fixed column after it */
.trkCard2[data-card="layers"] .tgl .btn.chip::after,.trkCard2 .mRow::after{min-width:3.2ch;text-align:center;box-sizing:content-box}
.trkCard2 .mRow .n{min-width:4.5ch;text-align:right}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"]{color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"] .n{color:#7f93a7}
.trkCard2[data-card="layers"] .tgl .btn.chip::after{content:"ON";flex:none;box-sizing:border-box;width:30px;text-align:center;font:800 9px/1 ui-monospace,Menlo,monospace;letter-spacing:.1em;padding:3px 5px;border-radius:4px;background:#1e3a26;color:#7ee787;margin-left:8px}
.trkCard2[data-card="layers"] .tgl .btn.chip[aria-pressed="false"]::after{content:"OFF";background:#2a1d1d;color:#ff8a6b}

/* ★ tweak 4: a tucked tab wears its chip's own glyph (the health pill's + is its own) */
.mDock .hTab{gap:3px}
.mDock .hTabIco{display:flex;flex-direction:column;align-items:center;gap:1px;color:var(--hc)}
.mDock .hTabIco svg,.mDock .tIco{width:16px;height:16px;display:block}
.mDock .tCn{font:700 8.5px/1 ui-monospace,Menlo,monospace;color:#e8eef5;letter-spacing:0}
.mDock .tCn:empty{display:none}
.mDock .hTab:has(.tCn:not(:empty)){height:62px}
.trkCards .sideL{left:0;top:var(--trk-cl-top,8px)}
.trkCards .sideR{right:0;top:var(--trk-cr-top,8px)}
/* ★ tweak 4: the LIST grows UP out of its chip at the bottom-left, where the old list sheet sat */
.mChipsBL{left:0;bottom:var(--trk-list-bottom,0px);overflow:hidden;padding-top:12px;margin-top:-12px}   /* ★ Stuart 12:07: the LIST is a DRAWER in the bottom-left corner */
.mChipsBL .edgeDock.bottom .hCard{border-left-width:0;border-radius:0 12px 0 0;padding-left:6px}
.mChipsBL .mDock .mchip{flex-direction:row;gap:8px;min-height:34px}
/* ★ the old scale corner is empty now (its lines live in the SCALE chip) */
.trkCards .sideB{left:0;right:auto;bottom:var(--trk-list-bottom,0px);top:30%;justify-content:flex-end;width:min(300px,calc(100% - 12px))}   /* ★ the LIST drawer: flush to the left edge and the bottom, as wide as its rows need */
.trkCards .sideB>.trkCard2[data-card="list"]{height:var(--trk-list-card-h,320px);flex:none!important;max-height:none!important}
.trkView[data-narrow] .trkCard2[data-card="list"]>.cb>.facts{display:none}
.trkView[data-narrow] .trkList .hd{flex-wrap:nowrap;gap:6px;padding:5px 8px}
.trkView[data-narrow] .trkList .hd{gap:6px}
/* ★★★ Stuart 12:47: the tabs are ONE ROW that SCROLLS, on every width that needs it — a fade and a small arrow at whichever
 *  edge has more beyond it (only then), the arrow nudging the row by about a tab; NEAREST | LATEST pinned at the right,
 *  outside the scroller, so it is always in reach. The fade is a mask (compositor work, no repaint of the rows). */
.trkList .hd .tabWrap{position:relative;display:flex;min-width:0;flex:1 1 auto}
.trkList .hd .tabWrap[hidden]{display:none}
.trkList .hd .tabs{flex-wrap:nowrap!important;overflow-x:auto;overscroll-behavior-x:contain;scrollbar-width:none;min-width:0;flex:1 1 auto;scroll-behavior:smooth}
.trkList .hd .tabs::-webkit-scrollbar{display:none}
.trkList .hd .tab{white-space:nowrap;flex:none}
.trkList .hd .tabWrap.mR .tabs{-webkit-mask-image:linear-gradient(to right,#000 calc(100% - 30px),transparent);mask-image:linear-gradient(to right,#000 calc(100% - 30px),transparent)}
.trkList .hd .tabWrap.mL .tabs{-webkit-mask-image:linear-gradient(to left,#000 calc(100% - 30px),transparent);mask-image:linear-gradient(to left,#000 calc(100% - 30px),transparent)}
.trkList .hd .tabWrap.mL.mR .tabs{-webkit-mask-image:linear-gradient(to right,transparent,#000 30px,#000 calc(100% - 30px),transparent);mask-image:linear-gradient(to right,transparent,#000 30px,#000 calc(100% - 30px),transparent)}
.trkList .hd .tArr{all:unset;box-sizing:border-box;position:absolute;top:50%;transform:translateY(-50%);width:20px;height:22px;display:none;align-items:center;justify-content:center;
  color:#ffd34d;font:700 15px/1 system-ui,sans-serif;cursor:pointer;z-index:1;border-radius:5px}
.trkList .hd .tArr.l{left:-2px}.trkList .hd .tArr.r{right:-2px}
.trkList .hd .tabWrap.mL .tArr.l,.trkList .hd .tabWrap.mR .tArr.r{display:flex}
.trkList .hd .tArr:focus-visible{outline:2px solid #ffd34d;outline-offset:-2px}
.trkList .hd .srt{flex:none}
.trkView[data-narrow] .trkCard2[data-card="list"]>.ch{padding-top:4px;padding-bottom:4px}
.trkCard2[data-card="list"]>.cb{display:flex;flex-direction:column;overflow:hidden}
.trkCard2[data-card="list"]>.cb>.facts{flex:none}
.trkCard2[data-card="list"] .trkList{display:flex;flex-direction:column;flex:1 1 auto;min-height:0}
.trkCard2[data-card="list"] .trkList>.hd,.trkCard2[data-card="list"] .trkList>.filt,.trkCard2[data-card="list"] .trkList>.safety{flex:none}
.trkCard2[data-card="list"] .trkList .rows{flex:1 1 auto;min-height:0;overflow-y:auto;scrollbar-width:thin;-webkit-overflow-scrolling:touch;overscroll-behavior:contain}
.trkCards .cSide>.trkCard2{border:1.5px solid var(--chip-rim);background:rgba(8,12,8,.92)}
/* ★ Stuart 19:xx ("the pop-up boxes need a full border"): ONE border for every card, all four sides, always. Where a card
 *  meets a screen edge (portrait / desktop) it sits its border's width PAST that edge, so the edge-side line is off screen
 *  and the joined-to-the-edge look is unchanged; short landscape's cards (inside the safe rectangle) show all four. */
.trkCards .sideB>.trkCard2{border-radius:0 12px 0 0;transform-origin:bottom left;margin:0 0 -1.5px -1.5px}
.trkCards .sideL>.trkCard2{border-radius:0 12px 12px 0;transform-origin:top left;margin-left:-1.5px}
.trkCards .sideR>.trkCard2{border-radius:12px 0 0 12px;transform-origin:top right;margin-right:-1.5px}
@keyframes trkCardIn{from{opacity:.2;transform:scale(.35)}to{opacity:1;transform:none}}
.trkCard2>.ch{cursor:pointer;font:700 9px/1 system-ui,sans-serif;letter-spacing:.16em;color:#B9C0B4}
.trkCard2>.ch:hover{background:rgba(255,255,255,.04)}
.trkCard2>.ch .x{font:700 18px/1 system-ui,sans-serif;color:var(--hc,#5BE36B);border:0;padding:0 6px}
/* ★ Stuart 19:xx ("final tiny change"): no orientation system — EVERY card closes with one [×] at its header's top right,
 *  the same everywhere; health / time's tabs carry only their icon, centred (the tab opens, the card's × closes) */
.trkView .mChipsR>.edgeDock:not(.mDock) .hTabChev{display:none!important}
.trkView .mChipsR>.edgeDock:not(.mDock) .hTab{justify-content:center!important;align-items:center!important}
.trkView .mChipsR>.edgeDock:not(.mDock) .hCard{flex-direction:row}
.trkView .mChipsR>.edgeDock:not(.mDock) .hCard .hTuck{order:9;align-self:flex-start;font-size:0!important;width:auto!important;margin:0 0 0 6px!important;padding:0 6px!important;border:0!important;transform:none!important;opacity:1}
.trkView .mChipsR>.edgeDock:not(.mDock) .hCard .hTuck::before{content:"×";font:700 18px/1 system-ui,sans-serif;color:var(--hc,#5BE36B)}
.trkCards .sideL>.trkCard2>.ch{flex-direction:row}
.mDock .hTuck{width:15px!important;font-size:15px!important;opacity:.6}
.mDock:not(.left) .hTuck{border-right:1px solid rgba(255,255,255,.14);margin-right:5px!important}
.mDock.left .hTuck{border-left:1px solid rgba(255,255,255,.14);margin-left:5px!important}
.mDock .hTuck:hover{opacity:1}
.mDock .mchip{padding:2px 6px;min-height:40px}
.trkView[data-narrow] .trkCards .sideL{left:0;right:var(--trk-chip-r,64px);top:var(--trk-cl-top,8px)}
.trkView[data-narrow] .trkCards .sideR{left:var(--trk-chip-l,64px);right:0}
.trkView[data-narrow] .trkCards .sideB{left:0;right:var(--trk-chip-r,44px);width:auto}
`;
function addCss() {
  if (document.getElementById('trk-style')) return;
  const st = document.createElement('style'); st.id = 'trk-style'; st.textContent = CSS; document.head.appendChild(st);
}
function runSource(src: string) { const el = document.createElement('script'); el.textContent = src; document.head.appendChild(el); }
/** ★★★ beta96 the map control cards (docs/v12/MAP-CONTROL-CARDS.md): their rules are shared with the app (mapCards.ts). */
const CARDS_KEY = 'vibesdr.mapCards';
/** ★ the chips' sides follow the window (mapCards.ts cardSide / oneSide) — MAP CONTROLS moves edge as it is resized. */
const CARD_SIDE_NOTE = 'mapCards.cardSide';
/** ★★★ tweak 3 the LAYERS chip's glyphs — the shapes the map draws: the aircraft and ship silhouettes (airSeaTune's,
 *  as the TUNE list draws them), the NAVTEX station's disc and mast (navtexMap.ts navIcon), the balloon, the beacon. */
export type LayerGlyph = 'aircraft' | 'ship' | 'navtex' | 'balloon' | 'beacon' | 'navaid' | 'dot';
const P16 = (d: string) => `<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true"><path d="${d}"/></svg>`;
const LAYER_GLYPH: Record<LayerGlyph, string> = {
  aircraft: P16(TUNE_ICON_PATH.aircraft), ship: P16(TUNE_ICON_PATH.ship), balloon: P16(TUNE_ICON_PATH.balloon),
  beacon: P16(TUNE_ICON_PATH.beacon), navaid: P16(TUNE_ICON_PATH.navaid),
  navtex: modeIconSvg('navtex', 'lgI'),   // ★★★ Stuart's NAVTEX glyph (modeIcons.ts — one source)
  dot: '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><circle cx="8" cy="8" r="6" fill="currentColor"/></svg>',
};
/** A SHOW toggle's layer id → its glyph (the maps whose host gives no summary: the ISM maps). */
function glyphOf(id: string): LayerGlyph {
  return /^(adsb|acars|vdl2|hfdl|adsc|aircraft)/.test(id) ? 'aircraft' : /^(ships|ais|dsc)/.test(id) ? 'ship' : /^(navst|navtex)/.test(id) ? 'navtex'
    : /sonde/.test(id) ? 'balloon' : /^(ndb|beacon)/.test(id) ? 'beacon' : /^vor/.test(id) ? 'navaid' : 'dot';
}
/** ★ the SERVERS and LIST chips' glyphs — the house set (modeIcons.ts): the servers glyph IS the app's ServersChip's */
const SERVERS_SVG = modeIconSvg('servers', 'mIco');
const LIST_SVG = modeIconSvg('list', 'mIco');
const WF_SVG = modeIconSvg('waterfall', 'mIco');
/** ★ the HELP card's glyphs — each chip's own */
const HELP_GLYPH: Record<string, string> = {
  servers: SERVERS_SVG.replace('mIco', 'tIco'), tune: modeIconSvg('radio', 'tIco'), layers: P16(TUNE_ICON_PATH.aircraft), map: modeIconSvg('map', 'tIco'),
  list: LIST_SVG.replace('mIco', 'tIco'), stats: modeIconSvg('stats', 'tIco'), rate: modeIconSvg('clock', 'tIco'), chat: modeIconSvg('spots', 'tIco'),
};
const FIT_KEY = 'vibesdr.mapFitTo';
/** ★★★ beta97 the ADS-B layer's state for the MAP card's keys (adsblayer.js ctlState / onCtl) */
interface AcCtl { auto: 'on' | 'off' | 'paused'; fit: boolean; rings: boolean; range: boolean; rangeOffered: boolean; labels: boolean; rx: boolean }
const AC_KEYS: Array<[string, string, string]> = [
  ['rings', 'RINGS', 'Range rings in nautical miles around the receiver'],
  ['range', 'RANGE', 'ADS-B RANGE outline: the furthest aircraft heard on each bearing (not the distance rings)'],
  ['labels', 'LABELS', 'Callsign and altitude beside each aircraft'],
  ['centre', '⌖', 'Recentre on the aircraft'],
];
export { CARD_IDS, type CardId, trimOutliers };

export interface TrackViewOpts {
  /** ★★ AUTO FIT (Stuart 2026-11-21, ISM: "the map needs the auto fit"; 2026-11-23, every map: "an auto fit that gently
   *  glides the zoom in and out like the adsb maps"). ✗ No longer an option: since beta92 EVERY track view runs the shared
   *  auto fit (src/utils/mapAutoFit.ts — the ADS-B map's camera) and its FIT key is the AUTO key. Kept so callers compile. */
  fitOnOpen?: boolean;
  title: string;                 // "AIS" / "APRS"
  exitLabel: string;             // "EXIT AIS" / "CLOSE MAP"
  listTitle: string;             // "SHIPS" / "STATIONS"
  onExit: () => void;
  icon: (it: any) => { html: string; w: number; h: number };
  label: (it: any) => string;
  card: (it: any) => string;
  trail?: (key: string, done: (pts: [number, number][]) => void) => void;
  /** ★ V12 the ISM map: the Leaflet zoom from which names show (the layer's own default is 11), and how far a chosen
   *  item is zoomed in to (10). */
  labelZoom?: number;
  focusZoom?: number;
  /** ★ a count that grows when an item is heard again — the layer pulses it (live pushes). */
  heard?: (it: any) => number;
  /** ★ who keeps its label when two collide (the layer's declutter; higher wins). */
  labelRank?: (it: any) => number;
  /** ★ a faint great-circle line from the receiver to this item (the NAVTEX stations map). */
  arc?: (it: any) => boolean;
  /** ★★ V12 2026-11-01 THE TIME FILTER (Stuart: "filter the map based on last hour last day last week last month all
   *  time"): a segmented control in the top bar — the page filters; `rangeHint` its title (what "All" means here). */
  ranges?: Array<{ id: string; label: string; words: string }>;
  range?: string;
  onRange?: (id: string) => void;
  rangeHint?: string;
  /** ★ V12 what the segmented control IS, for a screen reader (default: the time filter's words) — the datalink's bands. */
  rangeLabel?: string;
  /** ★ V12 2026-11-01: a KEY — a folded panel, bottom-right of the map, listing what each icon means (rows of HTML). The
   *  labels give way when the map is busy or zoomed out (the declutter), so the icons alone must still be readable. */
  legend?: string;
  /** ★ the basemap PROFILE (vibemap-style.js "vibesdr:profiles"): 'marine' for sea maps — ports, no airports or runways
   *  (Stuart, 2026-11-01: "the airports are on the map layer but we need the ports instead"). Default: everything. */
  profile?: string;
  /** ★★ V12 2026-11-01 THE NAVTEX MAP's layers (Stations · Warnings · Weather): toggle keys in the top bar. */
  toggles?: Array<{ id: string; label: string; words: string }>;
  toggled?: Record<string, boolean>;
  onToggle?: (id: string, on: boolean) => void;
  /** ★ passed to the layer (tracklayer.js): an item's shape, its cluster group, the words a screen reader says, the card drawn. */
  shape?: (it: any) => any;
  cluster?: (it: any) => string;
  clusterZoom?: number;
  clusterAlways?: string[];
  /** ★ groups whose icons give way to every other item and name (tracklayer.js yieldGroups — the NAVTEX weather labels) */
  yieldGroups?: string[];
  aria?: (it: any) => string;
  onCard?: (key: string, el: HTMLElement) => void;
  /** ★★ beta96 (Stuart 11-26) a tap on an icon ZOOMS IN to it as a row does (the ISM mesh maps) — not only its card */
  zoomOnSelect?: boolean;
  /** ★★ V12 beta90 the SCALE BAR (every map has one — vibemapgl.js / vibemap.js): true = NM under the km (the marine
   *  and aviation maps). Default: NM for the 'marine' and 'aero' profiles, km alone otherwise. */
  nautical?: boolean;
  /** ★★★ V12 beta90 THE AVIATION MAP ON THE ADS-B MAP (Stuart, 2026-11-21): items carrying `_adsb` (aviationMap.ts
   *  avPlaneAdsb) are drawn by the ADS-B layer (web/mapkit/adsblayer.js — silhouettes, tones, labels, range rings,
   *  AUTO / FIT, the glide, its own cards); the rest (HFDL ground stations) by the track layer as before. */
  aircraft?: boolean;
  /** ★ beta91 (aircraft:true) where this server serves the owner's logos and silhouettes ("…/adsb/", adsb.ts adsbImageBase);
   *  null / absent = none. Every source's aircraft carries the ADS-B data's keys (vibe_aircraft_info.h). */
  imageBase?: string | null;
  /** ★★ beta96 THE AIR & SEA MAP: aircraft AND other layers on one map — both cameras made, setCamera picks one. */
  mixed?: boolean;
  /** ★★ beta96 elements (by id) the view leaves reachable while it covers the page, raised above it while it is shown —
   *  ADS-B mode's CHAT panel, its key being in this view's top bar now. */
  liveIds?: string[];
  /** ★★★ beta96 a TUNE row the server says this radio cannot run, pressed: its reason (the host's pill). */
  onTuneRefused?: (why: string) => void;
  /** ★★★ beta97 A MAP THE HOST DRAWS (the amateur / spots map, the directory's community map): the host makes its map in
   *  `holder` (under every chip) and TrackView makes none — it keeps the scale chip, the credits' fold and (given
   *  `points`) the shared AUTO / FIT on it. Used through mapChromeApi.ts (MapHost). */
  hostMap?: (holder: HTMLElement) => MapHost | null | Promise<MapHost | null>;
}
/** ★★★ beta97 what a host-drawn map gives the chrome (TrackViewOpts.hostMap; mapChromeApi.ts re-exports it). */
export interface MapHost {
  /** Leaflet-like for the scale chip: getCenter() → {lat,lng}, getZoom() in LEAFLET zoom (MapLibre's + 1), on('a b', f). */
  map: any;
  /** The camera the auto fit drives: a Leaflet map, a MapLibre map or a VibeMapGL vm (mapAutoFit.ts attach). Default `map`. */
  fitMap?: any;
  /** The VibeMapGL vm when it is the GPU map (for host layers that ask — whenMap). */
  gl?: any;
  /** Every point AUTO / FIT should frame ([lat, lon]) — given, the shared auto fit runs on this map. */
  points?: () => Array<[number, number]>;
  receiver?: () => { lat: number; lon: number } | null;
  maxZoom?: number;
  /** What one point is, for the AUTO key's words ("station shown"). */
  what?: string;
  /** The map goes with the view (hide / destroy). */
  destroy?: () => void;
}
/** ★★ beta96 an AREA-class item (the click order's last tier): a sea area, an area-shaped NAVTEX warning, a forecast arrow. */
export function isAreaItem(it: any): boolean {
  return !!it && (!!it._nofit || it._cl === 'm' || (!!it._sh && it._sh.kind === 'area'));
}
export class TrackView {
  readonly root: HTMLElement;
  private mapHost: HTMLElement;
  private list: HTMLElement;
  private rowsEl: HTMLElement;
  private safetyEl: HTMLElement;
  private statEl: HTMLElement;
  private countEl: HTMLElement;
  private gen = 0;
  private vm: any = null;
  private lmap: any = null;
  private vmL: any = null;   // ★ the Leaflet map's VibeMap (its setProfile)
  private layer: any = null;
  private pending: any[] | null = null;
  private rx: LatLon | null = null;
  private lastRows = '';
  private sel = '';
  private inerted: HTMLElement[] = [];
  private acLayer: any = null;                       // ★ beta90 the ADS-B layer (aircraft:true)
  /** ★★★ beta97 the ADS-B layer's keys in the MAP card — drawn here, the layer's actions behind them (adsblayer.js keys:false).
   *  AUTO and FIT are the card's own AUTO / FIT row (one camera control for every map), so they are not repeated. */
  private acCtl(st: AcCtl | null) {
    const slot = this.slot('ctl');
    let host = slot.querySelector(':scope > .acKeys') as HTMLElement | null;
    if (!st || !this.acLayer) { host?.remove(); this.cardsLayout(); return; }
    if (!host) {
      host = document.createElement('div'); host.className = 'acKeys';
      host.innerHTML = AC_KEYS.map(([k, t, title]) => `<button type="button" class="btn" data-ac="${k}" title="${esc(title)}"${k === 'centre' ? ' aria-label="Recentre"' : ''}>${esc(t)}</button>`).join('');
      host.addEventListener('click', (e) => { const b = (e.target as HTMLElement).closest('[data-ac]') as HTMLElement | null; if (b && !(b as HTMLButtonElement).disabled) this.acLayer?.actions?.[b.dataset.ac!]?.(); });
      slot.appendChild(host);
    }
    const set = (k: string, on: boolean, hidden = false, disabled = false, title = '') => {
      const b = host!.querySelector(`[data-ac="${k}"]`) as HTMLButtonElement | null; if (!b) return;
      b.setAttribute('aria-pressed', String(on)); b.classList.toggle('on', on); b.hidden = hidden; b.disabled = disabled;
      if (title) b.title = title;
    };
    const noRx = 'Not available: this server has not given its receiver position';
    set('rings', st.rings, false, !st.rx, st.rx ? 'Range rings in nautical miles around the receiver' : noRx);
    set('range', st.range, !st.rangeOffered);
    set('labels', st.labels);
    set('centre', false);
    this.cardsLayout();
  }
  private acKeys = new Map<string, string>();        // item key → its hex, and back
  private acHex = new Map<string, string>();

  constructor(private o: TrackViewOpts) {
    addCss();
    const r = document.createElement('div');
    r.className = 'trkView'; r.hidden = true;
    r.setAttribute('role', 'dialog'); r.setAttribute('aria-label', o.title + ' map');
    // ★ beta96 everything but EXIT and the status line sits in .scr: in the row as before on a wide window, ONE row that
    //   scrolls sideways on a phone (ADS-B's own bar did, Stuart 2026-10-25: "the only thing that must stay anchored is the
    //   Exit ADS-B button") — the AIR & SEA map's chips, ranges, GAIN and CHAT no longer stack four rows deep
    /* ★★★ beta96 THE MAP CONTROL CARDS (Stuart 11-27, docs/v12/MAP-CONTROL-CARDS.md): the bar holds only the way out, the
     *  title (wide windows), the SHARED TUNER line + CHAT ('share' slot), the card buttons and the live rate (top right).
     *  Everything else is in a card — LIST · TUNE · VIEW · MAP · KEY · SOURCES — that pops out of its button. */
    /* ★★★ tweak 3+ (Stuart 11-27): each chip is an EDGE DOCK exactly as the SERVER HEALTH pill's (healthTuck.ts, index.html
     *  .edgeDock) — out: the card with its caption and glyphs and a › (‹ on the left edge) that tucks it away; tucked: the
     *  + tab on the edge that brings it back; the same drag, flick, slide and colours. The chip itself opens its card. */
    // ★★★ tweak 4: a tucked chip's tab wears ITS OWN glyph (`tabIcon`) — never the health pill's + (Stuart: "the collapsed
    //     modes need to show the icons for what they control")
    const dock = (id: string, side: 'left' | 'right' | 'bottom', cap: string, inner: string, what: string, dial = false, tabIcon = '') =>
      `<div class="edgeDock mDock${side === 'right' ? '' : ' ' + side}" data-dock="${id}" data-side="${side}" hidden>`
      // ★★★ Stuart 11-27 (11:38): TWO states only — the collapsed chip (its own glyph, and the counts) and the open card.
      //     One press opens the card from the chip; one press on the card's header shrinks it back. No tuck, no + tab:
      //     the chip is already the small thing on the edge. (tabIcon is kept for the drivers' sake — unused.)
      + `<div class="hCard"><button type="button" class="hBody mchip" data-cbtn="${id}"${dial ? ' data-dial="1"' : ''} aria-expanded="false" aria-label="${esc(cap)} — open ${esc(what)}"><span class="cap">${esc(cap)}</span>${inner}</button></div></div>`;
    const card = (id: string, title: string, body: string) => `<section class="trkCard2" data-card="${id}" role="region" aria-label="${esc(title)}" hidden>`
      + `<div class="ch"><span class="ct">${esc(title)}</span><button class="x" type="button" aria-label="Close ${esc(title.toLowerCase())}">×</button></div><div class="cb">${body}</div></section>`;
    /* ★★★ beta96 TWEAK 2 (Stuart 11-27, live on the preview): the bar is EXACTLY "EXIT | Shared VFO: … 2/5 | CHAT | the
     *  incoming messages" — no title (kept, hidden, for the dialog's name and the drivers), no card buttons: they are a
     *  small RAIL down the map's right edge (.cardBtns in .trkMapHost), which costs the map nothing. */
    r.innerHTML = `<div class="trkTop"><button class="btn exit" type="button">${esc(o.exitLabel)}</button><span class="ttl">${esc(o.title)}</span>`
      + `<span class="rate" role="status" aria-live="off"></span><span class="spacer"></span><span class="st" aria-live="polite"></span></div>`
      /* ★★★ TWEAK 3 (Stuart 11-27, live): the HEALTH CARDS' shape — chips FLANKING the map that pop out a card and shrink
       *  back: TUNE and LAYERS down the left edge, MAP CONTROLS on the right, LIST at the bottom (its card pops UP). */
      + `<div class="trkMapHost"><div class="mChips mChipsL">${dock('servers', 'left', 'SERVERS', `<span class="ic">${SERVERS_SVG}</span>`, 'the servers card', false, SERVERS_SVG.replace('mIco', 'tIco'))}${dock('tune', 'left', 'TUNE', `<span class="ic">${modeIconSvg('radio', 'mIco')}</span>`, 'the tuning card', true, modeIconSvg('radio', 'tIco'))}`
      + `${dock('layers', 'left', 'LAYERS', '<span class="lsum"></span>', 'the layers card', false, LAYER_GLYPH.aircraft)}`
      + `${dock('stats', 'left', 'STATS', `<span class="ic">${modeIconSvg('stats', 'mIco')}</span>`, 'the stats card')}</div>`
      + `<div class="mChips mChipsR">${dock('rate', 'right', 'RATE', `<span class="ic rateIc"><span class="rv"><span class="rn"></span><span class="ru"></span></span><span class="rmore"></span><svg class="arw" viewBox="0 0 12 12" aria-hidden="true"><path d="M2.5 4.5 6 8l3.5-3.5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></span>`, 'every source\u2019s message rate')}`
      + `${dock('chat', 'right', 'CHAT', `<span class="ic"><svg class="mIco" viewBox="0 0 24 24" aria-hidden="true"><path d="M4 5.5h16v10H10l-4 3.5v-3.5H4z" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linejoin="round"/><circle cx="9" cy="10.5" r="1.1" fill="currentColor"/><circle cx="12" cy="10.5" r="1.1" fill="currentColor"/><circle cx="15" cy="10.5" r="1.1" fill="currentColor"/></svg><span class="unr" hidden></span></span>`, 'the chat')}`
      /* ★★★ Stuart 15:13: TIME is the APP's EdgeChip (src/components/EdgeChip.tsx + SDRScreen's SessionClock) — a card
       *  with its › joined to the edge, flicked away to a tab carrying the clock glyph and a ‹; the tab's colour is the
       *  countdown's stage (amber · yellow · red; a soft limit never red). The same edge-dock machinery as the health
       *  pill (healthTuck.ts wireEdgeDock). Not an mDock: it is not a card chip, it has no card-rules. */
      + `<div class="edgeDock mTime" data-dock="time" data-side="right" hidden><div class="hCard"><button type="button" class="hTuck" aria-label="Hide the time remaining" title="Tuck away">›</button>`
      + `<div class="hBody tClock" role="timer" aria-live="off"><span class="tCap"></span><span class="tNum"></span></div></div>`
      + `<button type="button" class="hTab" aria-label="Time remaining" title="Time remaining"><span class="tIcon"><svg viewBox="0 0 24 24" width="15" height="15" aria-hidden="true"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.2"/><path d="M12 7v5l3.5 2" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg></span><span class="hTabChev" aria-hidden="true">‹</span></button></div>`
      + `${dock('map', 'right', 'MAP', `<span class="ic">${modeIconSvg('map', 'mIco')}<span class="zst" aria-hidden="true"></span></span>`, 'the map controls', false, modeIconSvg('map', 'tIco'))}</div>`
      + `<div class="mChips mChipsBL">${dock('list', 'bottom', 'LIST', `<span class="ic">${LIST_SVG}<span class="cn"></span><svg class="arw" viewBox="0 0 12 12" aria-hidden="true"><path d="M2.5 7.5 6 4l3.5 3.5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></span>`, 'the list', false, `${LIST_SVG.replace('mIco', 'tIco')}<span class="tCn"></span>`)}</div>`
      // ★★★ the ? HELP button, bottom-right above the map's attribution (its (i) is the attribution's own — kept)
      // ★★★ the SCALE as its own little edge chip (Stuart 11-27: "scale could be its own little chip on the side of the
      //     screen"), bottom right above the ? — the map's own scale lines, moved in (scaleDock), live with every zoom
      // ★ Stuart 11-27: "it doesnt have to collapse, just sit at the side with a line, and 100nm could be vertical too"
      + `<div class="mChips mChipsScale"><div class="edgeDock mDock mScale" data-dock="scale" role="img" aria-label="Map scale"><div class="hCard"><div class="mScaleBody"><span class="vsLine"></span><span class="vsLbl"></span></div></div></div></div>`
      + `<button type="button" class="mHelp" data-cbtn="help" aria-expanded="false" aria-label="Help — what the map's chips do" title="What the chips do">?</button>`
      + `<div class="trkCards"><div class="cSide sideL"></div><div class="cSide sideR"></div><div class="cSide sideB"></div><div class="cSide sideH"></div></div><div class="trkPark" hidden>`
      + card('help', 'HELP', `<div class="hPad"></div><div class="helpBody">${MAP_HELP.map((h) => `<div class="hRowH"><span class="hi">${HELP_GLYPH[h.id]}</span><span><b>${esc(h.name)}</b> \u2014 ${esc(h.words)}</span></div>`).join('')}`
        + MAP_HELP_NOTES.map((t) => `<div class="hNoteH">${esc(t)}</div>`).join('') + `</div>`)
      + card('list', 'LIST', `<div class="facts"></div><div class="trkList"><div class="hd"><span class="lt">${esc(o.listTitle)}</span><span class="tabWrap" hidden><button class="tArr l" type="button" tabindex="-1" aria-label="Earlier tabs">‹</button><span class="tabs" role="tablist" aria-label="Lists"></span><button class="tArr r" type="button" tabindex="-1" aria-label="More tabs">›</button></span> <span class="n">0</span>`
        + `<button class="lPause" type="button" hidden>\u2016 <span class="n"></span></button><span class="srt" role="group" aria-label="Sort the aircraft" hidden></span>`
        + `<span class="wfInfo" aria-live="off"></span></div><div class="safety" hidden></div><div class="filt" hidden></div><div class="rows"></div><div class="wfPane" aria-label="Live waterfall"></div></div>`
        /* ★★★ V12 (Stuart 11-27): the LIST ↔ WATERFALL switch — a folder tab out of the drawer's TOP-LEFT, the mirror of the
         *  maximise tab at its top-right; it carries the OTHER view's glyph. Only on a live map over a radio (setWaterfall). */
        + `<button class="lSwap" type="button" hidden aria-label="Show the live waterfall" title="Live waterfall (W)">${WF_SVG}</button>`
        + `<button class="lMax" type="button" aria-pressed="false" aria-label="Maximise the list" title="Maximise the list (M)"><svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2.5 7.5 6 4l3.5 3.5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></button>`)
      + card('servers', 'SERVERS', `<div class="srvBody" role="group" aria-label="Servers and radios"></div>`)
      + card('rate', 'RATE', `<div class="rateBody" role="list" aria-label="Message rate by source"></div>`)
      + card('stats', 'STATS', `<div class="stTabs" role="tablist" aria-label="Modes"></div><div class="stBody" role="list"></div>`)
      + card('chat', 'CHAT', `<div class="chatBody"></div>`)
      + card('tune', 'TUNE', `<div class="facts"></div><div class="trkSlot slot-tune"></div><div class="tunePop" role="menu" aria-label="Tune the radio"></div>`)
      + card('layers', 'LAYERS', `<div class="facts"></div><div class="sect"><span class="rng tgl" role="group" aria-label="Layers"${o.toggles?.length ? '' : ' hidden'}></span></div>`
        + `<div class="sect scopeSect" hidden><span class="rng scope" role="group" aria-label="Sources"></span><div class="note scopeNote2"></div></div>`)
      + card('map', 'MAP CONTROLS', `<div class="sect rngSect"${o.ranges?.length ? '' : ' hidden'}><div class="sh">TIME</div>`
        + (o.ranges?.length ? `<span class="rng" role="group" aria-label="${esc(o.rangeLabel || 'Show what was heard in the last…')}" title="${esc(o.rangeHint || '')}">`
           + o.ranges.map((x) => `<button class="btn" type="button" data-rng="${esc(x.id)}" aria-pressed="${x.id === o.range}" aria-label="${esc(x.words)}">${esc(x.label)}</button>`).join('') + '</span>' : '')
        + `</div>`
        + `<div class="sect fitSect"><div class="sh">MAP</div><div class="row"><button class="btn fit" type="button" title="Fit every ${esc(o.listTitle.toLowerCase())} on the map">AUTO</button>`
        + `<button class="btn fitOnce" type="button">FIT</button></div><div class="sh fitToH" style="margin-top:8px" hidden>FIT TO</div><div class="fitSel" role="radiogroup" aria-label="Fit to" hidden></div><div class="note fitNote"></div>`
        + `<div class="ctlSect" hidden><div class="trkSlot slot-ctl" style="margin-top:8px"></div><div class="note farNote"></div></div></div>`
        + `<div class="sect srcSect" hidden><div class="sh">SOURCES — TAP TO SHOW OR HIDE</div><div class="srcBody" role="group" aria-label="Sources"></div></div>`
        + `<details class="sect keySect"${o.legend ? '' : ' hidden'}><summary>KEY</summary><div class="trkKey"><div class="kb">${o.legend || ''}</div></div></details>`)
      + `</div></div>`;
    this.root = r;
    this.mapHost = r.querySelector('.trkMapHost') as HTMLElement;
    this.list = r.querySelector('.trkList') as HTMLElement;
    this.rowsEl = r.querySelector('.rows') as HTMLElement;
    this.safetyEl = r.querySelector('.safety') as HTMLElement;
    this.statEl = r.querySelector('.st') as HTMLElement;
    this.countEl = r.querySelector('.hd .n') as HTMLElement;
    (r.querySelector('.exit') as HTMLButtonElement).onclick = () => o.onExit();
    /* ★★★ beta92 the .fit key is the shared AUTO FIT's (attach → VibeAutoFit.attach, button): on · paused by a touch ·
     *  off, as the ADS-B map's AUTO key. Its click is the auto fit's own. */
    this.fitBtn = r.querySelector('.fit') as HTMLButtonElement;
    for (const b of Array.from(r.querySelectorAll('[data-rng]')) as HTMLButtonElement[]) {
      b.onclick = () => {
        const id = b.dataset.rng || '';
        for (const o2 of Array.from(r.querySelectorAll('[data-rng]'))) o2.setAttribute('aria-pressed', String(o2 === b));
        o.onRange?.(id);
        this.refit = true;
      };
    }
    // ★★★ beta96 THE CARDS: each button opens / shuts its card; × shuts it; FIT (once) fits the chosen layer
    this.cardsEl = r.querySelector('.trkCards') as HTMLElement;
    // ★ a drag inside a card scrolls the card — it never reaches the map underneath (Stuart 18:1x, the CHAT card on an iPhone)
    for (const ev of ['touchstart', 'touchmove', 'pointerdown', 'pointermove', 'wheel', 'mousedown', 'dblclick'])
      this.cardsEl.addEventListener(ev, (e) => { if ((e.target as HTMLElement | null)?.closest?.('.trkCard2')) e.stopPropagation(); }, { passive: true });
    // ★ Stuart 19:xx (iPhone, short landscape): health / time keep their TAB in the top row while their card is out (the card
    //   drops below it, like every card here) — so the tab, pressed again, tucks the card, as every other tab shuts its own
    r.querySelector('.mChipsR')?.addEventListener('click', (e) => {
      const tab = (e.target as HTMLElement | null)?.closest?.('.hTab') as HTMLElement | null, d = tab?.parentElement;
      if (!tab || !d || !this.root.hasAttribute('data-short') || !d.classList.contains('edgeDock') || d.classList.contains('mDock')) return;
      if (d.classList.contains('tucked')) {   // opening: ONE transient out at a time — the other of health / time, and any card
        this.shortEdgeTuck(d);
        this.closeTransients();
        return;
      }
      e.stopPropagation();
      (d.querySelector('.hTuck') as HTMLElement | null)?.click();
    }, true);
    this.parkEl = r.querySelector('.trkPark') as HTMLElement;
    for (const b of Array.from(r.querySelectorAll('[data-cbtn]')) as HTMLButtonElement[]) b.onclick = () => {
      // ★ the LIST stepped aside for a card: its chip brings it back (the card folds), never shuts it
      if (b.dataset.cbtn === 'list' && this.lpFired) { this.lpFired = false; return; }
      if (b.dataset.cbtn === 'rate' && this.rateStatic) return;
      if (b.dataset.cbtn === 'chat' && b.getAttribute('aria-disabled') === 'true') { this.onDialLocked?.(b.title); return; }
      if (b.dataset.cbtn === 'list' && this.listAside) { this.closeTransients(); return; }
      if (b.dataset.cbtn === 'list' && !this.isCardOpen('list')) this.setPane('list', false);   // ★ the chip opens the LIST; the ≋ tab the waterfall
      this.cardOpen(b.dataset.cbtn as CardId, !this.isCardOpen(b.dataset.cbtn as CardId), true);
    };
    // ★★★ tweak 4 (Stuart 11-27): the open card IS the chip, expanded in place — a press on its header (or its ‹ / ›) shrinks
    //     it back to the chip; the tuck to the edge stays the separate, deliberate › / drag / flick on the chip
    for (const ch of Array.from(r.querySelectorAll('.trkCard2 > .ch')) as HTMLElement[]) {
      ch.onclick = () => { const id = (ch.closest('.trkCard2') as HTMLElement).dataset.card as CardId; this.cardOpen(id, false, true); (r.querySelector(`[data-cbtn="${id}"]`) as HTMLElement | null)?.focus(); };
    }
    for (const d of Array.from(r.querySelectorAll('.mDock')) as HTMLElement[]) {
      const id = d.dataset.dock as CardId;
      edgeDockSet(d, true);   // ★ always out: the chip IS the collapsed state (no tuck since 11:38)
      void id; void wireEdgeDock;
    }
    for (const c of [...Array.from(r.querySelectorAll('.trkCard2')), ...Array.from(r.querySelectorAll('.mChips'))] as HTMLElement[])
      for (const ev of ['mousedown', 'wheel', 'touchstart', 'dblclick', 'pointerdown']) c.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    (r.querySelector('.fitOnce') as HTMLButtonElement).onclick = () => this.fitOnce();
    // ★★★ tweak 5: a tap or a drag on the MAP folds the transient cards back (cards and chips stop their own presses)
    this.mapHost.addEventListener('pointerdown', (e) => { if (!(e.target as HTMLElement).closest?.('.trkCard2, .mDock, .mHelp')) this.closeTransients(); });
    for (const c of Array.from(r.querySelectorAll('.trkCard2')) as HTMLElement[]) {
      if (STICKY_CARDS.has(c.dataset.card as CardId)) continue;
      c.addEventListener('pointerenter', () => { this.overCard = true; });
      c.addEventListener('pointerleave', () => { this.overCard = false; this.idleArm(); });
      for (const ev of ['pointerdown', 'keydown', 'wheel', 'focusin']) c.addEventListener(ev, () => this.idleArm(), { passive: true });
    }
    this.cardsLoad();
    this.setToggles(o.toggles ?? [], o.toggled ?? {});
    for (const id of CARD_IDS) this.cardsAvail(id, this.cardAvail.has(id));
    this.viewSync();
    this.cardsLayout();
    if (typeof ResizeObserver !== 'undefined') { this.cardRo = new ResizeObserver(() => this.cardsLayout()); this.cardRo.observe(r); }
    { // ★ the visual viewport (iOS toolbars, the keyboard): the LIST's rows follow it, settled (debounced)
      const vv = (window as any).visualViewport as EventTarget | undefined; let t: ReturnType<typeof setTimeout> | null = null;
      const on = () => { if (t) clearTimeout(t); t = setTimeout(() => { if (this.open && this.cardOrder.includes('list')) this.cardsLayout(); }, 150); };
      vv?.addEventListener('resize', on); vv?.addEventListener('scroll', on);
    }
    if (typeof ResizeObserver === 'undefined') window.addEventListener('resize', () => this.cardsLayout());
    this.rowsEl.addEventListener('scroll', () => {
      const st = this.rowsEl.scrollTop;
      if (st > 6 && !this.listFrozen) this.listFreeze(true);
      else if (st <= 1 && this.listFrozen) this.listFreeze(false);
    }, { passive: true });
    (r.querySelector('.trkList .hd .lPause') as HTMLElement).addEventListener('click', (e) => { e.stopPropagation(); this.rowsEl.scrollTo({ top: 0 }); this.listFreeze(false); });
    this.rowsEl.addEventListener('pointerdown', (e) => {
      const row = (e.target as HTMLElement).closest('.trkRow') as HTMLElement | null;
      this.down = { key: row?.dataset.key || '', at: performance.now(), x: e.clientX, y: e.clientY };
    });
    this.rowsEl.addEventListener('click', (e) => this.rowPick(e));
    this.rowsEl.addEventListener('keydown', (e) => {
      const k = (e as KeyboardEvent).key;
      if (k !== 'Enter' && !(k === ' ' && (e.target as HTMLElement).closest('.trkRow'))) return;
      if (k === ' ') e.preventDefault();
      this.rowPick(e);
    });
    for (const ev of ['mousedown', 'wheel', 'touchstart', 'dblclick', 'pointerdown']) this.list.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    /* ★★★ beta96 Escape (and the pad's BACK): inside a card — that card shuts, focus back on its button; else an item's
     *  own card on the map (the selection) first, as before; else the TOP card (the last opened); else the map. */
    { // ★ the LIST: its tab, a long press of its chip, or M — maximise / restore
      const lb = r.querySelector('[data-cbtn="list"]') as HTMLElement | null; let lp: ReturnType<typeof setTimeout> | null = null;
      lb?.addEventListener('pointerdown', () => { if (lp) clearTimeout(lp); lp = setTimeout(() => { this.lpFired = true; if (!this.isCardOpen('list')) this.cardOpen('list', true, true); this.setListMax(!this.listMax); }, 550); });
      for (const ev of ['pointerup', 'pointercancel', 'pointerleave']) lb?.addEventListener(ev, () => { if (lp) { clearTimeout(lp); lp = null; } });
      (r.querySelector('.lMax') as HTMLElement | null)?.addEventListener('click', (e) => { e.stopPropagation(); this.setListMax(!this.listMax); });
      r.addEventListener('keydown', (e) => {
        if ((e.key !== 'm' && e.key !== 'M') || e.metaKey || e.ctrlKey || e.altKey) return;
        const t = e.target as HTMLElement | null; if (t && (t.closest('input, textarea, select, [contenteditable="true"]'))) return;
        if (!this.isCardOpen('list')) return;
        e.preventDefault(); e.stopPropagation(); this.setListMax(!this.listMax);
      });
      /* ★★★ V12 the LIST ↔ WATERFALL tabs: the drawer's top-left tab swaps its contents in place; the collapsed chip's
       *  own ≋ tab opens the drawer straight onto the waterfall. W does the same from the keyboard. */
      const hc = r.querySelector('.mDock[data-dock="list"] .hCard') as HTMLElement | null;
      hc?.insertAdjacentHTML('beforeend', `<button class="lSwapC" type="button" hidden aria-label="Open the live waterfall" title="Live waterfall (W)">${WF_SVG}</button>`);
      (r.querySelector('.lSwap') as HTMLElement | null)?.addEventListener('click', (e) => { e.stopPropagation(); this.setPane(this.pane === 'wf' ? 'list' : 'wf'); });
      (r.querySelector('.lSwapC') as HTMLElement | null)?.addEventListener('click', (e) => { e.stopPropagation(); this.waterfallOpen(); });
      r.addEventListener('keydown', (e) => {
        if ((e.key !== 'w' && e.key !== 'W') || e.metaKey || e.ctrlKey || e.altKey || !this.wfOffered) return;
        const t = e.target as HTMLElement | null; if (t && (t.closest('input, textarea, select, [contenteditable="true"]'))) return;
        e.preventDefault(); e.stopPropagation();
        if (!this.isCardOpen('list') || this.listAside) this.waterfallOpen(); else this.setPane(this.pane === 'wf' ? 'list' : 'wf');
      });
      if (this.listMax) { r.classList.add('lMax'); const b = r.querySelector('.lMax') as HTMLElement | null; if (b) { b.setAttribute('aria-pressed', 'true'); b.setAttribute('aria-label', 'Restore the list'); } }
    }
    r.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape') return;
      e.stopPropagation();
      // ★ the page's one back (gamepad.ts → main.ts hooks.back) runs first and may have shut a card for this very press
      if (performance.now() - this.cardClosedAt < 80) return;
      if (this.closeTopCard(e.target)) return;
      if (this.sel) this.select('');
      else if (this.dialExit) this.onDialLocked?.(this.dialWhy);
      else o.onExit();
    });
    // ★★★ beta96 the dial lock (setDialLock): captured before EXIT's / a band key's own handler sees the press
    for (const ev of ['click', 'keydown'] as const) r.addEventListener(ev, (e) => {
      if (ev === 'keydown' && (e as KeyboardEvent).key !== 'Enter' && (e as KeyboardEvent).key !== ' ') return;
      if (this.dialRefuses(e.target)) { e.preventDefault(); e.stopImmediatePropagation(); }
    }, { capture: true });
    document.body.appendChild(r);
    if (/[?&]perf=1\b/.test(location.search)) perfOverlay();
  }
  /* ── ★★★ beta96 THE CARDS (Stuart 11-27) ─────────────────────────────────────────────────────────────────────────────
   *  ONE card on a phone-width window (opening another shuts the first); wider windows take more, side by side in columns
   *  (two stacked per column where the window is tall), never more than covers ~70 % of the map's width. Which cards are
   *  open is remembered per viewer, separately for a narrow and a wide window: nothing on a phone, LIST on a wide one. */
  private cardsEl: HTMLElement;
  private parkEl: HTMLElement;
  private cardRo: ResizeObserver | null = null;
  private cardOrder: CardId[] = [];
  private cardAvail = new Set<CardId>(['list', 'map', 'help']);
  private narrow(): boolean { const w = this.root.clientWidth || window.innerWidth; return w < CARD_NARROW_W; }
  /** How many cards this window takes at once (the window's size, never the device). */
  cardLimit(): number { return cardLimitFor(this.root.clientWidth || window.innerWidth, this.root.clientHeight || window.innerHeight); }
  private cardClosedAt = -1e9;
  /* ── ★★★ tweak 5 THE TRANSIENT CARDS fold back by themselves: after CARD_IDLE_MS untouched (not while the pointer rests on
   *  one), on a tap or drag on the map, after their action (a mode picked in TUNE, a server picked). ── */
  private idleT: ReturnType<typeof setTimeout> | null = null;
  private overCard = false;
  /** ★ short landscape: health / time's cards drop below the row like the cards — one transient out at a time, them included */
  private shortEdgeTuck(except: HTMLElement | null = null) {
    if (!this.root.hasAttribute('data-short')) return;
    for (const o of Array.from(this.root.querySelectorAll('.mChipsR > .edgeDock:not(.mDock):not(.tucked)')) as HTMLElement[]) if (o !== except) (o.querySelector('.hTuck') as HTMLElement | null)?.click();
  }
  /** Shut every transient card (LIST stays). True when one was shut. */
  closeTransients(): boolean {
    const t = this.cardOrder.filter((x) => !STICKY_CARDS.has(x));
    if (!t.length) return false;
    this.cardOrder = this.cardOrder.filter((x) => STICKY_CARDS.has(x));
    this.cardsLayout();
    this.fitPauseForCards();
    this.idleArm();
    return true;
  }
  private idleArm() {
    if (this.idleT) { clearTimeout(this.idleT); this.idleT = null; }
    if (!this.cardOrder.some((x) => !STICKY_CARDS.has(x))) return;
    this.idleT = setTimeout(() => { this.idleT = null; if (this.overCard) { this.idleArm(); return; } this.closeTransients(); }, CARD_IDLE_MS);
  }
  /** ★ BACK / Escape on the map: the card holding `from` (focus), else — unless an item's own card is open (the selection
   *  goes first) — the top card. True = one was shut. The pad's BACK reaches it through main.ts hooks.back. */
  closeTopCard(from: EventTarget | null = document.activeElement): boolean {
    // ★★ the pad / keyboard highlight counts as being in a card (real focus may sit on an item's card on the map)
    const inCard = ((from as HTMLElement | null)?.closest?.('.trkCard2') ?? this.root.querySelector('.gpFocus')?.closest('.trkCard2:not([data-card="list"])')) as HTMLElement | null;
    const open = this.openCards.filter((id) => this.cardAvail.has(id) && this.cardOrder.includes(id));
    const shown = Array.from(this.cardsEl.querySelectorAll('.trkCard2')).map((c) => (c as HTMLElement).dataset.card as CardId);
    const top = inCard && this.root.contains(inCard) ? inCard.dataset.card as CardId : this.sel ? undefined : [...open].reverse().find((id) => shown.includes(id));
    if (!top) return false;
    if (top === 'list' && this.listMax) { this.setListMax(false); this.cardClosedAt = performance.now(); return true; }
    this.cardOpen(top, false, true);
    this.cardClosedAt = performance.now();
    if (inCard) (this.root.querySelector(`[data-cbtn="${top}"]`) as HTMLElement | null)?.focus();
    this.navScope();
    padHighlight(this.root.querySelector(`[data-cbtn="${top}"]`) as HTMLElement | null);   // ★★ the pad's ring back on its chip
    return true;
  }
  isCardOpen(id: CardId): boolean { return this.cardOrder.includes(id); }
  get openCards(): CardId[] { return this.cardOrder.slice(); }
  private cardsKey(): string { return this.narrow() ? CARDS_KEY + '.narrow' : CARDS_KEY + '.wide'; }
  private cardsNarrowWas: boolean | null = null;
  private cardsLoad() {
    let ids: CardId[] | null = null;
    try { const j = JSON.parse(localStorage.getItem(this.cardsKey()) || 'null'); if (Array.isArray(j)) ids = j.filter((x) => (CARD_IDS as readonly string[]).includes(x)); } catch { /* private window */ }
    // ★ tweak 5: only LIST is remembered; the transient cards always start shut
    this.cardOrder = (ids ?? cardDefaults(this.root.clientWidth || window.innerWidth)).filter((x) => STICKY_CARDS.has(x));
    this.cardsNarrowWas = this.narrow();
  }
  private cardsSave() { try { localStorage.setItem(this.cardsKey(), JSON.stringify(this.cardOrder.filter((x) => STICKY_CARDS.has(x)))); } catch { /* private window */ } }
  /** Open / shut a card. `save` = a person's press (remembered); the host's own opening (TUNE's list from elsewhere) too. */
  /** The host's hook: a card opened or shut (CHAT fills its card on open). */
  onCard: ((id: CardId, open: boolean) => void) | null = null;
  /** Something happened in an open card (a chat message came): its idle clock starts again. */
  cardActivity() { this.idleArm(); }
  cardOpen(id: CardId, on: boolean, save = false) {
    if (on && !this.cardAvail.has(id)) return;
    if (on && id === 'rate' && this.rateStatic) return;
    // ★★★ tweak 5: LIST sticky, the rest transient — one transient at a time (mapCards.ts cardOpenRule); the window's limit
    //     is the layout's (a phone shows the newest: LIST comes back when the transient one shuts)
    void cardToggle;
    this.cardOrder = cardOpenRule(this.cardOrder.filter((x) => this.cardAvail.has(x)), id, on);
    if (on && id === 'help') this.helpPad();
    if (on && !STICKY_CARDS.has(id)) this.shortEdgeTuck();
    if (save) this.cardsSave();
    this.cardsLayout();
    this.fitPauseForCards();
    this.idleArm();
    if (on && save) {
      // ★ the pad / keyboard: into the card (its first control), so Escape / BACK come straight back out
      const el = this.root.querySelector(`.trkCard2[data-card="${id}"]`) as HTMLElement | null;
      // ★ TUNE: onto the modes (the live one), not the gain row lent above them (map-shared e2e: keyboard / pad)
      const first = (id === 'tune' ? el?.querySelector('.cb .tpRow[aria-checked="true"], .cb .tpRow') : null) ?? el?.querySelector('.cb button:not([disabled]):not([hidden]), .cb [tabindex="0"]');
      (first as HTMLElement | null)?.focus({ preventScroll: true });
      queueMicrotask(() => { this.navScope(); padHighlight((first as HTMLElement | null) ?? null); });   // ★★ the pad's ring goes in with it
    }
  }
  /* ★★★ V12 CONTROLLERS ON THE MAP (Stuart's fixed scheme — gamepad.ts): the highlight walks the chips (the top row in
   *  landscape, the left family in portrait), the LIST (one stop: enter takes its rows, back gives them up) and the
   *  bottom-right group; ENTER on a chip opens its card and the highlight goes INSIDE it — the open card is the scope
   *  until BACK shuts it and puts the highlight back on its chip. The map itself, its markers and popups are never
   *  stops (data-gpskip); its zoom keys and credits are (data-gpkeep). Zoom = MAP zoom (zoomBy); tune = the radio only
   *  where TUNE applies (main.ts hooks). */
  private navScope() {
    if (!this.open) { this.root.removeAttribute('data-navscope'); return; }
    const top = [...this.cardOrder].reverse().find((id) => !STICKY_CARDS.has(id) && this.cardAvail.has(id));
    const el = top ? this.root.querySelector(`.trkCards .trkCard2[data-card="${top}"]`) as HTMLElement | null : null;
    for (const c of Array.from(this.root.querySelectorAll('.trkCard2[data-navscope]'))) if (c !== el) c.removeAttribute('data-navscope');
    if (el && !el.hidden && el.getClientRects().length) { el.setAttribute('data-navscope', ''); this.root.removeAttribute('data-navscope'); }
    else this.root.setAttribute('data-navscope', '');
  }
  private navMarks() {
    for (const m of Array.from(this.root.querySelectorAll('.maplibregl-map, .leaflet-container')) as HTMLElement[]) m.setAttribute('data-gpskip', '');
    for (const c of Array.from(this.root.querySelectorAll('.maplibregl-ctrl-top-left, .maplibregl-ctrl-bottom-right, .leaflet-top.leaflet-left, .leaflet-bottom.leaflet-right')) as HTMLElement[]) c.setAttribute('data-gpkeep', '');
    const L = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
    if (L && !L.hasAttribute('data-navbox')) {
      L.setAttribute('data-navbox', ''); L.dataset.navstart = '.trkRow:not(.nopos)';   // ★ the first row that has a place on the map
      const rows = L.querySelector('.rows') as HTMLElement | null; if (rows) rows.dataset.navlist = '.trkList .hd';
    }
  }
  /** ★★ the pad's / keyboard's ZOOM on a map: the map's own zoom keys (as a tap — AUTO steps aside the same way) */
  zoomBy(d: 1 | -1) {
    const k = this.root.querySelector(d > 0 ? '.maplibregl-ctrl-zoom-in, .leaflet-control-zoom-in' : '.maplibregl-ctrl-zoom-out, .leaflet-control-zoom-out') as HTMLElement | null;
    k?.click();
  }
  /** ★ the HELP card's controller lines, in the layout in use (Switch-style face glyphs by POSITION, never letters) */
  private helpPad() {
    const el = this.root.querySelector('.trkCard2[data-card="help"] .hPad') as HTMLElement | null;
    if (!el) return;
    const sticks = activePadLayout() === 'sticks';
    const rows: Array<[string, string]> = sticks
      ? [['D-pad', 'move the highlight'], [padFace('bottom'), 'open / choose'], [padFace('right'), 'back / shut'], ['left stick turned', 'tune (where TUNE applies)'], ['right stick turned', 'zoom the map']]
      : [['D-pad', 'move the highlight'], ['R', 'open / choose'], ['L', 'back / shut'], [padFace('top') + ' ' + padFace('bottom'), 'zoom the map in / out'], [padFace('left') + ' ' + padFace('right'), 'tune down / up (where TUNE applies)']];
    el.innerHTML = `<div class="hPadH">With a controller</div>` + rows.map(([k, w]) => `<div class="hPadR"><span class="k">${k}</span><span>${esc(w)}</span></div>`).join('')
      + `<div class="hPadR"><span class="k">keyboard</span><span>arrows / WASD move · Enter · Esc back · &lt; &gt; tune · − + zoom</span></div>`;
  }
  /** Which cards may be offered (a card with nothing in it has no button — AGENTS.md: never a dead control). */
  /** A chip and its card offered (true) or withdrawn — mapChromeApi chips(). */
  cardsAvail(id: CardId, on: boolean) {
    if (on) this.cardAvail.add(id); else this.cardAvail.delete(id);
    const d = this.root.querySelector(`.mDock[data-dock="${id}"]`) as HTMLElement | null;
    if (d) d.hidden = !on;
  }
  /** ★ A chip tucked into its edge (its card shuts with it) or brought back out (its card stays shut until pressed). */
  private oneSideNow = false;
  /** ★★★ V12 short landscape (mapCards shortMap): the drawer a left side panel, the family a bottom row */
  private shortNow = false;
  get shortLayout(): boolean { return this.shortNow; }
  /** For the drivers: every chip down the left (a tall map) or flanking (MAP CONTROLS top-right). */
  get chipsOneSide(): boolean { return this.oneSideNow; }
  private layoutSig = '';
  /** Put the open cards beside their chips (and the rest in the park), the chips pressed, the obstacle told. */
  private cardsTold = new Set<CardId>();
  /** Tell the host of every card that opened or shut since last time (however it happened: a press, another card, idle). */
  private cardsTell(force?: Set<CardId>) {
    const now = force ?? new Set(this.cardOrder);
    for (const c of CARD_IDS) if (now.has(c) !== this.cardsTold.has(c)) { if (now.has(c)) this.cardsTold.add(c); else this.cardsTold.delete(c); this.onCard?.(c, now.has(c)); }
    this.wfTell();
  }
  cardsLayout() {
    if (!this.cardsEl) return;   // ★ still being made
    this.cardsTell();
    queueMicrotask(() => { this.navScope(); this.navMarks(); });
    { /* ★★★ Stuart 11-27: a TALL window puts every chip down the LEFT (SERVERS, TUNE, LAYERS, MAP CONTROLS; LIST bottom-left);
       *  a shorter one flanks — MAP CONTROLS top-right. By the window, live as it is resized. */
      // ★ the whole stack measured: every offered chip's own height (a tucked one counts as out — no flapping as it tucks)
      /* ★ Stuart 14:58 ("the map controls moved over to the right on the iphone"): ONLY the left family counts — SERVERS,
       *  TUNE, LAYERS, STATS, MAP CONTROLS, LIST at the foot, and the ✕ above them. The right column (RATE, CHAT, TIME,
       *  the lent SERVER HEALTH, the zoom) and the bottom-right stack never push the family into flanking. */
      const LEFT_FAMILY = new Set(['servers', 'tune', 'layers', 'stats', 'map', 'list']);
      const docks = (Array.from(this.root.querySelectorAll('.mDock')) as HTMLElement[]).filter((d) => !d.hidden && LEFT_FAMILY.has(d.dataset.dock || ''));
      const xH = ((this.root.querySelector('.trkTop .exit') as HTMLElement | null)?.offsetHeight || 48) + 14;   // ★ the X above SERVERS
      const stackH = docks.reduce((t, d) => t + Math.max(40, (d.querySelector('.hCard') as HTMLElement).offsetHeight) + 8, 16 + xH);
      // ★★★ V12 short landscape: the family is a row along the bottom — all of it on the left family's side (MAP too)
      const short = shortMap(this.mapHost.clientWidth || 0, this.mapHost.clientHeight || 0);
      /* ★★ the layout's decision, readable (Stuart's iPhone saw no change where the harness did — window.__mapGeom, and on
       *  the ?perf=1 overlay): the window, the visual viewport, the map box, the insets, the island rule, short or not */
      try {
        const vv = window.visualViewport, ins = this.insetPx();
        (window as any).__mapGeom = { iw: innerWidth, ih: innerHeight, vvw: vv ? Math.round(vv.width) : null, vvh: vv ? Math.round(vv.height) : null,
          mapW: this.mapHost.clientWidth, mapH: this.mapHost.clientHeight, ins, island: document.documentElement.dataset.island || '', short,
          cover: /viewport-fit=cover/.test((document.querySelector('meta[name="viewport"]') as HTMLMetaElement | null)?.content || ''),
          geomErr: this.geomErr, ua: navigator.userAgent.slice(0, 120) };
      } catch { /* diagnostics only */ }
      if (short !== this.shortNow) { this.shortNow = short; this.root.toggleAttribute('data-short', short); this.layoutSig = ''; }
      const one = short || oneSide(this.mapHost.clientHeight || 0, stackH);
      this.oneSideNow = one;
      const md = this.root.querySelector('.mDock[data-dock="map"]') as HTMLElement | null;
      const host = this.root.querySelector(one ? '.mChipsL' : '.mChipsR') as HTMLElement | null;
      if (md && host && md.parentElement !== host) {
        host.appendChild(md);
        md.dataset.side = one ? 'left' : 'right';
        md.classList.toggle('left', one);
        this.layoutSig = '';
      }
      this.root.toggleAttribute('data-oneside', one);
    }
    { // ★ the MAP card's layer keys: shown while a layer put keys in it (ADS-B, NDB)
      const sl = this.root.querySelector('.slot-ctl') as HTMLElement | null, sc = this.root.querySelector('.ctlSect') as HTMLElement | null;
      if (sl && sc) { const any = Array.from(sl.children).some((c) => !(c as HTMLElement).hidden); if (sc.hidden === any) sc.hidden = !any; }
    }
    if (this.cardsNarrowWas !== null && this.cardsNarrowWas !== this.narrow()) { this.cardsLoad(); }
    this.root.toggleAttribute('data-narrow', this.narrow());
    const W = this.root.clientWidth || window.innerWidth, H = this.root.clientHeight || window.innerHeight;
    // ★ the cards stand beside their chips: the chips' widths, the bottom chip's height
    const cw = (sel: string) => { const e = this.root.querySelector(sel) as HTMLElement | null; const b = e?.getBoundingClientRect(); return b && b.width > 0 ? b : null; };
    // ★ a tucked dock's card is off the edge: only its 26 px tab counts
    const dw = (sel: string) => Math.max(0, ...(Array.from(this.root.querySelectorAll(sel)) as HTMLElement[]).filter((d) => !d.hidden)
      .map((d) => (d.classList.contains('tucked') ? 26 : (d.querySelector('.hCard') as HTMLElement).getBoundingClientRect().width)));
    void CARD_SIDE_NOTE;
    void cw;
    const setv = (k: string, v: string) => { if (this.root.style.getPropertyValue(k) !== v) this.root.style.setProperty(k, v); };
    setv('--trk-chip-l', `${Math.ceil(dw('.mChipsL .mDock')) + 8}px`);
    setv('--trk-chip-r', `${Math.ceil(dw('.mChipsR .mDock')) + 8}px`);
    { const md = this.root.querySelector('.mChipsR') as HTMLElement | null; const b = md?.getBoundingClientRect(), mh = this.mapHost.getBoundingClientRect();
      setv('--trk-zoom-top', b && b.height ? `${Math.ceil(b.bottom - mh.top) + 10}px` : '0px'); }   // ★ always under RATE / CHAT / MAP
    { // ★ tweak 4: the LIST dock at the bottom-left — the scale bar steps right of it
      const ld = this.root.querySelector('.mChipsBL .mDock') as HTMLElement | null;
      const lb = ld && !ld.hidden ? (ld.classList.contains('tucked') ? (ld.querySelector('.hTab') as HTMLElement).getBoundingClientRect() : (ld.querySelector('.hCard') as HTMLElement).getBoundingClientRect()) : null;
      setv('--trk-list-w', `${Math.ceil(lb?.width ?? 0) + 8}px`);
      setv('--trk-list-h', `${Math.ceil(lb?.height ?? 0) + 8}px`);
    }
    setv('--trk-card-w', `${W >= CARD_ROOMY_W ? CARD_W : CARD_W_SNUG}px`);
    const place = cardPlace(this.cardOrder.filter((id) => this.cardAvail.has(id)), W, H, this.oneSideNow);
    const open = [...place.L, ...place.R, ...place.B, ...place.H];
    const sig = JSON.stringify([place, this.narrow()]);
    for (const b of Array.from(this.root.querySelectorAll('[data-cbtn]')) as HTMLElement[]) {
      const on = open.includes(b.dataset.cbtn as CardId);
      if (!(b.dataset.cbtn === 'rate' && this.rateStatic)) b.setAttribute('aria-expanded', String(on));   // ★ a one-source RATE is a reading
      b.closest('.mDock')?.classList.toggle('open', on);
      if (b.classList.contains('mHelp')) b.classList.toggle('on', on);
    }
    this.cardsEl.classList.toggle('both', false);
    this.cardsEl.classList.toggle('lAndB', false);   // ★ tweak 5: a transient card simply lies OVER the list (its shadow says so)
    this.listRefit();
    if (sig === this.layoutSig) { this.cardsAnchor(place); this.listStepAside(); this.liftOverList(); return; }   // ★ a data tick: only the anchors follow the chips
    this.layoutSig = sig;
    /* ★★★ tweak 5 (Stuart 11-27: "the list keeps popping open every refresh of data"): a card is MOVED only when its own
     *  place changes — one already where it belongs is never re-inserted (a re-insert restarts its pop-out animation and
     *  resets its scroll). Closed ones go to the park; open ones to their side, in order, only if out of place. */
    const focused = document.activeElement as HTMLElement | null;
    for (const c of Array.from(this.root.querySelectorAll('.trkCard2')) as HTMLElement[]) {
      if (open.includes(c.dataset.card as CardId)) continue;
      c.hidden = true;
      if (c.parentElement !== this.parkEl) this.parkEl.appendChild(c);
    }
    for (const side of ['L', 'R', 'B', 'H'] as const) {
      const host = this.cardsEl.querySelector(`.side${side}`) as HTMLElement;
      place[side].forEach((id, i) => {
        const c = this.root.querySelector(`.trkCard2[data-card="${id}"]`) as HTMLElement;
        c.hidden = false;
        if (c.parentElement !== host || host.children[i] !== c) {
          const st = (c.querySelector('.cb') as HTMLElement | null)?.scrollTop ?? 0;
          host.insertBefore(c, host.children[i] ?? null);
          const cb = c.querySelector('.cb') as HTMLElement | null; if (cb && st) cb.scrollTop = st;
        }
      });
    }
    if (focused && focused !== document.activeElement && this.root.contains(focused) && !focused.closest('[hidden]')) focused.focus({ preventScroll: true });
    this.cardsAnchor(place);
    this.listStepAside(); this.liftOverList();
    setTimeout(() => { this.listStepAside(); this.liftOverList(); }, 30);   // ★ once the new card has its size
    setTimeout(() => { this.placeAcObstacle(); try { this.af?.poke?.(); } catch { /* none */ } }, 0);
  }
  /** ★★★ tweak 4: each side's cards open AT their chip — the left / right column's top is its first open chip's top, the
   *  bottom card's foot the LIST chip's foot — so the chip seems to grow into its card (the health pill's way). */
  private cardsAnchor(place: Record<'L' | 'R' | 'B' | 'H', CardId[]>) {
    const m = this.mapHost.getBoundingClientRect();
    const dockTop = (id: CardId) => (this.root.querySelector(`.mDock[data-dock="${id}"]`) as HTMLElement | null)?.getBoundingClientRect() ?? null;
    const top = (ids: CardId[]) => Math.min(...ids.map((id) => { const b = dockTop(id); return b ? b.top - m.top : 1e9; }));
    const setv = (k: string, v: string) => { if (this.root.style.getPropertyValue(k) !== v) this.root.style.setProperty(k, v); };
    if (place.L.length) setv('--trk-cl-top', `${Math.max(6, Math.round(top(place.L)))}px`);
    if (this.shortNow && place.L.length) {   // ★ short landscape: the card pops UP from its chip on the bottom row
      const b = dockTop(place.L[0]);
      if (b && b.width) setv('--trk-cl-left', `${Math.max(0, Math.round(Math.min(b.left - m.left, m.width - 320 - 8)))}px`);
    }
    { const hb = (this.root.querySelector('.mHelp') as HTMLElement | null)?.getBoundingClientRect();
      if (hb && hb.width) { setv('--trk-help-r', `${Math.round(m.right - hb.right)}px`); setv('--trk-help-b', `${Math.round(m.bottom - hb.top)}px`); } }
    if (place.R.length) setv('--trk-cr-top', `${Math.max(6, Math.round(top(place.R)))}px`);
    { // ★ the pill centred between the X (left) and the right column's widest chip, clear of both
      const rc = this.root.querySelector('.mChipsR') as HTMLElement | null, x = this.root.querySelector('.trkTop .exit') as HTMLElement | null;
      const rw = rc ? Math.max(0, ...Array.from(rc.querySelectorAll('.mDock:not([hidden]) .hCard')).map((e) => (e as HTMLElement).offsetWidth)) : 0;
      const lw = (x?.offsetWidth || 48) + 10;
      setv('--trk-pill-pad', `${Math.max(rw, lw) + 10}px`);
      const rd = this.root.querySelector('.mDock[data-dock="rate"] .hCard') as HTMLElement | null, rb = rd?.getBoundingClientRect();
      if (rb && rb.height) { setv('--trk-rate-bottom', `${Math.round(rb.bottom - m.top)}px`); setv('--trk-rate-w', `${Math.round(rb.width)}px`); }
      this.root.classList.toggle('rateOpen', place.R.includes('rate'));
    }
    if (place.B.length) { const b = dockTop('list'); if (b) setv('--trk-cb-bottom', `${Math.max(0, Math.round(m.bottom - b.bottom))}px`); }
    { /* ★★★ Stuart 11:48 "wrong layering": the LIST never grows over the chips on its edge — its top stops under the left
       *  column of chips (SERVERS, TUNE, LAYERS …), so every tab stays visible and pressable */
      const col = this.root.querySelector('.mChipsL') as HTMLElement | null, cb = col?.getBoundingClientRect();
      const capTop = cb && cb.height ? Math.round(cb.bottom - m.top + 8) : 0;
      setv('--trk-cb-top', `${capTop}px`);
      /* ★★★ Stuart 11:59 "the list should be a fixed height and scrollable": its OWN height — about half the map, 240 to
       *  480 px, never more than the room under the chips — so it never jumps as rows come and go; only the rows scroll */
      const attrib = parseFloat(this.root.style.getPropertyValue('--trk-attrib-h')) || 0;
      { const a = this.mapHost.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution') as HTMLElement | null, ab = a?.getBoundingClientRect();
        const reachesLeft = !!ab && ab.width > 0 && ab.left < m.left + 320;   // ★ an attribution wrapped across a phone runs under the drawer
        setv('--trk-list-bottom', `${reachesLeft ? Math.round(m.bottom - ab!.top) : 0}px`); }
      const room = Math.max(120, m.height - capTop - attrib - 8);
      /* ★ Stuart 12:38 ("the list could show fewer lines at a time"): WHOLE ROWS — the header and N rows, N from a share of
       *  the map (≈ a third on a phone-sized map under 700 px, about half on a large one, ≤ 480 px), at least 3, never
       *  past the room; the last row always whole */
      const lcEl = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
      const rowsEl = lcEl?.querySelector('.rows') as HTMLElement | null;
      const rowH = (rowsEl?.querySelector('.trkRow') as HTMLElement | null)?.offsetHeight || 44;
      /* ★★ Stuart 11-27 ("the waterfall history is making the window grow by itself"): the head was the card's height less
       *  the rows' — in the waterfall view the rows are not shown (0 high), so the head was the WHOLE card, and every
       *  layout tick added n rows to it. The head is measured only while the list shows; the waterfall reuses it. */
      const listShows = !!rowsEl && rowsEl.getClientRects().length > 0;
      if (lcEl && rowsEl && lcEl.offsetHeight > 0 && listShows) this.listHead = Math.max(20, lcEl.offsetHeight - rowsEl.clientHeight);
      const head = this.listHead || 96;
      /* ★ Stuart 12:40 ("use the viewport size to determine the list size"): a continuous function of the VISUAL viewport
       *  (iOS Safari's toolbars and keyboard counted). Its height alone cannot tell an iPhone (390×844: 3–4 rows wanted)
       *  from a laptop window (1440×900: 8–9), so the share grows with the width too: 30 % of the height at phone width,
       *  rising to 55 % by 1500 px. At least 3 rows, never past the room. */
      const vv = (window as any).visualViewport as { width: number; height: number } | undefined;
      const vw = vv?.width || window.innerWidth, vh = vv?.height || window.innerHeight;
      const share = Math.min(0.55, Math.max(0.29, 0.29 + (vw - 400) / 4200));
      // ★ Stuart 12:55: the chip floats over the corner now — the strip it took is one more row at every size
      // ★ Stuart 13:06 ("the smaller list could be even more compact"): NORMAL is compact on every size — 22 % of the
      //   viewport's height in whole rows, 3 to 7 — so MAXIMISE is the big one (at least twice the rows)
      void share;
      let n = Math.min(7, Math.max(3, Math.round((vh * 0.22) / rowH)));
      while (n > 1 && head + n * rowH > room) n--;
      this.listNormH = Math.round(head + n * rowH);
      /* ★ Stuart 13:13 ("we must only cover when scrolling is available"): a short list (3 ships, a couple of messages)
       *  shrinks to its rows plus the chip's own clear space under them — no row under the chip, no empty drawer */
      // ★ the waterfall's view is always the full drawer (n rows' height) — it has no rows to shrink to
      if (rowsEl && this.pane !== 'wf' && rowsEl.childElementCount <= n + 2) {
        const chipH = parseFloat(this.root.style.getPropertyValue('--trk-lchip-h')) || 34;
        const content = (Array.from(rowsEl.children) as HTMLElement[]).reduce((t, c) => t + c.offsetHeight, 0);
        // fits without scrolling → its rows and the chip's clear strip (at most the chip's height over normal: a list that
        // just fills the drawer gets the strip rather than a row under the chip)
        void chipH;
        if (content <= n * rowH) this.listNormH = Math.round(head + content);
      }
      this.listMaxH = Math.max(this.listNormH, Math.round(m.height - capTop - (parseFloat(this.root.style.getPropertyValue('--trk-list-bottom')) || 0)));
      if (this.shortNow) {   // ★ short landscape: the side panel runs the full height under the ✕
        const xb = (this.root.querySelector('.trkTop .exit') as HTMLElement | null)?.getBoundingClientRect();
        this.listNormH = this.listMaxH = Math.max(120, Math.round(m.bottom - (xb && xb.height ? xb.bottom : m.top + 52) - 6));
      }
      setv('--trk-list-card-h', `${this.listMax ? this.listMaxH : this.listNormH}px`);
      // ★ the scale bar steps right of the open LIST (not just its chip)
      const lc = place.B.includes('list') ? (this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null)?.getBoundingClientRect() : null;
      if (lc && lc.width) setv('--trk-list-w', `${Math.ceil(lc.right - m.left) + 8}px`);
    }
  }
  /** ★★★ Stuart 12:18 ("the list auto hides when it will clip"): while a transient card would OVERLAP the LIST drawer, the
   *  LIST steps aside (down into its chip — dimmed, it is only away for the moment; a press brings it back and folds the
   *  card). Measured, never by window size; its node, scroll, tab and its remembered-open state untouched; no re-fit. */
  /** ★★★ Stuart 12:40 ("the help button and scale could shift up when list opened, the credits button can stay hidden"):
   *  while the open LIST drawer's rectangle covers the ? or the scale strip (measured, not by width) they slide UP to sit
   *  just above its top edge — transform only, the drawer's timing — and back down as it closes. The credits' (i) stays
   *  where it is, hidden under the drawer (they were shown as the map opened). HELP grows from the ? where it now is. */
  private liftOverList() {
    const m = this.mapHost.getBoundingClientRect();
    if (this.shortNow) {   // ★ short landscape: the family's bottom row starts right of the side panel (or of the ☰ chip)
      const lc = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
      const lOpen = !!lc && !lc.hidden && !this.listAside && !!lc.parentElement?.classList.contains('sideB') && lc.offsetWidth > 0;
      const ch = (this.root.querySelector('.mDock[data-dock="list"]:not([hidden]) .hCard') as HTMLElement | null)?.getBoundingClientRect();
      void lOpen; void ch; void lc;
    }
    { const ch = this.root.querySelector('.mDock[data-dock="list"] .hCard') as HTMLElement | null;   // the drawer's chip strip
      const v = `${ch && ch.offsetHeight ? ch.offsetHeight : 40}px`; if (this.root.style.getPropertyValue('--trk-lchip-h') !== v) this.root.style.setProperty('--trk-lchip-h', v);
      const w = `${ch && ch.offsetWidth ? ch.offsetWidth : 76}px`; if (this.root.style.getPropertyValue('--trk-lchip-w') !== w) this.root.style.setProperty('--trk-lchip-w', w); }
    if (this.shortNow) { this.shortGeom(m); return; }   // ★ short landscape: one geometry pass owns the bottom-right
    if (this.geomOn) this.shortGeomClear();
    const list = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
    const open = !!list && !list.hidden && !this.listAside && !!list.parentElement?.classList.contains('sideB') && list.offsetHeight > 0;
    let L: { l: number; t: number; r: number; b: number } | null = null;
    if (open) { const p = (list!.parentElement as HTMLElement).getBoundingClientRect(); L = { l: p.left + list!.offsetLeft, t: p.top + list!.offsetTop, r: p.left + list!.offsetLeft + list!.offsetWidth, b: p.top + list!.offsetTop + list!.offsetHeight }; }
    /* ★ Stuart 12:44 ("help clips the scale"): the right-bottom items are ONE stack — (i), ?, the scale, fixed gaps from the
     *  bottom up — and it moves as one, by the amount its lowest covered item needs; lifted separately, each landed on the
     *  drawer's edge and the ? sat on the scale */
    const stack = (Array.from(this.root.querySelectorAll('.mHelp, .mChipsScale')) as HTMLElement[]).map((el) => {
      const was = Number(el.dataset.lift || 0), b = el.getBoundingClientRect();
      return { el, was, b, base: { l: b.left, r: b.right, t: b.top + was, b: b.bottom + was } };   // where it sits unlifted
    });
    let lift = 0;
    // ★ Stuart 12:56 ("? clips the maximise button"): the drawer's top counts its folder tab too, 8 px clear of it
    const tab = list?.querySelector('.lMax') as HTMLElement | null, tabH = L && tab && tab.offsetHeight ? tab.offsetHeight : 0;
    const top = L ? L.t - tabH : 0;
    for (const { b, base } of stack) if (L && b.width && base.l < L.r - 1 && base.r > L.l + 1 && base.b > top - 7 && base.t < L.b - 1) lift = Math.max(lift, Math.ceil(base.b - top + 8));
    // ★ maximised, the list IS the screen: the ? and the scale step out of the way (the scale has no map to measure)
    { /* ★ Stuart 12:58 ("move the scale up slightly, then it can tuck in"): lifted, the scale must not reach the right
       *  column (RATE, CHAT, MAP) or the zoom — it shortens (a smaller round distance) before anything overlaps */
      const sc = stack.find((x) => x.el.classList.contains('mChipsScale'));
      // ★ short landscape: the zoom is on the bottom edge, not above the scale (Stuart 17:0x)
      const obs = Array.from(this.root.querySelectorAll(this.shortNow ? '.mChipsR' : '.mChipsR, .trkMapHost .maplibregl-ctrl-top-left, .trkMapHost .leaflet-top.leaflet-left')) as HTMLElement[];
      const floor = Math.max(0, ...obs.map((e) => { const r = e.getBoundingClientRect(); return r.height ? r.bottom : 0; }));
      let want = 96;
      let noRoom = false;
      if (sc && (sc.b.width || this.root.classList.contains('scaleAway')) && (L || this.shortNow)) {
        const over = floor + 8 - (sc.base.t - lift), room = Math.round(this.scaleMaxPx - over);
        want = Math.max(24, Math.min(96, room));
        // ★ no room under the zoom → no scale; but in SHORT landscape the zoom is on the bottom edge and the scale always stays
        //   (Stuart 17:07: "the scale strip is still missing") — at its shortest if it must
        noRoom = room < 24 && !this.shortNow;
      }
      this.root.classList.toggle('scaleAway', noRoom);
      if (want !== this.scaleMaxPx) { this.scaleMaxPx = want; this.scaleDraw(); }
    }
    /* ★ Stuart 16:25 ("maximised, the ? and the scale are GONE"): they step away only when the maximised drawer really
     *  lies under them (a phone's full-width drawer); beside a desktop's drawer they stay where they are */
    const hideStack = !!L && this.listMax && stack.some(({ base }) => base.l < L!.r - 1 && base.r > L!.l + 1);
    this.root.classList.toggle('stackAway', hideStack);
    if (hideStack) lift = 0;
    for (const { el, was, b, base } of stack) {
      if (lift !== was) { el.dataset.lift = String(lift); el.style.transform = lift ? `translateY(${-lift}px)` : ''; }
      if (el.classList.contains('mHelp') && b.width) {
        const setv = (k: string, v: string) => { if (this.root.style.getPropertyValue(k) !== v) this.root.style.setProperty(k, v); };
        setv('--trk-help-r', `${Math.round(m.right - base.r)}px`); setv('--trk-help-b', `${Math.round(m.bottom - (base.t - lift))}px`);
      }
    }
    // the (i): hidden while the drawer covers it, back as the LIST closes
    const at = this.mapHost.querySelector('.maplibregl-ctrl-attrib, .leaflet-control-attribution') as HTMLElement | null, ab = at?.getBoundingClientRect();
    const under = !!(L && ab && ab.width && ab.left < L.r && ab.right > L.l && ab.bottom > L.t && ab.top < L.b);
    this.root.classList.toggle('listOverAttrib', under);
  }
  /* ★★★ SHORT LANDSCAPE — ONE GEOMETRY PASS (Stuart 17:09: "each preview change fixes one thing and breaks another"). Like the
   *  app's edgeChipGeometry: every bottom-right piece placed here, from the box, the insets and what is already on the edges.
   *   · the RIGHT column (RATE, CHAT, health, time) is the flex column at the physical edge — its pieces pad their content in;
   *   · the BOTTOM-RIGHT group, right to left: the credits (i), the ?, the zoom pair, the scale (HORIZONTAL in this layout) —
   *     on the bottom edge above the inset, left of nothing the right column reaches; whatever would run into the family row
   *     goes up a row, still clear of the right column; the scale is the only piece that may stand down (no room at all).
   *  Inline px, applied together; cleared as the layout leaves short landscape. */
  private geomOn = false;
  private insetPx(): { l: number; r: number; b: number; t: number } {
    let pr = this.root.querySelector('.saiProbe') as HTMLElement | null;
    if (!pr) { pr = document.createElement('div'); pr.className = 'saiProbe'; pr.setAttribute('aria-hidden', 'true'); this.root.appendChild(pr); }
    const cs = getComputedStyle(pr);
    return { l: parseFloat(cs.paddingLeft) || 0, r: parseFloat(cs.paddingRight) || 0, b: parseFloat(cs.paddingBottom) || 0, t: parseFloat(cs.paddingTop) || 0 };
  }
  private geomErr = '';
  private shortGeom(m: DOMRect) {
    if (!m.width || !m.height || this.root.hidden) return;   // ★ never from an empty box (a map not laid out yet, or hidden) — its px would stick
    try { this.shortGeomRun(m); this.geomErr = ''; } catch (e) { this.geomErr = String((e as Error)?.message || e).slice(0, 120); }
  }
  private shortGeomRun(m: DOMRect) {
    this.geomOn = true;
    this.root.classList.add('scaleH');
    this.root.classList.remove('stackAway', 'scaleAway', 'listOverAttrib');
    const ins = this.insetPx(), GAP = 8;
    const W = m.width, H = m.height;
    const put = (el: HTMLElement | null, right: number, bottom: number) => {
      if (!el) return;
      const host = (el.offsetParent as HTMLElement | null)?.getBoundingClientRect() ?? m;
      el.style.setProperty('right', `${Math.round(right + (host.right - m.right))}px`, 'important');
      el.style.setProperty('bottom', `${Math.round(bottom + (host.bottom - m.bottom))}px`, 'important');
      el.style.setProperty('top', 'auto', 'important'); el.style.setProperty('left', 'auto', 'important');
      el.style.setProperty('margin-bottom', '0', 'important');
      el.style.transform = ''; el.dataset.lift = '0';
    };
    const help = this.root.querySelector('.mHelp') as HTMLElement | null;
    const attrib = this.mapHost.querySelector('.maplibregl-ctrl-bottom-right, .leaflet-bottom.leaflet-right') as HTMLElement | null;
    const zoom = this.mapHost.querySelector('.maplibregl-ctrl-top-left, .leaflet-top.leaflet-left') as HTMLElement | null;
    const scale = this.root.querySelector('.mChipsScale') as HTMLElement | null;
    if (this.scaleMaxPx !== 80) { this.scaleMaxPx = 80; this.scaleDraw(); }
    const size = (el: HTMLElement | null) => { if (!el) return { w: 0, h: 0 }; const kid = (el.querySelector('.maplibregl-ctrl-attrib-button, .leaflet-control-attribution') as HTMLElement | null) ?? (el.firstElementChild as HTMLElement | null) ?? el; const b = (el === attrib || el === zoom ? kid : el).getBoundingClientRect(); return { w: Math.ceil(b.width), h: Math.ceil(b.height) }; };
    // the family row's right end (the row's left is right of the panel / chip) and the right column's foot
    // ★ the bottom edge now holds the panel (left), the pill (centre) and this group (right): the group keeps clear of the
    //   panel's edge and leaves the pill at least 150 px
    const pn = this.root.querySelector('.trkCards .sideB > .trkCard2[data-card="list"]') as HTMLElement | null, pb = pn && !pn.hidden ? pn.getBoundingClientRect() : null;
    const ld = (this.root.querySelector('.mDock[data-dock="list"]:not([hidden]) .hCard') as HTMLElement | null)?.getBoundingClientRect();
    const leftUsed = Math.max(pb && pb.width ? pb.right - m.left : 0, ld && ld.width ? ld.right - m.left : 0);
    const famR = leftUsed + 150;
    const col = this.root.querySelector('.mChipsR') as HTMLElement | null, cb = col?.getBoundingClientRect();
    const colFoot = cb && cb.height ? cb.bottom - m.top : 0, colLeft = cb && cb.width ? cb.left - m.left : W;
    const pieces: Array<{ el: HTMLElement | null; w: number; h: number; optional?: boolean }> = [
      { el: attrib, ...size(attrib) }, { el: help, ...size(help) }, { el: zoom, ...size(zoom) }, { el: scale, ...size(scale), optional: true },
    ].filter((p) => p.el && p.w > 0);
    { // ★ one height for every top-hanging tab: the family chip's own (its content + padding), set before the row is measured
      const fc = this.root.querySelector('.mChipsL .mDock:not([hidden]) .mchip') as HTMLElement | null;
      if (fc && fc.offsetHeight) { const v = `${fc.offsetHeight + 8}px`; if (this.root.style.getPropertyValue('--trk-tab-h') !== v) this.root.style.setProperty('--trk-tab-h', v); }
      // ★ Stuart 18:22: and ONE WIDTH — the widest tab's content (a family chip with its counts, CHAT), every hanging tab at it
      const chips = Array.from(this.root.querySelectorAll('.mChipsL .mDock:not([hidden]):not([data-dock="layers"]) .mchip, .mChipsR .mDock:not([data-dock="rate"]):not([hidden]) .mchip')) as HTMLElement[];
      const wNeed = Math.max(36, ...chips.map((c) => c.scrollWidth + 8));
      const vw = `${Math.ceil(wNeed)}px`; if (this.root.style.getPropertyValue('--trk-tab-w') !== vw) this.root.style.setProperty('--trk-tab-w', vw); }
    // ★ the top row: its height (the cards drop from it) and, when the family would run into the right end, compact glyphs
    { const fam = this.root.querySelector('.mChipsL') as HTMLElement | null, fb = fam?.getBoundingClientRect(), rb = (this.root.querySelector('.mChipsR') as HTMLElement | null)?.getBoundingClientRect();
      const rowH = Math.max(fb && fb.height ? fb.bottom - m.top : 0, rb && rb.height ? rb.bottom - m.top : 0);
      this.root.style.setProperty('--trk-row-h', `${Math.round(rowH) + 6}px`);
      if (fb && rb && fb.width && rb.width) {
        const gap = rb.left - fb.right;
        if (gap < 8 && !this.root.classList.contains('famCompact')) { this.root.classList.add('famCompact'); this.layoutSig = ''; }
        else if (gap > 90 && this.root.classList.contains('famCompact')) { this.root.classList.remove('famCompact'); this.layoutSig = ''; }
      } }
    let row = 0, x = W - ins.r - GAP, rowBottom = ins.b + 6, rowH = 0, row0Left = W - ins.r;
    for (const p of pieces) {
      let left = x - p.w;
      if (left < famR + GAP && row === 0) {   // ★ would run into the family row: up a row, from the right again
        row = 1; rowBottom += rowH + GAP; rowH = 0; x = W - ins.r - GAP; left = x - p.w;
      }
      const top = H - rowBottom - p.h;
      const clearsCol = top >= colFoot + GAP || left + p.w <= colLeft - GAP;
      if (!clearsCol) {
        if (p.optional) { p.el!.style.setProperty('visibility', 'hidden'); continue; }
      }
      if (p.optional) p.el!.style.removeProperty('visibility');
      put(p.el, W - (left + p.w), rowBottom);
      if (row === 0) row0Left = left;
      x = left - GAP; rowH = Math.max(rowH, p.h);
    }
    { // the pill between the panel and the group (row 0's leftmost piece)
      const pl = Math.round(leftUsed + GAP), pr = Math.round(W - row0Left + GAP);
      this.root.style.setProperty('--trk-pill-pl', `${pl}px`); this.root.style.setProperty('--trk-pill-pr', `${Math.max(GAP, pr)}px`);
    }
  }
  private shortGeomClear() {
    this.geomOn = false;
    this.root.classList.remove('scaleH');
    for (const el of Array.from(this.root.querySelectorAll('.mHelp, .mChipsScale, .trkMapHost .maplibregl-ctrl-bottom-right, .trkMapHost .maplibregl-ctrl-top-left, .trkMapHost .leaflet-bottom.leaflet-right, .trkMapHost .leaflet-top.leaflet-left')) as HTMLElement[]) {
      for (const k of ['right', 'bottom', 'top', 'left', 'visibility', 'margin-bottom']) el.style.removeProperty(k);
    }
    this.scaleMaxPx = 96; this.scaleDraw();
  }
  /* ★★★ the LIST maximised (Stuart 12:46): it fills the map under the chips' column; AUTO pauses (there is no map to fit);
   *  remembered per viewer; Escape un-maximises before it folds. One layout at the toggle, then a transform slide. */
  private listNormH = 0;
  private listFrozen = false;
  private rowsPending = '';
  /** Frozen: each row already shown takes its new content in place; new keys are held (counted); gone keys dim. */
  private frozenApply(html: string) {
    const t = document.createElement('div'); t.innerHTML = html;
    const fresh = new Map<string, HTMLElement>();
    for (const r of Array.from(t.querySelectorAll('.trkRow[data-key]')) as HTMLElement[]) fresh.set(r.dataset.key!, r);
    const shown = new Set<string>();
    for (const r of Array.from(this.rowsEl.querySelectorAll('.trkRow[data-key]')) as HTMLElement[]) {
      const k = r.dataset.key!; shown.add(k);
      const n = fresh.get(k);
      if (!n) { r.classList.add('stale'); continue; }
      const sel = r.classList.contains('sel'), gp = r.classList.contains('gpFocus');
      if (r.innerHTML !== n.innerHTML) r.innerHTML = n.innerHTML;
      const want = n.className + (sel ? ' sel' : '') + (gp ? ' gpFocus' : '');
      if (r.className !== want) r.className = want;
    }
    let held = 0; for (const k of fresh.keys()) if (!shown.has(k)) held++;
    this.pauseShow(held);
  }
  private pauseShow(held: number) {
    const b = this.root.querySelector('.trkList .hd .lPause') as HTMLElement | null;
    if (!b) return;
    b.hidden = !this.listFrozen;
    const t = held ? `+${held}` : '';
    const n = b.querySelector('.n') as HTMLElement; if (n.textContent !== t) n.textContent = t;
    b.title = held ? `Paused while you scroll — ${held} new; press (or scroll to the top) to bring them in` : 'Paused while you scroll — press (or scroll to the top) to carry on';
    b.setAttribute('aria-label', b.title);
  }
  private listFreeze(on: boolean) {
    if (on === this.listFrozen) return;
    this.listFrozen = on;
    if (!on) {   // resume: the newest order, the held rows in
      this.lastRows = '';
      const html = this.rowsPending;
      if (html) { this.rowsEl.innerHTML = html; this.lastRows = html; if (this.sel) this.markSel(this.sel); }
      this.rowsCountWas = -1; requestAnimationFrame(() => this.cardsLayout());
    }
    this.pauseShow(0);
  }
  private rowsCountWas = -1;
  private listMaxH = 0;
  listMax = (() => { try { return localStorage.getItem('vibesdr.mapListMax') === '1'; } catch { return false; } })();
  setListMax(on: boolean) {
    if (on === this.listMax) return;
    const c = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
    const shown = !!c && !c.hidden && c.offsetHeight > 0;
    const t0 = shown ? c!.getBoundingClientRect().top : 0, h0 = shown ? c!.offsetHeight : 0;
    const apply = () => {
      this.listMax = on;
      try { localStorage.setItem('vibesdr.mapListMax', on ? '1' : '0'); } catch { /* private window */ }
      this.root.classList.toggle('lMax', on);
      const b = this.root.querySelector('.lMax') as HTMLElement | null;
      if (b) { b.setAttribute('aria-pressed', String(on)); b.setAttribute('aria-label', on ? 'Restore the list' : 'Maximise the list'); b.title = on ? 'Restore the list (M)' : 'Maximise the list (M)'; }
      this.cardsLayout();
      this.listMaxPause();
    };
    if (!shown) { apply(); return; }
    const rows = c!.querySelector('.rows') as HTMLElement | null, st = rows?.scrollTop ?? 0;
    if (on) {   // grow: take the new height at once, slide up from where its top was (FLIP — compositor only)
      apply();
      const d = t0 - c!.getBoundingClientRect().top;
      if (rows) rows.scrollTop = st;
      if (d > 1) { c!.style.transition = 'none'; c!.style.transform = `translateY(${d}px)`; void c!.offsetHeight; c!.style.transition = ''; c!.style.transform = ''; }
    } else {    // shrink: slide down by the difference, then take the new height
      const d = h0 - (this.listNormH || h0);
      if (d <= 1) { apply(); return; }
      c!.style.transform = `translateY(${d}px)`;
      let done = false;
      const fin = () => { if (done) return; done = true; c!.style.transition = 'none'; c!.style.transform = ''; apply(); if (rows) rows.scrollTop = st; void c!.offsetHeight; c!.style.transition = ''; };
      c!.addEventListener('transitionend', fin, { once: true }); setTimeout(fin, 320);
    }
  }
  private listMaxPause() { this.fitPauseForCards(); }   // ★ maximised counts as a card over the map: AUTO waits
  /* ★★★ V12 THE LIVE WATERFALL in the LIST's drawer (Stuart 11-27): a live map over a radio offers it (setWaterfall); the
   *  drawer shows EITHER the list OR the waterfall, swapped in place by the top-left tab — same size, no close / reopen.
   *  The host is told the moment the waterfall becomes visible (its pane, to draw into) and the moment it is not (null:
   *  the drawer shut, swapped back, stepped aside for a card, or the map gone) — the spectrum runs only in between. */
  private wfOffered = false;
  /** the drawer's head (bar, tabs, safety line) as last measured with the list showing */
  private listHead = 0;
  private pane: 'list' | 'wf' = 'list';
  private wfToldEl: HTMLElement | null = null;
  /** The host: the waterfall's pane while it is on screen, null as it goes. */
  onWaterfall: ((pane: HTMLElement | null) => void) | null = null;
  /** Offer the waterfall (a live map over a radio) or not (the viewer, a map with no radio): the tabs come and go. */
  setWaterfall(on: boolean) {
    if (on === this.wfOffered) return;
    this.wfOffered = on;
    for (const b of Array.from(this.root.querySelectorAll('.lSwap, .lSwapC')) as HTMLElement[]) b.hidden = !on;
    this.root.classList.toggle('wfOn', on);
    if (!on && this.pane === 'wf') this.setPane('list', false); else this.wfTell();
    this.layoutSig = ''; this.cardsLayout();
  }
  get waterfallShowing(): boolean { return !!this.wfToldEl; }
  /** The drawer opened onto the waterfall (the collapsed chip's ≋ tab, W). */
  waterfallOpen() {
    if (!this.wfOffered) return;
    if (this.listAside) this.closeTransients();
    this.setPane('wf', false);
    if (!this.isCardOpen('list')) this.cardOpen('list', true, true); else { this.layoutSig = ''; this.cardsLayout(); }
  }
  /** ★★ The data credits a licence asks for (the aircraft database's) — in HELP, under the chips' words, beside the map's own
   *  attribution; never in the LIST (Stuart 16:53). '' = none. The html is the host's own fixed markup. */
  setCredit(html: string) {
    const b = this.root.querySelector('.trkCard2[data-card="help"] .helpBody') as HTMLElement | null;
    if (!b) return;
    let c = b.querySelector('.hCredit') as HTMLElement | null;
    if (!html) { c?.remove(); return; }
    if (!c) { c = document.createElement('div'); c.className = 'hCredit'; b.appendChild(c); }
    if (c.innerHTML !== html) c.innerHTML = html;
  }
  /** The drawer's bottom bar in the waterfall view: what the waterfall needs (the dial frequency). */
  setWaterfallInfo(html: string) {
    const el = this.root.querySelector('.trkList .wfInfo') as HTMLElement | null;
    if (el && el.innerHTML !== html) el.innerHTML = html;
  }
  private setPane(p: 'list' | 'wf', relayout = true) {
    if (p === 'wf' && !this.wfOffered) p = 'list';
    this.pane = p;
    const card = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
    card?.setAttribute('data-pane', p);
    this.list?.setAttribute('data-pane', p);
    const sw = this.root.querySelector('.lSwap') as HTMLElement | null;
    if (sw) {
      sw.innerHTML = p === 'wf' ? LIST_SVG : WF_SVG;
      const w = p === 'wf' ? 'Show the list' : 'Show the live waterfall';
      sw.setAttribute('aria-label', w); sw.title = `${p === 'wf' ? 'The list' : 'Live waterfall'} (W)`;
    }
    const lm = this.root.querySelector('.lMax') as HTMLElement | null;
    if (lm) { const what = p === 'wf' ? 'waterfall' : 'list'; lm.setAttribute('aria-label', `${this.listMax ? 'Restore' : 'Maximise'} the ${what}`); }
    if (relayout) { this.layoutSig = ''; this.cardsLayout(); }
    this.wfTell();
  }
  private wfTell() {
    const pane = this.list?.querySelector('.wfPane') as HTMLElement | null;
    const on = this.wfOffered && this.pane === 'wf' && this.isCardOpen('list') && !this.listAside && !this.root.hidden && !!pane;
    const el = on ? pane : null;
    if (el === this.wfToldEl) return;
    this.wfToldEl = el;
    try { this.onWaterfall?.(el); } catch { /* the host's fault is its own */ }
  }
  private lpFired = false;
  private listAside = false;
  private listStepAside() {
    const list = this.root.querySelector('.trkCard2[data-card="list"]') as HTMLElement | null;
    const dock = this.root.querySelector('.mDock[data-dock="list"]') as HTMLElement | null;
    if (!list || !dock) return;
    const listOpen = !list.hidden && list.parentElement?.classList.contains('sideB');
    const r = (c: HTMLElement) => { const p = (c.parentElement as HTMLElement).getBoundingClientRect(); return { l: p.left + c.offsetLeft, t: p.top + c.offsetTop, r: p.left + c.offsetLeft + c.offsetWidth, b: p.top + c.offsetTop + c.offsetHeight }; };
    let aside = false;
    if (listOpen) {
      const L = r(list);
      for (const c of Array.from(this.cardsEl.querySelectorAll('.sideL > .trkCard2, .sideR > .trkCard2, .sideH > .trkCard2')) as HTMLElement[]) {
        if (c.hidden) continue;
        const T = r(c);
        if (T.l < L.r - 1 && T.r > L.l + 1 && T.t < L.b - 1 && T.b > L.t + 1) { aside = true; break; }
      }
    }
    if (aside === this.listAside) return;
    this.listAside = aside;
    list.classList.toggle('stepAside', aside);
    list.setAttribute('aria-hidden', String(aside));
    list.toggleAttribute('inert', aside);
    dock.classList.toggle('aside', aside);
    dock.title = aside ? 'The list is tucked away while that card is open — press to bring it back' : '';
    this.wfTell();
  }
  /** ★★★ tweak 3 the LAYERS chip: a glyph and a count per layer present ([plane] 157 [ship] 3 [NAVTEX] 40); a slim column
   *  on a phone-width window (each glyph over its count). Entries: the host's (setLayerSummary), else the SHOW toggles'. */
  private lsumSig = '';
  private lsumHost = false;
  setLayerSummary(entries: Array<{ glyph: LayerGlyph; n: number; colour: string; words: string }>) {
    this.lsumHost = true;
    this.drawLayerSummary(entries);
  }
  private drawLayerSummary(entries: Array<{ glyph: LayerGlyph; n: number; colour: string; words: string }>) {
    const sig = JSON.stringify(entries);
    if (sig === this.lsumSig) return;
    this.lsumSig = sig;
    const el = this.root.querySelector('.mDock[data-dock="layers"] .lsum') as HTMLElement;
    el.innerHTML = entries.map((e) => `<span class="le" title="${esc(e.words)}"><span class="lg" style="color:${esc(e.colour)}">${LAYER_GLYPH[e.glyph] ?? LAYER_GLYPH.dot}</span><span class="ln">${esc(e.n)}</span></span>`).join('');
    const b = this.root.querySelector('[data-cbtn="layers"]') as HTMLElement;
    b.setAttribute('aria-label', `Layers — ${entries.map((e) => e.words).join(', ') || 'what the map shows'}`);
    // ★ its tucked tab: the main family's glyph in its colour (the aircraft where there are aircraft)
    const ti = this.root.querySelector('.mDock[data-dock="layers"] .hTabIco') as HTMLElement | null;
    if (ti) { const f = entries[0]; ti.innerHTML = LAYER_GLYPH[f?.glyph ?? 'aircraft'] ?? LAYER_GLYPH.dot; ti.style.color = f?.colour ?? ''; }
    this.cardsLayout();   // ★ tweak 5: measure and anchor only — an open card stays put (layoutSig untouched)
  }
  /** The live rate, top right ("ADS-B 1,425 msg/s" · "VDL2 153 msg/min") — '' = none (a viewer, nothing live). */
  setRate(text: string, title = '') {
    const el = this.root.querySelector('.trkTop .rate') as HTMLElement;
    if (el.textContent !== text) el.textContent = text;
    el.title = title;
    if (!this.rateRowsHost) this.setRateRows(text ? [{ name: text.replace(/\s+[\d.,]+\s*(msg\/min|\/s|msg\/s|\/min)$/, ''), rate: (/[\d.,]+\s*(msg\/min|\/s|msg\/s|\/min)$/.exec(text)?.[0] ?? '').replace('msg/', '/'), live: true, words: title }] : [], false);
  }
  /* ★★★ Stuart 12:52 THE RATE CHIP, top-right: the main live rate ("VDL2 153/min", "ADS-B 1,425/s"; a phone shows only
   *  the number and unit). ONE source: a static reading, not a button. SEVERAL (this radio, a PiAware feed …): the main
   *  one, "+N" and a ⌄ — a press grows a card down out of it, a row per source with its rate and live / stalled, updated
   *  in place. Monitoring only: showing and hiding a source stays in MAP → SOURCES. */
  private rateStatic = true;
  private rateRowsHost = false;
  setRateRows(rows: Array<{ name: string; rate: string; live: boolean; words?: string; mine?: boolean }>, host = true) {
    if (host) this.rateRowsHost = true;
    const d = this.root.querySelector('.mDock[data-dock="rate"]') as HTMLElement | null;
    if (!d) return;
    this.cardsAvail('rate', rows.length > 0);
    if (!rows.length) { if (this.isCardOpen('rate')) this.cardOpen('rate', false); return; }
    const many = rows.length > 1;
    this.rateStatic = !many;
    const b = d.querySelector('.mchip') as HTMLButtonElement;
    d.classList.toggle('static', !many);
    if (!many) { b.setAttribute('aria-disabled', 'true'); b.tabIndex = -1; b.removeAttribute('aria-expanded'); if (this.isCardOpen('rate')) this.cardOpen('rate', false); }
    else { b.removeAttribute('aria-disabled'); b.tabIndex = 0; b.setAttribute('aria-expanded', String(this.isCardOpen('rate'))); }
    const m = rows[0];
    const set = (q: string, t: string) => { const e = d.querySelector(q) as HTMLElement; if (e.textContent !== t) e.textContent = t; };
    set('.rn', m.name ? m.name + ' ' : ''); set('.ru', m.rate); set('.rmore', many ? `+${rows.length - 1}` : '');
    const words = rows.map((r) => `${r.name} ${r.rate}${r.live ? '' : ' (stalled)'}`).join(' \u00b7 ');
    b.title = many ? `${words} \u2014 tap for each source` : (m.words || words);
    b.setAttribute('aria-label', many ? `Message rate: ${words}. Open the rate card` : `Message rate: ${words}`);
    // the card: a row per source, written in place (keyed by position + name)
    const body = this.root.querySelector('.trkCard2[data-card="rate"] .rateBody') as HTMLElement | null;
    if (!body) return;
    while (body.children.length > rows.length) body.lastElementChild!.remove();
    rows.forEach((r, i) => {
      let row = body.children[i] as HTMLElement | undefined;
      if (!row) { row = document.createElement('div'); row.className = 'mRow rateRow'; row.setAttribute('role', 'listitem'); row.innerHTML = '<span class="dot"></span><span class="nm"></span><span class="v"></span>'; body.appendChild(row); }
      row.classList.toggle('down', !r.live);
      row.classList.toggle('mine', !!r.mine);   // ★ filled dot = this radio, outlined = another source (as the LIVE badges)
      const nm = row.querySelector('.nm') as HTMLElement, v = row.querySelector('.v') as HTMLElement;
      if (nm.textContent !== r.name) nm.textContent = r.name;
      const vt = r.live ? r.rate : `${r.rate} \u00b7 stalled`;
      if (v.textContent !== vt) v.textContent = vt;
      row.title = r.words || '';
    });
  }
  /** ★★★ Stuart 12:50 the CHAT chip (right edge, under RATE): shown while the dial is shared; its unread count; a gentle
   *  single pulse when a new message lands. Its card is filled by the host (onCard) — the canned replies, no typing. */
  private chatUnread = 0;
  setChat(o: { show: boolean; disabled?: boolean; unread?: number; why?: string }) {
    const d = this.root.querySelector('.mDock[data-dock="chat"]') as HTMLElement | null;
    if (!d) return;
    this.cardsAvail('chat', o.show);
    if (!o.show && this.isCardOpen('chat')) this.cardOpen('chat', false);
    const b = d.querySelector('.mchip') as HTMLButtonElement;
    if (o.disabled !== undefined) { d.classList.toggle('off', o.disabled); if (o.disabled) b.setAttribute('aria-disabled', 'true'); else b.removeAttribute('aria-disabled'); }
    const n = Math.max(0, o.unread ?? this.chatUnread), u = d.querySelector('.unr') as HTMLElement;
    u.hidden = !n; if (u.textContent !== String(n)) u.textContent = n > 99 ? '99+' : String(n);
    if (n > this.chatUnread) { d.classList.remove('pulse'); void d.offsetWidth; d.classList.add('pulse'); setTimeout(() => d.classList.remove('pulse'), 1300); }
    this.chatUnread = n;
    b.title = b.getAttribute('aria-disabled') === 'true' ? (o.why || 'Chat is closed') : n ? `Chat \u2014 ${n} new` : 'Chat';
    b.setAttribute('aria-label', `CHAT \u2014 ${n ? n + ' new message' + (n === 1 ? '' : 's') + '. ' : ''}open the chat`);
  }
  /* ★★★ Stuart 14:20 THE STATS CARD: per MODE (ADS-B, HFDL, VDL2 …) — each its own NEWEST, FURTHEST, FURTHEST TRACKED
   *  (distance) and LONGEST TRACKED (time) — so an HFDL aircraft followed 3 000 NM never competes with ADS-B's. A tab per mode present; a row
   *  per stat; a press shows that aircraft (selected, its card open, the map moved to it). Rewritten only when it changes. */
  private statsSig = '';
  private statsMode = '';
  private statsPick: ((key: string) => void) | null = null;
  setStats(sections: Array<{ mode: string; rows: Array<{ key: string; label: string; value: string; name: string; lat?: number; lon?: number }> }>, onPick?: (key: string) => void) {
    const live = sections.filter((x) => x.rows.length);
    this.cardsAvail('stats', live.length > 0);
    if (onPick) this.statsPick = onPick;
    if (!live.length) return;
    if (!live.some((x) => x.mode === this.statsMode)) this.statsMode = live[0].mode;
    const sig = JSON.stringify([live, this.statsMode]);
    if (sig === this.statsSig) return;
    this.statsSig = sig;
    const tabs = this.root.querySelector('.trkCard2[data-card="stats"] .stTabs') as HTMLElement, body = this.root.querySelector('.trkCard2[data-card="stats"] .stBody') as HTMLElement;
    tabs.hidden = live.length < 2;
    tabs.innerHTML = live.map((x) => `<button type="button" class="stTab" role="tab" data-st="${esc(x.mode)}" aria-selected="${x.mode === this.statsMode}">${esc(x.mode)}</button>`).join('');
    for (const b of Array.from(tabs.querySelectorAll('[data-st]')) as HTMLElement[]) b.onclick = (e) => { e.stopPropagation(); this.statsMode = b.dataset.st || ''; this.statsSig = ''; this.setStats(live); this.idleArm(); };
    const sec = live.find((x) => x.mode === this.statsMode)!;
    body.innerHTML = sec.rows.map((r) => `<button type="button" class="stRow" role="listitem" data-key="${esc(r.key)}"${r.lat != null ? ` data-lat="${r.lat}" data-lon="${r.lon}"` : ''}><span class="k">${esc(r.label)}</span><span class="v">${esc(r.value)}</span><span class="n">${esc(r.name)}</span></button>`).join('');
    for (const b of Array.from(body.querySelectorAll('.stRow')) as HTMLElement[]) b.onclick = (e) => {
      e.stopPropagation();
      const k = b.dataset.key || '';
      this.select(k);
      const lat = Number(b.dataset.lat), lon = Number(b.dataset.lon);
      const m = (this.vm as any)?.map || (this.vm as any)?._gl || (this.vm as any)?.gl;
      if (m?.easeTo && isFinite(lat) && isFinite(lon)) m.easeTo({ center: [lon, lat], duration: 600 });
      this.statsPick?.(k);
      this.closeTransients();
    };
  }
  /** ★★ Stuart 14:38 the TIME LIMIT on the map (the radio page's #rxTimeLeft, mirrored): shown only while a limit applies. */
  /** The session clock (SessionClock's words): `leftS` seconds, or null for no limit; `soft` a guarantee, not a sentence. */
  private timeUser: 'shown' | 'tucked' | null = null;
  setTimeLeft(leftS: number | null, soft: boolean) {
    const d = this.root.querySelector('[data-dock="time"]') as HTMLElement | null;
    if (!d) return;
    if (!d.dataset.wired) {
      d.dataset.wired = '1';
      wireEdgeDockT(d, 'right', () => { this.timeUser = 'tucked'; this.setTimeLeft(this.timeLast.left, this.timeLast.soft); }, () => { this.timeUser = 'shown'; this.setTimeLeft(this.timeLast.left, this.timeLast.soft); });
    }
    this.timeLast = { left: leftS, soft };
    d.hidden = leftS == null;
    if (leftS == null) return;
    // the stage, as the app's: 2 = under 30 s, 1 = under 2 min; a soft limit never reaches red
    const stage = soft ? (leftS > 0 && leftS < 120 ? 1 : 0) : leftS < 30 ? 2 : leftS < 120 ? 1 : 0;
    const cap = soft ? (leftS <= 0 ? 'GUARANTEED TIME OVER' : 'GUARANTEED TIME ENDS IN') : 'YOUR TURN ENDS IN';
    const num = soft && leftS <= 0 ? 'yours until someone else wants it' : `${Math.floor(leftS / 60)}:${String(Math.floor(leftS % 60)).padStart(2, '0')}`;
    const c = d.querySelector('.tCap') as HTMLElement, n = d.querySelector('.tNum') as HTMLElement;
    if (c.textContent !== cap) c.textContent = cap;
    if (n.textContent !== num) n.textContent = num;
    n.classList.toggle('soft', soft && leftS <= 0);
    d.dataset.stage = String(stage);
    /* ★★ Stuart 17:1x ("the time chip doesn't collapse anymore"): the listener's flick ALWAYS wins until the NEXT stage —
     *  the app's SDRScreen timeOut rule: a new, more urgent stage brings the card out once; a › then tucks it for good */
    if (stage !== this.timeStage) { if (this.timeStage >= 0 && stage > this.timeStage) this.timeUser = null; this.timeStage = stage; }
    // shown as the app decides: urgency brings it out; otherwise it waits as its tab — the listener's own flick wins
    const out = this.timeUser ? this.timeUser === 'shown' : stage >= 1;
    edgeDockSetT(d, out);
  }
  private timeLast: { left: number | null; soft: boolean } = { left: null, soft: false };
  private timeStage = -1;
  /** The right column, where the page's own chips (SERVER HEALTH) are lent while the map is up. */
  rightColumn(): HTMLElement { return this.root.querySelector('.mChipsR') as HTMLElement; }
  /** ★★ THE LAYOUT REGISTRY (Stuart 15:04): every piece of chrome over the map — the corner pieces, the edge chips, the
   *  lent SERVER HEALTH and badge, the bar, the zoom, the ?, the scale and any open card — as screen rectangles. The
   *  aircraft card's clamp (adsblayer opts.obstacles) and the overlap e2e read the same list, so a new chip is avoided
   *  without anyone remembering to add it. */
  /** ★★★ V12 viewport-fit=cover WHILE A MAP IS UP (Stuart 11-27: black bars either side in iPhone landscape): the map runs
   *  under the rounded corners and the island, its controls padded in by the insets (the --sai-* vars). Only while a map
   *  is up: the radio page's own chrome has not had that safe-area pass, so it keeps Safari's letterbox. */
  private static coverWas: string | null = null;
  static coverOn(on: boolean) {
    const m = document.querySelector('meta[name="viewport"]') as HTMLMetaElement | null;
    if (!m) return;
    if (on) {
      if (TrackView.coverWas === null) TrackView.coverWas = m.content;
      if (!/viewport-fit\s*=\s*cover/.test(m.content)) m.content = `${TrackView.coverWas.replace(/,?\s*viewport-fit\s*=\s*\w+/, '')},viewport-fit=cover`;
    } else if (TrackView.coverWas !== null && !document.querySelector('.trkView:not([hidden])')) { m.content = TrackView.coverWas; TrackView.coverWas = null; }
  }
  /** ★★ Stuart 18:4x (iPhone, island left: "a strip on the RIGHT shows the radio page"): switching viewport-fit=cover on
   *  while the page is up widens the layout viewport, and Safari can keep a fixed box at its OLD width until something
   *  re-lays it out. The map's box is sized from the window itself, now and on every resize / rotation — edge to edge;
   *  only the CONTROLS stand in by the insets. */
  private fitWired = false;
  private fitViewport() {
    const fit = () => {
      if (this.root.hidden) return;
      const w = Math.max(window.innerWidth || 0, document.documentElement.clientWidth || 0);
      const v = `${w}px`; if (this.root.style.width !== v) { this.root.style.width = v; this.root.style.right = 'auto'; }
    };
    fit(); requestAnimationFrame(fit); setTimeout(fit, 250); setTimeout(fit, 800);
    if (!this.fitWired) {
      this.fitWired = true;
      window.addEventListener('resize', fit); window.addEventListener('orientationchange', () => setTimeout(fit, 300));
      window.visualViewport?.addEventListener('resize', fit);
    }
  }
  static CHROME_SEL = '.mDock:not([hidden]) .hCard, .trkTop .exit, #mapShLine, .maplibregl-ctrl-top-left, .leaflet-top.leaflet-left, .mHelp, .mChipsScale, #srvHealth .hCard, #rxBadge, .trkCards .trkCard2:not([hidden])';
  chromeRects(): Array<{ l: number; t: number; r: number; b: number; w?: number }> {
    const out: Array<{ l: number; t: number; r: number; b: number; w?: number }> = [];
    for (const e of Array.from(this.root.querySelectorAll(TrackView.CHROME_SEL)) as HTMLElement[]) {
      if (!e.getClientRects().length || getComputedStyle(e).visibility === 'hidden') continue;
      const q = e.getBoundingClientRect(); if (q.width < 2 || q.height < 2) continue;
      // ★ w: how much covering it matters — the drawer (a big surface the card may lie over) far less than a control
      out.push({ l: q.left, t: q.top, r: q.right, b: q.bottom, w: e.matches('.trkCard2[data-card="list"]') ? 0.15 : 1 });
    }
    return out;
  }
  chatHost(): HTMLElement { return this.root.querySelector('.trkCard2[data-card="chat"] .chatBody') as HTMLElement; }
  /** The facts the old status paragraph carried, each in the card it belongs to ('' = none). */
  setFacts(f: Partial<Record<'list' | 'tune' | 'layers', string>>) {
    this.hostFacts = true;
    for (const k of ['list', 'tune', 'layers'] as const) {
      const el = this.root.querySelector(`.trkCard2[data-card="${k}"] .facts`) as HTMLElement | null;
      const t = f[k] ?? '';
      if (el && el.textContent !== t) el.textContent = t;
    }
  }
  private hostFacts = false;
  /** The MAP card's aircraft note ("Furthest: RYR12K 213 NM SE") — '' = none. */
  setFarNote(t: string) { const el = this.root.querySelector('.farNote') as HTMLElement; if (el.textContent !== t) el.textContent = t; }
  private srcSig = '';
  /** ★★★ tweak 3 SOURCES, in MAP CONTROLS: each radio and feed and what it adds ("PiAware 127", "RTL-SDR 36"), each row a
   *  show / hide toggle for what ONLY it heard (this viewer's own view, never sent). null / one source = no section. */
  setSources(rows: Array<{ id: string; name: string; what: string; value: string; on: boolean; down?: boolean }> | null, onToggle?: (id: string, on: boolean) => void) {
    const sect = this.root.querySelector('.srcSect') as HTMLElement;
    sect.hidden = !rows || rows.length < 2;
    if (onToggle) this.onSource = onToggle;
    const sig = JSON.stringify(rows);
    if (sig === this.srcSig) return;
    this.srcSig = sig;
    const body = this.root.querySelector('.srcBody') as HTMLElement;
    // ★★★ Stuart 11:59 ("the sources look like windows 95"): the LAYERS rows' own look — one row style, .mRow
    body.innerHTML = (rows ?? []).map((x) => `<button type="button" class="mRow srcRow${x.down ? ' down' : ''}" data-src="${esc(x.id)}" aria-pressed="${x.on}" title="${esc(x.on ? 'Shown — tap to hide what only this source heard' : 'Hidden — tap to show it again')}">`
      + `<span class="dot" style="background:${x.id === 'feed' ? '#ffd34d' : '#7ee787'}"></span><span class="nm"><b>${esc(x.name)}</b><small>${esc(x.what)}</small></span><span class="n">${esc(x.value)}</span></button>`).join('');
    for (const b of Array.from(body.querySelectorAll('[data-src]')) as HTMLButtonElement[]) b.onclick = () => this.onSource?.(b.dataset.src || '', b.getAttribute('aria-pressed') !== 'true');
  }
  private onSource: ((id: string, on: boolean) => void) | null = null;
  private srvSig2 = '';
  /** ★★★ tweak 3 THE SERVERS chip (Stuart 11-27: "must ALWAYS be reachable on every map screen, even when EXIT is hidden or
   *  the mode can't be left"): back out to this server's radios / landing, or over to another radio on it. */
  setServers(rows: Array<{ id: string; label: string; sub: string }>, onPick: (id: string) => void) {
    this.onServer = onPick;
    this.cardsAvail('servers', rows.length > 0);
    const sig = JSON.stringify(rows);
    if (sig === this.srvSig2) return;
    this.srvSig2 = sig;
    const body = this.root.querySelector('.srvBody') as HTMLElement;
    body.innerHTML = rows.map((x) => `<button type="button" class="srvPick" data-srv="${esc(x.id)}"><b>${x.id === 'back' ? '‹ ' : ''}${esc(x.label)}</b><small>${esc(x.sub)}</small></button>`).join('');
    for (const b of Array.from(body.querySelectorAll('[data-srv]')) as HTMLButtonElement[]) b.onclick = () => { this.cardOpen('servers', false); this.onServer?.(b.dataset.srv || ''); };
    this.cardsLayout();
  }
  private onServer: ((id: string) => void) | null = null;
  /* ── ★★★ beta96 FIT TO ONE LAYER (Stuart 11-27: "with this much data the fit button doesnt work, maybe the fit button
   *  fits to the adsb layer or user chosen layer") — the shared auto fit and FIT frame ONE layer (ADS-B by default where
   *  there is ADS-B, else the map's main layer; the viewer's choice remembered), and drop the furthest few per cent (one
   *  aircraft at Tallinn must not zoom the whole UK view out). Never greyed while the layer has anything. ── */
  private fitGroups: Array<{ id: string; label: string; n: number }> = [];
  private fitPick = '';
  private fitDefault = '';
  private fitSig = '';
  setFitGroups(groups: Array<{ id: string; label: string; n: number }>, def: string) {
    this.fitGroups = groups; this.fitDefault = def;
    if (!this.fitPick) { try { this.fitPick = localStorage.getItem(FIT_KEY) || ''; } catch { /* private */ } }
    const host = this.root.querySelector('.fitSel') as HTMLElement, hd = this.root.querySelector('.fitToH') as HTMLElement;
    host.hidden = hd.hidden = groups.length < 2;
    const cur = this.fitChoice();
    const sig = JSON.stringify([groups.map((g) => g.id), cur]);
    if (sig !== this.fitSig) {
      this.fitSig = sig;
      host.innerHTML = groups.map((g) => `<button class="btn chip" type="button" role="radio" data-fit="${esc(g.id)}" aria-checked="${g.id === cur}" aria-pressed="${g.id === cur}">${esc(g.label)}<span class="n"></span></button>`).join('');
      for (const b of Array.from(host.querySelectorAll('[data-fit]')) as HTMLButtonElement[]) b.onclick = () => {
        this.fitPick = b.dataset.fit || '';
        try { localStorage.setItem(FIT_KEY, this.fitPick); } catch { /* private */ }
        this.fitSig = ''; this.setFitGroups(this.fitGroups, this.fitDefault);
        this.fitOnce();
      };
    }
    for (const b of Array.from(host.querySelectorAll('[data-fit]')) as HTMLElement[]) {
      const n = b.querySelector('.n') as HTMLElement, g = groups.find((x) => x.id === b.dataset.fit);
      const t = g ? String(g.n) : ''; if (n.textContent !== t) n.textContent = t;
    }
    this.fitTitles();
  }
  /** The layer FIT and AUTO frame now: the viewer's pick while that layer is on the map, else the default, else the first. */
  fitChoice(): string {
    const ids = this.fitGroups.map((g) => g.id);
    return ids.includes(this.fitPick) ? this.fitPick : ids.includes(this.fitDefault) ? this.fitDefault : ids[0] || '';
  }
  private fitTitles() {
    const g = this.fitGroups.find((x) => x.id === this.fitChoice());
    const b = this.root.querySelector('.fitOnce') as HTMLButtonElement;
    const what = g ? g.label : this.o.listTitle.toLowerCase();
    b.title = g && g.n === 0 ? `Nothing on the map from ${what} yet — FIT will frame it once there is` : `Fit the map to ${what} — far outliers (the furthest 5 %) are left out, so one far-off aircraft does not zoom the whole view out`;
    const note = this.root.querySelector('.fitNote') as HTMLElement;
    const t = this.fitGroups.length ? `AUTO and FIT frame ${what}, leaving out far outliers.` : '';
    if (note.textContent !== t) note.textContent = t;
  }
  /** The chosen layer's points, the furthest few per cent dropped (from the receiver, else from the middle of them). */
  private fitPoints(all: Array<{ lat: number; lon: number; g: string }>): [number, number][] {
    const ch = this.fitChoice();
    const pts = this.fitGroups.length ? all.filter((p) => p.g === ch) : all;
    return trimOutliers(pts, this.rx).map((p) => [p.lat, p.lon] as [number, number]);
  }
  private fitAll: Array<{ lat: number; lon: number; g: string }> = [];
  /** FIT pressed: the auto fit frames the chosen layer now (and AUTO, if it was paused, resumes on it). */
  fitOnce() {
    this.refit = false;
    try {
      if (this.af) { this.af.fitNow(); return; }   // ★ frames it now; AUTO (on, or paused by a touch) carries on from there
      this.layer?.fit?.();
    } catch (e) { console.error('[tracks] fit', e); }
  }
  get open() { return !this.root.hidden; }
  /** ★★ beta96 a host's own chrome in this view (ADS-B mode's GAIN, CHAT, capture warning; its list chips; its feed
   *  warning and gain pop-over): 'top' = in the top bar before the status line, 'head' = under the list's header, 'over' =
   *  over the map. Made on first use; the host moves its elements in and hides them when they do not apply. */
  slot(where: 'top' | 'head' | 'over' | 'share' | 'tune' | 'ctl'): HTMLElement {
    if (where === 'tune' || where === 'ctl') return this.root.querySelector(`.slot-${where}`) as HTMLElement;   // ★ beta96 in the TUNE / MAP cards
    const cls = `slot-${where}`;
    let el = this.root.querySelector(`.${cls}`) as HTMLElement | null;
    if (el) return el;
    el = document.createElement(where === 'top' || where === 'share' ? 'span' : 'div');
    el.className = `trkSlot ${cls}`;
    // ★★★ beta96 'share' = the SHARED TUNER line + CHAT, right after the title: first in the sideways row, so a phone
    //   shows it without scrolling (main.ts mapSharedSync)
    if (where === 'share') (this.root.querySelector('.ttl') as HTMLElement).after(el);
    else if (where === 'top') (this.root.querySelector('.trkTop .rate') as HTMLElement).before(el);   // ★ CHAT before the rate
    else if (where === 'head') this.rowsEl.before(el);
    else this.mapHost.appendChild(el);
    return el;
  }
  private tglSig = '';
  /** ★★ V12 2026-11-02 the layer toggles, redrawn (only when they changed — a poll must not steal a focused button). */
  setToggles(toggles: Array<{ id: string; label: string; words: string; group?: string; live?: boolean; n?: number; colour?: string; others?: string[] }>, toggled: Record<string, boolean>) {
    const host = this.root.querySelector('.tgl') as HTMLElement;
    // ★★★ beta96 SHOW (Stuart 11-26): the counts change every second — written in place (below), never a redraw, so a poll
    //   cannot steal a focused chip
    const counts = () => {
      const by = new Map(toggles.map((x) => [x.id, x.n]));
      for (const b of Array.from(host.querySelectorAll('[data-tgl]')) as HTMLElement[]) {
        const n = b.querySelector('.n') as HTMLElement | null; if (!n) continue;
        const v = by.get(b.dataset.tgl || ''); const t = v != null ? String(v) : '';
        if (n.textContent !== t) n.textContent = t;
      }
    };
    const sig = JSON.stringify([toggles.map((x) => [x.id, x.group || '', !!x.live, x.n != null, x.colour || '', (x.others || []).join('|')]), toggles.map((x) => toggled[x.id] !== false)]);
    if (sig === this.tglSig) { counts(); if (!this.lsumHost) this.drawLayerSummary(toggles.filter((x) => x.n != null && toggled[x.id] !== false).map((x) => ({ glyph: glyphOf(x.id), n: x.n!, colour: x.colour || '#9fb0c2', words: `${x.label} ${x.n}` }))); return; }
    this.tglSig = sig;
    host.hidden = !toggles.length;
    this.viewSync();
    // ★★ beta96 THE LIVE MAPS (liveMaps.ts liveChips): a group's name before its first chip, a red LIVE dot on the live
    //   layer's (what the receiver is doing now). ★★★ SHOW (Stuart 11-26): THIS viewer's own filter, labelled, the community
    //   map's chips — a colour dot, the name, its count; nothing here is ever sent to the server.
    // ★ Stuart 15:27: LIVE tells the live layers from the stored / other radios' ones — only when the card shows both
    /* ★★ Stuart 15:31 / 15:33: three states, as small as the TUNE card's tag — LIVE filled = THIS radio decodes it; LIVE
     *  outlined = another radio or feed here does (its name in the title, or the name itself when ≤ 5 letters); nothing
     *  = stored. Shown only when the rows differ — all alike, there is nothing to tell apart. */
    const stateOf = (x: { live?: boolean; others?: string[] }) => (x.live ? 'mine' : x.others?.length ? 'other' : 'stored');
    const liveMixed = new Set(toggles.map(stateOf)).size > 1;
    const badge = (x: { live?: boolean; others?: string[] }) => {
      if (!liveMixed) return '';
      if (x.live) return '<span class="lv" title="This radio is decoding it now">LIVE</span>';
      if (!x.others?.length) return '';
      const one = x.others.length === 1 && x.others[0].length <= 5 ? x.others[0].toUpperCase() : 'LIVE';
      return `<span class="lv o" title="Live on ${esc(x.others.join(', '))} — another of this server's sources">${esc(one)}</span>`;
    };
    host.innerHTML = (toggles.length ? '<span class="lbl" title="What this map shows you — your own view, nothing changes on the radio">SHOW</span>' : '')
      + toggles.map((x) => (x.group ? `<span class="grp" aria-hidden="true">${esc(x.group)}</span>` : '')
      + `<button class="btn chip${x.live ? ' live' : ''}" type="button" data-tgl="${esc(x.id)}" aria-pressed="${toggled[x.id] !== false}" aria-label="${esc(x.words)}" title="${esc(x.words)}">`
      + (x.colour ? `<span class="dot" style="background:${esc(x.colour)};color:${esc(x.colour)}"></span>` : '')
      + `${esc(x.label)}${badge(x)}<span class="n"></span></button>`).join('');
    counts();
    if (!this.lsumHost) this.drawLayerSummary(toggles.filter((x) => x.n != null && toggled[x.id] !== false).map((x) => ({ glyph: glyphOf(x.id), n: x.n!, colour: x.colour || '#9fb0c2', words: `${x.label} ${x.n}` })));
    for (const b of Array.from(host.querySelectorAll('[data-tgl]')) as HTMLButtonElement[]) {
      b.onclick = () => {
        const on = b.getAttribute('aria-pressed') !== 'true';
        b.setAttribute('aria-pressed', String(on));
        this.o.onToggle?.(b.dataset.tgl || '', on);
      };
    }
  }
  private scopeSig = '';
  /** ★★★ beta96 OTHER RADIOS ON THIS SERVER (mapScope.ts): a SHOW chip of its own after the layer chips — this viewer's view,
   *  never sent, never dial-locked. null = none (one source, the viewer). */
  setScope(chip: { label: string; words: string; on: boolean } | null, onToggle: (on: boolean) => void) {
    const host = this.root.querySelector('.trkCard2 .scope') as HTMLElement;
    host.hidden = !chip;
    (this.root.querySelector('.scopeSect') as HTMLElement).hidden = !chip;
    this.viewSync();
    const sig = chip ? JSON.stringify(chip) : '';
    if (sig !== this.scopeSig) {
      this.scopeSig = sig;
      host.innerHTML = chip ? `<button class="btn chip" type="button" data-scope="others" aria-pressed="${chip.on}" aria-label="${esc(chip.words)}" title="${esc(chip.words)}">${esc(chip.label)}</button>` : '';
    }
    const b = host.querySelector('[data-scope]') as HTMLButtonElement | null;
    if (b) b.onclick = () => onToggle(b.getAttribute('aria-pressed') !== 'true');
  }
  /** ★★★ beta96 what the scope hides, under the status line ('' = nothing hidden). */
  setScopeNote(words: string) {
    const el = this.root.querySelector('.scopeNote2') as HTMLElement;
    if (el.textContent !== words) el.textContent = words;
    // ★ the VIEW button says a filter is on (its words are in the card)
    (this.root.querySelector('[data-cbtn="layers"]') as HTMLElement).classList.toggle('filtered', !!words);
  }
  private tuneSig = '';
  private tuneRows: Array<{ id: string; name: string; freq: string; icon: string; group: string; live: boolean; why: string }> = [];
  /** ★★★ beta96 TUNE (Stuart 11-26): ONE collapsible chip in the top bar — "TUNE · VDL2 136.700–136.975 MHz ▾", the live
   *  mode — that opens a LIST of the whole-radio modes this radio can run (airSeaTune.ts: the server's own tables, grouped,
   *  an icon, the name, the frequencies; greyed with the server's reason where it cannot run). The same chip on every width.
   *  A row picked → onTune (the host tunes, under the ask / lock rules), the list shuts. The chip carries data-dial, so the
   *  dial lock (setDialLock) greys it and every row with its reason. [] = no chip (no radio behind the map). Touch, mouse,
   *  keyboard and the pad: the chip and every row are buttons; ↑ ↓ move between rows, Escape shuts the list. */
  setTune(rows: Array<{ id: string; name: string; freq: string; icon: string; group: string; live: boolean; why: string }>, chip: string, onTune: (id: string) => void) {
    /* ★★★ beta96 (map cards, Stuart 11-27) the TUNE list is the TUNE CARD's body now: its button in the bar, the rows in
     *  the card (the same rows, the same lock and refusal rules); `chip` is the live mode's words, the button's title.
     *  Opening the card moves nothing — the ROWS carry data-dial. */
    const pop = this.root.querySelector('.tunePop') as HTMLElement;
    if (!pop.dataset.wired) {
      pop.dataset.wired = '1';
      pop.addEventListener('keydown', (e) => {
        const items = Array.from(pop.querySelectorAll('[data-tune]')) as HTMLElement[];
        const i = items.indexOf(document.activeElement as HTMLElement);
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); items[(i + (e.key === 'ArrowDown' ? 1 : items.length - 1)) % items.length]?.focus(); }
      });
    }
    this.onTune = onTune;
    this.tuneRows = rows;
    const btn = this.root.querySelector('[data-cbtn="tune"]') as HTMLButtonElement;
    btn.title = chip ? `${chip} — tune the radio (for everybody listening on a shared dial)` : 'Tune the radio';
    const sig = JSON.stringify(rows);
    if (sig !== this.tuneSig) {
      this.tuneSig = sig;
      let last = '';
      pop.innerHTML = rows.map((r) => {
        const head = r.group !== last ? `<div class="tpG" role="presentation">${esc(r.group)}</div>` : '';
        last = r.group;
        return head + `<button type="button" class="tpRow${r.live ? ' live' : ''}" role="menuitemradio" aria-checked="${r.live}" data-tune="${esc(r.id)}" data-dial="1"`
          + `${r.why ? ` aria-disabled="true" data-why="${esc(r.why)}" title="${esc(r.why)}"` : ''}>`
          + `<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true"><path d="${esc(r.icon)}"/></svg>`
          + `<span class="nm">${esc(r.name)}</span><span class="fq">${esc(r.freq)}</span>${r.live ? '<span class="lv">LIVE</span>' : ''}</button>`;
      }).join('');
      for (const b of Array.from(pop.querySelectorAll('[data-tune]')) as HTMLButtonElement[]) {
        b.onclick = () => {
          if (b.dataset.why) { this.o.onTuneRefused?.(b.dataset.why); return; }
          if (b.getAttribute('aria-checked') !== 'true') this.onTune?.(b.dataset.tune || '');
          this.cardOpen('tune', false);   // ★ tweak 5: its action done, TUNE folds back
        };
      }
      if (this.dialWhy) this.setDialLock(this.dialWhy, this.dialExit);   // ★ new rows: the lock goes back on them
    }
    pop.hidden = !rows.length;
    this.tuneSync();
  }
  /** TUNE is offered when it has rows or the host put a control in it (ADS-B's GAIN) — never an empty card. */
  tuneSync() {
    const slot = this.root.querySelector('.slot-tune') as HTMLElement;
    const any = this.tuneRows.length > 0 || Array.from(slot.children).some((c) => !(c as HTMLElement).hidden);
    if (any !== this.cardAvail.has('tune')) { this.cardsAvail('tune', any); this.cardsLayout(); }
  }
  /** VIEW is offered while it has a SHOW chip or the scope chip. */
  private viewSync() {
    const t = this.root.querySelector('.trkCard2 .tgl') as HTMLElement | null, sc = this.root.querySelector('.trkCard2 .scope') as HTMLElement | null;
    if (!t || !sc) return;
    const any = !t.hidden || !sc.hidden;
    if (any !== this.cardAvail.has('layers')) { this.cardsAvail('layers', any); this.cardsLayout(); }
  }
  /** Open / shut the TUNE card (the old chip's list) — kept for the host and the drivers. */
  tuneOpen(on: boolean) {
    if (on && !this.tuneRows.length) on = false;
    this.cardOpen('tune', on, false);
    if (on) ((this.root.querySelector('.tunePop .tpRow.live') || this.root.querySelector('.tunePop .tpRow')) as HTMLElement | null)?.focus();
  }
  get tuneList(): Array<{ id: string; name: string; freq: string; group: string; live: boolean; why: string; locked: boolean }> {
    return (Array.from(this.root.querySelectorAll('.tunePop [data-tune]')) as HTMLElement[]).map((b) => {
      const r = this.tuneRows.find((x) => x.id === b.dataset.tune)!;
      return { id: r.id, name: r.name, freq: r.freq, group: r.group, live: r.live, why: r.why, locked: b.classList.contains('dialLocked') };
    });
  }
  private onTune: ((id: string) => void) | null = null;
  /** ★★ V12 2026-11-16 the AVIATION MAP's AIRPORTS layer: the basemap's profile, switched live ('aero' ↔ 'aero-plain'),
   *  on whichever map this view made (GPU or Leaflet). Remembered for a map made later. */
  setProfile(name: string) {
    if (this.o.profile === name) return;
    this.o.profile = name;
    (window as any).__vibeTrkProfile = name;   // ★ for drivers (scripts/test-web-avmap.mjs), like __vibeTrkMap
    try {
      // ★ a server whose map pack predates 'aero-plain' says no: the airport layers are switched by name instead
      if (this.vm && this.vm.setProfile?.(name) === false) {
        // ★ beta96 'airsea' / 'airsea-plain' on an older pack: its aero profile, then the ports switched on by name
        const sea = name === 'airsea' || name === 'airsea-plain';
        const base = name === 'airsea' ? 'aero' : name === 'airsea-plain' ? 'aero-plain' : name;
        if (sea) this.vm.setProfile?.(base);
        const m = this.vm.map;
        for (const id of ['airports-4', 'airports-6', 'airports-7', 'airports-8', 'runways', 'runway-ends'])
          if (m?.getLayer?.(id)) m.setLayoutProperty(id, 'visibility', base === 'aero-plain' ? 'none' : 'visible');
        if (sea && m?.getLayer?.('ports')) m.setLayoutProperty('ports', 'visibility', 'visible');
      }
    } catch (e) { console.error('[tracks] profile', e); }
    try { if (this.vmL?.setProfile?.(name) === false) this.vmL?.setProfile?.(name === 'airsea' ? 'aero' : name === 'airsea-plain' ? 'aero-plain' : name); } catch (e) { console.error('[tracks] profile', e); }
  }
  get profile(): string { return this.o.profile || ''; }
  /** ★ V12 2026-11-02: the time filter's pressed button, from outside (the two shipping views share one filter). */
  setRange(id: string) {
    for (const b of Array.from(this.root.querySelectorAll('[data-rng]')) as HTMLElement[]) b.setAttribute('aria-pressed', String(b.dataset.rng === id));
  }
  /** ★★★ beta96 THE LIVE MAPS (liveMaps.ts liveChrome): the title and the exit key are COMPUTED from the current mode on
   *  every render and set here — a view is never left wearing the mode it was made for (Mac Ha 11-26). */
  setChrome(title: string, exitLabel: string) {
    const t = this.root.querySelector('.ttl') as HTMLElement, x = this.root.querySelector('.exit') as HTMLElement;
    if (t.textContent !== title) { t.textContent = title; this.root.setAttribute('aria-label', title + ' map'); }
    if (x.textContent !== exitLabel) x.textContent = exitLabel;
    // ★ the round X: what it does, said in words ("Close map", "Exit datalink", "Leave server")
    const said = exitLabel.charAt(0).toUpperCase() + exitLabel.slice(1).toLowerCase();
    if (x.getAttribute('aria-label') !== said) { x.setAttribute('aria-label', said); if (!x.classList.contains('dialLocked')) x.title = said; }
  }
  /** ★★★ beta96 THE DIAL LOCK ON A MAP (Stuart 11-26): while the server says this listener may not move the shared radio
   *  (ask before tuning, the owner's scan), the controls that WOULD move it are greyed with `why` — EXIT when `exitToo`
   *  (it leaves a whole-radio mode, which retunes the radio) and every element the host marked `data-dial` (band keys,
   *  GAIN). A press on one is refused here and handed to `onDialLocked` (the host's nudge: the line flashes, CHAT pulses).
   *  ✗ NEVER the map itself: pan, zoom, FIT, layer chips, tabs, sort, cards, KEY, labels and the time filter are each
   *  listener's own view and stay live. '' = unlocked. */
  private dialWhy = '';
  private dialExit = false;
  onDialLocked: ((why: string) => void) | null = null;
  setDialLock(why: string, exitToo: boolean) {
    this.dialWhy = why; this.dialExit = !!why && exitToo;
    const x = this.root.querySelector('.exit') as HTMLElement;
    const mark = (el: HTMLElement, on: boolean) => {
      el.classList.toggle('dialLocked', on);
      if (on) { el.setAttribute('aria-disabled', 'true'); if (el.dataset.dialTitle === undefined) el.dataset.dialTitle = el.title || ''; el.title = why; }
      else if (el.classList.contains('dialLockedWas') || el.dataset.dialTitle !== undefined) {
        // ★ a TUNE row the SERVER greys (data-why) stays greyed with its own reason when the lock lifts
        if (el.dataset.why) { el.setAttribute('aria-disabled', 'true'); el.title = el.dataset.why; }
        else { el.removeAttribute('aria-disabled'); el.title = el.dataset.dialTitle || ''; }
        delete el.dataset.dialTitle;
      }
    };
    mark(x, this.dialExit);
    // ★★★ tweak 3: the TUNE chip FADES while this listener must ask first (the ask shield's look); a press nudges
    this.root.querySelector('.mDock[data-dock="tune"]')?.classList.toggle('askFaded', !!why);
    for (const el of Array.from(this.root.querySelectorAll('[data-dial], [data-dial] button')) as HTMLElement[]) if (el.tagName === 'BUTTON' || el.tagName === 'SELECT' || el.tagName === 'INPUT') mark(el, !!why);
  }
  get dialLocked(): { why: string; exit: boolean } { return { why: this.dialWhy, exit: this.dialExit }; }
  /** ★★ the pad's / keyboard's TUNE on this map: only where TUNE is offered (a viewer, a map with nothing to tune: none) */
  get canTune(): boolean { return this.cardAvail.has('tune'); }
  /** A press the lock refuses: true = refused (the host was told). */
  private dialRefuses(t: EventTarget | null): boolean {
    if (!this.dialWhy) return false;
    const el = t as HTMLElement | null;
    if (!el?.closest) return false;
    const hit = (this.dialExit && el.closest('.trkTop .exit')) || el.closest('[data-dial]');
    if (!hit) return false;
    this.onDialLocked?.(this.dialWhy);
    return true;
  }
  get title(): string { return (this.root.querySelector('.ttl') as HTMLElement).textContent || ''; }
  get exitLabel(): string { return (this.root.querySelector('.exit') as HTMLElement).textContent || ''; }
  private rngSig: string | null = null;
  /** ★ beta96 the segmented control, redrawn when what it offers changes (the time filter for the maritime layers, the
   *  DATALINK's bands) — [] = none. `onRange` is the view's own. */
  setRanges(ranges: Array<{ id: string; label: string; words: string }>, current: string, label = '', hint = '') {
    const sig = JSON.stringify([ranges.map((r) => r.id), label, hint]);
    let host = this.root.querySelector('.rng:not(.tgl):not(.scope)') as HTMLElement | null;
    if (sig !== this.rngSig) {
      this.rngSig = sig;
      if (!host) { host = document.createElement('span'); host.className = 'rng'; host.setAttribute('role', 'group'); (this.root.querySelector('.rngSect') as HTMLElement).appendChild(host); }
      host.hidden = !ranges.length;
      (this.root.querySelector('.rngSect') as HTMLElement).hidden = !ranges.length;
      host.setAttribute('aria-label', label || 'Show what was heard in the last…'); host.title = hint;
      host.innerHTML = ranges.map((x) => `<button class="btn" type="button" data-rng="${esc(x.id)}" aria-label="${esc(x.words)}">${esc(x.label)}</button>`).join('');
      for (const b of Array.from(host.querySelectorAll('[data-rng]')) as HTMLButtonElement[]) {
        b.onclick = () => {
          for (const o2 of Array.from(host!.querySelectorAll('[data-rng]'))) o2.setAttribute('aria-pressed', String(o2 === b));
          this.o.onRange?.(b.dataset.rng || '');
          this.refit = true;
        };
      }
    }
    if (host) for (const b of Array.from(host.querySelectorAll('[data-rng]')) as HTMLElement[]) b.setAttribute('aria-pressed', String(b.dataset.rng === current));
  }
  private tabSig = '';
  /** ★★ beta96 the list's TABS (AIRCRAFT · MESSAGES · SHIPS …, liveMaps.ts liveTabs) in its header, with their counts.
   *  [] = the plain list title. A tab pressed calls `onTab`; the host draws that tab's rows. */
  setTabs(tabs: Array<{ id: string; label: string; n: number }>, current: string, onTab: (id: string) => void) {
    const host = this.root.querySelector('.hd .tabs') as HTMLElement, lt = this.root.querySelector('.hd .lt') as HTMLElement;
    (host.parentElement as HTMLElement).hidden = tabs.length < 2; lt.hidden = tabs.length >= 2; this.countEl.hidden = tabs.length >= 2;
    this.tabScrollWire(host);
    const sig = JSON.stringify([tabs, current]);
    if (sig === this.tabSig) return;
    const changedTab = !this.tabSig || JSON.parse(this.tabSig)[1] !== current;
    if (changedTab && this.listFrozen) { this.listFrozen = false; this.pauseShow(0); }   // ★ another tab: the old one carries on
    this.tabSig = sig;
    if (tabs.length === 1) lt.textContent = tabs[0].label;
    const tg: Record<string, string> = { aircraft: LAYER_GLYPH.aircraft, ships: LAYER_GLYPH.ship, navtex: LAYER_GLYPH.navtex, sondes: LAYER_GLYPH.balloon, navaids: LAYER_GLYPH.navaid };
    host.innerHTML = tabs.map((t) => `<button class="tab" type="button" role="tab" data-tab="${esc(t.id)}" aria-selected="${t.id === current}">${tg[t.id] ? `<span class="tg" aria-hidden="true">${tg[t.id]}</span>` : ''}${esc(t.label)}<span class="tn">${t.n}</span></button>`).join('');
    for (const b of Array.from(host.querySelectorAll('[data-tab]')) as HTMLButtonElement[]) b.onclick = (e) => { e.stopPropagation(); onTab(b.dataset.tab || ''); };
    const sel = host.querySelector('[aria-selected="true"]') as HTMLElement | null;
    if (sel && changedTab) this.tabIntoView(host, sel);
    this.tabEdges(host);
  }
  /** the tab row's edges: a fade + arrow only where there is more beyond */
  private tabEdges(host: HTMLElement) {
    const w = host.parentElement as HTMLElement;
    const l = host.scrollLeft > 1, r = host.scrollLeft + host.clientWidth < host.scrollWidth - 1;
    if (w.classList.contains('mL') !== l) w.classList.toggle('mL', l);
    if (w.classList.contains('mR') !== r) w.classList.toggle('mR', r);
  }
  private tabIntoView(host: HTMLElement, t: HTMLElement) {
    const a = t.offsetLeft - host.offsetLeft, pad = 24;
    if (a < host.scrollLeft + pad) host.scrollLeft = Math.max(0, a - pad);
    else if (a + t.offsetWidth > host.scrollLeft + host.clientWidth - pad) host.scrollLeft = a + t.offsetWidth - host.clientWidth + pad;
  }
  private tabScrollWired = false;
  private tabScrollWire(host: HTMLElement) {
    if (this.tabScrollWired) return;
    this.tabScrollWired = true;
    const w = host.parentElement as HTMLElement;
    host.addEventListener('scroll', () => this.tabEdges(host), { passive: true });
    if (typeof ResizeObserver !== 'undefined') new ResizeObserver(() => this.tabEdges(host)).observe(host);
    const step = (dir: number) => { const t = host.querySelector('.tab') as HTMLElement | null; host.scrollBy({ left: dir * (t?.offsetWidth || 90), behavior: 'smooth' }); };
    (w.querySelector('.tArr.l') as HTMLElement).addEventListener('click', (e) => { e.stopPropagation(); step(-1); });
    (w.querySelector('.tArr.r') as HTMLElement).addEventListener('click', (e) => { e.stopPropagation(); step(1); });
    // ★ keyboard / pad: ← → walk the tabs, each brought into view
    host.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      const ts = Array.from(host.querySelectorAll('.tab')) as HTMLElement[], i = ts.indexOf(document.activeElement as HTMLElement);
      if (i < 0) return;
      const n = ts[i + (e.key === 'ArrowRight' ? 1 : -1)];
      if (n) { e.preventDefault(); e.stopPropagation(); n.focus({ preventScroll: true }); this.tabIntoView(host, n); }
    });
    host.addEventListener('focusin', (e) => { const t = (e.target as HTMLElement).closest('.tab') as HTMLElement | null; if (t) this.tabIntoView(host, t); });
  }
  get tab(): string { return (this.root.querySelector('.hd .tab[aria-selected="true"]') as HTMLElement | null)?.dataset.tab || ''; }
  private srtSig = '';
  /** ★★ beta96 the list's sort chip, NEAREST | LATEST — null = none (another tab, or no receiver position). `what` names
   *  the rows for a screen reader ("aircraft"; the mesh maps' "nodes" — Stuart 11-26). */
  setSort(sort: string | null, onSort: (s: string) => void, what = 'aircraft') {
    const host = this.root.querySelector('.hd .srt') as HTMLElement;
    host.hidden = !sort;
    const sig = sort ? `${sort}|${what}` : '';
    if (sig === this.srtSig) return;
    this.srtSig = sig;
    host.setAttribute('aria-label', `Sort the ${what}`);
    /* ★★★ Stuart 12:58 ("nearest latest takes a lot of room … a simple single button toggle"): ONE button wearing the
     *  CURRENT sort — the rings for nearest, the clock for latest — a press swaps */
    if (!sort) { host.innerHTML = ''; return; }
    const next = sort === 'nearest' ? 'latest' : 'nearest';
    const said = sort === 'nearest' ? `Sorted nearest first \u2014 press for latest` : `Sorted latest first \u2014 press for nearest`;
    host.innerHTML = `<button type="button" class="srtT" data-srt="${sort}" data-next="${next}" aria-label="${esc(said.replace('Sorted', `The ${what} are sorted`))}" title="${esc(said)}">`
      + `${modeIconSvg(sort === 'nearest' ? 'nearest' : 'clock', 'sIco')}<span class="sw">${sort === 'nearest' ? 'NEAR' : 'LATEST'}</span></button>`;
    (host.querySelector('.srtT') as HTMLButtonElement).onclick = (e) => { e.stopPropagation(); onSort(next); };
  }
  /** ★★ beta96 the MESSAGES filter line ("Messages from RYR12K · ALL") — '' = none. */
  setFilter(words: string, onClear: () => void) {
    const f = this.root.querySelector('.filt') as HTMLElement;
    f.hidden = !words;
    if (!words) { f.innerHTML = ''; return; }
    if (f.dataset.w === words) return;
    f.dataset.w = words;
    f.innerHTML = `<span>${esc(words)}</span><button type="button" aria-label="Show every message">× ALL</button>`;
    (f.querySelector('button') as HTMLButtonElement).onclick = () => { f.dataset.w = ''; onClear(); };
  }
  /** ★ beta96 the camera's owner on a map with aircraft AND other things: 'aircraft' = the ADS-B layer's AUTO / FIT (the
   *  aviation layers live), 'track' = the shared auto fit over the other items (ships, sondes, NAVTEX live). */
  private camOwner: 'aircraft' | 'track' = 'aircraft';
  private acAuto: boolean | null = null;
  setCamera(owner: 'aircraft' | 'track') {
    if (owner === this.camOwner) return;
    this.camOwner = owner;
    this.applyCamera();
  }
  private applyCamera() {
    if (!this.acLayer) { this.fitBtn.hidden = false; return; }
    // ★★★ beta96 FIT TO ONE LAYER (Stuart 11-27): on the AIR & SEA map the shared auto fit always owns the camera, over the
    //     layer chosen in the MAP card (ADS-B by default) — the ADS-B layer's own AUTO / FIT keys are hidden there
    const track = this.o.mixed ? true : this.camOwner === 'track';
    this.fitBtn.hidden = !track;
    try {
      if (track) { if (this.acAuto == null) this.acAuto = !!this.acLayer.prefs?.().autoZoom; this.acLayer.setPrefs?.({ autoZoom: false }); this.af?.resume?.(); this.af?.fitNow?.(); }
      else { if (this.acAuto != null) { this.acLayer.setPrefs?.({ autoZoom: this.acAuto }); this.acAuto = null; } this.af?.pause?.(); }
    } catch (e) { console.error('[tracks] camera', e); }
  }
  /** ★★ beta96 stage 8 (NDB): another layer on THIS view's map (web/mapkit/ndblayer.js). `cb` gets the map as it is made
   *  ({ gl: the VibeMapGL, or null; lm: the Leaflet map, or null }) and null just before it is destroyed (the view hidden).
   *  Returns the unsubscribe. A host layer never makes a map of its own (one WebGL context per view). */
  private mapCbs = new Set<(m: { gl: any; lm: any } | null) => void>();
  whenMap(cb: (m: { gl: any; lm: any } | null) => void): () => void {
    this.mapCbs.add(cb);
    if (this.layer && (this.vm || this.lmap)) { try { cb({ gl: this.vm, lm: this.vm ? null : this.lmap }); } catch (e) { console.error('[tracks] whenMap', e); } }
    return () => { this.mapCbs.delete(cb); };
  }
  private mapSay(m: { gl: any; lm: any } | null) { for (const cb of Array.from(this.mapCbs)) try { cb(m); } catch (e) { console.error('[tracks] whenMap', e); } }
  /** ★ beta96 stage 8 a host layer's points for the shared auto fit (the NDB beacons heard) — null = none. */
  morePoints: (() => [number, number][]) | null = null;
  /** ★ beta96 the click order for a host layer: is an aircraft under this press? (it is selected if so) */
  pickAircraft(x: number, y: number, touch: boolean): boolean {
    try { return !!this.acLayer?.pickAt?.(x, y, touch); } catch { return false; }
  }
  /** ★ an item to select when its row is pressed (a MESSAGES row names its aircraft's key) — `onRow` hears every press. */
  onRow: ((key: string) => void) | null = null;
  private keySig = '';
  /** ★★ V12 2026-11-02 the KEY, redrawn — it lists the glyphs of the layers this map has (ships, DSC, hazards, arrows). */
  setLegend(html: string) {
    if (html === this.keySig) return;
    this.keySig = html;
    const d = this.root.querySelector('.trkKey') as HTMLElement | null;
    if (!d) return;
    (d.querySelector('.kb') as HTMLElement).innerHTML = html;
    (this.root.querySelector('.keySect') as HTMLElement).hidden = !html;   // ★ tweak 3 the KEY is a fold in MAP CONTROLS
  }
  /** ★ the row under the last press — a list redrawn every second (ADS-B's live report) can replace it between the press
   *  and its click: found again by its key (the ADS-B view's rule, adsbDown, carried over in beta96). */
  private down = { key: '', at: 0, x: 0, y: 0 };
  private rowOf(e: Event): HTMLElement | null {
    let row = (e.target as HTMLElement).closest('.trkRow') as HTMLElement | null;
    const me = e as MouseEvent;
    if (!row && e.type === 'click' && this.down.key && performance.now() - this.down.at < 1000
        && Math.abs(me.clientX - this.down.x) < 12 && Math.abs(me.clientY - this.down.y) < 12) {
      const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
      row = this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(this.down.key) : this.down.key.replace(/["\\]/g, '')}"]`);
    }
    return row;
  }
  private rowPick(e: Event) {
    const row = this.rowOf(e);
    if (!row) return;
    const k = row.dataset.focus || row.dataset.key || '';
    try { this.onRow?.(row.dataset.key || ''); } catch (e) { console.error('[tracks] onRow', e); }
    if (row.dataset.nofocus) { this.markSel(row.dataset.key || ''); return; }
    const hex = this.acKeys.get(k);
    // ★ beta96 a row chosen is where the person wants the map (mapAutoFit's own rule): the auto fit pauses, or its next
    //   tick glides straight back out to everything (the AIR & SEA map pokes it on every feed) — FIT brings AUTO back
    if (!hex || !this.acLayer) this.selPause();
    if (!(hex && this.acLayer ? this.acLayer.focus(hex) : this.layer?.focus(k))) this.note('Not on the map — no position heard yet.', 2500);
    this.markSel(k);
  }
  /** ★ beta96 the selection changed (an icon tapped, a row pressed) — the AIR & SEA map filters its MESSAGES by it. */
  onSel: ((key: string) => void) | null = null;
  /* ★★ beta96 A MESH NODE's ROUTE (src/utils/meshRoute.ts — Stuart 11-26: "clicking a node lights up the repeaters its
   *  advert passed through"): MESH_ROUTE_JS draws it hop by hop on this view's map; null clears it. The ISM maps call it
   *  from onSel — another tap (another node, or the empty map) clears it. */
  private rtMap: any = null; private rtL: any = null; private rtH: { clear(): void } | null = null;
  showRoute(rt: unknown | null) {
    try { this.rtH?.clear(); } catch { /* gone */ }
    this.rtH = null;
    if (!rt || !this.rtMap || !this.rtL) return;
    const w = window as any;
    if (!w.meshRouteDraw) runSource(MESH_ROUTE_JS);
    try { this.rtH = w.meshRouteDraw(this.rtMap, this.rtL, rt); } catch (e) { console.error('[tracks] route', e); }
  }
  /** ★★ beta96 is a route drawn now (the test's handle) */
  get routeShown(): boolean { return !!this.rtH; }
  /* ★★ beta96 THE CARD's ROUTE BUTTON (meshRoute.ts meshRouteButtonHtml): `routeOf` gives a node's route; ROUTE draws it
   *  (and fits the path), ROUTE again or the selection changing / clearing (the card closing) takes it away. The host
   *  passes routeCard() as the view's onCard. */
  private rtKey = '';
  private routeOf: ((key: string) => unknown | null) | null = null;
  setRouteSource(f: (key: string) => unknown | null) { this.routeOf = f; }
  routeCard(_key: string, el: HTMLElement) {
    const b = el.querySelector('.vmrBtn[data-rtk]') as HTMLButtonElement | null;
    if (!b) return;
    b.onclick = (e) => { e.stopPropagation(); this.routeToggle(b.dataset.rtk || ''); };
    this.routeBtn();
  }
  routeToggle(k: string) {
    if (!k || this.rtKey === k) { this.rtKey = ''; this.showRoute(null); }
    else { const rt = this.routeOf?.(k) ?? null; this.rtKey = rt ? k : ''; this.showRoute(rt); }
    this.routeBtn();
  }
  private routeBtn() {
    for (const b of Array.from(this.root.querySelectorAll('.vmrBtn[data-rtk]')) as HTMLElement[]) {
      const on = !!this.rtKey && b.dataset.rtk === this.rtKey;
      b.setAttribute('aria-pressed', String(on)); b.textContent = on ? 'HIDE ROUTE' : 'ROUTE';
    }
  }
  /* ★★ beta96 A SELECTION HOLDS THE VIEW (Stuart's 11-26 recording, the ISM mesh map: a node picked from NODES was flown
   *  to, then the next refresh glided back out to all of England). mapAutoFit's rule, for every way a selection is made —
   *  a row, a link, a tap on the map (and so a mesh route shown): the auto fit PAUSES until FIT is pressed or the
   *  selection is cleared (then it resumes; AUTO switched OFF stays off).
   *  Not while the ADS-B layer owns the camera (its AUTO / FIT keys). */
  private selPaused = false;
  /* ★★★ Stuart 12:14 ("not pausing auto zoom when card is open"): an ITEM's card (an aircraft, a ship, a station — a tap on
   *  its marker or its LIST row) pauses AUTO like a chip's card: the map holds still under it (the MAP chip amber); it
   *  folds → AUTO resumes with one glide. A pan or zoom by hand meanwhile turns AUTO OFF (the red sign), as ever. On the
   *  AIR & SEA map the shared auto fit always owns the camera (applyCamera), so this holds there too. */
  private selPause() {
    if (this.acLayer && this.camOwner !== 'track' && !this.o.mixed) return;
    try { const st = this.af?.state?.(); if (st?.on && !st.paused) { this.af.pause(); this.selPaused = true; this.movedWhilePaused = false; } } catch { /* none */ }
    this.zoomStateSync();
  }
  private selResume() {
    if (!this.selPaused) return;
    this.selPaused = false;
    if (this.acLayer && this.camOwner !== 'track' && !this.o.mixed) return;
    if (!this.cardPausedFit && !this.movedWhilePaused) try { const st = this.af?.state?.(); if (st?.on && st.paused) { this.af.resume(); this.af.fitNow?.(); } } catch { /* none */ }
    this.zoomStateSync();
  }
  /** A pan or zoom by hand while AUTO waited for a card: it stays off when the card folds (the person took the map). */
  private movedWhilePaused = false;
  private markSel(k: string) {
    if (k && k !== this.sel) this.selPause(); else if (!k && this.sel) this.selResume();
    if (k !== this.sel) try { this.onSel?.(k); } catch (e) { console.error('[tracks] onSel', e); }
    if (this.rtKey && k !== this.rtKey) { this.rtKey = ''; this.showRoute(null); this.routeBtn(); }   // ★★ beta96 the card closed / another chosen
    // ★★ beta96 a tap zooms in too (the layer's own focus: the fly, the rings — select() on the same key does nothing more)
    if (this.o.zoomOnSelect && k && k !== this.sel) setTimeout(() => { if (this.sel === k) try { this.layer?.focus(k); } catch { /* gone */ } }, 0);
    this.sel = k;
    for (const el of Array.from(this.rowsEl.querySelectorAll('.trkRow.sel'))) el.classList.remove('sel');
    const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
    if (k) this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(k) : k.replace(/["\\]/g, '')}"]`)?.classList.add('sel');
  }
  private select(k: string) { this.layer?.select(k || null); this.markSel(k); }
  private wantFocus = '';
  private refit = false;
  /** ★★★ beta92 THE AUTO FIT (src/utils/mapAutoFit.ts): one per map shown, made in attach, gone in hide. Not on the aviation
   *  map's aircraft view — the ADS-B layer frames its aircraft with its own AUTO / FIT keys. */
  private af: any = null;
  private fitBtn: HTMLButtonElement;
  /** The items changed: the auto fit considers a move (its hysteresis decides; a 1 s tick does the rest). */
  /** ★ beta96 stage 8 a host layer's points changed (morePoints): the auto fit considers a move. */
  pokeFit() { this.autoFitCheck(); }
  /** ★ a host layer's item chosen: the auto fit pauses (FIT brings AUTO back), as a row chosen here does. */
  pauseFit() { try { this.af?.pause(); } catch { /* none */ } }
  private autoFitCheck(_items?: any[]) { try { this.af?.poke(); } catch (e) { console.error('[tracks] auto fit', e); } }
  /** ★ The card again from its item (a fetched text arrived for it). */
  refreshCard() { try { this.layer?.refreshCard?.(); } catch (e) { console.error('[tracks] card', e); } }
  /** ★ beta95 the padded view [s, w, n, e] (the sea areas drawn worldwide are those in it), null before the map is made. */
  viewBox(): [number, number, number, number] | null { try { return this.layer?.viewBox?.() ?? null; } catch { return null; } }
  /** Select an item and fly to it (a row tapped outside the map, e.g. the APRS box). Before the map is up: when it is. */
  focus(key: string): boolean {
    this.refit = false;
    this.selPause();   // ★ a focus (a node's link) is where the map should be — FIT brings AUTO back
    if (!this.layer) { this.wantFocus = key; return false; }
    const hex = this.acKeys.get(key);
    const ok = hex && this.acLayer ? !!this.acLayer.focus(hex) : !!this.layer.focus(key);
    if (ok) this.markSel(key);
    return ok;
  }
  private noteT: ReturnType<typeof setTimeout> | null = null;
  note(text: string, ms = 0) {
    let n = this.mapHost.querySelector('.trkNote') as HTMLElement | null;
    if (!text) { n?.remove(); return; }
    if (!n) { n = document.createElement('div'); n.className = 'trkNote'; this.mapHost.appendChild(n); }
    n.textContent = text;
    if (this.noteT) clearTimeout(this.noteT);
    if (ms) this.noteT = setTimeout(() => n?.remove(), ms);
  }

  show(rx: LatLon | null) {
    document.body.classList.add('trkMapUp');
    TrackView.coverOn(true);
    this.fitViewport();
    wireIslandSide();   // ★ the island's side from the orientation (iOS gives the inset to both)
    queueMicrotask(() => window.dispatchEvent(new Event('vibe:mapopened')));   // ★ the page lends its health chip (main.ts)
    this.rx = rx;
    (window as any).__vibeTrkView = this;   // ★ for drivers (scripts/test-web-map-cards.mjs): the view last shown
    if (this.open) return;
    this.root.hidden = false;
    this.navScope();   // ★★ V12 the pad / keyboard highlight stays on the map (gamepad.ts scope), never the page under it
    // ★ the page under the map is not reachable while the map covers it (keyboard, a screen reader)
    this.inerted = [];
    for (const el of Array.from(document.body.children) as HTMLElement[]) {
      if (el === this.root || el.tagName === 'SCRIPT' || el.tagName === 'STYLE' || el.inert) continue;
      if (el.classList.contains('trkView') || el.id === 'pill' || el.id === 'toast') continue;
      if (this.o.liveIds?.includes(el.id)) continue;
      el.inert = true; this.inerted.push(el);
    }
    for (const id of this.o.liveIds ?? []) { const el = document.getElementById(id); if (el) el.style.zIndex = '700'; }
    (this.root.querySelector('.exit') as HTMLElement).focus();
    this.boot(++this.gen).catch((e) => { console.error('[tracks] the map could not load', e); this.note('The map did not finish loading — the list still updates.'); });
  }
  hide() {
    if (!this.open) return;
    { const h = this.hostNow; this.hostNow = null; try { h?.destroy?.(); } catch (e) { console.error('[mapChrome] host map destroy', e); } }
    this.tuneOpen(false);
    this.cardsTell(new Set());   // ★ every card the host filled gives its nodes back (CHAT's log and replies)
    this.root.hidden = true;
    TrackView.coverOn(false);
    this.wfTell();   // ★ the waterfall's pane goes with the map: the host stops the spectrum and takes its canvases home
    if (!document.querySelector('.trkView:not([hidden])')) { document.body.classList.remove('trkMapUp'); window.dispatchEvent(new Event('vibe:mapclosed')); }
    this.attribObs?.disconnect(); this.attribObs = null;
    for (const el of this.inerted) el.inert = false;
    this.inerted = [];
    for (const id of this.o.liveIds ?? []) { const el = document.getElementById(id); if (el) el.style.zIndex = ''; }
    this.gen++;
    this.mapSay(null);   // ★ beta96 stage 8 a host layer (NDB) lets go first
    try { this.af?.destroy(); } catch (e) { console.error('[tracks] auto fit destroy', e); }
    this.af = null;
    try { this.acLayer?.destroy(); } catch (e) { console.error('[tracks] aircraft layer destroy', e); }
    this.acLayer = null; this.acMap = null; this.acAuto = null; this.acKeys.clear(); this.acHex.clear();
    (this.root.querySelector('.ctlSect') as HTMLElement).hidden = true;
    (this.root.querySelector('.slot-ctl') as HTMLElement).innerHTML = '';
    this.showRoute(null); this.rtMap = null; this.rtL = null; this.rtKey = '';   // ★★ beta96 a route drawn goes with its map
    try { this.layer?.destroy(); } catch (e) { console.error('[tracks] layer.destroy', e); }
    this.layer = null;
    try { this.vm?.destroy(); } catch (e) { console.error('[tracks] map destroy', e); }
    this.vm = null;
    try { this.lmap?.remove(); } catch (e) { console.error('[tracks] Leaflet remove', e); }
    this.lmap = null; this.vmL = null;
    // ★ V12 2026-11-02: the KEY stays (it is the view's, not the map's) — it was removed here, so a map closed and opened
    //   again had no KEY (and setLegend found nothing to fill)
    // ★ beta96 the cards (open ones and the park) are the view's, not the map's — they stay
    for (const el of Array.from(this.mapHost.children)) if (!el.classList.contains('trkCards') && !el.classList.contains('trkPark') && !el.classList.contains('trkSlot') && !el.classList.contains('cardBtns') && !el.classList.contains('mChips') && !el.classList.contains('mHelp')) el.remove();
    this.scaleMap = null;   // ★ the scale goes with its map
    this.mapHost.className = 'trkMapHost';
    this.lastRows = ''; this.sel = ''; this.pending = null; this.selPaused = false;
  }
  destroy() { this.hide(); this.root.remove(); }

  private hostNow: MapHost | null = null;
  /** ★★★ beta97 the host's map instead of our own (TrackViewOpts.hostMap) — the chrome round it as for ours. */
  private async bootHost(my: number, make: NonNullable<TrackViewOpts['hostMap']>) {
    const holder = document.createElement('div');
    holder.style.cssText = 'position:absolute;inset:0;z-index:0';
    this.mapHost.insertBefore(holder, this.mapHost.firstChild);
    let hm: MapHost | null = null;
    try { hm = await make(holder); } catch (e) { console.error('[mapChrome] the host map', e); }
    if (my !== this.gen) { try { hm?.destroy?.(); } catch { /* tidy */ } return; }
    if (!hm) { this.note('The map could not be loaded here — the list still updates.'); return; }
    this.hostNow = hm;
    this.acMap = hm.map;
    const w = window as any;
    if (hm.points && w.VibeAutoFit) {
      try {
        this.af = w.VibeAutoFit.attach(hm.fitMap ?? hm.map, {
          points: hm.points, receiver: hm.receiver ?? (() => null), button: this.fitBtn, maxZoom: hm.maxZoom,
          what: hm.what || this.o.listTitle.toLowerCase().replace(/s$/, ''),
          onState: () => this.zoomStateSync(), obstacle: () => this.cardsObstacle(),
        });
      } catch (e) { console.error('[mapChrome] auto fit', e); this.af = null; }
    }
    this.fitBtn.hidden = !this.af;
    (this.root.querySelector('.fitOnce') as HTMLElement).hidden = !this.af;
    try { this.watchAttrib(); this.attribAuto(); this.scaleDock(); } catch (e) { console.error('[mapChrome] host map chrome', e); }
    this.zoomStateSync();
    this.mapSay({ gl: hm.gl ?? null, lm: hm.gl ? null : hm.map });
    this.navMarks();
  }
  /* ★★★ beta97 for a host's map (mapChromeApi.ts): the shared auto fit — frame now, let it look again, hold it */
  fitNow() { try { this.af?.fitNow(); } catch (e) { console.error('[mapChrome] fit', e); } }
  fitPoke() { try { this.af?.poke(); } catch (e) { console.error('[mapChrome] fit', e); } }
  fitPause() { try { this.af?.pause(); } catch (e) { console.error('[mapChrome] fit', e); } }
  /** A card's title (and its chip's caption, its names for a screen reader) — SERVERS as the community map's RECEIVERS. */
  retitle(id: CardId, title: string) {
    const ct = this.root.querySelector(`.trkCard2[data-card="${id}"] .ct`) as HTMLElement | null;
    if (ct) ct.textContent = title;
    this.root.querySelector(`.trkCard2[data-card="${id}"]`)?.setAttribute('aria-label', title);
    const cap = this.root.querySelector(`[data-cbtn="${id}"] .cap`) as HTMLElement | null;
    if (cap) cap.textContent = title;
    this.root.querySelector(`[data-cbtn="${id}"]`)?.setAttribute('aria-label', `${title} — open the ${title.toLowerCase()} card`);
  }
  /** A card's body for the host's own rows; `avail` offers its chip. The host owns what is in it from then on. */
  cardBody(id: CardId, avail = true): HTMLElement {
    this.cardsAvail(id, avail); this.cardsLayout();
    return this.root.querySelector(`.trkCard2[data-card="${id}"] .cb`) as HTMLElement;
  }
  /** The host's own section in MAP CONTROLS (shown once asked for). */
  controlsSlot(): HTMLElement { (this.root.querySelector('.ctlSect') as HTMLElement).hidden = false; return this.slot('ctl'); }
  private async boot(my: number) {
    if (this.o.hostMap) return this.bootHost(my, this.o.hostMap);
    const w = window as any;
    const { TRACK_LAYER_JS } = await import('./generated/trackLayerSource');
    if (my !== this.gen) return;
    if (!w.VibeTrackLayer) runSource(TRACK_LAYER_JS);
    if (!w.VibeAutoFit) runSource(MAP_AUTOFIT_JS);
    if (this.o.aircraft && !w.VibeAdsbLayer) {
      const { ADSB_LAYER_JS } = await import('./generated/adsbLayerSource');
      if (my !== this.gen) return;
      if (!w.VibeAdsbLayer) runSource(ADSB_LAYER_JS);
    }
    const holder = document.createElement('div');
    // ★ Stuart 15:13 ("health chip doesn't expand"): the map in its OWN stacking context (z-index 0) — a station or
    //   label marker's own z-index climbed over the edge chips and took the tap meant for SERVER HEALTH's tab
    holder.style.cssText = 'position:absolute;inset:0;z-index:0';
    this.mapHost.insertBefore(holder, this.mapHost.firstChild);
    const rx = this.rx;
    const kit = await probeMapGL();
    if (my !== this.gen) return;
    if (kit && await loadMapGLScripts(kit.base)) {
      if (my !== this.gen) return;
      try {
        const vm = w.VibeMapGL.createNow(holder, { style: kit.style, load: mapglLoad, base: kit.base, rangeBase: kit.base, detail: kit.detail,
                                                   ...(this.o.profile ? { profile: this.o.profile } : {}), scale: false,   // ★ the SCALE chip draws ours
                                                   center: rx ? [rx.lon, rx.lat] : [0, 30], zoom: rx ? 7 : 1.5 });
        if (vm) {
          this.vm = vm;
          vm.ready.catch((e: unknown) => console.error('[tracks] GPU map: the style did not load', e));
          vm.map.addControl(new w.maplibregl.NavigationControl({ showCompass: false }), 'top-left');
          const L = w.VibeMapGLCompat.install(vm, { leafletZoomOffset: 1 });
          this.attach(L.map(), L);
          return;
        }
      } catch (e) {
        console.error('[tracks] GPU map failed — using the Leaflet map', e);
        try { this.vm?.destroy(); } catch { /* tidy */ }
        this.vm = null; holder.innerHTML = '';
      }
    }
    if (!await loadLeafletInto(document)) { if (my === this.gen) this.note('The map could not be loaded on this server — the list still updates.'); return; }
    if (my !== this.gen) return;
    const L = w.L;
    this.lmap = L.map(holder, { worldCopyJump: true, preferCanvas: true });
    this.lmap.setView(rx ? [rx.lat, rx.lon] : [30, 0], rx ? 8 : 2);
    if (my !== this.gen) return;
    // ★ the shared map data (coastlines, places) — as AdsbMap draws it under Leaflet
    if (!w.VibeMap) { try { const { VIBEMAP_JS } = await loadVibemapSource(); if (my !== this.gen) return; if (!w.VibeMap) runSource(VIBEMAP_JS); } catch (e) { console.error('[tracks] vibemap', e); } }
    if (w.VibeMap) this.vmL = w.VibeMap.attach(this.lmap, { dataBase: dataUrl('/mapdata/v1/'), ...(this.o.profile ? { profile: this.o.profile } : {}), scale: false });   // ★ the SCALE chip draws ours
    this.attach(this.lmap, L);
  }
  /** The scale bar's units: NM beside km on the sea and air maps. */
  private scaleKind(): 'nautical' | 'metric' {
    const n = this.o.nautical ?? (this.o.profile === 'marine' || this.o.profile === 'aero' || this.o.profile === 'airsea');
    return n ? 'nautical' : 'metric';
  }
  private attach(map: any, L: any) {
    const w = window as any;
    // ★ A handle for drivers (scripts/test-web-navtex-map.mjs — the close-up coast check), like window.__vibeWf: the map of
    //   the track view last opened. Read-only use; nothing in the page reads it.
    w.__vibeTrkMap = map;
    this.rtMap = map; this.rtL = L;   // ★★ beta96 showRoute draws on this map, with this map's Leaflet (or the GPU adapter)
    this.layer = w.VibeTrackLayer.create(map, L, {
      receiver: this.rx, icon: this.o.icon, label: this.o.label, card: this.o.card, trail: this.o.trail,
      labelZoom: this.o.labelZoom, focusZoom: this.o.focusZoom, heard: this.o.heard, labelRank: this.o.labelRank, arc: this.o.arc,
      shape: this.o.shape, cluster: this.o.cluster, clusterZoom: this.o.clusterZoom, clusterAlways: this.o.clusterAlways, yieldGroups: this.o.yieldGroups, aria: this.o.aria, onCard: this.o.onCard,
      onSelect: (k: string | null) => this.markSel(k || ''),
      // ★★ beta96 THE CLICK ORDER (receiver → points → lines → areas): a press on an AREA's marker (a sea area's name, a
      //   NAVTEX area warning, a forecast arrow) goes to an aircraft under it first — Stuart's community-map bug
      yieldClick: (it: any, ev: any) => {
        if (!this.acLayer || !isAreaItem(it)) return false;
        const t = ev?.changedTouches?.[0] ?? ev;
        const took = !!this.acLayer.pickAt?.(t?.clientX, t?.clientY, !!ev?.changedTouches || ev?.pointerType === 'touch');
        if (took) this.layer?.select?.(null);
        return took;
      },
      // ★ beta92 the first framing is the auto fit's (a glide's rules), not an immediate fitBounds — unless it is absent
      firstFit: (this.o.aircraft && !this.o.mixed) || !w.VibeAutoFit ? undefined : false,   // ★ beta96 a mixed map is the auto fit's
    });
    this.acMap = map;
    // ★ beta96 a MIXED map (AIR & SEA) makes the ADS-B layer only when it first has aircraft — a shipping-only view has no
    //   dead AUTO / RINGS / RANGE keys and no rings over the sea (feed → makeAcLayer)
    if (this.o.aircraft && !this.o.mixed) this.makeAcLayer();
    /* ★★★ beta92 THE AUTO FIT — every item heard and the receiver, clear of the list's corner; the FIT key is its AUTO key.
     *  On the aviation map the ADS-B layer frames the aircraft (its own AUTO / FIT), so the key is hidden there. */
    this.fitBtn.hidden = !!this.acLayer;
    // ★ beta96 a map with aircraft AND other layers (the AIR & SEA map) has the shared auto fit too; setCamera says whose
    //   camera it is (the ADS-B layer's while aviation is live, else this one's)
    if ((!this.acLayer || this.o.mixed) && w.VibeAutoFit) {
      const layer = this.layer;
      try {
        this.af = w.VibeAutoFit.attach(map, {
          points: () => {
            if (this.fitGroups.length) return this.fitPoints(this.fitAll);   // ★★★ beta96 the chosen layer, outliers out
            const p = layer.points(); const m = this.morePoints?.(); return m?.length ? [...p, ...m] : p;
          },
          receiver: () => layer.receiver(), button: this.fitBtn,
          onState: () => { if (this.cardPausedFit || this.selPaused) { try { const st = this.af?.state?.(); if (st && (!st.on || !st.paused)) { this.cardPausedFit = false; this.selPaused = false; this.movedWhilePaused = false; } } catch { /* none */ } } this.zoomStateSync(); },
          what: this.o.listTitle.toLowerCase().replace(/s$/, ''),
          obstacle: () => this.cardsObstacle(),
        });
      } catch (e) { console.error('[tracks] auto fit', e); this.af = null; }
    }
    if (this.o.mixed) this.applyCamera();
    if (this.pending) {
      const p = this.pending; this.pending = null; this.feed(p);
      if (!this.wantFocus) this.autoFitCheck(p);
    }
    this.watchAttrib();
    this.attribAuto();
    this.scaleDock();
    { const host = this.mapHost; let x0 = -1, y0 = -1;
      const moved = () => { if ((this.cardPausedFit || this.selPaused) && !this.movedWhilePaused) { this.movedWhilePaused = true; this.zoomStateSync(); } };
      host.addEventListener('wheel', (e) => { if (!(e.target as HTMLElement).closest?.('.trkCard2, .mDock, .mHelp')) moved(); }, { passive: true });
      host.addEventListener('pointerdown', (e) => { x0 = e.clientX; y0 = e.clientY; });
      host.addEventListener('pointermove', (e) => { if (e.buttons && x0 >= 0 && Math.hypot(e.clientX - x0, e.clientY - y0) > 8 && !(e.target as HTMLElement).closest?.('.trkCard2, .mDock, .mHelp')) { x0 = -1; moved(); } });
      host.addEventListener('pointerup', () => { x0 = -1; }); }
    this.mapSay({ gl: this.vm, lm: this.vm ? null : this.lmap });   // ★ beta96 stage 8 a host layer (NDB) joins this map
    this.navMarks();
    if (this.wantFocus) { const k = this.wantFocus; this.wantFocus = ''; setTimeout(() => this.focus(k), 50); }
  }

  private acMap: any = null;
  /** ★ beta90 the ADS-B layer (aircraft:true) — at attach, or (mixed) when the first aircraft arrives. */
  private makeAcLayer() {
    const w = window as any, map = this.acMap;
    if (this.acLayer || !map || !w.VibeAdsbLayer) return;
    try {
      let prefs: any = null;
      // ★ beta96 ADS-B mode's own map is this one now: its toggles (RANGE, RINGS, AUTO …) carry over until set here
      try { prefs = JSON.parse(localStorage.getItem('vibesdr.avMapPrefs') || localStorage.getItem('vibesdr.adsbMapPrefs') || 'null'); } catch { /* private */ }
      this.acLayer = w.VibeAdsbLayer.create(map, {
        receiver: this.rx, prefs, theme: 'dark', sparseLegend: true, drawReceiver: false,   // ★ the track layer draws the house
        obstacles: () => this.chromeRects(),   // ★ an aircraft's card keeps clear of every chip, corner piece and card
        imageBase: this.o.imageBase ?? null,
        // ★★★ beta97 the layer builds NO keys (keys:false): the MAP card draws its own and calls layer.actions (acCtl)
        keys: false, onCtl: (st: AcCtl) => this.acCtl(st),
        onPrefs: (p: unknown) => { try { localStorage.setItem('vibesdr.avMapPrefs', JSON.stringify(p)); } catch { /* private */ } },
        // ★ an aircraft tapped on the map: its list row is marked (the layer opens its own card)
        onSelect: (h: string | null) => { this.layer?.select?.(null); this.markSel(h ? this.acHex.get(h) || '' : ''); },
      });
      this.placeAcObstacle();
      try { this.acCtl(this.acLayer.ctlState?.() ?? null); } catch { /* an older layer: no keys */ }
      this.cardsLayout();
      w.__vibeAcLayer = this.acLayer;   // ★ a handle for drivers (scripts/test-web-avmap.mjs), as __vibeTrkMap
    } catch (e) { console.error('[tracks] the aircraft layer could not start', e); this.acLayer = null; }
    if (this.o.mixed) this.applyCamera();
  }
  /* ★★★ THE CREDITS FOLD AWAY BY THEMSELVES (Stuart 11-27: "it always pissed me off having to hide it every time"): the
   *  attribution opens EXPANDED with the map, then folds to its (i) after 5 s or at the first touch of the map (a pan, a
   *  zoom, a tap), whichever comes first; the (i) opens it again, and it folds again 5 s later. Never remembered — every
   *  map opens with the credits shown. This follows the OSMF Attribution Guidelines for interactive maps: the credit is
   *  shown without any interaction at first, and may then collapse "automatically on map interaction such as panning,
   *  clicking, or zooming" or "automatically after five seconds", provided the user can still find it — the (i) in the
   *  corner, which the guidelines give as an example. GPU map (MapLibre's own compact control); the Leaflet fallback keeps
   *  its full line. */
  /* ★★★ THE AUTO-ZOOM STATE (Stuart 11-27): on · off (the person moved the map, or switched AUTO off) · paused (a transient
   *  card is open — AUTO waits while it is used, and resumes when it folds; the LIST is the exception, it re-fits once).
   *  Shown on the MAP chip (under its glyph — a badge in the corner did not read at 16 px) and on the AUTO key. */
  private cardPausedFit = false;
  private zoomStateNow: 'on' | 'off' | 'paused' | '' = '';
  private zoomStateSync() {
    let st: { on?: boolean; paused?: boolean } | null = null;
    try { st = this.af?.state?.() ?? null; } catch { st = null; }
    const z: 'on' | 'off' | 'paused' | '' = !st ? '' : this.movedWhilePaused && (this.cardPausedFit || this.selPaused) ? 'off'
      : this.cardPausedFit || this.selPaused ? 'paused' : st.on && !st.paused ? 'on' : 'off';
    if (z === this.zoomStateNow) return;
    this.zoomStateNow = z;
    const dk = this.root.querySelector('.mDock[data-dock="map"]') as HTMLElement | null;
    const words = z === 'on' ? 'Auto zoom on' : z === 'paused' ? 'Auto zoom paused while a card is open' : z === 'off' ? 'Auto zoom off \u2014 you moved the map; press FIT or AUTO to resume' : '';
    if (dk) {
      if (z) dk.dataset.zoom = z; else delete dk.dataset.zoom;
      const zs = dk.querySelector('.zst') as HTMLElement | null;
      if (zs) zs.innerHTML = z === 'on' ? modeIconSvg('zoomAuto', 'zIco') : z === 'paused' ? modeIconSvg('zoomPause', 'zIco') : z === 'off' ? `${modeIconSvg('zoomLens', 'zIco')}${modeIconSvg('noEntry', 'zNo')}` : '';
      const b = dk.querySelector('.mchip') as HTMLElement | null;
      if (b) { b.title = words ? `Map controls \u2014 ${words}` : 'Map controls'; b.setAttribute('aria-label', `MAP \u2014 open the map controls${words ? '. ' + words : ''}`); }
    }
    this.fitBtn.dataset.zoom = z;
    if (words) this.fitBtn.title = words;
  }
  /** A transient card opened / every transient folded: AUTO waits while one is open (only when it was running). */
  private fitPauseForCards() {
    const open = this.cardOrder.some((x) => !STICKY_CARDS.has(x)) || (this.listMax && this.isCardOpen('list'));
    try {
      const st = this.af?.state?.();
      if (open && !this.cardPausedFit && st?.on && !st.paused) { this.cardPausedFit = true; this.af.pause(); }
      else if (open && !this.cardPausedFit && this.selPaused) { this.cardPausedFit = true; }
      // ★ Stuart 12:31 ("sluggish"): a chip card folding only RESUMES AUTO — the map did not change under it, so no forced
      //   glide (a glide moves every marker each frame); AUTO's own hysteresis moves it if something did change
      else if (!open && this.cardPausedFit) { this.cardPausedFit = false; const s2 = this.af?.state?.(); if (!this.selPaused && !this.movedWhilePaused && s2?.on && s2.paused) this.af.resume(); }
    } catch (e) { console.error('[tracks] fit pause', e); }
    this.zoomStateSync();
  }
  /** ★ the map's scale lines (MapLibre's / Leaflet's own control — still theirs, still live) into the SCALE chip. A press
   *  does nothing: it is a reading, not a control. */
  /* ★★★ THE VERTICAL SCALE (Stuart 11-27): a line down the right edge as long as a round distance at this zoom (1 · 2 · 5 ×
   *  10ⁿ), its words stacked beside it a character a line ("1 0 0 n m"). NM on the sea and air maps, km on the others. True
   *  at the map's centre latitude (Mercator stretches it away from there — as every scale bar is, MapLibre's included). */
  private scaleMap: any = null;
  private scaleDock() {
    const map = this.acMap ?? this.lmap;
    if (!map) return;
    this.scaleMap = map;
    const upd = () => { try { this.scaleDraw(); } catch (e) { console.error('[tracks] scale', e); } };
    try { map.on?.('zoomend moveend resize', upd); } catch { /* none */ }
    try { if (this.vm?.map?.on) for (const ev of ['zoom', 'move', 'resize']) this.vm.map.on(ev, upd); } catch { /* none */ }
    upd();
  }
  private scaleMaxPx = 96;
  private scaleDraw() {
    const map = this.scaleMap, chip = this.root.querySelector('.mScale') as HTMLElement | null;
    if (!map || !chip) return;
    const c = map.getCenter?.(), z = map.getZoom?.();
    if (!c || !Number.isFinite(z)) { chip.hidden = true; return; }
    const mpp = 40075016.686 * Math.cos(((c.lat ?? c[1]) * Math.PI) / 180) / (256 * Math.pow(2, z));   // metres per CSS px (Leaflet zoom)
    const nm = this.scaleKind() === 'nautical', unit = nm ? 1852 : 1000, word = nm ? 'nm' : 'km';
    const maxPx = this.scaleMaxPx, max = (mpp * maxPx) / unit;
    const p10 = Math.pow(10, Math.floor(Math.log10(Math.max(max, 1e-6))));
    const nice = [5, 2, 1].map((k) => k * p10).find((v) => v <= max) ?? p10;
    const px = Math.max(12, Math.round((nice * unit) / mpp));
    const label = `${nice >= 1 ? Math.round(nice) : nice}${word}`;
    const line = chip.querySelector('.vsLine') as HTMLElement, lbl = chip.querySelector('.vsLbl') as HTMLElement;
    if (this.root.classList.contains('scaleH')) { line.style.width = `${px}px`; line.style.height = ''; }
    else { line.style.height = `${px}px`; line.style.width = ''; }
    if (lbl.dataset.t !== label) { lbl.dataset.t = label; lbl.innerHTML = [...label].map((ch) => `<span>${esc(ch)}</span>`).join(''); }
    chip.setAttribute('aria-label', `Map scale: the line is ${nice} ${nm ? 'nautical miles' : 'kilometres'}`);
    chip.hidden = false;
  }
  private attribT: ReturnType<typeof setTimeout> | null = null;
  private attribAuto() {
    const at = this.mapHost.querySelector('.maplibregl-ctrl-attrib') as HTMLElement | null;
    if (!at) return;
    const gen = this.gen;
    at.classList.add('maplibregl-compact', 'maplibregl-compact-show');
    const fold = () => { if (this.attribT) { clearTimeout(this.attribT); this.attribT = null; } if (gen === this.gen) at.classList.remove('maplibregl-compact-show'); };
    const arm = () => { if (this.attribT) clearTimeout(this.attribT); this.attribT = setTimeout(fold, 5000); };
    arm();
    const canvas = this.mapHost.querySelector('.maplibregl-canvas-container, .maplibregl-canvas') as HTMLElement | null;
    const first = (e: Event) => { if (!at.contains(e.target as Node) && at.classList.contains('maplibregl-compact-show')) fold(); };
    for (const ev of ['pointerdown', 'wheel', 'touchstart']) (canvas ?? this.mapHost).addEventListener(ev, first, { passive: true });
    // ★ the (i): MapLibre's own toggle opens it; ours folds it again 5 s later
    (at.querySelector('.maplibregl-ctrl-attrib-button') as HTMLElement | null)?.addEventListener('click', () => setTimeout(() => { if (at.classList.contains('maplibregl-compact-show')) arm(); }, 0));
  }
  /** ★ The map's attribution (a licence line we may not hide) sits at the bottom; on a phone it wraps to three lines and
   *  the list's last row went under it. The list stands just above it, however tall it grows. */
  private attribObs: ResizeObserver | null = null;
  private watchAttrib() {
    this.attribObs?.disconnect(); this.attribObs = null;
    const at = this.mapHost.querySelector('.leaflet-control-attribution, .maplibregl-ctrl-attrib') as HTMLElement | null;
    if (!at || typeof ResizeObserver === 'undefined') return;
    const sync = () => {
      // ★ Stuart 14:38 (the ? on the credits' (i)): the room the credits take is from their TOP to the map's foot — their
      //   own height left out the control's margin, so the ? sat on the (i)
      const ab = at.getBoundingClientRect(), mb = this.mapHost.getBoundingClientRect();
      const h = ab.height > 0 ? Math.max(0, mb.bottom - ab.top) : 0;
      const wide = this.mapHost.clientWidth > 520;
      // ★ on a wide map the attribution is to the right of the list; only where they share the width does it push up
      // ★ beta96 the cards always stop above the attribution line (a tall KEY card ran under it on a wide window, Stuart 11-27)
      void wide;
      this.root.style.setProperty('--trk-attrib-h', `${Math.round(h)}px`);
    };
    this.attribObs = new ResizeObserver(sync);
    this.attribObs.observe(at); this.attribObs.observe(this.mapHost);
    sync();
  }

  /** Every report: the whole list (items with key/lat/lon), the rows' HTML, the status line, the count. */
  /** ★ beta90 the items to their layers: aircraft (`_adsb`) to the ADS-B layer, the rest to the track layer. */
  private feed(items: any[]) {
    // ★★★ beta96 FIT TO: every placed item's group (the host's _fg; aircraft default to 'adsb' / 'datalink' by their data)
    this.fitAll = [];
    for (const it of items) {
      const lat = Number(it?.lat), lon = Number(it?.lon);
      if (!Number.isFinite(lat) || !Number.isFinite(lon) || it?._nofit) continue;
      const g = typeof it._fg === 'string' ? it._fg : it._adsb ? (it._adsb.sparse ? 'datalink' : 'adsb') : 'other';
      this.fitAll.push({ lat, lon, g });
    }
    if (!this.acLayer && this.o.mixed && this.acMap && items.some((it) => it && it._adsb)) this.makeAcLayer();
    if (!this.acLayer) { this.layer.update(items); return; }
    const rest: any[] = [], ac: any[] = [];
    this.acKeys.clear(); this.acHex.clear();
    for (const it of items) {
      const a = it && it._adsb;
      if (a && typeof a.hex === 'string') { ac.push(a); this.acKeys.set(it.key, a.hex); this.acHex.set(a.hex, it.key); }
      else rest.push(it);
    }
    this.layer.update(rest);
    try { this.acLayer.update(ac); } catch (e) { console.error('[tracks] aircraft update', e); }
  }
  /** ★ beta96 the open cards' corner of the map (top-left; a phone's card spans the width) — the fits keep clear of it. */
  private cardsObstacle(): { corner: string; w: number; h: number } | null {
    /* ★★★ Stuart 11-27: "when in fit mode if the list is opened the map should shift over to accommodate like it did before" —
     *  the fits keep clear of the open LIST (bottom-left, where the old list sheet sat); the transient cards fold away on
     *  their own and never move the map. No LIST: the slim left tabs only. */
    const m = this.mapHost.getBoundingClientRect();
    if (m.width <= 0) return null;
    const list = this.root.querySelector('.trkCards .sideB .trkCard2[data-card="list"]') as HTMLElement | null;
    const lb = list && !list.hidden ? list.getBoundingClientRect() : null;
    if (lb && lb.width > 0 && lb.height > 0) return { corner: 'bottom-left', w: Math.round(lb.right - m.left + 8), h: Math.round(m.bottom - lb.top + 8) };
    const col = this.root.querySelector('.mChipsL') as HTMLElement | null, cb = col?.getBoundingClientRect();
    return cb && cb.width > 0 ? { corner: 'top-left', w: Math.round(cb.right - m.left + 6), h: Math.round(cb.bottom - m.top + 6) } : null;
  }
  private listWasOpen: boolean | null = null;
  /** LIST opened or shut: re-fit (a glide) to the room left — only while AUTO is on; never moves a map the person holds. */
  private listRefit() {
    const open = this.cardOrder.includes('list') && this.cardAvail.has('list');
    if (this.listWasOpen === open) return;
    const first = this.listWasOpen === null;
    this.listWasOpen = open;
    if (first) return;
    setTimeout(() => {
      this.placeAcObstacle();
      try { const st = this.af?.state?.(); if (st?.on && !st.paused) this.af.fitNow(); } catch (e) { console.error('[tracks] list refit', e); }
    }, 60);
  }
  /** For the drivers: the points the fit frames now (the chosen layer, outliers out) and the map to project them with. */
  fitPointsForTest(): { pts: [number, number][]; map: any } { return { pts: this.fitPoints(this.fitAll), map: this.acMap }; }
  /** The list's corner of the map — the ADS-B layer keeps aircraft (and its fit) clear of it. */
  private placeAcObstacle() {
    if (!this.acLayer?.setObstacle) return;
    try { this.acLayer.setObstacle(this.cardsObstacle()); }
    catch (e) { console.error('[tracks] obstacle', e); }
  }
  update(items: any[], rowsHtml: string, status: string, count: number, rx: LatLon | null, safety = '') {
    if (rx && (!this.rx || rx.lat !== this.rx.lat || rx.lon !== this.rx.lon)) {
      this.rx = rx;
      try { this.layer?.setReceiver(rx.lat, rx.lon); this.acLayer?.setReceiver(rx.lat, rx.lon, rx.approx || ''); } catch (e) { console.error('[tracks] rx', e); }
    }
    if (this.layer) {
      try { this.feed(items); } catch (e) { console.error('[tracks] layer.update', e); }
      // ★ a new time range: fit what it shows (the ADS-B layer fits its own aircraft — AUTO / FIT)
      if (this.refit && items.length && !this.acLayer) {
        this.refit = false;
        try { if (this.af) this.af.fitNow(); else this.layer.fit(); } catch (e) { console.error('[tracks] fit', e); }
      }
      this.autoFitCheck(items);
      if (this.acLayer) this.placeAcObstacle();
    }
    else this.pending = items;
    /* ★★★ Stuart 13:15 ("when scrolling the list it should pause moving when new aircraft come in"): scrolled away from
     *  the top, the order FREEZES — rows update in place (FL, distance, age, SNR), new ones are held, gone ones dim; a
     *  "‖ +N" key beside the sort says so; it, or scrolling back to the top, applies the lot */
    this.rowsPending = rowsHtml;
    if (this.listFrozen && rowsHtml !== this.lastRows) { this.frozenApply(rowsHtml); this.lastRows = rowsHtml; }
    else if (rowsHtml !== this.lastRows) {
      const top = this.rowsEl.scrollTop;
      /* ★ beta96 (ADS-B's live list, redrawn every second) the controller's highlight and the keyboard's focus are on an
       *  element about to be replaced: found again by its key */
      const hiKey = (this.rowsEl.querySelector('.trkRow.gpFocus') as HTMLElement | null)?.dataset.key || '';
      const act = document.activeElement as HTMLElement | null;
      const focusKey = act && this.rowsEl.contains(act) ? (act.closest('.trkRow') as HTMLElement | null)?.dataset.key || '' : '';
      this.rowsEl.innerHTML = rowsHtml; this.lastRows = rowsHtml;
      this.rowsEl.scrollTop = top;
      // ★ the LIST's height follows a short list (fits its rows) — re-measured once after the render, only when the count moved
      const cnt = this.rowsEl.childElementCount;
      if (cnt !== this.rowsCountWas) { this.rowsCountWas = cnt; requestAnimationFrame(() => this.cardsLayout()); }
      if (this.sel) this.markSel(this.sel);
      const cssEsc = (globalThis as any).CSS?.escape as ((v: string) => string) | undefined;
      const byKey = (k: string) => this.rowsEl.querySelector(`.trkRow[data-key="${cssEsc ? cssEsc(k) : k.replace(/["\\]/g, '')}"]`) as HTMLElement | null;
      if (hiKey) { const r2 = byKey(hiKey); if (r2) padRehighlight(r2); }
      if (focusKey) byKey(focusKey)?.focus({ preventScroll: true });
    }
    if (this.statEl.textContent !== status) this.statEl.textContent = status;
    // ★ beta96 a map whose host does not place its facts (the ISM maps): the status line heads the LIST card
    if (!this.hostFacts) { const f = this.root.querySelector('.trkCard2[data-card="list"] .facts') as HTMLElement; if (f.textContent !== status) f.textContent = status; }
    this.countEl.textContent = String(count);
    for (const c of Array.from(this.root.querySelectorAll('.mDock[data-dock="list"] .cn, .mDock[data-dock="list"] .tCn')) as HTMLElement[]) if (c.textContent !== String(count)) c.textContent = String(count);
    this.safetyEl.hidden = !safety;
    if (safety && this.safetyEl.innerHTML !== safety) this.safetyEl.innerHTML = safety;
  }
}


/* ★★ ?perf=1 — a tiny on-page meter (Stuart reads it in Safari, where we cannot trace WebKit headless): rAF callbacks,
 *  MapLibre renders, setData calls a second, and the worst main-thread stall (a 100 ms heartbeat's lateness — Safari has
 *  no long-task API). Off unless asked for; nothing here runs otherwise. */
function perfOverlay() {
  if ((window as any).__perfOn) return; (window as any).__perfOn = true;
  const c = { raf: 0, render: 0, setData: 0, lag: 0 };
  let mode = 'ambient', modeT: ReturnType<typeof setTimeout> | null = null;
  const oRaf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb: FrameRequestCallback) => oRaf((t) => { c.raf++; cb(t); });
  const hook = () => {
    const ml = (window as any).maplibregl; if (!ml || ml.__perf) return !!ml?.__perf; ml.__perf = true;
    const P = ml.Map.prototype, oR = P._render; if (oR) P._render = function (...a: any[]) { c.render++; return oR.apply(this, a); };
    // ★ the frame-rate mode: a finger on the map (MapLibre's own loop, full rate) or ambient (≤ 30 fps)
    const oF = P.fire; if (oF) P.fire = function (ev: any, ...a: any[]) { const t = typeof ev === 'string' ? ev : ev?.type; const oe = typeof ev === 'object' && ev?.originalEvent;
      if (t === 'movestart' && oe) { mode = 'max: gesture'; if (modeT) clearTimeout(modeT); } else if (t === 'moveend' && mode !== 'ambient') { if (modeT) clearTimeout(modeT); modeT = setTimeout(() => { mode = 'ambient'; }, 300); }
      return oF.call(this, ev, ...a); };
    const G = ml.GeoJSONSource?.prototype; if (G) { const oS = G.setData; G.setData = function (...a: any[]) { c.setData++; return oS.apply(this, a); }; }
    return true;
  };
  const box = document.createElement('div');
  box.style.cssText = 'position:fixed;left:50%;bottom:4px;transform:translateX(-50%);z-index:99999;background:rgba(0,0,0,.8);color:#7fff9a;font:11px/1.3 ui-monospace,Menlo,monospace;padding:4px 8px;border-radius:6px;pointer-events:none;white-space:pre';
  document.body.appendChild(box);
  // the overlay's own frame clock (it ticks only while the overlay is on — ?perf=1): the frames the browser actually runs
  /* ★ no frame meter of its own: a requestAnimationFrame loop ticks at the display's rate whether or not anything is
   *  drawn (it read 60 while the map drew 4) and it would keep the page awake. MAP RENDERS is the presented-frames
   *  figure — every frame the map draws is one MapLibre render. */
  let last = performance.now();
  setInterval(() => { const t = performance.now(); c.lag = Math.max(c.lag, t - last - 100); last = t; }, 100);
  setInterval(() => {
    hook();
    const g = ((window as any).__vsGpu || []).filter((x: any) => x).slice(-1)[0];
    const gs = g ? g.stats() : null;
    const why = gs && gs.why ? Object.entries(gs.why).map(([k, v]) => `${k} ${v}`).join(', ') : '';
    const gpuWords = !gs ? 'GPU layer: none' : gs.ok ? `GPU layer: OK (${gs.n}) · ambient ${gs.fps} fps for ${gs.pxs} px/s${why ? ` · easing: ${why}` : ''}` : `GPU layer: failed — ${gs.err || 'not started'}`;
    const anims = (document as any).getAnimations ? (document as any).getAnimations().filter((a: any) => a.playState === 'running').length : -1;
    const modeW = mode.startsWith('max') ? mode : gs && gs.ok ? `ambient ${gs.fps} fps` : 'ambient';
    const mg = (window as any).__mapGeom;
    const geomWords = mg ? `layout: ${mg.short ? 'SHORT' : 'normal'} · window ${mg.iw}×${mg.ih} · visual ${mg.vvw}×${mg.vvh} · map ${mg.mapW}×${mg.mapH} · insets L${Math.round(mg.ins.l)} R${Math.round(mg.ins.r)} T${Math.round(mg.ins.t)} B${Math.round(mg.ins.b)} · island ${mg.island || '-'} · cover ${mg.cover ? 'on' : 'off'}${mg.geomErr ? ' · ERROR ' + mg.geomErr : ''}` : 'layout: -';
    { const sh = !!document.querySelector('.trkView:not([hidden])[data-short]');   // ★ short landscape: under the pill, the controls stay testable
      box.style.bottom = sh ? 'auto' : '4px'; box.style.top = sh ? 'calc(env(safe-area-inset-top, 0px) + 44px)' : 'auto'; }
    box.textContent = `${modeW} · page rAF ${c.raf}/s · map renders ${c.render}/s · setData ${c.setData}/s · CSS animations ${anims} · worst stall ${Math.round(c.lag)} ms\n${gpuWords}\n${geomWords}`;
    c.raf = 0; c.render = 0; c.setData = 0; c.lag = 0;
  }, 1000);
}
