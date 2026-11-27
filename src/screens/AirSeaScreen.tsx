/**
 * ★★★ V12 beta96 THE STANDALONE AIR & SEA MAP — the app's screen (docs/v12/ADSB-FEED.md §The standalone AIR & SEA map; the
 * web's openAirSeaViewer).
 *
 * The AIR & SEA map with NO RADIO: nothing is tuned, no spectrum socket, no listener slot — only this server's HTTP reads, so
 * it works with every radio busy or none (a server that is mainly feeds: Stuart's Lenovo with a PiAware and an UberSDR's
 * HFDL). It is LIVE: the aircraft as deltas every 3 s (aircraft.json?d=<cursor> — services/airSeaViewer.ts), the ships,
 * DSC and NAVTEX every 30 s, the remembered sondes and VOR stations every 2 minutes. The SHOW chips and the normal time
 * windows only — no TUNE, no "this session", no other radios. CLOSE MAP goes back.
 *
 * ★★ THE OWNER'S LIMITS (Stuart: "so people on limited or sketchy connections can host it"): a ticket on open
 *    (vibe_airsea_viewers.h). A full map says so politely ("The map is full (20/20) — try again shortly") and opens nothing;
 *    a time limit counts down in the status line, warns in the last minute, and at the end the map stops and says why.
 *
 * Reached from the server's landing (SDRScreen's radio list: AIR & SEA MAP) and from anything else through
 * openAirSeaScreen(navigation, …) — the menu's AIR & SEA entry beside GALLERY.
 */
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import MapOverlay from '../components/MapOverlay';
import PanelBoundary from '../components/PanelBoundary';
import { NavRing } from '../components/PanelNav';
import { useKeyList } from '../components/ServerKey';
import { withReadAuth } from '../services/vibeAuth';
import { galleryReader } from '../components/GalleryOverlay';
import { AirSeaViewer, airSeaOffer, type AirSeaTab } from '../services/airSeaViewer';
import { composeAirSeaStandalone, newMsgFeed } from '../services/airSeaStandalone';
import { avLayersOf, sharedSanitise, type AvLayers, type SharedAircraft } from '../services/aviationMap';
import { shipLayersOf, storeSanitise, dscPinsFromGallery, type ShipLayers, type StoredShips, type DscPin } from '../services/shippingMap';
import { navSanitise, navSince, navRangeOf, type NavReport } from '../services/navtexMap';
import { navWarnSanitise, navMetSanitise, type NavWarnReport, type NavMetReport } from '../services/navtexWarn';
import { storedLoader, type GalPos, type StoredKind } from '../services/storedLayers';
import type { LiveTab, AircraftSort } from '../services/liveMaps';
import type { MapCardsIn } from '../components/MapChrome';
import { rateRows } from '../utils/mapChrome';
import { statsSeen, statsSections, type StatsStore } from '../utils/mapStats';

export type AirSeaParams = {
  /** the SERVER's address (the front door, or the one-radio server itself) */
  baseUrl: string;
  name?: string;
  /** the read auth (a PIN-gated server's ?…), as the SDR screen holds it */
  authSuffix?: string;
  /** the tab to open on (airSeaOffer's most populated layer) */
  tab?: AirSeaTab;
};

/** ★★ THE NAV TARGET — one way in for the landing, the menu and anything else. */
export function openAirSeaScreen(navigation: { navigate: (name: string, params: AirSeaParams) => void }, p: AirSeaParams) {
  navigation.navigate('AirSea', p);
}

export default function AirSeaScreen({ navigation, route }: { navigation: any; route: { params: AirSeaParams } }) {
  const insets = useSafeAreaInsets();
  const base = (route.params?.baseUrl ?? '').replace(/\/+$/, '');
  const auth = route.params?.authSuffix ?? '';
  const get = useCallback(async (path: string): Promise<any | null> => {
    try { const r = await fetch(withReadAuth(`${base}${path}`, auth), { cache: 'no-store' } as any); return r.ok ? await r.json() : null; }
    catch { return null; }
  }, [base, auth]);

  // ── the viewer's ticket: refused (full / time used) → a polite card, nothing opened ──
  const viewer = useMemo(() => new AirSeaViewer(get), [get]);
  const [phase, setPhase] = useState<'joining' | 'open' | 'refused'>('joining');
  const [refusal, setRefusal] = useState('');
  const [openTab, setOpenTab] = useState<LiveTab | null>(route.params?.tab ?? null);
  useEffect(() => {
    let live = true;
    void (async () => {
      const ok = await viewer.join();
      if (!live) return;
      if (!ok) { setRefusal(viewer.status().refusal); setPhase('refused'); return; }
      setPhase('open');
      if (!route.params?.tab) { const o = await airSeaOffer(get).catch(() => null); if (live && o?.show) setOpenTab(o.tab); }
    })();
    return () => { live = false; void viewer.leave(); };   // ★ the ticket ends with the screen (else it lapses in 30 s)
  }, [viewer]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ── what the map draws ──
  const [shared, setShared] = useState<SharedAircraft | null>(null);
  const [shipStore, setShipStore] = useState<StoredShips | null>(null);
  const [dscGal, setDscGal] = useState<DscPin[]>([]);
  const [navReport, setNavReport] = useState<NavReport | null>(null);
  const [navWarnRep, setNavWarnRep] = useState<NavWarnReport | null>(null);
  const [navMetRep, setNavMetRep] = useState<NavMetReport | null>(null);
  const [navRange, setNavRange] = useState('all');
  const [sdStored, setSdStored] = useState<GalPos[]>([]);
  const [vmStored, setVmStored] = useState<GalPos[]>([]);
  const [rx, setRx] = useState<{ lat: number; lon: number } | null>(null);
  const [avLayers, setAvLayers] = useState<AvLayers>(avLayersOf(null));
  const [shipLayers, setShipLayers] = useState<ShipLayers>(shipLayersOf(null));
  const [view, setView] = useState<[number, number, number, number] | null>(null);
  const [picked, setPicked] = useState<LiveTab | null>(null);
  const [sort, setSort] = useState<AircraftSort>('nearest');
  const [msgKey, setMsgKey] = useState('');
  const [tick, setTick] = useState(0);
  const msgs = useRef(newMsgFeed());
  // ★ the device's own choices, the same keys as a radio's map (one viewer, one set of chips)
  useEffect(() => {
    AsyncStorage.multiGet(['lsv_av_map_layers', 'lsv_ship_map_layers', 'lsv_nav_map_layers', 'lsv_nav_map_range', 'lsv_airsea_sort']).then((kv) => {
      setAvLayers(avLayersOf(kv[0][1])); setShipLayers(shipLayersOf(kv[1][1], kv[2][1])); setNavRange(navRangeOf(kv[3][1]));
      if (kv[4][1] === 'latest' || kv[4][1] === 'nearest') setSort(kv[4][1]);
    }).catch(() => {});
  }, []);
  // ★ where the receiver is (the server's /location) — the rings, NEAREST, the sea areas near it
  useEffect(() => {
    let live = true;
    void get('/location').then((j) => { if (live && j && typeof j.lat === 'number' && typeof j.lon === 'number' && !(j.lat === 0 && j.lon === 0)) setRx({ lat: j.lat, lon: j.lon }); });
    return () => { live = false; };
  }, [get]);
  // ── the aircraft: deltas every 3 s with the ticket (the clock ticks the status line once a second) ──
  useEffect(() => {
    if (phase !== 'open') return;
    let live = true, t: ReturnType<typeof setTimeout> | null = null;
    const go = async () => {
      const j = await viewer.poll();
      if (!live) return;
      if (j) { const s = sharedSanitise(j); if (s) setShared(s); }
      setTick((n) => n + 1);
      if (viewer.state === 'live') t = setTimeout(() => { void go(); }, 3000);
    };
    void go();
    const clock = setInterval(() => {
      if (viewer.left() === 0 && viewer.state === 'live') viewer.end(0);
      if (viewer.left() >= 0 || viewer.state === 'ended') setTick((n) => n + 1);
    }, 1000);
    return () => { live = false; if (t) clearTimeout(t); clearInterval(clock); };
  }, [phase, viewer]);
  // ── ships, DSC, NAVTEX: every 30 s (2 s while the server builds its warnings index), stopped at the time limit's end ──
  useEffect(() => {
    if (phase !== 'open') return;
    let live = true, t: ReturnType<typeof setTimeout> | null = null;
    const since = navSince(navRange);
    const go = async () => {
      if (viewer.state !== 'live') return;
      const [w, m, r, sh, dg] = await Promise.all([
        get(`/vibeserver/gallery/navtex-warnings.json?since=${since}`), get(`/vibeserver/gallery/navtex-met.json?since=${since}`),
        get(`/vibeserver/gallery/navtex-stations.json?since=${since}`), get(`/vibeserver/gallery/ais-ships.json?since=${since}&limit=1000&tracks=1`),
        get('/vibeserver/gallery.json?kind=dsc&limit=300'),
      ]);
      if (!live) return;
      const ws = w ? navWarnSanitise(w) : null;
      if (ws) setNavWarnRep(ws);
      const ms = m ? navMetSanitise(m) : null; if (ms) setNavMetRep(ms);
      const rs = r ? navSanitise(r) : null; if (rs) setNavReport(rs);
      const ss = sh ? storeSanitise(sh) : null; if (ss) setShipStore(ss);
      if (dg) setDscGal(dscPinsFromGallery(dg));
      t = setTimeout(() => { void go(); }, ws && ws.pending > 0 ? 2000 : 30_000);
    };
    void go();
    return () => { live = false; if (t) clearTimeout(t); };
  }, [phase, navRange, get, viewer]);
  // ── the remembered sondes and VOR stations (the gallery's day cards): on open and every 2 minutes ──
  const storedLoads = useMemo(() => {
    const rd = galleryReader(base, auth);
    const list = async (kind: StoredKind) => { const lj = await rd(`/vibeserver/gallery.json?kind=${kind}&limit=60`); if (!lj) return null;
                                               try { const it = JSON.parse(lj)?.items; return Array.isArray(it) ? it : null; } catch { return null; } };
    const text = (f: string) => rd(`/vibeserver/gallery/file/${encodeURIComponent(f)}`);
    return { sd: storedLoader('sondes', { list, text }), vm: storedLoader('vormap', { list, text }) };
  }, [base, auth]);
  useEffect(() => {
    if (phase !== 'open') return;
    let live = true;
    const go = async () => {
      if (viewer.state !== 'live') return;
      const now = Math.floor(Date.now() / 1000);
      const a = await storedLoads.sd(now); if (live && a) setSdStored(a);
      const b = await storedLoads.vm(now); if (live && b) setVmStored(b);
    };
    void go();
    const t = setInterval(() => { void go(); }, 120_000);
    return () => { live = false; clearInterval(t); };
  }, [phase, storedLoads, viewer]);

  const cardText = useCallback(async (f: string) => {
    try {
      const r = await fetch(withReadAuth(`${base}/vibeserver/gallery/file/${encodeURIComponent(f)}`, auth), { cache: 'no-store' } as any);
      return r.ok ? { ok: true, text: await r.text() } : { ok: false, text: 'This message is no longer in the gallery (it keeps the newest).' };
    } catch { return { ok: false, text: 'Could not reach the server.' }; }
  }, [base, auth]);
  const vst = viewer.status();
  const trk = useMemo(() => composeAirSeaStandalone({
    shared, shipStore, dscGal, navReport, navWarnRep, navMetRep, navRange, sdStored, vmStored, avLayers, shipLayers, rx, view,
    picked, opened: openTab, sort, msgKey, clock: vst.clock, nowMs: Date.now(),
  }, {
    setPicked, setMsgKey,
    setSort: (s) => { setSort(s); AsyncStorage.setItem('lsv_airsea_sort', s).catch(() => {}); },
    setAvLayers: (f) => setAvLayers((l) => { const n = f(l); AsyncStorage.setItem('lsv_av_map_layers', JSON.stringify(n)).catch(() => {}); return n; }),
    setShipLayers: (f) => setShipLayers((l) => { const n = f(l); AsyncStorage.setItem('lsv_ship_map_layers', JSON.stringify(n)).catch(() => {}); return n; }),
    setNavRange: (r) => { setNavRange(r); AsyncStorage.setItem('lsv_nav_map_range', r).catch(() => {}); },
    onView: (b) => setView((cur) => (cur && cur.every((v, i) => v === b[i]) ? cur : b)),
    cardText,
  }, msgs.current),
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [shared, shipStore, dscGal, navReport, navWarnRep, navMetRep, navRange, sdStored, vmStored, avLayers, shipLayers, rx, view, picked, openTab, sort, msgKey, vst.clock, tick, cardText]);

  const close = useCallback(() => { void viewer.leave(); navigation.goBack(); }, [viewer, navigation]);
  /* ★★★ beta97 THE MAP CARDS on the viewer too (Stuart, build 586 on the Lenovo: SERVERS and STATS were missing): SERVERS
   *  (LEAVE — back to where the map was opened), STATS (every source's aircraft, per mode — mapStats.ts), RATE (each feed's
   *  own messages a second, the busiest first). No TUNE (nothing here moves a radio), no gain, no waterfall. */
  const stats = useRef<StatsStore>(new Map());
  const statsOpened = useRef(Date.now());
  const sections = useMemo(() => {
    const now = Date.now();
    const obs: Array<{ mode: string; key: string; lat: number; lon: number; name: string }> = [];
    for (const pl of shared?.aircraft ?? []) if (pl.lat != null && pl.lon != null) for (const m of pl.src) obs.push({ mode: m, key: `a:${pl.key}`, lat: pl.lat, lon: pl.lon, name: pl.flight || pl.reg || pl.icao || '' });
    statsSeen(stats.current, obs, now);
    return statsSections(stats.current, rx, now, statsOpened.current);
  }, [shared, rx]);
  const cards = useMemo<MapCardsIn>(() => ({
    servers: { rows: [{ id: 'back', label: 'Leave \u2014 back', sub: route.params?.name ?? base.replace(/^https?:\/\//, '') }], onPick: () => close() },
    gain: { kind: 'none', more: false }, gainH: { onAgc: () => {}, onSet: () => {} },
    stats: sections.length ? { sections, onPick: () => {} } : null,
    rate: rateRows({ feeds: shared?.feeds ?? [] }),
    health: null, time: null, wf: null,
  }), [sections, shared?.feeds, close, route.params?.name, base]);

  // ── refused: one sentence and the way back (pad: the one key, highlighted) ──
  // ★ the pad: the card's one key (Enter or Back); while the map is open it is MapOverlay's own (useOverlay — Back closes it)
  const [hl] = useKeyList(phase !== 'open' ? 1 : 0, () => close(), () => close());
  if (phase !== 'open') {
    return (
      <View style={[st.wrap, { paddingTop: insets.top + 40 }]}>
        <Text style={st.title}>AIR &amp; SEA MAP</Text>
        <Text style={st.sub}>{route.params?.name ?? ''}</Text>
        <Text style={st.msg}>{phase === 'joining' ? 'Opening the map…' : refusal}</Text>
        <Pressable onPress={close} accessibilityRole="button" accessibilityLabel="Back" style={st.btn}>
          <Text style={st.btnTx}>BACK</Text>
          <NavRing on={hl === 0} color="#FFB833" radius={8} />
        </Pressable>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, backgroundColor: '#0A0A12' }}>
      <PanelBoundary name="Air & sea map" onClose={close} resetKey={phase}>
        <MapOverlay
          visible
          kind="airsea"
          baseUrl={base}
          radioUrl={base}
          sessionUuid=""
          onClose={close}
          rxLat={rx?.lat ?? null}
          rxLon={rx?.lon ?? null}
          trk={trk as any}
          cards={cards}
          adsbFeedWarn={vst.banner || shared?.feedWarn || ''}
          safeInsets={insets}
        />
      </PanelBoundary>
    </View>
  );
}

const st = StyleSheet.create({
  wrap: { flex: 1, backgroundColor: '#0A0A12', paddingHorizontal: 24, alignItems: 'center' },
  title: { color: '#FFB833', fontFamily: 'Courier', fontSize: 20, fontWeight: '700', letterSpacing: 1 },
  sub: { color: '#c9d1d9', fontSize: 14, marginTop: 4 },
  msg: { color: '#e6edf3', fontSize: 16, marginTop: 28, textAlign: 'center', lineHeight: 22 },
  btn: { marginTop: 28, paddingHorizontal: 28, paddingVertical: 12, borderRadius: 8, borderWidth: 1, borderColor: '#FFB833' },
  btnTx: { color: '#FFB833', fontFamily: 'Courier', fontSize: 16, fontWeight: '700' },
});
