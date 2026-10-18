/**
 * KeyboardShortcuts — the full reference, opened from a button in the main menu.
 *
 * ★ PROSE, NOT A KEY TABLE (Stuart). A table of single keys makes the reader assemble the
 * mental model themselves; a sentence hands it to them. "↑↓ = move" says nothing about what
 * moving MEANS on a given screen, whereas naming what the arrows do — and what stays behind —
 * teaches the shape of the thing. It is the same reasoning that makes the drums feel like
 * drums: describe the behaviour, not the mechanism.
 *
 * ★ It also lets the reference carry the WHY where that matters, which no table could: that
 * backspace leaves the profile picker WITHOUT switching because a profile change retunes the
 * receiver for everyone else on it, and that FM-DX commits once when you stop because the tuner
 * is shared. That is the part a user would otherwise have to discover by upsetting someone.
 *
 * ★★ ORDERED FOR THE 99% (Stuart, 2026-07-26). The Full Keyboard Access substitutes were briefly
 * the FIRST section, which was wrong twice over: it led with an edge case almost nobody is in,
 * and it repeated what the last section already said. Everything accessibility-related now lives
 * in ONE place at the bottom, and the list opens with the screens a user actually meets. Anyone
 * who needs the substitutes is told by the splash and by the notice at the top of the menu, so
 * the reference does not have to shout about them as well.
 *
 * ★★★ ONE SCHEME (Stuart, 2026-10-18): arrows / W A S D move the highlight, Enter selects, Backspace / Escape go
 * back, < > tune, - + zoom — the game controller and the Apple TV remote, and the web page, exactly. Every older
 * shortcut was stripped as too complicated, so this reference was rewritten from the code (src/utils/keyActions.ts,
 * hardwareKeys, PanelNav, ScreenNav), not edited. Re-audit rather than recall when it changes, because a stale list is
 * worse than none. A user who tries something listed and gets nothing concludes the KEYBOARD is broken, not the
 * documentation.
 */
import React, { useRef } from 'react';
import { Modal, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useTheme } from '../contexts/ThemeContext';
import { useRepeatingKeys, noteTouchInteraction } from './PanelNav';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText,
  PopupKey, PopupPlate, PopupScrim, PopupWindow, POPUP_FONT, type PopupTokens,
} from './PopupShell';

type Section = { title: string; body: string[] };

const SECTIONS: Section[] = [
  {
    title: 'ONE SET OF KEYS, EVERYWHERE',
    body: [
      'The keyboard works the way a game controller or the Apple TV remote does, and the same way on the web page. There are only five things to know.',
      'The arrow keys — or W, A, S and D — move a highlight box from control to control, following the screen as it is laid out. Enter presses whatever is highlighted. Backspace or Escape goes back: it closes the menu or panel that is open, one layer at a time.',
      '< and > tune down and up one step, and - and + zoom out and in. The unshifted keys work too — comma and full stop, and = for zoom in — and so does the number pad. Hold any of them to keep going; on the receiver screen a held tune key sweeps the band, speeding up as you hold it, exactly as the tuner keys do under a thumb.',
    ],
  },
  {
    title: 'ON THE RECEIVER',
    body: [
      'The highlight reaches the frequency box, the mode, the tuning step, audio, the menu, chat, the servers chip and the decoder box. Tuning and zoom work from anywhere, with a menu open or not.',
      'If you have hidden the controls, Backspace or Escape brings them back.',
    ],
  },
  {
    title: 'MENUS, SLIDERS AND DROPDOWNS',
    body: [
      'Inside a menu the arrows move through it and Enter presses. On a slider, or the squelch bar, Enter starts adjusting it — the highlight turns amber, left and right change the value, and Enter or Backspace finishes. A dropdown opens with Enter and closes with Backspace without changing what was selected.',
      'If you walk away, a menu opened by keyboard closes itself after ten seconds, flashing once first so you can see why.',
    ],
  },
  {
    title: 'TYPING',
    body: [
      'While a text box has the cursor — the frequency, a search, chat, a server address — every key is the box\'s own, so W, A, S, D, the full stop and Backspace type as they always do. Enter tunes a typed frequency. Escape leaves the box, and the arrows then move round the panel it is in: in the frequency card that reaches the Tune and Bookmarks tabs, Hz, kHz and MHz, and the profile list on an OpenWebRX server.',
      'Backing out of the profile list never switches profile — which matters, because changing profile retunes the receiver for everyone else listening to it.',
    ],
  },
  {
    title: 'DECODER BOXES',
    body: [
      'Move the highlight onto the box and press Enter to take it over. Up and down then move through the list — or scroll it, where there is nothing to select — and left and right reach the controls along its top. Backspace hands the keyboard back. On DAB and ADS-B the box takes the keyboard by itself, and Backspace still lets you out.',
    ],
  },
  {
    title: 'SERVER LIST AND FM-DX',
    body: [
      'On the server list the arrows move through everything, Enter connects or opens a section, and Backspace steps back out of a directory or collapses a group.',
      'On FM-DX the highlight works as on the receiver, and ‹ Back on the controls leaves the server. Because FM-DX is one radio shared by everyone connected, holding > retunes it once when you stop, not once per step.',
    ],
  },
  {
    title: 'COMPATIBILITY MODE',
    body: [
      "A receiver's own pages — compatibility mode, and the server admin pages in the menu — cannot be driven from the keyboard. They are not our pages, so we cannot know whether or how a keyboard will work on them, and taking you somewhere the keys stop working would be worse than not going.",
    ],
  },
  {
    title: 'IF YOUR ARROW KEYS DO NOTHING',
    body: [
      'iOS has an accessibility feature called Full Keyboard Access, in Settings › Accessibility › Keyboards. When it is on, iOS claims the plain arrow keys for its own focus navigation before any app sees them.',
      'Leave it switched on and use W, A, S and D instead — they move the highlight exactly as the arrows do. < > and - + tune and zoom, Enter selects and Backspace goes back, whether or not Full Keyboard Access is on. You never have to turn an accessibility feature off to use the radio.',
    ],
  },
];

export default function KeyboardShortcuts({ visible, onClose }: {
  visible: boolean; onClose: () => void;
}) {
  const { theme: t } = useTheme();
  const s = usePopupStyles(makeS);
  const pt = usePopupTheme();
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(16, false);
  const ff = pt.metal ? POPUP_FONT : t.font;
  const scroll = useRef<ScrollView | null>(null);
  const y = useRef(0);

  // ★ The reference is itself keyboard-reachable, which it has to be — a shortcut list you can
  // only read by touching the screen would be a joke on the person who needs it. Up and down
  // scroll it, Back closes it, and the header says so rather than leaving it to be guessed.
  useRepeatingKeys(visible, (k: string) => {
    if (k === 'Back') { onClose(); return true; }
    if (k !== 'ArrowUp' && k !== 'ArrowDown') return;
    y.current = Math.max(0, y.current + (k === 'ArrowDown' ? 120 : -120));
    scroll.current?.scrollTo({ y: y.current, animated: false });
  }, ['ArrowUp', 'ArrowDown']);

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      {/* ★★★ Transparency OFF: the tap-to-close view stays, the dim goes. */}
      <PopupScrim style={s.backdrop} color="rgba(0,0,0,0.6)" onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View style={[s.card, { borderColor: pt.ink.border }, surf.opaque && !pt.metal && { backgroundColor: surf.fill(CARD_BG) },
                    metalFrame]} onTouchStart={noteTouchInteraction}>
        <PopupPlate radius={16} />
        <View style={s.head}>
          <Text style={[s.title, { fontFamily: ff }]}>KEYBOARD</Text>
          <Text style={[s.hint, { fontFamily: ff }]}>↑↓ scroll · backspace or esc closes</Text>
        </View>
        {/* ★ §10.3: the reference is reading matter — a recessed window on silver / black. */}
        <PopupWindow style={s.bodyWrap} metalStyle={s.bodyWin}>
        <ScrollView
          ref={scroll}
          onScroll={(e) => { y.current = e.nativeEvent.contentOffset.y; }}
          scrollEventThrottle={32}
          style={s.body}
          /* ★ scroll lane: reading matter, inset 14 pt by s.body's own padding */
          contentContainerStyle={{ paddingBottom: 20 }}>
          {SECTIONS.map((sec) => (
            <View key={sec.title} style={s.sec}>
              <Text style={[s.secTitle, { fontFamily: ff }]}>{sec.title}</Text>
              {sec.body.map((para, i) => (
                <Text key={i} style={[s.para, { fontFamily: ff }]}>{para}</Text>
              ))}
            </View>
          ))}
        </ScrollView>
        </PopupWindow>
        {/* ★★ OUTSIDE the ScrollView, deliberately. At the end of the scrollable content it
            would mean reading the whole reference to find the way out — the same trap this
            button exists to fix, just further along.
            ★ A keyboard reference is exactly what a TOUCH user opens out of curiosity, having
            no keyboard to press Back with; the backdrop is a thin margin on a small screen,
            so tapping outside is not a way out you can rely on. (Stuart, 2026-07-26.) */}
        {pt.metal ? (
          <PopupKey label="CLOSE" onPress={onClose} height={32}
            style={{ alignSelf: 'center', width: 110, marginVertical: 10 }} />
        ) : (
        <Pressable style={s.closeBtn} onPress={onClose} onTouchStart={noteTouchInteraction}>
          <Text style={[s.closeTxt, { fontFamily: t.font }]}>CLOSE</Text>
        </Pressable>
        )}
      </View>
    </Modal>
  );
}

/** Today's card glass — made opaque with Transparency OFF. */
const CARD_BG = 'rgba(8,10,9,0.98)';

const makeS = (pt: PopupTokens) => StyleSheet.create({
  backdrop: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  card: {
    position: 'absolute', left: 12, right: 12, top: '8%', bottom: '8%',
    backgroundColor: CARD_BG, borderWidth: 1, borderRadius: 12, overflow: 'hidden',
  },
  head: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 14, paddingVertical: 10,
    ...(pt.metal ? { borderBottomWidth: 0 } : { borderBottomWidth: 1, borderBottomColor: pt.ink.divider }),
  },
  /* ★★ Default chassis in the user's colours (Stuart, 2026-10-12: "yes colours apply everywhere"): the title,
   *  headings and reading matter in the TEXT colour (they were focus green / white), CLOSE in the CONTROLS colour. */
  title: onMetal(pt, { color: pt.ink.text, fontSize: 13, letterSpacing: 3, fontWeight: '700' }, engraveText(pt)),
  hint:  onMetal(pt, { color: pt.ink.textDim, fontSize: 10, letterSpacing: 0.5 }, engraveText(pt, pt.note)),
  bodyWrap: { flex: 1 },
  bodyWin:  { marginHorizontal: 10 },
  body:  { flex: 1, paddingHorizontal: 14 },
  sec:   { marginTop: 16 },
  // Sections are titled and spaced rather than ruled: this is read straight through, and a
  // divider every few lines makes a reference feel like a form.
  secTitle: onMetal(pt, { color: pt.ink.section, fontSize: 11, letterSpacing: 2, marginBottom: 6 }, { color: pt.readout }),
  para:  { color: pt.metal ? 'rgba(255,255,255,0.88)' : pt.ink.textSoft, fontSize: 13, lineHeight: 20, marginBottom: 8 },
  closeBtn: {
    borderTopWidth: 1, borderTopColor: pt.ink.divider,
    paddingVertical: 14, alignItems: 'center',
  },
  closeTxt: { color: pt.gold.close, fontSize: 12, letterSpacing: 2, fontWeight: '700' },
});
