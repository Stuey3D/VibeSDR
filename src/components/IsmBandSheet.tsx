/**
 * IsmBandSheet — the ISM box's BANDS list (V12 beta72).
 *
 * ★★ Stuart, 2026-11-13: the box's ◀ ▶ keys did exactly what the main tune keys do, so they were a duplicate. One BANDS
 *    key replaces them and opens EVERY band this radio reaches, each with its title and where it is used, so a listener
 *    can jump straight to one ("433 ISM (Europe)", "868 SRD LoRa (Europe)" …). Scrolls on a small screen. No region lock:
 *    every server lists every band its radio reaches — the server's position only chooses where ISM OPENS ("just in case
 *    pirates in Brazil are using MeshCore on 868").
 * The tune keys, the drum and the pad still step band by band; a pick here goes through the same stepper (one retune).
 * The bottom-sheet chassis is StepPicker's.
 */

import React, { useEffect, useRef } from 'react';
import {
  Modal, ScrollView, StyleSheet, Text, TouchableOpacity, TouchableWithoutFeedback, View, Animated, useWindowDimensions,
} from 'react-native';
import { RotateSheet, SheetRise } from './RotateFade';
import { useTheme } from '../contexts/ThemeContext';
import { useListNav, noteTouchInteraction, useNavFocus } from './PanelNav';
import {
  usePopupTheme, usePopupSurface, usePopupFrame, engraveText, PopupKey, PopupPlate, PopupHandle, useSheetDrag, sheetCap,
} from './PopupShell';
import type { IsmBand } from '../services/ism';

const SHEET_BG = 'rgba(8,6,1,0.97)';
const ROW_H = 54;

interface IsmBandSheetProps {
  visible: boolean;
  bands: IsmBand[];
  /** The band the receiver is on (or a burst is heading for) — marked. */
  currentId: string;
  onPick: (id: string) => void;
  onClose: () => void;
}

export default function IsmBandSheet({ visible, bands, currentId, onPick, onClose }: IsmBandSheetProps) {
  const focusC = useNavFocus();
  const { theme: t } = useTheme();
  const pt = usePopupTheme();
  const sheetDrag = useSheetDrag(onClose);
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(14, true);
  const { height: winH } = useWindowDimensions();
  // ★ the list scrolls inside the sheet: never taller than ~60 % of the window (the heading, CLOSE and the handle fit too)
  const listMax = Math.max(ROW_H * 2.5, Math.min(bands.length * (ROW_H + 6), winH * 0.6 - 60));
  const scrollRef = useRef<ScrollView | null>(null);
  const curIdx = bands.findIndex((b) => b.id === currentId);

  const pick = (i: number) => { const b = bands[i]; if (b) { onPick(b.id); } onClose(); };
  const closeIdx = bands.length;
  const navFocus = useListNav(
    visible, closeIdx + 1,
    (i) => { if (i >= bands.length) onClose(); else pick(i); },
    (i) => { if (i < bands.length) scrollRef.current?.scrollTo({ y: Math.max(0, i * (ROW_H + 6) - listMax / 2 + ROW_H / 2), animated: false }); },
    onClose,
    () => { onClose(); return true; },
    { startAt: Math.max(0, curIdx) },
  );
  // ★ open on the current band: scrolled so it is in view
  useEffect(() => {
    if (!visible || curIdx < 0) return;
    const y = Math.max(0, curIdx * (ROW_H + 6) - listMax / 2 + ROW_H / 2);
    const h = setTimeout(() => scrollRef.current?.scrollTo({ y, animated: false }), 0);
    return () => clearTimeout(h);
  }, [visible, curIdx, listMax]);

  return (
    <Modal visible={visible} transparent animationType="none" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <RotateSheet><SheetRise>
      <View style={StyleSheet.absoluteFill} onTouchStart={noteTouchInteraction}>
        <TouchableWithoutFeedback onPress={onClose}><View style={st.backdrop} /></TouchableWithoutFeedback>
        <Animated.View style={[{ transform: [{ translateY: sheetDrag.y }] }, st.sheet, { borderTopColor: pt.ink.border },
                      surf.opaque && !pt.metal && { backgroundColor: surf.fill(SHEET_BG) },
                      metalFrame, metalFrame && { paddingTop: 0 }]}>
          <PopupPlate radius={14} />
          <PopupHandle drag={sheetDrag} />
          <Text style={[st.sheetLabel, { color: pt.ink.section, fontFamily: t.font },
                        pt.metal && { ...engraveText(pt), fontWeight: '700' }]}>
            ISM BANDS
          </Text>
          <ScrollView ref={scrollRef} style={{ maxHeight: listMax }} contentContainerStyle={st.list}
                      showsVerticalScrollIndicator keyboardShouldPersistTaps="handled">
            {bands.map((b, i) => {
              const cur = b.id === currentId;
              return (
                <TouchableOpacity key={b.id}
                  style={[st.row, { borderColor: pt.ink.keyBorder },
                          cur && { backgroundColor: pt.ink.keyBgAct, borderColor: pt.gold.selBorder },
                          navFocus === i && { borderColor: focusC.ring, borderWidth: 2 }]}
                  onPress={() => pick(i)} activeOpacity={0.75}
                  accessibilityRole="button" accessibilityState={{ selected: cur }}
                  accessibilityLabel={`${b.name}${b.region ? `, used in ${b.region}` : ''}${cur ? ', the band you are on' : ''}`}>
                  <Text numberOfLines={1} style={[st.name, { fontFamily: t.font, color: cur ? pt.gold.sel : pt.ink.key }]}>
                    {(cur ? '● ' : '') + b.name}
                  </Text>
                  {!!b.region && (
                    <Text numberOfLines={1} style={[st.region, { fontFamily: t.font, color: pt.ink.section }]}>{b.region}</Text>
                  )}
                </TouchableOpacity>
              );
            })}
          </ScrollView>
          {pt.metal ? (
            <PopupKey label="CLOSE" onPress={onClose} focused={navFocus === closeIdx} height={32}
              style={{ alignSelf: 'center', width: 110, marginTop: 14 }} />
          ) : (
            <TouchableOpacity style={[st.closeBtn, { borderColor: pt.ink.keyBorder },
                                      navFocus === closeIdx && { borderColor: focusC.ring, borderWidth: 2 }]}
                              onPress={onClose} activeOpacity={0.75}>
              <Text style={[st.closeBtnText, { fontFamily: t.font, color: pt.gold.close }]}>CLOSE</Text>
            </TouchableOpacity>
          )}
        </Animated.View>
      </View>
      </SheetRise></RotateSheet>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop: { flex: 1 },
  sheet: {
    ...sheetCap,
    backgroundColor: SHEET_BG,
    borderTopWidth: 1, borderTopLeftRadius: 14, borderTopRightRadius: 14,
    paddingHorizontal: 16, paddingTop: 16, paddingBottom: 40,
  },
  sheetLabel: { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 14 },
  list: { gap: 6 },
  row: {
    height: ROW_H, borderWidth: 1, borderRadius: 3, paddingHorizontal: 12, justifyContent: 'center', backgroundColor: 'transparent',
  },
  name: { fontSize: 14 },
  region: { fontSize: 11, marginTop: 3 },
  closeBtn: {
    alignSelf: 'center', marginTop: 14, backgroundColor: 'transparent',
    borderWidth: 1, borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText: { fontSize: 11, textAlign: 'center' },
});
