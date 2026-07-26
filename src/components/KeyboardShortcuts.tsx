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
 * ★ Ordered by WHERE it applies rather than alphabetically — the question is "how does this
 * work here", not "what does K do". Content audited from the source in
 * BRIEF-keyboard-shortcut-list.md; re-audit rather than recall when it changes, because a stale
 * list is worse than none. A user who tries something listed and gets nothing concludes the
 * KEYBOARD is broken, not the documentation.
 */
import React, { useRef } from 'react';
import { Modal, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useTheme } from '../contexts/ThemeContext';
import { useRepeatingKeys, noteTouchInteraction, NAV_FOCUS } from './PanelNav';

type Section = { title: string; body: string[] };

const SECTIONS: Section[] = [
  {
    title: 'SERVER LIST',
    body: [
      'The up and down arrows move through everything on the page — the custom URL box at the top, your favourites and custom servers, and the directories below them. Enter connects to the highlighted server, opens a section that is collapsed, or drops you into the URL box to type an address. Backspace steps back out: it leaves a directory you have opened, or collapses the group you are standing in.',
      'With a server highlighted, F favourites it, D sets or clears it as your default, and E edits it if it is one of your own — directory entries come from the directory, so they are not ours to change. S cycles the sort order.',
    ],
  },
  {
    title: 'WATERFALL SCREEN',
    body: [
      'Left and right tune, up and down zoom. A tap moves one step; hold the key and it sweeps the band, speeding up the longer you hold it, exactly as the tuner keys do under a thumb.',
      'Enter opens the frequency box. D opens the demodulator, S the tuning step, A audio, M the menu and C chat.',
      'Escape is the way back from anything: it closes whatever is open, brings the controls back if you have hidden them, and — when there is nothing left to close — opens the servers menu.',
    ],
  },
  {
    title: 'TYPING A FREQUENCY',
    body: [
      'Type the number and press Enter. T and B switch between the Tune and Bookmarks tabs, and H, K and M choose Hz, kHz or MHz.',
      'On an OpenWebRX server, P opens the profile picker. Backspace leaves it without switching — which matters, because changing profile retunes the receiver for everyone else listening to it.',
      'In the Bookmarks tab, up and down move through the results, including while you are still typing in the search box, so you can filter and then step straight down into what you found.',
    ],
  },
  {
    title: 'MENUS, PANELS AND DROPDOWNS',
    body: [
      'Menus are navigated with the up and down arrows, while left and right adjust anything that holds a value — a slider, or the squelch bar. Enter presses the highlighted control. Backspace goes up a level: out of a sub-panel, or out of a dropdown list without changing what was selected. Escape closes the menu completely.',
      'Hold an arrow to keep moving. If you walk away, a menu opened by keyboard closes itself after ten seconds, flashing once first so you can see why.',
    ],
  },
  {
    title: 'DECODER BOXES',
    body: [
      'On the HF decoders the waterfall is still the point, so the box leaves the arrows alone until you ask for them. Press Tab and it takes the keyboard, announcing itself with a flash. Press it again for the controls in its header, and once more to hand the keyboard back.',
      'While it has the keyboard, up and down move through the list or scroll the output, left and right move along the header controls, and Space presses whichever you last moved to. Escape or backspace hand the keyboard straight back, and so does ten seconds of not touching it — tune and zoom are what you want back.',
    ],
  },
  {
    title: 'DAB AND ADS-B',
    body: [
      'Here the box is the screen rather than something on top of it: the VFO is locked to the multiplex, so there is nothing to tune. The arrows belong to the box from the moment it appears, and it keeps them — up and down move through the stations, or scroll the aircraft table.',
      'Space selects a station. On ADS-B there is nothing to select, so the arrows simply scroll. Tab moves between the list and the header controls rather than leaving, and inside SPEED FIX the arrows choose a preset, Space sets it, and backspace closes it having changed nothing.',
    ],
  },
  {
    title: 'FM-DX',
    body: [
      'Left and right tune a step at a time and hold to keep going; up and down zoom the dial. Because FM-DX is one radio shared by everyone connected, holding an arrow retunes it once when you stop, not once per step.',
      'Enter opens the frequency box, D the demodulator options, S the tuning step, C chat and R your recordings. Escape closes whatever is open, or takes you back to the server list.',
      'The shared-tuner notice you meet on connecting will go with Enter, Space or Escape — whichever you reach for.',
    ],
  },
  {
    title: 'RECORDINGS',
    body: [
      'Up and down select, Space plays and pauses, and backspace deletes — or forward-delete, since a Mac keyboard has no such key. Escape closes the list.',
      'Sharing is not on the keyboard: it hands over to the system share sheet, which is not ours to drive.',
    ],
  },
  {
    title: 'MAPS',
    body: ['Escape brings you back to the receiver.'],
  },
  {
    title: 'COMPATIBILITY MODE',
    body: [
      "A receiver's own pages — compatibility mode, and the server admin pages in the menu — cannot be reached from the keyboard at all. They are not our pages, so we cannot know whether or how a keyboard will work on them, and taking you somewhere the keys stop working would be worse than not going.",
      'The admin buttons are simply skipped as the highlight moves past them, and the compatibility-mode button is greyed while you are using a keyboard. Touching it wakes it up — the tap is proof you are back on the touchscreen — and the next tap goes through.',
    ],
  },
];

export default function KeyboardShortcuts({ visible, onClose }: {
  visible: boolean; onClose: () => void;
}) {
  const { theme: t } = useTheme();
  const scroll = useRef<ScrollView | null>(null);
  const y = useRef(0);

  // ★ The reference is itself keyboard-reachable, which it has to be — a shortcut list you can
  // only read by touching the screen would be a joke on the person who needs it. Arrows scroll
  // it, Escape closes it, and the header says so rather than leaving it to be guessed.
  useRepeatingKeys(visible, (k: string) => {
    if (k === 'Escape' || k === 'Backspace') { onClose(); return; }
    if (k !== 'ArrowUp' && k !== 'ArrowDown') return;
    y.current = Math.max(0, y.current + (k === 'ArrowDown' ? 120 : -120));
    scroll.current?.scrollTo({ y: y.current, animated: false });
  }, ['ArrowUp', 'ArrowDown']);

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={s.backdrop} onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View style={[s.card, { borderColor: t.barBorder }]} onTouchStart={noteTouchInteraction}>
        <View style={s.head}>
          <Text style={[s.title, { fontFamily: t.font }]}>KEYBOARD</Text>
          <Text style={[s.hint, { fontFamily: t.font }]}>↑↓ scroll · esc close</Text>
        </View>
        <ScrollView
          ref={scroll}
          onScroll={(e) => { y.current = e.nativeEvent.contentOffset.y; }}
          scrollEventThrottle={32}
          style={s.body}
          contentContainerStyle={{ paddingBottom: 20 }}>
          {SECTIONS.map((sec) => (
            <View key={sec.title} style={s.sec}>
              <Text style={[s.secTitle, { fontFamily: t.font }]}>{sec.title}</Text>
              {sec.body.map((para, i) => (
                <Text key={i} style={[s.para, { fontFamily: t.font }]}>{para}</Text>
              ))}
            </View>
          ))}
        </ScrollView>
      </View>
    </Modal>
  );
}

const s = StyleSheet.create({
  backdrop: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.6)' },
  card: {
    position: 'absolute', left: 12, right: 12, top: '8%', bottom: '8%',
    backgroundColor: 'rgba(8,10,9,0.98)', borderWidth: 1, borderRadius: 12, overflow: 'hidden',
  },
  head: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 14, paddingVertical: 10,
    borderBottomWidth: 1, borderBottomColor: 'rgba(255,255,255,0.12)',
  },
  title: { color: NAV_FOCUS, fontSize: 13, letterSpacing: 3, fontWeight: '700' },
  hint:  { color: 'rgba(255,255,255,0.55)', fontSize: 10, letterSpacing: 0.5 },
  body:  { flex: 1, paddingHorizontal: 14 },
  sec:   { marginTop: 16 },
  // Sections are titled and spaced rather than ruled: this is read straight through, and a
  // divider every few lines makes a reference feel like a form.
  secTitle: { color: NAV_FOCUS, fontSize: 11, letterSpacing: 2, marginBottom: 6 },
  para:  { color: 'rgba(255,255,255,0.88)', fontSize: 13, lineHeight: 20, marginBottom: 8 },
});
