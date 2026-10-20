import React, { useDeferredValue, useEffect, useMemo, useRef, useState } from 'react';
import {
  Keyboard, KeyboardAvoidingView, Modal, Platform,
  Pressable, StyleSheet, Text, TextInput, TouchableOpacity, useWindowDimensions, View,
} from 'react-native';
import { ActivityIndicator, ScrollView } from 'react-native';
import { isMacHost } from '../services/macAudio';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { MAX_FREQ_HZ, MIN_FREQ_HZ } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import ProfilePicker from './ProfilePicker';
import StationLogo from './StationLogo';
import {
  searchStations, fmtFreq, fmtRange, grpAbbr,
  type ServerBookmark, type ServerBand, type SearchResult,
} from '../services/stations';
import { type UserBookmark, bookmarkPassband } from '../services/userBookmarks';
import { airbandEntry } from '../utils/airband';
import { useListNav, revealIn, noteTouchInteraction, useNavFocus } from './PanelNav';
import { addKeyListener } from '../utils/hardwareKeys';
import { tuneCardItems, type TuneCardKey } from '../utils/freqCardNav';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, PopupScrim, PopupWindow, POPUP_FONT, type PopupTokens,
  scrollLane, scrollLaneOutset,
} from './PopupShell';
import { toSegCells } from '../constants/displayText';

type Unit = 'hz' | 'khz' | 'mhz';

/** Today's dim behind the card and the card's own glass — Transparency OFF drops the first
 *  (PopupScrim) and makes the second opaque (usePopupSurface). */
const BACKDROP = 'rgba(0,0,0,0.58)';
const CARD_BG  = 'rgba(8,6,1,0.97)';

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
  /** Said when a typed frequency falls outside minHz–maxHz ("This receiver covers …"). Absent = the
   *  old silent refusal. */
  rangeMsg?: string;
  /** Lock to MHz (FM-DX broadcast) — grey out the Hz/kHz options. */
  lockUnit?: boolean;
  /** Share the current station (moved here from the controls bar). Hidden when
   *  sharing isn't available (undefined). */
  onShare?:  () => void;
  /** ★ Set when this radio's dial is SHARED: Share is shown GREYED with this reason instead of working — a link
   *  must never retune a radio other people are hearing (Stuart, 2026-09-19). */
  shareOffReason?: string;

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
  /** ★★★ OWRX'S MAGIC KEY, under the profiles it unlocks — Stuart's placement: "maybe under the
   *  Profiles dropdown for the tuner box we have a text entry box for magic key which unlocks
   *  OWRX." It is the operator's password for a LOCKED profile (and for centre-frequency changes),
   *  checked server-side as `params.key` — see OwrxAdapter.setMagicKey.
   *  ★ Offered only where there are profiles to unlock, i.e. on OWRX. */
  magicKey?:        string;
  onMagicKey?:      (key: string) => void;

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
  /** `bw` = the bookmark's own passband (bookmarkPassband); omitted = the mode's default. */
  onSearchTune?:     (hz: number, mode?: string | null, isBand?: boolean, voiceStep?: boolean,
                      bw?: [number, number] | null) => void;
  /** ★ A DAB bookmark: tune the multiplex and select the service (web client's dabGoTo). Only
   *  offered when the receiver can do DAB; otherwise a DAB row tunes like any other. */
  onDabTune?:        (hz: number, sid: number) => void;
  /** ★ Opened from the DAB window's ★: the bookmarks tab, showing DAB stations only, as the web
   *  client's `dabBm` does. The filter is a starting point — "show all" is one tap away. */
  dabOnly?:          boolean;
  /** ★★★ MEASURED BY THE PARENT. useSafeAreaInsets returns 0 INSIDE an RN Modal — BrowserOverlay
   *  learned this the hard way (build 70: its bar drew under the clock). This card lives in a
   *  Modal too, so the hook below cannot be trusted on its own and the parent, which is not in a
   *  modal, passes the real value in. Falls back to the hook where nobody supplies it. */
  topInset?:         number;
  searchBookmarks?:  ServerBookmark[];
  /** ★★★ THIS RECEIVER'S OWN STATIONS, on their own, for the "ON THIS SERVER" list.
   *  Passed separately rather than filtered out of `searchBookmarks`, which is the MERGED list
   *  (yours + the server's + EiBi) — picking them back out of it by `source` would be guessing at
   *  the shape of something assembled elsewhere, and would silently follow any change to it. */
  serverBookmarks?:  ServerBookmark[];
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
  /** ★ Owner-only (present when the admin password is held): save on the RECEIVER, for everyone. */
  onAddServerBookmark?:     (name: string) => Promise<string>;
  onImportToServer?:        (text: string, onProgress?: (done: number, total: number) => void) => Promise<string>;
  onPickImportFileToServer?: (onProgress?: (done: number, total: number) => void) => Promise<string>;
  /** ★★★ PICK MODE — the chat's "Check out [Bookmark]" (Stuart, 2026-10-01). Present = the card opens
   *  on BOOKMARKS and a row is CHOSEN, not tuned: search (yours, the receiver's, EiBi) and ON THIS
   *  SERVER work exactly as always, and the pick goes back to the chat as a draft to send. No TUNE tab,
   *  no band-plan ranges (a band is not a station), no add / delete / import — this is a chooser. */
  onPickShare?: (b: { name: string; frequency: number; mode?: string | null;
                      bandwidth_low?: number | null; bandwidth_high?: number | null;
                      sid?: number | null; eid?: number | null }) => void;
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
  unit: unitProp, onUnit, onDabTune, dabOnly, onAddServerBookmark, onImportToServer, onPickImportFileToServer,
  minHz = MIN_FREQ_HZ, maxHz = MAX_FREQ_HZ, rangeMsg, lockUnit = false,
  onShare,
  shareOffReason,
  profiles = [], activeProfileId, sdrUsage, clientCount, onSelectProfile,
  magicKey, onMagicKey,
  vtsName, vtsFreq, onVtsPrev, onVtsNext, vtsLookup,
  currentMode = 'usb', onSearchTune, searchBookmarks = [], serverBookmarks = [], searchBands = [], topInset,
  eibiEnabled = true, onEibiToggle, userBookmarks = [],
  onAddBookmark, onDeleteBookmark, onToggleBookmarkSync, onExportBookmarks, onImportBookmarks, onPickImportFile,
  onPickShare,
}: FreqModalProps) {
  const focusC = useNavFocus();   // ★ focus ring: white on green controls (Stuart, 2026-10-12)
  const picking = !!onPickShare;
  const hasBookmarks = !!onSearchTune;   // bookmarks mode available
  const pt = usePopupTheme();
  const st = usePopupStyles(makeSt);
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(16, false);
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  const { width: winW, height: winH } = useWindowDimensions();
  /* ★★ THE REAL INSETS, not a constant. The card is centred, so whatever it cannot fit overflows
   *  BOTH ends equally — and the top end is where the Dynamic Island is. */
  const hookInsets = useSafeAreaInsets();
  /** ★ The parent's measurement wins; the hook is the fallback. See the topInset prop. */
  const safeTop = topInset ?? hookInsets.top;
  const safeBottom = hookInsets.bottom;
  const isLandscape = winW > winH;
  const [unitState, setUnitState] = useState<Unit>('khz');
  const unit = unitProp ?? unitState;
  const [value, setValue] = useState('');
  const valueRef = useRef(value);   // the field's text as focus lands (Android's select-on-focus, below)
  valueRef.current = value;
  // Live VTS while typing (see vtsLookup). null → show the tuned station instead.
  const [draftVts, setDraftVts] = useState<{ name: string; freq: number } | null>(null);
  const inputRef          = useRef<TextInput>(null);
  const searchRef         = useRef<TextInput>(null);
  const bmNameRef         = useRef<TextInput>(null);

  // ── Bookmarks mode (relocated from MenuSheet §4.2) ──────────────────────────
  const [cardMode, setCardMode]         = useState<'tune' | 'bookmarks'>('tune');
  const [dabFilter, setDabFilter]       = useState(false);
  const [searchQuery, setSearchQuery]   = useState('');
  const [bmName, setBmName]             = useState('');
  const [bmAll, setBmAll]               = useState(false);
  /** ★ Closed by default — see the ON THIS SERVER block. A phone card cannot afford a list that
   *  opens itself, and this one is as long as the receiver has been listening. */
  const [showServerList, setShowServerList] = useState(false);
  const [bmImportOpen, setBmImportOpen] = useState(false);
  const [bmImportText, setBmImportText] = useState('');
  const [bmImportMsg, setBmImportMsg]   = useState('');
  /** ★ THE WORKING INDICATOR for an import to the receiver (Stuart, 2026-10-09: "we need to see a little working icon
   *  to show its not frozen"). A file is written one bookmark per request — 1,478 UK ATC rows take minutes on a Pi —
   *  so it shows a spinner and how far it has got, and the import keys stand down until it finishes (a second press
   *  would post the whole file again alongside the first). */
  const [bmBusy, setBmBusy] = useState<{ done: number; total: number } | null>(null);
  const bmBusyRef = useRef(false);
  const runServerImport = async (go: (p: (done: number, total: number) => void) => Promise<string>, clearText: boolean) => {
    if (bmBusyRef.current) return;
    bmBusyRef.current = true;
    setBmImportMsg('');
    try {
      const msg = await go((done, total) => setBmBusy({ done, total }));
      if (msg) { setBmImportMsg(msg); if (!clearText) setBmImportOpen(false); else if (msg.startsWith('Imported')) setBmImportText(''); }
    } finally { bmBusyRef.current = false; setBmBusy(null); }
  };
  const importFileToServer = () => onPickImportFileToServer && runServerImport((p) => onPickImportFileToServer(p), false);
  const importTextToServer = () => onImportToServer && runServerImport((p) => onImportToServer(bmImportText, p), true);
  /** ★ One tune path for every server row: a DAB service goes through onDabTune when the
   *  receiver can do DAB, everything else through onSearchTune. */
  const tuneBm = (b: ServerBookmark) => {
    if (onPickShare) { onPickShare(b); return; }   // ★ pick mode: chosen, not tuned
    const isDab = (b.mode || '').toLowerCase() === 'dab' && (b.sid ?? -1) >= 0;
    if (isDab && onDabTune) onDabTune(b.frequency, b.sid!);
    else onSearchTune?.(b.frequency, b.mode, false, false, bookmarkPassband(b));
  };
  // ★ Deferred: the keystroke paints first, the thousands-long scan follows when the thread is free.
  const deferredQuery = useDeferredValue(searchQuery);
  const searchResults = useMemo(
    () => {
      const r = searchStations(searchBookmarks, searchBands, deferredQuery);
      return picking ? r.filter((x: SearchResult) => !x.isBand) : r;   // ★ a band is not a station
    },
    [searchBookmarks, searchBands, deferredQuery, picking],
  );

  // As the user types, resolve the nearest station to the DRAFT frequency.
  /* ★★★ ON A MAC THE FIELD IS NOT PRE-SELECTED — the first keystroke replaces it instead. A selection is
   *  what raises macOS's own menu over the field (Ask Siri · Cut/Copy/Paste · Spelling and Grammar ·
   *  Speech) and eats the click; contextMenuHidden only greys the iPhone-style items inside it (Stuart,
   *  B16: "still getting this annoying bug"). So on a Mac: focus WITHOUT a selection, and the first edit
   *  after focus starts a fresh number — the same "type to replace" the select-all gave everywhere else. */
  const onMac = isMacHost();
  const replaceOnType = useRef(false);
  const onChangeValue = (raw0: string) => {
    let raw = raw0;
    if (onMac && replaceOnType.current) {
      replaceOnType.current = false;
      raw = raw0.startsWith(value) ? raw0.slice(value.length) : raw0.length < value.length ? '' : raw0;
    }
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
      setDabFilter(!!dabOnly && !picking);
      if (picking) {
        // ★ A chooser starts clean and leaves the user's sticky search alone for their next tune.
        setCardMode('bookmarks'); setSearchQuery(''); setShowServerList(false);
      } else if (dabOnly) {
        setCardMode('bookmarks'); setSearchQuery('');
      } else if (sticky) {
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
  const [entryMsg, setEntryMsg] = useState('');
  useEffect(() => { setEntryMsg(''); }, [value, visible]);
  /* ★ As you type a channel NAME, say which frequency it tunes — 118.010 is 118.0083 MHz, and an
   *  aviation radio's user already knows that; everyone else deserves to be told. */
  const entryHint = useMemo(() => {
    const air = airbandEntry(fromDisplay(value, unit));
    if (!air || !air.ok) return '';
    return `Channel ${air.name} · ${air.spacing === 833 ? '8.33' : '25'} kHz · ${(air.hz / 1e6).toFixed(4)} MHz`;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, unit]);
  const confirm = () => {
    if (confirming.current) return;
    confirming.current = true;
    setTimeout(() => { confirming.current = false; }, 0);
    const hz = fromDisplay(value, unit);
    /* ★★ AN AIRBAND CHANNEL NAME THAT DOES NOT EXIST IS REFUSED, WITH THE REASON. In 118–137 MHz a
     *  value on a 5 kHz boundary is a channel name, pilot-style (utils/airband.ts), and 8.33 kHz
     *  names never end .x20/.x45/.x70/.x95. Snapping to a neighbour would tune somewhere nobody asked
     *  for — so the card stays open and says which two channels are either side. */
    const air = airbandEntry(hz);
    if (air && !air.ok) { setEntryMsg(air.message); Keyboard.dismiss(); return; }
    if (hz >= minHz && hz <= maxHz) { onConfirm(hz); onClose(); }
    /* ★★ OUT OF RANGE IS REFUSED WITH THE REASON, and the card stays open to correct it — the web
     *  client's rule. It used to do nothing at all: the card just sat there. On a receiver whose
     *  owner has locked the centre the range is the locked window, which is when this matters. */
    else if (hz > 0 && rangeMsg) setEntryMsg(rangeMsg);
    Keyboard.dismiss();
  };

  // ★★ A HARDWARE Enter has to come from the NATIVE key stream, not from the TextInput.
  // The field is keyboardType="decimal-pad", which has no return key for RN to map, so
  // onSubmitEditing never fires. The native window mirrors Enter through while typing (marked
  // `typing`), so listening here is what actually works.
  // (Stuart: "the enter button isnt working on frequency typing still".)
  // ★ ONLY WHILE THE FIELD HAS THE CURSOR, and only on the tune card. Not typing, Enter belongs to
  // the card's highlight (a tab, a unit, the field itself) — see tuneNav below.
  useEffect(() => {
    if (!visible) return;
    const sub = addKeyListener('down', (e) => {
      if (cardModeRef.current !== 'tune' || !e.typing || e.key !== 'Enter') return;
      confirmRef.current();
      return true;
    });
    return () => sub.remove();
  }, [visible]);

  // ★★★ ONE SCHEME (Stuart, 2026-10-18): the [T]une / [B]ookmarks and [H]z / [K]Hz / [M]Hz letters are gone, and so
  // are their boxed key caps. The tabs and the units are places the highlight goes instead: Escape leaves the
  // frequency field (the native window lets go of the cursor), then the arrows / W A S D walk the tabs, the units and
  // the field, Enter presses, Back closes the card. Exactly the game controller's way round the web page's card.
  // ★★ A GRID IN THE ORDER IT IS DRAWN (src/utils/freqCardNav.ts — why: the field used to hide after the units, where
  //    no arrow went). Opened from the keyboard, the highlight starts on the frequency box.
  const hasVtsSkip = !!(onVtsPrev && onVtsNext);
  const tuneLayout = useMemo(
    () => tuneCardItems({ tabs: hasBookmarks && !picking, vts: hasVtsSkip, lockUnit: !!lockUnit, profiles: profiles.length > 0,
                          actions: { share: !!onShare && !shareOffReason } }),
    [hasBookmarks, picking, hasVtsSkip, lockUnit, profiles.length, !!onShare, !!shareOffReason]);   // eslint-disable-line react-hooks/exhaustive-deps
  const tuneRun = (k: TuneCardKey) => {
    switch (k) {
      case 'tab:tune': setCardMode('tune'); break;
      case 'tab:bookmarks': Keyboard.dismiss(); setCardMode('bookmarks'); break;
      case 'vts:prev': onVtsPrev?.(); break;
      case 'vts:next': onVtsNext?.(); break;
      case 'field': inputRef.current?.focus(); break;
      case 'profile': setProfileToggle(n => n + 1); break;
      case 'act:cancel': onClose(); break;
      case 'act:share': doShareRef.current(); break;
      case 'act:tune': confirmRef.current(); break;
      default: switchUnitRef.current(k.slice(5) as Unit); break;
    }
  };
  const tuneRunRef = useRef(tuneRun); tuneRunRef.current = tuneRun;
  const [profileToggle, setProfileToggle] = useState(0);
  const tuneNavHandle = useRef<{ setFocus(i: number): void } | null>(null);
  const tuneNav = useListNav(visible && cardMode === 'tune', tuneLayout.items.length,
    (i) => { const it = tuneLayout.items[i]; if (it) tuneRunRef.current(it.k); },
    undefined, undefined, undefined,
    { rowOf: (i) => tuneLayout.items[i]?.row ?? i, startAt: tuneLayout.start, handle: tuneNavHandle });
  const tuneItems = tuneLayout.items;
  const tuneOn = (k: string) => tuneNav >= 0 && tuneItems[tuneNav]?.k === k;

  // Bookmark results: arrow through them and Enter to tune. While the search box has the
  // cursor the keys are the box's (typing is never hijacked); Escape leaves it, then the arrows
  // walk the results.
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
  /** A bookmarks-pane button that takes part in that order. ★ Given a `label`, it is a DOME KEY on
   *  silver / black (§10.3) — `active` + `pip` for a toggle / choice, `primary` for the SAVE keys;
   *  `children` is today's legend on the default chassis. Without a label it is a list row (a saved
   *  bookmark), which stays a row in its window on every chassis. */
  const BmBtn = ({ onPress, style, children, label, active, pip, primary, ...rest }: any) => {
    const { on, ref } = bmSlot(onPress ?? (() => {}));
    if (pt.metal && label) {
      return (
        <PopupKey ref={ref} label={label} active={!!active} pip={!!pip} primary={!!primary} onPress={onPress}
          disabled={rest.disabled} focused={on} height={32} fontSize={11}
          style={[StyleSheet.flatten(style), { marginBottom: StyleSheet.flatten(style)?.marginBottom ?? 0 }]} />
      );
    }
    return (
      <TouchableOpacity ref={ref} onPress={onPress}
        style={[style, on && { borderColor: focusC.ring, borderWidth: 2 }]} {...rest}>
        {children}
      </TouchableOpacity>
    );
  };

  // Read through a ref so the listener above never captures a stale `value`.
  const confirmRef = useRef(confirm); confirmRef.current = confirm;
  /** SHARE — the key's press, and Enter on it (controller audit group C). */
  const doShare = () => {
    if (!onShare) return;
    if (Platform.OS === 'ios') { pendingShare.current = true; onClose(); }
    else { onShare(); onClose(); }
  };
  const doShareRef = useRef(doShare); doShareRef.current = doShare;
  // Read through a ref: the listener above is installed once, on `visible` alone.
  const cardModeRef = useRef(cardMode); cardModeRef.current = cardMode;

  // ★ §10.3 on silver / black: text INSIDE a recessed window keeps light colours (the station,
  //   the entry, the lists — `winC` / `winDim`, in the text colour); text left ON the plate is
  //   engraved (`plateDim`).
  // ★★ On the DEFAULT chassis these follow the user's colours now (Stuart, 2026-10-12: "yes colours apply
  //   everywhere including directory if possible"): every key's legend and border in the CONTROLS colour
  //   (`keyDim` an unselected key, `keyOn` a selected or primary one, `bdrDim` / `bdrBrt` their borders),
  //   every readout and line of text in the TEXT colour (popupTokens ink). They were the white theme's whites.
  const dimText  = pt.gold.close;          // an unselected key's legend (was white .45)
  const keyOn    = pt.gold.sel;            // a selected / primary key's legend (was the white freqColor)
  const unitText = pt.ink.textDim;         // hints and the unit label: text (was #b0b8c8)
  const bdrDim   = pt.ink.keyBorder;       // a key's border at rest (was white .20)
  const bdrBrt   = pt.gold.selBorder;      // a selected / primary key's border (was white .45)
  const btnPadY  = isWhite ? 12 : 10;
  const winC     = pt.metal ? pt.winText : pt.ink.text;
  const winName  = pt.metal ? pt.winText : pt.ink.text;
  const winDim   = pt.metal ? pt.winDim : pt.ink.textDim;
  const plateDim = pt.metal ? pt.note : pt.ink.textDim;
  /** Popup text is always Atkinson on silver / black (§10.3) — only the tune entry follows the Display. */
  const ff       = pt.metal ? POPUP_FONT : t.font;
  const e = pt.entry;
  const entryStyle = {
    color: e.color, fontFamily: e.fontFamily, fontSize: e.fontSize, fontWeight: e.fontWeight,
    fontStyle: e.fontStyle, letterSpacing: e.letterSpacing,
    ...(e.glow ? { textShadowColor: e.glow, textShadowRadius: 6, textShadowOffset: { width: 0, height: 0 } } : null),
  } as const;
  /* Bookmarks list cap (§6.4/§6.5): size to the space actually available above the keyboard, so a
   * big phone (17PM) shows every button with no scroll, while small phones cap-and-scroll.
   *
   * ★★★ THE CHROME ALLOWANCE WAS A CONSTANT, AND THE STATUS BAR IS NOT. 150 stood for "card
   *     padding + segmented header + top safe gap", so on a phone whose Dynamic Island is deeper
   *     than that guess the card ran up UNDER it — the first rows sat behind the clock and the
   *     battery (Stuart, 2026-09-03, with a screenshot). The card is centred, so half of any
   *     overflow lands at the top, which is the one edge that has furniture on it.
   *  ★★ It only started showing now: the search results survive a tune, so the card OPENS at its
   *     full height instead of growing there while you look at it. My change did not cause the
   *     bad arithmetic, it stopped hiding it.
   *  ★ Both insets, doubled, because centring splits the overflow: keeping the top clear is the
   *    requirement, and the bottom must not be allowed to buy the space back.
   */
  const chrome = 96 + Math.max(safeTop, safeBottom) * 2;
  const bmMaxH = Math.max(160, winH - kbHeight - chrome);

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}
           onDismiss={() => { if (pendingShare.current) { pendingShare.current = false; onShare?.(); } }}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      {/* ★★★ Transparency OFF: the tap-to-close view stays, the dim goes (PopupScrim). */}
      <PopupScrim style={st.backdrop} color={BACKDROP} onPress={onClose} />
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
        <View style={[st.modal, { borderColor: pt.ink.border },
                      surf.opaque && !pt.metal && { backgroundColor: surf.fill(CARD_BG) }, metalFrame]}
              // ★ Touching the card ends keyboard mode, so the highlight disappears rather than
              // sitting on a key under someone's thumb. A Modal is its own window, so SDRScreen's
              // root touch sniff never sees this.
              onTouchStart={noteTouchInteraction}>
          <PopupPlate radius={16} />
          {picking ? (
            <View style={pt.metal ? st.segHeaderMetal : st.segHeader}>
              <Text style={[st.title, { flex: 1, marginBottom: 0, color: pt.ink.section, fontFamily: t.font }, st.titleMetal]}
                    numberOfLines={1}>CHECK OUT A STATION</Text>
              {pt.metal ? (
                <PopupKey label="✕" onPress={onClose} height={32} fontSize={15} hitSlop={10}
                  accessibilityLabel="Close" style={{ width: 36, paddingHorizontal: 0 }} />
              ) : (
                <TouchableOpacity onPress={onClose} style={st.segClose} hitSlop={10}
                                  accessibilityRole="button" accessibilityLabel="Close">
                  <Text style={{ fontFamily: t.font, fontSize: 18, color: dimText }}>✕</Text>
                </TouchableOpacity>
              )}
            </View>
          ) : hasBookmarks && pt.metal ? (
            // ★ §10.3: TUNE | BOOKMARKS are an input selector — two dome keys, the lit one with its
            //   pip — and the ✕ is a plain key. Same order; the keyboard's highlight lands on each.
            <View style={st.segHeaderMetal}>
              {(['tune', 'bookmarks'] as const).map(m => { const tabRun = () => { setCardMode(m); if (m === 'bookmarks') Keyboard.dismiss(); };
                const slot = cardMode === 'bookmarks' ? bmSlot(tabRun) : null;
                return (
                <PopupKey key={m} ref={slot?.ref} active={cardMode === m} pip height={32} style={{ flex: 1 }}
                  focused={cardMode === 'tune' ? tuneOn('tab:' + m) : !!slot?.on}
                  accessibilityLabel={m === 'tune' ? 'Tune' : 'Bookmarks'}
                  onPress={tabRun}>
                  {(c: string) => (
                    <Text style={[st.segTabText, { fontFamily: POPUP_FONT, color: c }]}>
                      {m === 'tune' ? 'TUNE' : 'BOOKMARKS'}
                    </Text>
                  )}
                </PopupKey>
              ); })}
              <PopupKey label="✕" onPress={onClose} height={32} fontSize={15} hitSlop={10}
                accessibilityLabel="Close" style={{ width: 36, paddingHorizontal: 0 }} />
            </View>
          ) : hasBookmarks ? (
            <View style={st.segHeader}>
              {(['tune', 'bookmarks'] as const).map(m => { const tabRun = () => { setCardMode(m); if (m === 'bookmarks') Keyboard.dismiss(); };
                const slot = cardMode === 'bookmarks' ? bmSlot(tabRun) : null;
                const hl = cardMode === 'tune' ? tuneOn('tab:' + m) : !!slot?.on;
                return (
                <TouchableOpacity key={m} ref={slot?.ref}
                  style={[st.segTab, { borderBottomColor: cardMode === m ? keyOn : 'transparent' },
                          hl && { borderWidth: 2, borderColor: focusC.ring, borderRadius: 6 }]}
                  onPress={tabRun} activeOpacity={0.7}>
                  <Text style={[st.segTabText, { fontFamily: t.font, color: cardMode === m ? keyOn : dimText }]}>
                    {m === 'tune' ? 'TUNE' : 'BOOKMARKS'}
                  </Text>
                </TouchableOpacity>
              ); })}
              {/* ★★★ AN EXIT THAT IS ALWAYS ON SCREEN. The only ways out of the bookmarks tab were
                  the backdrop and the TUNE tab's CANCEL — and on a tall phone the card can fill the
                  screen, leaving no backdrop to tap (Stuart, 2026-09-03). A modal whose dismissal
                  depends on there being spare screen around it is one bad screen size from being a
                  trap.
                  ★ In the HEADER, which sits outside the ScrollView, so it cannot be scrolled away
                    however long the results list gets — the tune tab's CANCEL scrolls with its
                    content and this one must not. */}
              <TouchableOpacity onPress={onClose} style={st.segClose} hitSlop={10}
                                accessibilityRole="button" accessibilityLabel="Close">
                <Text style={{ fontFamily: t.font, fontSize: 18, color: dimText }}>✕</Text>
              </TouchableOpacity>
            </View>
          ) : (
            <Text style={[st.title, { color: pt.ink.section, fontFamily: t.font }, st.titleMetal]}>FREQUENCY</Text>
          )}

          {cardMode === 'tune' && (<>
          {/* VTS nearby-station skip — relocated from MenuSheet (§4.1). Absent on FM-DX
              (SDRScreen doesn't pass the handlers there — the shared-tuner guard travels). */}
          {/* ★ §10.3: the station and the entry are DATA — on silver / black they sit in one recessed
              window, lit in the text colour; the entry follows the Display style (pt.entry). */}
          <PopupWindow metalStyle={st.tuneWin}>
          {onVtsPrev && onVtsNext && (
            <View style={st.vtsRow}>
              <TouchableOpacity style={[st.vtsArrow, tuneOn('vts:prev') && { borderWidth: 2, borderColor: focusC.ring, borderRadius: 6 }]}
                                onPress={onVtsPrev} hitSlop={8}>
                <Text style={[st.vtsArrowText, st.vtsArrowLeft, { color: pt.metal ? pt.winDim : pt.ink.text }]}>►</Text>
              </TouchableOpacity>
              <View style={st.vtsInfo}>
                <Text style={[st.vtsName, { color: winName, fontFamily: ff }]} numberOfLines={1}>
                  {(draftVts?.name ?? vtsName) || '—'}
                </Text>
                {(draftVts ? draftVts.freq : vtsFreq) != null && (
                  <Text style={[st.vtsFreq, { color: winDim }]}>
                    {((draftVts ? draftVts.freq : vtsFreq)! / 1_000_000).toFixed(3)} MHz
                  </Text>
                )}
              </View>
              <TouchableOpacity style={[st.vtsArrow, tuneOn('vts:next') && { borderWidth: 2, borderColor: focusC.ring, borderRadius: 6 }]}
                                onPress={onVtsNext} hitSlop={8}>
                <Text style={[st.vtsArrowText, { color: pt.metal ? pt.readout : pt.ink.text }]}>►</Text>
              </TouchableOpacity>
            </View>
          )}
          <View style={[st.inputRow, { borderBottomColor: pt.ink.border }, st.inputRowMetal,
                        tuneOn('field') && { borderWidth: 2, borderColor: focusC.ring, borderRadius: 6 }]}>
            <TextInput
              ref={inputRef}
              style={[st.input, pt.metal ? entryStyle : { color: pt.entry.color, fontFamily: t.font }]}
              // ★ 14-segment: fed through toSegCells like every DSEG14 string (§7) — for a frequency
              //   (digits and one point) it is the identity, so the field stays a live TextInput.
              value={pt.entry.seg && pt.metal ? toSegCells(value) : value}
              onChangeText={onChangeValue}
              keyboardType="decimal-pad"
              autoComplete="off"
              autoCorrect={false}
              // ★ On a Mac (the iPad app) a field with no content type and spell-check on gets macOS's
              //   Siri / AutoFill suggestion bubble on its first click, which eats the click (B11,
              //   Stuart: "a stupid Siri option comes up on first click and I have to click again").
              spellCheck={false}
              textContentType="none"
              // ★★ AND NO EDIT MENU. selectTextOnFocus selects the old frequency so typing replaces it —
              //   and on a Mac a selection raises the Cut / Copy / Look Up / Translate menu over the field,
              //   eating the first click (Stuart: "a popup with Look Up etc in it"). Nothing in that menu
              //   is useful on a frequency; the select-all stays.
              contextMenuHidden
              /* ★★ NOT ON ANDROID: there React Native's selectTextOnFocus re-selects on a LATER LAYOUT. Its flag is
               *  only cleared by the first layout that happens while focused — and the field is not focused when the
               *  popup first lays out, so the flag survived; typing "4" changed the text, the field laid out again and
               *  selected everything, and "582" then replaced the 4 (Stuart, XCover 4s, 2026-10-12: typing 4582 gave
               *  582). Android selects the old frequency itself, once, as the field takes focus. */
              selectTextOnFocus={!onMac && Platform.OS !== 'android'}
              onFocus={() => {
                // ★ The highlight sits on the field while it has the cursor, however the cursor got there (a tap, or
                //   Enter on the highlighted box) — so when Escape lets go of it, the arrows carry on from HERE.
                if (tuneLayout.start >= 0) tuneNavHandle.current?.setFocus(tuneLayout.start);
                if (onMac) replaceOnType.current = true;
                else if (Platform.OS === 'android') {
                  const len = valueRef.current.length;
                  requestAnimationFrame(() => inputRef.current?.setSelection(0, len));
                }
              }}
              onSubmitEditing={confirm}
              // ★ A hardware Enter is handled by the key listener above (hardwareKeys, `typing`), NOT
              // here: a decimal pad has no return key, so onSubmitEditing never fires for it.
              returnKeyType="done"
            />
            <Text style={[st.unitLabel, { color: pt.metal ? pt.winDim : unitText, fontFamily: ff }]}>
              {unit === 'hz' ? 'Hz' : unit === 'khz' ? 'kHz' : 'MHz'}
            </Text>
          </View>
          </PopupWindow>
          {!!(entryMsg || entryHint) && (
            <Text style={[st.bmMsg, { color: entryMsg ? (pt.metal ? pt.danger : pt.ink.m.bad) : (pt.metal ? pt.note : unitText),
                                      // ★ An entry error is a `bad` meaning: red, or white BOLD on red text (meaningsFor).
                                      ...(entryMsg && !pt.metal && pt.ink.m.badBold ? { fontWeight: '700' as const } : null),
                                      fontFamily: ff, textAlign: 'center' }]}>
              {entryMsg || entryHint}
            </Text>
          )}
          <View style={st.units}>
            {(['hz', 'khz', 'mhz'] as Unit[]).map(u => pt.metal ? (
              // ★ An input selector (§10.3): the lit unit carries the LED pip.
              <PopupKey key={u} active={unit === u} pip height={34} style={{ flex: 1 }}
                disabled={lockUnit && u !== 'mhz'} onPress={() => switchUnit(u)} focused={tuneOn('unit:' + u)}
                accessibilityLabel={u === 'hz' ? 'Hz' : u === 'khz' ? 'kHz' : 'MHz'}>
                {(c: string) => (
                  <Text style={[st.unitBtnText, st.unitBtnTextMetal, { color: c }]}>
                    {u === 'hz' ? 'Hz' : u === 'khz' ? 'kHz' : 'MHz'}
                  </Text>
                )}
              </PopupKey>
            ) : (
              <TouchableOpacity
                key={u}
                disabled={lockUnit && u !== 'mhz'}
                style={[
                  st.unitBtn,
                  { borderColor: bdrDim, paddingVertical: btnPadY },
                  unit === u && { borderColor: bdrBrt, backgroundColor: pt.ink.keyBgAct },
                  lockUnit && u !== 'mhz' && { opacity: 0.3 },
                  tuneOn('unit:' + u) && { borderColor: focusC.ring, borderWidth: 2 },
                ]}
                onPress={() => switchUnit(u)}
              >
                <Text style={[
                  st.unitBtnText,
                  { fontFamily: t.font, color: dimText },
                  unit === u && { color: keyOn },
                ]}>
                  {u === 'hz' ? 'Hz' : u === 'khz' ? 'kHz' : 'MHz'}
                </Text>
              </TouchableOpacity>
            ))}
          </View>
          {profiles.length > 0 && (
            <View style={st.profiles}>
              <PopupWindow metalStyle={st.profileWin}>
              <ProfilePicker
                active={visible && cardMode === 'tune'}
                navFocused={tuneOn('profile')}
                toggleToken={profileToggle}
                profiles={profiles}
                activeProfileId={activeProfileId}
                sdrUsage={sdrUsage}
                clientCount={clientCount}
                onSelectProfile={onSelectProfile}
                onPicked={onClose}
              />
              </PopupWindow>
              {/* ★★★ THE MAGIC KEY. An OWRX operator can LOCK a profile; selecting it without the
                  key is refused and the picker snaps back, with the server saying "This profile is
                  locked, keeping current profile." (now surfaced — see onLogMessage). Given the
                  key, the same selection succeeds.
                  ★ Sits directly under the profiles because that is what it unlocks, and because a
                    listener only discovers they need it by being refused one.
                  ★★ AND IT IS NOT A GUARANTEE. The key also unlocks centre-frequency changes, but
                     only where the operator has ALSO enabled allow_center_freq_changes — with that
                     off, a correct key changes nothing and OWRX says nothing either. Said here so
                     it reads as the receiver's choice rather than our bug.
                  ★ No auto-capitalise/correct: it is a password, and a phone keyboard "helping"
                    with it is the oldest way to make a correct key look wrong. */}
              {!!onMagicKey && (
                <View style={st.magicRow}>
                  <TextInput
                    style={st.magicInput}
                    value={magicKey ?? ''}
                    onChangeText={onMagicKey}
                    placeholder="OWRX magic key (optional)"
                    placeholderTextColor={pt.metal ? pt.winDim : pt.ink.textFaint}
                    secureTextEntry
                    autoCapitalize="none"
                    autoCorrect={false}
                    spellCheck={false}
                    textContentType="none"
                    returnKeyType="done"
                  />
                  <Text style={st.magicHint}>
                    The server owner's key for locked profiles. Blank unless they gave you one.
                    Kept for this session only.
                  </Text>
                </View>
              )}
            </View>
          )}

          {pt.metal ? (
            // ★ §10.3: CANCEL and SHARE are plain keys; TUNE ▶ is the primary action, its legend lit.
            <View style={st.actions}>
              <PopupKey label="CANCEL" focused={tuneOn('act:cancel')} onPress={onClose} height={40} fontSize={12} style={{ flex: 1 }} />
              {onShare && shareOffReason ? (
                <PopupKey label={shareOffReason} disabled numberOfLines={2} height={40} fontSize={9}
                  accessibilityLabel={`Share. ${shareOffReason}`} style={{ flex: 1 }} />
              ) : onShare && (
                <PopupKey label="SHARE" focused={tuneOn('act:share')} height={40} fontSize={12} style={{ flex: 1 }}
                  onPress={doShare} />
              )}
              <PopupKey label="TUNE ▶" primary focused={tuneOn('act:tune')} onPress={confirm} height={40} fontSize={12} style={{ flex: 2 }} />
            </View>
          ) : (
          <View style={st.actions}>
            <TouchableOpacity
              style={[st.cancelBtn, { borderColor: bdrDim, paddingVertical: btnPadY }, tuneOn('act:cancel') && { borderColor: focusC.ring, borderWidth: 2 }]}
              onPress={onClose}
            >
              <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: dimText }}>
                CANCEL
              </Text>
            </TouchableOpacity>
            {onShare && shareOffReason ? (
              <View style={[st.cancelBtn, { borderColor: bdrDim, paddingVertical: btnPadY, opacity: 0.45 }]}
                    accessibilityState={{ disabled: true }} accessibilityLabel={`Share. ${shareOffReason}`}>
                <Text style={{ fontFamily: t.font, fontSize: isWhite ? 11 : 10, color: dimText, textAlign: 'center' }}>
                  {shareOffReason}
                </Text>
              </View>
            ) : onShare && (
              <TouchableOpacity
                style={[st.cancelBtn, { borderColor: bdrDim, paddingVertical: btnPadY }, tuneOn('act:share') && { borderColor: focusC.ring, borderWidth: 2 }]}
                onPress={doShare}
              >
                <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: dimText }}>
                  SHARE
                </Text>
              </TouchableOpacity>
            )}
            <TouchableOpacity
              style={[st.tuneBtn, { borderColor: bdrBrt, paddingVertical: btnPadY }, tuneOn('act:tune') && { borderColor: focusC.ring, borderWidth: 2 }]}
              onPress={confirm}
            >
              <Text style={{ fontFamily: t.font, fontSize: isWhite ? 13 : 12, color: keyOn, fontWeight: 'bold' }}>
                TUNE ▶
              </Text>
            </TouchableOpacity>
          </View>
          )}
          </>)}

          {/* BOOKMARKS mode — search + band plan, EiBi, add current, saved list, transfer.
              Lifted verbatim from MenuSheet (§4.2). */}
          {cardMode === 'bookmarks' && (
            <ScrollView ref={bmScrollRef} style={[{ maxHeight: bmMaxH }, scrollLaneOutset]}
                        /* ★ The indicator's lane — the bookmark rows end in delete / cloud keys. The card's
                           20 pt padding lends it, so the rows keep their width. */
                        contentContainerStyle={scrollLane}
                        onScroll={(e) => { bmScrollY.current = e.nativeEvent.contentOffset.y; }}
                        scrollEventThrottle={16}
                        keyboardShouldPersistTaps="handled" showsVerticalScrollIndicator>
              {(() => { const { on, ref: slotRef } = bmSlot(() => searchRef.current?.focus()); return (
              <TextInput
                ref={(r: any) => { (searchRef as any).current = r; slotRef(r); }}
                style={[st.searchInput, { color: winC, fontFamily: ff,
                                          borderColor: on ? focusC.ring : (pt.metal ? pt.window.border : bdrDim), borderWidth: on ? 2 : 1 },
                        st.inputMetal]}
                value={searchQuery} onChangeText={(v: string) => {
                  setSearchQuery(v);
                  // ★ Emptying the box by hand is the user saying they are done with that list.
                  if (!v.trim()) stickySearch = null;
                }}
                placeholder={picking ? '🔍 Search bookmarks & EiBi…' : '🔍 Search bookmarks & band plan…'} placeholderTextColor={winDim}
                autoCorrect={false} autoCapitalize="none" spellCheck={false} textContentType="none" clearButtonMode="while-editing" />
              ); })()}
              {dabFilter && (() => {
                const rows = searchBookmarks
                  .filter(b => (b.mode || '').toLowerCase() === 'dab' && (b.sid ?? -1) >= 0)
                  .sort((a, b) => a.name.localeCompare(b.name));
                return (<>
                  <View style={st.bmToggleRow}>
                    <Text style={[st.bmSub, { color: plateDim, marginTop: 0 }]}>DAB STATIONS HEARD BY THIS RECEIVER ({rows.length})</Text>
                    {pt.metal ? (
                      <PopupKey label="SHOW ALL" onPress={() => setDabFilter(false)} height={26} fontSize={10} hitSlop={8} />
                    ) : (
                    <TouchableOpacity onPress={() => setDabFilter(false)} hitSlop={8}>
                      <Text style={{ color: keyOn, fontFamily: t.font, fontSize: 11 }}>SHOW ALL</Text>
                    </TouchableOpacity>
                    )}
                  </View>
                  {rows.length === 0 && <Text style={[st.bmMsg, { color: plateDim }]}>None yet — the receiver learns them as multiplexes are decoded.</Text>}
                  <PopupWindow metalStyle={st.listWin}>
                  {rows.map((b, i) => (
                    <TouchableOpacity key={`dab|${b.frequency}|${b.sid}|${i}`} activeOpacity={0.7} style={st.searchRow}
                      onPress={() => { tuneBm(b); onClose(); }}>
                      <Text style={[st.searchFreq, { color: winC }]}>{fmtFreq(b.frequency)}</Text>
                      <Text style={[st.searchMode, { color: winDim }]}>DAB</Text>
                      <Text style={[st.searchName, { color: winName }]} numberOfLines={1}>{b.name}</Text>
                    </TouchableOpacity>
                  ))}
                  </PopupWindow>
                </>);
              })()}
              {!dabFilter && searchQuery.trim().length > 0 && (searchResults.length === 0 ? (
                <Text style={[st.bmMsg, { color: plateDim }]}>No results for “{searchQuery.trim()}”</Text>
              ) : (<>
                <Text style={[st.bmHint, { color: plateDim }]}>{searchResults.length} result{searchResults.length !== 1 ? 's' : ''} · tap to {picking ? 'share' : 'tune'}</Text>
                <PopupWindow metalStyle={st.listWin}>
                {searchResults.map((r: SearchResult, i: number) => {
                  const tune = () => {
                    // ★ Keep the query rather than clearing it: this is the tap that USED the
                    //   list, which is exactly when you are most likely to want it again.
                    if (!picking) stickySearch = { q: searchQuery, at: Date.now() };
                    if (r.isBand && r.band) onSearchTune?.(r.band.start, r.band.mode, true);
                    else if (r.bm) tuneBm(r.bm);
                    onClose();
                  };
                  const { on, ref: slotRef } = bmSlot(tune);
                  return (
                  <TouchableOpacity key={i} ref={slotRef} activeOpacity={0.7}
                    style={[st.searchRow, on && { backgroundColor: focusC.tint(0.16) }]}
                    onPress={tune}>
                    <Text style={[st.searchFreq, { color: winC }]}>{r.isBand && r.band ? fmtRange(r.band.start, r.band.end) : fmtFreq(r.bm?.frequency ?? 0)}</Text>
                    <Text style={[st.searchMode, { color: winDim }]}>{r.isBand ? grpAbbr(r.band?.group) : (r.bm?.mode ?? '—').toUpperCase()}</Text>
                    {!r.isBand && r.bm?.name ? <StationLogo name={r.bm.name} itu={r.bm.itu} /> : null}
                    <Text style={[st.searchName, { color: winName }]} numberOfLines={1}>
                      {!r.isBand && r.bm?.flag ? r.bm.flag + ' ' : ''}{r.isBand ? (r.band?.label ?? '') : (r.bm?.name ?? '')}
                    </Text>
                  </TouchableOpacity>
                  );
                })}
                </PopupWindow>
              </>))}

              {/* ★★★ WHAT THIS RECEIVER HAS HEARD, WITHOUT HAVING TO GUESS ITS NAME FIRST.
                  The stations a server has learned by RDS (and the ones its owner saved by hand)
                  were reachable only by TYPING one into the search box — which works if you
                  already know what is there, and is useless if you do not. The web client shows
                  the list outright; the app has the same data and never offered a way to see it
                  (Stuart, 2026-09-23).
                  ★★ A BUTTON, NOT AN EXPANDED LIST. His call, and the right one for a phone: the
                     card is already tall and a receiver that has heard fifty stations would push
                     everything else off the screen. Closed by default, and the count is on the
                     button so it is worth pressing (or obviously not).
                  ★ Only where there IS one — a server with nothing learned yet gets no button
                    rather than a button that opens an empty list. */}
              {(serverBookmarks?.length ?? 0) > 0 && (<>
                {/* ★ BmBtn registers its OWN keyboard-nav slot (see its definition), so it must
                    not be wrapped in another one: that would put the button in the focus order
                    twice, and a `ref` on a plain function component is dropped with a warning. */}
                <BmBtn style={[st.bmSeg, { marginTop: 10,
                                           borderColor: showServerList ? bdrBrt : bdrDim }]}
                  label={`ON THIS SERVER · ${serverBookmarks!.length}`} active={showServerList} pip
                  onPress={() => setShowServerList(v => !v)}>
                  <Text style={[st.bmSegText, { color: showServerList ? keyOn : dimText }]}>
                    {showServerList ? '▾ ' : '▸ '}ON THIS SERVER · {serverBookmarks!.length}
                  </Text>
                </BmBtn>
                {showServerList && (<>
                  {/* ★ Learned entries EXPIRE if the aerial stops hearing them, and a saved one is
                      somebody's decision — different claims, so the row says which. Where the
                      backend does not tell us (UberSDR, OWRX, Kiwi send no such flag) the honest
                      label is none at all rather than a guess. */}
                  <Text style={[st.bmHint, { color: plateDim }]}>
                    tap to {picking ? 'share' : 'tune'} · LEARNT = heard by this aerial, SAVED = set by its owner
                  </Text>
                  <PopupWindow metalStyle={st.listWin}>
                  {serverBookmarks!.map((b: ServerBookmark, i: number) => {
                    const go = () => { tuneBm(b); onClose(); };
                    const { on, ref: slotRef } = bmSlot(go);
                    const tag = b.manual === undefined ? '' : (b.manual ? 'SAVED' : 'LEARNT');
                    return (
                      <TouchableOpacity key={`srv${i}`} ref={slotRef} activeOpacity={0.7}
                        style={[st.searchRow, on && { backgroundColor: focusC.tint(0.16) }]}
                        onPress={go}>
                        <Text style={[st.searchFreq, { color: winC }]}>{fmtFreq(b.frequency)}</Text>
                        <Text style={[st.searchMode, { color: winDim }]}>{(b.mode ?? '—').toUpperCase()}</Text>
                        {b.name ? <StationLogo name={b.name} itu={b.itu} /> : null}
                        <Text style={[st.searchName, { color: winName }]} numberOfLines={1}>
                          {b.flag ? b.flag + ' ' : ''}{b.name}
                        </Text>
                        {tag ? <Text style={[st.searchMode, { color: winDim }]}>{tag}</Text> : null}
                      </TouchableOpacity>
                    );
                  })}
                  </PopupWindow>
                </>)}
              </>)}

              {onEibiToggle && (
                <View style={st.bmToggleRow}>
                  <Text style={[st.bmSub, { color: plateDim, marginTop: 0 }]}>EiBi SCHEDULE</Text>
                  {pt.metal ? (
                    <PopupKey label={eibiEnabled ? 'ON' : 'OFF'} active={eibiEnabled} pip hitSlop={8} height={28} fontSize={10}
                      style={{ minWidth: 58 }} onPress={() => onEibiToggle(!eibiEnabled)} />
                  ) : (
                  <TouchableOpacity onPress={() => onEibiToggle(!eibiEnabled)} hitSlop={8}
                    style={[st.bmToggle, { borderColor: eibiEnabled ? bdrBrt : bdrDim, backgroundColor: eibiEnabled ? pt.ink.keyBgAct : 'transparent' }]}>
                    <Text style={{ color: eibiEnabled ? keyOn : dimText, fontFamily: t.font, fontSize: 11 }}>{eibiEnabled ? 'ON' : 'OFF'}</Text>
                  </TouchableOpacity>
                  )}
                </View>
              )}

              {!picking && (<>
              <Text style={[st.bmSub, { color: plateDim }]}>Add: {(currentHz / 1_000_000).toFixed(4)} MHz {currentMode.toUpperCase()}</Text>
              {(() => { const { on, ref: slotRef } = bmSlot(() => bmNameRef.current?.focus()); return (
              <TextInput ref={(r: any) => { (bmNameRef as any).current = r; slotRef(r); }}
                style={[st.searchInput, { color: winC, fontFamily: ff,
                                          borderColor: on ? focusC.ring : (pt.metal ? pt.window.border : bdrDim), borderWidth: on ? 2 : 1 },
                        st.inputMetal]}
                value={bmName} onChangeText={setBmName} placeholder="Bookmark name…" placeholderTextColor={winDim} maxLength={60} autoCorrect={false} />
              ); })()}
              <View style={st.bmSegRow}>
                <BmBtn style={[st.bmSeg, { borderColor: !bmAll ? bdrBrt : bdrDim }]} label="THIS SERVER" active={!bmAll} pip onPress={() => setBmAll(false)}><Text style={[st.bmSegText, { color: !bmAll ? keyOn : dimText }]}>THIS SERVER</Text></BmBtn>
                <BmBtn style={[st.bmSeg, { borderColor: bmAll ? bdrBrt : bdrDim }]} label="ALL SERVERS" active={bmAll} pip onPress={() => setBmAll(true)}><Text style={[st.bmSegText, { color: bmAll ? keyOn : dimText }]}>ALL SERVERS</Text></BmBtn>
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
                     label="★ SAVE BOOKMARK" primary
                     disabled={!bmName.trim()}
                     onPress={() => { if (!bmName.trim()) return; onAddBookmark?.(bmName, bmAll); setBmName(''); }}>
                <Text style={[st.bmBtnText, { color: keyOn },
                              !bmName.trim() && { opacity: 0.4 }]}>★ SAVE BOOKMARK</Text>
              </BmBtn>
              {/* ★ ON THE RECEIVER — shared with every later visitor, so it is only offered while the
                  admin password is held (the web's ADD TO SERVER). Same name box, same rule about
                  an empty one. */}
              {!!onAddServerBookmark && (
                <BmBtn style={[st.bmBtn, { borderColor: bmName.trim() ? bdrBrt : bdrDim, marginTop: 6 }]}
                       label="⚿ SAVE ON THE RECEIVER" primary
                       disabled={!bmName.trim()}
                       onPress={async () => { if (!bmName.trim()) return; const m = await onAddServerBookmark(bmName); setBmImportMsg(m); if (m.startsWith('Saved')) setBmName(''); }}>
                  <Text style={[st.bmBtnText, { color: keyOn }, !bmName.trim() && { opacity: 0.4 }]}>⚿ SAVE ON THE RECEIVER</Text>
                </BmBtn>
              )}

              </>)}

              <Text style={[st.bmSub, { color: plateDim }]}>Saved ({userBookmarks.length})</Text>
              {userBookmarks.length === 0 && <Text style={[st.bmMsg, { color: plateDim }]}>No bookmarks yet — tune somewhere good and save it.</Text>}
              <PopupWindow metalStyle={userBookmarks.length ? st.listWin : st.listWinEmpty}>
              {userBookmarks.map((b: UserBookmark, i: number) => (
                <View key={`${b.name}|${b.frequency}|${i}`} style={st.bmSaveRow}>
                  <BmBtn style={{ flex: 1 }} activeOpacity={0.7} onPress={() => {
                    if (onPickShare) onPickShare(b);
                    else onSearchTune?.(b.frequency, b.mode, false, false, bookmarkPassband(b));
                    onClose();
                  }}>
                    <Text style={[st.bmName2, { color: winC }]} numberOfLines={1}>{b.name}</Text>
                    <Text style={[st.bmFreq2, { color: winDim }]}>{fmtFreq(b.frequency)}  {b.mode.toUpperCase()}</Text>
                  </BmBtn>
                  {!picking && !!onToggleBookmarkSync && (
                    <TouchableOpacity hitSlop={8} onPress={() => onToggleBookmarkSync(b)}
                      accessibilityLabel={b.synced ? `Stop syncing ${b.name} to iCloud` : `Sync ${b.name} to iCloud`}>
                      <Text style={[st.bmCloud, { color: b.synced ? winC : winDim,
                                                  opacity: b.synced ? 1 : 0.45 }]}>
                        {/* Distinct GLYPHS, not just a colour: a dim cloud and a
                            bright one are the same shape, and "is this one
                            syncing?" is exactly the question the row has to
                            answer at a glance. */}
                        {b.synced ? '\u2601\u2713' : '\u2601'}
                      </Text>
                    </TouchableOpacity>
                  )}
                  {!picking && <TouchableOpacity hitSlop={8} onPress={() => onDeleteBookmark?.(b)}><Text style={[st.bmDel, { color: winDim }]}>✕</Text></TouchableOpacity>}
                </View>
              ))}
              </PopupWindow>

              {!picking && (<>
              <Text style={[st.bmSub, { color: plateDim }]}>Transfer</Text>
              <View style={st.bmSegRow}>
                <BmBtn style={[st.bmSeg, { borderColor: bdrDim }]} label="⇧ EXPORT JSON" onPress={onExportBookmarks}><Text style={[st.bmSegText, { color: dimText }]}>⇧ EXPORT JSON</Text></BmBtn>
                <BmBtn style={[st.bmSeg, { borderColor: bmImportOpen ? bdrBrt : bdrDim }]} label="⇩ PASTE" active={bmImportOpen} pip onPress={() => { setBmImportOpen(p => !p); setBmImportMsg(''); }}><Text style={[st.bmSegText, { color: bmImportOpen ? keyOn : dimText }]}>⇩ PASTE</Text></BmBtn>
              </View>
              {onPickImportFile && (
                pt.metal ? (
                  <PopupKey label="📁 IMPORT FILE (JSON / YAML)" height={32} fontSize={11} style={{ marginBottom: 8 }} onPress={async () => { const msg = await onPickImportFile(bmAll); if (msg) { setBmImportMsg(msg); setBmImportOpen(false); } }} />
                ) : (
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrDim }]} onPress={async () => { const msg = await onPickImportFile(bmAll); if (msg) { setBmImportMsg(msg); setBmImportOpen(false); } }}>
                  <Text style={[st.bmBtnText, { color: dimText }]}>📁 IMPORT FILE (JSON / YAML)</Text>
                </TouchableOpacity>
                )
              )}
              {!!onPickImportFileToServer && (
                pt.metal ? (
                  <PopupKey label="⚿ IMPORT FILE TO THE RECEIVER" height={32} fontSize={11} style={{ marginBottom: 8 }} disabled={!!bmBusy} onPress={importFileToServer} />
                ) : (
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrDim }]} disabled={!!bmBusy} onPress={importFileToServer}>
                  <Text style={[st.bmBtnText, { color: dimText }]}>⚿ IMPORT FILE TO THE RECEIVER</Text>
                </TouchableOpacity>
                )
              )}
              {!!bmBusy && (
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }} accessibilityLiveRegion="polite">
                  <ActivityIndicator size="small" color={plateDim} />
                  <Text style={[st.bmMsg, { color: plateDim, flex: 1 }]}>
                    {bmBusy.total ? `Importing to the receiver… ${bmBusy.done.toLocaleString()} of ${bmBusy.total.toLocaleString()}` : 'Importing to the receiver…'}
                  </Text>
                </View>
              )}
              {!!bmImportMsg && !bmBusy && <Text style={[st.bmMsg, { color: plateDim }]}>{bmImportMsg}</Text>}
              {bmImportOpen && (<>
                <TextInput style={[st.searchInput, st.bmImportBox, { color: winC, fontFamily: ff, borderColor: pt.metal ? pt.window.border : bdrDim }, st.inputMetal]}
                  value={bmImportText} onChangeText={setBmImportText} placeholder="Paste UberSDR bookmarks (JSON or YAML) here…" placeholderTextColor={winDim} autoCorrect={false} autoCapitalize="none" multiline />
                {pt.metal ? (
                  <PopupKey label="CONFIRM IMPORT" primary height={32} fontSize={11} style={{ marginBottom: 8 }} onPress={() => { const msg = onImportBookmarks?.(bmImportText, bmAll) ?? ''; setBmImportMsg(msg); if (msg.startsWith('Imported')) setBmImportText(''); }} />
                ) : (
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrBrt }]} onPress={() => { const msg = onImportBookmarks?.(bmImportText, bmAll) ?? ''; setBmImportMsg(msg); if (msg.startsWith('Imported')) setBmImportText(''); }}>
                  <Text style={[st.bmBtnText, { color: keyOn }]}>CONFIRM IMPORT</Text>
                </TouchableOpacity>
                )}
                {!!onImportToServer && (
                  pt.metal ? (
                  <PopupKey label="⚿ CONFIRM IMPORT TO THE RECEIVER" primary height={32} fontSize={11} style={{ marginBottom: 8 }} disabled={!!bmBusy} onPress={importTextToServer} />
                ) : (
                <TouchableOpacity style={[st.bmBtn, { borderColor: bdrBrt }]} disabled={!!bmBusy} onPress={importTextToServer}>
                    <Text style={[st.bmBtnText, { color: keyOn }]}>⚿ CONFIRM IMPORT TO THE RECEIVER</Text>
                  </TouchableOpacity>
                )
                )}
              </>)}
              </>)}
              <View style={{ height: 12 }} />
            </ScrollView>
          )}
        </View>
      </KeyboardAvoidingView>
    </Modal>
  );
}

const makeSt = (pt: PopupTokens) => StyleSheet.create({
  backdrop:     { ...StyleSheet.absoluteFill },
  // Anchor near the bottom (over the control pill) so it's thumb-reachable on
  // big phones; the auto-opened keyboard then sits just below it.
  center:       { ...StyleSheet.absoluteFill, justifyContent: 'flex-end', alignItems: 'center' },
  modal:        { backgroundColor: CARD_BG, borderWidth: 1, borderRadius: 12, padding: 20, width: '90%', maxWidth: 360 },
  title:        { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 14 },
  vtsRow:       onMetal(pt, { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 10 }, { marginBottom: 6 }),
  // ★ A transparent border at rest, so the keyboard's ring does not shift the station name when it lands.
  vtsArrow:     { paddingHorizontal: 6, paddingVertical: 0, borderWidth: 2, borderColor: 'transparent', borderRadius: 6 },
  vtsArrowText: { fontSize: 20, fontWeight: 'bold' },
  // ★ ► mirrored, not ◄: Apple draws the two from different fallback fonts at different sizes (see VTSBar).
  vtsArrowLeft: { transform: [{ scaleX: -1 }] },
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
  magicRow:     { marginTop: 8 },
  // ★ Default chassis: the box's edge in the CONTROLS colour, what is typed in the TEXT colour (2026-10-12).
  magicInput:   onMetal(pt, { borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6,
                  paddingHorizontal: 10, paddingVertical: 7, color: pt.ink.text,
                  fontFamily: 'Atkinson Hyperlegible', fontSize: 14 }, { ...windowStyle(pt), color: pt.readout }),
  magicHint:    onMetal(pt, { color: pt.ink.textDim, fontFamily: 'Atkinson Hyperlegible', fontSize: 11,
                  marginTop: 4 }, engraveText(pt, pt.note)),
  cancelBtn:    { flex: 1, borderWidth: 1, borderRadius: 3, alignItems: 'center' },
  tuneBtn:      { flex: 2, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderRadius: 3, alignItems: 'center' },
  // Bookmarks mode (§4.2)
  segHeader:    { flexDirection: 'row', marginBottom: 12 },
  segTab:       { flex: 1, alignItems: 'center', paddingVertical: 8, borderBottomWidth: 2 },
  // ★ Fixed width so it takes the same bite out of both tabs rather than squeezing one of them,
  //   and no bottom border: it is not a third tab and must not read as one.
  segClose:     { width: 34, alignItems: 'center', justifyContent: 'center', paddingVertical: 8 },
  segTabText:   { fontSize: 12, letterSpacing: 2, fontWeight: '700' },
  bmScroll:     { maxHeight: 340 },
  searchInput:  { borderWidth: 1, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 8, fontSize: 14, marginBottom: 8, backgroundColor: 'rgba(255,255,255,0.04)' },
  bmImportBox:  { minHeight: 64, textAlignVertical: 'top' },
  bmMsg:        onMetal(pt, { fontSize: 12, marginBottom: 8, fontStyle: 'italic' }, engraveText(pt, pt.note)),
  bmHint:       onMetal(pt, { fontSize: 10, letterSpacing: 1, marginBottom: 4 }, engraveText(pt, pt.note)),
  searchRow:    { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 7, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.metal ? 'rgba(255,255,255,0.10)' : pt.ink.divider },
  searchFreq:   { fontSize: 12, fontWeight: '700', minWidth: 70 },
  searchMode:   { fontSize: 10, minWidth: 34 },
  searchName:   { flex: 1, fontSize: 12 },
  bmToggleRow:  { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 8, marginBottom: 4 },
  bmToggle:     { paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6, borderWidth: 1 },
  bmSub:        onMetal(pt, { fontSize: 10, letterSpacing: 1, marginTop: 12, marginBottom: 5 }, { ...engraveText(pt, pt.note), fontWeight: '700', letterSpacing: 1.4 }),
  bmSegRow:     { flexDirection: 'row', gap: 6, marginBottom: 8 },
  bmSeg:        { flex: 1, borderWidth: 1, borderRadius: 4, paddingVertical: 8, alignItems: 'center' },
  bmSegText:    { fontSize: 11, fontWeight: '600' },
  bmBtn:        { borderWidth: 1, borderRadius: 4, paddingVertical: 9, alignItems: 'center', marginBottom: 8 },
  bmBtnText:    { fontSize: 12, fontWeight: '700' },
  bmSaveRow:    { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.metal ? 'rgba(255,255,255,0.10)' : pt.ink.divider },
  bmName2:      { fontSize: 13, fontWeight: '600' },
  bmFreq2:      { fontSize: 10, marginTop: 1 },
  bmDel:        { fontSize: 16, paddingHorizontal: 6 },
  bmCloud:      { fontSize: 14, paddingHorizontal: 6 },
  titleMetal: onMetal(pt, {}, { ...engraveText(pt), fontWeight: '700' }),
  segHeaderMetal: { flexDirection: 'row', gap: 6, alignItems: 'center', marginBottom: 10 },
  tuneWin: { paddingHorizontal: 10, paddingTop: 8, paddingBottom: 6, marginBottom: 10 },
  inputRowMetal: onMetal(pt, {}, { borderBottomWidth: 0, borderTopWidth: 1, borderTopColor: pt.winRule, marginBottom: 0, paddingTop: 4, paddingBottom: 2 }),
  unitBtnTextMetal: onMetal(pt, {}, { fontFamily: POPUP_FONT, fontSize: 12, fontWeight: '700' }),
  profileWin: { padding: 6 },
  inputMetal: onMetal(pt, {}, { backgroundColor: pt.window.bg, borderRadius: pt.window.radius }),
  listWin: { paddingHorizontal: 8, marginBottom: 8 },
  listWinEmpty: { display: 'none' },
});
