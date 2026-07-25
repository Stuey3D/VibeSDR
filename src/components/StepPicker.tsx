/**
 * StepPicker — bottom-sheet tuning step selector.
 * Theme-aware: uses ThemeContext for font/colour tokens.
 */

import React from 'react';
import {
  Modal, StyleSheet, Text, TouchableOpacity,
  TouchableWithoutFeedback, View,
} from 'react-native';
import { STEPS_HZ } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import { useListNav, NAV_FOCUS } from './PanelNav';

function stepLabel(hz: number): string {
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
}

export default function StepPicker({ visible, currentStep, steps, onSelect, onClose }: StepPickerProps) {
  const stepList = steps && steps.length ? steps : STEPS_HZ;
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';

  // Keyboard / D-pad navigation. The steps are a wrapped grid and CLOSE is the last
  // entry, so arrows walk the ladder and then land on CLOSE — no separate rule for it.
  // Shared machinery, so the game controller's D-pad drives this unchanged.
  const navFocus = useListNav(
    visible,
    stepList.length + 1,
    (i) => {
      if (i >= stepList.length) { onClose(); return; }
      onSelect(stepList[i]); onClose();
    },
  );

  return (
    <Modal visible={visible} transparent animationType="none" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <View style={StyleSheet.absoluteFill}>
        <TouchableWithoutFeedback onPress={onClose}>
          <View style={st.backdrop} />
        </TouchableWithoutFeedback>
        <View style={[st.sheet, { borderTopColor: t.barBorder }]}>
          <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font }]}>
            TUNING STEP
          </Text>
          <View style={st.grid}>
            {stepList.map((hz, i) => (
              <TouchableOpacity
                key={hz}
                style={[
                  st.btn,
                  { borderColor: isWhite ? 'rgba(255,255,255,0.20)' : 'rgba(80,50,0,0.40)',
                    paddingVertical: isWhite ? 14 : 12 },
                  hz === currentStep && { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
                  // Focus outranks 'active' so the caret is never hidden on the
                  // step you are already using — the one you most need to see.
                  navFocus === i && { borderColor: NAV_FOCUS, borderWidth: 2 },
                ]}
                onPress={() => { onSelect(hz); onClose(); }}
                hitSlop={4} activeOpacity={0.75}
              >
                <Text style={[
                  st.btnText,
                  { fontFamily: t.font, fontSize: isWhite ? 15 : 14,
                    color: isWhite ? 'rgba(255,255,255,0.55)' : 'rgba(150,100,30,0.70)' },
                  hz === currentStep && { color: t.btnActiveText },
                ]}>
                  {stepLabel(hz)}
                </Text>
              </TouchableOpacity>
            ))}
          </View>
          <TouchableOpacity
            style={[st.closeBtn, { borderColor: t.btnBorder },
                    navFocus === stepList.length && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
            onPress={onClose} activeOpacity={0.75}
          >
            <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>
              CLOSE
            </Text>
          </TouchableOpacity>
        </View>
      </View>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:      { flex: 1, backgroundColor: 'rgba(0,0,0,0.52)' },
  sheet: {
    backgroundColor: 'rgba(8,6,1,0.97)',
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
});
