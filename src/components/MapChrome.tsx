/**
 * MapChrome — ★★★ beta97 THE MAP CONTROL CARDS IN THE APP (Stuart's design, signed off on the web 2026-11-27 —
 * docs/v12/MAP-CONTROL-CARDS.md, MAP-TEMPLATE.md rule 6). The map takes centre stage; everything else lives in CARDS
 * that pop out of small chips and shrink back. React Native over the map's WebView (a pad and the keyboard reach the
 * app through hardwareKeys → PanelNav, never the page), so every chip, key and row here walks with the D-pad.
 *
 *  ★ WHERE: src/utils/mapChrome.ts chromeGeom — from the WINDOW and the parent's measured insets, never the device. Tall:
 *    the red ✕ top-left, RATE top-right, the family down the left (MAP joins it when the stack fits), CHAT / HEALTH /
 *    TIME down the right, LIST bottom-left, ? + scale + [− +] bottom-right, the shared-tuner pill top-centre. Short
 *    landscape: ONE top row inside the safe rectangle (✕ first … RATE last), cards drop down, LIST a side panel, the
 *    pill at the bottom centre.
 *  ★ CARDS: TRANSIENT (TUNE, LAYERS, SERVERS, STATS, MAP, RATE, HELP, health / time in short landscape) — one at a time;
 *    fold on another card, a map tap, CARD_IDLE_MS untouched, × or BACK. LIST is the one STICKY card (remembered).
 *    Every card: a plain × top-right, a full border, opaque, no blur. Muted rim at rest; an open chip = a gold icon on a
 *    warm ground; bright green ONLY for the controller's focus (NavRing).
 *  ★ THE ASK LOCK: a refused ✕ / TUNE makes the pill and CHAT breathe (a radar pulse, ASK_PULSE_MS, then still) and the
 *    parent says why; TUNE's card fades while locked.
 *  ★ No dead controls (AGENTS.md): a card with nothing in it is not offered; RATE with one source is a reading.
 */
import React, { useContext, useEffect, useMemo, useRef, useState } from 'react';
import { AccessibilityInfo, Animated, Easing, ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';
import Slider from '@react-native-community/slider';
import AsyncStorage from '@react-native-async-storage/async-storage';
import Svg, { Path, Circle } from 'react-native-svg';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, useNavRange, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import ModeIcon from './ModeIcon';
import SectionIcon from './SectionIcon';
import { SnrChip } from './SnrChip';
import EdgeChip, { HealthTabIcon, ClockTabIcon } from './EdgeChip';
import MapMiniWaterfall, { type MwfFrame, type MwfStore } from './MapMiniWaterfall';
import { addKeyListener } from '../utils/hardwareKeys';
import { CARD_IDLE_MS, MAP_HELP, MAP_HELP_NOTES } from '../utils/mapCards';
import { chromeGeom, chromeGeomAt, cardBoxAt, chromeScale, liveBadges, rateChip, pillWords, ASK_PULSE_MS, GAP, LIST_CHIP_H, ROW_H,
         type ChipId, type Insets, type GainRow, type RateRow } from '../utils/mapChrome';
import { TUNE_GROUP_LABEL, TUNE_ICON_PATH, type AirSeaTuneRow } from '../services/airSeaTune';
import type { TrackRow } from '../services/tracks';
import type { SnrMode } from '../utils/snrColour';
import type { MapWfModel } from '../utils/mapWfModel';
import type { ModeIconName } from '../utils/modeIcons';

// ── the map's own dark chrome (the old bar's palette — not the faceplate's; MAP-CONTROL-CARDS "Skins") ──
const C = {
  ground: 'rgba(12,14,12,0.97)', rim: 'rgba(110,150,110,0.45)', ink: '#e8eef5', dim: '#9fb0c2', faint: '#6f8090',
  gold: '#ffd34d', warm: 'rgba(255,190,80,0.16)', red: '#e2483d', redGround: 'rgba(120,22,18,0.95)', live: '#5BE36B',
};
const F = DECODER_FONT;

export interface MapLayerRow { id: string; label: string; words: string; n?: number; colour?: string; live?: boolean; others?: string[]; group?: string }
export interface MapStatsSection { mode: string; rows: Array<{ key: string; label: string; value: string; name: string }> }
export interface MapCtlState { auto: 'on' | 'off' | 'paused'; rings?: boolean; range?: boolean; rangeOffered?: boolean; labels?: boolean; aircraft: boolean }
export interface MapGainHandlers { onAgc: (on: boolean) => void; onSet: (at: number) => void; onUnlock?: (password: string) => Promise<boolean>; onMore?: () => void }

/** ★ The shared tuner's line + CHAT (dialChat.mapBarLine words — the SAME inputs as the controls bar's banner). */
export interface MapSharedProps {
  line: { show: boolean; text: string; dialLocked: boolean; why: string };
  chatDisabled: boolean;
  chatUnread: boolean;
  onChat: () => void;
}
/** ★ TUNE's mode list (airSeaTune.ts rows — THIS radio's vibeserver.json). */
export interface MapTuneProps {
  rows: AirSeaTuneRow[];
  chip: string;
  onPick: (id: string) => void;
  onRefused: (why: string) => void;
}
/** ★★★ What the SCREEN works out for the cards (SDRScreen — the web's asCardsSync): SERVERS, the gain row, STATS, RATE,
 *  HEALTH / TIME, the mini waterfall, SOURCES. Everything else comes from the map's own `trk`. */
export interface MapCardsIn {
  servers: MapChromeProps['servers'];
  gain: GainRow; gainH: MapGainHandlers;
  /** TUNE's line: the live mode's frequency, gain, capture */
  facts?: string;
  stats: MapChromeProps['stats'];
  rate: RateRow[];
  health: MapChromeProps['health'];
  time: MapChromeProps['time'];
  wf: MapChromeProps['wf'];
  sources?: Array<{ id: string; name: string; what: string; on: boolean }>;
  onSource?: (id: string, on: boolean) => void;
  /** LIST's facts: heard but not placed, sources hidden */
  listNote?: string;
}

export interface MapChromeProps {
  visible: boolean;
  winW: number; winH: number; insets: Insets;
  exit: { label: string; words: string; locked: boolean; onPress: () => void };
  /** SERVERS: LEAVE SERVER always works; then this server's other radios. */
  servers: { rows: Array<{ id: string; label: string; sub?: string }>; onPick: (id: string) => void;
             /** the card opened (and every 10 s while it stays open): the other radios' listeners are asked for (floored by the host) */
             onOpen?: () => void } | null;
  tune: { rows: AirSeaTuneRow[]; facts?: string; onPick: (id: string) => void; onRefused: (why: string) => void;
          gain: GainRow; gainH: MapGainHandlers } | null;
  layers: { rows: MapLayerRow[]; toggled: Record<string, boolean>; onToggle: (id: string, on: boolean) => void;
            fam: Array<{ glyph: ModeIconName; n: number; colour: string }>; note?: string } | null;
  stats: { sections: MapStatsSection[]; onPick: (key: string) => void } | null;
  map: { ctl: MapCtlState; onCtl: (k: 'auto' | 'fit' | 'rings' | 'range' | 'labels' | 'centre') => void;
         ranges?: Array<{ id: string; label: string; words: string }>; range?: string; onRange?: (id: string) => void;
         fitTo?: { groups: Array<{ id: string; label: string; n: number }>; pick: string; onPick: (id: string) => void };
         sources?: Array<{ id: string; name: string; what: string; on: boolean }>; onSource?: (id: string, on: boolean) => void;
         keyOn: boolean; onKey: (on: boolean) => void; extra?: React.ReactNode; far?: string;
         /** ★★★ AUTO here FOLLOWS THE LATEST (the HFDL map's SNAP), not a fit — its label and words say so */
         autoFollow?: boolean;
         /** ★★★ the receiver's place unknown: SET MY LOCATION (a row here — no map draws its own button) */
         setLocation?: () => void };
  chat: { disabled: boolean; unread: boolean; onOpen: () => void } | null;
  rate: RateRow[];
  health: { colour: string; content: React.ReactNode } | null;
  time: { colour: string; frame: string; content: React.ReactNode } | null;
  shared: { text: string; dialLocked: boolean; why: string } | null;
  lockWhy: string; onLocked: (why: string) => void;
  list: ListIn;
  wf: { offered: boolean; model: MapWfModel | null; colormap: string; store: React.MutableRefObject<MwfStore>;
        sinkRef: React.MutableRefObject<((f: MwfFrame) => void) | null>; onShown: (on: boolean) => void } | null;
  scale: { mpp: number; nm: boolean } | null;
  onZoom: (dir: 1 | -1) => void;
  /** a tap on the map (the page told us): transient cards fold */
  mapTapSeq: number;
  /** LIST's footprint for the page (the attribution, the fit's obstacle). */
  onFootprint?: (w: number, h: number, side: boolean) => void;
  /** the cards / maximised list cover the map: AUTO pauses (and resumes without a glide). */
  onCovered?: (covered: boolean) => void;
}
export interface ListIn {
  rows: TrackRow[];
  /** the ADS-B / NDB bodies' rows (AdsbLiveList / NdbLiveList), drawn first */
  extraRows?: React.ReactNode[];
  extraCount?: number;
  empty: string;
  status?: string;
  tabs?: Array<{ id: string; label: string; n: number }>; tab?: string; onTab?: (id: string) => void;
  sort?: string | null; onSort?: (s: string) => void;
  filter?: string; onFilterClear?: () => void;
  safety?: string[];
  selected?: string | null;
  onPick: (r: TrackRow) => void;
  /** under the rows: the data credit … */
  foot?: React.ReactNode;
  /** ★★ the mesh ROUTE key (the ISM maps): only while the chosen node carries a route — no dead control */
  route?: { name: string; on: boolean } | null;
  onRoute?: () => void;
}

const LIST_OPEN_KEY = 'lsv_map_list_open', LIST_MAX_KEY = 'lsv_map_list_max';
/* ★★★ THE CHROME'S SIZE (Stuart, build 586 on the Mac: "super tiny"): every chip, card, key and line of text is drawn at the
 *  window's scale (mapChrome chromeScale — 1 on a phone, up to 1.45 on an iPad or a Mac window, as the rest of the app). */
const KCtx = React.createContext(1);
const SCALED_KEYS = new Set(['width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight', 'fontSize', 'lineHeight', 'letterSpacing', 'gap',
  'padding', 'paddingHorizontal', 'paddingVertical', 'paddingTop', 'paddingBottom', 'paddingLeft', 'paddingRight',
  'margin', 'marginHorizontal', 'marginVertical', 'marginTop', 'marginBottom', 'marginLeft', 'marginRight',
  'borderRadius', 'borderTopLeftRadius', 'borderTopRightRadius', 'borderBottomLeftRadius', 'borderBottomRightRadius', 'top', 'left', 'right', 'bottom']);
const styleCache = new Map<number, typeof RAW>();
function scaledStyles(k: number): typeof RAW {
  const kk = Math.round(k * 100) / 100;
  let out = styleCache.get(kk);
  if (!out) {
    const o: Record<string, Record<string, unknown>> = {};
    for (const [name, st] of Object.entries(RAW)) {
      const t: Record<string, unknown> = {};
      for (const [key, v] of Object.entries(st as Record<string, unknown>)) t[key] = typeof v === 'number' && SCALED_KEYS.has(key) ? v * kk : v;
      o[name] = t;
    }
    out = StyleSheet.create(o as never) as typeof RAW;
    if (styleCache.size > 8) styleCache.clear();
    styleCache.set(kk, out);
  }
  return out;
}
function useStyles() { return scaledStyles(useContext(KCtx)); }
function useK() { return useContext(KCtx); }
type Transient = ChipId | 'help' | null;

export default function MapChrome(p: MapChromeProps) {
  const [card, setCard] = useState<Transient>(null);
  const [listOpen, setListOpen] = useState(p.winW >= 600);
  const [listMax, setListMax] = useState(false);
  const [pane, setPane] = useState<'list' | 'wf'>('list');
  const [healthOut, setHealthOut] = useState(false);
  const [timeOut, setTimeOut] = useState(false);
  const [pulseAt, setPulseAt] = useState(0);
  const [reduce, setReduce] = useState(false);
  useEffect(() => { AccessibilityInfo.isReduceMotionEnabled().then(setReduce).catch(() => {}); }, []);
  useEffect(() => {
    AsyncStorage.multiGet([LIST_OPEN_KEY, LIST_MAX_KEY]).then((kv) => {
      const o = kv[0][1], m = kv[1][1];
      if (o === '1' || o === '0') setListOpen(o === '1');
      if (m === '1') setListMax(true);
    }).catch(() => {});
  }, []);
  const keep = (k: string, v: boolean) => AsyncStorage.setItem(k, v ? '1' : '0').catch(() => {});

  // ── the offered chips (MAP-TEMPLATE rule 7) ──
  const rc = rateChip(p.rate);
  const chips: ChipId[] = [
    ...(p.servers ? ['servers' as const] : []), ...(p.tune ? ['tune' as const] : []), ...(p.layers && p.layers.rows.length ? ['layers' as const] : []),
    ...(p.stats && p.stats.sections.length ? ['stats' as const] : []), 'map', ...(p.chat ? ['chat' as const] : []),
    ...(p.health ? ['health' as const] : []), ...(p.time ? ['time' as const] : []), ...(rc.show ? ['rate' as const] : []),
  ];
  const famText = p.layers?.fam.map((f) => String(f.n)) ?? [];
  const k = chromeScale(p.winW, p.winH);
  const geo = chromeGeomAt({ w: p.winW, h: p.winH, insets: p.insets, chips,
                             layersW: 30 + famText.reduce((a, t) => a + 18 + t.length * 7, 0), layersCompactW: 30 + (famText[0]?.length ?? 1) * 7 + 18,
                             // ★ tall: each family an icon over its count, the chip as tall and as wide as they need
                             layersTallH: famText.length ? famText.length * 31 + 10 : 44, layersTallW: 16 + Math.max(15, ...famText.map((t) => t.length * 7.2)),
                             rateW: Math.min(150, 26 + rc.text.length * 6.6 + (rc.more ? 26 : 0)) }, k);
  const g = geo.g;

  // ── transient cards: one at a time; idle / map tap / another card fold it ──
  const idleT = useRef<ReturnType<typeof setTimeout> | null>(null);
  const idleArm = () => { if (idleT.current) clearTimeout(idleT.current); idleT.current = setTimeout(() => setCard(null), CARD_IDLE_MS); };
  useEffect(() => { if (card) idleArm(); else if (idleT.current) clearTimeout(idleT.current); return () => { if (idleT.current) clearTimeout(idleT.current); }; }, [card]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { setCard(null); }, [p.mapTapSeq]);
  useEffect(() => { if (!p.visible) { setCard(null); } }, [p.visible]);
  const toggle = (id: Transient) => { noteTouchInteraction(); setCard((c) => (c === id ? null : id)); };
  // ★ SERVERS open: each other radio's listeners, live while the card is open (every 10 s)
  useEffect(() => {
    if (card !== 'servers' || !p.servers?.onOpen) return;
    p.servers.onOpen();
    const t = setInterval(() => p.servers?.onOpen?.(), 10_000);
    return () => clearInterval(t);
  }, [card]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { p.onCovered?.(!!card || listMax); }, [card, listMax]);   // eslint-disable-line react-hooks/exhaustive-deps
  const phone = p.winW < 600;
  // ★ a phone shows one card: LIST steps aside while a transient card is open there
  const listShown = listOpen && !(phone && !!card && !g.short);

  const locked = !!p.lockWhy;
  const refuse = () => { setPulseAt(Date.now()); p.onLocked(p.lockWhy); };

  // ── the pad: chips → the open card → LIST → bottom-right; BACK folds a card, then un-maximises, then exits ──
  const nav = usePanelNav(p.visible, { flat: true, onBack: () => {
    if (card) { setCard(null); return true; }
    if (listMax) { setListMax(false); keep(LIST_MAX_KEY, false); return true; }
    return false;
  } });
  // ★ the jog / right stick zooms the map (tune only via TUNE)
  useEffect(() => {
    if (!p.visible) return;
    const sub = addKeyListener('down', (e) => {
      const k = String((e as { key?: string }).key ?? '');
      if (k === 'ZoomIn' || k === '+' || k === '=') { p.onZoom(1); return true; }
      if (k === 'ZoomOut' || k === '-') { p.onZoom(-1); return true; }
      return false;
    }, { priority: 5 });
    return () => sub.remove();
  }, [p.visible, p.onZoom]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ── LIST's footprint for the page ──
  const listH = !listShown ? LIST_CHIP_H * k : listMax ? g.list.maxH : Math.min(g.list.maxH, (g.list.rows * ROW_H + 34) * k);
  useEffect(() => { p.onFootprint?.(listShown ? g.list.w : 120, listH, g.list.side); }, [listShown, g.list.w, listH, g.list.side]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { p.wf?.onShown(listShown && pane === 'wf' && !!p.wf?.offered); }, [listShown, pane, p.wf?.offered]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { if (!p.wf?.offered && pane === 'wf') setPane('list'); }, [p.wf?.offered]);   // eslint-disable-line react-hooks/exhaustive-deps

  const s = scaledStyles(k);
  const box = (id: ChipId) => g.boxes[id]!;
  const cardAt = (id: ChipId) => cardBoxAt(geo, id, p.winW, p.winH);
  const chipIcon = (id: ChipId, col: string): React.ReactNode => {
    switch (id) {
      case 'exit': return <Text style={[s.exitX, { color: '#fff' }]}>{'✕'}</Text>;
      case 'servers': return <ModeIcon name="servers" size={22 * k} color={col} />;
      case 'tune': return <ModeIcon name="radio" size={22 * k} color={col} />;
      case 'stats': return <ModeIcon name="stats" size={22 * k} color={col} />;
      /* ★ MAP: the folded map with the camera's state on it — AUTO on (magnifier + A), paused (the pause), off (no entry) */
      case 'map': return (
        <View>
          <ModeIcon name="map" size={22 * k} color={col} />
          <View style={s.mapState}><ModeIcon name={p.map.ctl.auto === 'on' ? 'zoomAuto' : p.map.ctl.auto === 'paused' ? 'zoomPause' : 'noEntry'} size={12 * k}
                                             color={p.map.ctl.auto === 'off' ? '#ff6b5e' : p.map.ctl.auto === 'paused' ? '#ffb833' : C.live} /></View>
        </View>);
      case 'chat': return <ChatGlyph col={col} />;
      default: return null;
    }
  };
  const corner = (id: 'exit' | 'rate') => {
    const b = box(id);
    // ★ portrait / tall: the corner piece on the physical edge, its content padded in by the inset
    return g.short ? { left: b.x, top: b.y, width: b.w, height: b.h }
      : id === 'exit' ? { left: 0, top: 0, width: b.w, height: b.h, paddingLeft: p.insets.left, paddingTop: p.insets.top }
        : { right: 0, top: 0, width: b.w, height: b.h, paddingRight: p.insets.right, paddingTop: p.insets.top };
  };
  const open = (id: Transient) => card === id;

  return (
    <KCtx.Provider value={k}>
    <NavCtx.Provider value={nav.navCtx}>
      <View style={StyleSheet.absoluteFill} pointerEvents="box-none" onTouchStart={() => { noteTouchInteraction(); if (card) idleArm(); }}>
        {/* ── the ✕ — the first tab / the top-left corner piece ── */}
        <NavRow>
          <ChipKey style={[s.exit, corner('exit'), g.short ? s.tabShape : s.exitCorner, p.exit.locked && s.dim]} words={p.exit.locked ? `${p.exit.words} — ${p.lockWhy}` : p.exit.words}
                   onPress={() => (p.exit.locked ? refuse() : p.exit.onPress())}>
            <Text style={s.exitX}>{'✕'}</Text>
          </ChipKey>
          {(['servers', 'tune', 'layers', 'stats', 'map', 'chat'] as ChipId[]).filter((id) => g.boxes[id] && g.cardFrom[id] !== 'R').map((id) => renderChip(id))}
          {(['chat', 'map'] as ChipId[]).filter((id) => g.boxes[id] && g.cardFrom[id] === 'R').map((id) => renderChip(id))}
          {g.short && p.health && renderShortEdge('health')}
          {g.short && p.time && renderShortEdge('time')}
          {rc.show && (
            <ChipKey style={[s.rate, corner('rate'), g.short ? s.tabShape : s.rateCorner, open('rate') && s.chipOpen]}
                     words={rc.more ? `${rc.text}, and ${rc.more} more sources — show each` : `Live: ${rc.text}`}
                     onPress={() => { if (rc.more) toggle('rate'); }}>
              <Text style={[s.rateTxt, open('rate') && { color: C.gold }]} numberOfLines={2}>{rc.text}{rc.more ? `  +${rc.more} ⌄` : ''}</Text>
            </ChipKey>
          )}
        </NavRow>

        {/* ── HEALTH + TIME, tall: the app's EdgeChip pair, map-styled (icon-only tabs, × on the card) ── */}
        {!g.short && p.health && g.boxes.health && (
          <EdgeChip onMap top={box('health').y} tucked={!healthOut} label="Server health" onTuck={() => setHealthOut(false)} onShow={() => setHealthOut(true)}
                    tabColour={p.health.colour} frameColour={C.rim} tabIcon={<HealthTabIcon colour={p.health.colour} />}>{p.health.content}</EdgeChip>
        )}
        {!g.short && p.time && g.boxes.time && (
          <EdgeChip onMap top={box('time').y} tucked={!timeOut} label="Time remaining" onTuck={() => setTimeOut(false)} onShow={() => setTimeOut(true)}
                    tabColour={p.time.colour} frameColour={p.time.frame} tabIcon={<ClockTabIcon colour={p.time.colour} />}>{p.time.content}</EdgeChip>
        )}

        {/* ── the shared-tuner pill ── */}
        {!!p.shared && <Pill g={g} text={p.shared.text} locked={p.shared.dialLocked} pulseAt={pulseAt} reduce={reduce}
                             onPress={() => p.chat?.onOpen()} disabled={!p.chat || p.chat.disabled} />}

        {/* ── the open transient card ── */}
        {card && card !== 'help' && renderCard(card as ChipId)}
        {card === 'help' && (
          <Card title="HELP" at={{ x: Math.max(8, p.winW - g.br.right - 330 * k), y: Math.max(g.safe.t + 8, 60 * k), w: Math.min(330 * k, p.winW - 16), maxH: p.winH - 140 * k }} onClose={() => setCard(null)}>
            {MAP_HELP.filter((h) => h.id === 'list' || h.id === 'map' || chips.includes(h.id as ChipId)).map((h) => (
              <Text key={h.id} style={s.helpLine}><Text style={s.helpName}>{h.name}  </Text>{h.words}</Text>
            ))}
            {MAP_HELP_NOTES.map((n, i) => <Text key={i} style={s.helpNote}>{n}</Text>)}
            {p.map.autoFollow && <Text style={s.helpNote}>{`${AUTO_FOLLOW_LABEL} (MAP): on this map the camera goes to the newest flight as it is heard; press it again to stop following.`}</Text>}
          </Card>
        )}

        {/* ── LIST: the sticky drawer (or its chip) ── */}
        <ListDrawer g={g} p={p} shown={listShown} max={listMax} pane={pane} winW={p.winW} winH={p.winH}
                    onOpen={(v) => { setListOpen(v); keep(LIST_OPEN_KEY, v); if (v) setCard(null); }}
                    onMax={(v) => { setListMax(v); keep(LIST_MAX_KEY, v); }} onPane={setPane} height={listH} />

        {/* ── bottom-right: ? · scale · [− +] ── */}
        <BottomRight g={g} scale={p.scale} helpOpen={open('help')} onHelp={() => toggle('help')} onZoom={p.onZoom}
                     lift={!g.short && listShown && g.list.w >= p.winW - 60 ? listH : 0} hide={listMax && !g.short} />
      </View>
    </NavCtx.Provider>
    </KCtx.Provider>
  );

  function renderChip(id: ChipId) {
    const b = box(id);
    const isOpen = open(id);
    const col = isOpen ? C.gold : C.ink;
    const pos = g.short ? { left: b.x, top: b.y, width: b.w, height: b.h }
      : g.cardFrom[id] === 'R' ? { right: 0, top: b.y, width: b.w, height: b.h, paddingRight: p.insets.right }
        : { left: 0, top: b.y, width: b.w, height: b.h, paddingLeft: p.insets.left };
    const shape = g.short ? s.tabShape : g.cardFrom[id] === 'R' ? s.edgeR : s.edgeL;
    if (id === 'layers' && p.layers) {
      const fam = g.famCompact ? p.layers.fam.slice(0, 1) : p.layers.fam;
      return (
        <ChipKey key={id} style={[s.chip, shape, pos, isOpen && s.chipOpen]} words={`Layers: ${p.layers.fam.map((f) => `${f.n}`).join(', ') || 'what is on the map'}`} onPress={() => toggle('layers')}>
          <View style={g.short ? s.famRow : s.famCol}>
            {fam.length ? fam.map((f, i) => (
              <View key={i} style={g.short ? s.famItemRow : s.famItem}>
                <ModeIcon name={f.glyph} size={(g.short ? 15 : 15) * k} color={isOpen ? C.gold : f.colour} />
                <Text style={[s.famN, isOpen && { color: C.gold }]} numberOfLines={1}>{f.n > 999 ? '999+' : f.n}</Text>
              </View>
            )) : <ModeIcon name="aircraft" size={20 * k} color={col} />}
          </View>
        </ChipKey>
      );
    }
    if (id === 'chat' && p.chat) {
      return (
        <PulseWrap key={id} pulseAt={pulseAt} reduce={reduce} style={[s.chipPos, pos]}>
          <ChipKey style={[s.chip, shape, { width: '100%', height: '100%' }, p.chat.disabled && s.dim]} words={p.chat.disabled ? 'Chat is not available here' : `Chat${p.chat.unread ? ', unread' : ''}`}
                   onPress={() => { if (!p.chat!.disabled) p.chat!.onOpen(); }}>
            <ChatGlyph col={C.ink} />
            {p.chat.unread && <View style={s.badge} />}
          </ChipKey>
        </PulseWrap>
      );
    }
    const tuneLocked = id === 'tune' && locked;
    return (
      <ChipKey key={id} style={[s.chip, shape, pos, isOpen && s.chipOpen, tuneLocked && s.dim]}
               words={id === 'tune' && tuneLocked ? `Tune — ${p.lockWhy}` : `${CHIP_WORDS[id]}${isOpen ? ' — close' : ''}`}
               onPress={() => (tuneLocked ? refuse() : toggle(id))}>
        {chipIcon(id, col)}
      </ChipKey>
    );
  }
  function renderShortEdge(id: 'health' | 'time') {
    const b = g.boxes[id]; if (!b) return null;
    const v = id === 'health' ? p.health! : p.time!;
    return (
      <ChipKey key={id} style={[s.chip, s.tabShape, { left: b.x, top: b.y, width: b.w, height: b.h }, open(id) && s.chipOpen]}
               words={id === 'health' ? 'Server health' : 'Time remaining'} onPress={() => toggle(id)}>
        {id === 'health' ? <HealthTabIcon colour={v.colour} /> : <ClockTabIcon colour={v.colour} />}
      </ChipKey>
    );
  }
  function renderCard(id: ChipId) {
    const at = cardAt(id);
    const close = () => setCard(null);
    switch (id) {
      case 'servers': return p.servers && (
        <Card title="SERVERS" at={at} onClose={close}>
          {p.servers.rows.map((r) => <RowKey key={r.id} label={r.label} sub={r.sub} onPress={() => { close(); p.servers!.onPick(r.id); }} />)}
        </Card>);
      case 'tune': return p.tune && (
        <Card title="TUNE" at={at} onClose={close} faded={locked}>
          {!!p.tune.facts && <Text style={s.facts} numberOfLines={3}>{p.tune.facts}</Text>}
          <GainRowView row={p.tune.gain} h={p.tune.gainH} locked={locked} onLocked={refuse} />
          <TuneList t={p.tune} locked={locked} onLocked={refuse} onDone={close} />
        </Card>);
      case 'layers': return p.layers && (
        <Card title="LAYERS" at={at} onClose={close}>
          {!!p.layers.note && <Text style={s.facts}>{p.layers.note}</Text>}
          <LayerRows l={p.layers} />
        </Card>);
      case 'stats': return p.stats && <StatsCard at={at} st={p.stats} onClose={close} />;
      case 'map': return <MapCard at={at} m={p.map} onClose={close} />;
      case 'rate': return (
        <Card title="RATE" at={at} onClose={close}>
          {p.rate.map((r, i) => (
            <View key={i} style={s.rateRow} accessibilityLabel={`${r.name} ${r.rate}${r.mine ? ', this radio' : ''}${r.live ? '' : ', stalled'}`}>
              <View style={[s.dot, { backgroundColor: r.live ? C.live : '#7a6a50' }]} />
              <Text style={s.rateName} numberOfLines={1}>{r.name}{r.mine ? '  (this radio)' : ''}</Text>
              <Text style={s.rateVal}>{r.rate}</Text>
            </View>
          ))}
        </Card>);
      case 'health': return p.health && <Card title="SERVER HEALTH" at={at} onClose={close}>{p.health.content}</Card>;
      case 'time': return p.time && <Card title="TIME" at={at} onClose={close}>{p.time.content}</Card>;
      default: return null;
    }
  }
}

const CHIP_WORDS: Partial<Record<ChipId, string>> = { servers: 'Servers — leave, or switch radio', tune: 'Tune — mode and gain', layers: 'Layers',
  stats: 'Stats — newest, furthest, longest tracked', map: 'Map controls', chat: 'Chat' };

// ── pieces ──
function ChipKey({ style, words, onPress, children, hitSlop, padPress, preferred }: { style: unknown; words: string; onPress: () => void; children: React.ReactNode; hitSlop?: number;
                   /** what ENTER on a pad / keyboard does when it differs from a tap (the LIST stop: step into the rows) */
                   padPress?: () => void; preferred?: boolean }) {
  const { focused, viewRef } = useNavButton(padPress ?? onPress, false, { preferred: !!preferred });
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={style as never} onPress={onPress} activeOpacity={0.75} accessibilityRole="button" accessibilityLabel={words}
                      hitSlop={hitSlop}>
      {children}
      <NavRing on={focused} color={ring} radius={10} />
    </TouchableOpacity>
  );
}
function ChatGlyph({ col }: { col: string }) {
  const k = useK();   // ★ the web's chat glyph: the bubble with its three dots
  return (
    <Svg width={22 * k} height={22 * k} viewBox="0 0 24 24">
      <Path d="M4 5.5h16v10H10l-4 3.5v-3.5H4z" fill="none" stroke={col} strokeWidth={1.9} strokeLinejoin="round" />
      <Circle cx={9} cy={10.5} r={1.1} fill={col} /><Circle cx={12} cy={10.5} r={1.1} fill={col} /><Circle cx={15} cy={10.5} r={1.1} fill={col} />
    </Svg>
  );
}
function PulseWrap({ pulseAt, reduce, style, children }: { pulseAt: number; reduce: boolean; style: unknown; children: React.ReactNode }) {
  const styles = useStyles();
  const a = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!pulseAt || reduce) return;
    a.setValue(0);
    const loop = Animated.loop(Animated.timing(a, { toValue: 1, duration: 850, easing: Easing.out(Easing.quad), useNativeDriver: true }));
    loop.start();
    const t = setTimeout(() => { loop.stop(); a.setValue(0); }, ASK_PULSE_MS);
    return () => { clearTimeout(t); loop.stop(); };
  }, [pulseAt, reduce]);   // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <View style={style as never} pointerEvents="box-none">
      <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.pulseRing,
        { opacity: a.interpolate({ inputRange: [0, 1], outputRange: [0, 0.9] }).interpolate({ inputRange: [0, 0.9], outputRange: [0.9, 0] }),
          transform: [{ scale: a.interpolate({ inputRange: [0, 1], outputRange: [1, 1.35] }) }] }]} />
      {children}
    </View>
  );
}
function Pill({ g, text, locked, pulseAt, reduce, onPress, disabled }: { g: ReturnType<typeof chromeGeom>; text: string; locked: boolean; pulseAt: number; reduce: boolean; onPress: () => void; disabled: boolean }) {
  const styles = useStyles();
  const k = useK();
  const words = pillWords(text, Math.floor((g.pill.w - 24 * k) / (7.2 * k))   /* 11 pt mono ≈ 0.6 em + 0.5 spacing = 7.1 pt a character (6.4 let it ellipsise through the count) */);
  const w = Math.min(g.pill.w, Math.max(80 * k, (words.length * 7.2 + 26) * k));
  // ★★★ attached to its edge (the web's): the inset is padding inside it, so its words sit clear of the notch / home bar
  const edgePad = g.pill.at === 'top' ? { paddingTop: g.pill.pad } : { paddingBottom: g.pill.pad };
  return (
    <PulseWrap pulseAt={pulseAt} reduce={reduce} style={{ position: 'absolute', top: g.pill.y, left: g.pill.x + (g.pill.w - w) / 2, width: w, height: g.pill.h }}>
      <NavRow>
        <ChipKey style={[styles.pill, locked && styles.pillLocked, g.pill.at === 'top' ? styles.pillTop : styles.pillBottom, edgePad, { width: '100%', height: '100%' }]}
                 words={`${text} — ${disabled ? 'chat is not available here' : 'open the chat'}`} onPress={() => { if (!disabled) onPress(); }}>
          <Text style={[styles.pillTxt, locked && { color: '#ff9d8a' }]} numberOfLines={1}>{words}</Text>
        </ChipKey>
      </NavRow>
    </PulseWrap>
  );
}
function Card({ title, at, onClose, children, faded }: { title: string; at: { x: number; y: number; w: number; maxH: number }; onClose: () => void; children: React.ReactNode; faded?: boolean }) {
  const styles = useStyles();
  /* ★★★ THE OPEN CARD IS THE PAD'S SCOPE (the web's rule): its keys walk, the highlighted one is scrolled into view (its own
   *  ScrollView carries the scope's scrollProps), and BACK — a pad's back, Escape or Backspace — shuts it; the chip that opened it
   *  is still the highlight in the chrome's own walk beneath. The × is a full 44-point target (Stuart, build 586: "can't close an
   *  open card"). */
  const scope = usePanelNav(true, { flat: true, onBack: () => { onClose(); return true; } });
  return (
    <NavCtx.Provider value={scope.navCtx}>
      <View style={[styles.card, { left: at.x, top: at.y, width: at.w, maxHeight: at.maxH }, faded && { opacity: 0.6 }]} accessibilityViewIsModal={false}>
        <View style={styles.cardHead}>
          <Text style={styles.cardTitle}>{title}</Text>
          <NavRow><ChipKey style={styles.cardX} words={`Close ${title.toLowerCase()}`} onPress={onClose} hitSlop={8}><Text style={styles.cardXTxt}>{'\u00d7'}</Text></ChipKey></NavRow>
        </View>
        <ScrollView {...scope.scrollProps} style={{ maxHeight: at.maxH - 46 * (StyleSheet.flatten(styles.cardHead).height as number) / 46 }}
                    contentContainerStyle={[scrollLane, { paddingBottom: 8 }]} keyboardShouldPersistTaps="handled">{children}</ScrollView>
      </View>
    </NavCtx.Provider>
  );
}
function RowKey({ label, sub, onPress, on, right, dim }: { label: string; sub?: string; onPress: () => void; on?: boolean; right?: React.ReactNode; dim?: boolean }) {
  const styles = useStyles();
  return (
    <NavRow>
      <ChipKey style={[styles.rowKey, on && styles.rowKeyOn, dim && styles.dim]} words={`${label}${sub ? ', ' + sub : ''}`} onPress={onPress}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={styles.rowKeyTxt} numberOfLines={1}>{label}</Text>
          {!!sub && <Text style={styles.rowKeySub} numberOfLines={1}>{sub}</Text>}
        </View>
        {right}
      </ChipKey>
    </NavRow>
  );
}
function Key({ label, on, onPress, words, dim }: { label: string; on?: boolean; onPress: () => void; words: string; dim?: boolean }) {
  const styles = useStyles();
  return (
    <ChipKey style={[styles.key, on && styles.keyOn, dim && styles.dim]} words={words} onPress={onPress}>
      <Text style={[styles.keyTxt, on && { color: '#111' }]} numberOfLines={1}>{label}</Text>
    </ChipKey>
  );
}

/** ★★★ THE ONE-ROW GAIN: [AGC name] value ⌄ (AGC on) · slider value [AGC name] (manual) · 🔒 + inline password (locked). */
function GainRowView({ row, h, locked, onLocked }: { row: GainRow; h: MapGainHandlers; locked: boolean; onLocked: () => void }) {
  const styles = useStyles();
  const [pw, setPw] = useState(false);
  const [pwText, setPwText] = useState('');
  const [pwMsg, setPwMsg] = useState('');
  const [drag, setDrag] = useState<number | null>(null);
  if (row.kind === 'none' && !row.more) return null;
  const ask = () => { if (locked) { onLocked(); return true; } return false; };
  const more = row.more && h.onMore ? <Key label={'⌄'} words="More radio controls" onPress={() => { if (!ask()) h.onMore?.(); }} /> : null;
  if (row.kind === 'none') return <NavRow><View style={styles.gainRow}><Text style={styles.gainLbl}>RADIO</Text><View style={{ flex: 1 }} />{more}</View></NavRow>;
  const agc = row.name ? <Key label={row.name} on={row.kind === 'auto'} words={`${row.name} — ${row.kind === 'auto' ? 'on; press for manual gain' : 'off; press to let it set the gain'}`}
                              onPress={() => { if (ask() || row.kind === 'locked') return; h.onAgc(row.kind !== 'auto'); }} /> : null;
  const val = drag != null ? row.value.replace(/[\d.]+/, String(drag)) : row.value;
  return (
    <>
      <NavRow>
        <View style={styles.gainRow} accessibilityRole="adjustable" accessibilityLabel={`Gain ${row.value}`}>
          {row.kind === 'manual' ? (
            <>
              {!!row.label && <Text style={styles.gainLbl}>{row.label}</Text>}
              <GainSlide row={row} onDrag={setDrag} onSet={(v) => { if (!ask()) h.onSet(v); }} />
              <Text style={styles.gainVal}>{drag != null ? '' : val}</Text>
              {agc}
            </>
          ) : (
            <>
              {agc}
              <Text style={[styles.gainVal, { flex: 1 }]}>{row.value}</Text>
              {row.kind === 'locked' && <Key label={'\u{1F512}'} words="Gain locked by the owner — enter the admin password" onPress={() => setPw((v) => !v)} />}
            </>
          )}
          {row.unlocked && <Text style={styles.gainLbl}>{'\u{1F513}'}</Text>}
          {more}
        </View>
      </NavRow>
      {pw && row.kind === 'locked' && (
        <NavRow>
          <View style={styles.gainRow}>
            <TextInput style={styles.pwIn} secureTextEntry autoComplete="current-password" placeholder="Admin password" placeholderTextColor={C.faint}
                       value={pwText} onChangeText={setPwText} onSubmitEditing={() => void unlock()} accessibilityLabel="Admin password" />
            <Key label="UNLOCK" words="Unlock with the admin password" onPress={() => void unlock()} />
            {!!pwMsg && <Text style={styles.gainLbl}>{pwMsg}</Text>}
          </View>
        </NavRow>
      )}
    </>
  );
  async function unlock() {
    if (!pwText || !h.onUnlock) return;
    setPwMsg('…');
    const ok = await h.onUnlock(pwText).catch(() => false);
    if (ok) { setPw(false); setPwText(''); setPwMsg(''); } else setPwMsg('Not accepted');
  }
}
/** The gain slider: a pad reaches it too (Enter, then left / right a step — useNavRange), the ring round it. */
function GainSlide({ row, onDrag, onSet }: { row: Extract<GainRow, { min: number }>; onDrag: (v: number | null) => void; onSet: (v: number) => void }) {
  const { focused, adjusting, viewRef } = useNavRange((dir) => onSet(Math.max(row.min, Math.min(row.max, row.at + dir * row.step))));
  const { ring } = useNavFocus();
  return (
    <View ref={viewRef as never} style={{ flex: 1 }}>
      <Slider style={{ flex: 1, height: 32 }} minimumValue={row.min} maximumValue={Math.max(row.min + row.step, row.max)} step={row.step} value={row.at}
              onValueChange={(v) => onDrag(Math.round(v))}
              onSlidingComplete={(v) => { onDrag(null); onSet(Math.round(v)); }}
              minimumTrackTintColor={C.gold} maximumTrackTintColor="#444" thumbTintColor={C.gold} accessibilityLabel={`Gain ${row.value}`} />
      <NavRing on={focused || adjusting} color={adjusting ? '#ffb833' : ring} radius={8} />
    </View>
  );
}
function TuneList({ t, locked, onLocked, onDone }: { t: NonNullable<MapChromeProps['tune']>; locked: boolean; onLocked: () => void; onDone: () => void }) {
  const styles = useStyles();
  if (t.rows.length < 2) return null;
  let last = '';
  return (
    <>
      {t.rows.map((r) => {
        const head = TUNE_GROUP_LABEL[r.group] !== last ? TUNE_GROUP_LABEL[r.group] : '';
        last = TUNE_GROUP_LABEL[r.group];
        return (
          <React.Fragment key={r.id}>
            {!!head && <Text style={styles.grp}>{head}</Text>}
            <RowKey label={r.name} sub={r.freq} on={r.live} dim={!!r.why}
                    right={<>{r.live && <Text style={styles.lv}>LIVE</Text>}<Svg width={16} height={16} viewBox="0 0 16 16"><Path d={TUNE_ICON_PATH[r.icon]} fill="#9fc6e6" /></Svg></>}
                    onPress={() => { if (locked) { onLocked(); return; } if (r.why) { t.onRefused(r.why); return; } onDone(); if (!r.live) t.onPick(r.id); }} />
          </React.Fragment>
        );
      })}
    </>
  );
}
function LayerRows({ l }: { l: NonNullable<MapChromeProps['layers']> }) {
  const styles = useStyles();
  const badges = liveBadges(l.rows);
  let lastG = '';
  return (
    <>
      {l.rows.map((r, i) => {
        const on = l.toggled[r.id] !== false;
        const head = r.group && r.group !== lastG ? r.group : '';
        if (r.group) lastG = r.group;
        const b = badges[i];
        return (
          <React.Fragment key={r.id}>
            {!!head && <Text style={styles.grp}>{head}</Text>}
            <NavRow>
              <ChipKey style={[styles.rowKey, !on && styles.dim]} words={`${r.words}${r.n != null ? `, ${r.n}` : ''}${b ? `, ${b.words}` : ''} — ${on ? 'shown; hide it' : 'hidden; show it'}`}
                       onPress={() => l.onToggle(r.id, !on)}>
                {!!r.colour && <View style={[styles.dot, { backgroundColor: r.colour }]} />}
                <Text style={[styles.rowKeyTxt, { flex: 1 }]} numberOfLines={1}>{r.label}</Text>
                {b && <Text style={[styles.lv, b.kind === 'other' && styles.lvOut]}>{b.text}</Text>}
                {r.n != null && <Text style={styles.layerN}>{r.n}</Text>}
                <View style={[styles.toggle, on && styles.toggleOn]}><View style={[styles.toggleKnob, on && { alignSelf: 'flex-end' }]} /></View>
              </ChipKey>
            </NavRow>
          </React.Fragment>
        );
      })}
    </>
  );
}
function StatsCard({ at, st, onClose }: { at: { x: number; y: number; w: number; maxH: number }; st: NonNullable<MapChromeProps['stats']>; onClose: () => void }) {
  const styles = useStyles();
  const [mode, setMode] = useState('');
  const sec = st.sections.find((x) => x.mode === mode) ?? st.sections[0];
  return (
    <Card title="STATS" at={at} onClose={onClose}>
      {st.sections.length > 1 && (
        <NavRow><View style={styles.keyRow}>{st.sections.map((x) => <Key key={x.mode} label={x.mode} on={x.mode === sec?.mode} words={`${x.mode} stats`} onPress={() => setMode(x.mode)} />)}</View></NavRow>
      )}
      {sec?.rows.map((r) => <RowKey key={r.label} label={`${r.label}  ${r.value}`} sub={r.name} onPress={() => { onClose(); st.onPick(r.key); }} />)}
    </Card>
  );
}
function MapCard({ at, m, onClose }: { at: { x: number; y: number; w: number; maxH: number }; m: MapChromeProps['map']; onClose: () => void }) {
  const styles = useStyles();
  const [fitOpen, setFitOpen] = useState(false);
  const c = m.ctl;
  return (
    <Card title="MAP" at={at} onClose={onClose}>
      {!!m.ranges?.length && (
        <NavRow><View style={styles.keyRow}>
          {m.ranges.map((x) => <Key key={x.id} label={x.label} on={m.range === x.id} words={x.words} onPress={() => m.onRange?.(x.id)} />)}
        </View></NavRow>
      )}
      <NavRow><View style={styles.keyRow}>
        <ChipKey style={[styles.key, c.auto === 'on' && styles.keyOn, m.autoFollow && styles.keyWide]} words={autoWords(c.auto, !!m.autoFollow)}
                 onPress={() => m.onCtl('auto')}><AutoGlyph state={c.auto} />{m.autoFollow && <Text style={[styles.keyTxt, c.auto === 'on' && { color: '#111' }]}>{AUTO_FOLLOW_LABEL}</Text>}</ChipKey>
        <Key label="FIT" words={m.far ? `Frame it now. ${m.far}` : 'Frame it now'} onPress={() => m.onCtl('fit')} />
        {!!m.fitTo && m.fitTo.groups.length > 1 && <Key label="FIT TO ⌄" on={fitOpen} words="Choose which layer FIT frames" onPress={() => setFitOpen((v) => !v)} />}
        {c.aircraft && <Key label="RINGS" on={!!c.rings} words="Range rings around the receiver" onPress={() => m.onCtl('rings')} />}
        {c.aircraft && c.rangeOffered && <Key label="RANGE" on={!!c.range} words="ADS-B RANGE outline: the furthest aircraft heard on each bearing" onPress={() => m.onCtl('range')} />}
        {c.aircraft && <Key label="LABELS" on={c.labels !== false} words="Callsign and altitude beside each aircraft" onPress={() => m.onCtl('labels')} />}
        <Key label={'⌖'} words="Centre on the receiver" onPress={() => m.onCtl('centre')} />
      </View></NavRow>
      {fitOpen && m.fitTo && m.fitTo.groups.map((gq) => <RowKey key={gq.id} label={`${gq.label}  ${gq.n}`} on={m.fitTo!.pick === gq.id} onPress={() => { m.fitTo!.onPick(gq.id); setFitOpen(false); }} />)}
      {!!m.far && <Text style={styles.facts}>{m.far}</Text>}
      {m.extra}
      {!!m.sources?.length && (
        <>
          <Text style={styles.grp}>SOURCES</Text>
          {m.sources.map((x) => <RowKey key={`${x.id}:${x.name}`} label={x.name} sub={x.what} on={x.on} onPress={() => m.onSource?.(x.id, !x.on)} />)}
        </>
      )}
      {!!m.setLocation && <RowKey label="SET MY LOCATION" sub="Where this receiver is, so the distances mean something" onPress={() => { onClose(); m.setLocation?.(); }} />}
      <RowKey label={m.keyOn ? 'KEY ⌃  (hide)' : 'KEY ⌄'} sub="What the colours and symbols mean" on={m.keyOn} onPress={() => m.onKey(!m.keyOn)} />
    </Card>
  );
}
/** ★★★ AUTO's words: a fit (frame everything shown) — or, on the HFDL map, FOLLOW LATEST (the camera goes to the newest flight). */
export const AUTO_FOLLOW_LABEL = 'AUTO \u00b7 follow latest';
export function autoWords(state: 'on' | 'off' | 'paused', follow: boolean): string {
  if (follow) return state === 'off' ? `${AUTO_FOLLOW_LABEL} is off \u2014 press to follow the newest flight` : `${AUTO_FOLLOW_LABEL} is on \u2014 press to stop following`;
  return state === 'off' ? 'Auto fit is off \u2014 press to keep everything in view' : state === 'paused' ? 'Auto fit is paused \u2014 press to resume' : 'Auto fit is on \u2014 press to turn it off';
}
/** AUTO: the magnifier with an A; off = a red slash, paused = an amber pause. */
function AutoGlyph({ state }: { state: 'on' | 'off' | 'paused' }) {
  const col = state === 'on' ? '#111' : C.ink;
  return (
    <Svg width={22} height={18} viewBox="0 0 24 20">
      <Circle cx={9} cy={8} r={6} fill="none" stroke={col} strokeWidth={1.7} />
      <Path d="M13.5 12.5L18 17M6.5 11l2.5-6 2.5 6M7.4 9h3.2" fill="none" stroke={col} strokeWidth={1.5} strokeLinecap="round" />
      {state === 'off' && <Path d="M2 18L20 1" stroke="#ff5a4a" strokeWidth={2} strokeLinecap="round" />}
      {state === 'paused' && <Path d="M19 2v6M22 2v6" stroke="#ffb833" strokeWidth={2} strokeLinecap="round" />}
    </Svg>
  );
}

function BottomRight({ g, scale, helpOpen, onHelp, onZoom, lift, hide }: { g: ReturnType<typeof chromeGeom>; scale: MapChromeProps['scale']; helpOpen: boolean; onHelp: () => void; onZoom: (d: 1 | -1) => void; lift: number; hide: boolean }) {
  const styles = useStyles();
  if (hide) return null;
  const bar = scaleBar(scale, 80);
  const pos = { right: g.br.right, bottom: g.br.bottom + lift + (lift ? GAP : 0) };
  return (
    <View style={[styles.br, pos, g.br.horizontal ? { flexDirection: 'row', alignItems: 'flex-end' } : { flexDirection: 'column', alignItems: 'flex-end' }]} pointerEvents="box-none">
      {bar && (
        <View style={styles.scale} accessibilityLabel={`Scale: ${bar.label}`}>
          <Text style={styles.scaleTxt}>{bar.label}</Text>
          <View style={[styles.scaleBar, { width: bar.px }]} />
        </View>
      )}
      <NavRow>
        <View style={styles.zoomBox}>
          <ChipKey style={styles.zoomKey} words="Zoom out" onPress={() => onZoom(-1)}><Text style={styles.zoomTxt}>{'−'}</Text></ChipKey>
          <ChipKey style={styles.zoomKey} words="Zoom in" onPress={() => onZoom(1)}><Text style={styles.zoomTxt}>+</Text></ChipKey>
        </View>
        <ChipKey style={[styles.help, helpOpen && styles.chipOpen]} words="Help — what the chips and cards do" onPress={onHelp}>
          <Text style={[styles.helpTxt, helpOpen && { color: C.gold }]}>?</Text>
        </ChipKey>
      </NavRow>
    </View>
  );
}
/** A round-number scale bar ≤ maxPx: NM on the aviation map, else km. */
export function scaleBar(sc: { mpp: number; nm: boolean } | null, maxPx: number): { px: number; label: string } | null {
  if (!sc || !(sc.mpp > 0)) return null;
  const unit = sc.nm ? 1852 : 1000;
  const maxU = (sc.mpp * maxPx) / unit;
  const steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000];
  const v = [...steps].reverse().find((x) => x <= maxU) ?? steps[0];
  return { px: Math.max(8, Math.round((v * unit) / sc.mpp)), label: `${v} ${sc.nm ? 'NM' : 'km'}` };
}

// ── LIST: the sticky drawer ──
function ListDrawer({ g, p, shown, max, pane, winW, winH, onOpen, onMax, onPane, height }: {
  g: ReturnType<typeof chromeGeom>; p: MapChromeProps; shown: boolean; max: boolean; pane: 'list' | 'wf'; winW: number; winH: number;
  onOpen: (v: boolean) => void; onMax: (v: boolean) => void; onPane: (v: 'list' | 'wf') => void; height: number;
}) {
  const L = p.list;
  const count = (L.extraCount ?? 0) + L.rows.length;
  const wfOk = !!p.wf?.offered;
  // ★ the scroll pause: scrolled away from the top, the rows hold still and "‖ +N" counts what came in; a tap resumes
  const [held, setHeld] = useState<TrackRow[] | null>(null);
  const heldRef = useRef(held); heldRef.current = held;
  const rows = held ?? L.rows;
  const newN = held ? L.rows.filter((r) => !held.some((h) => h.key === r.key)).length : 0;
  useEffect(() => { setHeld(null); }, [L.tab]);
  const pad = g.short ? 0 : 0;
  const k = useK();
  const styles = useStyles();
  /* ★★★ STUART'S LIST FLOW (as the R36S handheld): the LIST is ONE stop in the chrome's walk (Down from it goes past the rows
   *  to the bottom-right group). ENTER steps INTO it (the first row with a place on the map); Up / Down move row by row and the
   *  list SCROLLS to keep the row in view (this scope's scrollProps on the list's ScrollView — PanelNav revealIn). ENTER on a
   *  row selects it — centred on the map, its card opened — and the highlight moves INTO that card (ItemCard). BACK in the card
   *  shuts it and the highlight is on the SAME row; BACK again leaves the list (the highlight back on the LIST stop). */
  const [inside, setInside] = useState(false);
  const [item, setItem] = useState<TrackRow | null>(null);
  useEffect(() => { if (!shown) { setInside(false); setItem(null); } }, [shown]);
  const scope = usePanelNav(inside, { flat: true, onBack: () => { setInside(false); setItem(null); return true; } });
  const scrollRef = scope.scrollRef;
  const firstPlaced = rows.find((r) => !r.noPos)?.key ?? rows[0]?.key;
  if (!shown) {
    // ★ the collapsed chip: one rounded rectangle [☰ n ⌃ | ≋]
    const pos = g.list.side ? { left: g.list.x, top: g.list.top } : { left: -2, bottom: -2 + pad };
    return (
      <NavRow>
        <View style={[styles.lChip, pos, !g.list.side && { paddingBottom: p.insets.bottom + 2, paddingLeft: p.insets.left + 2 }]}>
          <ChipKey style={styles.lChipKey} words={`Show the list, ${count}`} onPress={() => onOpen(true)}>
            <ModeIcon name="list" size={18 * k} color={C.ink} /><Text style={styles.lChipN}>{count}</Text><Text style={styles.lChipChev}>{'⌃'}</Text>
          </ChipKey>
          {wfOk && (
            <>
              <View style={styles.lChipSep} />
              <ChipKey style={styles.lChipKey} words="Open the live waterfall" onPress={() => { onPane('wf'); onOpen(true); }}>
                <ModeIcon name="waterfall" size={18 * k} color={C.ink} />
              </ChipKey>
            </>
          )}
        </View>
      </NavRow>
    );
  }
  const w = g.list.w;
  const sideTabs = g.list.side && pane === 'list' && (L.tabs?.length ?? 0) >= 2;
  /* ★★★ short landscape: the LIST is the LEFT SIDE PANEL, the full height under the top row (the web's) — never a short drawer */
  const pos = g.list.side ? { left: g.list.x, top: g.list.top, height: g.list.maxH }
    : { left: 0, bottom: 0, height: height + p.insets.bottom, paddingBottom: p.insets.bottom };
  const bodyH = (pos.height as number) - (g.list.side ? 0 : p.insets.bottom) - 34 * k - (sideTabs ? 38 * k : 0);
  const pickRow = (r: TrackRow) => { L.onPick(r); };
  const enterRow = (r: TrackRow) => { L.onPick(r); setItem(r); };
  return (
    <>
    <View style={[styles.drawer, { width: w }, pos, !g.list.side && styles.drawerFlush, inside && styles.drawerIn]} onTouchStart={noteTouchInteraction}>
      {/* ★ the LIST stop (the chrome's walk): ENTER steps into the rows — a tap on it is nothing more than a tap on the drawer */}
      {!inside && <NavRow><ListStop words={`The list, ${count} — Enter to step into the rows`} onEnter={() => setInside(true)} /></NavRow>}
      <NavCtx.Provider value={inside ? scope.navCtx : null}>
      {/* the folder tabs: swap (top-left), maximise (top-right) */}
      <NavRow>
        {wfOk && <ChipKey style={[styles.ftab, g.list.side ? { right: -34 * k, top: 0 } : { left: 8 * k, top: -26 * k }]} words={pane === 'wf' ? 'Show the list' : 'Show the live waterfall'}
                          onPress={() => onPane(pane === 'wf' ? 'list' : 'wf')}><ModeIcon name={pane === 'wf' ? 'list' : 'waterfall'} size={17 * k} color={C.ink} /></ChipKey>}
        {!g.list.side && <ChipKey style={[styles.ftab, { right: 8 * k, top: -26 * k }]} words={max ? 'Restore the list' : 'Maximise the list'} onPress={() => onMax(!max)}>
          <Text style={styles.ftabTxt}>{max ? '⌄' : '⌃'}</Text></ChipKey>}
      </NavRow>
      {/* ★★ the side panel's tabs: their own row at its head, wrapping — the foot bar is too narrow for them beside the keys */}
      {sideTabs && <NavRow><View style={styles.sideTabs}>
        {L.tabs!.map((t) => <Key key={t.id} label={`${t.label} ${t.n}`} on={L.tab === t.id} words={`${t.label}: ${t.n}`} onPress={() => L.onTab?.(t.id)} />)}
      </View></NavRow>}
      {pane === 'wf' && p.wf ? (
        <View style={{ height: bodyH }}>
          <MapMiniWaterfall width={w - 2} height={bodyH} screenW={winW} screenH={winH} model={p.wf.model} colormap={p.wf.colormap} store={p.wf.store} sinkRef={p.wf.sinkRef} />
        </View>
      ) : (
        <>
          {!!L.filter && <NavRow><View style={styles.listBar}><Text style={styles.status} numberOfLines={1}>{L.filter}</Text>
            <Key label={'× ALL'} words="Show every message" onPress={() => L.onFilterClear?.()} /></View></NavRow>}
          {(L.safety ?? []).map((t, i) => <Text key={i} style={styles.safety} numberOfLines={1}>{t}</Text>)}
          {!!L.route && <NavRow><View style={styles.listBar}>
            <Key label={L.route.on ? 'HIDE ROUTE' : 'ROUTE'} on={L.route.on} words={`${L.route.on ? 'Hide' : 'Show'} the route ${L.route.name}'s message came by, hop by hop`} onPress={() => L.onRoute?.()} />
            <Text style={styles.status} numberOfLines={1}>{L.route.name}</Text></View></NavRow>}
          <ScrollView ref={scrollRef} style={{ height: bodyH - ((L.filter ? 30 : 0) + (L.route ? 30 : 0) + (L.safety?.length ?? 0) * 18) * k }} contentContainerStyle={scrollLane} keyboardShouldPersistTaps="handled"
                      scrollEventThrottle={16}
                      onScroll={(e) => { scope.scrollProps.onScroll(e); const y = e.nativeEvent.contentOffset.y;
                                         if (y > 6 && !heldRef.current) setHeld(L.rows); else if (y <= 2 && heldRef.current) setHeld(null); }}>
            {L.extraRows}
            {rows.map((r) => <NavRow key={r.key}><ListRow r={r} sel={!!L.selected && L.selected === r.key} preferred={r.key === firstPlaced}
                                                          onPress={() => pickRow(r)} onEnter={() => enterRow(r)} /></NavRow>)}
            {!count && <Text style={styles.empty} numberOfLines={4}>{L.empty}</Text>}
            {L.foot}
          </ScrollView>
        </>
      )}
      {/* ── the foot bar: chip · tabs · ‖ +N · sort ── */}
      <NavRow>
        <View style={styles.foot}>
          <ChipKey style={styles.footChip} words={`Hide the list, ${count}`} onPress={() => onOpen(false)}>
            <ModeIcon name="list" size={16 * k} color={C.ink} /><Text style={styles.lChipN}>{count}</Text><Text style={styles.lChipChev}>{'⌄'}</Text>
          </ChipKey>
          <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ flex: 1 }} contentContainerStyle={{ alignItems: 'center', gap: 4 }} keyboardShouldPersistTaps="handled">
            {pane === 'list' && !g.list.side && (L.tabs?.length ?? 0) >= 2 && L.tabs!.map((t) => <Key key={t.id} label={`${t.label} ${t.n}`} on={L.tab === t.id} words={`${t.label}: ${t.n}`} onPress={() => L.onTab?.(t.id)} />)}
            {pane === 'wf' && !!p.wf?.model && <Text style={styles.status} numberOfLines={1}>{p.wf.model.info}</Text>}
          </ScrollView>
          {pane === 'list' && !!held && <Key label={`‖ +${newN}`} words={`Paused while you scroll — ${newN} new. Press to resume`} onPress={() => { setHeld(null); scrollRef.current?.scrollTo({ y: 0, animated: true }); }} />}
          {pane === 'list' && !!L.sort && (
            <ChipKey style={styles.key} words={L.sort === 'nearest' ? 'Sorted nearest first — press for latest' : 'Sorted latest first — press for nearest'}
                     onPress={() => L.onSort?.(L.sort === 'nearest' ? 'latest' : 'nearest')}>
              <ModeIcon name={L.sort === 'nearest' ? 'nearest' : 'clock'} size={16 * k} color={C.ink} />
            </ChipKey>
          )}
        </View>
      </NavRow>
      </NavCtx.Provider>
    </View>
    {item && <ItemCard r={item} g={g} list={L} onClose={() => setItem(null)} onCentre={() => L.onPick(item)} />}
    </>
  );
}
/** The LIST's one stop in the chrome's walk: ENTER (a pad's enter, the keyboard's Enter) steps into the rows. Drawn over the
 *  drawer's header line; a touch passes through to whatever is under it. */
function ListStop({ words, onEnter }: { words: string; onEnter: () => void }) {
  const { focused, viewRef } = useNavButton(onEnter);
  const { ring } = useNavFocus();
  return (
    <View ref={viewRef as never} style={StyleSheet.absoluteFill} pointerEvents="none" accessible accessibilityRole="button" accessibilityLabel={words}>
      <NavRing on={focused} color={ring} radius={12} />
    </View>
  );
}
/** ★★★ THE ITEM'S CARD for the pad (Stuart's flow, step 4): the row ENTER selected — centred on the map, its page card open —
 *  with the controls the pad can reach (the page's card is HTML, out of the pad's reach): ROUTE where its node carries one,
 *  CENTRE, CLOSE. Its own scope: the highlight lands on its first control; BACK shuts it and the highlight is on the same row. */
function ItemCard({ r, g, list, onClose, onCentre }: { r: TrackRow; g: ReturnType<typeof chromeGeom>; list: ListIn; onClose: () => void; onCentre: () => void }) {
  const styles = useStyles();
  const k = useK();
  const scope = usePanelNav(true, { flat: true, onBack: () => { onClose(); return true; } });
  const route = list.route && list.selected === r.key ? list.route : null;
  const x = g.list.side ? g.list.x + g.list.w + GAP * k : GAP * k;
  const kv = [r.sp, r.range && r.range !== 'no position' ? r.range : '', r.gs ?? '', r.age ? `heard ${r.age}` : ''].filter(Boolean).join(' \u00b7 ');
  return (
    <NavCtx.Provider value={scope.navCtx}>
      <View style={[styles.card, styles.itemCard, { left: x, top: g.safe.t + 70 * k, width: Math.min(320 * k, g.safe.r - x - GAP * k) }]} accessibilityLabel={`${r.name} card`}>
        <View style={styles.cardHead}>
          <Text style={styles.cardTitle} numberOfLines={1}>{r.name}</Text>
          <NavRow><ChipKey style={styles.cardX} words={`Close the ${r.name} card`} onPress={onClose} hitSlop={8}><Text style={styles.cardXTxt}>{'\u00d7'}</Text></ChipKey></NavRow>
        </View>
        {!!r.second && <Text style={styles.facts} numberOfLines={3}>{r.second}</Text>}
        {!!kv && <Text style={styles.facts} numberOfLines={2}>{kv}</Text>}
        <NavRow><View style={styles.keyRow}>
          {route && <ChipKey style={[styles.key, route.on && styles.keyOn]} preferred words={`${route.on ? 'Hide' : 'Show'} the route ${route.name}'s message came by`} onPress={() => list.onRoute?.()}>
            <Text style={[styles.keyTxt, route.on && { color: '#111' }]}>{route.on ? 'HIDE ROUTE' : 'ROUTE'}</Text></ChipKey>}
          <ChipKey style={styles.key} preferred={!route} words={`Centre ${r.name} on the map`} onPress={onCentre}><Text style={styles.keyTxt}>CENTRE</Text></ChipKey>
          <ChipKey style={styles.key} words="Close the card — back to the list" onPress={onClose}><Text style={styles.keyTxt}>CLOSE</Text></ChipKey>
        </View></NavRow>
      </View>
    </NavCtx.Provider>
  );
}
/** A row: [flag] CALLSIGN [MIL]/[EMERG] [FL | nm | kt] … SNR chip + age */
function ListRow({ r, sel, onPress, onEnter, preferred }: { r: TrackRow; sel: boolean; onPress: () => void; onEnter?: () => void; preferred?: boolean }) {
  const styles = useStyles();
  // ★ a tap selects and centres it (the page opens its card); ENTER on a pad / keyboard does that AND opens the pad's item card
  const { focused, viewRef } = useNavButton(onEnter ?? onPress, false, { preferred: !!preferred });
  const { ring } = useNavFocus();
  const kv = [r.sp, r.range && r.range !== 'no position' ? r.range : '', r.gs ?? ''].filter(Boolean);
  return (
    <TouchableOpacity ref={viewRef as never} style={[styles.lrow, sel && styles.lrowSel, r.em && styles.lrowEm, r.noPos && { opacity: 0.65 }, r.dx && styles.lrowDx]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="button" accessibilityLabel={`${r.name}${r.mil ? ', military' : ''}${r.em ? ', emergency' : ''}, ${r.second}${kv.length ? ', ' + kv.join(', ') : ''}${r.age ? ', ' + r.age : ''}`}>
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={styles.lrowTop}>
          <View style={[styles.sw, { backgroundColor: r.colour }]} />
          {!!r.flag && <Text style={styles.flag}>{r.flag}</Text>}
          <Text style={[styles.lname, r.em && { color: '#ffb0a0' }]} numberOfLines={1}>{r.name}</Text>
          {r.mil && <Text style={[styles.tag, styles.tagMil]}>MIL</Text>}
          {r.em && <Text style={[styles.tag, styles.tagEm]}>EMERG</Text>}
          {r.dx && <Text style={[styles.tag, { backgroundColor: C.gold, color: '#111' }]}>DX</Text>}
          {kv.length > 0 && <Text style={styles.kv} numberOfLines={1}>{kv.join(' | ')}</Text>}
        </View>
        <Text style={styles.lsecond} numberOfLines={1}>{r.second}</Text>
      </View>
      {r.snrMode ? <SnrChip mode={r.snrMode as SnrMode} db={r.snr} small /> : null}
      {!!r.age && <Text style={styles.age}>{r.age}</Text>}
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}
const RAW = {
  dim: { opacity: 0.42 },
  chip: { position: 'absolute', backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, alignItems: 'center', justifyContent: 'center' },
  chipPos: { position: 'absolute' },
  chipOpen: { backgroundColor: 'rgba(60,44,14,0.97)' },
  edgeL: { borderLeftWidth: 0, borderTopRightRadius: 12, borderBottomRightRadius: 12 },
  edgeR: { borderRightWidth: 0, borderTopLeftRadius: 12, borderBottomLeftRadius: 12 },
  tabShape: { borderTopWidth: 0, borderBottomLeftRadius: 10, borderBottomRightRadius: 10 },
  exit: { position: 'absolute', backgroundColor: C.redGround, borderColor: C.red, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
  exitCorner: { borderTopWidth: 0, borderLeftWidth: 0, borderBottomRightRadius: 14 },
  exitX: { color: '#fff', fontSize: 20, fontWeight: '700' },
  rate: { position: 'absolute', backgroundColor: C.ground, borderColor: C.rim, borderWidth: 1, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 6 },
  rateCorner: { borderTopWidth: 0, borderRightWidth: 0, borderBottomLeftRadius: 14 },
  rateTxt: { fontFamily: F, fontSize: 11, color: C.ink, textAlign: 'center' },
  famCol: { alignItems: 'center', gap: 3 },
  famRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  famItem: { alignItems: 'center' },
  famItemRow: { flexDirection: 'row', alignItems: 'center', gap: 2 },
  famN: { fontFamily: F, fontSize: 11.5, color: C.ink },
  badge: { position: 'absolute', top: 6, right: 6, width: 8, height: 8, borderRadius: 4, backgroundColor: '#ff6b5a' },
  pulseRing: { borderRadius: 14, borderWidth: 2, borderColor: '#ffcf7a' },
  pill: { backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 10 },
  pillTop: { borderTopWidth: 0, borderTopLeftRadius: 0, borderTopRightRadius: 0, borderBottomLeftRadius: 12, borderBottomRightRadius: 12 },
  pillBottom: { borderBottomWidth: 0, borderBottomLeftRadius: 0, borderBottomRightRadius: 0, borderTopLeftRadius: 12, borderTopRightRadius: 12 },
  pillLocked: { borderColor: 'rgba(255,120,100,0.6)', backgroundColor: 'rgba(58,20,16,0.96)' },
  pillTxt: { fontFamily: F, fontSize: 11, letterSpacing: 0.5, color: '#ffcf7a' },
  card: { position: 'absolute', backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, borderRadius: 12, paddingHorizontal: 8, paddingTop: 4, zIndex: 40 },
  cardHead: { flexDirection: 'row', alignItems: 'center', height: 46 },
  cardTitle: { flex: 1, fontFamily: F, fontSize: 11, letterSpacing: 1.4, color: C.dim },
  cardX: { width: 44, height: 44, marginRight: -8, alignItems: 'center', justifyContent: 'center' },
  cardXTxt: { color: C.ink, fontSize: 22, lineHeight: 24 },
  facts: { fontFamily: F, fontSize: 10.5, color: C.dim, paddingHorizontal: 4, paddingVertical: 4 },
  grp: { fontFamily: F, fontSize: 9.5, letterSpacing: 1.2, color: C.faint, paddingHorizontal: 6, paddingTop: 8, paddingBottom: 3 },
  rowKey: { flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 40, paddingHorizontal: 8, paddingVertical: 4, borderRadius: 8 },
  rowKeyOn: { backgroundColor: 'rgba(255,211,77,0.12)' },
  rowKeyTxt: { fontSize: 13, color: C.ink, fontWeight: '600' },
  rowKeySub: { fontFamily: F, fontSize: 10.5, color: C.dim },
  key: { minWidth: 34, height: 30, paddingHorizontal: 8, borderRadius: 7, borderWidth: 1, borderColor: C.rim, alignItems: 'center', justifyContent: 'center' },
  keyOn: { backgroundColor: C.gold, borderColor: C.gold },
  keyWide: { flexDirection: 'row', gap: 5, paddingHorizontal: 8 },
  keyTxt: { fontFamily: F, fontSize: 10.5, letterSpacing: 0.6, color: C.ink },
  keyRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 5, paddingVertical: 4 },
  gainRow: { flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 40, paddingHorizontal: 4 },
  gainLbl: { fontFamily: F, fontSize: 10, color: C.dim },
  gainVal: { fontFamily: F, fontSize: 12, color: C.ink, minWidth: 54, textAlign: 'right' },
  pwIn: { flex: 1, height: 32, borderWidth: 1, borderColor: C.rim, borderRadius: 6, color: C.ink, paddingHorizontal: 8 },
  lv: { fontFamily: F, fontSize: 9, fontWeight: '800', letterSpacing: 1, color: '#08140b', backgroundColor: C.live, borderWidth: 1, borderColor: C.live, borderRadius: 3, paddingHorizontal: 4, overflow: 'hidden' },
  lvOut: { backgroundColor: 'transparent', color: '#8fd79a', borderColor: '#4f8f5a' },
  layerN: { fontFamily: F, fontSize: 11, color: C.dim, minWidth: 26, textAlign: 'right' },
  toggle: { width: 30, height: 16, borderRadius: 8, backgroundColor: '#333', padding: 2 },
  toggleOn: { backgroundColor: '#3f8f52' },
  toggleKnob: { width: 12, height: 12, borderRadius: 6, backgroundColor: '#ddd' },
  dot: { width: 8, height: 8, borderRadius: 4 },
  rateRow: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 32, paddingHorizontal: 6 },
  rateName: { flex: 1, fontSize: 12.5, color: C.ink },
  rateVal: { fontFamily: F, fontSize: 12, color: C.ink },
  helpLine: { fontSize: 12.5, color: C.ink, paddingHorizontal: 4, paddingVertical: 3, lineHeight: 17 },
  helpName: { fontFamily: F, fontSize: 10.5, letterSpacing: 1, color: C.gold },
  helpNote: { fontSize: 11.5, color: C.dim, paddingHorizontal: 4, paddingVertical: 3, lineHeight: 16 },
  br: { position: 'absolute', gap: 6 },
  scale: { alignItems: 'flex-end', marginRight: 2 },
  scaleTxt: { fontFamily: F, fontSize: 9.5, color: '#fff', textShadowColor: '#000', textShadowRadius: 2 },
  scaleBar: { height: 4, borderWidth: 1.5, borderTopWidth: 0, borderColor: '#fff' },
  zoomBox: { flexDirection: 'row', backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, borderRadius: 10, overflow: 'hidden' },
  zoomKey: { width: 38, height: 36, alignItems: 'center', justifyContent: 'center' },
  zoomTxt: { color: C.ink, fontSize: 20 },
  help: { width: 36, height: 36, borderRadius: 18, backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, alignItems: 'center', justifyContent: 'center', marginLeft: 6 },
  helpTxt: { color: C.ink, fontSize: 17, fontWeight: '700' },
  lChip: { position: 'absolute', flexDirection: 'row', alignItems: 'center', backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, borderRadius: 12, minHeight: LIST_CHIP_H },
  lChipKey: { flexDirection: 'row', alignItems: 'center', gap: 6, height: LIST_CHIP_H, paddingHorizontal: 12 },
  lChipSep: { width: 1, height: 22, backgroundColor: C.rim },
  lChipN: { fontFamily: F, fontSize: 12.5, color: C.ink },
  lChipChev: { fontFamily: F, fontSize: 12, color: C.dim },
  drawer: { position: 'absolute', backgroundColor: C.ground, borderWidth: 1, borderColor: C.rim, borderRadius: 12 },
  sideTabs: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 4, height: 38, paddingHorizontal: 6, borderBottomWidth: 1, borderBottomColor: C.rim },
  drawerIn: { borderColor: '#ffd34d' },
  itemCard: { zIndex: 60, paddingBottom: 8 },
  mapState: { position: 'absolute', right: -7, bottom: -6, backgroundColor: 'rgba(12,14,12,0.95)', borderRadius: 7 },
  drawerFlush: { borderLeftWidth: 0, borderBottomWidth: 0, borderBottomLeftRadius: 0, borderTopLeftRadius: 0 },
  ftab: { position: 'absolute', width: 34, height: 26, backgroundColor: C.ground, borderWidth: 1, borderBottomWidth: 0, borderColor: C.rim, borderTopLeftRadius: 8, borderTopRightRadius: 8, alignItems: 'center', justifyContent: 'center' },
  ftabTxt: { color: C.ink, fontSize: 13 },
  foot: { height: 34, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.rim },
  footChip: { flexDirection: 'row', alignItems: 'center', gap: 5, height: 30, paddingHorizontal: 6 },
  listBar: { flexDirection: 'row', alignItems: 'center', gap: 6, height: 30, paddingHorizontal: 8 },
  status: { flex: 1, fontFamily: F, fontSize: 10.5, color: C.dim },
  safety: { height: 18, lineHeight: 18, paddingHorizontal: 10, fontFamily: F, fontSize: 10.5, backgroundColor: '#3a2a10', color: '#ffe2a8' },
  empty: { fontFamily: F, fontSize: 12, color: C.dim, padding: 10, lineHeight: 17 },
  lrow: { height: ROW_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 8 },
  lrowSel: { backgroundColor: 'rgba(255,229,102,0.12)', borderLeftWidth: 2, borderLeftColor: 'rgba(255,229,102,0.7)' },
  lrowEm: { backgroundColor: 'rgba(120,20,20,0.75)' },
  lrowDx: { borderLeftWidth: 2, borderLeftColor: C.gold },
  lrowTop: { flexDirection: 'row', alignItems: 'center', gap: 4, minWidth: 0 },
  sw: { width: 8, height: 8, borderRadius: 4 },
  flag: { fontSize: 11 },
  lname: { fontFamily: F, fontSize: 12.5, color: C.ink, flexShrink: 1 },
  tag: { fontFamily: F, fontSize: 8.5, fontWeight: '700', paddingHorizontal: 3, borderRadius: 3, overflow: 'hidden' },
  tagMil: { backgroundColor: '#3d5a2a', color: '#d9f2c4' },
  tagEm: { backgroundColor: '#c0392b', color: '#fff' },
  kv: { fontFamily: F, fontSize: 10, color: C.dim, flexShrink: 1, marginLeft: 4 },
  lsecond: { fontFamily: F, fontSize: 9.5, color: C.faint, marginTop: 1 },
  age: { fontFamily: F, fontSize: 10, color: C.dim, minWidth: 30, textAlign: 'right' },
} as const;
