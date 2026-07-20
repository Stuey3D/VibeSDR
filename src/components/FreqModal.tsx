import React, { useEffect, useRef, useState } from 'react';
import {
  Keyboard, KeyboardAvoidingView, Modal, Platform,
  Pressable, StyleSheet, Text, TextInput, TouchableOpacity, View,
} from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { MAX_FREQ_HZ, MIN_FREQ_HZ } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import ProfilePicker from './ProfilePicker';

type Unit = 'hz' | 'khz' | 'mhz';

interface FreqModalProps {
  visible:   boolean;
  currentHz: number;
  onConfirm: (hz: number) => void;
  onClose:   () => void;
  /** Controlled unit — selection here also drives the main frequency display. */
  unit?:     Unit;
  onUnit?:   (u: Unit) => void;
  /** Backend tuning range (Hz). Defaults to UberSDR HF limits; local hardware
   *  widens it so VHF/UHF entries (e.g. 96.6 MHz) aren't rejected. */
  minHz?:    number;
  maxHz?:    number;
  /** Lock to MHz (FM-DX broadcast) — grey out the Hz/kHz options. */
  lockUnit?: boolean;
  /** Share the current station (moved here from the controls bar). Hidden when
   *  sharing isn't available (undefined). */
  onShare?:  () => void;

  /** OWRX profiles — MOVED HERE FROM THE MAIN MENU. On OWRX a profile IS a frequency
   *  choice (it's how you pick the band you want to be in), so it belongs with the
   *  frequency rather than in settings. Hidden entirely when the backend reports no
   *  profiles. Etiquette warning + live user count come along with it: a profile
   *  switch changes the band for EVERYONE on that SDR. */
  profiles?:        { id: string; name: string }[];
  activeProfileId?: string;
  sdrUsage?:        Record<string, { name: string; inUse: boolean; activeProfileId?: string }>;
  clientCount?:     number;
  onSelectProfile?: (id: string) => void;

  /** VTS "nearby station" skip — relocated from MenuSheet (§4.1). Shown as a row above the
   *  number: ◄ name ►. The always-on VTSBar is untouched. FM-DX disables skip (one shared
   *  tuner) — SDRScreen omits the handlers there, so the guard travels with the row. */
  vtsName?:   string;
  vtsFreq?:   number | null;
  onVtsPrev?: () => void;
  onVtsNext?: () => void;
}

function toDisplay(hz: number, unit: Unit): string {
  if (unit === 'hz')  return Math.round(hz).toString();
  if (unit === 'khz') return (hz / 1000).toFixed(3);
  return (hz / 1e6).toFixed(6);
}

function fromDisplay(val: string, unit: Unit): number {
  const n = parseFloat(val.replace(/[^0-9.]/g, ''));
  if (isNaN(n) || n <= 0) return 0;
  if (unit === 'hz')  return Math.round(n);
  if (unit === 'khz') return Math.round(n * 1000);
  return Math.round(n * 1e6);
}

export default function FreqModal({
  visible, currentHz, onConfirm, onClose,
  unit: unitProp, onUnit,
  minHz = MIN_FREQ_HZ, maxHz = MAX_FREQ_HZ, lockUnit = false,
  onShare,
  profiles = [], activeProfileId, sdrUsage, clientCount, onSelectProfile,
  vtsName, vtsFreq, onVtsPrev, onVtsNext,
}: FreqModalProps) {
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  const [unitState, setUnitState] = useState<Unit>('khz');
  const unit = unitProp ?? unitState;
  const [value, setValue] = useState('');
  const inputRef          = useRef<TextInput>(null);
  // Share presents the native iOS share sheet (UIActivityViewController). Doing
  // that while this Modal is on screen (or mid-dismiss) wedges iOS touch
  // handling, so on iOS we close first and fire the share from the Modal's
  // onDismiss (fires after full dismissal). Android has no such conflict.
  const pendingShare = useRef(false);
  // Android Modals are a separate window that adjustResize doesn't shrink, so
  // KeyboardAvoidingView can't see the keyboard — track its height ourselves and
  // pad the box up by it so it floats just above the keypad.
  const [kbHeight, setKbHeight] = useState(0);
  useEffect(() => {
    if (Platform.OS !== 'android') return;
    const show = Keyboard.addListener('keyboardDidShow', e => setKbHeight(e.endCoordinates.height));
    const hide = Keyboard.addListener('keyboardDidHide', () => setKbHeight(0));
    return () => { show.remove(); hide.remove(); };
  }, []);

  useEffect(() => {
    if (unitProp !== undefined) return; // controlled by SDRScreen
    AsyncStorage.getItem('lsv_fq_unit').then((u: string | null) => {
      if (u === 'hz' || u === 'khz' || u === 'mhz') setUnitState(u as Unit);
    }).catch(() => {});
  }, [unitProp]);

  useEffect(() => {
    if (visible) {
      setValue(toDisplay(currentHz, unit));
      setTimeout(() => { inputRef.current?.focus(); }, 80);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible, currentHz]);

  const switchUnit = (u: Unit) => {
    const hz = fromDisplay(value, unit);
    setUnitState(u);
    onUnit?.(u);
    AsyncStorage.setItem('lsv_fq_unit', u).catch(() => {});
    if (hz > 0) setValue(toDisplay(hz, u));
  };

  const confirm = () => {
    const hz = fromDisplay(value, unit);
    if (hz >= minHz && hz <= maxHz) { onConfirm(hz); onClose(); }
    Keyboard.dismiss();
  };

  const dimText  = isWhite ? 'rgba(255,255,255,0.45)' : 'rgba(150,100,30,0.65)';
  const unitText = isWhite ? '#b0b8c8' : '#886600';
  const bdrDim   = isWhite ? 'rgba(255,255,255,0.20)' : 'rgba(80,50,0,0.40)';
  const bdrBrt   = isWhite ? 'rgba(255,255,255,0.45)' : 'rgba(160,90,0,0.60)';
  const btnPadY  = isWhite ? 12 : 10;

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}
           onDismiss={() => { if (pendingShare.current) { pendingShare.current = false; onShare?.(); } }}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={st.backdrop} onPress={onClose} />
      <KeyboardAvoidingView
        // Android uses windowSoftInputMode=adjustResize (the window already
        // shrinks above the keyboard) so no behavior here — adding one double-
        // adjusts and makes the box bounce. iOS needs padding.
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
        style={[st.center, { paddingBottom: 16 + kbHeight }]} pointerEvents="box-none"
      >
        <View style={[st.modal, { borderColor: t.barBorder }]}>
          <Text style={[st.title, { color: t.sectionColor, fontFamily: t.font }]}>
            FREQUENCY
          </Text>
          {/* VTS nearby-station skip — relocated from MenuSheet (§4.1). Absent on FM-DX
              (SDRScreen doesn't pass the handlers there — the shared-tuner guard travels). */}
          {onVtsPrev && onVtsNext && (
            <View style={st.vtsRow}>
              <TouchableOpacity style={st.vtsArrow} onPress={onVtsPrev} hitSlop={8}>
                <Text style={[st.vtsArrowText, { color: t.freqColor }]}>◄</Text>
              </TouchableOpacity>
              <View style={st.vtsInfo}>
                <Text style={[st.vtsName, { color: t.btnText, fontFamily: t.font }]} numberOfLines={1}>
                  {vtsName || '—'}
                </Text>
                {vtsFreq != null && (
                  <Text style={[st.vtsFreq, { color: dimText }]}>{(vtsFreq / 1_000_000).toFixed(3)} MHz</Text>
                )}
              </View>
              <TouchableOpacity style={st.vtsArrow} onPress={onVtsNext} hitSlop={8}>
                <Text style={[st.vtsArrowText, { color: t.freqColor }]}>►</Text>
              </TouchableOpacity>
            </View>
          )}
          <View style={[st.inputRow, { borderBottomColor: t.barBorder }]}>
            <TextInput
              ref={inputRef}
              style={[st.input, { color: t.freqColor, fontFamily: t.font }]}
              value={value}
              onChangeText={setValue}
              keyboardType="decimal-pad"
              autoComplete="off"
              autoCorrect={false}
              selectTextOnFocus
              onSubmitEditing={confirm}
              returnKeyType="done"
            />
            <Text style={[st.unitLabel, { color: unitText, fontFamily: t.font }]}>
              {unit === 'hz' ? 'Hz' : unit === 'khz' ? 'kHz' : 'MHz'}
            </Text>
          </View>
          <View style={st.units}>
            {(['hz', 'khz', 'mhz'] as Unit[]).map(u => (
              <TouchableOpacity
                key={u}
                disabled={lockUnit && u !== 'mhz'}
                style={[
                  st.unitBtn,
                  { borderColor: bdrDim, paddingVertical: btnPadY },
                  unit === u && { borderColor: bdrBrt, backgroundColor: t.btnActiveBg },
                  lockUnit && u !== 'mhz' && { opacity: 0.3 },
                ]}
                onPress={() => switchUnit(u)}
              >
                <Text style={[
                  st.unitBtnText,
                  { fontFamily: t.font, color: dimText },
                  unit === u && { color: t.btnActiveText },
                ]}>
                  {u === 'hz' ? 'Hz' : u === 'khz' ? 'kHz' : 'MHz'}
                </Text>
              </TouchableOpacity>
            ))}
          </View>
          {profiles.length > 0 && (
            <View style={st.profiles}>
              <ProfilePicker
                profiles={profiles}
                activeProfileId={activeProfileId}
                sdrUsage={sdrUsage}
                clientCount={clientCount}
                onSelectProfile={onSelectProfile}
                onPicked={onClose}
              />
            </View>
          )}

          <View style={st.actions}>
            <TouchableOpacity
              style={[st.cancelBtn, { borderColor: bdrDim, paddingVertical: btnPadY }]}
              onPress={onClose}
            >
              <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: dimText }}>
                CANCEL
              </Text>
            </TouchableOpacity>
            {onShare && (
              <TouchableOpacity
                style={[st.cancelBtn, { borderColor: bdrDim, paddingVertical: btnPadY }]}
                onPress={() => {
                  if (Platform.OS === 'ios') { pendingShare.current = true; onClose(); }
                  else { onShare(); onClose(); }
                }}
              >
                <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: dimText }}>
                  SHARE
                </Text>
              </TouchableOpacity>
            )}
            <TouchableOpacity
              style={[st.tuneBtn, { borderColor: bdrBrt, paddingVertical: btnPadY }]}
              onPress={confirm}
            >
              <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: t.freqColor, fontWeight: 'bold' }}>
                TUNE ▶
              </Text>
            </TouchableOpacity>
          </View>
        </View>
      </KeyboardAvoidingView>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:     { ...StyleSheet.absoluteFill, backgroundColor: 'rgba(0,0,0,0.58)' },
  // Anchor near the bottom (over the control pill) so it's thumb-reachable on
  // big phones; the auto-opened keyboard then sits just below it.
  center:       { ...StyleSheet.absoluteFill, justifyContent: 'flex-end', alignItems: 'center' },
  modal:        { backgroundColor: 'rgba(8,6,1,0.97)', borderWidth: 1, borderRadius: 12, padding: 20, width: '90%', maxWidth: 360 },
  title:        { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 14 },
  vtsRow:       { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 10 },
  vtsArrow:     { paddingHorizontal: 8, paddingVertical: 2 },
  vtsArrowText: { fontSize: 20, fontWeight: 'bold' },
  vtsInfo:      { flex: 1, alignItems: 'center' },
  vtsName:      { fontSize: 13, fontWeight: 'bold' },
  vtsFreq:      { fontSize: 10, marginTop: 1 },
  inputRow:     { flexDirection: 'row', alignItems: 'flex-end', gap: 6, borderBottomWidth: 1, paddingBottom: 6, marginBottom: 8 },
  input:        { flex: 1, fontSize: 32, letterSpacing: 3, padding: 4, backgroundColor: 'transparent' },
  unitLabel:    { fontSize: 11, letterSpacing: 2, paddingBottom: 6 },
  units:        { flexDirection: 'row', gap: 6, marginBottom: 16 },
  unitBtn:      { flex: 1, borderWidth: 1, borderRadius: 3, alignItems: 'center', backgroundColor: 'transparent' },
  unitBtnText:  { fontSize: 11 },
  actions:      { flexDirection: 'row', gap: 10 },
  profiles:     { marginBottom: 12 },
  cancelBtn:    { flex: 1, borderWidth: 1, borderRadius: 3, alignItems: 'center' },
  tuneBtn:      { flex: 2, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderRadius: 3, alignItems: 'center' },
});
