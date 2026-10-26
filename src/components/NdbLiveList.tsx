/**
 * NdbLiveList — the V12 NDB map's live list, beacon card and dialogs (docs/v12/NDB-SKIMMER.md; the web's NdbView in
 * web/client/src/ndb.ts). The map itself is MapOverlay's page with the shared beacon layer; this is everything over it.
 *
 * ★★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW, for AdsbLiveList's reason: a game pad and the keyboard reach the app
 *     through PanelNav, never through the page. Rows, chips, keys, the card's ✕ and every dialog row walk with the
 *     D-pad; Enter presses; Back passes to the map's overlay (EXIT NDB) — except while a dialog is open, which takes
 *     Back first (its own overlay, on top).
 * ★★ THE LIST (bottom-left, folds to a pill — remembered per device): its header holds the tiles (CARRIERS · IDENTIFIED ·
 *    FURTHEST · COVERAGE) and the keys (RINGS · UNHEARD · FIT · HEARD LOG · FIND · STATS). Rows LOWEST FREQUENCY FIRST
 *    (the CW skimmer's order): kHz · ident (✓ / ≈ / plain / "?") · station · NM + bearing · the 3-bar meter, and the
 *    readings streaming beneath, each word in its confidence's colour (the health pill's four). A row = select the
 *    beacon on the map and open its card.
 * ★★ THE CARD (top-left, under the page's top bar): ident, station, kHz, tone, WPM, NM and bearing, SNR now and its
 *    sparkline, the last readings, the others published on the frequency, a harmonic.
 * ★ The map's colours (MapOverlay `ink`), not the app theme's — it is part of the map, so it looks the same on every
 *   faceplate (default / silver / black) with transparency on or off. The decoder font, never the display font.
 * ★ Every rule (what a cell says, the bars, NM, the order, the tiles) is src/services/ndb.ts — the web's, mirrored and
 *   held to it by scripts/test_ndb_app.ts.
 */
import React, { useEffect, useMemo, useState } from 'react';
import { ScrollView, Share, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, useOverlay, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import TvTextInput from './TvTextInput';
import { scrollLane } from '../constants/popupTokens';
import { EDGE, MAP_TOPBAR_H, adsbListWidth, parseListOpen } from '../services/adsbList';
import {
  NDB_BAR_ON, NDB_NO_POS, NDB_Q_COLOURS, flag, khz, ndbBars, ndbByHourOfDay, ndbEmptyText, ndbFoundKey, ndbIdentCell,
  ndbOnMap, ndbRowLive, ndbRows, ndbStation, ndbStatsCsv, ndbStatText, ndbTiles, nmText, utc,
  type NdbBeacon, type NdbCopy, type NdbFound, type NdbMapPrefs, type NdbReport, type NdbStats, type NdbTracks,
} from '../services/ndb';

/** What SDRScreen hands the map for NDB. `tracks` is the readings' memory (NdbTracks); `seq` changes when it does. */
export interface NdbOverlay {
  report: NdbReport | null;
  tracks: NdbTracks;
  seq: number;
  /** The published beacons nearest first (an empty Find) — the map's faint unheard markers. */
  found: NdbFound[];
  /** The last Find answer (for the FIND dialog) and the last stats. */
  findResult: { q: string; pos?: boolean; list: NdbFound[] } | null;
  stats: NdbStats | null;
  onFind: (q: string) => void;
  onStats: () => void;
}

const OPEN_KEY = 'lsv_ndb_list_open';
const HEAD_H = 30;
const CHIPS_H = 30;
const ROW_H = 42;   // ★ two lines: the row and its readings

/** ★ How many rows: never more than half the map's height (the map is the screen), 2–6 (it scrolls). */
export function ndbListRowsFit(areaH: number): number {
  const mapH = Math.max(0, areaH - MAP_TOPBAR_H);
  return Math.max(2, Math.min(6, Math.floor((mapH * 0.5 - HEAD_H - CHIPS_H * 2) / ROW_H)));
}

type Dlg = '' | 'log' | 'find' | 'stats';

export default function NdbLiveList(p: {
  ndb: NdbOverlay; selected: number; onSelect: (id: number) => void;
  prefs: NdbMapPrefs; onPrefs: (p: Partial<NdbMapPrefs>) => void;
  onFootprint?: (w: number) => void;
  ink: { x: string; k: string; kt: string }; areaW: number; areaH: number;
  insets: { top: number; bottom: number; left: number; right: number };
}) {
  const [open, setOpen] = useState(true);
  useEffect(() => { AsyncStorage.getItem(OPEN_KEY).then((v) => setOpen(parseListOpen(v))).catch(() => {}); }, []);
  const setOpenKept = (v: boolean) => { setOpen(v); AsyncStorage.setItem(OPEN_KEY, v ? '1' : '0').catch(() => {}); };
  const [dlg, setDlg] = useState<Dlg>('');
  const r = p.ndb.report;
  const rows = useMemo(() => (r ? ndbRows(r) : []), [r]);
  const innerW = p.areaW - p.insets.left - p.insets.right;
  const innerH = p.areaH - p.insets.top - p.insets.bottom;
  // ★ a little wider than ADS-B's: the station name is the point of a row
  const width = Math.min(360, Math.max(adsbListWidth(innerW), Math.min(innerW - EDGE * 2, 300)));
  const fit = ndbListRowsFit(innerH);
  useEffect(() => { p.onFootprint?.(open ? width : 0); }, [open, width]);   // eslint-disable-line react-hooks/exhaustive-deps
  const { navCtx, scrollProps } = usePanelNav(!dlg, { flat: true });
  const s = useMemo(() => makeStyles(p.ink), [p.ink]);
  const nowMs = (r?.now || Date.now() / 1000) * 1000;
  const tiles = r ? ndbTiles(r) : null;
  const sel = p.selected ? r?.beacons.find((b) => b.id === p.selected) ?? null : null;

  return (
    <>
      {!dlg && (
        <NavCtx.Provider value={navCtx}>
          <View style={[s.wrap, { left: EDGE + p.insets.left, bottom: EDGE + p.insets.bottom }, open && { width }]}
                onTouchStart={noteTouchInteraction}>
            <NavRow>
              <Key s={s} style={[s.head, !open && s.pill]} onPress={() => setOpenKept(!open)}
                   a11y={open ? `Beacon list, ${r?.identified ?? 0} identified of ${r?.carriers ?? 0} carriers. Hide the list`
                              : `Show the beacon list, ${r?.identified ?? 0} identified`}>
                <Text style={s.headTxt}>{open ? 'BEACONS' : 'NDB'}</Text>
                <Text style={s.headCnt}>{`${r?.identified ?? 0} / ${r?.carriers ?? 0}`}</Text>
                {open && <Text style={s.headSub} numberOfLines={1}>{r ? ndbStatText(r) : 'taking the receiver to the beacon band…'}</Text>}
                {open && <View style={{ flex: 1 }} />}
                <Text style={s.chev}>{open ? '▾' : '▸'}</Text>
              </Key>
            </NavRow>
            {open && !!tiles && (
              <NavRow>
                <ScrollView horizontal showsHorizontalScrollIndicator={false} style={s.chipRow} contentContainerStyle={s.chipRowIn}>
                  {([['CARRIERS', tiles.carriers], ['IDENTIFIED', tiles.identified], ['FURTHEST', tiles.furthest], ['COVERAGE', tiles.coverage]] as const)
                    .map(([k, v]) => <Tile key={k} s={s} k={k} v={v} />)}
                </ScrollView>
              </NavRow>
            )}
            {open && (
              <NavRow>
                <ScrollView horizontal showsHorizontalScrollIndicator={false} style={s.chipRow} contentContainerStyle={s.chipRowIn}
                            keyboardShouldPersistTaps="handled">
                  <Chip s={s} label="RINGS" on={p.prefs.rings} a11y="Range rings in nautical miles" onPress={() => p.onPrefs({ rings: !p.prefs.rings })} />
                  <Chip s={s} label="UNHEARD" on={p.prefs.unheard} a11y="The published beacons not heard, faint"
                        onPress={() => { const v = !p.prefs.unheard; if (v) p.ndb.onFind(''); p.onPrefs({ unheard: v }); }} />
                  <Chip s={s} label="FIT" on={p.prefs.autofit} a11y="Fit the map to the beacons heard" onPress={() => p.onPrefs({ autofit: true })} />
                  <Chip s={s} label="HEARD LOG" on={false} a11y="Every beacon identified today" onPress={() => setDlg('log')} />
                  <Chip s={s} label="FIND NDB" on={false} a11y="Find a published beacon" onPress={() => { p.ndb.onFind(''); setDlg('find'); }} />
                  <Chip s={s} label="STATS" on={false} a11y="Reception statistics" onPress={() => { p.ndb.onStats(); setDlg('stats'); }} />
                </ScrollView>
              </NavRow>
            )}
            {open && r?.pos === false && <Text style={s.note} numberOfLines={2}>{NDB_NO_POS}</Text>}
            {open && (
              <ScrollView {...scrollProps} style={{ maxHeight: fit * ROW_H }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator
                          keyboardShouldPersistTaps="handled">
                {rows.length ? rows.map((b) => (
                  <NavRow key={b.id}>
                    <Row s={s} b={b} live={ndbRowLive(b, nowMs)} words={p.ndb.tracks.words(b.id)} sel={b.id === p.selected}
                         onPress={() => p.onSelect(b.id)} />
                  </NavRow>
                )) : <Text style={s.note} numberOfLines={4}>{ndbEmptyText(r)}</Text>}
              </ScrollView>
            )}
          </View>
          {!!sel && (
            <View style={[s.card, { left: EDGE + p.insets.left, top: p.insets.top + MAP_TOPBAR_H + EDGE + 36,
                                    maxWidth: Math.min(320, innerW - EDGE * 2 - 44) }]}>
              <Card s={s} b={sel} words={p.ndb.tracks.words(sel.id)} snr={p.ndb.tracks.snr(sel.id)} onClose={() => p.onSelect(0)} />
            </View>
          )}
        </NavCtx.Provider>
      )}
      {!!dlg && (
        <NdbDialog s={s} kind={dlg} ndb={p.ndb} insets={p.insets} onClose={() => setDlg('')}
                   onPick={(id) => { setDlg(''); p.onSelect(id); }} />
      )}
    </>
  );
}

type S = ReturnType<typeof makeStyles>;

function Key({ s, style, onPress, a11y, children }: { s: S; style: any; onPress: () => void; a11y: string; children: React.ReactNode }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={style} onPress={onPress} activeOpacity={0.75} accessibilityRole="button" accessibilityLabel={a11y}>
      {children}
      <NavRing on={focused} color={ring} radius={8} />
    </TouchableOpacity>
  );
}
function Chip({ s, label, on, onPress, a11y }: { s: S; label: string; on: boolean; onPress: () => void; a11y: string }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.chip, on && s.chipOn]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="button" accessibilityLabel={a11y} accessibilityState={{ selected: on }}>
      <Text style={[s.chipTxt, on && s.chipTxtOn]}>{label}</Text>
      <NavRing on={focused} color={ring} radius={7} />
    </TouchableOpacity>
  );
}
/** A tile — read, not pressed (the web's tiles are labels). */
function Tile({ s, k, v }: { s: S; k: string; v: string }) {
  return (
    <View style={s.tile} accessible accessibilityLabel={`${k.toLowerCase()} ${v}`}>
      <Text style={s.chipTxt} numberOfLines={1}><Text style={s.chipKey}>{k}</Text>{` ${v}`}</Text>
    </View>
  );
}

/** ★ The ident's look, by its class (the web's .conf / .near / .unl / .cand / .harm / .none). */
const identStyle = (cls: string, x: string) => (
  cls === 'conf' ? { color: '#8cff8c', fontWeight: '700' as const }
  : cls === 'near' ? { color: '#ffe066', fontWeight: '700' as const }
  : cls === 'unl' ? { color: '#ffd27a', fontWeight: '700' as const }
  : cls === 'cand' ? { color: `rgba(${x},0.6)`, fontStyle: 'italic' as const }
  : cls === 'harm' ? { color: `rgba(${x},0.5)`, textDecorationLine: 'line-through' as const }
  : { color: `rgba(${x},0.45)` });

function Bars({ n }: { n: number }) {
  return (
    <View style={sig.box} accessibilityLabel={n ? `signal ${n} of 3` : 'not heard in the last minute'}>
      {[6, 9, 12].map((h, i) => (
        <View key={i} style={[sig.bar, { height: h, backgroundColor: i < n ? NDB_BAR_ON[n] : 'rgba(255,184,51,0.22)' }]} />
      ))}
    </View>
  );
}
const sig = StyleSheet.create({
  box: { flexDirection: 'row', alignItems: 'flex-end', gap: 1.5, height: 12, marginLeft: 4 },
  bar: { width: 3, borderRadius: 1 },
});

function Words({ s, words, read }: { s: S; words: readonly NdbCopy[]; read: string }) {
  if (!words.length) return <Text style={s.dim} numberOfLines={1}>{read ? `reading ${read}…` : 'listening for the ident…'}</Text>;
  return (
    <Text style={s.tx} numberOfLines={1}>
      {words.map((w, i) => <Text key={w.s} style={{ color: NDB_Q_COLOURS[w.q] }}>{(i ? ' ' : '') + w.text}</Text>)}
    </Text>
  );
}

function Row({ s, b, live, words, sel, onPress }: { s: S; b: NdbBeacon; live: boolean; words: readonly NdbCopy[]; sel: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  const c = ndbIdentCell(b), st = ndbStation(b);
  const bars = ndbBars(b.snr, live);
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.row, !b.active && s.gone, sel && s.rowSel]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="button"
                      accessibilityLabel={`${khz(b.hz)} kilohertz, ${c.title}${st.name ? ', ' + st.name : ''}${st.dist ? ', ' + st.dist : ''}, `
                        + `${bars ? `signal ${bars} of 3` : 'not heard in the last minute'}${ndbOnMap(b) ? ' — show on the map' : ''}`}>
      <View style={s.line}>
        <Text style={[s.cell, s.f]}>{khz(b.hz)}</Text>
        <Text style={[s.cell, s.id, identStyle(c.cls, s.inkX)]} numberOfLines={1}>{c.text}</Text>
        <Text style={[s.cell, s.n]} numberOfLines={1}>{st.name}</Text>
        <Text style={[s.cell, s.d]} numberOfLines={1}>{st.dist}</Text>
        <Bars n={bars} />
      </View>
      <Words s={s} words={words} read={b.read} />
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

/** ★ The SNR sparkline over the last quarter hour — columns, newest right (the web's SVG path, in Views). */
function Spark({ s, pts }: { s: S; pts: readonly [number, number][] }) {
  if (pts.length < 2) return null;
  const mn = Math.min(...pts.map((q) => q[1])), mx = Math.max(mn + 1, ...pts.map((q) => q[1]));
  const t0 = pts[0][0], t1 = Math.max(t0 + 1, pts[pts.length - 1][0]);
  return (
    <View>
      <View style={s.spark}>
        {pts.map((q, i) => <View key={i} style={[s.sparkBar, { height: 2 + ((q[1] - mn) / (mx - mn)) * 24 }]} />)}
      </View>
      <Text style={s.dim}>{`SNR ${mn.toFixed(0)}–${mx.toFixed(0)} dB over ${Math.max(1, Math.round((t1 - t0) / 60))} min`}</Text>
    </View>
  );
}

function Card({ s, b, words, snr, onClose }: { s: S; b: NdbBeacon; words: readonly NdbCopy[]; snr: readonly [number, number][]; onClose: () => void }) {
  const c = ndbIdentCell(b), st = ndbStation(b);
  return (
    <View>
      <NavRow>
        <View style={s.cardHead}>
          <Text style={[s.cardIdent, identStyle(c.cls, s.inkX)]}>{c.text}</Text>
          <Text style={s.cardName} numberOfLines={2}>{st.name}</Text>
          <Key s={s} style={s.cardX} onPress={onClose} a11y="Close the beacon card"><Text style={s.chev}>✕</Text></Key>
        </View>
      </NavRow>
      <Text style={s.cardTxt}>{`${khz(b.hz)} kHz · ${b.a1a ? 'A1A' : b.tone ? `tone ${Math.round(b.tone)} Hz` : 'tone —'}${b.wpm ? ` · ${b.wpm.toFixed(1)} WPM` : ''}`}</Text>
      <Text style={s.cardTxt}>{`${st.dist ? `${st.dist} · ` : ''}${b.snr.toFixed(0)} dB${b.votes ? ` · read ${b.votes}× alike` : ''}`}</Text>
      <Spark s={s} pts={snr} />
      {words.length > 0 && <Words s={s} words={words.slice(-8)} read="" />}
      {!!b.alt?.length && <Text style={s.dim}>{`Also published on this frequency: ${b.alt.map((a) => `${a.ident} ${a.name}${a.km >= 0 ? ` (${nmText(a.km)})` : ''}`).join(', ')}`}</Text>}
      {!!b.harm && <Text style={s.dim}>{`2nd harmonic of ${b.harm.ident || ''} on ${khz(b.harm.hz)} kHz`}</Text>}
      {!ndbOnMap(b) && <Text style={s.dim}>Not in the published list — not on the map.</Text>}
    </View>
  );
}

/** ★ The three dialogs (the web's #ndbDlg): HEARD LOG, FIND NDB, RECEPTION STATS. Its own overlay, so Back closes it
 *  before it can leave NDB, and its own nav, so the pad walks its rows. */
function NdbDialog({ s, kind, ndb, insets, onClose, onPick }: {
  s: S; kind: Exclude<Dlg, ''>; ndb: NdbOverlay; insets: { top: number; bottom: number; left: number; right: number };
  onClose: () => void; onPick: (id: number) => void;
}) {
  useOverlay(true, { name: 'ndb dialog', onBack: onClose });
  const { navCtx, scrollProps } = usePanelNav(true, { flat: true, onBack: () => { onClose(); return true; } });
  const [q, setQ] = useState('');
  useEffect(() => {
    if (kind !== 'find') return;
    const t = setTimeout(() => ndb.onFind(q.trim()), 250);
    return () => clearTimeout(t);
  }, [q, kind]);   // eslint-disable-line react-hooks/exhaustive-deps
  const [snrOf, setSnrOf] = useState(-1);
  const r = ndb.report;
  const title = kind === 'log' ? 'HEARD LOG' : kind === 'find' ? 'FIND AN NDB' : 'RECEPTION STATS';
  const pick = (f: NdbFound) => {
    const b = r?.beacons.find((x) => x.db && ndbFoundKey(x.db) === ndbFoundKey(f));
    if (b) onPick(b.id);
  };
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.dlgWrap, { paddingTop: insets.top + 12, paddingBottom: insets.bottom + 12, paddingLeft: insets.left + 12, paddingRight: insets.right + 12 }]}
            onTouchStart={noteTouchInteraction}>
        <View style={s.dlgBox}>
          <NavRow>
            <View style={s.dlgHead}>
              <Text style={s.dlgTitle}>{title}</Text>
              <Key s={s} style={s.cardX} onPress={onClose} a11y={`Close ${title.toLowerCase()}`}><Text style={s.chev}>✕</Text></Key>
            </View>
          </NavRow>
          {kind === 'find' && (
            <TvTextInput value={q} onChangeText={setQ} placeholder="ident, name or kHz — empty: the nearest" autoCapitalize="characters"
                         autoCorrect={false} style={s.input} placeholderTextColor="rgba(255,190,40,0.4)" />
          )}
          <ScrollView {...scrollProps} contentContainerStyle={scrollLane} keyboardShouldPersistTaps="handled">
            {kind === 'log' && (!r || !r.log.length
              ? <Text style={s.note}>No beacons identified yet.</Text>
              : r.log.map((e, i) => (
                <NavRow key={`${e.ident}${e.hz}${i}`}>
                  <View style={s.tRow} accessible accessibilityLabel={`${e.ident} ${e.name} ${khz(e.hz)} kilohertz ${nmText(e.km, e.km >= 0 ? e.brg : undefined)}, best ${e.best.toFixed(0)} dB`}>
                    <Text style={[s.tc, { width: 46, color: '#ffd27a', fontWeight: '700' }]}>{e.ident}</Text>
                    <Text style={[s.tc, { flex: 1 }]} numberOfLines={1}>{`${flag(e.cc)} ${e.name}`.trim()}</Text>
                    <Text style={[s.tc, { width: 46, textAlign: 'right' }]}>{khz(e.hz)}</Text>
                    <Text style={[s.tc, { width: 74, textAlign: 'right' }]}>{nmText(e.km, e.km >= 0 ? e.brg : undefined)}</Text>
                    <Text style={[s.tc, { width: 44, textAlign: 'right' }]}>{`${e.best.toFixed(0)} dB`}</Text>
                    <Text style={[s.tc, { width: 58, textAlign: 'right' }]}>{utc(e.last)}</Text>
                  </View>
                </NavRow>
              )))}
            {kind === 'log' && <Text style={s.dim}>UTC. The gallery keeps each day's log and map.</Text>}
            {kind === 'find' && (() => {
              const fr = ndb.findResult;
              if (!fr) return <Text style={s.note}>Searching…</Text>;
              if (fr.pos === false) return <Text style={s.note}>This server has no position set, so nothing can be ranked by distance.</Text>;
              if (!fr.list.length) return <Text style={s.note}>Nothing matches.</Text>;
              return fr.list.slice(0, 120).map((f, i) => (
                <NavRow key={`${f.ident}${f.hz}${i}`}>
                  <FindRow s={s} f={f} onPress={f.heard ? () => pick(f) : undefined} />
                </NavRow>
              ));
            })()}
            {kind === 'find' && <Text style={s.dim}>Every published NDB (OurAirports, public domain), nearest you first. ✓ = heard here — choose it to see it on the map.</Text>}
            {kind === 'stats' && <Stats s={s} st={ndb.stats} snrOf={snrOf} onSnr={setSnrOf} />}
          </ScrollView>
        </View>
      </View>
    </NavCtx.Provider>
  );
}

function FindRow({ s, f, onPress }: { s: S; f: NdbFound; onPress?: () => void }) {
  const { focused, viewRef } = useNavButton(onPress, !onPress);
  const { ring } = useNavFocus();
  const body = (
    <>
      <Text style={[s.tc, { width: 58 }, f.heard && { color: '#8cff8c', fontWeight: '700' }]}>{`${f.heard ? '✓ ' : ''}${f.ident}`}</Text>
      <Text style={[s.tc, { flex: 1 }]} numberOfLines={1}>{`${flag(f.cc)} ${f.name}`.trim()}</Text>
      <Text style={[s.tc, { width: 46, textAlign: 'right' }]}>{khz(f.hz)}</Text>
      <Text style={[s.tc, { width: 74, textAlign: 'right' }]}>{nmText(f.km, f.brg)}</Text>
    </>
  );
  return onPress ? (
    <TouchableOpacity ref={viewRef as never} style={s.tRow} onPress={onPress} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${f.ident} ${f.name}, heard here — show it on the map`}>
      {body}<NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  ) : <View style={s.tRow} accessible accessibilityLabel={`${f.ident} ${f.name} ${khz(f.hz)} kilohertz ${nmText(f.km, f.brg)}`}>{body}</View>;
}

function BarChart({ s, vals, unit }: { s: S; vals: number[]; unit: string }) {
  const mx = Math.max(1, ...vals);
  return (
    <View style={s.chart} accessible accessibilityLabel={`${vals.length} bars, highest ${Math.round(mx * 10) / 10} ${unit}`}>
      {vals.map((v, i) => <View key={i} style={[s.chartBar, { height: Math.max(1, (v / mx) * 60) }]} />)}
    </View>
  );
}

function Stats({ s, st, snrOf, onSnr }: { s: S; st: NdbStats | null; snrOf: number; onSnr: (i: number) => void }) {
  if (!st) return <Text style={s.note}>Asking the server…</Text>;
  if (!st.hours.length && !st.beacons.length) {
    return <Text style={s.note}>No history yet — it builds up a minute at a time while NDB runs (kept in memory, at most 7 days).</Text>;
  }
  const one = snrOf >= 0 ? st.beacons[snrOf] : null;
  return (
    <View>
      <Text style={s.dim}>{`Since ${utc(st.since)} UTC on this server (in memory). UTC throughout.`}</Text>
      <Text style={s.h4}>BEACONS HEARD PER HOUR</Text>
      <BarChart s={s} vals={st.hours.map((h) => h.heard)} unit="beacons" />
      <Text style={s.h4}>FURTHEST BEACON HEARD PER HOUR (NM)</Text>
      <BarChart s={s} vals={st.hours.map((h) => h.far / 1.852)} unit="NM" />
      <Text style={s.h4}>BY HOUR OF DAY — AVERAGE BEACONS HEARD</Text>
      <BarChart s={s} vals={ndbByHourOfDay(st.hours)} unit="beacons" />
      <NavRow>
        <View style={s.dlgHead}>
          <Text style={[s.h4, { flex: 1 }]}>BEACONS, LEAST HEARD FIRST</Text>
          <Chip s={s} label="SHARE CSV" on={false} a11y="Share the beacons as CSV"
                onPress={() => { Share.share({ message: ndbStatsCsv(st), title: 'ndb-beacons.csv' }).catch(() => {}); }} />
        </View>
      </NavRow>
      {!!one && one.snr.length > 1 && (
        <View>
          <Text style={s.dim}>{`${one.ident} — SNR over time, ${utc(one.snr[0][0])}–${utc(one.snr[one.snr.length - 1][0])} UTC`}</Text>
          <Spark s={s} pts={one.snr} />
        </View>
      )}
      {st.beacons.map((b, i) => (
        <NavRow key={`${b.ident}${b.hz}${i}`}>
          <StatRow s={s} b={b} on={i === snrOf} onPress={() => onSnr(i === snrOf ? -1 : i)} />
        </NavRow>
      ))}
    </View>
  );
}
function StatRow({ s, b, on, onPress }: { s: S; b: NdbStats['beacons'][number]; on: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.tRow, on && s.rowSel]} onPress={onPress} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${b.ident} ${b.name}, ${b.minutes} minutes heard, mean ${b.mean.toFixed(1)} dB — show its SNR over time`}>
      <Text style={[s.tc, { width: 46 }]}>{b.ident}</Text>
      <Text style={[s.tc, { flex: 1 }]} numberOfLines={1}>{`${flag(b.cc)} ${b.name}`.trim()}</Text>
      <Text style={[s.tc, { width: 46, textAlign: 'right' }]}>{khz(b.hz)}</Text>
      <Text style={[s.tc, { width: 40, textAlign: 'right' }]}>{`${b.minutes}m`}</Text>
      <Text style={[s.tc, { width: 44, textAlign: 'right' }]}>{`${b.mean.toFixed(0)} dB`}</Text>
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
/* ★ AdsbLiveList's chrome values exactly — the amber-black at 0.92, a controls-colour border at 0.30, the text colour. */
const makeStyles = (ink: { x: string; k: string; kt: string }) => ({ inkX: ink.x, ...StyleSheet.create({
  wrap:   { position: 'absolute', backgroundColor: 'rgba(9,6,2,0.92)', borderWidth: 1, borderColor: `rgba(${ink.k},0.30)`,
            borderRadius: 10, overflow: 'hidden' },
  head:   { height: HEAD_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.25)` },
  pill:   { borderBottomWidth: 0, paddingHorizontal: 12 },
  headTxt:{ fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: `rgba(${ink.x},0.75)` },
  headCnt:{ fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)` },
  headSub:{ fontFamily: F, fontSize: 9.5, color: `rgba(${ink.x},0.55)`, flexShrink: 1, minWidth: 0 },
  chev:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.kt},0.80)` },
  chipRow:{ height: CHIPS_H, flexGrow: 0, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.25)` },
  chipRowIn: { alignItems: 'center', gap: 6, paddingHorizontal: 6 },
  chip:   { height: 22, justifyContent: 'center', paddingHorizontal: 7, borderRadius: 7, borderWidth: 1,
            borderColor: `rgba(${ink.k},0.30)`, backgroundColor: `rgba(${ink.k},0.06)` },
  chipOn: { borderColor: `rgba(${ink.k},0.70)`, backgroundColor: `rgba(${ink.k},0.16)` },
  chipTxt:{ fontFamily: F, fontSize: 11, color: `rgba(${ink.kt},0.88)`, fontVariant: ['tabular-nums'], letterSpacing: 0.5 },
  chipTxtOn: { color: `rgba(${ink.kt},1)` },
  chipKey:{ fontSize: 9.5, letterSpacing: 1.2, color: `rgba(${ink.kt},0.60)` },
  tile:   { height: 22, justifyContent: 'center', paddingHorizontal: 4 },
  row:    { height: ROW_H, justifyContent: 'center', paddingHorizontal: 8 },
  rowSel: { backgroundColor: 'rgba(255,229,102,0.12)', borderLeftWidth: 2, borderLeftColor: 'rgba(255,229,102,0.7)', paddingLeft: 6 },
  gone:   { opacity: 0.55 },
  line:   { flexDirection: 'row', alignItems: 'center', gap: 5 },
  cell:   { fontFamily: F, fontSize: 11, color: `rgba(${ink.x},0.75)`, fontVariant: ['tabular-nums'] },
  f:      { width: 40 },
  id:     { width: 46, fontSize: 12 },
  n:      { flex: 1, minWidth: 0 },
  d:      { width: 58, textAlign: 'right' },
  tx:     { fontFamily: F, fontSize: 10.5, letterSpacing: 0.6, marginTop: 2 },
  dim:    { fontFamily: F, fontSize: 10, color: `rgba(${ink.x},0.5)`, marginTop: 2 },
  note:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.60)`, padding: 10, lineHeight: 17 },
  card:   { position: 'absolute', backgroundColor: 'rgba(9,6,2,0.94)', borderWidth: 1, borderColor: `rgba(${ink.k},0.35)`,
            borderRadius: 10, padding: 10 },
  cardHead:{ flexDirection: 'row', alignItems: 'center', gap: 8 },
  cardIdent:{ fontFamily: F, fontSize: 15 },
  cardName:{ fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.9)`, flex: 1 },
  cardX:  { width: 32, height: 32, alignItems: 'center', justifyContent: 'center', borderRadius: 8, borderWidth: 1,
            borderColor: `rgba(${ink.k},0.35)` },
  cardTxt:{ fontFamily: F, fontSize: 11.5, color: `rgba(${ink.x},0.85)`, marginTop: 3, fontVariant: ['tabular-nums'] },
  spark:  { flexDirection: 'row', alignItems: 'flex-end', height: 28, gap: 1, marginTop: 6 },
  sparkBar:{ width: 2, backgroundColor: '#ffb000', borderRadius: 1 },
  dlgWrap:{ position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.55)', alignItems: 'center', justifyContent: 'center' },
  dlgBox: { width: '100%', maxWidth: 760, maxHeight: '100%', backgroundColor: '#0b0703', borderWidth: 1, borderColor: `rgba(${ink.k},0.4)`,
            borderRadius: 10, padding: 10 },
  dlgHead:{ flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 6 },
  dlgTitle:{ fontFamily: F, fontSize: 13, letterSpacing: 1.5, fontWeight: '600', color: `rgba(${ink.x},0.95)`, flex: 1 },
  input:  { fontFamily: F, fontSize: 14, color: '#ffd27a', borderWidth: 1, borderColor: `rgba(${ink.k},0.4)`, borderRadius: 6,
            paddingHorizontal: 8, paddingVertical: 6, backgroundColor: '#120c04', marginBottom: 6 },
  tRow:   { minHeight: 32, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 4,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.12)` },
  tc:     { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.85)`, fontVariant: ['tabular-nums'] },
  h4:     { fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: `rgba(${ink.x},0.7)`, marginTop: 12, marginBottom: 4 },
  chart:  { flexDirection: 'row', alignItems: 'flex-end', height: 62, gap: 1 },
  chartBar:{ flex: 1, backgroundColor: 'rgba(255,176,0,0.7)', minWidth: 1 },
}) });
