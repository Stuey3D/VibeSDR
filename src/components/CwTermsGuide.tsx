/**
 * CwTermsGuide — THE CW TERMINOLOGY GUIDE (V12, 2026-10-29), opened by CW TERMS GUIDE under "NOT SURE WHAT A SIGNAL IS?"
 * in the Mode sheet (ModeSelector). The web page has the same guide (web/client/src/cwGuide.ts) from the same data.
 *
 *   • a search box — utils/cwVocab searchCwTerms(): code, other spellings, plain words, meaning, example;
 *   • each term: its code as sent, its plain words, what it means and an example from a real exchange;
 *   • THE LOOK-ALIKES (CW_LOOKALIKES): DE is not a German callsign, 0 and O, prosigns printed as punctuation…;
 *   • RST in one line — readability, strength, tone — because it is the first thing a new listener asks.
 * ★ ONE source of the words: src/utils/cwVocab.ts (the PLAIN key and the server's language model read it too).
 * ★ KeyboardShortcuts' card (PopupShell parts: the default chassis, silver / black plates, Transparency on/off) with
 *   CityPickerModal's search box. Pad and keyboard: [search, every term, every look-alike, CLOSE] — up/down move and
 *   scroll, Enter on the search box puts the cursor in it, Back closes (useListNav: on top, so the sheet beneath waits).
 */
import React, { useMemo, useRef, useState } from 'react';
import { Modal, Pressable, ScrollView, StyleSheet, Text, TextInput, View } from 'react-native';
import { useTheme } from '../contexts/ThemeContext';
import { useListNav, useNavFocus, NavRing, noteTouchInteraction } from './PanelNav';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, PopupScrim, PopupWindow, POPUP_FONT, type PopupTokens,
} from './PopupShell';
import { CW_GUIDE_INTRO, CW_GUIDE_RST_LINE, CW_KIND_LABEL, matchCwLookalikes, searchCwTerms } from '../utils/cwVocab';
import { QRZ_SIGN_IN_NOTE } from '../utils/cwFormat';
import { scrollLane } from '../constants/popupTokens';

export default function CwTermsGuide({ visible, onClose }: { visible: boolean; onClose: () => void }) {
  const { theme: t } = useTheme();
  const s = usePopupStyles(makeS);
  const pt = usePopupTheme();
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(16, false);
  const ff = pt.metal ? POPUP_FONT : t.font;
  const navRing = useNavFocus().ring;
  const [q, setQ] = useState('');
  const terms = useMemo(() => searchCwTerms(q), [q]);
  const looks = useMemo(() => matchCwLookalikes(q), [q]);
  const searchRef = useRef<TextInput | null>(null);
  const scroll = useRef<ScrollView | null>(null);
  const rowY = useRef<number[]>([]);

  // ★ [search, terms…, look-alikes…, CLOSE]. A term has nothing to press — moving onto it brings it into view.
  const n = terms.length + looks.length;
  const len = useRef(n); len.current = n;
  const close = () => { setQ(''); onClose(); };   // ★ the next opening starts with every term
  const focus = useListNav(visible, n + 2,
    (i) => {
      if (i === 0) { searchRef.current?.focus(); return; }
      if (i === len.current + 1) close();
    },
    (i) => {
      if (i < 1 || i > len.current) return;
      const y = rowY.current[i - 1];
      if (y != null) scroll.current?.scrollTo({ y: Math.max(0, y - 40), animated: true });
    },
    undefined, () => { close(); return true; }, { startAt: 0 });

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={close}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <PopupScrim style={s.backdrop} color="rgba(0,0,0,0.6)" onPress={close} onTouchStart={noteTouchInteraction} />
      <View style={s.frame} pointerEvents="box-none">
      <View style={[s.card, { borderColor: pt.ink.border }, surf.opaque && !pt.metal && { backgroundColor: surf.fill(CARD_BG) },
                    metalFrame]} onTouchStart={noteTouchInteraction}>
        <PopupPlate radius={16} />
        <View style={s.head}>
          <Text style={[s.title, { fontFamily: ff }]} numberOfLines={1}>CW TERMS</Text>
          <Text style={[s.hint, { fontFamily: ff }]} numberOfLines={1}>backspace or esc closes</Text>
        </View>
        <View style={s.searchWrap}>
          <TextInput
            ref={searchRef}
            style={[s.search, { fontFamily: ff }, focus === 0 && { borderColor: navRing, borderWidth: 2 }]}
            placeholder="Search: QTH, 73, report, DE…"
            placeholderTextColor={pt.metal ? pt.winDim : pt.ink.textFaint}
            value={q}
            onChangeText={setQ}
            autoCorrect={false}
            autoCapitalize="characters"
            returnKeyType="search"
            accessibilityLabel="Search the CW terms"
          />
        </View>
        <PopupWindow style={s.bodyWrap} metalStyle={s.bodyWin}>
        <ScrollView ref={scroll} style={s.body} keyboardShouldPersistTaps="handled" contentContainerStyle={[{ paddingBottom: 20 }, scrollLane]}>
          {!q.trim() && (
            <View style={s.sec}>
              <Text style={[s.para, { fontFamily: ff }]}>{CW_GUIDE_INTRO}</Text>
              <Text style={[s.para, s.strong, { fontFamily: ff }]}>{CW_GUIDE_RST_LINE}</Text>
            </View>
          )}
          {!terms.length && !looks.length && (
            <Text style={[s.para, s.none, { fontFamily: ff }]}>Nothing matches “{q.trim()}”.</Text>
          )}
          {terms.map((tm, i) => (
            <View key={tm.code} style={s.term} onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}>
              <View style={s.termHead}>
                <Text style={[s.code, { fontFamily: ff }]}>{tm.code}{tm.aliases?.length ? ` · ${tm.aliases.join(' · ')}` : ''}</Text>
                <Text style={[s.kind, { fontFamily: ff }]}>{CW_KIND_LABEL[tm.kind]}</Text>
              </View>
              {!!tm.plain && tm.plain !== '—' && (
                <Text style={[s.plain, { fontFamily: ff }]}>“{tm.plain}”{tm.ask ? ` · with ?: “${tm.ask}”` : ''}</Text>
              )}
              <Text style={[s.para, { fontFamily: ff }]}>{tm.meaning}</Text>
              {!!tm.example && <Text style={[s.example, { fontFamily: ff }]}>{tm.example}</Text>}
              <NavRing on={focus === i + 1} color={navRing} radius={6} />
            </View>
          ))}
          {looks.length > 0 && (
            <Text style={[s.secTitle, { fontFamily: ff }]}>EASILY CONFUSED</Text>
          )}
          {looks.map((l, j) => (
            <View key={l.title} style={s.term} onLayout={(e) => { rowY.current[terms.length + j] = e.nativeEvent.layout.y; }}>
              <Text style={[s.lookTitle, { fontFamily: ff }]}>{l.title}</Text>
              <Text style={[s.para, { fontFamily: ff }]}>{l.text}</Text>
              <NavRing on={focus === terms.length + j + 1} color={navRing} radius={6} />
            </View>
          ))}
          <Text style={[s.para, s.note, { fontFamily: ff }]}>
            Tap a callsign in the CW, PSK, JTTY or digital spots lists to look it up on QRZ.com. {QRZ_SIGN_IN_NOTE}
          </Text>
        </ScrollView>
        </PopupWindow>
        {pt.metal ? (
          <PopupKey label="CLOSE" onPress={close} focused={focus === n + 1} height={32}
            style={{ alignSelf: 'center', width: 110, marginVertical: 10 }} />
        ) : (
        <Pressable style={s.closeBtn} onPress={close} onTouchStart={noteTouchInteraction}>
          <Text style={[s.closeTxt, { fontFamily: t.font }]}>CLOSE</Text>
          <NavRing on={focus === n + 1} color={navRing} radius={6} />
        </Pressable>
        )}
      </View>
      </View>
    </Modal>
  );
}

/** Today's card glass — made opaque with Transparency OFF (KeyboardShortcuts' value). */
const CARD_BG = 'rgba(8,10,9,0.98)';

const makeS = (pt: PopupTokens) => StyleSheet.create({
  backdrop: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  // ★ Wide screens: a reading width, centred — a 1,300 pt line of definitions is hard to read.
  frame: { position: 'absolute', left: 12, right: 12, top: '8%', bottom: '8%', alignItems: 'center' },
  card: {
    flex: 1, width: '100%', maxWidth: 640,
    backgroundColor: CARD_BG, borderWidth: 1, borderRadius: 12, overflow: 'hidden',
  },
  head: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10,
    paddingHorizontal: 14, paddingVertical: 10,
    ...(pt.metal ? { borderBottomWidth: 0 } : { borderBottomWidth: 1, borderBottomColor: pt.ink.divider }),
  },
  title: onMetal(pt, { color: pt.ink.text, fontSize: 13, letterSpacing: 3, fontWeight: '700' }, engraveText(pt)),
  hint:  onMetal(pt, { color: pt.ink.textDim, fontSize: 10, letterSpacing: 0.5, flexShrink: 1 }, engraveText(pt, pt.note)),
  searchWrap: { paddingHorizontal: 12, paddingTop: 10, paddingBottom: 6 },
  search: onMetal(pt, { backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: pt.ink.keyBorder,
            borderRadius: 8, color: pt.ink.text, paddingHorizontal: 12, paddingVertical: 9, fontSize: 14 },
            { ...windowStyle(pt), color: pt.readout }),
  bodyWrap: { flex: 1 },
  bodyWin:  { marginHorizontal: 10 },
  body:  { flex: 1, paddingHorizontal: 14 },
  sec:   { marginTop: 12 },
  secTitle: onMetal(pt, { color: pt.ink.section, fontSize: 11, letterSpacing: 2, marginTop: 18, marginBottom: 2 }, { color: pt.readout }),
  term:  { paddingVertical: 9, paddingHorizontal: 4, borderBottomWidth: StyleSheet.hairlineWidth, borderColor: pt.ink.divider },
  termHead: { flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between', gap: 8, flexWrap: 'wrap' },
  code:  onMetal(pt, { color: pt.ink.text, fontSize: 15, fontWeight: '700', letterSpacing: 1, flexShrink: 1 }, { color: pt.readout }),
  kind:  onMetal(pt, { color: pt.ink.textDim, fontSize: 9, letterSpacing: 1.5 }, { color: pt.winDim }),
  plain: onMetal(pt, { color: pt.ink.text, fontSize: 13, marginTop: 3 }, { color: pt.winText }),
  lookTitle: onMetal(pt, { color: pt.ink.text, fontSize: 13, fontWeight: '700', marginBottom: 2 }, { color: pt.readout }),
  para:  { color: pt.metal ? 'rgba(255,255,255,0.88)' : pt.ink.textSoft, fontSize: 13, lineHeight: 19, marginTop: 3 },
  strong: { fontWeight: '700' },
  example: { color: pt.metal ? 'rgba(255,255,255,0.7)' : pt.ink.textDim, fontSize: 12, lineHeight: 17, marginTop: 3, fontStyle: 'italic' },
  none:  { marginTop: 16, textAlign: 'center' },
  note:  { marginTop: 16, fontSize: 12 },
  closeBtn: {
    borderTopWidth: 1, borderTopColor: pt.ink.divider,
    paddingVertical: 14, alignItems: 'center',
  },
  closeTxt: { color: pt.gold.close, fontSize: 12, letterSpacing: 2, fontWeight: '700' },
});
