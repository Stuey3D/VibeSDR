/* ★★★ V12 SATELLITE MODE in the web client (docs/v12/SATELLITE-MODE.md, Stuart 2026-11-20): ONE whole-radio mode for
 *  every satellite — it replaced the SAT picker and the METEOR LRPT decoder key. The words, the order and the half-globe's
 *  geometry are the app's (src/services/satellites.ts) so both clients say the same thing.
 *
 *    header   SATELLITE · AUTO DOPPLER (default ON) · EXIT
 *    the pick (once the server says "picked"):
 *      voice / packet → COMPACT: name · downlink, the corrected frequency, "In range for 9:55" (or the next pass),
 *                       the decoder's line (APRS lines for packet), and a half-globe (2D canvas, no tiles): the bird
 *                       above its sub-point with its beam cone down to the ground, the receiver on the dome
 *      image          → BIG: the same readout with the picture as it arrives (the decoder box's own image canvas,
 *                       #decImgWrap, lent to this window) and the finished picture at pass end
 *      ISS 145.800 (sstv) opens compact and grows when an SSTV picture starts
 *    "Orbits synced 16:08 · 2 h ago" / "Fetching orbit data…" (the list re-asked every 3 s) / "No orbit data — why"
 *    the timetable: name · frequency · "UP · 6:12 left" / "AOS 16:42 (in 0:38)" — up now with the SHORTEST remaining pass
 *      first, then the next passes; what this radio cannot hear GREYED with why (a timetable, so greying is the rule here)
 *
 *  ★★ THE SERVER DOES THE WORK (THE CONTRACT): a row sends {"op":"pick"} — it tunes, locks the Doppler and attaches the
 *     decoder in one step, under the shared-dial contract (spectrum.ts asks before tuning). AUTO DOPPLER sends
 *     {"op":"doppler"}; leaving the mode (EXIT, the key again, another whole-radio mode) sends {"op":"drop"}.
 *  ★ No flashing: COAST dims the readout, nothing blinks. */
import { plainEnglish } from '../../../src/utils/cwVocab';
import { tlmLivePass, tlmLoadPasses, type TlmPass } from '../../../src/utils/tlmCharts';
import { tlmChartsHtml, tlmEnsureCss } from './tlmChartsWeb';
import { snrChipHtml } from '../../../src/utils/snrColour';   // ★ beta96 the colour-coded SNR per packet
import {
  GLOBE_ARROW_PX, GLOBE_GLYPH, GLOBE_KEY_TEXT, GLOBE_RX_DOT, globeArrowPath, globeGlyph, globeLayers, satReadoutLayout, globeLandInk, SatReminders, globeLabels, satMapOffered, satRemindEnds, satRemindKeyText, SAT_REMIND_FORGET_WHAT, SAT_REMIND_SET_WHAT, satRemindLead,
  satRemindText, satRemindWhy, satTelemetryAbsentText, satTelemetryHead, satTelemetryHex, satTelemetryLine, satTelemetryMerge, satTxDetail, satUplinkText, satTxModeLabel,
  satMapOverlay, type SatMapOverlay, type SatReminder, type SatTelemetry,
  globeScene, satAltKm, satBoxSize, satPassQuality, satPassText, satCorrectedHz, satCountdownText, satCreditText, satHzText, satListEveryMs, satModeRows,
  satStateWord, satSyncedText, satTxCreditText, satWhenText, txText, txUse,
  satCwTail, satDecView, satDecWord, satTextAppend, satViewShown, satViewSwitch, satVfoBLine, satVfoBOffers, SAT_VFOB_KEY, type SatVfoB,
  satBeaconText, SAT_BEACON_EMPTY, type SatBeaconList, txTuneHz,
  // ★★ beta94 SHOW — which satellites' passes the list shows (one set of rules with the app)
  SAT_SHOW_GROUPS, SAT_SHOW_KEY, SAT_SHOW_QS, satShowChoices, satShowCur, satShowEmptyText, satShowGroup, satShowGroupsOffered, satShowHiddenRemindText,
  satShowParse, satShowQ, satShowRows, satShowStore, satShowText, satShowToggle, satShowWithQ, type SatShow, type SatShowGroup, type SatShowQ,
  type SatPass,
  type SatInfo, type SatList, type SatPicked, type SatReadout, type SatTrack, type SatTx,
  satPickTlm,   // ★★★ beta94 a finder that decodes the telemetry too
} from '../../../src/services/satellites';
// ★★ 2026-11-24 ORBCOMM + the sky plot — the app's words and dome (src/services/orbcomm.ts, src/utils/skyPlot.ts)
import { ORB_CARRIER_CHOICES, ORB_CAVEAT, orbCarriersLabel, orbCountsText, orbFollowChoice, orbFollowText, orbHeadText, orbNoteText, orbPktLine, orbSatLines, orbShownSats, orbViewMerge,
         parseOrbcommPkt, parseOrbcommStatus, type OrbFollow, type OrbView } from '../../../src/services/orbcomm';
import { ORB_NO_TUNE, ORB_GUESS_COLOUR, orbLabelPlan, orbRowGuess } from '../../../src/utils/orbcommView';
import type { SatLabelEntry } from '../../../src/utils/knownSignals';
// ★★★ beta94 THE TRANSPONDER FINDER — the app's words and rules (src/services/xpdr.ts)
// ★★★ beta96 SCHEDULE A PASS (src/services/satSchedule.ts — one set of rules with the app)
import { SAT_SCHED_CANCEL_WHAT, SAT_SCHED_FULL, SAT_SCHED_IDLE, SAT_SCHED_KEY, SAT_SCHED_OVERLAP_RULE, SAT_SCHED_PRIO_WHAT, SAT_SCHED_REMIND_NOTE, SAT_SCHED_SET_WHAT,
         SatSchedule, satPassKey, satSchedDegradedText, satSchedKeyText, satSchedNowText, satSchedPlan, satSchedPrioText, satSchedRemindDue, satSchedRowText,
         satSchedStep, satSchedWhy, satSession, satSessionLine, satKeyKind, satOnSharedDial, SAT_SCHED_SHARED_NOTE, type SatSchedPrev, type SatSchedRun, type SatSessionFacts } from '../../../src/services/satSchedule';
// ★★★ beta96 SINGLE MODE: SATELLITES — the watch-only view (src/services/satSingle.ts — one set of rules with the app)
import { SAT_SINGLE_ONLY, SAT_SINGLE_WHY, satHybridText, satSingleHead, satSingleLastText, satSinglePlanRows, satSingleSub,
         type SatHybridInfo, type SatSingle } from '../../../src/services/satSingle';
import { parseXpdrStatus, xpdrAgo, xpdrFaded, xpdrFollowText, xpdrFreqText, xpdrHeardText, xpdrHint, xpdrListenable, xpdrListenLabel, xpdrRowsSorted, xpdrSkimLine, xpdrWhat, XPDR_SKIM_TITLE, type XpdrRow, type XpdrStatus } from '../../../src/services/xpdr';
import { parseSkyPlotJson, skyCellsAddDots, skyModeOf, skyModesHtml, skyPlotMerge, skyPlotQuery, skyPlotSvg, skySceneFor, skySummaryFor, SKY_LEGEND, SKY_PLOT_WHAT, type SkyCells, type SkyDot, type SkyMode } from '../../../src/utils/skyPlot';

export interface SatPanelDeps {
  /** ★★★ beta94 the transponder finder's LISTEN on a voice row: tune VFO A there (USB) and ask the finder to follow it */
  xpdrListen?: (row: XpdrRow) => void;
  /** ★★★ beta97 SKIM: the finder's FT8 / FT4 skimmer on / off ({"type":"xpdr_skim"}) */
  xpdrSkim?: (on: boolean) => void;
  list: () => void;
  /** {"op":"pick"} (spectrum.ts satPick — false = the ask-before-tuning gate held it). */
  pick: (norad: number, tx: number, doppler: boolean) => boolean;
  doppler: (on: boolean) => void;
  drop: () => void;
  /** The receiver's position (the server's location), for the half-globe — null = no globe. */
  rxPos: () => { lat: number; lon: number } | null;
  /** The server attached `decoder` for this pick: main.ts follows it on its decoder socket (to receive its frames). '' = none. */
  follow: (decoder: SatPicked['decoder'], tx: SatTx, params?: Record<string, unknown>, audio?: SatPicked['audio']) => void;
  /** ★★★ beta92 the pick went on the server (a tune past the lock's reach, a mode change): main.ts stops following its
   *  decoder. The mode stays on (the list is there to pick again). */
  pickGone?: () => void;
  /** ★★ beta93 open the view `spanHz` wide around `hz` (a NEW pick's SatPicked.view — Stuart 11-24: "start zoomed in, not
   *  all the way but enough to see the signal and neighbours"). Only on a new pick: a zoom of your own afterwards stays. */
  zoomView?: (hz: number, spanHz: number) => void;
  /** The mode was left: main.ts stops the followed decoder and takes its image canvas back. */
  left: () => void;
  /** ★★★ beta97 into the mode: main.ts sets the satellite view's 500 Hz step (src/services/satTune.ts; left puts theirs back). */
  entered?: () => void;
  /** Lend the decoder box's image canvas to `slot` (null = take it back). */
  image: (slot: HTMLElement | null) => void;
  /** ★ 1.7 {"type":"sat_list","op":"track"} — the pass's path (its `geo` draws the dotted orbit). Free: changes nothing. */
  track?: (norad: number, aos: number, los: number) => void;
  /** ★ 1.8 the receiver dot's label — the server's name ('' = RECEIVER). */
  rxName?: () => string;
  /** ★ 1.6 REMIND: this page's server (the reminders' key) and whether its socket is up now (a reminder with no connection
   *  never fires). */
  serverKey?: () => string;
  connected?: () => boolean;
  /** ★ 1.6 LISTEN on a reminder: into SATELLITE the way the key goes in (leaving any other whole-radio mode). */
  enter?: () => void;
  /** ★ 1.9 the MAP key: how many stations with a position the packet decoder has heard; the ONE DIGI MODES map's opener. */
  mapStations?: () => number;
  openMap?: (o: SatMapOpen) => void;
  /** the clock, ms (tests pass a fake one) */
  now?: () => number;
  /** ★★ beta91 TELEMETRY CHARTS: the server's gallery, read (a path → its text, null = not there) — LAST PASSES */
  galleryGet?: (path: string) => Promise<string | null>;
  /** ★★★ beta92 OPEN IN VFO B — {"type":"sat_vfob",norad,tx} (main.ts follows B's decoder on a second decoder socket) */
  vfobOpen?: (norad: number, tx: number) => void;
  /** ★★ 2026-11-24 ORBCOMM: the manual fallback under AUTO — carriers at most (0 = AUTO); main.ts re-attaches with
   *  "max_carriers". The sky plot: the server's stored dots read through `skyGet` for THIS radio (`skyRadio`). */
  orbMax?: (k: number) => void;
  /** ★★ b97 the server's sat_labels now — the card's guesses come from them through orbLabelPlan, as the waterfall's do */
  satLabels?: () => readonly SatLabelEntry[];
  skyGet?: (path: string) => Promise<string | null>;
  skyRadio?: () => string;
  /** ★★★ beta96 SCHEDULE: what decides SCHEDULE / REMIND (the server's words: dial, listeners, locked — pickPrivate is the
   *  list's own), where the listener is now (frequency, mode), and going back there after LOS (an ordinary tune). */
  session?: () => SatSessionFacts;
  here?: () => { hz: number; mode: string };
  back?: (hz: number, mode: string) => void;
  /** ★★★ beta96 single mode: satellites — GALLERY on the watch view (the last pass's pictures and packets) */
  openGallery?: () => void;
}
/** ★★★ beta96 a schedule is stored for this server — main.ts makes the panel at start-up so it runs with the mode off. */
export function satSchedStored(server: string): boolean {
  try { return SatSchedule.fromJSON(localStorage.getItem(SAT_SCHED_KEY), Date.now() / 1000).all(server).length > 0; } catch { return false; }
}
/** ★ 1.9: what the MAP key hands the DIGI MODES map (b90-maps' DigiMapOpts shape) — its APRS layer, and an overlay of the
 *  pass's orbit, footprint and the bird ("ISS PASS"); the stations carry "via ISS" from the server (aprs …sat). */
export interface SatMapOpen { layer: 'aprs'; overlay: SatMapOverlay | null }

const CSS = `
/* ★★★ THE DECODER BOX's PLACE AND SHAPE (Stuart 2026-11-21: "the satellite decoder box needs to be the same as a standard
   decoder box with the minimise and small/big buttons as right now on the phone you cant see the waterfall behind") —
   #decBox's anchor (bottom-left, above the control card: --decBoxBottom), width, ceiling and header; – folds it to the
   header, the size key swaps SMALL (room to tune) and BIG. */
#satPanel{position:fixed;z-index:46;left:14px;bottom:var(--decBoxBottom,14px);width:min(94vw,max(46vw,380px),560px);
  max-height:calc(100dvh - var(--decBoxBottom,14px) - 24px);display:flex;flex-direction:column;overflow:hidden;
  background:var(--ov-panel96,rgba(8,6,2,.96));color:var(--btn-text);border:1px solid var(--btn-border);border-radius:10px;
  font:13px/1.35 system-ui,sans-serif;box-shadow:0 12px 40px rgba(0,0,0,.6);box-sizing:border-box;transition:bottom .15s ease-out}
#satPanel.big{width:min(94vw,900px)}
#satPanel[hidden]{display:none}
#satPanel .sat-h{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:7px 10px;border-bottom:1px solid var(--bar-border,var(--btn-border));
  font-size:12px;overflow-x:auto;overflow-y:hidden;scrollbar-width:none;flex-wrap:nowrap}
#satPanel .sat-h::-webkit-scrollbar{display:none}
#satPanel .sat-h > *{flex:0 0 auto}
#satPanel .sat-h b{letter-spacing:.08em}
#satPanel .sat-dot{width:8px;height:8px;border-radius:50%;background:var(--text-dim);opacity:.6}
#satPanel .sat-dot.on{background:#5c5;opacity:1}
#satPanel .sat-hs{flex:1 1 auto !important;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text-dim);font-variant-numeric:tabular-nums}
#satPanel .sat-body{flex:1 1 auto;min-height:0;overflow-y:auto;overscroll-behavior:contain;padding:8px 10px;max-height:40vh}
#satPanel.big .sat-body{max-height:clamp(300px,72vh,900px)}
#satPanel.min .sat-body{display:none}
#satPanel.min .sat-h{border-bottom:0}
#satPanel button{background:var(--btn-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:6px;padding:4px 9px;
  font:inherit;cursor:pointer;min-height:30px}
#satPanel .sat-h button{padding:2px 8px;font-size:11px;min-height:26px}
#satPanel button.on{background:var(--btn-active-bg);border-color:var(--btn-active-bdr);color:var(--btn-active-tx)}
#satPanel button:disabled{opacity:.45;cursor:default}
#satPanel .sat-size{padding:2px 5px !important;line-height:0}
#satPanel .sat-size svg{width:11px;height:13px}
#satPanel .sat-size svg rect,#satPanel .sat-size svg path{fill:none;stroke:currentColor;stroke-width:1.2;stroke-linecap:round}
#satPanel.big .sat-size svg rect{y:3;height:10px}
#satPanel.big .sat-size svg path{transform:rotate(180deg);transform-origin:6px 3px}
#satPanel .sat-dim{color:var(--text-dim)}
#satPanel .sat-why{color:var(--text-dim);margin:0 0 4px}
#satPanel .sat-box{border:1px solid var(--btn-border);border-radius:8px;padding:8px;margin:0 0 6px}
#satPanel .sat-box[hidden]{display:none}
#satPanel .sat-ro{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:6px 10px;align-items:center}
#satPanel.big .sat-ro{grid-template-columns:minmax(0,1fr)}
/* ★★ beta96 too narrow for the words AND the small globe (satReadoutLayout): the globe goes under them, never over */
#satPanel .sat-ro.stack{grid-template-columns:minmax(0,1fr)}
#satPanel .sat-ro.stack canvas.sat-globe{margin:0 auto}
#satPanel .sat-ro > div{min-width:0;overflow-wrap:anywhere}
#satPanel .sat-ro.coast .sat-big{opacity:.55}
#satPanel .sat-name{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#satPanel .sat-big{font-size:18px;font-variant-numeric:tabular-nums;margin:2px 0}
#satPanel .sat-cd{font-variant-numeric:tabular-nums}
/* ★ SMALL: the half-globe sits beside the words, small; BIG: its own row at the box's width (Stuart 11-20) */
#satPanel canvas.sat-globe{width:150px;height:auto;aspect-ratio:640/300;display:block}
#satPanel.big canvas.sat-globe{width:100%;max-width:420px;margin:0 auto}
#satPanel .sat-img{margin-top:8px}
#satPanel .sat-img[hidden]{display:none}
/* ★★ beta92 (Stuart 11-23, a Meteor pass: "any chance of making the lines drawn full width of the box? i didnt even notice it
   drawing as it was that small"): the picture is drawn at the box's FULL WIDTH and the box scrolls — it was fitted whole
   into 440 px, so an LRPT pass (it grows down for 15 minutes) shrank to a sliver as it grew. The newest lines stay in
   view while the box is at its foot (main.ts satLendImage); scroll up and it stays where you put it. */
#satPanel .sat-img #decImgWrap{max-height:min(52vh,440px);overflow:auto}
#satPanel .sat-img #decImage{max-height:none;height:auto;object-fit:fill}
/* ★★★ beta96 METEOR: the WHOLE pass in the box (its gaps squashed, it usually fits) — main.ts lrptFitApply; a click flips
   it to following the newest lines at full width and back */
#satPanel .sat-img #decImgWrap.lrptFit #decImage{height:min(52vh,440px);width:auto;max-width:100%;margin:0 auto;object-fit:contain;cursor:zoom-in}
#satPanel .sat-img #decImgWrap:not(.lrptFit) #decImage[title]{cursor:zoom-out}
/* ★★★ beta92 OPEN IN VFO B — the key(s) under the readout, B's line, and the line switch (a line is tappable only while
   both lines have a decoder view; the one shown is lit) */
#satPanel .sat-vb{display:flex;flex-direction:column;gap:4px;margin-top:4px}
#satPanel .sat-vb[hidden]{display:none}
#satPanel .sat-vb button.sat-vbk{align-self:flex-start;max-width:100%;white-space:normal;text-align:left;font-size:11px;padding:2px 8px;min-height:28px;letter-spacing:.04em}
#satPanel .sat-ln{border-radius:6px;padding:2px 6px;margin:0 -6px}
#satPanel .sat-ln.sw{cursor:pointer;border:1px solid transparent}
#satPanel .sat-ln.sw.on{border-color:var(--btn-active-bdr);background:var(--btn-active-bg);color:var(--btn-active-tx)}
#satPanel .sat-cw{margin-top:6px;white-space:pre-wrap;word-break:break-word;font:12px/1.35 ui-monospace,Menlo,monospace;max-height:6.8em;overflow:auto}
#satPanel .sat-cw[hidden]{display:none}
#satPanel .sat-bimg canvas{display:block;width:100%;max-height:min(52vh,440px);object-fit:contain;image-rendering:auto}
#satPanel .sat-pk{margin-top:6px;max-height:9.5em;overflow:auto;font:12px/1.35 ui-monospace,Menlo,monospace}
#satPanel .sat-pk[hidden]{display:none}
#satPanel .sat-sync{margin:4px 0}
#satPanel details.sat-list > summary{cursor:pointer;padding:4px 0;letter-spacing:.06em;list-style:none}
#satPanel details.sat-list > summary::-webkit-details-marker{display:none}
#satPanel details.sat-list > summary::before{content:'▸ ';color:var(--text-dim)}
#satPanel details.sat-list[open] > summary::before{content:'▾ '}
/* ★★ beta94 SHOW (Stuart 11-25): which satellites' passes the list shows — a drop-down above the rows: the group shortcuts
   (keys, as REMIND's lead keys) and a tick box per satellite. The menu font, never the display font. */
#satPanel details.sat-show{margin:2px 0 4px;border:1px solid var(--btn-border);border-radius:6px;font-size:12px}
#satPanel details.sat-show[hidden]{display:none}
#satPanel details.sat-show > summary{cursor:pointer;list-style:none;padding:5px 8px;min-height:28px;box-sizing:border-box;display:flex;align-items:center;gap:6px;
  letter-spacing:.04em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#satPanel details.sat-show > summary::-webkit-details-marker{display:none}
#satPanel details.sat-show > summary::after{content:'▾';margin-left:auto;color:var(--text-dim)}
#satPanel details.sat-show[open] > summary::after{content:'▴'}
#satPanel details.sat-show > summary .sat-shv{overflow:hidden;text-overflow:ellipsis;min-width:0}
#satPanel details.sat-show.on > summary{color:var(--amber)}
#satPanel .sat-shb{border-top:1px solid var(--bar-border,var(--btn-border));padding:6px 8px;display:flex;flex-direction:column;gap:6px}
#satPanel .sat-shg{display:flex;flex-wrap:wrap;gap:6px}
#satPanel .sat-shg button{min-height:28px;padding:1px 9px;font-size:12px}
#satPanel .sat-shn{margin:-2px 0 4px;padding:0 8px;font-size:12px}
#satPanel .sat-shn[hidden]{display:none}
#satPanel .sat-sht{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:2px 8px}
#satPanel .sat-sht button{display:flex;align-items:center;gap:8px;text-align:left;background:transparent;border-color:transparent;padding:3px 4px;min-height:32px;min-width:0}
#satPanel .sat-sht button > span:last-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
#satPanel .sat-tick{flex:0 0 auto;width:14px;height:14px;border:1px solid var(--btn-border);border-radius:3px;box-sizing:border-box;position:relative;background:var(--btn-bg)}
#satPanel .sat-sht button[aria-checked="true"] .sat-tick{background:var(--btn-active-bg);border-color:var(--btn-active-bdr)}
#satPanel .sat-sht button[aria-checked="true"] .sat-tick::after{content:'';position:absolute;left:4px;top:1px;width:4px;height:8px;
  border:solid var(--btn-active-tx);border-width:0 2px 2px 0;transform:rotate(45deg)}
#satPanel .sat-row{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:2px 10px;width:100%;text-align:left;border-radius:6px;
  margin:3px 0;padding:6px 8px;min-height:40px}
#satPanel .sat-row .sat-n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#satPanel .sat-row .sat-w{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
#satPanel .sat-row .sat-f{grid-column:1/-1;color:var(--text-dim);font-size:12px}
#satPanel .sat-row.grey{opacity:.5;cursor:default}
#satPanel .sat-row.up .sat-w{color:var(--amber)}
/* ★★ THE PASS's RATING, shaded like n2yo's legend (Stuart 11-21) — from the pass's highest elevation (satPassQuality) */
#satPanel .sat-q{display:inline-block;padding:0 5px;border-radius:4px;margin-left:2px;font-variant-numeric:tabular-nums;color:var(--btn-text)}
#satPanel .sat-q.q-excellent{background:rgba(255,176,0,.42)}
#satPanel .sat-q.q-good{background:rgba(255,176,0,.24)}
#satPanel .sat-q.q-marginal{background:rgba(255,176,0,.10)}
#satPanel .sat-q.q-poor{color:var(--text-dim)}
/* ★★ 1.6 REMIND — the key beside each pass row (hidden where a pick would disturb anyone), the lead-time choice */
#satPanel .sat-rw{display:flex;gap:6px;align-items:stretch}
#satPanel .sat-rw .sat-row{flex:1 1 auto;min-width:0}
#satPanel .sat-rem{flex:0 0 auto;align-self:center;font-size:11px !important;padding:2px 7px !important;min-height:30px;letter-spacing:.04em;white-space:nowrap}
/* ★★★ beta96 SCHEDULE: the row's schedule line, the keys' pair, the session / overlap / status lines */
#satPanel .sat-row .sat-s{grid-column:1/-1;color:var(--amber);font-size:12px}
#satPanel .sat-row .sat-s.skip{color:var(--text-dim)}
#satPanel .sat-sk{display:flex;flex-direction:column;gap:4px;align-self:center}
#satPanel .sat-sline{font-size:12px;margin:6px 0 2px}
#satPanel .sat-watch{margin:2px 0 8px;padding:6px 8px;border:1px solid var(--btn-border);border-radius:6px}
#satPanel .sat-watch .sat-wh{font-weight:600;font-size:13px}
#satPanel .sat-watch .sat-wp{font-size:12px;margin:2px 0;opacity:.85}
#satPanel .sat-watch .sat-wp.live{opacity:1;font-weight:600}
#satPanel .sat-watch .sat-wk{margin-top:6px;display:flex;gap:6px;flex-wrap:wrap;align-items:center}
#satPanel .sat-only{font-size:11px;opacity:.75;padding:0 4px}
#satPanel .sat-lead{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:6px 0 2px;font-size:12px}
#satPanel .sat-lead button{min-height:26px;padding:1px 8px}
#satPanel .sat-key{font-size:11px;text-align:center;margin-top:2px}
#satPanel .sat-key[hidden]{display:none}
#satPanel .sat-tlm{margin-top:6px;font:12px/1.35 ui-monospace,Menlo,monospace}
#satPanel .sat-tlm[hidden]{display:none}
#satPanel .sat-tlm .sat-tk{display:inline-flex;gap:6px;margin-left:6px}
#satPanel .sat-tlm .sat-tv{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:1px 12px;margin-top:3px}
/* ★★★ beta94 THE TRANSPONDER FINDER: a row per signal — frequency, what it is, SNR and when heard; LISTEN on a voice; a CW
   row's decoded text under it; a row unheard for a minute faded; the followed row lit */
#satPanel .sat-xp{margin-top:6px;font:12px/1.35 ui-monospace,Menlo,monospace}
#satPanel .sat-xp[hidden]{display:none}
#satPanel .sat-xp .sat-cav{font-family:system-ui,sans-serif;margin:2px 0}
#satPanel .sat-xp .sat-note{font-family:system-ui,sans-serif;color:var(--amber);font-size:12px}
#satPanel .sat-xp .sat-xr{border-top:1px solid var(--bar-border,var(--btn-border));padding:3px 0;overflow-wrap:anywhere}
#satPanel .sat-xp .sat-xr.faded{opacity:.45}
#satPanel .sat-xp .sat-xr.on .sat-xf{color:var(--amber)}
#satPanel .sat-xp .sat-xr button{min-height:26px;padding:1px 8px;margin-left:4px}
#satPanel .sat-xp .sat-xt{color:var(--text-dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#satPanel .sat-xp .sat-xs{padding:3px 0;overflow-wrap:anywhere}
#satPanel .sat-xp .sat-xs button{min-height:26px;padding:1px 8px}
#satPanel .sat-xp .sat-xh{font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;padding:1px 0}
#satPanel .sat-xp .sat-xh.faded{color:var(--text-dim)}
/* ★★ 2026-11-24 ORBCOMM: the birds, the carriers' manual fallback, the sky plot, the packets (subscriber messages raw) */
#satPanel .sat-orb{margin-top:6px;font:12px/1.35 ui-monospace,Menlo,monospace}
#satPanel .sat-orb[hidden]{display:none}
#satPanel .sat-orb .sat-tk{display:inline-flex;gap:6px;margin-left:6px}
#satPanel .sat-orb .sat-cav{font-family:system-ui,sans-serif;margin:2px 0}
#satPanel .sat-orb .sat-note{font-family:system-ui,sans-serif;color:var(--text-dim);font-size:12px}
#satPanel .sat-orb .sat-bird{border-top:1px solid var(--bar-border,var(--btn-border));padding-top:3px;margin-top:3px;overflow-wrap:anywhere}
#satPanel .sat-orb .sat-bird > div:first-child{color:var(--amber)}
#satPanel .sat-orb .sat-bird.held{opacity:.5}
#satPanel .sat-orb .sat-car{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:6px 0 2px;font-family:system-ui,sans-serif;font-size:12px;color:var(--text-dim)}
#satPanel .sat-orb .sat-car button{min-height:26px;padding:1px 8px}
#satPanel .sat-orb .sat-sky[hidden]{display:none}
#satPanel .sat-orb .sat-sky svg{display:block;width:100%;max-width:260px;height:auto;margin:4px auto 0;color:var(--btn-text)}
#satPanel.big .sat-orb .sat-sky svg{max-width:360px}
#satPanel .sat-orb .sky-modes{display:flex;justify-content:center;gap:6px;margin:4px 0;font-family:system-ui,sans-serif;font-size:12px}
#satPanel .sat-orb .sky-modes button{min-height:26px;padding:1px 8px}
#satPanel .sat-orb .sat-leg{display:flex;flex-wrap:wrap;justify-content:center;gap:2px 10px;font:11px system-ui,sans-serif;color:var(--text-dim);margin-top:2px}
#satPanel .sat-orb .sat-leg i{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:4px;vertical-align:-1px}
#satPanel .sat-orb .sat-skys{text-align:center;font:11px system-ui,sans-serif;color:var(--text-dim)}
#satPanel .sat-orb .sat-pkh{font-family:system-ui,sans-serif;letter-spacing:.06em;color:var(--text-dim);margin-top:6px}
#satPanel .sat-orb .sat-pks{max-height:12em;overflow:auto}
#satPanel .sat-orb .sat-hex{color:var(--text-dim);padding-left:10px;word-break:break-all}
/* ★ the pop-ups: decoder-box styles, top of the window, newest on top, three at most ("+2 more") */
#satRemind{position:fixed;z-index:60;top:calc(env(safe-area-inset-top,0px) + 10px);left:50%;transform:translateX(-50%);
  width:min(94vw,460px);display:flex;flex-direction:column;gap:6px;pointer-events:none}
#satRemind[hidden]{display:none}
#satRemind .sat-toast{pointer-events:auto;background:var(--ov-panel96,rgba(8,6,2,.96));color:var(--btn-text);border:1px solid var(--btn-border);
  border-radius:10px;padding:8px 10px;font:13px/1.35 system-ui,sans-serif;box-shadow:0 12px 40px rgba(0,0,0,.6)}
#satRemind .sat-toast .sat-tw{color:var(--text-dim);font-size:12px;margin-top:2px}
#satRemind .sat-toast .sat-tb{display:flex;gap:8px;justify-content:flex-end;margin-top:6px}
#satRemind button{background:var(--btn-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:6px;padding:4px 10px;
  font:inherit;cursor:pointer;min-height:32px}
#satRemind button.on{background:var(--btn-active-bg);border-color:var(--btn-active-bdr);color:var(--btn-active-tx)}
#satRemind .sat-more{pointer-events:auto;text-align:center;color:var(--text-dim);font:12px system-ui,sans-serif}
/* ★ A phone: the box's own width, and SMALL leaves the waterfall above it to tune by */
@media (max-width:760px){
  #satPanel{left:8px;right:8px;width:auto}
  #satPanel.big{width:auto}
  #satPanel .sat-body{max-height:min(30vh,240px)}
  #satPanel.big .sat-body{max-height:min(62vh,560px)}
  #satPanel canvas.sat-globe{width:120px}
}
`;

const esc = (s: string) => s.replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' } as Record<string, string>)[c]);

/** ★ The row's pass rating, shaded like n2yo's legend (satPassQuality: the pass's highest elevation). */
function qualityHtml(p: SatPass | undefined): string {
  if (!p) return '';
  const q = satPassQuality(p.maxEl);
  return ` · <span class="sat-q q-${q.toLowerCase()}" title="How well this pass should come in: the higher it climbs, the closer and clearer it is">${esc(satPassText(p))}</span>`;
}

/** ★★ beta96 the CW box's EXPLAIN setting (main.ts CW_PLAIN_KEY 'vibe.cwPlain'), read and written here for the transponder rows;
 *  main.ts is told (vibe:cwplain) so its own key and text follow at once. */
function xpdrCwPlain(): boolean { try { return localStorage.getItem('vibe.cwPlain') === '1'; } catch { return false; } }
function xpdrCwPlainSet(on: boolean) {
  try { localStorage.setItem('vibe.cwPlain', on ? '1' : '0'); } catch { /* not stored: this page only */ }
  try { window.dispatchEvent(new CustomEvent('vibe:cwplain', { detail: on })); } catch { /* fine */ }
}

export class SatPanel {
  private el: HTMLDivElement;
  private list: SatList | null = null;
  private ro: SatReadout | null = null;
  private cap: { ok: boolean; why: string } | null = null;
  private timer = 0;
  private listAt = 0;
  /** ★ in the mode (the key lit) — the window IS the mode: it closes only by leaving it (EXIT, the key, another mode). */
  private on = false;
  private autoDoppler = true;
  private picked: SatPicked | null = null;
  private pending: { norad: number; tx: number } | null = null;
  private refusal = '';
  /** ★★★ beta96 single mode: satellites — this listener watches (no picks, no EXIT); the radio's timetable; the hybrid */
  private watch = false;
  private single: SatSingle | null = null;
  private hybrid: SatHybridInfo | null = null;
  private watchKey = '';
  private picture = false;
  private pictureDone = false;
  private decLine = '';
  private packets: string[] = [];
  private lent = false;
  /** ★ SMALL / BIG (the size key) and minimised (–), as the decoder box. BIG is grown once when a picture starts, unless the
   *  listener chose SMALL for this pick (`sized`). */
  private big = false;
  private sized = false;
  /** the size last chosen BY HAND — a new pick opens at it (an automatic grow for a picture is not carried over) */
  private handBig = false;
  private min = false;
  /** ★ 1.7 the pick's pass path (the track reply), and which pass it was asked for (asked once per pick, again when the
   *  next pass begins) */
  private track: SatTrack | null = null;
  private trackAsk: { norad: number; aos: number } | null = null;
  /** ★ 1.3 the telemetry the pick's decoder last sent (fctlm) */
  private tlm: SatTelemetry | null = null;
  /** ★★ 1.6 REMIND — held in this page only (never stored, never on the server); the pop-ups that have fired */
  private rem = new SatReminders();
  private toasts: Array<{ r: SatReminder; why: string; shownAt: number; note?: string }> = [];
  /** ★★★ beta96 SCHEDULE — kept in this browser per server (SAT_SCHED_KEY); the run (where the radio is, what goes back);
   *  the degraded reminders already fired; the last thing the schedule has to say */
  private sched = new SatSchedule();
  private schedRun: SatSchedRun = SAT_SCHED_IDLE;
  private schedFired = new Set<string>();
  private schedNote = '';
  private remTimer = 0;
  private rowsKey = '';                    // ★ the timetable's markup with the countdowns left out — see render()
  private rowWhen: HTMLElement[] = [];     // ★ each row's countdown, written in place between rebuilds
  private globeKey = '';                   // ★ what the half-globe last drew — see drawGlobe()
  private lead = 2;
  /** ★★ beta94 SHOW — which satellites' passes the list shows (null = All), remembered in this browser by NORAD number */
  private showF: SatShow = null;
  private sShow!: HTMLDetailsElement; private showKey = ''; private sShowNote!: HTMLElement;
  private remEl: HTMLDivElement;
  private now(): number { return (this.deps.now ? this.deps.now() : Date.now()) / 1000; }
  private srv(): string { return this.deps.serverKey?.() || location.host || 'server'; }
  // the skeleton's slots — built once, filled every second (the lent image canvas must never be re-created)
  private sHead!: HTMLElement; private sHs!: HTMLElement; private sDot!: HTMLElement; private sWhy!: HTMLElement; private sBox!: HTMLElement; private sRo!: HTMLElement;
  private sUp!: HTMLElement;   // ★ beta91 "Downlink · Up 145.990 MHz · 67 Hz tone" under the big (downlink) frequency
  private sName!: HTMLElement; private sHz!: HTMLElement; private sCd!: HTMLElement; private sState!: HTMLElement;
  private sGlobe!: HTMLCanvasElement; private sImg!: HTMLElement; private sPk!: HTMLElement; private sSync!: HTMLElement;
  private sList!: HTMLDetailsElement; private sRows!: HTMLElement; private sCredit!: HTMLElement;
  private sKey!: HTMLElement; private sTlm!: HTMLElement; private sMap!: HTMLButtonElement;
  // ★★★ beta92 OPEN IN VFO B — the keys / B's line, A's CW text, B's picture
  private sVb!: HTMLElement; private sCw!: HTMLElement; private sBImg!: HTMLElement; private bCanvas!: HTMLCanvasElement;
  private vbKey = '';
  /** the server's word on VFO B for this pick (on) — null = none open */
  private vfob: SatVfoB | null = null;
  /** which line's decoder the area shows while the switch exists (satViewSwitch) */
  private view: 'A' | 'B' = 'A';
  private aText = '';                       // ★ fix beta92: a CW beacon on A shows its decoded text
  /** ★★ beta92 a CW beacon's skimmer list (A, and B when B is on a beacon) — the box shows it nearest the prediction first */
  private aBeacon: SatBeaconList | null = null; private bBeacon: SatBeaconList | null = null;
  private bText = ''; private bPackets: string[] = []; private bStatus = ''; private bPicture = false; private bPictureDone = false;
  // ★★ 2026-11-24 ORBCOMM — the decoder's state + packets, the manual fallback, the sky plot (stored dots + live)
  private sOrb!: HTMLElement; private oTxt!: HTMLElement; private oSky!: HTMLElement; private oPk!: HTMLElement;
  private sWatch!: HTMLElement; private sOnly!: HTMLElement;   // ★★★ beta96 the watch view, the note in place of EXIT
  private sXp!: HTMLElement; private xp: XpdrStatus | null = null; private xpKey = '';   // ★★★ beta94 the transponder finder
  private orb: OrbView | null = null;
  private orbMaxK = 0;
  private orbSky = true;
  private skyDots: SkyDot[] = [];
  private skyNote = '';
  // ★★★ beta94 the sky by position + the view (AUTO: the heatmap once there is enough, the live pass a track on top)
  private skyCells: SkyCells | null = null;
  private skyMode: SkyMode = 'auto';
  private skySeq = 0;
  private oTxtKey = ''; private oSkyKey = ''; private oPkKey = '';

  constructor(private deps: SatPanelDeps) {
    if (!document.getElementById('satPanelCss')) {
      const st = document.createElement('style'); st.id = 'satPanelCss'; st.textContent = CSS; document.head.appendChild(st);
      tlmEnsureCss();   // ★★ beta91 TELEMETRY CHARTS
    }
    this.el = document.createElement('div');
    this.el.id = 'satPanel'; this.el.hidden = true;
    this.el.setAttribute('role', 'dialog'); this.el.setAttribute('aria-label', 'Satellite');
    this.el.innerHTML = `<div class="sat-h"><span class="sat-dot"></span><b>SATELLITE</b><span class="sat-hs" data-s="hs"></span>`
      + `<button data-act="map" hidden title="The stations heard through this satellite, on the DIGI MODES map">MAP</button>`
      + `<button data-act="doppler" class="on" aria-pressed="true" title="AUTO DOPPLER: keep the satellite centred for the whole pass">AUTO DOPPLER</button>`
      + `<button data-act="size" class="sat-size" title="Bigger panel" aria-label="Bigger panel"><svg viewBox="0 0 12 14" aria-hidden="true"><rect x="1.5" y="7" width="9" height="6"/><path d="M6 1 L6 5 M4 3 L6 1 L8 3"/></svg></button>`
      + `<button data-act="exit" title="Leave SATELLITE (the Doppler lock and the decoder stop)">EXIT</button>`
      + `<span class="sat-only" data-s="only" hidden title="${esc(SAT_SINGLE_WHY)}">${esc(SAT_SINGLE_ONLY)}</span>`
      + `<button data-act="min" title="Minimise" aria-label="Minimise">–</button></div>`
      + `<div class="sat-body">`
      + `<div class="sat-watch" data-s="watch" hidden></div>`
      + `<div class="sat-why" data-s="why" hidden></div>`
      + `<div class="sat-box" data-s="box" hidden><div class="sat-ro" data-s="ro"><div>`
      +   `<div class="sat-name" data-s="name"></div><div class="sat-big" data-s="hz"></div><div class="sat-dim" data-s="up" hidden></div>`
      +   `<div class="sat-cd" data-s="cd"></div><div class="sat-dim sat-ln" data-s="state"></div><div class="sat-vb" data-s="vb" hidden></div></div>`
      +   `<div><canvas class="sat-globe" data-s="globe" width="640" height="300" aria-label="Where the satellite is: above the globe, its beam to the ground, the pass's orbit dotted with arrows for its direction"></canvas>`
      +   `<div class="sat-key sat-dim" data-s="key" hidden></div></div></div>`
      + `<div class="sat-img" data-s="img" hidden></div><div class="sat-cw" data-s="cw" hidden></div><div class="sat-pk" data-s="pk" hidden></div><div class="sat-tlm" data-s="tlm" hidden></div>`
      + `<div class="sat-orb" data-s="orb" hidden><div data-o="txt"></div><div class="sat-sky" data-o="sky"></div><div data-o="pk"></div></div>`
      + `<div class="sat-xp" data-s="xp" hidden></div>`
      + `<div class="sat-img sat-bimg" data-s="bimg" hidden><canvas width="320" height="256" aria-label="The picture VFO B is receiving"></canvas></div></div>`
      + `<div class="sat-sync sat-dim" data-s="sync"></div>`
      + `<details class="sat-list" data-s="list" open><summary>SATELLITES</summary>`
      +   `<details class="sat-show" data-s="show" hidden><summary title="Which satellites' passes the list shows"><span class="sat-shv"></span></summary><div class="sat-shb"></div></details>`
      +   `<div class="sat-shn sat-dim" data-s="shownote" hidden></div>`
      +   `<div data-s="rows"></div></details>`
      + `<div class="sat-dim sat-credit" data-s="credit"></div></div>`;
    const q = <T extends HTMLElement>(n: string) => this.el.querySelector(`[data-s="${n}"]`) as T;
    this.sHead = this.el.querySelector('.sat-h') as HTMLElement;
    this.sHs = q('hs'); this.sDot = this.el.querySelector('.sat-dot') as HTMLElement;
    this.sWhy = q('why'); this.sBox = q('box'); this.sRo = q('ro'); this.sName = q('name'); this.sHz = q('hz'); this.sUp = q('up'); this.sCd = q('cd');
    this.sState = q('state'); this.sGlobe = q<HTMLCanvasElement>('globe'); this.sImg = q('img'); this.sPk = q('pk'); this.sSync = q('sync');
    this.sList = q<HTMLDetailsElement>('list'); this.sRows = q('rows'); this.sCredit = q('credit');
    this.sShow = q<HTMLDetailsElement>('show'); this.sShowNote = q('shownote');
    this.sKey = q('key'); this.sTlm = q('tlm'); this.sMap = this.el.querySelector('[data-act="map"]') as HTMLButtonElement;
    this.sVb = q('vb'); this.sCw = q('cw'); this.sBImg = q('bimg'); this.bCanvas = this.sBImg.querySelector('canvas') as HTMLCanvasElement;
    this.sOrb = q('orb');
    this.sXp = q('xp');
    this.sWatch = q('watch'); this.sOnly = q('only');
    this.oTxt = this.sOrb.querySelector('[data-o="txt"]') as HTMLElement; this.oSky = this.sOrb.querySelector('[data-o="sky"]') as HTMLElement;
    this.oPk = this.sOrb.querySelector('[data-o="pk"]') as HTMLElement;
    // ★ 1.6 the reminders' pop-ups — their own place at the top of the window (they fire with the mode off too)
    this.remEl = document.createElement('div');
    this.remEl.id = 'satRemind'; this.remEl.hidden = true; this.remEl.setAttribute('aria-live', 'polite');
    document.body.appendChild(this.remEl);
    this.remEl.addEventListener('click', (e) => this.onToastClick(e));
    try { this.lead = satRemindLead(localStorage.getItem('vibe.satRemindLead')); } catch { /* private window: the default */ }
    try { this.showF = satShowParse(localStorage.getItem(SAT_SHOW_KEY)); } catch { /* private window: All */ }
    try { this.sched = SatSchedule.fromJSON(localStorage.getItem(SAT_SCHED_KEY), this.now()); } catch { /* private window: none */ }
    queueMicrotask(() => this.remSchedule());   // ★★★ beta96 a stored schedule runs with the mode off
    // ★ fixed to the window at #decBox's anchor (--decBoxBottom: above the control card; the waterfall runs to the bottom)
    document.body.appendChild(this.el);
    this.el.addEventListener('click', (e) => this.onClick(e));
    // ★ a tap on the header's empty part folds / unfolds it, as the app's decoder header does
    this.sHead.addEventListener('click', (e) => { if (!(e.target as HTMLElement).closest('button')) this.setMin(!this.min); });
  }

  /** In the SATELLITE mode (its key lit). */
  get active(): boolean { return this.on; }
  get open(): boolean { return !this.el.hidden; }
  /** The pick's downlink, once the server confirmed it. */
  pickedTx(): SatTx | undefined { return this.picked ? this.list?.sats.find(s => s.norad === this.picked!.norad)?.tx[this.picked.tx] : undefined; }
  pickedDecoder(): SatPicked['decoder'] | '' { return this.picked?.decoder ?? ''; }
  /** ★★★ beta94 THE BOX FOLLOWS THE DECODER (src/services/orbcomm.ts orbFollowChoice): the bird the box shows — the pick,
   *  or, for an ORBCOMM pick whose pass is over, the bird its whole-band decoder hears now (else the next to rise). */
  followNow(): OrbFollow | null {
    if (!this.picked) return null;
    if (this.picked.decoder !== 'orbcomm') return { norad: this.picked.norad, why: 'pick' };
    return orbFollowChoice(this.picked.norad, this.orb?.status, this.list, this.now());
  }
  /** the box's bird's NORAD number (the pick, or the followed ORBCOMM bird) */
  private boxNorad(): number | null { return this.followNow()?.norad ?? null; }
  /** hwinfo's `doppler` / `dopplerWhy` (satCapFrom). null = no key at all. */
  setCap(c: { ok: boolean; why: string } | null) { this.cap = c; if (!c) this.exit(true); else if (this.open) this.render(); }
  /** ★★★ beta96 single mode: satellites — watch only (hwinfo singleMode "sat", not the admin): no picks, no EXIT. */
  setWatch(on: boolean) {
    if (on === this.watch) return;
    this.watch = on; this.watchKey = '';
    if (!on) this.refusal = '';
    if (this.open) this.render();
  }
  get watching(): boolean { return this.watch; }
  /** {"type":"sat_single"} — the radio's pass now / next, its timetable, the last pass */
  onSingle(s: SatSingle) { this.single = s; this.hybrid = s.hybrid ?? null; if (this.open) this.render(); }
  /** {"type":"sat_hybrid"} — a linear transponder pass: this listener's own VFO across the passband (null = over) */
  onHybrid(h: SatHybridInfo | null) { this.hybrid = h; this.watchKey = ''; if (this.open) this.render(); }
  hybridNow(): SatHybridInfo | null { return this.hybrid; }
  /** Into the mode: the window opens, the list is asked for. */
  show() {
    if (!this.cap) return;
    this.on = true;
    this.el.hidden = false;
    this.deps.list(); this.listAt = Date.now();
    this.deps.entered?.();
    if (!this.timer) this.timer = window.setInterval(() => this.tick(), 1000);
    this.render();
  }
  /** ★ Out of the mode — the drop is always sent (a lock must never outlive the mode), the decoder stops, the canvas goes home. */
  exit(force = false) {
    // ★★★ beta96 single mode: satellites — no way out for a listener (the signed-in admin is never `watch`)
    if (this.watch && this.on && !force && this.cap) { this.refusal = SAT_SINGLE_WHY; if (this.open) this.render(); return; }
    const was = this.on;
    this.on = false;
    this.el.hidden = true;
    if (this.timer) { clearInterval(this.timer); this.timer = 0; }
    if (was) this.deps.drop();
    this.clearPick();
    if (was) this.deps.left();
  }
  toggle() { this.on ? this.exit() : this.show(); }
  private clearPick() {
    this.picked = null; this.pending = null; this.picture = false; this.pictureDone = false; this.decLine = ''; this.packets = [];
    this.track = null; this.trackAsk = null; this.tlm = null;
    this.aText = ''; this.aBeacon = null; this.vfobClear();
    this.orb = null; this.xp = null; this.skyDots = []; this.skyCells = null; this.skyNote = ''; this.skySeq++;
    this.lend(false);
  }
  private vfobClear() {
    this.vfob = null; this.view = 'A'; this.bText = ''; this.bBeacon = null; this.bPackets = []; this.bStatus = ''; this.bPicture = false; this.bPictureDone = false;
  }
  // ── ★★★ beta92 OPEN IN VFO B ────────────────────────────────────────────────────────────────────────────────────
  /** {"type":"sat_vfob"} — B on another downlink of the pick (on), a refusal (says why), or B gone (the line goes). */
  onVfoB(b: SatVfoB) {
    if (b.on) {
      if (!this.picked || b.norad !== this.picked.norad) return;
      const fresh = !this.vfob || this.vfob.tx !== b.tx;
      if (fresh) { this.bText = ''; this.bBeacon = null; this.bPackets = []; this.bStatus = ''; this.bPicture = false; this.bPictureDone = false; }
      this.vfob = b; this.refusal = '';
      if (fresh && satDecView(b.decoder)) this.view = 'B';   // ★ the line just opened is the one shown (a tap on A goes back)
    } else if (b.refused) {
      this.refusal = b.why || 'VFO B could not be opened on that downlink';
    } else {
      this.vfobClear();
    }
    if (this.open) this.render();
  }
  /** the VFO B downlink open now (tests, main.ts) */
  vfobNow(): SatVfoB | null { return this.vfob; }
  /** which line's decoder the box shows (tests) */
  viewNow(): 'A' | 'B' { return satViewShown(this.picked?.decoder, this.vfob, this.view); }
  /** A CW beacon's decoded characters on VFO A (the pick's own CW decoder). */
  cwText(t: string) { if (this.picked?.decoder !== 'cw') return; this.aText = satTextAppend(this.aText, t); if (this.open) this.paintPick(); }
  /** ★★ beta92 a CW beacon's skimmer list on VFO A (the pick's 24 kHz CW skimmer). */
  beaconList(l: SatBeaconList) { if (this.picked?.decoder !== 'cwskim') return; this.aBeacon = l; if (this.open) this.paintPick(); }
  /** …and on VFO B (a beacon opened in VFO B runs the same skimmer on B). */
  bBeaconList(l: SatBeaconList) { if (!this.vfob || this.vfob.decoder !== 'cwskim') return; this.bBeacon = l; if (this.open) this.paintPick(); }
  /** the A beacon area's text now (tests) */
  beaconTextNow(): string { return satBeaconText(this.aBeacon); }
  /** VFO B's decoder: its text (CW, telemetry lines), its own status words, its packet lines, its picture. */
  bTextAdd(t: string) { if (!this.vfob) return; this.bText = satTextAppend(this.bText, t); if (this.open) this.paintPick(); }
  bState(text: string) { if (!this.vfob) return; this.bStatus = text; if (this.open) this.paintPick(); }
  bPacket(line: string) { if (!this.vfob) return; this.bPackets.unshift(line); if (this.bPackets.length > 30) this.bPackets.length = 30; if (this.open) this.paintPick(); }
  bImageStart(w: number, h: number) {
    if (!this.vfob) return;
    const c = this.bCanvas;
    c.width = Math.max(16, Math.min(2048, w || 320)); c.height = Math.max(16, Math.min(2048, h || 256));
    c.getContext('2d')?.clearRect(0, 0, c.width, c.height);
    this.bPicture = true; this.bPictureDone = false;
    if (this.viewNow() === 'B' && !this.sized && !this.big) this.setBig(true, false);   // ★ grows to show it, as A's picture does
    if (this.open) this.render();
  }
  bImageLine(y: number, w: number, px: Uint8Array, rgb: boolean) {
    if (!this.vfob || w <= 0 || y < 0) return;
    const c = this.bCanvas, ctx = c.getContext('2d');
    if (!ctx) return;
    if (c.width !== w) { c.width = Math.min(2048, w); }
    if (y >= c.height) {   // ★ grows with the picture (a size it did not say)
      const keep = ctx.getImageData(0, 0, c.width, c.height);
      c.height = Math.min(2048, Math.max(y + 1, c.height * 2)); ctx.putImageData(keep, 0, 0);
    }
    const n = Math.min(w, c.width), row = ctx.createImageData(n, 1);
    for (let x = 0; x < n; x++) {
      const o = x * 4;
      if (rgb) { row.data[o] = px[x * 3] ?? 0; row.data[o + 1] = px[x * 3 + 1] ?? 0; row.data[o + 2] = px[x * 3 + 2] ?? 0; }
      else { const v = px[x] ?? 0; row.data[o] = v; row.data[o + 1] = v; row.data[o + 2] = v; }
      row.data[o + 3] = 255;
    }
    ctx.putImageData(row, 0, y);
    if (!this.bPicture) { this.bPicture = true; if (this.open) this.render(); }
  }
  bImageDone() { if (!this.vfob) return; this.bPictureDone = true; if (this.open) this.paintPick(); }
  onList(l: SatList) {
    const wasFetching = this.list?.source?.fetching;
    this.list = l;
    this.rem.refresh(this.srv(), l);   // ★ a new element set moves a reminder with its pass
    if (this.sched.all(this.srv()).length) { this.sched.refresh(this.srv(), l); this.schedSave(); this.remSchedule(); }   // ★★★ beta96 … and a scheduled pass
    if (wasFetching && !l.source?.fetching) this.listAt = 0;
    if (this.open) this.render();
  }
  /** performance.now() of the last readout — the waterfall marker moves on at its rate between readouts. */
  readoutAt = 0;
  onReadout(r: SatReadout) {
    this.ro = r.state === 'OFF' && !r.why ? null : r; this.readoutAt = performance.now();
    /* ★★★ beta92 THE PICK WENT WITH THE LOCK (a tune past its reach, a mode change — the server detached its decoder): the
     *  box stops showing it. It said "TRACK · attached" for a pick that had gone (Stuart 11-23). Why, in its words. */
    if (r.pickGone && this.picked) {
      this.picked = null; this.pending = null; this.picture = false; this.pictureDone = false; this.decLine = ''; this.packets = [];
      this.track = null; this.trackAsk = null; this.tlm = null; this.aText = ''; this.aBeacon = null; this.vfobClear(); this.lend(false);
      this.orb = null; this.xp = null; this.skyDots = []; this.skyCells = null; this.skyNote = ''; this.skySeq++;
      this.refusal = r.why || 'The satellite pick ended';
      this.deps.pickGone?.();
      this.sList.open = true;
    }
    if (r.state === 'OFF' && r.why && this.pending) { this.refusal = r.why; this.pending = null; }   // ★ a refused pick says why
    if (this.open) this.render();
  }
  /** {"state":"picked"} — the server tuned, locked (if asked) and attached the decoder. */
  onPicked(p: SatPicked) {
    if (!this.on) return;
    const changed = !this.picked || this.picked.norad !== p.norad || this.picked.tx !== p.tx;
    /* ★★★ beta94 another ORBCOMM bird keeps the server's whole-band decoder (satPick): its birds, packets and sky dots stay,
     *  and the view (already the band) is not zoomed again */
    const orbToOrb = changed && this.picked?.decoder === 'orbcomm' && p.decoder === 'orbcomm';
    if (changed) { this.aText = ''; this.aBeacon = null; this.vfobClear(); this.picture = false; this.pictureDone = false; this.packets = []; this.decLine = ''; this.sized = false; this.track = null; this.trackAsk = null; this.tlm = null; if (this.big !== this.handBig) this.setBig(this.handBig, false); if (this.tlmCharts) this.tlmLoad(); }
    if (changed && !orbToOrb) { this.orb = null; this.xp = null; this.skyDots = []; this.skyCells = null; this.skyNote = ''; this.skySeq++; }
    this.picked = p; this.pending = null; this.refusal = '';
    if (changed && !orbToOrb && p.decoder === 'orbcomm') this.skyLoad();
    const t = this.pickedTx();
    if (t) this.deps.follow(p.decoder, t, p.params, p.audio);   // ★★ beta92 the server's own parameters (and passband): the page JOINS its run
    if (changed && !orbToOrb && t && p.view) this.deps.zoomView?.(p.viewCentre ?? txTuneHz(t), p.view);   // ★★ an ORBCOMM pick: the band's centre   // ★★ beta93 open at the pick's own view
    if (changed) this.sList.open = false;   // ★ the list folds away under the box once a bird is picked (one tap opens it again) — ★★★ beta94 only on a NEW pick: the room's pick re-told to a joiner (satTellRoomPick) leaves it as the listener set it
    this.render();
  }
  /** The decoder's own words (LRPT's carrier / frames / lines, SSTV's mode, APRS listening). */
  status(text: string) { this.decLine = text; if (this.open) this.paintPick(); }
  /** A picture started (LRPT's preview, an ISS SSTV frame): the box grows to show it. */
  pictureStart() {
    if (!this.picked) return;
    this.picture = true; this.pictureDone = false;
    if (!this.sized && !this.big) this.setBig(true, false);   // ★ grows to show it, once — unless SMALL was chosen for this pick
    if (this.min) this.setMin(false);
    if (this.open) this.render();
  }
  pictureEnd() { if (!this.picked) return; this.pictureDone = true; if (this.open) this.paintPick(); }
  /** An APRS line heard on a packet downlink. */
  packet(line: string) {
    if (!this.picked) return;
    this.packets.unshift(line); if (this.packets.length > 30) this.packets.length = 30;
    if (this.open) this.paintPick();
  }
  /** ★ 1.7 the pass's path: its `geo` is the dotted orbit (an older server sends none — no line). */
  onTrack(t: SatTrack) {
    if (!this.picked || t.norad !== this.boxNorad()) return;   // ★★★ beta94 the followed ORBCOMM bird's path too
    this.track = t;
    if (this.open) this.paintPick();
  }
  /** ★ 1.3 the pick's telemetry (fctlm_status counts, sat_tlm values) — merged */
  telemetry(t: Partial<SatTelemetry> | null) {
    if (!this.picked) return;
    const was = this.tlm?.sat || '';
    this.tlm = satTelemetryMerge(this.tlm, t);
    if (this.tlmCharts && (this.tlm?.sat || '') !== was) this.tlmLoad();   // ★ beta91 the satellite's own passes, once it names itself
    if (this.open) this.paintPick();
  }
  /** ★★ 2026-11-24 ORBCOMM: {"type":"orbcomm_status"|"orbcomm_pkt","data":…} from the pick's decoder (parsed in orbcomm.ts) */
  orbcomm(type: 'orbcomm_status' | 'orbcomm_pkt', data: unknown) {
    if (this.picked?.decoder !== 'orbcomm') return;
    if (type === 'orbcomm_status') {
      const st = parseOrbcommStatus(data);
      if (!st) return;
      this.orb = orbViewMerge(this.orb, { status: st });
      if (st.dots.length) { this.skyDots = skyPlotMerge(this.skyDots, st.dots); this.skyCells = skyCellsAddDots(this.skyCells, st.dots); }
    } else {
      const pk = parseOrbcommPkt(data);
      if (!pk) return;
      this.orb = orbViewMerge(this.orb, { pkt: pk });
    }
    if (this.open) this.paintPick();
  }
  /** ★★★ beta94 {"type":"xpdr_status","data":…} from the pick's transponder finder (parsed in src/services/xpdr.ts) */
  xpdr(data: unknown) {
    if (this.picked?.decoder !== 'xpdr') return;
    const st = parseXpdrStatus(data);
    if (!st) return;
    this.xp = st;
    if (this.open) this.paintPick();
  }
  /** the transponder finder's state now (tests, and the waterfall's overlay) */
  xpdrNow(): XpdrStatus | null { return this.picked?.decoder === 'xpdr' ? this.xp : null; }
  /** the ORBCOMM state now (tests) */
  orbNow(): OrbView | null { return this.orb; }
  /** This radio's stored ORBCOMM dots (the server's skyplot.json), read once per pick; live dots join as they come. */
  private skyLoad() {
    const seq = ++this.skySeq;
    if (!this.deps.skyGet) { this.skyNote = 'This server has no sky plot to read'; return; }
    this.deps.skyGet(skyPlotQuery('orbcomm', this.deps.skyRadio?.() ?? '')).then((txt) => {
      if (seq !== this.skySeq) return;
      let d = null; try { d = txt ? parseSkyPlotJson(JSON.parse(txt)) : null; } catch { d = null; }
      if (d) { this.skyDots = skyPlotMerge(d.dots, this.skyDots); this.skyCells = d.cells; } else this.skyNote = 'The stored sky plot could not be read — live dots only';
      if (this.open) this.paintPick();
    }).catch(() => { if (seq === this.skySeq) { this.skyNote = 'The stored sky plot could not be read — live dots only'; if (this.open) this.paintPick(); } });
  }
  private tlmRaw = false;
  // ★★ beta91 TELEMETRY CHARTS: CHARTS beside RAW — this pass (the live frames), the last passes (the gallery, read on opening)
  private tlmCharts = false;
  private tlmAll = false;
  private tlmAcross: TlmPass[] | null = null;
  private tlmAcrossNote = '';
  private tlmLoadSeq = 0;
  private tlmLoad() {
    const seq = ++this.tlmLoadSeq;
    this.tlmAcross = null; this.tlmAcrossNote = '';
    if (!this.deps.galleryGet) { this.tlmAcrossNote = 'LAST PASSES · this server has no gallery to read'; return; }
    tlmLoadPasses(this.deps.galleryGet, this.tlm?.sat || '').then((x) => { if (seq === this.tlmLoadSeq) { this.tlmAcross = x; if (this.open) this.paintPick(); } })
      .catch(() => { if (seq === this.tlmLoadSeq) { this.tlmAcrossNote = 'LAST PASSES · the gallery could not be read'; if (this.open) this.paintPick(); } });
  }
  /** ★ 1.9 the packet decoder heard a station with a position — the MAP key may appear */
  stationsChanged() { if (this.open) this.paintPick(); }
  /** ★ 1.6 Leaving the server / the session ending cancels every reminder for it (and its pop-ups). */
  remindClear() { this.rem.clearServer(this.srv()); this.toasts = []; this.schedRun = SAT_SCHED_IDLE; this.paintToasts(); this.remSchedule(); if (this.open) this.render(); }
  /** ★★★ beta96 the passes scheduled on this server (tests) */
  scheduled() { return this.sched.all(this.srv()); }
  /** ★★★ beta96 the session's key kind now (tests) */
  sessionKind() { return this.session().kind; }
  /** the reminders held (tests, and the page's own checks) */
  reminders(): SatReminder[] { return this.rem.all(); }
  readout(): SatReadout | null { return this.ro && this.ro.state !== 'OFF' ? this.ro : null; }

  private tick() {
    if (!this.open) return;
    this.askTrack();
    if (Date.now() - this.listAt > satListEveryMs(this.list)) { this.deps.list(); this.listAt = Date.now(); }
    this.render();
  }

  private onClick(e: Event) {
    // ★★★ beta92 the line switch — a tap on A's or B's line chooses which decoder the box shows (only while it exists)
    const ln = (e.target as HTMLElement).closest('[data-act="view"]') as HTMLElement | null;
    if (ln && ln.classList.contains('sw')) { this.view = ln.dataset.v === 'B' ? 'B' : 'A'; this.paintPick(); return; }
    const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
    if (!b || b.disabled) return;
    const act = b.dataset.act;
    if (act === 'exit') { this.exit(); return; }
    if ((act === 'remind' || act === 'sched' || act === 'prio') && this.watch) { this.refusal = SAT_SINGLE_WHY; this.render(); return; }
    if (act === 'min') { this.setMin(!this.min); return; }
    if (act === 'size') { this.setBig(!this.big, true); return; }
    if (act === 'map') { this.openMap(); return; }
    if (act === 'vfob') { if (this.picked && this.deps.vfobOpen) { this.refusal = ''; this.deps.vfobOpen(this.picked.norad, Number(b.dataset.tx)); } return; }
    if (act === 'tlmraw') { this.tlmRaw = !this.tlmRaw; if (this.tlmRaw) this.tlmCharts = false; this.paintPick(); return; }
    if (act === 'tlmcharts') { this.tlmCharts = !this.tlmCharts; if (this.tlmCharts) { this.tlmRaw = false; this.tlmLoad(); } this.paintPick(); return; }
    if (act === 'tlmall') { this.tlmAll = !this.tlmAll; this.paintPick(); return; }
    if (act === 'orbsky') { this.orbSky = !this.orbSky; this.paintPick(); return; }
    if (act === 'skymode') { this.skyMode = skyModeOf(b.dataset.v); this.paintPick(); return; }
    if (act === 'xexplain') { xpdrCwPlainSet(!xpdrCwPlain()); this.xpKey = ''; this.render(); return; }   // ★★ beta96
    if (act === 'xskim') { const k = this.xp?.skim; if (k && this.deps.xpdrSkim) { this.deps.xpdrSkim(!k.on); k.on = !k.on; this.xpKey = ''; this.render(); } return; }   // ★★★ beta97
    if (act === 'xlisten') { const r = this.xp?.rows.find((x) => x.id === b.dataset.xid); if (r && this.deps.xpdrListen) this.deps.xpdrListen(r); return; }
    if (act === 'orbmax') { this.orbMaxK = Math.max(0, Math.min(6, Number(b.dataset.v) || 0)); this.deps.orbMax?.(this.orbMaxK); this.paintPick(); return; }
    if (act === 'show') { this.setShow(satShowGroup((b.dataset.g || 'all') as SatShowGroup | 'all', this.showF)); return; }
    if (act === 'showq') { this.setShow(satShowWithQ(this.showF, (b.dataset.q || null) as SatShowQ | null)); return; }
    if (act === 'showsat') { this.setShow(satShowToggle(this.showF, this.list, Number(b.dataset.norad))); return; }
    if (act === 'remind') { this.remindToggle(Number(b.dataset.norad), Number(b.dataset.tx)); return; }
    if (act === 'sched') { this.schedToggle(Number(b.dataset.norad), Number(b.dataset.tx), Number(b.dataset.aos)); return; }
    if (act === 'prio') { if (this.sched.prioritise(b.dataset.key || '')) { this.schedSave(); this.render(); } return; }
    if (act === 'lead') {
      this.lead = satRemindLead(b.dataset.v);
      try { localStorage.setItem('vibe.satRemindLead', String(this.lead)); } catch { /* not kept: fine */ }
      this.remSchedule(); this.render(); return;
    }
    if (act === 'doppler') {
      this.autoDoppler = !this.autoDoppler;
      if (this.picked) this.deps.doppler(this.autoDoppler);
      this.render(); return;
    }
    if (act === 'gallery') { this.deps.openGallery?.(); return; }
    if (act === 'pick' && this.watch && !(this.hybrid && Number(b.dataset.norad) === this.hybrid.norad)) { this.refusal = SAT_SINGLE_WHY; this.render(); return; }
    if (act === 'pick') {
      if (b.getAttribute('aria-disabled') === 'true') { this.refusal = b.dataset.why || ''; this.render(); return; }   // ★ greyed: says why, starts nothing
      const norad = Number(b.dataset.norad), i = Number(b.dataset.tx);
      if (!this.list?.sats.find(s => s.norad === norad)?.tx[i]) return;
      if (this.picked && this.picked.norad === norad && this.picked.tx === i) { this.sList.open = false; this.render(); return; }
      if (this.deps.pick(norad, i, this.autoDoppler)) { this.pending = { norad, tx: i }; this.refusal = ''; }
      this.render();
    }
  }

  /** ★★ beta94 SHOW changed: kept in this browser (try/catch — a private window keeps it for the page only), list redrawn */
  private setShow(f: SatShow) {
    this.showF = f;
    try { const v = satShowStore(f); if (v == null) localStorage.removeItem(SAT_SHOW_KEY); else localStorage.setItem(SAT_SHOW_KEY, v); } catch { /* not kept: fine */ }
    this.render();
  }
  /** what SHOW is now (tests) */
  showNow(): SatShow { return this.showF; }
  /** the satellite that always stays in the list: the one picked, or being picked */
  private keepNorad(): number | null { return (this.picked ?? this.pending)?.norad ?? null; }
  /** ★★ beta94 the SHOW drop-down: hidden while the list has no satellites; its body rebuilt only when what it says changes
   *  (it stays open across the second-by-second redraws) */
  private paintShow() {
    const choices = satShowChoices(this.showF, this.list);
    this.sShow.hidden = !choices.length;
    // ★★ beta94 a reminder on a pass SHOW hides still fires — said under SHOW so it is not forgotten (Stuart 11-25)
    const note = choices.length ? satShowHiddenRemindText(this.showF, this.list, this.rem.all().filter((r) => r.server === this.srv()), this.keepNorad()) : '';
    if (this.sShowNote.textContent !== note) this.sShowNote.textContent = note;
    this.sShowNote.hidden = !note;
    if (!choices.length) return;
    const txt = satShowText(this.showF, this.list);
    const groups = satShowGroupsOffered(this.list);
    const cur = satShowCur(this.showF), curQ = satShowQ(this.showF);
    let h = `<div class="sat-shg" role="group" aria-label="Show a group">`
      + `<button data-act="show" data-g="all" class="${cur === 'all' ? 'on' : ''}" aria-pressed="${cur === 'all'}" title="Every satellite's passes">All</button>`
      + groups.map((g) => { const d = SAT_SHOW_GROUPS.find((x) => x.id === g)!;
          return `<button data-act="show" data-g="${g}" class="${cur === g ? 'on' : ''}" aria-pressed="${cur === g}" title="${esc(d.what)}">${esc(d.label)}</button>`; }).join('')
      + `</div><div class="sat-shg" role="group" aria-label="Pass quality">`
      + `<button data-act="showq" data-q="" class="${!curQ ? 'on' : ''}" aria-pressed="${!curQ}" title="Passes of any quality">All passes</button>`
      + SAT_SHOW_QS.map((x) => `<button data-act="showq" data-q="${x.id}" class="${curQ === x.id ? 'on' : ''}" aria-pressed="${curQ === x.id}" title="${esc(x.what)}">${esc(x.label)}</button>`).join('')
      + `</div><div class="sat-sht">`
      + choices.map((c) => `<button data-act="showsat" data-norad="${c.norad}" role="checkbox" aria-checked="${c.on}" title="${esc(c.name)}"><span class="sat-tick" aria-hidden="true"></span><span>${esc(c.name)}</span></button>`).join('')
      + `</div>`;
    const key = txt + '\u0000' + h;
    if (key === this.showKey) return;
    this.showKey = key;
    (this.sShow.querySelector('.sat-shv') as HTMLElement).textContent = `SHOW: ${txt}`;
    this.sShow.classList.toggle('on', !!this.showF && txt !== 'All');
    (this.sShow.querySelector('.sat-shb') as HTMLElement).innerHTML = h;
  }

  private setMin(on: boolean) {
    this.min = on; this.el.classList.toggle('min', on);
    const b = this.sHead.querySelector('[data-act="min"]') as HTMLButtonElement;
    b.textContent = on ? '□' : '–'; b.title = on ? 'Restore' : 'Minimise'; b.setAttribute('aria-label', b.title);
    if (this.open) this.render();
  }
  private setBig(on: boolean, byHand: boolean) {
    this.big = on; if (byHand) { this.sized = true; this.handBig = on; }
    if (on && this.min) this.setMin(false);
    this.el.classList.toggle('big', on);
    const b = this.sHead.querySelector('[data-act="size"]') as HTMLButtonElement;
    b.title = on ? 'Smaller panel' : 'Bigger panel'; b.setAttribute('aria-label', b.title);
    if (this.open) this.render();
  }
  /** The header's own line — what the folded box still says: "Meteor-M2-3 · AOS 12:05 (in 0:09)" / "ISS · UP · 6:12 left". */
  private headLine(now: number): string {
    if (this.watch && !this.picked) return satSingleHead(this.single, now);   // ★★★ beta96 folded, the watch view still counts down
    const sel = this.picked ?? this.pending;
    const nr = this.picked ? this.boxNorad() : sel?.norad;   // ★★★ beta94 the followed ORBCOMM bird
    const sat = sel ? this.list?.sats.find(s => s.norad === nr) : undefined;
    if (!sat) return '';
    const row = satModeRows(this.list, now).find(r => r.sat.norad === sat.norad);
    return row ? `${sat.name} · ${satWhenText(row, now)}` : sat.name;
  }

  private lend(on: boolean) {
    if (on === this.lent) return;
    this.lent = on;
    this.deps.image(on ? this.sImg : null);
  }

  private render() {
    const now = this.now();
    const cw = this.cap && !this.cap.ok ? this.cap.why : '';
    const dop = this.sHead.querySelector('[data-act="doppler"]') as HTMLButtonElement;
    dop.classList.toggle('on', this.autoDoppler); dop.setAttribute('aria-pressed', String(this.autoDoppler));
    dop.disabled = !!cw;
    // ★★★ beta96 single mode: satellites — watching: no EXIT (the note says why), AUTO DOPPLER only on a hybrid's own VFO
    { const ex = this.sHead.querySelector('[data-act="exit"]') as HTMLButtonElement;
      ex.hidden = this.watch; this.sOnly.hidden = !this.watch;
      dop.hidden = this.watch && !this.hybrid;
      this.sList.hidden = this.watch; }
    this.paintWatch(now);
    const live = this.readout();
    this.sDot.classList.toggle('on', !!live && live.state === 'TRACK');
    const hs = this.min ? this.headLine(now) : '';
    if (this.sHs.textContent !== hs) this.sHs.textContent = hs;
    if (this.min) return;   // ★ folded: the header is all there is to draw
    const why = cw || this.refusal || (this.ro?.state === 'OFF' ? this.ro.why ?? '' : '');
    this.sWhy.hidden = !why; this.sWhy.textContent = why;
    this.paintPick();
    // ★ spec item 10: when the orbits were synced (or that they are being fetched, or why there are none)
    const sync = satSyncedText(this.list, now);
    this.sSync.textContent = sync; this.sSync.hidden = !sync;
    // the timetable — ★★ beta94 under SHOW (the satellite picked always stays)
    this.paintShow();
    const allRows = satModeRows(this.list, now);
    const rows = satShowRows(allRows, this.showF, this.keepNorad());
    let h = '';
    if (!this.list) h = `<div class="sat-dim">Asking the server…</div>`;
    else if (!allRows.length) h = `<div class="sat-dim">${esc(this.list.why || (this.list.source?.fetching ? 'The satellites appear once the orbit data has arrived' : 'No satellites are listed on this server'))}</div>`;
    else if (!rows.length) h = `<div class="sat-dim">${esc(satShowEmptyText(this.showF, allRows, this.keepNorad()))}</div>`;
    let anyRemind = false, anySched = false;
    const whens: string[] = [];
    // ★★★ beta96 SCHEDULE / REMIND by whether tuning moves anyone else (satSession); the plan says what overlaps did
    const sess = this.session();
    const srvK = this.srv();
    const sEntries = this.sched.all(srvK);
    const plan = satSchedPlan(sEntries);
    for (const r of rows.slice(0, 160)) {   // ★ 2026-11-24 every pass is a row now (was 40)
      const sel = !r.later && ((this.picked && this.picked.norad === r.sat.norad && this.picked.tx === r.i) || (this.pending && this.pending.norad === r.sat.norad && this.pending.tx === r.i));
      const name = r.sat.tx.length > 1 ? `${r.sat.name} · ${r.tx.label}` : r.sat.name;
      const inactive = r.tx.status === 'inactive' ? ' · INACTIVE' : '';
      // ★★ 1.6 REMIND — only where a pick disturbs nobody (the server's pickPrivate), on a hearable row with a pass to come;
      //    hidden, not greyed, everywhere else
      const pk = satPassKey(this.list, r, satKeyKind(sess));   // ★★★ beta96 shared dial: always SCHEDULE (decided at AOS)   // ★★★ beta96 REMIND (shared dial) / SCHEDULE (nobody else moved) / none
      const offer = pk === 'remind';
      const se = r.next ? this.sched.find(srvK, r.sat.norad, r.i, r.next.aos) : undefined;
      const held = offer ? (this.rem.find(srvK, r.sat.norad, r.i) ?? (se ? { key: se.key } : undefined)) : undefined;   // ★ a degraded schedule reads as a reminder
      anyRemind ||= offer;
      anySched ||= pk === 'schedule';
      const slot = se ? plan.get(se.key) : undefined;
      h += `<div class="sat-rw"><button class="sat-row${r.grey ? ' grey' : ''}${r.up ? ' up' : ''}${sel ? ' on' : ''}" data-act="pick" data-norad="${r.sat.norad}" data-tx="${r.i}"`
        + (r.grey ? ` aria-disabled="true" data-why="${esc(r.why)}" title="${esc(r.why)}"` : ` title="${esc((r.tx.note || r.tx.label) + ' — tune there' + (this.autoDoppler ? ' and correct the Doppler' : ''))}"`)
        + `><span class="sat-n">${esc(name)}</span><span class="sat-w">\u0000${whens.push(satWhenText(r, now)) - 1}\u0000</span>`
        + `<span class="sat-f">${esc(satTxDetail(r.tx))}${inactive}${r.grey ? ' — ' + esc(r.why) : ''}${r.grey ? '' : qualityHtml(r.pass)}</span>`
        + (se ? `<span class="sat-s${slot?.state === 'skipped' ? ' skip' : ''}">${esc(sess.kind === 'remind' ? 'SCHEDULED · reminds you (another listener is on this dial)' : satSchedRowText(se, slot, sEntries))}</span>` : '')
        + `</button>`
        + (offer ? `<button class="sat-rem${held ? ' on' : ''}" data-act="remind" data-norad="${r.sat.norad}" data-tx="${r.i}" aria-pressed="${!!held}"`
            + ` title="${esc(held ? SAT_REMIND_FORGET_WHAT : SAT_REMIND_SET_WHAT)}" aria-label="${esc(held ? SAT_REMIND_FORGET_WHAT : 'Remind me before this pass')}">${esc(satRemindKeyText(!!held))}</button>` : '')
        // ★★★ beta96 SCHEDULE / CANCEL, and PRIORITISE on a scheduled pass that overlaps another
        + (pk === 'schedule' && r.next ? `<span class="sat-sk"><button class="sat-rem${se ? ' on' : ''}" data-act="sched" data-norad="${r.sat.norad}" data-tx="${r.i}" data-aos="${r.next.aos}" aria-pressed="${!!se}"`
            + ` title="${esc(se ? SAT_SCHED_CANCEL_WHAT : satOnSharedDial(sess) ? SAT_SCHED_SHARED_NOTE : SAT_SCHED_SET_WHAT)}" aria-label="${esc(se ? SAT_SCHED_CANCEL_WHAT : 'Schedule this pass: tune at AOS, back after LOS')}">${esc(satSchedKeyText(!!se))}</button>`
            + (se && slot?.over.length ? `<button class="sat-rem${se.prio ? ' on' : ''}" data-act="prio" data-key="${esc(se.key)}" aria-pressed="${!!se.prio}" title="${esc(SAT_SCHED_PRIO_WHAT)}">${esc(satSchedPrioText(!!se.prio))}</button>` : '')
            + `</span>` : '')
        + `</div>`;
    }
    // ★★★ beta96 why it is SCHEDULE / REMIND here, the overlap rule, the degraded note, and the schedule's own state
    if (anySched || anyRemind) { const sl = satSessionLine(sess, 'web'); if (sl) h += `<div class="sat-sline sat-dim">${esc(sl)}</div>`; }
    if (sess.kind === 'schedule' && sEntries.some((e) => (plan.get(e.key)?.over.length ?? 0) > 0)) h += `<div class="sat-sline sat-dim">${esc(SAT_SCHED_OVERLAP_RULE)}</div>`;
    if (sess.kind === 'remind' && sEntries.length) h += `<div class="sat-sline">${esc(satSchedDegradedText(sEntries.length))}</div>`;
    { const nt = sess.kind === 'schedule' ? satSchedNowText(this.schedRun, sEntries, plan, now) : ''; if (nt) h += `<div class="sat-sline">${esc(nt)}</div>`; }
    if (this.schedNote) h += `<div class="sat-sline">${esc(this.schedNote)}</div>`;
    if (anyRemind || (anySched && satOnSharedDial(sess))) h += `<div class="sat-lead sat-dim"><span>REMIND</span>${[1, 2, 5, 10].map((v) => `<button data-act="lead" data-v="${v}" class="${v === this.lead ? 'on' : ''}" aria-pressed="${v === this.lead}">${v}</button>`).join('')}<span>min before it rises · this page only</span></div>`;
    if (this.list?.time === 'system') h += `<div class="sat-dim">TIME: SYSTEM — this server's clock has not been checked against a time server</div>`;
    /* ★★ beta91 (Stuart, Safari: "i think the satellite box is causing the stutters on the waterfall"): the timetable was
     *  REBUILT every second — up to 40 rows of buttons parsed, styled and laid out on the thread that draws the waterfall,
     *  for a change of one countdown digit. Now the rows are rebuilt only when something but the countdowns changed (a
     *  pass rising, a pick, a reminder, the list); otherwise only each countdown's text is written. */
    if (h !== this.rowsKey) {
      this.rowsKey = h;
      this.sRows.innerHTML = h.replace(/\u0000(\d+)\u0000/g, (_m, i) => esc(whens[Number(i)]));
      this.rowWhen = Array.from(this.sRows.querySelectorAll<HTMLElement>('.sat-w'));
    } else {
      whens.forEach((w, i) => { const el = this.rowWhen[i]; if (el && el.textContent !== w) el.textContent = w; });
    }
    const summary = this.sList.querySelector('summary')!;
    const sumText = this.picked ? 'ALL SATELLITES' : 'SATELLITES';
    if (summary.textContent !== sumText) summary.textContent = sumText;
    const credit = satCreditText(this.list, now), txc = satTxCreditText(this.list);
    this.sCredit.textContent = credit ? credit + (txc ? ' · ' + txc : '') : '';
  }

  /** ★★★ beta96 THE WATCH VIEW (single mode: satellites): the pass now / next with its countdown, what the radio is doing,
   *  the last pass + GALLERY, and the radio's timetable — rebuilt only when something but the countdowns changed. */
  private paintWatch(now: number) {
    if (!this.watch) { if (!this.sWatch.hidden) { this.sWatch.hidden = true; this.sWatch.innerHTML = ''; this.watchKey = ''; } return; }
    const s = this.single ? { ...this.single, ...(this.hybrid ? { hybrid: this.hybrid } : {}) } : null;
    const rows = satSinglePlanRows(s, now);
    const last = satSingleLastText(s);
    let h = `<div class="sat-wh" data-w="head"></div><div class="sat-dim sat-ln" data-w="sub"></div>`;
    if (last) h += `<div class="sat-dim sat-ln">${esc(last)}</div>`;
    h += `<div class="sat-wk"><button data-act="gallery" title="The pictures, packets and telemetry this receiver has kept">GALLERY</button></div>`;
    if (rows.length) h += `<div class="sat-dim" style="margin-top:6px">THIS RECEIVER'S PASSES</div>`
      + rows.map((r) => `<div class="sat-wp${r.live ? ' live' : ''}">${esc(r.text)}</div>`).join('');
    this.sWatch.hidden = false;
    if (h !== this.watchKey) { this.watchKey = h; this.sWatch.innerHTML = h; }
    const head = satSingleHead(s, now);
    const sub = this.hybrid ? satHybridText(this.hybrid, now) : satSingleSub(s, now);
    const he = this.sWatch.querySelector('[data-w="head"]') as HTMLElement, se = this.sWatch.querySelector('[data-w="sub"]') as HTMLElement;
    if (he.textContent !== head) he.textContent = head;
    if (se.textContent !== sub) se.textContent = sub;
  }
  /** what the watch header says now (tests) */
  watchHead(): string { return this.watch ? satSingleHead(this.single, this.now()) : ''; }

  /** The pick's box: COMPACT (voice / packet) or BIG (image; ISS SSTV once a picture starts). */
  private paintPick() {
    const now = this.now();
    /* ★★★ beta94 THE BOX FOLLOWS THE DECODER: an ORBCOMM pick whose pass is over shows the bird its decoder hears now (else
     *  the next to rise) — its name, pass, half-globe and arrows — with no new pick; the line says so */
    const fol = this.followNow();
    const following = !!fol && fol.why !== 'pick';
    const sat = this.picked ? this.list?.sats.find(s => s.norad === (fol?.norad ?? this.picked!.norad)) : undefined;
    const t = following && sat ? sat.tx.find((x) => x.decoder === 'orbcomm' || x.mode === 'orbcomm') ?? sat.tx[0] : this.pickedTx();
    const pend = this.pending ? this.list?.sats.find(s => s.norad === this.pending!.norad) : undefined;
    if (!t || !sat) {
      this.lend(false);
      this.sBox.hidden = !pend;
      if (pend) {
        const pt = pend.tx[this.pending!.tx];
        this.sName.textContent = `${pend.name}${pt ? ' · ' + pt.label : ''}`;
        this.sHz.textContent = pt ? satHzText(satCorrectedHz(pt, null)) : '';
        { const up = pt ? satUplinkText(pt) : ''; const txt = up ? `${up} · the frequency above is the downlink` : ''; this.sUp.hidden = !txt; this.sUp.textContent = txt; }
        this.sCd.textContent = satCountdownText(pend, now);
        this.sState.textContent = 'Tuning…';
        this.sImg.hidden = true; this.sPk.hidden = true;
        this.drawGlobe(pend);
      }
      return;
    }
    // ★★ the box's size is the LISTENER's (SMALL / BIG), as any decoder box — a picture shows in BIG (pictureStart grows it
    //    once); an image downlink only says where a picture will appear
    const big = this.big;
    // ★ the picture area only once a picture has started (or finished) — never an empty black frame (Stuart's 11-21 shot)
    const imageKind = satBoxSize(t, true) === 'big' && (this.picture || this.pictureDone);
    this.sBox.hidden = false;
    const r = this.readout();
    const mine = r && r.norad === sat.norad ? r : null;
    this.sRo.classList.toggle('coast', mine?.state === 'COAST');
    // ★★ beta96 the small globe beside the words only where they keep their column (one rule with the app); the globe is
    //    150 px (120 px on a narrow window), 10 px gap
    const gw = this.sGlobe.clientWidth || 150;
    this.sRo.classList.toggle('stack', !this.big && satReadoutLayout(this.sRo.clientWidth, this.big, gw, 10) === 'stacked');
    const ml = satTxModeLabel(t);
    this.sName.textContent = `${sat.name} · ${t.label}${ml ? ' · ' + ml : ''}`;
    this.sHz.textContent = satHzText(satCorrectedHz(t, mine));
    { const up = satUplinkText(t); const txt = up ? `${up} · the frequency above is the downlink` : ''; this.sUp.hidden = !txt; if (this.sUp.textContent !== txt) this.sUp.textContent = txt; }
    this.sCd.textContent = satCountdownText(sat, now, this.track && this.track.norad === sat.norad ? this.track.geo : null);
    const use = txUse(t);
    const lockWord = !this.autoDoppler ? 'DOPPLER OFF' : mine ? satStateWord(mine) : '';
    // ★ beta92: a CW beacon says its CW decoder is listening for the beacon (it said "listening for packets") — satDecWord
    const orbSt = this.picked?.decoder === 'orbcomm' ? this.orb?.status ?? null : null;
    const decWord = this.picked?.decoder === 'orbcomm'
      ? (orbSt?.sats.length ? `decoding ${orbSt.sats.length} ORBCOMM bird${orbSt.sats.length === 1 ? '' : 's'}` : satDecWord('orbcomm', use, false, big))
      : this.decLine || satDecWord(this.picked?.decoder, use, t.sstv, big);
    // ★★★ beta92 OPEN IN VFO B — B's line and the line switch; with no B this is exactly the one-downlink box
    const vb = this.vfob && this.vfob.on ? this.vfob : null;
    const sw = satViewSwitch(this.picked?.decoder, vb);
    const shown = satViewShown(this.picked?.decoder, vb, this.view);
    const aLine = [following ? orbFollowText(fol, sat.name) : lockWord, decWord].filter(Boolean).join(' · ');
    const aText = sw ? `VFO A · ${aLine || t.label}` : aLine;
    if (this.sState.textContent !== aText) this.sState.textContent = aText;
    this.sState.classList.toggle('sw', sw); this.sState.classList.toggle('on', sw && shown === 'A');
    if (sw) { this.sState.dataset.act = 'view'; this.sState.dataset.v = 'A'; this.sState.setAttribute('role', 'button'); this.sState.title = 'Show VFO A\'s decoder in the box'; }
    else { delete this.sState.dataset.act; this.sState.removeAttribute('role'); this.sState.removeAttribute('title'); }
    const showA = shown === 'A';
    // big: the decoder box's image canvas, lent to this window (the live preview, the finished picture at pass end)
    this.lend(big && imageKind && showA);
    this.sImg.hidden = !(big && imageKind && showA);
    // ★ beta92 A CW beacon's decoded text, as CW shows it elsewhere (the last few lines)
    //   ★★ beta92 a skimmed beacon (cwskim): the signals in its 24 kHz, nearest the prediction first, each with its offset
    const cwA = showA && satDecView(this.picked?.decoder) === 'text';
    this.sCw.hidden = !cwA;
    if (cwA) {
      const ct = this.picked?.decoder === 'cwskim' ? satBeaconText(this.aBeacon) || SAT_BEACON_EMPTY : satCwTail(this.aText) || 'no characters yet';
      if (this.sCw.textContent !== ct) this.sCw.textContent = ct;
    }
    /* ★★ beta91 (Stuart's Meteor-M2-4 pass 2026-11-22: "picture complete — saved to the gallery" for minutes while the
     *  satellite was still strong and the receiver hunting again): LRPT's own line is the LIVE state and says a filed pass
     *  itself ("last pass saved to the gallery · …", src/services/lrpt.ts) — so it wins. SSTV keeps the words. */
    if (this.pictureDone && imageKind && !(this.picked?.decoder === 'lrpt' && this.decLine)) this.sState.textContent = [lockWord, 'picture complete — saved to the gallery'].filter(Boolean).join(' · ');
    // packet: the APRS lines
    const pkt = showA && use === 'packet' && this.picked?.decoder !== 'fctlm' && this.picked?.decoder !== 'orbcomm' && satDecView(this.picked?.decoder) !== 'text';   // ★ telemetry shows values; CW its text
    this.sPk.hidden = !pkt;
    if (pkt) this.sPk.innerHTML = this.packets.length ? this.packets.slice(0, 12).map(l => `<div>${esc(l)}</div>`).join('') : `<div class="sat-dim">no packets yet</div>`;
    // ★★ 1.3 the telemetry, as values with units (a channel with no published calibration says "raw")
    const tl = showA && (satPickTlm(this.picked) || !!this.tlm?.values.length);   // ★★★ beta94 + a finder that decodes the telemetry too
    this.sTlm.hidden = !tl;
    if (tl) {
      const hex = satTelemetryHex(this.tlm);
      this.sTlm.innerHTML = `<div>${esc(satTelemetryHead(this.tlm))}<span class="sat-tk">`
        + `<button data-act="tlmcharts" class="${this.tlmCharts ? 'on' : ''}" aria-pressed="${this.tlmCharts}" title="Telemetry charts: this pass, and the last passes saved in the gallery">CHARTS</button>`
        + `${hex ? `<button data-act="tlmraw" class="${this.tlmRaw ? 'on' : ''}" aria-pressed="${this.tlmRaw}" title="The frame's bytes as received">RAW</button>` : ''}</span></div>`
        + (this.tlmCharts ? tlmChartsHtml({ pass: tlmLivePass(this.tlm?.hist, this.tlm?.sat || ''), live: true, across: this.tlmAcross,
                                            acrossNote: this.tlmAcrossNote, width: this.sTlm.clientWidth || 300, all: this.tlmAll })
          : this.tlmRaw && hex ? `<div class="sat-dim" style="word-break:break-all">${esc(hex)}</div>`
          // ★★ beta94 values in their units; a raw one carries its reason (title); a board with no valid data is SAID
          : (satTelemetryAbsentText(this.tlm) ? `<div class="sat-dim">${esc(satTelemetryAbsentText(this.tlm))}</div>` : '')
            + (this.tlm?.values.length ? `<div class="sat-tv">${this.tlm.values.map((v) => `<span${v.note ? ` title="${esc(v.note)}"` : ''}>${esc(satTelemetryLine(v))}</span>`).join('')}</div>` : ''));
    }
    // ★★ 2026-11-24 ORBCOMM — its own panel
    this.paintOrb(showA && this.picked?.decoder === 'orbcomm');
    this.paintXpdr(showA && this.picked?.decoder === 'xpdr');   // ★★★ beta94
    // ★★ 1.9 the MAP key — a packet downlink, once a station with a position is heard (no dead key)
    this.sMap.hidden = !satMapOffered(t, this.deps.mapStations?.() ?? 0) || !this.deps.openMap;
    // ★★★ beta92 after A's area: B's line, and B's area when B's line is the one shown (A's parts are hidden then)
    this.paintVfoB(sat, mine, big, sw, shown);
    this.askTrack();
    this.drawGlobe(sat);
  }

  /** ★★ 2026-11-24 THE ORBCOMM PANEL: the head (+ SKY PLOT), the caveat (subscriber messages are never decoded), each bird
   *  with its housekeeping, CARRIERS AUTO / 1–6, the sky plot (redrawn only when its dots change), the packets. Each part is
   *  written only when its text changed — this runs every second with the countdowns. */
  /** ★★★ beta94 THE TRANSPONDER FINDER's rows (Stuart 11-24): every signal it has found — voices first while on air, each
   *  with LISTEN (VFO A snaps there and follows the person), every CW row with what it decoded (the beacon one of them);
   *  a row unheard for a minute fades. The follow's state in words. Written only when it changed. */
  private paintXpdr(show: boolean) {
    this.sXp.hidden = !show;
    if (!show) return;
    const st = this.xp;
    let h = `<div class="sat-cav">${esc(xpdrHint(st))}</div>`;
    const fw = xpdrFollowText(st?.follow ?? null);
    if (fw) h += `<div class="sat-note">${esc(fw)}</div>`;
    /* ★★★ beta97 THE DIGITAL-MODE SKIMMER (Stuart 11-27): SKIM (on/off — the benchmark set its default, the listener always
     *  may), its state in one line, and what it heard, newest first — compact; the spots list and the map have them too */
    const sk = st?.skim ?? null;
    if (sk) {
      h += `<div class="sat-xs"><span class="sat-tk"><button data-act="xskim" class="${sk.on ? 'on' : ''}" aria-pressed="${sk.on}" title="${esc(XPDR_SKIM_TITLE)}">SKIM FT8/FT4</button></span> <span class="sat-dim">${esc(xpdrSkimLine(sk))}</span></div>`;
      for (const hd of sk.heard.slice(0, 8)) h += `<div class="sat-xh${hd.late ? ' faded' : ''}">${esc(xpdrHeardText(hd))}</div>`;
    }
    const rows = xpdrRowsSorted(st?.rows ?? []);
    /* ★★ beta96 (Stuart 11-26: "we need the raw/explain here for the cw"): EXPLAIN ↔ RAW over the CW rows — the CW box's key,
     *  the SAME stored setting (main.ts 'vibe.cwPlain') and dictionary (src/utils/cwVocab); the legend names what a press does */
    const plain = xpdrCwPlain();
    if (rows.some((r) => r.kind === 'cw' && r.text))
      h += `<div class="sat-tk"><button data-act="xexplain" aria-pressed="${plain}" title="${plain ? 'Show the CW as sent' : 'Explain the CW in plain English (press again for the CW as sent)'}">${plain ? 'RAW' : 'EXPLAIN'}</button></div>`;
    if (!rows.length) h += `<div class="sat-dim">no signals found yet</div>`;
    for (const r of rows.slice(0, 16)) {
      const following = st?.follow?.id === r.id;
      h += `<div class="sat-xr${xpdrFaded(r) ? ' faded' : ''}${following ? ' on' : ''}" data-xid="${esc(r.id)}">`
        + `<span class="sat-xf">${esc(xpdrFreqText(r))}</span> <b>${esc(xpdrWhat(r))}</b> <span class="sat-dim">${esc(`${Math.round(r.snr)} dB · ${xpdrAgo(r)}`)}</span>`
        + (xpdrListenable(r) && this.deps.xpdrListen   // ★★★ beta94 voices and the telemetry
          ? ` <button data-act="xlisten" data-xid="${esc(r.id)}" class="${following ? 'on' : ''}" aria-pressed="${following}" title="${esc(xpdrListenLabel(r))}">${following ? 'LISTENING' : 'LISTEN'}</button>`
          : '')
        + (r.kind === 'cw' && r.text ? `<div class="sat-xt">${esc(plain ? plainEnglish(r.text) : r.text)}</div>` : '')
        + `</div>`;
    }
    if (h !== this.xpKey) { this.xpKey = h; this.sXp.innerHTML = h; }
  }
  private paintOrb(show: boolean) {
    this.sOrb.hidden = !show;
    if (!show) return;
    const v = this.orb, st = v?.status ?? null;
    let h = `<div>${esc(orbHeadText(st))}<span class="sat-tk"><button data-act="orbsky" class="${this.orbSky ? 'on' : ''}" aria-pressed="${this.orbSky}" title="The sky plot: where this aerial hears the ORBCOMM satellites">SKY PLOT</button></span></div>`
      + `<div class="sat-cav">${esc(ORB_NO_TUNE)}</div><div class="sat-cav">${esc(ORB_CAVEAT)}</div>`;
    const note = orbNoteText(st);
    if (note) h += `<div class="sat-note">${esc(note)}</div>`;
    const counts = orbCountsText(st?.counts);
    if (counts) h += `<div class="sat-note">Packets: ${esc(counts)}</div>`;
    // ★★ the birds listed now, then the held ones (last heard a while ago) dimmed — a carrier drop no longer blanks a bird
    // ★★ b97 an unnamed row whose carrier sits on a predicted channel: "probably FM116?" in amber — the label's own guess
    const plan = orbLabelPlan(this.deps.satLabels?.() ?? [], st);
    for (const { sat: b, heldSecs } of orbShownSats(v)) {
      const guess = !b.norad && heldSecs == null ? orbRowGuess(plan, b.carrier) : '';
      h += `<div class="sat-bird${heldSecs != null ? ' held' : ''}">${orbSatLines(b, v?.clockOff[b.norad], heldSecs, guess)
        .map((l, i) => (i === 0 && guess ? `<div class="sat-guess" style="color:${ORB_GUESS_COLOUR}">${esc(l)}</div>` : `<div>${esc(l)}</div>`)).join('')}</div>`;
    }
    if (this.deps.orbMax)
      h += `<div class="sat-car" role="group" aria-label="Carriers the decoder follows at most"><span>CARRIERS</span>`
        + ORB_CARRIER_CHOICES.map((k) => `<button data-act="orbmax" data-v="${k}" class="${k === this.orbMaxK ? 'on' : ''}" aria-pressed="${k === this.orbMaxK}"`
          + ` title="${k ? `Follow at most ${k} ORBCOMM carrier${k === 1 ? '' : 's'}` : 'AUTO: the decoder follows up to four'}">${orbCarriersLabel(k)}</button>`).join('') + `</div>`;
    if (h !== this.oTxtKey) { this.oTxtKey = h; this.oTxt.innerHTML = h; }
    this.oSky.hidden = !this.orbSky;
    if (this.orbSky) {
      const now = Math.floor(Date.now() / 10000) * 10, norad = this.picked?.norad ?? 0;
      const sum = [skySummaryFor(this.skyMode, this.skyCells, this.skyDots, now, false, norad) || 'No dots yet — they appear as the birds are heard', this.skyNote].filter(Boolean).join(' · ');
      const key = `${this.skyDots.length}|${this.skyDots[0]?.t ?? 0}|${sum}|${this.big ? 1 : 0}|${this.skyMode}|${now}|${this.skyCells?.cells.size ?? -1}`;
      if (key !== this.oSkyKey) {
        this.oSkyKey = key;
        this.oSky.innerHTML = skyPlotSvg(skySceneFor(this.skyMode, this.skyCells, this.skyDots, now, 300, 3000, norad), 'Sky plot: where this aerial hears the ORBCOMM satellites')
          + (this.skyCells ? skyModesHtml(this.skyMode) : '')
          + `<div class="sat-leg">${SKY_LEGEND.map((k) => `<span><i style="background:${k.css}"></i>${esc(k.label)}</span>`).join('')}</div>`
          + `<div class="sat-skys">${esc(sum)}</div>` + (this.big ? `<div class="sat-skys">${esc(SKY_PLOT_WHAT)}</div>` : '');
      }
    }
    const pk = `<div class="sat-pkh">RECENT PACKETS</div><div class="sat-pks">`
      + (v?.pkts.length ? v.pkts.slice(0, 20).map((k) => `<div>${esc(orbPktLine(k))}${snrChipHtml('orbcomm', k.snr)}</div>` + (k.raw && k.hex ? `<div class="sat-hex">${esc(k.hex)}</div>` : '')).join('')
        : `<div class="sat-dim">no packets yet</div>`) + `</div>`;
    if (pk !== this.oPkKey) { this.oPkKey = pk; this.oPk.innerHTML = pk; }
  }

  /** ★★★ beta92 the VFO B part of the box: OPEN IN VFO B key(s) for the downlinks the server offered, B's line once it is
   *  open (its frequency with the Doppler, its decoder's words), and — when B's line is the one shown — B's decoder area. */
  private paintVfoB(sat: SatInfo, r: SatReadout | null, big: boolean, sw: boolean, shown: 'A' | 'B') {
    const vb = this.vfob && this.vfob.on ? this.vfob : null;
    const offers = this.deps.vfobOpen ? satVfoBOffers(this.picked, sat, vb) : [];
    let h = '';
    if (vb) {
      const tb = sat.tx[vb.tx];
      const word = this.bStatus || (vb.decoderWhy ? `decoder: ${vb.decoderWhy}` : satDecWord(vb.decoder, vb.use ?? (tb ? txUse(tb) : 'voice'), tb?.sstv, big));
      const line = [satVfoBLine(vb, tb?.label, r), word].filter(Boolean).join(' · ');
      h += sw ? `<div class="sat-dim sat-ln sw${shown === 'B' ? ' on' : ''}" data-act="view" data-v="B" role="button" title="Show VFO B's decoder in the box">${esc(line)}</div>`
              : `<div class="sat-dim sat-ln">${esc(line)}</div>`;
    }
    for (const o of offers)
      h += `<button class="sat-vbk" data-act="vfob" data-tx="${o.i}" title="${esc(`${o.tx.label} on VFO B, with its decoder and the same Doppler correction — close it with VFO A/B on the frequency card`)}">${esc(`${SAT_VFOB_KEY} · ${o.tx.label}`)}</button>`;
    if (h !== this.vbKey) { this.vbKey = h; this.sVb.innerHTML = h; }
    this.sVb.hidden = !h;
    // B's decoder area (only while B's line is the one shown)
    const showB = !!vb && shown === 'B';
    const v = showB ? satDecView(vb!.decoder) : '';
    this.sBImg.hidden = !(v === 'image' && big && (this.bPicture || this.bPictureDone));
    if (v === 'text' || v === 'telemetry' || v === 'orbcomm') {
      this.sCw.hidden = false;
      const ct = vb!.decoder === 'cwskim' ? satBeaconText(this.bBeacon) || SAT_BEACON_EMPTY
        : satCwTail(this.bText, v === 'telemetry' ? 6 : 4) || (v === 'text' ? 'no characters yet' : 'no frames yet');
      if (this.sCw.textContent !== ct) this.sCw.textContent = ct;
    }
    if (v === 'packet') {
      this.sPk.hidden = false;
      this.sPk.innerHTML = this.bPackets.length ? this.bPackets.slice(0, 12).map(l => `<div>${esc(l)}</div>`).join('') : `<div class="sat-dim">no packets yet</div>`;
    }
  }

  /** ★ 1.7 the pass's path — asked once per pick (the pass in progress, else the next) and again when the next begins. */
  private askTrack() {
    if (!this.picked || !this.deps.track) return;
    const nr = this.boxNorad();   // ★★★ beta94 the followed ORBCOMM bird's pass
    const sat = this.list?.sats.find((x) => x.norad === nr);
    if (!sat) return;
    const row = satModeRows({ ...this.list!, sats: [sat] }, this.now())[0];
    const p = row?.pass;
    if (!p || (this.trackAsk && this.trackAsk.norad === sat.norad && this.trackAsk.aos === p.aos)) return;
    this.trackAsk = { norad: sat.norad, aos: p.aos };
    this.deps.track(sat.norad, p.aos, p.los);
  }

  /** ★ 1.9 the ONE DIGI MODES map, on its APRS layer, with this pass's orbit and footprint ("via ISS"). */
  private openMap() {
    const sat = this.picked ? this.list?.sats.find((x) => x.norad === this.picked!.norad) : undefined;
    if (!sat || !this.deps.openMap) return;
    this.deps.openMap({
      layer: 'aprs',
      overlay: satMapOverlay(sat.name, this.track && this.track.norad === sat.norad ? this.track.geo : null,
                             { lat: sat.lat, lon: sat.lon, altKm: satAltKm(sat.rangeKm, sat.el) }),
    });
  }

  // ── ★★ 1.6 REMIND ─────────────────────────────────────────────────────────────────────────────────────────────────
  private remindToggle(norad: number, tx: number) {
    const srv = this.srv(), now = this.now();
    const held = this.rem.find(srv, norad, tx);
    if (held) { this.rem.cancel(held.key); this.remSchedule(); this.render(); return; }
    // ★★★ beta96 a scheduled pass shown as a reminder (someone else on the dial): FORGET cancels it
    const nx = satModeRows(this.list, now).find((r) => r.sat.norad === norad && r.i === tx && !r.later)?.next;
    const se = nx ? this.sched.find(srv, norad, tx, nx.aos) : undefined;
    if (se) { this.sched.cancel(se.key); this.schedSave(); this.remSchedule(); this.render(); return; }
    const row = satModeRows(this.list, now).find((r) => r.sat.norad === norad && r.i === tx);
    if (!row || satPassKey(this.list, row, this.session().kind) !== 'remind') return;
    if (!this.rem.add(srv, row, now)) { this.refusal = 'Twenty reminders are held already — cancel one first'; this.render(); return; }
    // ★ the browser's permission is asked on the first REMIND press, never on load
    try { if (typeof Notification !== 'undefined' && Notification.permission === 'default') void Notification.requestPermission(); } catch { /* no notifications here */ }
    this.remSchedule(); this.render();
  }
  /** the reminders' own clock: one second while any is held or any pop-up is up; none otherwise */
  private remSchedule() {
    const need = this.rem.size > 0 || this.toasts.length > 0 || this.sched.all(this.srv()).length > 0 || !!this.schedRun.key;
    if (need && !this.remTimer) this.remTimer = window.setInterval(() => this.remTick(), 1000);
    if (!need && this.remTimer) { clearInterval(this.remTimer); this.remTimer = 0; }
  }
  /** Fire what is due (re-checked against the latest list), drop what is over, refresh the list shortly before. */
  remTick() {
    const now = this.now();
    const connected = this.deps.connected ? this.deps.connected() : true;
    // ★ a fresh list ~40 s before one fires (with the mode off nobody else is asking) — free, it changes nothing
    if (connected && this.rem.nextIn(now, this.lead) < 40 && (now * 1000 - this.listAt) > 30000) { this.deps.list(); this.listAt = now * 1000; }
    for (const r of this.rem.due(now, this.lead, connected)) {
      // ★★ beta94 (Stuart 11-25) SHOW filters the LIST only: a reminder set on a pass always fires, even one SHOW now hides
      const why = satRemindWhy(this.list, r);
      this.toasts.unshift({ r, why, shownAt: now });
      try {
        if (!why && typeof Notification !== 'undefined' && Notification.permission === 'granted') {
          const n = new Notification('VibeSDR · satellite pass', { body: satRemindText(r, now), tag: r.key });
          n.onclick = () => { try { window.focus(); } catch { /* */ } n.close(); };   // ★ focuses the tab: the pop-up is there; it never picks by itself
        }
      } catch { /* no notifications here */ }
    }
    this.schedTick(now, connected);   // ★★★ beta96
    this.toasts = this.toasts.filter((t) => now < t.r.los);   // ★ a pass that is over takes its pop-up with it
    this.paintToasts();
    this.remSchedule();
    if (this.open) this.render();
  }
  private paintToasts() {
    const now = this.now();
    this.remEl.hidden = !this.toasts.length;
    if (!this.toasts.length) { this.remEl.innerHTML = ''; return; }
    const picked = this.picked;
    const pickedName = picked ? this.list?.sats.find((x) => x.norad === picked.norad)?.name : undefined;
    let h = '';
    this.toasts.slice(0, 3).forEach((t) => {
      const ends = t.why ? '' : satRemindEnds(picked, pickedName, t.r);
      h += `<div class="sat-toast" role="alert" data-key="${esc(t.r.key)}"><div>${esc(t.why ? satRemindText(t.r, now).replace(/ — LISTEN\?$/, '') : satRemindText(t.r, now))}</div>`
        + (t.why ? `<div class="sat-tw">${esc(t.why)}</div>` : ends ? `<div class="sat-tw">${esc(ends)}</div>` : '')
        + (t.note ? `<div class="sat-tw">${esc(t.note)}</div>` : '')
        + `<div class="sat-tb">${t.why ? '' : `<button class="on" data-act="listen" data-key="${esc(t.r.key)}">LISTEN</button>`}`
        + `<button data-act="dismiss" data-key="${esc(t.r.key)}">DISMISS</button></div></div>`;
    });
    if (this.toasts.length > 3) h += `<div class="sat-more">+${this.toasts.length - 3} more</div>`;
    this.remEl.innerHTML = h;
  }
  private onToastClick(e: Event) {
    const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
    if (!b) return;
    const i = this.toasts.findIndex((t) => t.r.key === b.dataset.key);
    if (i < 0) return;
    const t = this.toasts[i];
    if (b.dataset.act === 'dismiss') { this.toasts.splice(i, 1); this.paintToasts(); this.remSchedule(); return; }
    if (b.dataset.act !== 'listen') return;
    // ★ re-checked at the tap too (the list may have moved on since it fired)
    const why = satRemindWhy(this.list, t.r);
    if (why) { t.why = why; this.paintToasts(); return; }
    if (!this.on) { if (this.deps.enter) this.deps.enter(); else this.show(); }
    if (!this.on) return;
    if (this.picked && this.picked.norad === t.r.norad && this.picked.tx === t.r.tx) { this.toasts.splice(i, 1); this.paintToasts(); return; }
    // ★★ the ORDINARY pick: ask-before-tuning holds it like a hand pick (false = held — the pop-up stays to try again)
    if (this.deps.pick(t.r.norad, t.r.tx, this.autoDoppler)) {
      this.pending = { norad: t.r.norad, tx: t.r.tx }; this.refusal = '';
      this.toasts.splice(i, 1);
    }
    this.paintToasts(); this.remSchedule(); this.render();
  }

  // ── ★★★ beta96 SCHEDULE A PASS ───────────────────────────────────────────────────────────────────────────────────
  private session() { return satSession({ ...(this.deps.session?.() ?? {}), pickPrivate: this.list?.source?.pickPrivate }); }
  private schedSave() { try { localStorage.setItem(SAT_SCHED_KEY, this.sched.toJSON()); } catch { /* not kept: it still runs this visit */ } }
  private schedToggle(norad: number, tx: number, aos: number) {
    const srv = this.srv(), now = this.now();
    const ex = this.sched.find(srv, norad, tx, aos);
    if (ex) {
      this.sched.cancel(ex.key);
      if (this.schedRun.key === ex.key) this.schedRun = { ...this.schedRun, took: true, prev: null };   // ★ cancelled mid-pass: stays, nothing put back
    } else {
      const row = satModeRows(this.list, now).find((r) => r.sat.norad === norad && r.i === tx && r.next?.aos === aos);
      if (!row || satPassKey(this.list, row, satKeyKind(this.session())) !== 'schedule') return;
      if (!this.sched.add(srv, row, now)) { this.refusal = SAT_SCHED_FULL; this.render(); return; }
      try { if (typeof Notification !== 'undefined' && Notification.permission === 'default') void Notification.requestPermission(); } catch { /* none here */ }
    }
    // ★★★ beta96 a shared receiver: say what the booking will do (Stuart 11-26)
    this.schedNote = !ex && satOnSharedDial(this.session()) ? SAT_SCHED_SHARED_NOTE : '';
    this.schedSave(); this.remSchedule(); this.render();
  }
  /** One second of the schedule: prune, fire degraded reminders, step the runner, act. Nothing while disconnected. */
  private schedTick(now: number, connected: boolean) {
    const srv = this.srv();
    const n0 = this.sched.size; this.sched.prune(now); if (this.sched.size !== n0) this.schedSave();
    const entries = this.sched.all(srv);
    if (!connected || (!entries.length && !this.schedRun.key)) return;
    // ★ a fresh list shortly before a scheduled pass (pickPrivate, reach and the pass time are re-read from it)
    if (entries.some((e) => e.aos - now < 40 && e.los > now) && now * 1000 - this.listAt > 30000) { this.deps.list(); this.listAt = now * 1000; }
    if (!this.list) return;   // ★ nothing decided without the server's word
    const kind = this.session().kind;
    if (kind === 'remind')
      for (const r of satSchedRemindDue(entries, now, this.lead, this.schedFired)) this.toasts.unshift({ r, why: satRemindWhy(this.list, r), shownAt: now, note: SAT_SCHED_REMIND_NOTE });
    const d = this.deps.here?.() ?? { hz: 0, mode: '' };
    const here: SatSchedPrev = { hz: d.hz, mode: d.mode, sat: this.on, pick: this.picked ? { norad: this.picked.norad, tx: this.picked.tx } : null };
    const st = satSchedStep(this.schedRun, entries, now, { pick: here.pick }, here, kind);
    this.schedRun = st.run;
    const a = st.act;
    if (a.act === 'tune') {
      const name = a.e.label ? `${a.e.name} · ${a.e.label}` : a.e.name;
      const why = satSchedWhy(this.list, a.e);
      if (why) { this.schedNote = `Scheduled ${name}: ${why}`; return; }
      if (!this.on) { if (this.deps.enter) this.deps.enter(); else this.show(); }
      if (!this.on) { this.schedNote = `Scheduled ${name}: SATELLITE could not be opened`; return; }
      if (this.picked && this.picked.norad === a.e.norad && this.picked.tx === a.e.tx) { this.schedNote = ''; return; }
      if (this.deps.pick(a.e.norad, a.e.tx, this.autoDoppler)) { this.pending = { norad: a.e.norad, tx: a.e.tx }; this.refusal = ''; this.schedNote = ''; }
      else this.schedNote = `Scheduled ${name}: held — this dial asks before tuning`;
      try {
        if (document.hidden && typeof Notification !== 'undefined' && Notification.permission === 'granted') {
          const nf = new Notification('VibeSDR · scheduled pass', { body: `Tuned to ${name}`, tag: a.e.key });
          nf.onclick = () => { try { window.focus(); } catch { /* */ } nf.close(); };
        }
      } catch { /* no notifications here */ }
    } else if (a.act === 'back') {
      this.schedBack(a.prev);
    } else if (a.act === 'end') {
      this.schedNote = a.why === 'shared' ? 'Another listener joined this dial — the schedule stopped tuning; the radio stays where it is' : '';
    }
  }
  /** After LOS: back to what the listener was on — their satellite, or the frequency and mode (an ordinary tune). */
  private schedBack(prev: SatSchedPrev) {
    if (prev.sat && prev.pick) {
      if (!this.on) { if (this.deps.enter) this.deps.enter(); else this.show(); }
      if (this.on && this.deps.pick(prev.pick.norad, prev.pick.tx, this.autoDoppler)) this.pending = prev.pick;
      return;
    }
    if (this.on && !prev.sat) this.exit();
    else if (this.on) { this.deps.drop(); this.clearPick(); this.deps.left(); }   // ★ they were in SATELLITE with no pick: stay in, nothing picked
    if (prev.hz > 0) this.deps.back?.(prev.hz, prev.mode);
  }

  /** ★ The half-globe: a 2D canvas (no WebGL, no tiles) — the dome, a 30° graticule (fainter than the land), the pass's
   *  orbit DOTTED with its highest point ticked (1.7), the receiver's GREEN dot and the drawn bird with its beam (1.8), both
   *  labelled; BIG adds a one-line key under it. */
  private drawGlobe(sat: { norad?: number; name?: string; lat?: number; lon?: number; rangeKm?: number; el?: number }) {
    const cv = this.sGlobe, ctx = cv.getContext('2d');
    if (!ctx) return;
    const rx = this.deps.rxPos();
    cv.style.display = rx ? '' : 'none';
    if (!rx) { this.sKey.hidden = true; return; }
    const W = cv.width, H = cv.height;
    /* ★★ beta91 drawn only when the picture would change: the bird moves ~0.1° in under 2 s, so this is a redraw every
     *  second or two instead of a full repaint (and a forced style read) every second; a resize, a new path, a new name
     *  or BIG / SMALL redraw at once. */
    const gk = [W, H, this.big ? 1 : 0, rx.lat.toFixed(3), rx.lon.toFixed(3), sat.norad ?? '', sat.name ?? '',
      sat.lat != null ? sat.lat.toFixed(1) : '', sat.lon != null ? sat.lon.toFixed(1) : '', sat.rangeKm != null ? Math.round(sat.rangeKm / 20) : '',
      this.track ? `${this.track.norad}:${this.track.tcaIdx ?? ''}:${this.track.geo?.length ?? 0}:${this.track.geo?.[0]?.[0] ?? ''}` : '',
      this.deps.rxName?.() || ''].join('|');
    if (gk === this.globeKey) return;
    this.globeKey = gk;
    const mineT = this.track && sat.norad != null && this.track.norad === sat.norad ? this.track : null;
    const g = globeScene(W, H, rx, sat.lat != null && sat.lon != null ? { lat: sat.lat, lon: sat.lon, altKm: satAltKm(sat.rangeKm, sat.el) } : null, mineT?.geo ?? null, mineT?.tcaIdx);
    // ★ scene units per CSS pixel — the dot, the lines and the words are sized on SCREEN, so they read at SMALL and BIG
    const sc = W / Math.max(60, cv.clientWidth || (this.big ? 420 : 150));
    const css = getComputedStyle(this.el);
    const ly = globeLayers(!this.big);   // ★★ beta96 (Stuart 11-26) SMALL: orbit, footprint, receiver dot, the glyph — no words
    const ink = css.color || '#ccc';
    const amber = css.getPropertyValue('--amber').trim() || '#ffb000';
    const ground = css.backgroundColor || 'rgba(8,6,2,.96)';
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.beginPath(); ctx.rect(0, 0, W, g.cy); ctx.clip();
    ctx.beginPath(); ctx.arc(g.cx, g.cy, g.r, Math.PI, 2 * Math.PI);
    ctx.globalAlpha = 0.12; ctx.fillStyle = ink; ctx.fill();
    ctx.globalAlpha = 0.55; ctx.strokeStyle = ink; ctx.lineWidth = 2; ctx.stroke();
    if (g.grid) { ctx.globalAlpha = 0.12; ctx.lineWidth = 1; ctx.stroke(new Path2D(g.grid)); }   // ★ fainter than the land
    // ★ rough land (globeCoast.ts) — beta91 neutral, never the faceplate's hue, so it reads through the beam (globeLandInk)
    if (g.coast) { ctx.globalAlpha = 0.7; ctx.lineWidth = 1; ctx.strokeStyle = globeLandInk(ground); ctx.stroke(new Path2D(g.coast)); ctx.strokeStyle = ink; }
    if (g.foot) { ctx.globalAlpha = 0.28; ctx.fillStyle = amber; ctx.fill(new Path2D(g.foot)); }
    if (ly.beam && g.beam) { ctx.globalAlpha = 0.35; ctx.fillStyle = amber; ctx.fill(new Path2D(g.beam)); }
    // ★ 1.7 the orbit, dotted, and its highest point
    if (g.orbit) {
      ctx.globalAlpha = 0.85; ctx.strokeStyle = amber; ctx.lineWidth = 1.5 * sc; ctx.lineCap = 'round';
      ctx.setLineDash([1.5 * sc, 4 * sc]); ctx.stroke(new Path2D(g.orbit)); ctx.setLineDash([]);
      if (ly.tick && g.tcaTick) { ctx.lineWidth = 1.5 * sc; ctx.stroke(new Path2D(g.tcaTick)); }
      // ★ beta91 which way it travels: open chevrons along increasing time, haloed in the ground so they read over the land
      if (ly.arrows && g.arrows.length) {
        const ar = new Path2D(globeArrowPath(g.arrows, GLOBE_ARROW_PX * sc));
        ctx.lineJoin = 'round';
        ctx.globalAlpha = 0.8; ctx.strokeStyle = ground; ctx.lineWidth = 3.6 * sc; ctx.stroke(ar);
        ctx.globalAlpha = 1; ctx.strokeStyle = amber; ctx.lineWidth = 1.6 * sc; ctx.stroke(ar);
      }
    }
    ctx.globalAlpha = 1;
    // ★ the receiver: green with a dark ring on every faceplate (never the dome's ink)
    if (g.rx) {
      ctx.beginPath(); ctx.arc(g.rx.x, g.rx.y, GLOBE_RX_DOT.r * sc, 0, 2 * Math.PI);
      ctx.fillStyle = GLOBE_RX_DOT.fill; ctx.fill();
      ctx.lineWidth = GLOBE_RX_DOT.ringW * sc; ctx.strokeStyle = GLOBE_RX_DOT.ring; ctx.stroke();
    }
    // ★ 1.8 the bird: tube, dish towards the Earth, panels along its heading (cells ruled)
    // ★★ beta96 SMALL: the satellite as a glyph — a square body, a line either side, on the BIG bird's heading, whole pixels
    if (ly.glyph && g.bird && g.sat) {
      const gl = globeGlyph(g.sat, g.bird.u, sc), bird = globeLandInk(ground);
      ctx.globalAlpha = 1; ctx.lineCap = 'butt'; ctx.strokeStyle = bird; ctx.lineWidth = GLOBE_GLYPH.line * sc; ctx.stroke(new Path2D(gl.panels));
      ctx.fillStyle = bird; ctx.fill(new Path2D(gl.body));
    }
    if (ly.bird && g.bird) {
      const b = g.bird;
      const bird = globeLandInk(ground);   // ★ beta91 the bird is neutral too, so it stands off the beam (Stuart 11-22)
      ctx.lineJoin = 'round';
      ctx.globalAlpha = 0.45; ctx.fillStyle = bird; ctx.fill(new Path2D(b.panels));
      ctx.globalAlpha = 1; ctx.strokeStyle = bird; ctx.lineWidth = 1.2 * sc; ctx.stroke(new Path2D(b.panels));
      ctx.lineWidth = 0.8 * sc; ctx.globalAlpha = 0.9; ctx.stroke(new Path2D(b.cells));
      ctx.globalAlpha = 1; ctx.lineWidth = 1.6 * sc; ctx.stroke(new Path2D(b.boom));
      ctx.fillStyle = bird; ctx.fill(new Path2D(b.body));
      ctx.lineWidth = 1 * sc; ctx.strokeStyle = ground; ctx.stroke(new Path2D(b.body));
      ctx.fillStyle = bird; ctx.fill(new Path2D(b.dish));
    }
    // ★ the labels: the receiver (the server's name, else RECEIVER) and the bird's name, kept off each other
    const font = 10 * sc;
    const L = ly.labels ? globeLabels(g, this.deps.rxName?.() || '', sat.name, font) : { rx: null, sat: null };
    ctx.font = `${font.toFixed(1)}px system-ui,sans-serif`; ctx.textBaseline = 'alphabetic';
    for (const lb of [L.rx, L.sat]) {
      if (!lb) continue;
      ctx.textAlign = lb.anchor === 'start' ? 'left' : lb.anchor === 'end' ? 'right' : 'center';
      ctx.globalAlpha = 0.9; ctx.lineWidth = 3 * sc; ctx.strokeStyle = ground; ctx.strokeText(lb.text, lb.x, lb.y);
      ctx.globalAlpha = 1; ctx.fillStyle = ink; ctx.fillText(lb.text, lb.x, lb.y);
    }
    ctx.restore();
    // ★ BIG: the one-line key (the dotted half only when there is an orbit to see)
    const key = this.big ? (g.orbit ? GLOBE_KEY_TEXT : GLOBE_KEY_TEXT.replace(/^.*· /, '')) : '';
    this.sKey.hidden = !key; if (this.sKey.textContent !== key) this.sKey.textContent = key;
  }
}
