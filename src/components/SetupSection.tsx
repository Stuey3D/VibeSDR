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
import { headerLine } from '../utils/setupSections';

type Props = {
  sectionKey: string;
  title: string;
  summary: string;
  must: string;                 // "" = nothing missing; else the reason (the header turns red, "! " + reason)
  open: boolean;
  onToggle: (key: string) => void;
  onMustCleared?: (key: string) => void;   // its "!" cleared while open — the parent keeps it open
  C: any; F: string;
  children?: React.ReactNode;
};

export default function SetupSection({ sectionKey, title, summary, must, open, onToggle, onMustCleared, C, F, children }: Props) {
  const [focused, setFocused] = useState(false);
  const prevMust = useRef(must);
  useEffect(() => {
    if (prevMust.current && !must && open) onMustCleared?.(sectionKey);
    prevMust.current = must;
  }, [must]);   // eslint-disable-line react-hooks/exhaustive-deps
  const red = !!must;
  const ink = red ? C.bad : C.heading;
  const line = headerLine(summary, must);
  return (
    <View style={[s.wrap, { borderColor: red ? C.bad : focused ? C.keySelBorder : C.border }]}>
      <TouchableOpacity
        focusable={Platform.OS === 'android'}
        accessibilityRole="button"
        accessibilityState={{ expanded: open }}
        accessibilityLabel={`${title}. ${line}`}
        onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
        onPress={() => onToggle(sectionKey)}
        style={[s.head, focused && { backgroundColor: C.keySelBg }]}>
        <Text style={[s.chev, { color: ink, fontFamily: F }]}>{open ? '▾' : '▸'}</Text>
        <View style={{ flex: 1 }}>
          <Text style={[s.title, { color: ink, fontFamily: F }]}>{title.toUpperCase()}</Text>
          {!!line && (
            <Text numberOfLines={red ? 3 : 1}
              style={[s.sum, { color: red ? C.badSoft : C.textDim, fontFamily: F }]}>{line}</Text>
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
  title: { fontSize: 12, letterSpacing: 1, lineHeight: 18 },
  sum: { fontSize: 11.5, lineHeight: 16, marginTop: 1 },
  body: { paddingHorizontal: 12, paddingBottom: 12 },
});
