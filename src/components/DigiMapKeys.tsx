/**
 * DigiMapKeys — ★★ beta92 the spots maps' panel keys (STATS · SUMMARY · LEGEND — the web's checkboxes), over the top-left of
 * the map, on the maps that are NOT on the map cards: the on-device FT8 map and an UberSDR's spots map. The panels
 * themselves are the map page's (src/utils/digiMapStats.ts, the web's same code).
 *
 * ★★★ beta97: the AMATEUR map (a VibeServer that keeps stations) is on the MAP CARDS (MapChrome — TIME, SOURCES, MODE / BAND
 *     / RADIO in MAP CONTROLS, LAYERS, STATS, the KEY), so this component's window keys, THIS SESSION, SHOW and OTHER RADIOS
 *     were DELETED with it — none of them was ever drawn anywhere else.
 * ★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW: a pad and the keyboard reach the app through hardwareKeys → PanelNav, never the
 *    page — so the keys that open the statistics are walked with the D-pad and pressed with Enter like any other row.
 * ★ The map's colours, never the faceplate's; the decoder font. It reports its height (onHeight) so the page puts its
 *   panels under it, not under the keys.
 */
import React, { useMemo } from 'react';
import { StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import { DIGI_PANELS, type DigiPanelsShown } from '../utils/digiMapStats';

export interface DigiMapKeysProps {
  /** The page's map area, from the window's top (the page's top bar and filter row are above it). */
  top: number;
  left: number;
  shown: DigiPanelsShown;
  onPanel: (id: keyof DigiPanelsShown, on: boolean) => void;
  onHeight?: (h: number) => void;
  ink: { x: string; k: string; kt: string };
}

export default function DigiMapKeys(p: DigiMapKeysProps) {
  const { navCtx } = usePanelNav(true, { flat: true });
  const s = useMemo(() => makeStyles(p.ink), [p.ink]);
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.wrap, { top: p.top + 8, left: p.left + 8 }]} onTouchStart={noteTouchInteraction} pointerEvents="box-none"
            onLayout={(e) => p.onHeight?.(Math.round(e.nativeEvent.layout.height))}>
        <NavRow>
          <View style={s.row}>
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
function Key({ s, label, words, on, onPress }: { s: S; label: string; words: string; on: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.key, on && s.panOn]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="switch" accessibilityState={{ checked: on }} accessibilityLabel={words}>
      <Text style={[s.txt, on && s.txtOn]}>{label}</Text>
      <NavRing on={focused} color={ring} radius={5} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
/* ★ the map's dark chrome, the controls colour for the borders, blue for a panel shown */
const makeStyles = (ink: { x: string; k: string; kt: string }) => StyleSheet.create({
  wrap:  { position: 'absolute', zIndex: 20 },
  row:   { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  group: { flexDirection: 'row', gap: 4, padding: 4, borderRadius: 8, backgroundColor: 'rgba(9,6,2,0.88)',
           borderWidth: 1, borderColor: `rgba(${ink.k},0.30)` },
  key:   { height: 26, minWidth: 34, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 8, borderRadius: 5,
           borderWidth: 1, borderColor: `rgba(${ink.k},0.35)` },
  panOn: { backgroundColor: '#7fc8ff', borderColor: '#7fc8ff' },
  txt:   { fontFamily: F, fontSize: 11, letterSpacing: 0.8, color: `rgba(${ink.x},0.88)` },
  txtOn: { color: '#111' },
});
