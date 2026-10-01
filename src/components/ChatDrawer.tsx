/**
 * ChatDrawer — slide-up chat panel.
 *
 * Two states:
 *   1. Join flow — callsign input + JOIN button
 *   2. Chat interface — message thread + input row
 *
 * Matches VibeSDR_Mockup_SAVE.html #lsv-chat-drawer exactly.
 * Chat button pulses blue in ControlsBar when chatUnread=true.
 */

import React, {
  useCallback, useEffect, useRef, useState,
} from 'react';
import {
  Animated,
  Dimensions,
  FlatList,
  KeyboardAvoidingView,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
  type LayoutChangeEvent,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useTheme } from '../contexts/ThemeContext';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, PopupHandle, PopupWindow, POPUP_FONT, type PopupTokens,
} from './PopupShell';
import type { ChatUserRow } from '../services/DecoderClient';
import { phrasePadMaxHeight } from '../constants/chatPad';
import type { ShareOut, SharedStation } from '../services/chatShare';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface ChatMessage {
  id:     string;
  type:   'own' | 'other' | 'system';
  user?:  string;
  text:   string;
  ts:     string; // "HHMMz"
  /** ★ A station somebody shared (canned chat) — the SERVER's line, drawn with a TUNE key. */
  share?: SharedStation;
}

/** One row of the share picker. `title` may be the user's OWN bookmark label — shown on their own
 *  screen only; what is sent is `out`, which chatShare built without it. */
export interface ShareItem { key: string; title: string; detail?: string; out: ShareOut }

export interface ChatDrawerProps {
  visible:    boolean;
  messages:   ChatMessage[];
  myCallsign: string | null;          // null = not yet joined
  onJoin:     (callsign: string) => void;
  onSend:     (text: string) => void;
  onClose:    () => void;
  /** Re-open the name entry to change handle (e.g. after a server rename clash). */
  onChangeName?: () => void;
  onMute?:    () => void;
  muted?:     boolean;
  /** Active user list (chat_active_users) + tune/zoom sync controls */
  users?:            ChatUserRow[];
  syncedUser?:       string | null;
  zoomSync?:         boolean;
  onToggleSync?:     (username: string) => void;
  onToggleZoomSync?: () => void;
  onUserTap?:        (user: ChatUserRow) => void;
  /** OWRX = basic text chat: hide the active-users panel + tune/zoom-sync UI. */
  textOnly?:         boolean;
  /** ★★★ CANNED MODE — a shared-VFO VibeServer. When this is set there is NO text input and NO
   *  join: the vocabulary is fixed, ids travel on the wire, and people are ordinals ("User 3").
   *  That is what removes the moderation burden, the abuse vector and the translation problem in
   *  one go, and it is the only reason this can exist on a receiver run by one person who is
   *  asleep. It also makes the chat usable on a WATCH, where a keyboard cannot be.
   *  ★ The drawer chrome is deliberately unchanged: same open/close, same unread pulse, same
   *    transcript. Only the way you SPEAK differs, so there is one chat in this app, not two. */
  canned?:           { id: string; text: string }[];
  /** Send a canned phrase id (canned mode only). */
  onSay?:            (id: string) => void;
  /** One line about the room — who is tuning, how many are here, why it is not moving. */
  dialLine?:         string;
  /** ★★ SHARE A STATION (canned mode): what this user may share — what is playing now, then their
   *  own bookmarks for this receiver. Absent = no Share chip (a chat without canned mode has a text
   *  box, and FM-DX has its own chat). */
  shareItems?:       ShareItem[];
  /** Send one (the frame chatShare built — never a label). */
  onShare?:          (out: ShareOut) => void;
  /** TUNE on a shared line — a USER action, through the host's ordinary tune path. */
  onShareTune?:      (s: SharedStation) => void;
}

function fmtUserFreq(hz?: number): string {
  if (!hz || hz <= 0) return '';
  return (hz / 1_000_000).toFixed(hz % 1000 === 0 ? 3 : 4) + ' MHz';
}

// ── Constants ──────────────────────────────────────────────────────────────────

const C = {
  bg:       'rgba(6,4,2,0.99)',
  border:   'rgba(255,160,0,0.30)',
  goldDim:  '#c8893a',
  btnBg:    'rgba(20,10,0,0.80)',
  btnBdr:   'rgba(255,160,0,0.35)',
  inputBg:  'rgba(15,10,0,0.90)',
  inputBdr: 'rgba(255,160,0,0.28)',
  msgBdr:   'rgba(255,160,0,0.05)',
  timeCl:   'rgba(150,130,80,0.55)',
  userCl:   '#c8893a',
  ownCl:    'rgba(255,200,80,0.90)',
  textCl:   'rgba(220,210,190,0.90)',
  sysCl:    'rgba(180,160,100,0.55)',
  handle:   'rgba(255,160,0,0.35)',
};

const FONT = 'Atkinson Hyperlegible';
const { height: SCREEN_H } = Dimensions.get('window');
const DRAWER_H = Math.min(SCREEN_H * 0.55, 480);
const HANDLE_H = 32;

// ── Component ──────────────────────────────────────────────────────────────────

function ChatDrawerBody({
  visible, messages, myCallsign,
  onJoin, onSend, onClose, onChangeName,
  onMute, muted = false,
  users = [], syncedUser = null, zoomSync = false,
  onToggleSync, onToggleZoomSync, onUserTap, textOnly = false, canned, onSay, dialLine,
  shareItems, onShare, onShareTune,
}: ChatDrawerProps) {
  const cd = usePopupStyles(makeCd);
  const pt = usePopupTheme();
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  // White-aware colour overrides — backgrounds stay dark
  const cc = {
    border:  isWhite ? 'rgba(255,255,255,0.25)' : C.border,
    title:   isWhite ? 'rgba(255,255,255,0.55)' : pt.gold.amberA(0.40),
    btnBdr:  isWhite ? 'rgba(255,255,255,0.30)' : C.btnBdr,
    btnText: isWhite ? '#ffffff' : pt.gold.amber,
    inputBdr:isWhite ? 'rgba(255,255,255,0.22)' : C.inputBdr,
    inputCl: isWhite ? '#ffffff' : pt.gold.amber,
    userCl:  isWhite ? '#b0c8ff' : C.userCl,
    ownCl:   isWhite ? '#ffe566' : C.ownCl,
    textCl:  isWhite ? 'rgba(240,240,240,0.90)' : C.textCl,
    timeCl:  isWhite ? 'rgba(180,190,210,0.50)' : C.timeCl,
    sysCl:   isWhite ? 'rgba(180,190,210,0.55)' : C.sysCl,
    handle:  isWhite ? 'rgba(255,255,255,0.25)' : C.handle,
  };
  // ★★ SILVER / BLACK (§10.3): text ON the plate is engraved (title, the user list, the room line);
  //   the thread and every input sit in recessed windows, where chat's MEANING colours stay — own
  //   name blue, others amber, system grey italic — lit on the dark glass. Only the palette changes
  //   here; the default chassis reads `cc` exactly as before.
  if (pt.metal) {
    Object.assign(cc, {
      border: 'transparent', title: pt.label, btnText: pt.legend, inputCl: pt.winText,
      userCl: pt.chatOther, ownCl: pt.chatOwn, textCl: pt.chatText, timeCl: pt.winDim, sysCl: pt.chatSys,
    });
  }
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(14, true);
  const ff = pt.metal ? POPUP_FONT : t.font;
  const insets = useSafeAreaInsets();
  const translateY = useRef(new Animated.Value(DRAWER_H)).current;
  const backdropOp = useRef(new Animated.Value(0)).current;
  const [nameInput, setNameInput] = useState('');
  const [msgInput,  setMsgInput]  = useState('');
  const [showUsers, setShowUsers] = useState(false);
  const listRef = useRef<FlatList>(null);
  // ★★ CANNED MODE IS ALWAYS "JOINED". Every gate below asked `myCallsign`, which conflated two
  //    different questions: "may I speak?" and "have I chosen a name?". On a shared dial there are
  //    no names at all — the server hands out ordinals — so asking for one would be a join flow
  //    with nothing to type into it, and the transcript would never render.
  const isCanned = !!canned && canned.length > 0;
  /** ★ The pad shows the phrases OR the share picker — never both: the picker is a list, and a list
   *  inside a wrap of chips is a pad nobody can read. Closes itself on a send and on closing the drawer. */
  const [picking, setPicking] = useState(false);
  useEffect(() => { if (!visible) setPicking(false); }, [visible]);
  const joined = isCanned || !!myCallsign;
  // ★★ The phrase pad's cap (constants/chatPad.ts) comes off the drawer's body: its fixed height
  //    less the bottom inset + padding, the handle and the header (measured — metal keys are taller
  //    than the default's glyphs), and the room line above the chips.
  const [headerH, setHeaderH] = useState(36);
  const [lineH, setLineH] = useState(dialLine ? 18 : 0);
  const padMaxH = phrasePadMaxHeight(DRAWER_H - insets.bottom - 8 - HANDLE_H - headerH, dialLine ? lineH + 6 : 0);
  const nameRef = useRef<TextInput>(null);
  const msgRef  = useRef<TextInput>(null);

  // ── Animation ──────────────────────────────────────────────────────────────

  useEffect(() => {
    if (visible) {
      Animated.parallel([
        Animated.timing(backdropOp, { toValue: 1, duration: 200, useNativeDriver: true }),
        Animated.spring(translateY, { toValue: 0, damping: 24, stiffness: 220, useNativeDriver: true }),
      ]).start(() => {
        // Focus appropriate input after open animation
        if (isCanned) { /* no text input anywhere in canned mode — nothing to focus */ }
        else if (!myCallsign) nameRef.current?.focus();
        else             msgRef.current?.focus();
      });
    } else {
      Animated.parallel([
        Animated.timing(backdropOp, { toValue: 0, duration: 160, useNativeDriver: true }),
        Animated.timing(translateY, { toValue: DRAWER_H, duration: 200, useNativeDriver: true }),
      ]).start();
    }
  }, [visible, backdropOp, translateY, myCallsign]);

  // ── Scroll to bottom on new message ───────────────────────────────────────

  // ★★ Only while the reader is at the bottom (B11, Stuart: scrolling up to read history snapped back
  //    on every new line — the decoder box had the same fault). Opening the drawer follows again.
  const followTail = useRef(true);
  useEffect(() => { if (visible) followTail.current = true; }, [visible]);
  useEffect(() => {
    if (visible && messages.length > 0 && followTail.current) {
      setTimeout(() => listRef.current?.scrollToEnd({ animated: true }), 60);
    }
  }, [messages.length, visible]);

  // Server username rules: 1–15 chars, letters/digits plus - _ / inside,
  // NO spaces, mixed case preserved (capitals not required)
  const handleJoin = useCallback(() => {
    const cs = nameInput.replace(/[^A-Za-z0-9\-_\/]/g, '').slice(0, 15);
    if (!cs) return;
    onJoin(cs);
    setNameInput('');
  }, [nameInput, onJoin]);

  const handleSend = useCallback(() => {
    const t = msgInput.trim();
    if (!t) return;
    onSend(t);
    setMsgInput('');
  }, [msgInput, onSend]);

  if (!visible) return null;

  return (
    <>
      {/* Backdrop */}
      <Animated.View
        // ★★★ Transparency OFF: no dim over the live waterfall — the view (and its tap-to-close)
        //   stays, invisible.
        style={[StyleSheet.absoluteFill, !surf.opaque && cd.backdrop, { opacity: backdropOp }]}
        pointerEvents={visible ? 'auto' : 'none'}
        onStartShouldSetResponder={() => { onClose(); return true; }}
      />

      {/* Drawer */}
      <KeyboardAvoidingView
        behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
        style={cd.kavWrap}
        pointerEvents="box-none"
      >
        {/* ★★ Side insets too: a landscape phone's nav bar (Android) or notch (iOS) sits over the
            drawer's right / left edge, and it had the ✕ key and the longest phrase's end under it —
            seen, and not tappable. The plate still runs edge to edge; only the contents step in. */}
        <Animated.View style={[cd.drawer, { borderTopColor: cc.border, paddingBottom: insets.bottom + 8,
                                            paddingLeft: insets.left, paddingRight: insets.right, transform: [{ translateY }] },
                               surf.opaque && !pt.metal && { backgroundColor: surf.fill(C.bg) }, surf.shadow, metalFrame]}>
          <PopupPlate radius={14} />

          {/* Handle */}
          <TouchableOpacity style={cd.handle} onPress={onClose} hitSlop={12} activeOpacity={0.7}>
            <PopupHandle><View style={[cd.handleBar, { backgroundColor: cc.handle }]} /></PopupHandle>
          </TouchableOpacity>

          {/* Header */}
          <View style={[cd.header, { borderBottomColor: cc.border }]}
                onLayout={(e: LayoutChangeEvent) => setHeaderH(Math.ceil(e.nativeEvent.layout.height))}>
            {!showUsers && !isCanned && myCallsign && onChangeName ? (
              <TouchableOpacity onPress={onChangeName} hitSlop={8} activeOpacity={0.6}>
                <Text style={[cd.title, { color: cc.title, fontFamily: ff }, cd.engrave]}>
                  CHAT · {myCallsign} <Text style={{ color: cc.btnText }}>✎</Text>
                </Text>
              </TouchableOpacity>
            ) : (
              <Text style={[cd.title, { color: cc.title, fontFamily: ff }, cd.engrave]}>
                {showUsers ? `USERS · ${users.length}`
                  : isCanned ? 'CHAT'
                  : myCallsign ? `CHAT · ${myCallsign}` : 'CHAT'}
              </Text>
            )}
            {pt.metal ? (<>
              {/* ★ Dome keys: 👥 and 🔍 are toggles (their pips light while on), 🔔 lights while
                  alerts are ON, ✕ is a plain key. */}
              {joined && !textOnly && !isCanned && (
                <PopupKey label="👥" active={showUsers} pip height={28} fontSize={14} style={cd.hkey}
                  accessibilityLabel="Users" onPress={() => setShowUsers((p: boolean) => !p)} hitSlop={8} />
              )}
              {showUsers && (
                <PopupKey label="🔍" active={zoomSync} pip height={28} fontSize={14} style={cd.hkey}
                  accessibilityLabel="Follow zoom" onPress={onToggleZoomSync} hitSlop={8} />
              )}
              <PopupKey label={muted ? '🔇' : '🔔'} active={!muted} pip height={28} fontSize={14} style={cd.hkey}
                accessibilityLabel={muted ? 'Chat alerts off' : 'Chat alerts on'} onPress={onMute} hitSlop={8} />
              <PopupKey label="✕" height={28} fontSize={13} style={cd.hkey} accessibilityLabel="Close"
                onPress={onClose} hitSlop={8} />
            </>) : (<>
            {joined && !textOnly && !isCanned && (
              <TouchableOpacity style={cd.hbtn} onPress={() => setShowUsers((p: boolean) => !p)} hitSlop={8}>
                <Text style={[cd.hbtnTxt, { color: cc.btnText }, showUsers && cd.hbtnActive]}>👥</Text>
              </TouchableOpacity>
            )}
            {showUsers && (
              <TouchableOpacity style={cd.hbtn} onPress={onToggleZoomSync} hitSlop={8}>
                <Text style={[cd.hbtnTxt, { color: cc.btnText }, !zoomSync && cd.hbtnMuted]}>🔍</Text>
              </TouchableOpacity>
            )}
            <TouchableOpacity style={cd.hbtn} onPress={onMute} hitSlop={8}>
              <Text style={[cd.hbtnTxt, { color: cc.btnText }, muted && cd.hbtnMuted]}>{muted ? '🔇' : '🔔'}</Text>
            </TouchableOpacity>
            <TouchableOpacity style={cd.hbtn} onPress={onClose} hitSlop={8}>
              <Text style={[cd.hbtnTxt, { color: 'rgba(255,120,120,0.70)' }]}>✕</Text>
            </TouchableOpacity>
            </>)}
          </View>

          {/* Join flow — never in canned mode: there are no names to choose. */}
          {!joined && (
            <View style={cd.setupWrap}>
              <Text style={[cd.setupLbl, { color: cc.title, fontFamily: ff }, cd.engrave]}>
                Enter your callsign or handle to join
              </Text>
              <View style={cd.setupRow}>
                <TextInput
                  ref={nameRef}
                  style={[cd.nameInp, { borderColor: cc.inputBdr, color: cc.inputCl, fontFamily: ff }, cd.inputMetal]}
                  value={nameInput}
                  onChangeText={(v: string) => setNameInput(v.replace(/\s+/g, ''))}
                  placeholder="Callsign / handle"
                  placeholderTextColor={pt.metal ? pt.winDim : isWhite ? 'rgba(255,255,255,0.25)' : 'rgba(255,160,0,0.28)'}
                  autoCapitalize="none"
                  autoCorrect={false}
                  maxLength={15}
                  returnKeyType="done"
                  onSubmitEditing={handleJoin}
                />
                {pt.metal ? (
                  <PopupKey label="JOIN" primary onPress={handleJoin} height={40} fontSize={12} style={{ minWidth: 72 }} />
                ) : (
                <TouchableOpacity
                  style={[cd.joinBtn, { borderColor: cc.btnBdr }]}
                  onPress={handleJoin} activeOpacity={0.75}
                >
                  <Text style={[cd.joinBtnTxt, { color: cc.btnText, fontFamily: t.font }]}>JOIN</Text>
                </TouchableOpacity>
                )}
              </View>
            </View>
          )}

          {/* Active users — tap a row to jump to their tune once, SYNC to
              follow continuously; 🔍 in the header also mirrors their zoom */}
          {joined && showUsers && (
            <FlatList
              data={users}
              keyExtractor={(u: ChatUserRow) => u.username}
              style={cd.msgList}
              contentContainerStyle={cd.msgContent}
              renderItem={({ item: u }: { item: ChatUserRow }) => {
                const isMe = u.username === myCallsign;
                const isSynced = syncedUser === u.username;
                return (
                  <TouchableOpacity
                    style={[cd.userRow, u.is_idle && cd.userRowIdle]}
                    activeOpacity={0.7}
                    disabled={isMe}
                    onPress={() => onUserTap?.(u)}
                  >
                    <Text style={[cd.userName, { color: pt.metal ? (isMe ? pt.chatOwnPlate : pt.chatOtherPlate) : isMe ? cc.ownCl : cc.userCl,
                                               fontFamily: ff }, cd.engraveName]} numberOfLines={1}>
                      {u.username}{u.country_code ? `  ·${u.country_code}` : ''}{u.tx ? ' 📡TX' : ''}
                    </Text>
                    <Text style={[cd.userFreq, { color: pt.metal ? pt.label : cc.textCl, fontFamily: ff }, cd.engrave]} numberOfLines={1}>
                      {fmtUserFreq(u.frequency)}{u.mode ? ` ${u.mode.toUpperCase()}` : ''}
                      {u.is_idle && u.idle_minutes ? `  idle ${u.idle_minutes}m` : ''}
                    </Text>
                    {!isMe && pt.metal ? (
                      <PopupKey label={isSynced ? 'SYNCED' : 'SYNC'} active={isSynced} pip height={26} fontSize={10}
                        style={{ width: 70 }} hitSlop={6} onPress={() => onToggleSync?.(u.username)} />
                    ) : !isMe && (
                      <TouchableOpacity
                        style={[cd.syncBtn, isSynced && cd.syncBtnOn]}
                        onPress={() => onToggleSync?.(u.username)}
                        hitSlop={6}
                      >
                        <Text style={[cd.syncBtnTxt, { fontFamily: t.font }, isSynced && cd.syncBtnTxtOn]}>
                          {isSynced ? 'SYNCED' : 'SYNC'}
                        </Text>
                      </TouchableOpacity>
                    )}
                  </TouchableOpacity>
                );
              }}
            />
          )}

          {/* Message list */}
          {joined && !showUsers && (
            <PopupWindow style={cd.msgList} metalStyle={cd.threadWin}>
            <FlatList
              ref={listRef}
              data={messages}
              keyExtractor={(m: ChatMessage) => m.id}
              style={cd.msgList}
              contentContainerStyle={cd.msgContent}
              showsVerticalScrollIndicator
              onContentSizeChange={() => { if (followTail.current) listRef.current?.scrollToEnd({ animated: false }); }}
              scrollEventThrottle={32}
              onScrollBeginDrag={() => { followTail.current = false; }}
              onScroll={(e: any) => {
                const n = e?.nativeEvent;
                if (n?.contentSize && n?.layoutMeasurement)
                  followTail.current = n.contentSize.height - (n.contentOffset.y + n.layoutMeasurement.height) < 24;
              }}
              renderItem={({ item: m }: { item: ChatMessage }) => (
                <View style={[cd.msg, m.type === 'system' && cd.msgSystem]}>
                  <Text style={[cd.msgTime, { color: cc.timeCl, fontFamily: ff }]}>{m.ts}</Text>
                  {m.type !== 'system' && (
                    <Text style={[cd.msgUser, { color: m.type === 'own' ? cc.ownCl : cc.userCl, fontFamily: ff }]}>
                      {m.user}
                    </Text>
                  )}
                  <Text style={[
                    cd.msgText,
                    { color: cc.textCl, fontFamily: ff },
                    m.type === 'system' && { color: cc.sysCl },
                    // ★ Metal: system lines in the mockup's grey italic (a meaning, not a look).
                    m.type === 'system' && pt.metal && cd.msgTextSystem,
                  ]} selectable>
                    {m.text}
                  </Text>
                  {/* ★★ TUNE is the RECEIVER's choice — arriving never moves anything. On a shared dial
                       the host asks first when somebody else is on it (chatShare.shareTuneStep). */}
                  {!!m.share && !!onShareTune && (pt.metal ? (
                    <PopupKey label="TUNE" height={24} fontSize={10} style={cd.tuneKey} hitSlop={6}
                      accessibilityLabel="Tune to the shared station" onPress={() => onShareTune?.(m.share!)} />
                  ) : (
                    <TouchableOpacity style={[cd.tuneBtn, { borderColor: cc.btnBdr }]} hitSlop={6} activeOpacity={0.75}
                      accessibilityLabel="Tune to the shared station" onPress={() => onShareTune?.(m.share!)}>
                      <Text style={[cd.tuneTxt, { color: cc.btnText, fontFamily: t.font }]}>TUNE</Text>
                    </TouchableOpacity>
                  ))}
                </View>
              )}
            />
            </PopupWindow>
          )}

          {/* ★★ THE PHRASE PAD — canned mode's entire means of speaking. Wrapped, not a row: the
               longest line ("I'm running a decoder — can you wait please?") must not force a
               horizontal scroll on the narrowest phone.
               ★★★ …and SCROLLED, inside a cap (constants/chatPad.ts). Fourteen phrases are taller
               than a phone's drawer: laid straight in, they squeezed the transcript to nothing and
               put phrases 7–14 below the drawer's edge, where no finger could reach them. Same
               chips, same wrap, same look — only now in a ScrollView no taller than `padMaxH`. */}
          {isCanned && (
            <View style={[cd.inputRow, cd.padWrap, { borderTopColor: cc.border }]}>
              {!!dialLine && (
                <Text style={[cd.cannedLine, { color: pt.metal ? pt.note : cc.title, fontFamily: ff }, cd.engrave]}
                      numberOfLines={2} onLayout={(e: LayoutChangeEvent) => setLineH(Math.ceil(e.nativeEvent.layout.height))}>
                  {dialLine}
                </Text>
              )}
              <ScrollView style={[cd.padScroll, { maxHeight: padMaxH }]} contentContainerStyle={cd.padContent}
                showsVerticalScrollIndicator keyboardShouldPersistTaps="handled">
              {/* ★★ SHARE A STATION — first, because it is the one key that says WHAT you found. */}
              {isCanned && !!shareItems && !picking && (pt.metal ? (
                <PopupKey key="__share" label="📻 Share a station…" numberOfLines={1} height={32} fontSize={12} hitSlop={0}
                  style={{ alignSelf: 'flex-start', maxWidth: '100%' }} onPress={() => setPicking(true)} />
              ) : (
                <TouchableOpacity key="__share" style={[cd.cannedBtn, { borderColor: cc.btnBdr }]} activeOpacity={0.75}
                  onPress={() => setPicking(true)}>
                  <Text style={[cd.cannedTxt, { color: cc.btnText, fontFamily: t.font }]}>📻 Share a station…</Text>
                </TouchableOpacity>
              ))}
              {/* ★★★ THE PICKER: what is playing now, then this user's own bookmarks for this receiver.
                   Their labels are shown HERE, on their own screen; what is sent is the row's `out`,
                   which carries no label — the room hears the name this receiver knows. */}
              {picking && (<>
                {pt.metal ? (
                  <PopupKey key="__back" label="‹ Phrases" height={28} fontSize={11} hitSlop={0}
                    style={{ alignSelf: 'flex-start' }} onPress={() => setPicking(false)} />
                ) : (
                  <TouchableOpacity key="__back" style={[cd.cannedBtn, { borderColor: cc.btnBdr }]} activeOpacity={0.75}
                    onPress={() => setPicking(false)}>
                    <Text style={[cd.cannedTxt, { color: cc.btnText, fontFamily: t.font }]}>‹ Phrases</Text>
                  </TouchableOpacity>
                )}
                <Text style={[cd.cannedLine, { color: pt.metal ? pt.note : cc.title, fontFamily: ff }, cd.engrave]}>
                  Shares the frequency and mode only — the room sees the name this receiver knows.
                </Text>
                {(shareItems ?? []).length === 0 && (
                  <Text style={[cd.cannedLine, { color: pt.metal ? pt.note : cc.title, fontFamily: ff }, cd.engrave]}>
                    Nothing to share yet — tune a station or save a bookmark.
                  </Text>
                )}
                {(shareItems ?? []).map(it => pt.metal ? (
                  <PopupKey key={it.key} label={it.detail ? `${it.title}  ·  ${it.detail}` : it.title} numberOfLines={2}
                    height={32} fontSize={12} hitSlop={0} style={{ width: '100%' }}
                    onPress={() => { onShare?.(it.out); setPicking(false); }} />
                ) : (
                  <TouchableOpacity key={it.key} style={[cd.cannedBtn, cd.shareRow, { borderColor: cc.btnBdr }]} activeOpacity={0.75}
                    onPress={() => { onShare?.(it.out); setPicking(false); }}>
                    <Text style={[cd.cannedTxt, { color: cc.btnText, fontFamily: t.font }]} numberOfLines={1}>{it.title}</Text>
                    {!!it.detail && (
                      <Text style={[cd.shareDetail, { color: cc.title, fontFamily: t.font }]} numberOfLines={1}>{it.detail}</Text>
                    )}
                  </TouchableOpacity>
                ))}
              </>)}
              {!picking && canned!.map(ph => pt.metal ? (
                // ★★ hitSlop 0: the chips sit 6 pt apart and a dome key's default 4 pt slop on BOTH
                //    neighbours overlapped in the gap — a tap there could say either phrase.
                <PopupKey key={ph.id} label={ph.text} numberOfLines={3} height={32} fontSize={12} hitSlop={0}
                  style={{ alignSelf: 'flex-start', maxWidth: '100%' }} onPress={() => onSay?.(ph.id)} />
              ) : (
                <TouchableOpacity
                  key={ph.id}
                  style={[cd.cannedBtn, { borderColor: cc.btnBdr }]}
                  activeOpacity={0.75}
                  onPress={() => onSay?.(ph.id)}
                >
                  <Text style={[cd.cannedTxt, { color: cc.btnText, fontFamily: t.font }]}>
                    {ph.text}
                  </Text>
                </TouchableOpacity>
              ))}
              </ScrollView>
            </View>
          )}

          {/* Input row */}
          {joined && !isCanned && (
            <View style={[cd.inputRow, { borderTopColor: cc.border }]}>
              <Text style={[cd.meLbl, { color: cc.title, fontFamily: ff }, cd.engrave]} numberOfLines={1}>
                {myCallsign}
              </Text>
              <TextInput
                ref={msgRef}
                style={[cd.msgInp, { borderColor: cc.inputBdr, color: cc.inputCl, fontFamily: ff }, cd.inputMetal]}
                value={msgInput}
                onChangeText={setMsgInput}
                placeholder="Message…"
                placeholderTextColor={pt.metal ? pt.winDim : isWhite ? 'rgba(255,255,255,0.25)' : 'rgba(255,160,0,0.25)'}
                returnKeyType="send"
                onSubmitEditing={handleSend}
                maxLength={250}
                multiline={false}
                blurOnSubmit={false}
              />
              {pt.metal ? (
                // ★ §10.3: send is the primary action — its legend lit in the controls colour.
                <PopupKey label="➤" primary onPress={handleSend} height={34} fontSize={15}
                  accessibilityLabel="Send" style={{ width: 44, paddingHorizontal: 0 }} />
              ) : (
              <TouchableOpacity
                style={[cd.sendBtn, { borderColor: cc.btnBdr }]}
                onPress={handleSend} activeOpacity={0.75}
              >
                <Text style={[cd.sendBtnTxt, { color: cc.btnText, fontFamily: t.font }]}>▶</Text>
              </TouchableOpacity>
              )}
            </View>
          )}

        </Animated.View>
      </KeyboardAvoidingView>
    </>
  );
}

// ── Styles ────────────────────────────────────────────────────────────────────

const makeCd = (pt: PopupTokens) => StyleSheet.create({
  backdrop: { backgroundColor: 'rgba(0,0,0,0.55)' },
  kavWrap:  { position: 'absolute', left: 0, right: 0, bottom: 0, top: 0, justifyContent: 'flex-end', pointerEvents: 'box-none' },
  drawer: {
    backgroundColor: C.bg,
    borderTopWidth: 1, borderTopColor: C.border,
    borderTopLeftRadius: 14, borderTopRightRadius: 14,
    height: DRAWER_H,
    shadowColor: '#000', shadowOffset: { width: 0, height: -6 },
    shadowOpacity: 0.70, shadowRadius: 12, elevation: 20,
  },
  handle: { alignItems: 'center', justifyContent: 'center', height: HANDLE_H },
  handleBar: { width: 36, height: 4, borderRadius: 2, backgroundColor: C.handle },
  header: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, paddingBottom: 8, gap: 6 },
  title:  onMetal(pt, { flex: 1, color: 'rgba(255,160,0,0.60)', fontFamily: FONT, fontSize: 11, letterSpacing: 2 }, { fontSize: 11, letterSpacing: 2.2, fontWeight: '700' }),
  hbtn:   { padding: 4 },
  hbtnTxt:    { color: 'rgba(255,160,0,0.55)', fontSize: 16 },
  hbtnMuted:  { opacity: 0.35 },
  hbtnActive: { opacity: 1 },
  hbtnClose:  { color: '#e05050' },

  userRow: {
    flexDirection: 'row', alignItems: 'center', gap: 8,
    paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.msgBdr,
  },
  userRowIdle: { opacity: 0.45 },
  userName: { flexShrink: 1, fontSize: 13, fontWeight: 'bold', letterSpacing: 0.5 },
  userFreq: { flex: 1, fontSize: 11, textAlign: 'right' },
  syncBtn: {
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.40)', borderRadius: 5,
    paddingHorizontal: 8, paddingVertical: 4, flexShrink: 0,
  },
  syncBtnOn: {
    borderColor: 'rgba(80,220,100,0.70)', backgroundColor: 'rgba(80,220,100,0.12)',
  },
  syncBtnTxt:   { fontSize: 10, letterSpacing: 1, color: pt.gold.amberA(0.80) },
  syncBtnTxtOn: { color: 'rgba(120,235,140,0.95)' },

  setupWrap: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20, gap: 10 },
  setupLbl:  { color: 'rgba(200,180,100,0.75)', fontFamily: FONT, fontSize: 11, letterSpacing: 1.5, textAlign: 'center' },
  setupRow:  { flexDirection: 'row', gap: 8, width: '100%', maxWidth: 380 },
  nameInp: {
    flex: 1, backgroundColor: 'rgba(20,12,0,0.90)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)',
    borderRadius: 6, paddingHorizontal: 12, paddingVertical: 10,
    fontFamily: FONT, fontSize: 14, letterSpacing: 1, color: pt.gold.amber,
  },
  joinBtn: {
    backgroundColor: 'rgba(255,160,0,0.12)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.45)',
    borderRadius: 6, paddingHorizontal: 18, paddingVertical: 10,
    justifyContent: 'center', alignItems: 'center',
  },
  joinBtnTxt: { fontFamily: FONT, fontSize: 12, letterSpacing: 1, color: pt.gold.amber },

  msgList:    { flex: 1 },
  msgContent: { paddingHorizontal: 14, paddingVertical: 4 },
  msg: {
    flexDirection: 'row', alignItems: 'baseline', gap: 6,
    paddingVertical: 3,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.msgBdr,
  },
  msgSystem:     {},
  msgTime:       { flexShrink: 0, fontSize: 9, color: C.timeCl, fontFamily: FONT, alignSelf: 'flex-start', paddingTop: 1 },
  msgUser:       { flexShrink: 0, fontSize: 11, fontWeight: 'bold', color: C.userCl, fontFamily: FONT, letterSpacing: 0.5 },
  msgUserOwn:    { color: C.ownCl },
  msgText:       { color: C.textCl, fontFamily: FONT, fontSize: 12, flex: 1, lineHeight: 18 },
  msgTextSystem: onMetal(pt, { color: C.sysCl, fontStyle: 'italic', fontSize: 10 }, { color: pt.chatSys }),

  inputRow: {
    flexDirection: 'row', alignItems: 'center', gap: 8,
    paddingHorizontal: 14, paddingVertical: 8,
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'rgba(255,160,0,0.10)',
  },
  meLbl:  { flexShrink: 0, fontFamily: FONT, fontSize: 10, color: 'rgba(200,160,60,0.70)', maxWidth: 80 },
  msgInp: {
    flex: 1, backgroundColor: C.inputBg,
    borderWidth: 1, borderColor: C.inputBdr,
    borderRadius: 20, paddingHorizontal: 14, paddingVertical: 8,
    fontFamily: FONT, fontSize: 13, color: '#ffe0a0', minWidth: 0,
  },
  sendBtn: {
    width: 36, height: 36, borderRadius: 18,
    backgroundColor: 'rgba(255,160,0,0.15)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.40)',
    alignItems: 'center', justifyContent: 'center', flexShrink: 0,
  },
  sendBtnTxt: { color: pt.gold.amber, fontSize: 16 },
  // ★★★ THE PHRASE PAD HAS ITS OWN STYLE, and this is why: it first reused `sendBtn`, which is a
  //     fixed 36×36 CIRCLE built for a single ▶ glyph. Twelve sentences in twelve circles came out
  //     as unreadable two-letter stacks — "C an", "A nyt", "Tu nir" (Stuart, on an iPad, 2026-08-20).
  //     A button that must hold a SENTENCE has to be sized by its text, never by a fixed box.
  // ★★ alignSelf 'flex-start' so a wrapped row does not stretch every pill to the tallest one, and
  //    no width cap at all: the longest phrase decides, and the row wraps when it must.
  cannedBtn: {
    alignSelf: 'flex-start',
    paddingHorizontal: 12, paddingVertical: 8, borderRadius: 14,
    backgroundColor: 'rgba(255,160,0,0.12)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)',
  },
  cannedTxt: { fontFamily: FONT, fontSize: 13, color: '#ffe0a0' },
  // ★ A picker row is a LIST row — full width, label over its frequency — not a chip in the wrap.
  shareRow:    { width: '100%', borderRadius: 10, paddingVertical: 6 },
  shareDetail: { fontFamily: FONT, fontSize: 11, marginTop: 1 },
  // ★ TUNE beside a shared line: small, after the text, never wider than its word.
  tuneBtn: {
    flexShrink: 0, alignSelf: 'center', paddingHorizontal: 8, paddingVertical: 3, borderRadius: 10,
    backgroundColor: 'rgba(255,160,0,0.12)', borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)',
  },
  tuneTxt: { fontFamily: FONT, fontSize: 10, letterSpacing: 1, color: '#ffe0a0' },
  tuneKey: { flexShrink: 0, alignSelf: 'center', paddingHorizontal: 8 },
  // ★★ The pad: the room line over a capped scroller of the same flowing wrap (constants/chatPad.ts).
  //    3 pt under the chips so the last row's cast shadow and its 2 pt of travel are not clipped by
  //    the scroller — the last chip used to sit hard on the drawer's edge.
  padWrap:    { flexDirection: 'column', alignItems: 'stretch', gap: 4 },
  padScroll:  { flexGrow: 0 },
  padContent: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingTop: 1, paddingBottom: 3 },
  // ★ The room line is a SENTENCE too — `meLbl` caps at 80px, which squeezed it into a column.
  cannedLine: onMetal(pt, {
    width: '100%', fontFamily: FONT, fontSize: 11,
    color: pt.gold.amberA(0.75), marginBottom: 2,
  }, { fontSize: 11 }),
  engrave: onMetal(pt, {}, engraveText(pt)),
  engraveName: onMetal(pt, {}, pt.engraveName ? { textShadowColor: pt.engraveName.color, textShadowOffset: { width: 0, height: pt.engraveName.dy }, textShadowRadius: 0.01 } : {}),
  inputMetal: onMetal(pt, {}, { ...windowStyle(pt), borderRadius: pt.window.radius }),
  hkey: { minWidth: 34, paddingHorizontal: 4 },
  threadWin: { marginHorizontal: 14, marginVertical: 4 },
});

/** ★ Same rule as MenuSheet: no hooks run for a drawer that is shut. */
export default function ChatDrawer(props: ChatDrawerProps) {
  if (!props.visible) return null;
  return <ChatDrawerBody {...props} />;
}
