/**
 * FkaSplash — shown once, when the app works out that iOS Full Keyboard Access has taken the
 * arrow keys away from it.
 *
 * ★★ WHY A SPLASH AND NOT A HINT SOMEWHERE. In this state the user has a keyboard that half
 * works: letters and Enter arrive, so the menu opens and the frequency box opens, but nothing
 * tunes and no list can be stepped through. Nothing is broken enough to look broken, so the
 * natural conclusion is that VibeSDR simply has poor keyboard support — and a person who has
 * concluded that does not go hunting through a menu for the page that would correct them. The
 * explanation has to come to them. (Stuart, 2026-07-26.)
 *
 * ★ It carries only the FOUR SUBSTITUTE KEYS and one instruction — press M. A splash competing
 * with the full reference would either be too long to read or a worse copy of it; this exists to
 * get the user moving again and to tell them where the rest lives.
 *
 * ★ Shown ONCE and remembered. The detection is a heuristic (see fullKeyboardAccessSuspected in
 * PanelNav), so it can be wrong — and a heuristic that can be wrong must never be able to nag.
 */
import React, { useEffect, useState } from 'react';
import { Modal, Pressable, StyleSheet, Text, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useTheme } from '../contexts/ThemeContext';
import { useFullKeyboardAccessSuspected, useRepeatingKeys, useSuppressShortcuts, noteTouchInteraction, NAV_FOCUS } from './PanelNav';

const SEEN_KEY = 'lsv_fka_splash_seen_v2';

// ★ The Alt key is labelled differently on every keyboard likely to be in front of a reader:
// Alt on a Windows or handheld one (Stuart's Rii says ALT), Option on an older Apple keyboard,
// and on a modern Mac nothing but the symbol. ALL THREE are shown (Stuart), because the
// only failure that matters here is a user unable to FIND the key an instruction names.
const KEYS: { k: string; what: string }[] = [
  { k: '<  >', what: 'tune down and up' },
  { k: '−  +', what: 'zoom out and in' },
  { k: 'shift + arrow', what: 'move through menus and lists' },
  { k: 'alt/option/⌥ + tab', what: 'in and out of the decoder box' },
];

export default function FkaSplash({ onOpenHelp }: { onOpenHelp: () => void }) {
  const { theme: t } = useTheme();
  const suspected = useFullKeyboardAccessSuspected();
  const [seen, setSeen] = useState<boolean | null>(null);

  useEffect(() => {
    AsyncStorage.getItem(SEEN_KEY).then((v) => setSeen(v === '1')).catch(() => setSeen(false));
  }, []);

  const visible = suspected && seen === false;

  // ★ While the splash is up the screen underneath must not act on keys — otherwise M would
  // open the menu BEHIND it. useRepeatingKeys deliberately ignores this flag, which is what
  // lets the splash still hear the very keys it has just silenced for everyone else.
  useSuppressShortcuts(visible);

  const close = () => {
    setSeen(true);
    AsyncStorage.setItem(SEEN_KEY, '1').catch(() => {});
  };

  // ★ Every key here has to be one that still ARRIVES in this state — which rules out Escape's
  // usual companions. Enter and Space are safe (letters and Enter get through, which is the
  // whole shape of the fault), and M both dismisses and does the thing it is advertising.
  useRepeatingKeys(visible, (k: string) => {
    if (k === 'M') { close(); onOpenHelp(); return; }
    if (k === 'Enter' || k === 'Space' || k === 'Escape') close();
  }, []);

  if (!visible) return null;

  return (
    <Modal visible transparent animationType="fade" onRequestClose={close}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <View style={s.backdrop} onTouchStart={noteTouchInteraction}>
        <View style={[s.card, { borderColor: t.barBorder }]}>
          <Text style={[s.title, { fontFamily: t.font }]}>YOUR ARROW KEYS ARE ELSEWHERE</Text>
          <Text style={[s.body, { fontFamily: t.font }]}>
            iOS Full Keyboard Access is switched on, so the system is taking the arrow keys and
            Tab before VibeSDR can see them. Leave it on — these do the same jobs:
          </Text>
          <View style={s.table}>
            {KEYS.map((r) => (
              <View key={r.k} style={s.row}>
                <View style={s.cap}><Text style={[s.capTxt, { fontFamily: t.font }]}>{r.k}</Text></View>
                <Text style={[s.what, { fontFamily: t.font }]}>{r.what}</Text>
              </View>
            ))}
          </View>
          <Text style={[s.body, { fontFamily: t.font }]}>
            Press <Text style={s.inline}>M</Text> for the menu, where KEYBOARD lists every
            shortcut in the app.
          </Text>
          <View style={s.btns}>
            <Pressable style={s.btn} onPress={() => { close(); onOpenHelp(); }} onTouchStart={noteTouchInteraction}>
              <Text style={[s.btnTxt, { fontFamily: t.font }]}>SHOW ME</Text>
            </Pressable>
            <Pressable style={s.btn} onPress={close} onTouchStart={noteTouchInteraction}>
              <Text style={[s.btnTxt, { fontFamily: t.font }]}>GOT IT</Text>
            </Pressable>
          </View>
        </View>
      </View>
    </Modal>
  );
}

const s = StyleSheet.create({
  backdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.72)', alignItems: 'center', justifyContent: 'center', padding: 20 },
  card: {
    maxWidth: 460, width: '100%', backgroundColor: 'rgba(8,10,9,0.99)',
    borderWidth: 1, borderRadius: 12, padding: 18,
  },
  title: { color: NAV_FOCUS, fontSize: 13, letterSpacing: 2, fontWeight: '700', marginBottom: 10 },
  body:  { color: 'rgba(255,255,255,0.85)', fontSize: 13, lineHeight: 19 },
  inline: { color: NAV_FOCUS, fontWeight: '700' },
  table: { marginVertical: 14 },
  row:   { flexDirection: 'row', alignItems: 'center', marginBottom: 8 },
  // Key caps are VIEWS, not nested Text — a nested <Text> cannot carry a border on iOS.
  cap: {
    minWidth: 138, paddingHorizontal: 8, paddingVertical: 5, marginRight: 12,
    borderWidth: 1, borderColor: NAV_FOCUS, borderRadius: 5, alignItems: 'center',
  },
  capTxt: { color: NAV_FOCUS, textAlign: 'center', fontSize: 11, fontWeight: '700', letterSpacing: 1 },
  what:   { color: 'rgba(255,255,255,0.85)', fontSize: 13, flexShrink: 1 },
  btns: { flexDirection: 'row', justifyContent: 'flex-end', marginTop: 16 },
  btn:  { paddingHorizontal: 18, paddingVertical: 10, marginLeft: 10, borderWidth: 1, borderColor: NAV_FOCUS, borderRadius: 6 },
  btnTxt: { color: NAV_FOCUS, fontSize: 12, letterSpacing: 1.5, fontWeight: '700' },
});
