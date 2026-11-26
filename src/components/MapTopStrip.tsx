/**
 * MapTopStrip — beta96 (Stuart 11-26, "big issue"): the full-screen maps' SHARED TUNER line, CHAT and TUNE chip in the app,
 * the web's TrackView 'share' slot + tune chip (web/client/src/tracks.ts, main.ts mapSharedSync).
 *
 * ★★★ DRAWN BY REACT NATIVE, ONE ROW UNDER THE MAP PAGE'S TOP BAR (that bar is HTML inside the WebView; a pad and the keyboard
 *     reach the app through hardwareKeys → PanelNav, never the page — TrackLiveList's note). One sideways-scrolling row, so a
 *     phone fits the line, CHAT and TUNE without clipping.
 * ★★ The words are dialChat.mapSharedLine (the SAME inputs as the controls bar's banner: the server's dial, the count, the
 *    ask lock); the TUNE rows are airSeaTune.ts (THIS radio's vibeserver.json). Nothing is computed here.
 * ★★ Locked (the server's ask-before-tuning answer, the owner's scan): the TUNE chip and its rows are greyed with the reason
 *    and a press says why (onLocked) — never the map: the list, tabs, chips, cards and the camera stay this listener's own.
 * ★ The line opens the chat (where the asking is done); CHAT is the bar's chat (the same drawer, drawn over the map by
 *   SDRScreen, the same unread count, greyed when the bar's is).
 */
import React, { useMemo, useState } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import type { MapSharedLine } from '../services/dialChat';
import { TUNE_GROUP_LABEL, TUNE_ICON_PATH, type AirSeaTuneRow } from '../services/airSeaTune';

export interface MapSharedProps {
  line: MapSharedLine;
  chatDisabled: boolean;
  /** The bar's CHAT key's unread pulse (a line arrived, or the ask nudge). */
  chatUnread: boolean;
  onChat: () => void;
}
export interface MapTuneProps {
  rows: AirSeaTuneRow[];
  chip: string;
  onPick: (id: string) => void;
  /** A row the server says this radio cannot run: its reason. */
  onRefused: (why: string) => void;
}
export interface MapTopStripProps {
  top: number;
  left: number;
  shared?: MapSharedProps | null;
  tune?: MapTuneProps | null;
  /** The dial lock's reason ('' = free) and what a press on a locked control does (the nudge). */
  lockWhy: string;
  onLocked: (why: string) => void;
  /** ★ the map's shared pad order (with TrackLiveList) — absent = the strip's own. */
  navCtx?: React.ContextType<typeof NavCtx>;
}

/** What the strip shows, for the source tests: nothing at all when there is neither a shared line nor a TUNE choice. */
export function mapTopStripShown(p: Pick<MapTopStripProps, 'shared' | 'tune'>): boolean {
  return !!(p.shared?.line.show || (p.tune && p.tune.rows.length > 1));
}

export default function MapTopStrip(p: MapTopStripProps) {
  const own = usePanelNav(!p.navCtx && mapTopStripShown(p), { flat: true });
  const navCtx = p.navCtx ?? own.navCtx;
  const [open, setOpen] = useState(false);
  const s = useMemo(() => makeStyles(), []);
  if (!mapTopStripShown(p)) return null;
  const locked = !!p.lockWhy;
  const sh = p.shared?.line.show ? p.shared : null;
  const tune = p.tune && p.tune.rows.length > 1 ? p.tune : null;
  let last = '';
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.wrap, { top: p.top + 6, left: p.left + 8 }]} onTouchStart={noteTouchInteraction} pointerEvents="box-none">
        <NavRow>
          <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={s.row} keyboardShouldPersistTaps="handled">
            {sh && <Key s={s} style={[s.line, sh.line.dialLocked ? s.lineLocked : /FREE/.test(sh.line.text) ? s.lineFree : null]}
                        txt={[s.lineTxt, sh.line.dialLocked ? s.lineTxtLocked : /FREE/.test(sh.line.text) ? s.lineTxtFree : null]}
                        label={sh.line.text} words={`${sh.line.text.replace('👤', 'listening')} — open the chat`} onPress={sh.onChat} />}
            {sh && <Key s={s} style={[s.key, sh.chatDisabled && s.dim]} txt={s.txt} label={sh.chatUnread ? 'CHAT ●' : 'CHAT'}
                        words={sh.chatDisabled ? 'Chat is not available here' : `Chat${sh.chatUnread ? ', unread' : ''}`}
                        onPress={() => { if (!sh.chatDisabled) sh.onChat(); }} />}
            {tune && <Key s={s} style={[s.key, s.tune, locked && s.dim]} txt={[s.txt, s.tuneTxt]} label={`${tune.chip} ▾`}
                          words={locked ? p.lockWhy : `${tune.chip}. Tune the radio — the modes this radio can run`}
                          onPress={() => { if (locked) p.onLocked(p.lockWhy); else setOpen(!open); }} />}
          </ScrollView>
        </NavRow>
        {tune && open && !locked && (
          <View style={s.pop} accessibilityRole="menu" accessibilityLabel="Tune the radio">
            <ScrollView style={{ maxHeight: 380 }}>
              {tune.rows.map((r) => {
                const head = TUNE_GROUP_LABEL[r.group] !== last ? TUNE_GROUP_LABEL[r.group] : '';
                last = TUNE_GROUP_LABEL[r.group];
                return (
                  <React.Fragment key={r.id}>
                    {!!head && <Text style={s.grp}>{head}</Text>}
                    <NavRow>
                      <TuneRow s={s} r={r} onPress={() => {
                        if (r.why) { tune.onRefused(r.why); return; }
                        setOpen(false);
                        if (!r.live) tune.onPick(r.id);
                      }} />
                    </NavRow>
                  </React.Fragment>
                );
              })}
            </ScrollView>
          </View>
        )}
      </View>
    </NavCtx.Provider>
  );
}

type S = ReturnType<typeof makeStyles>;
function Key({ s, style, txt, label, words, onPress }: { s: S; style: unknown; txt: unknown; label: string; words: string; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={style as never} onPress={onPress} activeOpacity={0.7} accessibilityRole="button" accessibilityLabel={words}>
      <Text style={txt as never} numberOfLines={1}>{label}</Text>
      <NavRing on={focused} color={ring} radius={6} />
    </TouchableOpacity>
  );
}
function TuneRow({ s, r, onPress }: { s: S; r: AirSeaTuneRow; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.tRow, r.live && s.tRowLive, !!r.why && s.dim]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="radio" accessibilityState={{ checked: r.live, disabled: !!r.why }}
                      accessibilityLabel={`${r.name}, ${r.freq}${r.live ? ', live now' : ''}${r.why ? '. ' + r.why : ''}`}>
      <Svg width={16} height={16} viewBox="0 0 16 16"><Path d={TUNE_ICON_PATH[r.icon]} fill="#9fc6e6" /></Svg>
      <Text style={s.tName}>{r.name}</Text>
      <Text style={s.tFreq} numberOfLines={1}>{r.freq}</Text>
      {r.live && <Text style={s.tLive}>LIVE</Text>}
      <NavRing on={focused} color={ring} radius={6} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
const makeStyles = () => StyleSheet.create({
  wrap:  { position: 'absolute', right: 8, zIndex: 25 },
  row:   { flexDirection: 'row', alignItems: 'center', gap: 6, paddingRight: 8 },
  key:   { height: 28, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 6, backgroundColor: 'rgba(30,42,54,0.95)',
           borderWidth: 1, borderColor: '#33475a' },
  txt:   { fontFamily: F, fontSize: 11, letterSpacing: 0.8, color: '#e8eef5' },
  dim:   { opacity: 0.42 },
  line:  { height: 28, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 6, backgroundColor: 'rgba(255,190,80,0.14)',
           borderWidth: 1, borderColor: 'rgba(255,190,80,0.5)' },
  lineFree:   { backgroundColor: 'rgba(123,216,143,0.12)', borderColor: 'rgba(123,216,143,0.5)' },
  lineLocked: { backgroundColor: 'rgba(255,90,70,0.14)', borderColor: 'rgba(255,120,100,0.6)' },
  lineTxt:    { fontFamily: F, fontSize: 10.5, letterSpacing: 1, color: '#ffcf7a' },
  lineTxtFree:   { color: '#7bd88f' },
  lineTxtLocked: { color: '#ff9d8a' },
  tune:  { backgroundColor: 'rgba(58,24,20,0.95)', borderColor: '#a3473a' },
  tuneTxt: { color: '#ffd9d2' },
  pop:   { marginTop: 6, alignSelf: 'flex-start', minWidth: 260, maxWidth: 360, backgroundColor: 'rgba(16,24,32,0.98)', borderRadius: 10,
           borderWidth: 1, borderColor: '#2c3e52', padding: 6 },
  grp:   { fontFamily: F, fontSize: 9.5, letterSpacing: 1.2, color: '#7f93a7', paddingHorizontal: 8, paddingTop: 8, paddingBottom: 4 },
  tRow:  { flexDirection: 'row', alignItems: 'center', gap: 9, paddingHorizontal: 8, paddingVertical: 8, borderRadius: 6 },
  tRowLive: { backgroundColor: 'rgba(255,107,90,0.12)' },
  tName: { fontSize: 13, fontWeight: '600', color: '#e8eef5' },
  tFreq: { flex: 1, textAlign: 'right', fontFamily: F, fontSize: 11.5, color: '#9fb0c2' },
  tLive: { fontFamily: F, fontSize: 9, letterSpacing: 1, color: '#111', backgroundColor: '#ff6b5a', borderRadius: 3, paddingHorizontal: 4, paddingVertical: 2, overflow: 'hidden' },
});
