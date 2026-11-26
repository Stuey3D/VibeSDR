/**
 * DigiMapKeys — ★★ beta92 THE DIGI MODES MAP's keys, over the top-left of the map: the time window (1H · 24H · 7D · 30D ·
 * ALL, where the server keeps stations — the web's window buttons) and the panels (STATS · SUMMARY · LEGEND — the web's
 * checkboxes). The panels themselves are the map page's (src/utils/digiMapStats.ts, the web's same code).
 *
 * ★★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW (TrackLiveList's reason): a pad and the keyboard reach the app through
 *     hardwareKeys → PanelNav, never the page — so the keys that open the statistics are walked with the D-pad and pressed
 *     with Enter like any other row.
 * ★ The map's colours, never the faceplate's (AdsbLiveList's rule: it is part of the map); the decoder font.
 * ★ It reports its height (onHeight) so the page puts its panels under it, not under the keys.
 */
import React, { useMemo } from 'react';
import { StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import { DIGI_WINDOWS } from '../utils/digiMapShared';
import { DIGI_PANELS, type DigiPanelsShown } from '../utils/digiMapStats';

export interface DigiMapKeysProps {
  /** The page's map area, from the window's top (the page's top bar and filter row are above it). */
  top: number;
  left: number;
  /** The window keys: only where the server keeps stations (no dead keys). */
  windows: boolean;
  win: string;
  onWin: (w: string) => void;
  shown: DigiPanelsShown;
  onPanel: (id: keyof DigiPanelsShown, on: boolean) => void;
  onHeight?: (h: number) => void;
  ink: { x: string; k: string; kt: string };
  /** ★★★ beta96 maps2: the map's shared pad order (with the AMATEUR list, TrackLiveList) — absent = the keys' own. */
  navCtx?: React.ContextType<typeof NavCtx>;
}

export default function DigiMapKeys(p: DigiMapKeysProps) {
  const own = usePanelNav(!p.navCtx, { flat: true });
  const navCtx = p.navCtx ?? own.navCtx;
  const s = useMemo(() => makeStyles(p.ink), [p.ink]);
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.wrap, { top: p.top + 8, left: p.left + 8 }]} onTouchStart={noteTouchInteraction} pointerEvents="box-none"
            onLayout={(e) => p.onHeight?.(Math.round(e.nativeEvent.layout.height))}>
        <NavRow>
          <View style={s.row}>
            {p.windows && (
              <View style={s.group} accessibilityRole="radiogroup" accessibilityLabel="Time window">
                {DIGI_WINDOWS.map((w) => (
                  <Key key={w.id} s={s} label={w.label} words={`Stations heard in ${w.words}`} on={p.win === w.id} radio
                       onPress={() => { if (p.win !== w.id) p.onWin(w.id); }} />
                ))}
              </View>
            )}
            <View style={s.group}>
              {DIGI_PANELS.map((x) => (
                <Key key={x.id} s={s} label={x.label} words={x.words} on={p.shown[x.id]} onPress={() => p.onPanel(x.id, !p.shown[x.id])} />
              ))}
            </View>
          </View>
        </NavRow>
      </View>
    </NavCtx.Provider>
  );
}

type S = ReturnType<typeof makeStyles>;
function Key({ s, label, words, on, radio, onPress }: { s: S; label: string; words: string; on: boolean; radio?: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.key, on && (radio ? s.winOn : s.panOn)]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole={radio ? 'radio' : 'switch'} accessibilityState={{ checked: on }} accessibilityLabel={words}>
      <Text style={[s.txt, on && s.txtOn]}>{label}</Text>
      <NavRing on={focused} color={ring} radius={5} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
/* ★ TrackLiveList's range / toggle keys: the map's dark chrome, the controls colour for the borders, the same yellow for the
 *  window picked and blue for a panel shown (they mean the same on every map). */
const makeStyles = (ink: { x: string; k: string; kt: string }) => StyleSheet.create({
  wrap:  { position: 'absolute', zIndex: 20 },
  row:   { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  group: { flexDirection: 'row', gap: 4, padding: 4, borderRadius: 8, backgroundColor: 'rgba(9,6,2,0.88)',
           borderWidth: 1, borderColor: `rgba(${ink.k},0.30)` },
  key:   { height: 26, minWidth: 34, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 8, borderRadius: 5,
           borderWidth: 1, borderColor: `rgba(${ink.k},0.35)` },
  winOn: { backgroundColor: '#ffd34d', borderColor: '#ffd34d' },
  panOn: { backgroundColor: '#7fc8ff', borderColor: '#7fc8ff' },
  txt:   { fontFamily: F, fontSize: 11, letterSpacing: 0.8, color: `rgba(${ink.x},0.88)` },
  txtOn: { color: '#111' },
});
