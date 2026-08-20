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
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useTheme } from '../contexts/ThemeContext';
import type { ChatUserRow } from '../services/DecoderClient';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface ChatMessage {
  id:     string;
  type:   'own' | 'other' | 'system';
  user?:  string;
  text:   string;
  ts:     string; // "HHMMz"
}

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
}

function fmtUserFreq(hz?: number): string {
  if (!hz || hz <= 0) return '';
  return (hz / 1_000_000).toFixed(hz % 1000 === 0 ? 3 : 4) + ' MHz';
}

// ── Constants ──────────────────────────────────────────────────────────────────

const C = {
  bg:       'rgba(6,4,2,0.99)',
  border:   'rgba(255,160,0,0.30)',
  gold:     '#ffb833',
  goldDim:  '#c8893a',
  muted:    'rgba(255,184,51,0.40)',
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

// ── Component ──────────────────────────────────────────────────────────────────

export default function ChatDrawer({
  visible, messages, myCallsign,
  onJoin, onSend, onClose, onChangeName,
  onMute, muted = false,
  users = [], syncedUser = null, zoomSync = false,
  onToggleSync, onToggleZoomSync, onUserTap, textOnly = false, canned, onSay, dialLine,
}: ChatDrawerProps) {
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  // White-aware colour overrides — backgrounds stay dark
  const cc = {
    border:  isWhite ? 'rgba(255,255,255,0.25)' : C.border,
    title:   isWhite ? 'rgba(255,255,255,0.55)' : C.muted,
    btnBdr:  isWhite ? 'rgba(255,255,255,0.30)' : C.btnBdr,
    btnText: isWhite ? '#ffffff' : C.gold,
    inputBdr:isWhite ? 'rgba(255,255,255,0.22)' : C.inputBdr,
    inputCl: isWhite ? '#ffffff' : C.gold,
    userCl:  isWhite ? '#b0c8ff' : C.userCl,
    ownCl:   isWhite ? '#ffe566' : C.ownCl,
    textCl:  isWhite ? 'rgba(240,240,240,0.90)' : C.textCl,
    timeCl:  isWhite ? 'rgba(180,190,210,0.50)' : C.timeCl,
    sysCl:   isWhite ? 'rgba(180,190,210,0.55)' : C.sysCl,
    handle:  isWhite ? 'rgba(255,255,255,0.25)' : C.handle,
  };
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
  const joined = isCanned || !!myCallsign;
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

  useEffect(() => {
    if (visible && messages.length > 0) {
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
        style={[StyleSheet.absoluteFill, cd.backdrop, { opacity: backdropOp }]}
        pointerEvents={visible ? 'auto' : 'none'}
        onStartShouldSetResponder={() => { onClose(); return true; }}
      />

      {/* Drawer */}
      <KeyboardAvoidingView
        behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
        style={cd.kavWrap}
        pointerEvents="box-none"
      >
        <Animated.View style={[cd.drawer, { borderTopColor: cc.border, paddingBottom: insets.bottom + 8, transform: [{ translateY }] }]}>

          {/* Handle */}
          <TouchableOpacity style={cd.handle} onPress={onClose} hitSlop={12} activeOpacity={0.7}>
            <View style={[cd.handleBar, { backgroundColor: cc.handle }]} />
          </TouchableOpacity>

          {/* Header */}
          <View style={[cd.header, { borderBottomColor: cc.border }]}>
            {!showUsers && !isCanned && myCallsign && onChangeName ? (
              <TouchableOpacity onPress={onChangeName} hitSlop={8} activeOpacity={0.6}>
                <Text style={[cd.title, { color: cc.title, fontFamily: t.font }]}>
                  CHAT · {myCallsign} <Text style={{ color: cc.btnText }}>✎</Text>
                </Text>
              </TouchableOpacity>
            ) : (
              <Text style={[cd.title, { color: cc.title, fontFamily: t.font }]}>
                {showUsers ? `USERS · ${users.length}`
                  : isCanned ? 'CHAT'
                  : myCallsign ? `CHAT · ${myCallsign}` : 'CHAT'}
              </Text>
            )}
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
          </View>

          {/* Join flow — never in canned mode: there are no names to choose. */}
          {!joined && (
            <View style={cd.setupWrap}>
              <Text style={[cd.setupLbl, { color: cc.title, fontFamily: t.font }]}>
                Enter your callsign or handle to join
              </Text>
              <View style={cd.setupRow}>
                <TextInput
                  ref={nameRef}
                  style={[cd.nameInp, { borderColor: cc.inputBdr, color: cc.inputCl, fontFamily: t.font }]}
                  value={nameInput}
                  onChangeText={(v: string) => setNameInput(v.replace(/\s+/g, ''))}
                  placeholder="Callsign / handle"
                  placeholderTextColor={isWhite ? 'rgba(255,255,255,0.25)' : 'rgba(255,160,0,0.28)'}
                  autoCapitalize="none"
                  autoCorrect={false}
                  maxLength={15}
                  returnKeyType="done"
                  onSubmitEditing={handleJoin}
                />
                <TouchableOpacity
                  style={[cd.joinBtn, { borderColor: cc.btnBdr }]}
                  onPress={handleJoin} activeOpacity={0.75}
                >
                  <Text style={[cd.joinBtnTxt, { color: cc.btnText, fontFamily: t.font }]}>JOIN</Text>
                </TouchableOpacity>
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
                    <Text style={[cd.userName, { color: isMe ? cc.ownCl : cc.userCl, fontFamily: t.font }]} numberOfLines={1}>
                      {u.username}{u.country_code ? `  ·${u.country_code}` : ''}{u.tx ? ' 📡TX' : ''}
                    </Text>
                    <Text style={[cd.userFreq, { color: cc.textCl, fontFamily: t.font }]} numberOfLines={1}>
                      {fmtUserFreq(u.frequency)}{u.mode ? ` ${u.mode.toUpperCase()}` : ''}
                      {u.is_idle && u.idle_minutes ? `  idle ${u.idle_minutes}m` : ''}
                    </Text>
                    {!isMe && (
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
            <FlatList
              ref={listRef}
              data={messages}
              keyExtractor={(m: ChatMessage) => m.id}
              style={cd.msgList}
              contentContainerStyle={cd.msgContent}
              showsVerticalScrollIndicator
              onContentSizeChange={() => listRef.current?.scrollToEnd({ animated: false })}
              renderItem={({ item: m }: { item: ChatMessage }) => (
                <View style={[cd.msg, m.type === 'system' && cd.msgSystem]}>
                  <Text style={[cd.msgTime, { color: cc.timeCl, fontFamily: t.font }]}>{m.ts}</Text>
                  {m.type !== 'system' && (
                    <Text style={[cd.msgUser, { color: m.type === 'own' ? cc.ownCl : cc.userCl, fontFamily: t.font }]}>
                      {m.user}
                    </Text>
                  )}
                  <Text style={[
                    cd.msgText,
                    { color: cc.textCl, fontFamily: t.font },
                    m.type === 'system' && { color: cc.sysCl },
                  ]} selectable>
                    {m.text}
                  </Text>
                </View>
              )}
            />
          )}

          {/* ★★ THE PHRASE PAD — canned mode's entire means of speaking. Wrapped, not a row: the
               longest line ("I'm running a decoder — can you wait please?") must not force a
               horizontal scroll on the narrowest phone. */}
          {isCanned && (
            <View style={[cd.inputRow, { borderTopColor: cc.border, flexWrap: 'wrap', gap: 6 }]}>
              {!!dialLine && (
                <Text style={[cd.cannedLine, { color: cc.title, fontFamily: t.font }]}
                      numberOfLines={2}>
                  {dialLine}
                </Text>
              )}
              {canned!.map(ph => (
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
            </View>
          )}

          {/* Input row */}
          {joined && !isCanned && (
            <View style={[cd.inputRow, { borderTopColor: cc.border }]}>
              <Text style={[cd.meLbl, { color: cc.title, fontFamily: t.font }]} numberOfLines={1}>
                {myCallsign}
              </Text>
              <TextInput
                ref={msgRef}
                style={[cd.msgInp, { borderColor: cc.inputBdr, color: cc.inputCl, fontFamily: t.font }]}
                value={msgInput}
                onChangeText={setMsgInput}
                placeholder="Message…"
                placeholderTextColor={isWhite ? 'rgba(255,255,255,0.25)' : 'rgba(255,160,0,0.25)'}
                returnKeyType="send"
                onSubmitEditing={handleSend}
                maxLength={250}
                multiline={false}
                blurOnSubmit={false}
              />
              <TouchableOpacity
                style={[cd.sendBtn, { borderColor: cc.btnBdr }]}
                onPress={handleSend} activeOpacity={0.75}
              >
                <Text style={[cd.sendBtnTxt, { color: cc.btnText, fontFamily: t.font }]}>▶</Text>
              </TouchableOpacity>
            </View>
          )}

        </Animated.View>
      </KeyboardAvoidingView>
    </>
  );
}

// ── Styles ────────────────────────────────────────────────────────────────────

const cd = StyleSheet.create({
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
  handle: { alignItems: 'center', justifyContent: 'center', height: 32 },
  handleBar: { width: 36, height: 4, borderRadius: 2, backgroundColor: C.handle },
  header: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, paddingBottom: 8, gap: 6 },
  title:  { flex: 1, color: 'rgba(255,160,0,0.60)', fontFamily: FONT, fontSize: 11, letterSpacing: 2 },
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
  syncBtnTxt:   { fontSize: 10, letterSpacing: 1, color: 'rgba(255,184,51,0.80)' },
  syncBtnTxtOn: { color: 'rgba(120,235,140,0.95)' },

  setupWrap: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20, gap: 10 },
  setupLbl:  { color: 'rgba(200,180,100,0.75)', fontFamily: FONT, fontSize: 11, letterSpacing: 1.5, textAlign: 'center' },
  setupRow:  { flexDirection: 'row', gap: 8, width: '100%', maxWidth: 380 },
  nameInp: {
    flex: 1, backgroundColor: 'rgba(20,12,0,0.90)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)',
    borderRadius: 6, paddingHorizontal: 12, paddingVertical: 10,
    fontFamily: FONT, fontSize: 14, letterSpacing: 1, color: C.gold,
  },
  joinBtn: {
    backgroundColor: 'rgba(255,160,0,0.12)',
    borderWidth: 1, borderColor: 'rgba(255,160,0,0.45)',
    borderRadius: 6, paddingHorizontal: 18, paddingVertical: 10,
    justifyContent: 'center', alignItems: 'center',
  },
  joinBtnTxt: { fontFamily: FONT, fontSize: 12, letterSpacing: 1, color: C.gold },

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
  msgTextSystem: { color: C.sysCl, fontStyle: 'italic', fontSize: 10 },

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
  sendBtnTxt: { color: C.gold, fontSize: 16 },
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
  // ★ The room line is a SENTENCE too — `meLbl` caps at 80px, which squeezed it into a column.
  cannedLine: {
    width: '100%', fontFamily: FONT, fontSize: 11,
    color: 'rgba(255,184,51,0.75)', marginBottom: 2,
  },
});
