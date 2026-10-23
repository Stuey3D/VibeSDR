/**
 * WeatherReportText — the RTTY box's text with the SERVER's translated weather reports drawn in place (V12, 2026-10-23).
 *
 * ★★★ The server reads each SYNOP/SHIP report out of the RTTY text (vibe_synop.h) and sends it as the 0x08 frame;
 *     SDRScreen puts a marker for it into the box's text at the moment it arrives — right after the raw groups it came
 *     from — and this draws the report there: one line, tap for the details and the raw groups again (collapsible).
 * ★ No marker in the text (every decoder but RTTY, every server older than V12): ONE <Text>, exactly as before.
 * ★ A marker whose report is gone (scrolled out of the map) draws nothing — never the marker's characters.
 */
import React, { useCallback, useState } from 'react';
import { Text, TouchableOpacity, View, type StyleProp, type TextStyle } from 'react-native';
import { splitWxText, WX_OPEN, type WeatherReport } from '../utils/weatherReport';

interface Props {
  text: string;
  reports?: ReadonlyMap<number, WeatherReport>;
  /** The box's own output style (font, size, glow, colour). */
  textStyle: StyleProp<TextStyle>;
  /** The report line's colour (the NAVTEX title's accent) and its details' (the status colour). */
  accent: string;
  muted: string;
  fontFamily?: string;
}

export default function WeatherReportText({ text, reports, textStyle, accent, muted, fontFamily }: Props) {
  const [open, setOpen] = useState<ReadonlySet<number>>(new Set());
  const toggle = useCallback((id: number) => setOpen((s) => {
    const n = new Set(s); if (n.has(id)) n.delete(id); else n.add(id); return n;
  }), []);
  if (!reports || text.indexOf(WX_OPEN) < 0) return <Text style={textStyle} selectable>{text}</Text>;
  const parts = splitWxText(text);
  return (
    <View>
      {parts.map((p, i) => {
        // ★ The text before a report ends its line with '\n' — the report IS the next line, so that newline goes.
        if ('text' in p) {
          const t = parts[i + 1] && 'id' in parts[i + 1] ? p.text.replace(/\n$/, '') : p.text;
          return t ? <Text key={'t' + i} style={textStyle} selectable>{t}</Text> : null;
        }
        const r = reports.get(p.id);
        if (!r) return null;
        const isOpen = open.has(p.id);
        return (
          <TouchableOpacity key={'r' + p.id} onPress={() => toggle(p.id)} activeOpacity={0.6}
                            accessibilityRole="button" accessibilityState={{ expanded: isOpen }}
                            accessibilityLabel={`Weather report: ${r.line}`}
                            accessibilityHint={isOpen ? 'Hides the details' : 'Shows what each group said'}
                            style={{ borderLeftWidth: 2, borderLeftColor: accent, paddingLeft: 8, marginVertical: 2 }}>
            <Text style={[textStyle, { color: accent, textShadowRadius: 0 }]}>{(isOpen ? '▾ ' : '▸ ') + r.line}</Text>
            {isOpen && (
              <View style={{ paddingLeft: 12, paddingBottom: 4 }}>
                {r.details.map((d, k) => (
                  <Text key={k} style={{ color: muted, fontSize: 11, lineHeight: 16, fontFamily }} selectable>{d}</Text>
                ))}
                {!!r.raw && <Text style={{ color: muted, fontSize: 11, lineHeight: 16, fontFamily, marginTop: 2 }} selectable>{r.raw}</Text>}
              </View>
            )}
          </TouchableOpacity>
        );
      })}
    </View>
  );
}
