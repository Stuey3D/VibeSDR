/**
 * GalleryOverlay — a VibeServer's GALLERY in the app (V12, 2026-10-22): what the server's decoders heard, kept on the
 * server (vibe_gallery.h) — background decoding while nobody listened (Stuart, for NickB's NAVTEX the night Storm
 * Austen came in) and listeners' own decoders. The same list as the web client's GALLERY key: newest first, filters by
 * decoder, a NAVTEX message / RTTY run / FT8 summary opens as text, a WEFAX chart or SSTV picture as an image, and
 * OPEN hands the item to the phone (its own browser saves or shares it).
 * ★ Text from the air is shown as text, never parsed or linked. Read-only: nothing here changes the server.
 * ★★ V12 beta 4 — PAGES (utils/galleryPager.ts, the web client's rule): 25 a page, newest first, the pager at the
 *   bottom ("‹ Newer · 1 … 6 7 8 … 14 · Older ›", "Page 7 of 14"); the filter kept across pages, page 1 on a filter
 *   change; BACK from an item returns to the same page and scroll — the list stays MOUNTED (hidden) under the item, so
 *   it is neither reloaded nor scrolled back to the top. An older server sends no "total": its newest 300, no pager.
 * ★ Offered only on a VibeServer (SDRScreen passes `galleryAvail` from client.isVibe): a GALLERY key on a receiver that
 *   has none would be a dead control (AGENTS.md).
 * ★★ TRANSLATE (2026-10-23) beside ‹ BACK on a text item: the message through the phone's own translator — the one
 *   TRANSCRIBE's captions use (Apple Translation / ML Kit), into TRANSLATE TO (the phone's language when that is off).
 *   Line by line, so the layout survives; ZCZC/NNNN, figures, positions, fade markers and garbage are left as received
 *   (utils/galleryTranslate.ts). Shown in place, marked MACHINE TRANSLATION, ORIGINAL toggles back. Drawn only when the
 *   phone can translate, the language is known and is not already the target.
 */
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ActivityIndicator, FlatList, Image, Linking, Modal, Platform, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { usePopupTheme, scrollLane, type PopupTokens } from './PopupShell';
import { ModeLabel } from './ModeIcon';
import { withReadAuth } from '../services/vibeAuth';
import TlmCharts from './TlmCharts';   // ★★ beta91 TELEMETRY CHARTS: a FUNcube pass item is shown as its charts
import { tlmLoadPasses, tlmParseCsv, tlmSatOfTitle, type TlmPass } from '../utils/tlmCharts';
import { rankCard } from '../utils/spotRanks';   // ★ V12 beta90 this server's PSK Reporter / WSPR places
import { galleryClampPage, galleryPageCount, galleryPageLabel, galleryPageQuery, galleryPagerSlots,
         GALLERY_RANGES, GALLERY_RANGE_KEY, galleryRangeOf, galleryRangeQuery, galleryRangePhrase, galleryKnowsRange, galleryRangeFooter }
  from '../utils/galleryPager';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { galleryOriginTag, galleryOriginWords } from '../utils/galleryOrigin';
import { logRadioChoices, keepRadioPick, radioQuery, heardByText, radioChipLabel, type LogRadio } from '../utils/logRadio';   // ★ beta92
import { langName } from '../services/transcriber';
// ★ the translate key's plan and run, shared with the NAVTEX map's cards (MapOverlay) — one implementation
import { planGalleryTranslate, runGalleryTranslate, galleryTranslateBanner, type GalleryTrPlan } from '../services/galleryTranslatePlan';
import { ismIsNodeCard, ismNodesParse, ismGalMerge, ismNodeCardBand, type IsmGalNode } from '../services/ism';
import { navSanitise, type NavReport } from '../services/navtexMap';
import { dscGalSummary, dscPinsFromGallery, DSC_GAL_WORDS } from '../services/shippingMap';   // ★ beta88 the DSC row
import { digiGalCard, digiGalQuery, digiGalItemWindow, DIGI_GAL_KINDS } from '../utils/digiMapShared';   // ★ beta92 the DIGI MODES MAP row (the web's words)

export interface GalleryItem {
  f: string; k: string; o: string; t: number; hz: number; m: string; d: string; p: string; r: string;
  title: string; partial?: boolean; wl?: boolean; bytes: number;
  /** ★ V12 2026-10-25: a standing card's slot ("ismsum-433-…"), and an item's own download name ("ISM 433.92 2026-10-25.txt"). */
  s?: string; dn?: string;
  /** ★ V12 2026-10-26 (DSC): 3 distress · 2 urgency · 1 safety, and the position a call sent ("plat"/"plon"). */
  lv?: number; plat?: number; plon?: number;
  /** ★ V12 beta92 the radio id(s) that heard it ("a1b2c3d4[,…]"; absent = unknown — vibe_log_radio.h). */
  ri?: string;
}
const KINDS: { id: string; label: string }[] = [
  { id: '', label: 'ALL' }, { id: 'navtex', label: 'NAVTEX' }, { id: 'rtty', label: 'RTTY' },
  { id: 'wefax', label: 'WEFAX' }, { id: 'sstv', label: 'SSTV' },
  // ★ 'DIGITAL SPOTS' (Stuart 2026-10-28): the ft8 kind holds FT8 cards AND the Band digital-spots summaries (every mode)
  { id: 'ft8', label: 'DIGITAL SPOTS' },
  // ★★ V12 2026-10-25: CW — a listener's decoded text (a run) and the skimmer's spots summary cards (vibe_bgdecode.h)
  { id: 'cw', label: 'CW' },
  // ★★ V12 2026-10-26: PSK31/63 — the same, from the server's PSK decoder and skimmer
  { id: 'psk', label: 'PSK' },
  // ★★ V12 2026-10-25: ISM — each band's day as a .txt of readings, and the day's summary card (vibe_ism_gallery.h)
  { id: 'ism', label: 'ISM' },
  // ★★ V12 2026-10-26: NDB — each day's heard log (a card) and its map (a picture) (vibe_ndb_gallery.h)
  { id: 'ndb', label: 'NDB' },
  // ★★ V12: AIS (ships) and APRS (stations) — each a day's log of messages and a summary card of who was heard (the web's chips)
  { id: 'ais', label: 'AIS' }, { id: 'aprs', label: 'APRS' },
  // ★★ V12 2026-10-26: DSC — each frequency's day of calls as a .txt, every distress/urgency call on its own (vibe_dsc_gallery.h)
  { id: 'dsc', label: 'DSC' },
  { id: 'sitora', label: 'SITOR-A' },
  // ★★ V12 balloons: the reports as a run, and each SSDV picture (a .jpg) — vibe_bgdecode.h BalloonGalleryTap
  { id: 'balloon', label: 'BALLOONS' },
  // ★★ V12 beta91 FUNcube telemetry: a .csv per pass (shown as charts) and the day's log (vibe_fctlm_gallery.h)
  // ★ beta92: METEOR LRPT pictures — the web page had the key, the app never did (Stuart 11-23, "the meteor button is also missing")
  { id: 'lrpt', label: 'METEOR' },
  { id: 'fctlm', label: 'FUNCUBE' },
];
/** ★★ beta91 a FUNcube pass item (its CSV) — drawn as charts, not as text */
export const isTlmPass = (it: { k: string; f: string }) => it.k === 'fctlm' && /\.csv$/i.test(it.f);
/** ★★ beta91 TELEMETRY CHARTS: a server's gallery as a reader — a path ("/vibeserver/gallery.json?…") → its text, or
 *  null (not there / no gallery). The SATELLITE box's LAST PASSES and a pass item read through it. */
export function galleryReader(base: string, readAuth = ''): (path: string) => Promise<string | null> {
  const b = base.replace(/\/+$/, '');
  return async (path: string) => {
    try { const r = await fetch(withReadAuth(`${b}${path}`, readAuth), { cache: 'no-store' } as any); return r.ok ? await r.text() : null; }
    catch { return null; }
  };
}
const p2 = (n: number) => String(n).padStart(2, '0');
export function galleryWhen(t: number): string {
  const d = new Date(t * 1000);
  return `${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())} ${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())} UTC`;
}
export function galleryFreq(hz: number): string { return hz < 30e6 ? `${(hz / 1e3).toFixed(1)} kHz` : `${(hz / 1e6).toFixed(4)} MHz`; }
function size(b: number): string { return b >= 1_048_576 ? `${(b / 1_048_576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`; }

export default function GalleryOverlay({ visible, base, readAuth = '', translateTo = '', onClose, onNodesMap, onNavtexMap, onShipMap, onDigiMap }: {
  /** TRANSCRIBE's TRANSLATE TO ('' = off → the phone's own language). */
  visible: boolean; base: string; readAuth?: string; translateTo?: string; onClose: () => void;
  /** ★★ V12 2026-11-01 (Stuart: "being able to open up the map in the Gallery … we can see which stations we received"):
   *  a day's "stations received" card (slot "ismnodes-…", vibe_ism_gallery.h) has MAP — its nodes, handed to the screen's
   *  ISM map. Absent where there is no map (Apple TV): no key. */
  onNodesMap?: ((title: string, nodes: IsmGalNode[], rx: { lat: number; lon: number } | null, refresh: (() => Promise<IsmGalNode[] | null>) | null) => void) | null;
  /** ★★ V12 2026-11-01: the NAVTEX stations received — a standing row on the first page of ALL / NAVTEX with MAP. */
  onNavtexMap?: (() => void) | null;
  /** ★★ V12 2026-11-02 THE SHIPPING MAP from a row: a DSC call's pinned position (DSC first, on that call) or an AIS day
   *  (the ships first). The keys are shippingMap.ts's (a gallery DSC pin is "d:g:<file>"). */
  onShipMap?: ((focus: 'dsc' | 'ships', key: string) => void) | null;
  /** ★★ beta92 THE DIGI MODES MAP from the gallery (the web's SPOTS row and a spots card's MAP): the window to open on. Absent
   *  where there is no map (Apple TV): no key. */
  onDigiMap?: ((window: string) => void) | null;
}) {
  const pt = usePopupTheme();
  const s = useMemo(() => makeStyles(pt), [pt]);
  const b = base.replace(/\/+$/, '');
  const [kind, setKind] = useState('');
  /* ★★ V12 beta92 THE RADIO FILTER (src/utils/logRadio.ts, docs/v12/LOG-RADIO-FILTER.md): the gallery stays one per
   *  server; the chips ALL RADIOS · each radio by name appear only when the list holds items from 2+ radios. A view. */
  const [radio, setRadio] = useState('');
  const [radios, setRadios] = useState<LogRadio[] | null>(null);
  /* ★★ V12 beta92 THE TIME RANGE (Stuart, 2026-11-23: "the gallery needs some sorting last hour last day last week last
   *  month buttons"): the maps' keys 1 H · 24 H · 7 DAYS · 30 DAYS · ALL, sent as since= (a view). Remembered on this
   *  device (AsyncStorage); default ALL. Drawn only when the server says it knows since= — never dead keys. */
  const [range, setRange] = useState('all');
  const [rangeOk, setRangeOk] = useState(false);
  const [rangeLoaded, setRangeLoaded] = useState(false);
  useEffect(() => {
    AsyncStorage.getItem(GALLERY_RANGE_KEY).then((v) => setRange(galleryRangeOf(v))).catch(() => {}).finally(() => setRangeLoaded(true));
  }, []);
  const pickRange = (id: string) => {
    setRange(id); setPage(1);
    AsyncStorage.setItem(GALLERY_RANGE_KEY, id).catch(() => {});
  };
  const [page, setPage] = useState(1);
  const [pages, setPages] = useState(1);
  const [items, setItems] = useState<GalleryItem[] | null>(null);
  const listRef = React.useRef<FlatList<GalleryItem>>(null);
  const [note, setNote] = useState('');
  const [rangeShown, setRangeShown] = useState('all');   // ★ beta92 the range the shown list was answered for
  const [open, setOpen] = useState<GalleryItem | null>(null);
  const [text, setText] = useState('');
  const [textOk, setTextOk] = useState(false);
  // ★★ beta91 the open pass item's charts: its own pass, and the passes up to it
  const [tlmAcross, setTlmAcross] = useState<TlmPass[] | null>(null);
  const tlmPass = useMemo(() => (open && isTlmPass(open) && textOk ? tlmParseCsv(text) : null), [open, text, textOk]);
  useEffect(() => {
    setTlmAcross(null);
    if (!open || !isTlmPass(open)) return;
    let live = true;
    void tlmLoadPasses(galleryReader(b, readAuth), tlmSatOfTitle(open.title), open.t).then((x) => { if (live) setTlmAcross(x); });
    return () => { live = false; };
  }, [open, b, readAuth]);
  // ★ TRANSLATE: the plan (what is sent, from what, into what), the result, which is shown, and what to say.
  const [trPlan, setTrPlan] = useState<GalleryTrPlan | null>(null);
  const [trText, setTrText] = useState<string | null>(null);
  const [showTr, setShowTr] = useState(false);
  const [trBusy, setTrBusy] = useState(false);
  const [trNote, setTrNote] = useState('');

  // ★ read in load's catch without making load depend on it (a dependency would refetch on every page-count change)
  const pagesRef = useRef(1); pagesRef.current = pages;
  const load = useCallback(async () => {
    setItems(null); setNote('');
    try {
      let r = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?${galleryPageQuery(page, kind)}${radioQuery(radio)}${galleryRangeQuery(range)}`, readAuth), { cache: 'no-store' } as any);
      let j = await r.json();
      setRangeOk(r.ok && galleryKnowsRange(j || {}));
      {   // ★ beta92 the radio chips: a pick no longer offered (one radio left in this kind) falls back to ALL
        const choices = logRadioChoices(j?.radios);
        setRadios(choices);
        const kept = keepRadioPick(radio, choices);
        if (kept !== radio) { setRadio(kept); setPage(1); return; }
      }
      const paged = typeof j?.total === 'number';
      if (r.ok && !j?.error && !paged) {   // ★ an older server: its newest 300, no pages
        r = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?limit=300${kind ? `&kind=${kind}` : ''}`, readAuth), { cache: 'no-store' } as any);
        j = await r.json();
      }
      // ★ V12 2026-11-04 a failed PAGE keeps the pager (Stuart: "trying to go back pages … the page buttons disappeared") —
      //   only a server with no gallery at all (404) loses it. Same rule as the web client.
      if (!r.ok || j?.error) { setItems([]); if (r.status === 404) setPages(1); setNote(String(j?.error || 'This server has no gallery.')); return; }
      const n = paged ? galleryPageCount(j.total) : 1;
      setPages(n);
      if (paged && page > n && j.total > 0) { setPage(n); return; }   // ★ the gallery shrank under us
      setItems(Array.isArray(j.items) ? j.items : []);
      listRef.current?.scrollToOffset({ offset: 0, animated: false });
      // ★★ The footer counts what is SHOWN (galleryPager.ts galleryFooter): the filter's kind, then the whole gallery.
      setNote(j.enabled === false ? 'This server keeps no gallery.'
        : galleryRangeFooter(j, kind, KINDS.find((k) => k.id === kind)?.label || kind.toUpperCase(), range, size));
      setRangeShown(j.since ? range : 'all');
    } catch { setItems([]); setNote(pagesRef.current > 1 ? 'That page did not load — try again, or pick another page.' : 'This server has no gallery (it may need updating).'); }
  }, [b, kind, page, readAuth, radio, range]);
  // ★ Loaded on opening, a filter and a page — NOT on BACK from an item (the list under it is still there).
  useEffect(() => { if (visible && rangeLoaded) void load(); }, [visible, load, rangeLoaded]);
  useEffect(() => { if (!visible) { setOpen(null); setPage(1); setRadio(''); } }, [visible]);

  // ★ the NAVTEX stations row: asked once per opening, on page 1 of ALL / NAVTEX
  const [navSum, setNavSum] = useState<NavReport | null>(null);
  useEffect(() => {
    if (!visible || !onNavtexMap || page !== 1 || (kind !== '' && kind !== 'navtex')) { setNavSum(null); return; }
    let live = true;
    fetch(withReadAuth(`${b}/vibeserver/gallery/navtex-stations.json?since=0`, readAuth), { cache: 'no-store' } as any)
      .then((r) => (r.ok ? r.json() : null)).then((j) => { if (live) setNavSum(navSanitise(j)); }).catch(() => {});
    return () => { live = false; };
  }, [visible, kind, page, b, readAuth, !!onNavtexMap]);   // eslint-disable-line react-hooks/exhaustive-deps
  /* ★★ beta88 THE DSC CALLS WITH A POSITION (Stuart, 2026-11-20: "i have no map button to view them from the gallery on the
   *  pi500") — the web's standing row: on page 1 of ALL / DSC, MAP opens the SHIPPING MAP, DSC first. Only when the gallery
   *  holds a pinned DSC call (dscGalSummary; no dead control). */
  const [dscSum, setDscSum] = useState<string | null>(null);
  /* ★★ V12 beta90 THIS SERVER'S RANKING (src/utils/spotRanks.ts): a standing card on page 1 of ALL while the owner has
   *  PSK Reporter reporting on — the places the directory published for this receiver's callsign, source and age named.
   *  Nothing to press (no dead control); absent when the server has nothing to say. */
  const [rankC, setRankC] = useState<{ title: string; src: string } | null>(null);
  /* ★★ beta92 THE DIGI MODES MAP ROW (Stuart 11-23: the web's ALL page has "SPOTS · DIGI MODES MAP · 5,000 stations heard ·
   *  FT8 4175 · …" with MAP, the app had none): page 1 of ALL and the spot kinds, the radio filter's pick, the web's words
   *  (digiMapShared.ts digiGalCard). No row where the store has nothing (never a dead MAP key). */
  const [digiC, setDigiC] = useState<{ title: string; src: string } | null>(null);
  useEffect(() => {
    if (!visible || !onDigiMap || page !== 1 || !(kind === '' || DIGI_GAL_KINDS.includes(kind))) { setDigiC(null); return; }
    let live = true;
    fetch(withReadAuth(`${b}${digiGalQuery(Date.now() / 1000)}${radioQuery(radio)}`, readAuth), { cache: 'no-store' } as any)
      .then((r) => (r.ok ? r.json() : null)).then((j) => { if (live) setDigiC(digiGalCard(j)); }).catch(() => {});
    return () => { live = false; };
  }, [visible, kind, page, b, readAuth, radio, !!onDigiMap]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (!visible || page !== 1 || !['', 'ft8', 'cw', 'psk'].includes(kind)) { setRankC(null); return; }   // ★ beta92: ALL + the spot kinds (Stuart 11-23)
    let live = true;
    fetch(withReadAuth(`${b}/vibeserver/gallery/ranks.json`, readAuth), { cache: 'no-store' } as any)
      .then((r) => (r.ok ? r.json() : null)).then((j) => { if (live) setRankC(rankCard(j, Date.now() / 1000)); }).catch(() => {});
    return () => { live = false; };
  }, [visible, kind, page, b, readAuth]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (!visible || !onShipMap || page !== 1 || (kind !== '' && kind !== 'dsc')) { setDscSum(null); return; }
    let live = true;
    fetch(withReadAuth(`${b}/vibeserver/gallery.json?kind=dsc&limit=300`, readAuth), { cache: 'no-store' } as any)
      .then((r) => (r.ok ? r.json() : null)).then((j) => { if (live) setDscSum(dscGalSummary(dscPinsFromGallery(j))); }).catch(() => {});
    return () => { live = false; };
  }, [visible, kind, page, b, readAuth, !!onShipMap]);   // eslint-disable-line react-hooks/exhaustive-deps
  const fileUrl = (it: GalleryItem) => withReadAuth(`${b}/vibeserver/gallery/file/${encodeURIComponent(it.f)}`, readAuth);
  /* ★ MAP: every day card of the band still in the gallery (the web's ismGalMapOpen — at most 31, one fetch at a time),
   *  merged; the screen's time filter picks. Today's card is offered for a refresh each minute (it is rewritten that often
   *  while ISM runs). */
  const nodesMap = async (it: GalleryItem) => {
    try {
      const band = ismNodeCardBand(it.s);
      let cards: GalleryItem[] = [];
      try {
        const l = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?kind=ism&limit=1000`, readAuth), { cache: 'no-store' } as any);
        const j = l.ok ? await l.json() as { items?: GalleryItem[] } : {};
        cards = (j.items ?? []).filter((c) => ismIsNodeCard(c) && ismNodeCardBand(c.s) === band).sort((x, y) => y.t - x.t).slice(0, 31);
      } catch { /* the one pressed, below */ }
      if (!cards.some((c) => c.f === it.f)) cards.unshift(it);
      const per = new Map<string, IsmGalNode[]>();
      let rx: { lat: number; lon: number } | null = null;
      const one = async (c: GalleryItem) => {
        const r = await fetch(fileUrl(c), { cache: 'no-store' } as any);
        if (!r.ok) return false;
        const p = ismNodesParse(await r.text(), c.t, Math.floor(Date.now() / 1000));
        per.set(c.f, p.nodes); rx = rx ?? p.rx;
        return true;
      };
      for (const c of cards) await one(c).catch(() => false);
      if (!per.size) { setNote('This item is no longer on the server (the gallery is full and keeps the newest).'); return; }
      const merged = () => ismGalMerge([...per.values()], Math.floor(Date.now() / 1000));
      const today = cards.find((c) => Math.floor(c.t / 86400) === Math.floor(Date.now() / 86400000));
      const days = new Set(cards.map((c) => Math.floor(c.t / 86400))).size;
      onNodesMap?.(`LoRa nodes heard · ${band} MHz · ${days} ${days === 1 ? 'day' : 'days'} in the gallery`, merged(), rx,
                   today ? async () => ((await one(today).catch(() => false)) ? merged() : null) : null);
    } catch { setNote('Could not reach the server.'); }
  };
  const isImage = (it: GalleryItem) => /\.(png|jpg)$/i.test(it.f);   // ★ V12 balloons: SSDV pictures are JPEGs
  const show = async (it: GalleryItem) => {
    setOpen(it); setText(''); setTextOk(false);
    if (isImage(it)) return;
    try {
      const r = await fetch(fileUrl(it), { cache: 'no-store' } as any);
      setText(r.ok ? await r.text() : 'This item is no longer on the server (the gallery is full and keeps the newest).');
      setTextOk(r.ok);
    } catch { setText('Could not reach the server.'); }
  };
  // ★ Whether to draw TRANSLATE for the item now open — worked out once its text is here.
  useEffect(() => {
    setTrPlan(null); setTrText(null); setShowTr(false); setTrBusy(false); setTrNote('');
    if (!open || !textOk || !text || isImage(open)) return;
    let dead = false;
    void planGalleryTranslate(text, open.k, open.title, translateTo).then((pl) => { if (!dead && pl) setTrPlan(pl); }).catch(() => {});
    return () => { dead = true; };
  }, [open, text, textOk, translateTo]);
  const onTranslate = async () => {
    if (!trPlan || trBusy) return;
    if (trText != null) { setShowTr(v => !v); return; }
    setTrBusy(true); setTrNote('');
    const r = await runGalleryTranslate(trPlan);
    setTrBusy(false);
    if (!r.ok) { setTrNote(r.note); return; }
    setTrText(r.text); setShowTr(true);
    setTrNote(r.note);
  };
  const close = () => { setOpen(null); onClose(); };

  return (
    <Modal visible={visible} animationType="fade" transparent onRequestClose={() => (open ? setOpen(null) : close())}>
      <View style={s.scrim}>
        <SafeAreaView style={s.panel} edges={['top', 'bottom']}>
          <View style={s.head}>
            <Text style={s.title}>GALLERY</Text>
            <View style={{ flexDirection: 'row' }}>
              {open && trPlan && (
                <TouchableOpacity onPress={() => { void onTranslate(); }} style={[s.key, showTr && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}
                                  accessibilityRole="button" accessibilityState={{ busy: trBusy, selected: showTr }}
                                  accessibilityLabel={showTr ? 'Show the original' : `Translate from ${langName(trPlan.source)}`}>
                  <Text style={[s.keyText, showTr && { color: pt.ink.selected }]}>{trBusy ? 'TRANSLATING…' : showTr ? 'ORIGINAL' : 'TRANSLATE'}</Text>
                </TouchableOpacity>
              )}
              <TouchableOpacity onPress={open ? () => setOpen(null) : close} style={s.key} accessibilityRole="button">
                <Text style={s.keyText}>{open ? '‹ BACK' : 'CLOSE'}</Text>
              </TouchableOpacity>
            </View>
          </View>
          {open && (
            <View style={{ flex: 1 }}>
              <Text style={s.meta}>
                {`${open.title} · ${galleryFreq(open.hz)}${open.m ? ` ${open.m.toUpperCase()}` : ''} · ${galleryWhen(open.t)} · `
                 + galleryOriginWords(open)
                 + (open.partial ? ' · PARTIAL' : '')}
              </Text>
              {isTlmPass(open) && tlmPass ? (
                <ScrollView style={s.textBox} contentContainerStyle={scrollLane}>
                  <TlmCharts pass={tlmPass} live={false} across={tlmAcross}
                             ink={{ text: pt.ink.text, muted: pt.ink.textDim, line: pt.gold.readout, grid: pt.ink.divider }} />
                </ScrollView>
              ) : isImage(open) ? (
                <ScrollView style={{ flex: 1 }} contentContainerStyle={[scrollLane, { alignItems: 'center' }]} maximumZoomScale={4}>
                  <Image source={{ uri: fileUrl(open) }} style={s.image} resizeMode="contain" />
                </ScrollView>
              ) : (
                <>
                  {(showTr || !!trNote) && (
                    <Text style={s.trNote}>
                      {(showTr && trPlan ? galleryTranslateBanner(trPlan) : '')
                       + (showTr && trNote ? ' · ' : '') + trNote}
                    </Text>
                  )}
                  <ScrollView style={s.textBox} contentContainerStyle={scrollLane}>
                    <Text selectable style={s.mono}>{(showTr && trText != null ? trText : text) || 'Loading…'}</Text>
                  </ScrollView>
                </>
              )}
              <TouchableOpacity onPress={() => { void Linking.openURL(fileUrl(open) + (fileUrl(open).includes('?') ? '&' : '?') + 'download=1'); }}
                                style={[s.key, { alignSelf: 'flex-start', marginTop: 8 }]} accessibilityRole="button">
                <Text style={s.keyText}>OPEN / SAVE</Text>
              </TouchableOpacity>
            </View>
          )}
          {/* ★ The list stays mounted under an open item (display: none), so BACK lands on the same page and scroll. */}
            <View style={open ? { display: 'none' } : { flex: 1 }}>
              <View style={s.filters}>
                {KINDS.map((k) => (
                  <TouchableOpacity key={k.id || 'all'} onPress={() => { setKind(k.id); setPage(1); }}
                                    style={[s.key, kind === k.id && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                    {/* ★ V12: the decoder's category glyph before its name (a ship for AIS / DSC / NAVTEX…); ALL has none */}
                    <ModeLabel id={k.id} iconSize={12} color={kind === k.id ? pt.ink.selected : pt.ink.key}
                               style={[s.keyText, kind === k.id && { color: pt.ink.selected }]}>{k.label}</ModeLabel>
                  </TouchableOpacity>
                ))}
              </View>
              {!!radios && (
                <View style={s.filters} accessibilityRole="toolbar" accessibilityLabel="Filter by radio">
                  {[{ id: '', name: 'ALL RADIOS' } as LogRadio, ...radios].map((r) => (
                    <TouchableOpacity key={r.id || 'allradios'} onPress={() => { setRadio(r.id); setPage(1); }}
                                      accessibilityRole="button" accessibilityState={{ selected: radio === r.id }}
                                      accessibilityLabel={r.id ? `Only what ${r.name} heard` : 'Every radio on this server'}
                                      style={[s.key, radio === r.id && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                      <Text style={[s.keyText, radio === r.id && { color: pt.ink.selected }]}>{r.id ? radioChipLabel(r) : r.name}</Text>
                    </TouchableOpacity>
                  ))}
                </View>
              )}
              {rangeOk && (
                <View style={s.range} accessibilityRole="toolbar" accessibilityLabel="Show what was kept in the last…">
                  {GALLERY_RANGES.map((x, i) => (
                    <TouchableOpacity key={x.id} onPress={() => pickRange(x.id)}
                                      accessibilityRole="button" accessibilityState={{ selected: range === x.id }} accessibilityLabel={x.words}
                                      style={[s.key, s.rangeKey, i === 0 && s.rangeFirst, i === GALLERY_RANGES.length - 1 && s.rangeLast,
                                              range === x.id && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                      <Text style={[s.keyText, range === x.id && { color: pt.ink.selected }]}>{x.label}</Text>
                    </TouchableOpacity>
                  ))}
                </View>
              )}
              {items == null ? <ActivityIndicator style={{ marginTop: 24 }} /> : (
                <FlatList ref={listRef} data={items} keyExtractor={(it) => it.f} contentContainerStyle={scrollLane}
                  ListFooterComponent={pages > 1 ? (
                    <View style={s.pager} accessibilityRole="toolbar">
                      <TouchableOpacity onPress={() => setPage(galleryClampPage(page - 1, pages))} disabled={page <= 1}
                                        style={[s.key, page <= 1 && s.off]} accessibilityRole="button" accessibilityLabel="Newer page">
                        <Text style={s.keyText}>{'\u2039 Newer'}</Text>
                      </TouchableOpacity>
                      {galleryPagerSlots(page, pages).map((sl, i) => sl === 'gap'
                        ? <Text key={`g${i}`} style={s.gap}>{'\u2026'}</Text>
                        : (
                          <TouchableOpacity key={sl} onPress={() => setPage(sl)} accessibilityRole="button" accessibilityLabel={`Page ${sl}`}
                                            accessibilityState={{ selected: sl === page }}
                                            style={[s.key, sl === page && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                            <Text style={[s.keyText, sl === page && { color: pt.ink.selected }]}>{String(sl)}</Text>
                          </TouchableOpacity>
                        ))}
                      <TouchableOpacity onPress={() => setPage(galleryClampPage(page + 1, pages))} disabled={page >= pages}
                                        style={[s.key, page >= pages && s.off]} accessibilityRole="button" accessibilityLabel="Older page">
                        <Text style={s.keyText}>{'Older \u203a'}</Text>
                      </TouchableOpacity>
                      <Text style={s.pageLab}>{galleryPageLabel(page, pages)}</Text>
                    </View>
                  ) : null}
                  ListHeaderComponent={(navSum && navSum.stations.length > 0 && onNavtexMap) || (dscSum && onShipMap) || rankC || (digiC && onDigiMap) ? (<>
                  {!!digiC && !!onDigiMap && (
                    <TouchableOpacity onPress={() => onDigiMap('all')} accessibilityRole="button"
                                      accessibilityLabel={`${digiC.title}. Show them on the digi modes map`} style={s.row}>
                      <Text style={s.freq}>SPOTS</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{digiC.title}</Text>
                        <Text style={s.dim}>{digiC.src}</Text>
                      </View>
                      <View style={[s.key, s.mapKey]}><Text style={s.keyText}>MAP</Text></View>
                    </TouchableOpacity>
                  )}
                  {!!rankC && (
                    <View accessible accessibilityLabel={`${rankC.title}. ${rankC.src}`} style={s.row}>
                      <Text style={s.freq}>RANK</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{rankC.title}</Text>
                        <Text style={s.dim}>{rankC.src}</Text>
                      </View>
                    </View>
                  )}
                  {!!dscSum && !!onShipMap && (
                    <TouchableOpacity onPress={() => onShipMap('dsc', '')} accessibilityRole="button"
                                      accessibilityLabel={`${dscSum}. Show them on the shipping map`} style={s.row}>
                      <Text style={s.freq}>DSC</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{dscSum}</Text>
                        <Text style={s.dim}>{DSC_GAL_WORDS}</Text>
                      </View>
                      <View style={[s.key, s.mapKey]}><Text style={s.keyText}>MAP</Text></View>
                    </TouchableOpacity>
                  )}
                  {!!navSum && navSum.stations.length > 0 && !!onNavtexMap && (
                    <TouchableOpacity onPress={onNavtexMap} accessibilityRole="button"
                                      accessibilityLabel={`NAVTEX stations received: ${navSum.stations.length} stations, ${navSum.messages} messages. Show them on the map`}
                                      style={s.row}>
                      <Text style={s.freq}>NAVTEX</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{`SHIPPING MAP · NAVTEX stations received: ${navSum.stations.length} stations · ${navSum.messages} messages`}</Text>
                        <Text style={s.dim}>Every NAVTEX message in this gallery, by the station that sent it</Text>
                      </View>
                      <View style={[s.key, s.mapKey]}><Text style={s.keyText}>MAP</Text></View>
                    </TouchableOpacity>
                  )}
                  </>) : null}
                  ListEmptyComponent={<Text style={s.dim}>{galleryRangePhrase(rangeShown)
                    ? `Nothing ${kind ? 'from this decoder ' : ''}kept ${galleryRangePhrase(rangeShown)} — try a longer range.`
                    : kind ? 'Nothing from this decoder yet.' : 'Nothing decoded yet — background decoding fills this while nobody listens.'}</Text>}
                  renderItem={({ item: it }) => (
                    <TouchableOpacity onPress={() => { void show(it); }} accessibilityRole="button"
                                      style={[s.row, (it.lv ?? 0) >= 2 && { borderLeftWidth: 3, borderLeftColor: (it.lv ?? 0) >= 3 ? '#FF4B4B' : '#FF8A3D', paddingLeft: 6 },
                                              // ★ beta88 a routine / safety DSC call with a position: its map pin's colour (DSC_PIN_COLOURS)
                                              it.k === 'dsc' && typeof it.plat === 'number' && (it.lv === 0 || it.lv === 1) && { borderLeftWidth: 3, borderLeftColor: it.lv === 1 ? '#E8C547' : '#9fb3c8', paddingLeft: 6 }]}>
                      <Text style={s.freq}>{galleryFreq(it.hz)}</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{it.title || it.k.toUpperCase()}
                          <Text style={s.tag}>{`  ${galleryOriginTag(it)}${it.partial ? ' · PARTIAL' : ''}`}</Text>
                        </Text>
                        {/* ★ only the parts an item has (an ISM day has no demodulator) — the web client's rule */}
                        <Text style={s.dim}>{[galleryWhen(it.t), (it.m || '').toUpperCase(), it.d, it.p, heardByText(it.ri, radios) || it.r, size(it.bytes)].filter((x) => !!x).join(' · ')}</Text>
                      </View>
                      {!!onShipMap && it.k === 'dsc' && typeof it.plat === 'number' && typeof it.plon === 'number' && (
                        <TouchableOpacity style={[s.key, s.mapKey]} accessibilityRole="button" accessibilityLabel={`Show this call's position on the shipping map: ${it.title}`}
                                          onPress={() => onShipMap('dsc', `d:g:${it.f}`)}>
                          <Text style={s.keyText}>MAP</Text>
                        </TouchableOpacity>
                      )}
                      {!!onShipMap && it.k === 'ais' && (
                        <TouchableOpacity style={[s.key, s.mapKey]} accessibilityRole="button" accessibilityLabel="Show the ships on the shipping map"
                                          onPress={() => onShipMap('ships', '')}>
                          <Text style={s.keyText}>MAP</Text>
                        </TouchableOpacity>
                      )}
                      {!!onDigiMap && DIGI_GAL_KINDS.includes(it.k) && (
                        <TouchableOpacity style={[s.key, s.mapKey]} accessibilityRole="button" accessibilityLabel={`Show the stations heard on the digi modes map: ${it.title}`}
                                          onPress={() => onDigiMap(digiGalItemWindow(it.t, Date.now() / 1000))}>
                          <Text style={s.keyText}>MAP</Text>
                        </TouchableOpacity>
                      )}
                      {!!onNodesMap && ismIsNodeCard(it) && (
                        <TouchableOpacity style={[s.key, s.mapKey]} accessibilityRole="button" accessibilityLabel={`Show the nodes heard on the map: ${it.title}`}
                                          onPress={() => { void nodesMap(it); }}>
                          <Text style={s.keyText}>MAP</Text>
                        </TouchableOpacity>
                      )}
                    </TouchableOpacity>
                  )} />
              )}
              {!!note && <Text style={s.dim}>{note}</Text>}
            </View>
        </SafeAreaView>
      </View>
    </Modal>
  );
}

function makeStyles(pt: PopupTokens) {
  return StyleSheet.create({
    scrim: { flex: 1, backgroundColor: 'rgba(0,0,0,0.6)', alignItems: 'center', justifyContent: 'center' },
    panel: { width: '94%', maxWidth: 640, height: '86%', backgroundColor: 'rgba(8,6,2,0.98)', borderRadius: 12,
             borderWidth: 1, borderColor: pt.ink.border, padding: 14 },
    head: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 },
    title: { color: pt.ink.section, fontSize: 15, letterSpacing: 2, fontWeight: '600' },
    key: { borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginRight: 6, marginBottom: 6 },
    keyText: { color: pt.ink.key, fontSize: 13, letterSpacing: 1 },
    filters: { flexDirection: 'row', flexWrap: 'wrap', marginBottom: 6 },
    // ★ beta92 the time range: the maps' segmented keys (one joined row)
    range: { flexDirection: 'row', marginBottom: 6 },
    rangeKey: { borderRadius: 0, marginRight: 0, marginLeft: -1, marginBottom: 0, paddingHorizontal: 8 },
    rangeFirst: { borderTopLeftRadius: 6, borderBottomLeftRadius: 6, marginLeft: 0 },
    rangeLast: { borderTopRightRadius: 6, borderBottomRightRadius: 6 },
    row: { flexDirection: 'row', paddingVertical: 8, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.ink.divider },
    freq: { color: pt.gold.readout, width: 92, fontVariant: ['tabular-nums'] },
    name: { color: pt.ink.text, fontSize: 14 },
    tag: { color: pt.ink.textFaint, fontSize: 10, letterSpacing: 1 },
    dim: { color: pt.ink.textDim, fontSize: 12, marginTop: 4 },
    meta: { color: pt.ink.textDim, fontSize: 12, marginBottom: 8 },
    trNote: { color: pt.ink.selected, fontSize: 11, letterSpacing: 1, marginBottom: 6 },
    textBox: { flex: 1, borderWidth: 1, borderColor: pt.ink.border, borderRadius: 8, padding: 10, backgroundColor: 'rgba(0,0,0,0.25)' },
    mono: { color: pt.ink.text, fontFamily: Platform.select({ ios: 'Menlo', default: 'monospace' }), fontSize: 13, lineHeight: 19 },
    image: { width: '100%', aspectRatio: 1.2, backgroundColor: '#fff' },
    pager: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', marginTop: 10 },
    gap: { color: pt.ink.textDim, marginRight: 6, marginBottom: 6 },
    pageLab: { color: pt.ink.textDim, fontSize: 12, marginLeft: 'auto', marginBottom: 6 },
    off: { opacity: 0.35 },
    mapKey: { alignSelf: 'center', marginLeft: 8, marginBottom: 0 },
  });
}
