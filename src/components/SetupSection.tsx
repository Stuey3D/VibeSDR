/* ★★★ beta96 b96-setupux ONE SECTION OF THE SERVER SCREEN (Stuart 11-26: "scrolling the long screen with a remote took
 *  ages") — the rules live in src/utils/setupSections.ts.
 *
 *  A header (title + one line saying what it is set to now, or the RED "!" and the reason) that opens and closes the
 *  section. On a TV the header is one D-pad stop: with every section closed the remote steps header to header; OK opens
 *  it and D-pad down walks into it (Android's own focus search — the contents are only drawn while open, so a closed
 *  section has nothing to step into). The header draws its own focus ring (`focused`), as every touchable a remote can
 *  reach must (scripts/test_nav_coverage.ts). */
import React, { useEffect, useRef, useState } from 'react';
import { Platform, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import type { SecTone } from '../utils/setupSections';

type Props = {
  sectionKey: string;
  title: string;
  summary: string;
  must: string;                 // "" = not red; else the reason START was refused for (the header turns red)
  /** ★★★ beta96 three states: green (set up) · amber (worth setting) · red (START refused for it). `summary` is the line. */
  tone?: SecTone;
  open: boolean;
  onToggle: (key: string) => void;
  onMustCleared?: (key: string) => void;   // its "!" cleared while open — the parent keeps it open
  C: any; F: string;
  children?: React.ReactNode;
};

export default function SetupSection({ sectionKey, title, summary, must, tone, open, onToggle, onMustCleared, C, F, children }: Props) {
  const [focused, setFocused] = useState(false);
  const prevMust = useRef(must);
  useEffect(() => {
    if (prevMust.current && !must && open) onMustCleared?.(sectionKey);
    prevMust.current = must;
  }, [must]);   // eslint-disable-line react-hooks/exhaustive-deps
  const red = !!must;
  const ink = red ? C.bad : C.heading;
  const t: SecTone = red ? 'red' : (tone ?? 'green');
  const line = summary;
  // ★ calm: a small mark, never a shouting header — only red colours the title and the border
  const mark = t === 'red' ? C.bad : t === 'amber' ? (C.warn ?? '#ffb833') : (C.ok ?? '#7ddc6a');
  return (
    <View style={[s.wrap, { borderColor: red ? C.bad : focused ? C.keySelBorder : C.border }]}>
      <TouchableOpacity
        focusable={Platform.OS === 'android'}
        accessibilityRole="button"
        accessibilityState={{ expanded: open }}
        accessibilityLabel={`${title}. ${t === 'green' ? '' : t === 'amber' ? 'Worth setting: ' : 'Not started: '}${line}`}
        onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
        onPress={() => onToggle(sectionKey)}
        style={[s.head, focused && { backgroundColor: C.keySelBg }]}>
        <Text style={[s.chev, { color: ink, fontFamily: F }]}>{open ? '▾' : '▸'}</Text>
        <View testID={`sec-mark-${sectionKey}-${t}`} style={[s.mark, { backgroundColor: mark }]} />
        <View style={{ flex: 1 }}>
          <Text style={[s.title, { color: ink, fontFamily: F }]}>{title.toUpperCase()}</Text>
          {!!line && (
            <Text numberOfLines={red ? 3 : t === 'amber' ? 2 : 1}
              style={[s.sum, { color: red ? C.badSoft : t === 'amber' ? (C.warn ?? C.textDim) : C.textDim, fontFamily: F }]}>{line}</Text>
          )}
        </View>
      </TouchableOpacity>
      {open && <View style={s.body}>{children}</View>}
    </View>
  );
}

const s = StyleSheet.create({
  wrap: { borderWidth: 1, borderRadius: 10, marginTop: 10, overflow: 'hidden' },
  head: { flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 12, paddingVertical: 10, minHeight: 44 },
  chev: { fontSize: 15, lineHeight: 18, width: 14 },
  mark: { width: 8, height: 8, borderRadius: 4, marginTop: 5 },
  title: { fontSize: 12, letterSpacing: 1, lineHeight: 18 },
  sum: { fontSize: 11.5, lineHeight: 16, marginTop: 1 },
  body: { paddingHorizontal: 12, paddingBottom: 12 },
  allRow: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 8 },
  allBtn: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 8 },
  tabRow: { flexDirection: 'row', gap: 8, marginTop: 14 },
  tab: { flex: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 8, borderRadius: 10,
         paddingHorizontal: 12, paddingVertical: 12, minHeight: 48 },
});

/** ★★★ OPEN ALL / CLOSE ALL — at the top of the screen; two D-pad stops with their own focus ring. */
export function SectionAllRow({ C, F, onAll }: { C: any; F: string; onAll: (open: boolean) => void }) {
  const [focused, setFocused] = useState<'' | 'open' | 'close'>('');
  const btn = (k: 'open' | 'close', label: string) => (
    <TouchableOpacity key={k} focusable={Platform.OS === 'android'} accessibilityRole="button"
      onFocus={() => setFocused(k)} onBlur={() => setFocused('')} onPress={() => onAll(k === 'open')}
      style={[s.allBtn, { borderColor: focused === k ? C.keySelBorder : C.border, backgroundColor: focused === k ? C.keySelBg : 'transparent' }]}>
      <Text style={{ color: C.key, fontFamily: F, fontSize: 12, letterSpacing: 1 }}>{label}</Text>
    </TouchableOpacity>
  );
  return <View style={s.allRow}>{btn('open', 'OPEN ALL')}{btn('close', 'CLOSE ALL')}</View>;
}

/** ★★★ beta96 FULL'S TABS — SERVER | RADIO, as the web Setup page (Stuart 11-26). Each tab is one D-pad stop with its own focus
 *  ring (scripts/test_nav_coverage.ts), carries its sections' worst mark (red only after a refused START), and the selected one
 *  is filled. Left / right on a TV remote moves between them (Android's own focus search across a row). */
export function SetupTabs({ C, F, tab, onTab, tabs }: { C: any; F: string; tab: string; onTab: (t: any) => void;
                                                       tabs: Array<{ id: string; label: string; tone: SecTone }> }) {
  const [focused, setFocused] = useState('');
  return (
    <View style={s.tabRow} accessibilityRole="tablist">
      {tabs.map((t) => {
        const sel = t.id === tab, foc = focused === t.id;
        const mark = t.tone === 'red' ? C.bad : t.tone === 'amber' ? (C.warn ?? '#ffb833') : (C.ok ?? '#7ddc6a');
        return (
          <TouchableOpacity key={t.id} focusable={Platform.OS === 'android'} accessibilityRole="tab" accessibilityState={{ selected: sel }}
            testID={`full-tab-${t.id}`}
            onFocus={() => setFocused(t.id)} onBlur={() => setFocused('')} onPress={() => onTab(t.id)}
            style={[s.tab, { borderColor: foc ? C.keySelBorder : sel ? C.keySelBorder : C.border,
                             backgroundColor: sel ? C.keySelBg : 'transparent', borderWidth: foc ? 2 : 1 }]}>
            <View style={[s.mark, { backgroundColor: mark, marginTop: 0 }]} />
            <Text numberOfLines={1} style={{ color: sel ? C.keySel : C.key, fontFamily: F, fontSize: 13, letterSpacing: 1, flexShrink: 1 }}>{t.label}</Text>
          </TouchableOpacity>
        );
      })}
    </View>
  );
}
