/**
 * StepPicker — bottom-sheet tuning step selector.
 * Theme-aware: uses ThemeContext for font/colour tokens.
 */

import React from 'react';
import {
  Modal, StyleSheet, Text, TouchableOpacity,
  TouchableWithoutFeedback, View,
} from 'react-native';
import { RotateSheet, SheetRise } from './RotateFade';
import { STEPS_HZ } from '../services/sdrTypes';
import { STEP_833 } from '../utils/airband';
import { useTheme } from '../contexts/ThemeContext';
import { useListNav, noteTouchInteraction, useNavFocus } from './PanelNav';
import {
  usePopupTheme, usePopupSurface, usePopupFrame, engraveText, PopupKey, PopupPlate, PopupHandle, sheetCap,
} from './PopupShell';

/** Today's dim and sheet glass — Transparency OFF drops the first and makes the second opaque. */
const SHEET_BG = 'rgba(8,6,1,0.97)';

function stepLabel(hz: number): string {
  if (hz === STEP_833) return '8.33 kHz';   // the airband raster, 25/3 kHz — see utils/airband.ts
  if (hz >= 1_000_000) return (hz / 1_000_000) + ' MHz';
  if (hz >= 1_000)     return (hz / 1_000) + ' kHz';
  return hz + ' Hz';
}

interface StepPickerProps {
  visible:     boolean;
  currentStep: number;
  steps?:      number[];   // band-aware list (VHF/UHF gets larger steps); defaults to HF
  onSelect:    (hz: number) => void;
  onClose:     () => void;
  /** ★★ V12 VFO A/B — THE one place it is turned on and off (Stuart: "the VFO A/B button lives in the tuning menu, a
   *  simple toggle"). Passed only where the server offers it (VibeServer, hwinfo `watch`); absent = no row. */
  vfoAB?:      boolean;
  onVfoAB?:    (on: boolean) => void;
}

export default function StepPicker({ visible, currentStep, steps, onSelect, onClose, vfoAB = false, onVfoAB }: StepPickerProps) {
  const focusC = useNavFocus();   // ★ focus ring: white on green controls (Stuart, 2026-10-12)
  const stepList = steps && steps.length ? steps : STEPS_HZ;
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  const pt = usePopupTheme();
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(14, true);

  // Keyboard / D-pad navigation. The steps are a wrapped grid and CLOSE is the last
  // entry, so arrows walk the ladder and then land on CLOSE — no separate rule for it.
  // Shared machinery, so the game controller's D-pad drives this unchanged.
  // ★ VFO A/B (when offered) is one more stop before CLOSE; activating it flips it and leaves the sheet open.
  const abIdx = onVfoAB ? stepList.length : -1;
  const closeIdx = stepList.length + (onVfoAB ? 1 : 0);
  const navFocus = useListNav(
    visible,
    closeIdx + 1,
    (i) => {
      if (i === abIdx) { onVfoAB?.(!vfoAB); return; }
      if (i >= stepList.length) { onClose(); return; }
      onSelect(stepList[i]); onClose();
    },
    undefined,
    onClose,   // keyboard-mode idle close — see PanelNav
  );

  return (
    /* ★ SLIDES UP like every other sheet (AudioSheet, ModeSelector — Stuart, 2026-10-11: "every other menu pops up from
       the bottom except the step rate one which does this weird pop in and drop"). It had been "none" since June. */
    <Modal visible={visible} transparent animationType="none" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <RotateSheet><SheetRise>
      <View style={StyleSheet.absoluteFill} onTouchStart={noteTouchInteraction}>
        <TouchableWithoutFeedback onPress={onClose}>
          {/* ★★★ Transparency OFF: the tap-to-close view stays, the dim goes (§10.2). */}
          {/* ★★ NO DIM behind a bottom sheet, as AudioSheet and ModeSelector (Stuart, 2026-10-04). With the sheet now
              sliding up, a dark backdrop slid up with it and flickered (2026-10-11). The tap-to-close view stays. */}
          <View style={st.backdrop} />
        </TouchableWithoutFeedback>
        {/* ★★ Default chassis in the user's colours (Stuart, 2026-10-12: "yes colours apply everywhere"): the sheet's
            edge and the step keys in the CONTROLS colour, the heading in the TEXT colour (popupTokens ink). */}
        <View style={[st.sheet, { borderTopColor: pt.ink.border },
                      surf.opaque && !pt.metal && { backgroundColor: surf.fill(SHEET_BG) },
                      metalFrame, metalFrame && { paddingTop: 0 }]}>
          <PopupPlate radius={14} />
          <PopupHandle />
          <Text style={[st.sheetLabel, { color: pt.ink.section, fontFamily: t.font },
                        pt.metal && { ...engraveText(pt), fontWeight: '700' }]}>
            TUNING STEP
          </Text>
          <View style={st.grid}>
            {stepList.map((hz, i) => pt.metal ? (
              // ★ §10.3: an input selector — the current step's pip lit.
              <PopupKey key={hz} label={stepLabel(hz)} active={hz === currentStep} pip focused={navFocus === i}
                height={44} fontSize={13} onPress={() => { onSelect(hz); onClose(); }} hitSlop={4}
                style={{ width: '22%', flexGrow: 1 }} />
            ) : (
              <TouchableOpacity
                key={hz}
                style={[
                  st.btn,
                  { borderColor: pt.ink.keyBorder, paddingVertical: isWhite ? 14 : 12 },
                  hz === currentStep && { backgroundColor: pt.ink.keyBgAct, borderColor: pt.gold.selBorder },
                  // Focus outranks 'active' so the caret is never hidden on the
                  // step you are already using — the one you most need to see.
                  navFocus === i && { borderColor: focusC.ring, borderWidth: 2 },
                ]}
                onPress={() => { onSelect(hz); onClose(); }}
                hitSlop={4} activeOpacity={0.75}
              >
                <Text style={[
                  st.btnText,
                  { fontFamily: t.font, fontSize: isWhite ? 15 : 14,
                    color: pt.ink.key },
                  hz === currentStep && { color: pt.gold.sel },
                ]}>
                  {stepLabel(hz)}
                </Text>
              </TouchableOpacity>
            ))}
          </View>
          {/* ── ★★ V12 VFO A/B: a second VFO on the same radio, heard at once (lower frequency in the left ear) ── */}
          {onVfoAB && (
            <View style={st.abRow}>
              <Text style={[st.abLabel, { color: pt.ink.section, fontFamily: t.font },
                            pt.metal && { ...engraveText(pt), fontWeight: '700' }]}>VFO A/B</Text>
              {pt.metal ? (
                <PopupKey label={vfoAB ? 'ON' : 'OFF'} active={vfoAB} pip focused={navFocus === abIdx}
                  height={36} fontSize={13} onPress={() => onVfoAB(!vfoAB)} hitSlop={4} style={{ width: 96 }} />
              ) : (
                <TouchableOpacity
                  style={[st.btn, st.abKey, { borderColor: pt.ink.keyBorder },
                          vfoAB && { backgroundColor: pt.ink.keyBgAct, borderColor: pt.gold.selBorder },
                          navFocus === abIdx && { borderColor: focusC.ring, borderWidth: 2 }]}
                  onPress={() => onVfoAB(!vfoAB)} hitSlop={4} activeOpacity={0.75}
                  accessibilityRole="switch" accessibilityState={{ checked: vfoAB }}
                  accessibilityLabel="VFO A/B" accessibilityHint="A second frequency on the same radio, heard at the same time">
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 14, color: vfoAB ? pt.gold.sel : pt.ink.key }]}>
                    {vfoAB ? 'ON' : 'OFF'}
                  </Text>
                </TouchableOpacity>
              )}
            </View>
          )}
          {onVfoAB && (
            <Text style={[st.abNote, { color: pt.ink.section, fontFamily: t.font }]}>
              {vfoAB ? 'Switch VFO with the A/B key, or press and hold the tune drum\'s window'
                     : 'Listen to a second frequency on this radio at the same time'}
            </Text>
          )}
          {pt.metal ? (
            <PopupKey label="CLOSE" onPress={onClose} focused={navFocus === closeIdx} height={32}
              style={{ alignSelf: 'center', width: 110, marginTop: 14 }} />
          ) : (
          <TouchableOpacity
            style={[st.closeBtn, { borderColor: pt.ink.keyBorder },
                    navFocus === closeIdx && { borderColor: focusC.ring, borderWidth: 2 }]}
            onPress={onClose} activeOpacity={0.75}
          >
            <Text style={[st.closeBtnText, { fontFamily: t.font, color: pt.gold.close }]}>
              CLOSE
            </Text>
          </TouchableOpacity>
          )}
        </View>
      </View>
      </SheetRise></RotateSheet>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:      { flex: 1 },
  /* ★★ CAPPED AND CENTRED, like the audio sheet (maxWidth 640). On a Mac the window is the screen, and an
   *  uncapped sheet stretched seven keys across ~2000 pt — and drew its brushed plate over only the left
   *  half until it was closed and reopened (Stuart, B16): a canvas that wide being resized is the one
   *  this sheet had no reason to be. A phone is narrower than the cap, so nothing changes there. */
  sheet: {
    ...sheetCap,
    backgroundColor: SHEET_BG,
    borderTopWidth: 1,
    borderTopLeftRadius: 14, borderTopRightRadius: 14,
    paddingHorizontal: 16, paddingTop: 16, paddingBottom: 40,
  },
  sheetLabel:    { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 14 },
  grid:          { flexDirection: 'row', flexWrap: 'wrap', gap: 7 },
  btn: {
    width: '22%', flexGrow: 1, backgroundColor: 'transparent',
    borderWidth: 1, borderRadius: 3,
    paddingHorizontal: 4, alignItems: 'center', justifyContent: 'center',
  },
  btnText:       { textAlign: 'center' },
  closeBtn: {
    alignSelf: 'center', marginTop: 14, backgroundColor: 'transparent',
    borderWidth: 1, borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText:  { fontSize: 11, textAlign: 'center' },
  abRow:         { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 16, gap: 12 },
  abLabel:       { fontSize: 10, letterSpacing: 3 },
  abKey:         { flexGrow: 0, width: 96, paddingVertical: 10 },
  abNote:        { fontSize: 10, opacity: 0.75, marginTop: 6, textAlign: 'right' },
});
