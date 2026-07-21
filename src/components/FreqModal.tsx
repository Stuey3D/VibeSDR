import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  Keyboard, KeyboardAvoidingView, Modal, Platform,
  Pressable, StyleSheet, Text, TextInput, TouchableOpacity, useWindowDimensions, View,
} from 'react-native';
import { ScrollView } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { MAX_FREQ_HZ, MIN_FREQ_HZ } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import ProfilePicker from './ProfilePicker';
import StationLogo from './StationLogo';
import {
  searchStations, fmtFreq, fmtRange, grpAbbr,
  type ServerBookmark, type ServerBand, type SearchResult,
} from '../services/stations';
import { type UserBookmark } from '../services/userBookmarks';

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
  /** Live nearby-station lookup — a nice side-effect of the move: as you TYPE a frequency the
   *  VTS row updates to the nearest station to the DRAFT value, before you've tuned. Falls back
   *  to the tuned station (vtsName/vtsFreq) when the field is untouched. */
  vtsLookup?: (hz: number) => { name: string; freq: number } | null;

  /** BOOKMARKS mode (relocated from MenuSheet §4.2). When these are supplied the card gains a
   *  Tune | Bookmarks segmented header. All lifted verbatim from MenuSheet. */
  currentMode?:      string;
  onSearchTune?:     (hz: number, mode?: string | null, isBand?: boolean) => void;
  searchBookmarks?:  ServerBookmark[];
  searchBands?:      ServerBand[];
  eibiEnabled?:      boolean;
  onEibiToggle?:     (on: boolean) => void;
  userBookmarks?:    UserBookmark[];
  onAddBookmark?:    (name: string, allInstances: boolean) => void;
  onDeleteBookmark?: (bm: UserBookmark) => void;
  onExportBookmarks?: () => void;
  onImportBookmarks?: (text: string, allInstances: boolean) => string;
  onPickImportFile?:  (allInstances: boolean) => Promise<string>;
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
  vtsName, vtsFreq, onVtsPrev, onVtsNext, vtsLookup,
  currentMode = 'usb', onSearchTune, searchBookmarks = [], searchBands = [],
  eibiEnabled = true, onEibiToggle, userBookmarks = [],
  onAddBookmark, onDeleteBookmark, onExportBookmarks, onImportBookmarks, onPickImportFile,
}: FreqModalProps) {
  const hasBookmarks = !!onSearchTune;   // bookmarks mode available
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  const { width: winW, height: winH } = useWindowDimensions();
  const isLandscape = winW > winH;
  const [unitState, setUnitState] = useState<Unit>('khz');
  const unit = unitProp ?? unitState;
  const [value, setValue] = useState('');
  // Live VTS while typing (see vtsLookup). null → show the tuned station instead.
  const [draftVts, setDraftVts] = useState<{ name: string; freq: number } | null>(null);
  const inputRef          = useRef<TextInput>(null);

  // ── Bookmarks mode (relocated from MenuSheet §4.2) ──────────────────────────
  const [cardMode, setCardMode]         = useState<'tune' | 'bookmarks'>('tune');
  const [searchQuery, setSearchQuery]   = useState('');
  const [bmName, setBmName]             = useState('');
  const [bmAll, setBmAll]               = useState(false);
  const [bmImportOpen, setBmImportOpen] = useState(false);
  const [bmImportText, setBmImportText] = useState('');
  const [bmImportMsg, setBmImportMsg]   = useState('');
  const searchResults = useMemo(
    () => searchStations(searchBookmarks, searchBands, searchQuery),
    [searchBookmarks, searchBands, searchQuery],
  );

  // As the user types, resolve the nearest station to the DRAFT frequency.
  const onChangeValue = (v: string) => {
    setValue(v);
    if (!vtsLookup) return;
    const hz = fromDisplay(v, unit);
    setDraftVts(hz > 0 ? vtsLookup(hz) : null);
  };
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
    // Track on BOTH platforms now: landscape positions the modal a fixed gap above the
    // MEASURED keyboard (iOS's auto-padding over-lifted it in landscape — clipped the top
    // while leaving a gap below). iOS keyboardWillShow is smoother than DidShow.
    const showEvt = Platform.OS === 'ios' ? 'keyboardWillShow' : 'keyboardDidShow';
    const hideEvt = Platform.OS === 'ios' ? 'keyboardWillHide' : 'keyboardDidHide';
    const show = Keyboard.addListener(showEvt, e => setKbHeight(e.endCoordinates.height));
    const hide = Keyboard.addListener(hideEvt, () => setKbHeight(0));
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
      setDraftVts(null);   // start from the tuned station; typing takes over
      setCardMode('tune'); setSearchQuery(''); setBmImportOpen(false); setBmImportMsg('');
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
  // Bookmarks list cap (§6.4/§6.5): size to the space actually available above the keyboard, so a
  // big phone (17PM) shows every button with no scroll, while small phones cap-and-scroll. Chrome =
  // card padding + segmented header + top safe gap + the gap above the keyboard.
  const bmMaxH = Math.max(160, winH - kbHeight - (isLandscape ? 96 : 150));

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}
           onDismiss={() => { if (pendingShare.current) { pendingShare.current = false; onShare?.(); } }}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={st.backdrop} onPress={onClose} />
      <KeyboardAvoidingView
        // LANDSCAPE: position manually a small gap above the measured keyboard — iOS's
        // auto-padding over-lifts a tall modal in landscape (clips the top, gap below).
        // PORTRAIT: unchanged — iOS padding behavior; Android adjustResize already shrinks
        // the window (adding behavior double-adjusts and bounces).
        behavior={Platform.OS === 'ios' && !isLandscape ? 'padding' : undefined}
        style={[st.center, {
          // Two rest states (§6.4): with NO keyboard (e.g. Bookmarks tab before you tap a field)
          // CENTRE the card so a tall list isn't shoved off the bottom; with the keyboard up,
          // anchor it just above the keyboard.
          justifyContent: kbHeight === 0 && cardMode === 'bookmarks' ? 'center' : 'flex-end',
          paddingBottom: isLandscape ? kbHeight + 8 : 16 + (Platform.OS === 'android' ? kbHeight : 0),
        }]} pointerEvents="box-none"
      >
        <View style={[st.modal, { borderColor: t.barBorder }]}>
          {hasBookmarks ? (
            <View style={st.segHeader}>
              {(['tune', 'bookmarks'] as const).map(m => (
                <TouchableOpacity key={m}
                  style={[st.segTab, { borderBottomColor: cardMode === m ? t.freqColor : 'transparent' }]}
                  onPress={() => { setCardMode(m); if (m === 'bookmarks') Keyboard.dismiss(); }} activeOpacity={0.7}>
                  <Text style={[st.segTabText, { fontFamily: t.font, color: cardMode === m ? t.freqColor : dimText }]}>
                    {m === 'tune' ? 'TUNE' : 'BOOKMARKS'}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
          ) : (
            <Text style={[st.title, { color: t.sectionColor, fontFamily: t.font }]}>FREQUENCY</Text>
          )}

          {cardMode === 'tune' && (<>
          {/* VTS nearby-station skip — relocated from MenuSheet (§4.1). Absent on FM-DX
              (SDRScreen doesn't pass the handlers there — the shared-tuner guard travels). */}
          {onVtsPrev && onVtsNext && (
            <View style={st.vtsRow}>
              <TouchableOpacity style={st.vtsArrow} onPress={onVtsPrev} hitSlop={8}>
                <Text style={[st.vtsArrowText, { color: t.freqColor }]}>◄</Text>
              </TouchableOpacity>
              <View style={st.vtsInfo}>
                <Text style={[st.vtsName, { color: t.btnText, fontFamily: t.font }]} numberOfLines={1}>
                  {(draftVts?.name ?? vtsName) || '—'}
                </Text>
                {(draftVts ? draftVts.freq : vtsFreq) != null && (
                  <Text style={[st.vtsFreq, { color: dimText }]}>
                    {((draftVts ? draftVts.freq : vtsFreq)! / 1_000_000).toFixed(3)} MHz
                  </Text>
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
              onChangeText={onChangeValue}
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
          </>)}

          {/* BOOKMARKS mode — search + band plan, EiBi, add current, saved list, transfer.
              Lifted verbatim from MenuSheet (§4.2). */}
          {cardMode === 'bookmarks' && (
            <ScrollView style={{ maxHeight: bmMaxH }} keyboardShouldPersistTaps="handled" showsVerticalScrollIndicator>
              <TextInput
                style={[st.searchInput, { color: t.freqColor, fontFamily: t.font, borderColor: bdrDim }]}
                value={searchQuery} onChangeText={setSearchQuery}
                placeholder="🔍 Search bookmarks & band plan…" placeholderTextColor={dimText}
                autoCorrect={false} autoCapitalize="none" spellCheck={false} clearButtonMode="while-editing" />
              {searchQuery.trim().length > 0 && (searchResults.length === 0 ? (
                <Text style={[st.bmMsg, { color: dimText }]}>No results for “{searchQuery.trim()}”</Text>
              ) : (<>
                <Text style={[st.bmHint, { color: dimText }]}>{searchResults.length} result{searchResults.length !== 1 ? 's' : ''} · tap to tune</Text>
                {searchResults.map((r: SearchResult, i: number) => (
                  <TouchableOpacity key={i} style={st.searchRow} activeOpacity={0.7}
                    onPress={() => {
                      setSearchQuery('');
                      if (r.isBand && r.band) onSearchTune?.(r.band.start, r.band.mode, true);
                      else if (r.bm) onSearchTune?.(r.bm.frequency, r.bm.mode);
                      onClose();
                    }}>
                    <Text style={[st.searchFreq, { color: t.freqColor }]}>{r.isBand && r.band ? fmtRange(r.band.start, r.band.end) : fmtFreq(r.bm?.frequency ?? 0)}</Text>
                    <Text style={[st.searchMode, { color: dimText }]}>{r.isBand ? grpAbbr(r.band?.group) : (r.bm?.mode ?? '—').toUpperCase()}</Text>
                    {!r.isBand && r.bm?.name ? <StationLogo name={r.bm.name} itu={r.bm.itu} /> : null}
                    <Text style={[st.searchName, { color: t.btnText }]} numberOfLines={1}>
                      {!r.isBand && r.bm?.flag ? r.bm.flag + ' ' : ''}{r.isBand ? (r.band?.label ?? '') : (r.bm?.name ?? '')}
                    </Text>
                  </TouchableOpacity>
                ))}
              </>))}

              {onEibiToggle && (
                <View style={st.bmToggleRow}>
                  <Text style={[st.bmSub, { color: dimText, marginTop: 0 }]}>EiBi SCHEDULE</Text>
                  <TouchableOpacity onPress={() => onEibiToggle(!eibiEnabled)} hitSlop={8}
                    style={[st.bmToggle, { borderColor: eibiEnabled ? bdrBrt : bdrDim, backgroundColor: eibiEnabled ? t.btnActiveBg : 'transparent' }]}>
                    <Text style={{ color: eibiEnabled ? t.btnActiveText : dimText, fontFamily: t.font, fontSize: 11 }}>{eibiEnabled ? 'ON' : 'OFF'}</Text>
                  </TouchableOpacity>
                </View>
              )}

              <Text style={[st.bmSub, { color: dimText }]}>Add: {(currentHz / 1_000_000).toFixed(4)} MHz {currentMode.toUpperCase()}</Text>
              <TextInput style={[st.searchInput, { color: t.freqColor, fontFamily: t.font, borderColor: bdrDim }]}
                value={bmName} onChangeText={setBmName} placeholder="Bookmark name…" placeholderTextColor={dimText} maxLength={60} autoCorrect={false} />
              <View style={st.bmSegRow}>
                <TouchableOpacity style={[st.bmSeg, { borderColor: !bmAll ? bdrBrt : bdrDim }]} onPress={() => setBmAll(false)}><Text style={[st.bmSegText, { color: !bmAll ? t.freqColor : dimText }]}>THIS SERVER</Text></TouchableOpacity>
                <TouchableOpacity style={[st.bmSeg, { borderColor: bmAll ? bdrBrt : bdrDim }]} onPress={() => setBmAll(true)}><Text style={[st.bmSegText, { color: bmAll ? t.freqColor : dimText }]}>ALL SERVERS</Text></TouchableOpacity>
              </View>
              <TouchableOpacity style={[st.bmBtn, { borderColor: bdrBrt }]} onPress={() => { if (!bmName.trim()) return; onAddBookmark?.(bmName, bmAll); setBmName(''); }}>
                <Text style={[st.bmBtnText, { color: t.freqColor }]}>★ SAVE BOOKMARK</Text>
              </TouchableOpacity>

              <Text style={[st.bmSub, { color: dimText }]}>Saved ({userBookmarks.length})</Text>
              {userBookmarks.length === 0 && <Text style={[st.bmMsg, { color: dimText }]}>No bookmarks yet — tune somewhere good and save it.</Text>}
              {userBookmarks.map((b: UserBookmark, i: number) => (
                <View key={`${b.name}|${b.frequency}|${i}`} style={st.bmSaveRow}>
                  <TouchableOpacity style={{ flex: 1 }} activeOpacity={0.7} onPress={() => { onSearchTune?.(b.frequency, b.mode); onClose(); }}>
                    <Text style={[st.bmName2, { color: t.freqColor }]} numberOfLines={1}>{b.name}</Text>
                    <Text style={[st.bmFreq2, { color: dimText }]}>{fmtFreq(b.frequency)}  {b.mode.toUpperCase()}</Text>
                  </TouchableOpacity>
                  <TouchableOpacity hitSlop={8} onPress={() => onDeleteBookmark?.(b)}><Text style={[st.bmDel, { color: dimText }]}>✕</Text></TouchableOpacity>
                </View>
              ))}

              <Text style={[st.bmSub, { color: dimText }]}>Transfer</Text>
              <View style={st.bmSegRow}>
                <TouchableOpacity style={[st.bmSeg, { borderColor: bdrDim }]} onPress={onExportBookmarks}><Text style={[st.bmSegText, { color: dimText }]}>⇧ EXPORT JSON</Text></TouchableOpacity>
                <TouchableOpacity style={[st.bmSeg, { borderColor: bmImportOpen ? bdrBrt : bdrDim }]} onPress={() => { setBmImportOpen(p => !p); setBmImportMsg(''); }}><Text style={[st.bmSegText, { color: bmImportOpen ? t.freqColor : dimText }]}>⇩ PASTE</Text></TouchableOpacity>
              </View>
              {onPickImportFile && (
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrDim }]} onPress={async () => { const msg = await onPickImportFile(bmAll); if (msg) { setBmImportMsg(msg); setBmImportOpen(false); } }}>
                  <Text style={[st.bmBtnText, { color: dimText }]}>📁 IMPORT FILE (JSON / YAML)</Text>
                </TouchableOpacity>
              )}
              {!!bmImportMsg && <Text style={[st.bmMsg, { color: dimText }]}>{bmImportMsg}</Text>}
              {bmImportOpen && (<>
                <TextInput style={[st.searchInput, st.bmImportBox, { color: t.freqColor, fontFamily: t.font, borderColor: bdrDim }]}
                  value={bmImportText} onChangeText={setBmImportText} placeholder="Paste UberSDR bookmarks (JSON or YAML) here…" placeholderTextColor={dimText} autoCorrect={false} autoCapitalize="none" multiline />
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrBrt }]} onPress={() => { const msg = onImportBookmarks?.(bmImportText, bmAll) ?? ''; setBmImportMsg(msg); if (msg.startsWith('Imported')) setBmImportText(''); }}>
                  <Text style={[st.bmBtnText, { color: t.freqColor }]}>CONFIRM IMPORT</Text>
                </TouchableOpacity>
              </>)}
              <View style={{ height: 12 }} />
            </ScrollView>
          )}
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
  // Bookmarks mode (§4.2)
  segHeader:    { flexDirection: 'row', marginBottom: 12 },
  segTab:       { flex: 1, alignItems: 'center', paddingVertical: 8, borderBottomWidth: 2 },
  segTabText:   { fontSize: 12, letterSpacing: 2, fontWeight: '700' },
  bmScroll:     { maxHeight: 340 },
  searchInput:  { borderWidth: 1, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 8, fontSize: 14, marginBottom: 8, backgroundColor: 'rgba(255,255,255,0.04)' },
  bmImportBox:  { minHeight: 64, textAlignVertical: 'top' },
  bmMsg:        { fontSize: 12, marginBottom: 8, fontStyle: 'italic' },
  bmHint:       { fontSize: 10, letterSpacing: 1, marginBottom: 4 },
  searchRow:    { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 7, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.10)' },
  searchFreq:   { fontSize: 12, fontWeight: '700', minWidth: 70 },
  searchMode:   { fontSize: 10, minWidth: 34 },
  searchName:   { flex: 1, fontSize: 12 },
  bmToggleRow:  { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 8, marginBottom: 4 },
  bmToggle:     { paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6, borderWidth: 1 },
  bmSub:        { fontSize: 10, letterSpacing: 1, marginTop: 12, marginBottom: 5 },
  bmSegRow:     { flexDirection: 'row', gap: 6, marginBottom: 8 },
  bmSeg:        { flex: 1, borderWidth: 1, borderRadius: 4, paddingVertical: 8, alignItems: 'center' },
  bmSegText:    { fontSize: 11, fontWeight: '600' },
  bmBtn:        { borderWidth: 1, borderRadius: 4, paddingVertical: 9, alignItems: 'center', marginBottom: 8 },
  bmBtnText:    { fontSize: 12, fontWeight: '700' },
  bmSaveRow:    { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.10)' },
  bmName2:      { fontSize: 13, fontWeight: '600' },
  bmFreq2:      { fontSize: 10, marginTop: 1 },
  bmDel:        { fontSize: 16, paddingHorizontal: 6 },
});
