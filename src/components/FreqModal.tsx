import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  Keyboard, KeyboardAvoidingView, Modal, NativeEventEmitter, NativeModules, Platform,
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
import { useListNav, useKeyboardMode, NAV_FOCUS, revealIn, noteTouchInteraction } from './PanelNav';

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
  /** Flip a bookmark's iCloud opt-in. Absent (Android, or no iCloud) hides the
   *  cloud button entirely — a control that cannot work must not be offered. */
  onToggleBookmarkSync?: (bm: UserBookmark) => void;
  onExportBookmarks?: () => void;
  onImportBookmarks?: (text: string, allInstances: boolean) => string;
  onPickImportFile?:  (allInstances: boolean) => Promise<string>;
}

function toDisplay(hz: number, unit: Unit): string {
  if (unit === 'hz')  return Math.round(hz).toString();
  if (unit === 'khz') return (hz / 1000).toFixed(3);
  return (hz / 1e6).toFixed(6);
}

/**
 * Accept whatever decimal separator the user's keyboard actually gives us.
 *
 * ★ A Dutch keyboard — and most of Europe — puts a COMMA on the decimal pad, and the
 * old `replace(/[^0-9.]/g, '')` simply DELETED it: `100,5` became `1005`, so asking for
 * 100.5 MHz tuned you to 1005 MHz. Silently wrong, which is far worse than refusing the
 * input, and invisible to anyone testing on a UK/US keyboard.
 *
 * We cannot choose which key the system keyboard offers — decimal-pad follows the device
 * locale — so instead we accept the comma and normalise it to a point, both for parsing
 * and in the field itself, so the user sees a `.` whatever their layout.
 *
 * If BOTH separators appear the comma must be a THOUSANDS separator (`1,234.5`, e.g.
 * pasted), so it is dropped rather than treated as the decimal point.
 */
export function normaliseDecimal(s: string): string {
  const hasComma = s.includes(','), hasDot = s.includes('.');
  if (hasComma && hasDot) return s.replace(/,/g, '');
  if (hasComma) return s.replace(/,/g, '.');
  return s;
}

function fromDisplay(val: string, unit: Unit): number {
  const n = parseFloat(normaliseDecimal(val).replace(/[^0-9.]/g, ''));
  if (isNaN(n) || n <= 0) return 0;
  if (unit === 'hz')  return Math.round(n);
  if (unit === 'khz') return Math.round(n * 1000);
  return Math.round(n * 1e6);
}

/**
 * A boxed shortcut letter: [H]z, [T]une.
 *
 * ★ Shown ONLY while that letter actually does something — see `lettersArmed`. That makes the
 * box a live indicator rather than a label: when you tab into the bookmark search the boxes
 * disappear, which answers "why did my key do nothing" before it is asked. (Stuart's idea.)
 */
function KeyCap({ letter, color, label, font, textStyle }: {
  letter: string; color: string; label: string; font?: string; textStyle?: any;
}) {
  // ★ A VIEW, not a nested <Text>. iOS renders nested Text as attributed-string runs and
  // simply DROPS borderWidth on them — so the letter drew and the box never did, which is
  // exactly what Stuart saw ("the boxes around the keys are not showing"). The cap has to be
  // a real view to have a real border, so the label becomes a row rather than one string.
  return (
    <View style={st.keyCapRow}>
      <View style={[st.keyCap, { borderColor: color }]}>
        <Text style={[textStyle, { color, fontFamily: font }]}>{letter}</Text>
      </View>
      <Text style={[textStyle, { color, fontFamily: font }]}>{label}</Text>
    </View>
  );
}

/** Below this the "keyboard" is an accessory bar, not something to move out of the way. */
const KB_ANCHOR_MIN = 140;

/* ★★★ THE SEARCH SURVIVES THE STATION YOU JUST TRIED.
 *
 *  Tuning a result closed the card and cleared the query, so a search that returns SEVERAL
 *  frequencies for one broadcaster — which is the normal case on shortwave — could only ever be
 *  used once. Stuart: "if I am not able to receive the first frequency I choose then I can click
 *  the frequency input and the list still remains so I can click the next and the next after
 *  that." Voice of Korea is the example: half a dozen frequencies, most of them inaudible from
 *  any given place at any given hour, and you find the one that works by trying them.
 *
 *  ★ MODULE SCOPE, not component state, because the card resets itself every time it opens — the
 *    whole point is to outlive that.
 *  ★★ IT EXPIRES, so the card does not silently change what it does. Reopening the frequency box
 *     an hour later must give you a frequency box; reopening it while you are still working
 *     through a list must give you the list. The window restarts each time you come back, so a
 *     long hunt never times out mid-hunt — only walking away ends it.
 *  ★ Cleared when the query is emptied by hand: that is the user saying they are finished with it.
 */
const SEARCH_STICKY_MS = 90_000;
let stickySearch: { q: string; at: number } | null = null;

/** The short provenance tag at the end of a search row — see the note where it is drawn.
 *  ★ 'SERVER' rather than a guess when the backend does not distinguish saved from heard: UberSDR,
 *    OWRX and Kiwi send no such flag, and inventing one would be a label that is sometimes a lie. */
function srcTag(b?: ServerBookmark): string {
  if (!b) return '';
  if (b.source === 'user') return 'YOURS';
  if (b.source === 'eibi') return 'EiBi';
  if (b.manual === true)  return 'SAVED';
  if (b.manual === false) return 'HEARD';
  return 'SERVER';
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
  onAddBookmark, onDeleteBookmark, onToggleBookmarkSync, onExportBookmarks, onImportBookmarks, onPickImportFile,
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
  const searchRef         = useRef<TextInput>(null);
  const bmNameRef         = useRef<TextInput>(null);

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
  const onChangeValue = (raw: string) => {
    // ★ Normalise AS THEY TYPE, not just on parse, so the field shows a `.` even on a
    // keyboard whose decimal key is a comma — which is what the user actually asked for.
    // ★★ And STRIP anything that is not a digit or a point. keyboardType only constrains the
    // ON-SCREEN keyboard; a hardware keyboard can type whatever it likes, so the field
    // happily accepted letters. A frequency has no letters in it. (Stuart, 2026-07-25.)
    const v = normaliseDecimal(raw).replace(/[^0-9.]/g, '');
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
      // ★ Come back to the list you were working through — see stickySearch.
      const sticky = stickySearch && (Date.now() - stickySearch.at) < SEARCH_STICKY_MS
        ? stickySearch : null;
      if (sticky) {
        sticky.at = Date.now();          // the window restarts on every return, not on the first
        setCardMode('bookmarks'); setSearchQuery(sticky.q);
      } else {
        stickySearch = null;
        setCardMode('tune'); setSearchQuery('');
      }
      setBmImportOpen(false); setBmImportMsg('');
      // ★★★ DO NOT RAISE THE KEYBOARD ON OPEN. This card is not a frequency box any more — it
      // houses frequency entry, the VTS bookmarks AND the bookmark search, so it is TALL, and a
      // soft keyboard arriving with it pushed the top of the card off the screen in landscape.
      // It read as broken when it was only the compromise for screen height (Stuart, 2026-08-02:
      // "makes the app look broken when it is simply the compromise for screen size"). Tapping a
      // field brings the keyboard up, and the card moves to meet it — which is also how the
      // search and bookmark-name fields have always behaved, so the card is now consistent with
      // itself whichever of its three jobs you came for.
      // ★★ NO EXCEPTION, INCLUDING FOR HARDWARE KEYBOARDS. I had this focus automatically when the
      //   user was driving by keyboard, on the grounds that focusing raises nothing on a Mac.
      //   Stuart, twice: "we want to click the field." The card opens inert and every route into
      //   it is the same one — which is also the only version that cannot surprise anybody.
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible, currentHz]);

  // Read through a ref so the key listener never captures a stale `value`/`unit`.
  const switchUnitRef = useRef<(u: Unit) => void>(() => {});
  const switchUnit = (u: Unit) => {
    const hz = fromDisplay(value, unit);
    setUnitState(u);
    onUnit?.(u);
    AsyncStorage.setItem('lsv_fq_unit', u).catch(() => {});
    if (hz > 0) setValue(toDisplay(hz, u));
  };
  switchUnitRef.current = switchUnit;

  // ★ Guarded, because Enter can now arrive from two places at once — the hardware key
  // mirrored by VibeKeyWindow, and onSubmitEditing from the on-screen keyboard. Tuning
  // twice on one press would be a real (if brief) double retune of the server.
  const confirming = useRef(false);
  const confirm = () => {
    if (confirming.current) return;
    confirming.current = true;
    setTimeout(() => { confirming.current = false; }, 0);
    const hz = fromDisplay(value, unit);
    if (hz >= minHz && hz <= maxHz) { onConfirm(hz); onClose(); }
    Keyboard.dismiss();
  };

  // ★★ A HARDWARE Enter has to come from the NATIVE key stream, not from the TextInput.
  // The field is keyboardType="decimal-pad", which has no return key for RN to map, so
  // onSubmitEditing never fires — and onKeyPress did not save it either, because
  // VibeKeyWindow was swallowing Enter before the field ever saw it. The window now
  // mirrors Enter through while typing, so listening here is what actually works.
  // (Stuart: "the enter button isnt working on frequency typing still".)
  useEffect(() => {
    if (!visible) return;
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      // ★★ ONLY ON THE TUNE CARD. This card has two Enter listeners — this one, which
      // commits a typed frequency, and the bookmarks pane's, which activates whatever the
      // focus is on. Ungated, both fired: highlighting the bookmarks SEARCH BOX and pressing
      // Enter tuned and closed the card instead of entering the field. Stuart: "it closes the
      // card down like I've just pressed enter to tune." It was doing exactly that.
      if (cardModeRef.current !== 'tune') return;
      if (e?.key === 'Enter') confirmRef.current();
    });
    return () => sub.remove();
  }, [visible]);

  // ★ TAB SWITCHES THE CARD (Stuart: "add the tab bar switching to the frequency box").
  // Tab is the obvious key for a tab bar and has no other job in this card, so it needs no
  // modifier and cannot collide with typing — the native side withholds it from us whenever
  // a field has focus anyway.
  // ★★ LETTER SHORTCUTS, not Tab (Stuart). Tab was the wrong key: with the frequency field
  // focused it moves the caret inside the box rather than reaching us. Letters are free here
  // because a frequency has no letters in it — the native side hands them over precisely
  // because the focused field is a number pad.
  //
  //   [T]une / [B]ookmarks · [H]z / [K]Hz / [M]Hz
  //
  // ★ And the boxes are a LIVE INDICATOR, not decoration: they show only while the letters
  // actually work, so when you are typing into the bookmark search they vanish. That answers
  // "why did my key do nothing" before it is asked.
  // ★ GLOBAL keyboard mode, not local state. You press Enter to OPEN this card, so a local
  // flag was always one keypress behind and the caps only appeared after an arrow — exactly
  // what Stuart reported. Asking the app-wide flag means they are there the moment it opens.
  const kbSeen = useKeyboardMode();
  const lettersArmed = visible && cardMode === 'tune';
  const showKeyCaps = kbSeen && lettersArmed && !lockUnit;
  useEffect(() => {
    if (!visible) return;
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      const k = e?.key;
      if (hasBookmarks && (k === 'T' || k === 'B')) {
        const next = k === 'T' ? 'tune' : 'bookmarks';
        if (next === 'bookmarks') Keyboard.dismiss();
        setCardMode(next);
        return;
      }
      // Unit keys only while the tune card is up — in bookmarks you are typing a search.
      if (cardMode !== 'tune' || lockUnit) return;
      if (k === 'H') switchUnitRef.current('hz');
      else if (k === 'K') switchUnitRef.current('khz');
      else if (k === 'M') switchUnitRef.current('mhz');
    });
    return () => sub.remove();
  }, [visible, hasBookmarks, cardMode, lockUnit]);

  // Bookmark results: arrow through them and Enter to tune. Up/Down reach us even while the
  // search box has focus (see typingPassthrough in AppDelegate), so you can type to filter
  // and step straight down into the list without dismissing the keyboard first.
  // ★ ONE ordered focus space for the WHOLE bookmarks pane, not just the results. Slots are
  // claimed during render in JSX order — the search field, each result, the EiBi toggle, the
  // name field, the save row, each saved bookmark, the transfer buttons — so the order is
  // whatever is actually on screen and cannot drift from it.
  //
  // The LENGTH comes from state rather than the ref, for the same reason as the server
  // picker: the bank is cleared at the top of this render and only filled while the children
  // render, so reading it here would always give zero and focus would stop at the first item.
  const bmSlots = useRef<Array<() => void>>([]);
  const bmViews = useRef<any[]>([]);
  bmSlots.current = [];
  const [bmCount, setBmCount] = useState(0);
  useEffect(() => {
    if (bmSlots.current.length !== bmCount) setBmCount(bmSlots.current.length);
  });

  // ★ The pane SCROLLS — a search for something common comes back with dozens of results —
  // so focus has to drag the view with it. Without a reveal the caret walked off the bottom
  // and the list stayed put: Stuart searched "china", got 62 results, and could reach none of
  // them past the first screenful. Same measured reveal the menus use.
  const bmScrollRef = useRef<ScrollView | null>(null);
  const bmScrollY   = useRef(0);

  const bmNavFocus = useListNav(
    visible && cardMode === 'bookmarks',
    bmCount,
    (i) => bmSlots.current[i]?.(),
    (i) => revealIn(bmScrollRef, { current: bmViews.current[i] }, -1, 90, bmScrollY),
  );

  /**
   * Claim the next slot. Returns whether it has focus, and a ref to attach so reveal can
   * measure it. Render order = JSX order.
   */
  const bmSlot = (onPress: () => void) => {
    const i = bmSlots.current.length;
    bmSlots.current.push(onPress);
    return { on: bmNavFocus === i, ref: (r: any) => { bmViews.current[i] = r; } };
  };
  /** A bookmarks-pane button that takes part in that order. */
  const BmBtn = ({ onPress, style, children, ...rest }: any) => {
    const { on, ref } = bmSlot(onPress ?? (() => {}));
    return (
      <TouchableOpacity ref={ref} onPress={onPress}
        style={[style, on && { borderColor: NAV_FOCUS, borderWidth: 2 }]} {...rest}>
        {children}
      </TouchableOpacity>
    );
  };

  // Read through a ref so the listener above never captures a stale `value`.
  const confirmRef = useRef(confirm); confirmRef.current = confirm;
  // Read through a ref: the listener above is installed once, on `visible` alone.
  const cardModeRef = useRef(cardMode); cardModeRef.current = cardMode;

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
          // ★★ CENTRED WHENEVER THERE IS NO KEYBOARD — not just on the bookmarks tab.
          // This read `kbHeight === 0 && cardMode === 'bookmarks'`, so with no keyboard up the TUNE
          // card fell to flex-end and sat at the bottom over the controls; switching to bookmarks
          // centred it and switching back dropped it again. On a Mac or Android, where no soft
          // keyboard ever appears, that was its permanent resting place (Stuart, 2026-08-02).
          // ★ The card should start where bookmarks starts and MOVE only in response to the
          //   keyboard — so the condition is about the keyboard alone, which is the only thing
          //   that has ever justified moving it.
          // ★★★ A "Done" BAR IS NOT A KEYBOARD. On a Mac — and on an iPad with a hardware
          //     keyboard — focusing the field raises no on-screen keyboard at all, only iOS's
          //     accessory bar, and that still fires a keyboard-show event with a height of about
          //     40-60 pt. Testing `kbHeight === 0` treated that as "keyboard up" and dropped the
          //     card to the bottom, onto the controls, with nothing occupying the space it had
          //     just vacated (Stuart, 2026-08-02: "no on screen keyboard to anchor to").
          //     ★ So the test is whether the keyboard is BIG ENOUGH TO BE WORTH AVOIDING. A real
          //     soft keyboard is 250 pt and up even on the smallest phone in landscape; an
          //     accessory bar never approaches 140. Anything below that leaves the card centred,
          //     which is where it belongs when nothing is covering the bottom of the screen.
          justifyContent: kbHeight > KB_ANCHOR_MIN ? 'flex-end' : 'center',
          paddingBottom: isLandscape ? kbHeight + 8 : 16 + (Platform.OS === 'android' ? kbHeight : 0),
        }]} pointerEvents="box-none"
      >
        <View style={[st.modal, { borderColor: t.barBorder }]}
              // ★ Touching the card ends keyboard mode, so the [H]z / [T]une caps disappear
              // rather than advertising shortcuts to someone using their thumb. A Modal is its
              // own window, so SDRScreen's root touch sniff never sees this.
              onTouchStart={noteTouchInteraction}>
          {hasBookmarks ? (
            <View style={st.segHeader}>
              {(['tune', 'bookmarks'] as const).map(m => (
                <TouchableOpacity key={m}
                  style={[st.segTab, { borderBottomColor: cardMode === m ? t.freqColor : 'transparent' }]}
                  onPress={() => { setCardMode(m); if (m === 'bookmarks') Keyboard.dismiss(); }} activeOpacity={0.7}>
                  {kbSeen ? (
                    <KeyCap letter={m === 'tune' ? 'T' : 'B'}
                            label={m === 'tune' ? 'UNE' : 'OOKMARKS'}
                            color={cardMode === m ? t.freqColor : dimText}
                            font={t.font} textStyle={st.segTabText} />
                  ) : (
                    <Text style={[st.segTabText, { fontFamily: t.font, color: cardMode === m ? t.freqColor : dimText }]}>
                      {m === 'tune' ? 'TUNE' : 'BOOKMARKS'}
                    </Text>
                  )}
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
              // ★ A hardware Enter is handled by the VibeKeyDown listener above, NOT here.
              // onKeyPress was tried and never fired: VibeKeyWindow was swallowing Enter
              // before the field could see it, so there was nothing to hear.
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
                {showKeyCaps ? (
                  <KeyCap
                    letter={u === 'hz' ? 'H' : u === 'khz' ? 'K' : 'M'}
                    label={u === 'hz' ? 'z' : 'Hz'}
                    color={unit === u ? t.btnActiveText : dimText}
                    font={t.font} textStyle={st.unitBtnText} />
                ) : (
                  <Text style={[
                    st.unitBtnText,
                    { fontFamily: t.font, color: dimText },
                    unit === u && { color: t.btnActiveText },
                  ]}>
                    {u === 'hz' ? 'Hz' : u === 'khz' ? 'kHz' : 'MHz'}
                  </Text>
                )}
              </TouchableOpacity>
            ))}
          </View>
          {profiles.length > 0 && (
            <View style={st.profiles}>
              <ProfilePicker
                active={visible && cardMode === 'tune'}
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
            <ScrollView ref={bmScrollRef} style={{ maxHeight: bmMaxH }}
                        onScroll={(e) => { bmScrollY.current = e.nativeEvent.contentOffset.y; }}
                        scrollEventThrottle={16}
                        keyboardShouldPersistTaps="handled" showsVerticalScrollIndicator>
              {(() => { const { on, ref: slotRef } = bmSlot(() => searchRef.current?.focus()); return (
              <TextInput
                ref={(r: any) => { (searchRef as any).current = r; slotRef(r); }}
                style={[st.searchInput, { color: t.freqColor, fontFamily: t.font,
                                          borderColor: on ? NAV_FOCUS : bdrDim, borderWidth: on ? 2 : 1 }]}
                value={searchQuery} onChangeText={(v: string) => {
                  setSearchQuery(v);
                  // ★ Emptying the box by hand is the user saying they are done with that list.
                  if (!v.trim()) stickySearch = null;
                }}
                placeholder="🔍 Search bookmarks & band plan…" placeholderTextColor={dimText}
                autoCorrect={false} autoCapitalize="none" spellCheck={false} clearButtonMode="while-editing" />
              ); })()}
              {searchQuery.trim().length > 0 && (searchResults.length === 0 ? (
                <Text style={[st.bmMsg, { color: dimText }]}>No results for “{searchQuery.trim()}”</Text>
              ) : (<>
                <Text style={[st.bmHint, { color: dimText }]}>{searchResults.length} result{searchResults.length !== 1 ? 's' : ''} · tap to tune</Text>
                {searchResults.map((r: SearchResult, i: number) => {
                  const tune = () => {
                    // ★ Keep the query rather than clearing it: this is the tap that USED the
                    //   list, which is exactly when you are most likely to want it again.
                    stickySearch = { q: searchQuery, at: Date.now() };
                    if (r.isBand && r.band) onSearchTune?.(r.band.start, r.band.mode, true);
                    else if (r.bm) onSearchTune?.(r.bm.frequency, r.bm.mode);
                    onClose();
                  };
                  const { on, ref: slotRef } = bmSlot(tune);
                  return (
                  <TouchableOpacity key={i} ref={slotRef} activeOpacity={0.7}
                    style={[st.searchRow, on && { backgroundColor: 'rgba(124,255,155,0.16)' }]}
                    onPress={tune}>
                    <Text style={[st.searchFreq, { color: t.freqColor }]}>{r.isBand && r.band ? fmtRange(r.band.start, r.band.end) : fmtFreq(r.bm?.frequency ?? 0)}</Text>
                    <Text style={[st.searchMode, { color: dimText }]}>{r.isBand ? grpAbbr(r.band?.group) : (r.bm?.mode ?? '—').toUpperCase()}</Text>
                    {!r.isBand && r.bm?.name ? <StationLogo name={r.bm.name} itu={r.bm.itu} /> : null}
                    <Text style={[st.searchName, { color: t.btnText }]} numberOfLines={1}>
                      {!r.isBand && r.bm?.flag ? r.bm.flag + ' ' : ''}{r.isBand ? (r.band?.label ?? '') : (r.bm?.name ?? '')}
                    </Text>
                  </TouchableOpacity>
                  );
                })}
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
              {(() => { const { on, ref: slotRef } = bmSlot(() => bmNameRef.current?.focus()); return (
              <TextInput ref={(r: any) => { (bmNameRef as any).current = r; slotRef(r); }}
                style={[st.searchInput, { color: t.freqColor, fontFamily: t.font,
                                          borderColor: on ? NAV_FOCUS : bdrDim, borderWidth: on ? 2 : 1 }]}
                value={bmName} onChangeText={setBmName} placeholder="Bookmark name…" placeholderTextColor={dimText} maxLength={60} autoCorrect={false} />
              ); })()}
              <View style={st.bmSegRow}>
                <BmBtn style={[st.bmSeg, { borderColor: !bmAll ? bdrBrt : bdrDim }]} onPress={() => setBmAll(false)}><Text style={[st.bmSegText, { color: !bmAll ? t.freqColor : dimText }]}>THIS SERVER</Text></BmBtn>
                <BmBtn style={[st.bmSeg, { borderColor: bmAll ? bdrBrt : bdrDim }]} onPress={() => setBmAll(true)}><Text style={[st.bmSegText, { color: bmAll ? t.freqColor : dimText }]}>ALL SERVERS</Text></BmBtn>
              </View>
              {/* ★★★ A BUTTON THAT DOES NOTHING MUST LOOK LIKE IT. The press was guarded by
                  `if (!bmName.trim()) return` and nothing else, so with the name box empty the
                  button lit, took the tap and silently did nothing — and the field above it is the
                  SEARCH box, which is where a name naturally gets typed first. The whole feature
                  then reads as broken: "the app itself wasn't saving bookmarks either" (Stuart,
                  2026-08-15, with "radio caroline" in the search box and the name box empty).
                  ★ Dimmed and disabled, the same shape as the admin TAKE OVER button, so the
                    reason is visible before the tap rather than inferred from nothing happening. */}
              <BmBtn style={[st.bmBtn, { borderColor: bmName.trim() ? bdrBrt : bdrDim }]}
                     disabled={!bmName.trim()}
                     onPress={() => { if (!bmName.trim()) return; onAddBookmark?.(bmName, bmAll); setBmName(''); }}>
                <Text style={[st.bmBtnText, { color: t.freqColor },
                              !bmName.trim() && { opacity: 0.4 }]}>★ SAVE BOOKMARK</Text>
              </BmBtn>

              <Text style={[st.bmSub, { color: dimText }]}>Saved ({userBookmarks.length})</Text>
              {userBookmarks.length === 0 && <Text style={[st.bmMsg, { color: dimText }]}>No bookmarks yet — tune somewhere good and save it.</Text>}
              {userBookmarks.map((b: UserBookmark, i: number) => (
                <View key={`${b.name}|${b.frequency}|${i}`} style={st.bmSaveRow}>
                  <BmBtn style={{ flex: 1 }} activeOpacity={0.7} onPress={() => { onSearchTune?.(b.frequency, b.mode); onClose(); }}>
                    <Text style={[st.bmName2, { color: t.freqColor }]} numberOfLines={1}>{b.name}</Text>
                    <Text style={[st.bmFreq2, { color: dimText }]}>{fmtFreq(b.frequency)}  {b.mode.toUpperCase()}</Text>
                  </BmBtn>
                  {!!onToggleBookmarkSync && (
                    <TouchableOpacity hitSlop={8} onPress={() => onToggleBookmarkSync(b)}
                      accessibilityLabel={b.synced ? `Stop syncing ${b.name} to iCloud` : `Sync ${b.name} to iCloud`}>
                      <Text style={[st.bmCloud, { color: b.synced ? t.freqColor : dimText,
                                                  opacity: b.synced ? 1 : 0.45 }]}>
                        {/* Distinct GLYPHS, not just a colour: a dim cloud and a
                            bright one are the same shape, and "is this one
                            syncing?" is exactly the question the row has to
                            answer at a glance. */}
                        {b.synced ? '\u2601\u2713' : '\u2601'}
                      </Text>
                    </TouchableOpacity>
                  )}
                  <TouchableOpacity hitSlop={8} onPress={() => onDeleteBookmark?.(b)}><Text style={[st.bmDel, { color: dimText }]}>✕</Text></TouchableOpacity>
                </View>
              ))}

              <Text style={[st.bmSub, { color: dimText }]}>Transfer</Text>
              <View style={st.bmSegRow}>
                <BmBtn style={[st.bmSeg, { borderColor: bdrDim }]} onPress={onExportBookmarks}><Text style={[st.bmSegText, { color: dimText }]}>⇧ EXPORT JSON</Text></BmBtn>
                <BmBtn style={[st.bmSeg, { borderColor: bmImportOpen ? bdrBrt : bdrDim }]} onPress={() => { setBmImportOpen(p => !p); setBmImportMsg(''); }}><Text style={[st.bmSegText, { color: bmImportOpen ? t.freqColor : dimText }]}>⇩ PASTE</Text></BmBtn>
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
  // Small enough to sit inside a label without changing its metrics — the boxes must not
  // reflow the row as they appear and disappear.
  keyCapRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  keyCap: {
    borderWidth: 1, borderRadius: 3, paddingHorizontal: 3, marginRight: 1,
    alignItems: 'center', justifyContent: 'center',
  },
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
  bmCloud:      { fontSize: 14, paddingHorizontal: 6 },
});
