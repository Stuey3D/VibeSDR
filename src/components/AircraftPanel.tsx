/**
 * OWRX ADS-B — the aircraft table, as a real list.
 *
 * It used to be the generic DecoderPanel's text blob ("EZY26BX 39700ft 434kt -25dB"),
 * because the adapter flattened the records to a string on arrival. It doesn't any
 * more, so we can show what's actually in them: the registry country, the vertical
 * trend, the range and bearing from the receiver.
 *
 * The flag is the aircraft's REGISTRY (the server sends `ccode` — no ICAO-range table
 * needed), NOT where the flight departed: a Ryanair 737 is Irish wherever it took off
 * from. That's what makes it worth showing — it's how you spot the unusual visitor.
 */
import type React from 'react';
import { View, Text, ScrollView, StyleSheet } from 'react-native';
import { useMemo } from 'react';
import type { Aircraft } from '../services/SDRBackend';
import { useTheme, type ThemeTokens } from '../contexts/ThemeContext';

const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

/** ISO country code → flag emoji (regional-indicator offset). */
function isoFlag(iso?: string): string {
  if (!iso || iso.length !== 2) return '';
  return String.fromCodePoint(
    ...[...iso.toUpperCase()].map((c) => 0x1f1e6 + c.charCodeAt(0) - 65),
  );
}

/** Flight levels above the transition altitude, feet below — what you'd hear on air. */
function altText(ft?: number): string {
  if (ft == null) return '—';
  return ft >= 18_000
    ? `FL${String(Math.round(ft / 100)).padStart(3, '0')}`
    : `${(Math.round(ft / 100) * 100).toLocaleString()} ft`;
}

function title(a: Aircraft): string {
  // Callsign, else registration, else the bare ICAO address — always present, so a
  // row can never be nameless.
  return a.flight?.trim() || a.reg?.trim() || a.icao.toUpperCase();
}

export default function AircraftPanel({ aircraft, scrollRef }: {
  aircraft: Aircraft[];
  /** ★ Exposed so the decoder box can scroll this list from the keyboard — the table is the
   *  whole point of ADS-B, so it has to be reachable without a finger. */
  scrollRef?: React.RefObject<ScrollView | null>;
}) {
  const { theme } = useTheme();
  const s = useMemo(() => styles(theme), [theme]);

  // Nearest first; the ones that haven't sent a position yet fall back to signal so
  // they sort last rather than vanish — they're real aircraft, just not locatable.
  const rows = useMemo(() => [...aircraft].sort((a, b) => {
    if (a.distKm != null && b.distKm != null) return a.distKm - b.distKm;
    if (a.distKm != null) return -1;
    if (b.distKm != null) return 1;
    return (b.rssi ?? -99) - (a.rssi ?? -99);
  }), [aircraft]);

  return (
    <View style={s.wrap}>
      <ScrollView ref={scrollRef as any}>
        {rows.map((a) => {
          const climbing = a.vspeed != null && Math.abs(a.vspeed) >= 100;
          return (
            <View key={a.icao} style={s.row}>
              <Text style={s.flag}>{isoFlag(a.ccode)}</Text>

              <View style={s.idCol}>
                <Text style={s.call} numberOfLines={1}>{title(a)}</Text>
                <Text style={s.icao} numberOfLines={1}>
                  {a.icao.toUpperCase()}{a.squawk ? ` · ${a.squawk}` : ''}
                </Text>
              </View>

              <View style={s.altCol}>
                <Text style={s.alt} numberOfLines={1}>
                  {climbing && (
                    <Text style={a.vspeed! > 0 ? s.up : s.down}>
                      {a.vspeed! > 0 ? '▲ ' : '▼ '}
                    </Text>
                  )}
                  {altText(a.altitude)}
                </Text>
                <Text style={s.sub} numberOfLines={1}>
                  {a.speed != null ? `${Math.round(a.speed)} kt` : '—'}
                </Text>
              </View>

              <View style={s.distCol}>
                <Text style={s.dist} numberOfLines={1}>
                  {a.distKm != null ? `${Math.round(a.distKm)} km` : '—'}
                </Text>
                <Text style={s.sub} numberOfLines={1}>
                  {a.bearing != null ? COMPASS[Math.round(a.bearing / 45) % 8] : ''}
                  {a.rssi != null ? `  ${a.rssi.toFixed(0)}dB` : ''}
                </Text>
              </View>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );
}

const styles = (t: ThemeTokens) => StyleSheet.create({
  wrap:    { flex: 1 },
  row:     { flexDirection: 'row', alignItems: 'center', gap: 8,
             paddingVertical: 6, paddingHorizontal: 4,
             borderBottomWidth: 1, borderBottomColor: t.barBorder },
  flag:    { fontSize: 16, width: 22 },
  idCol:   { flex: 1, minWidth: 0 },
  call:    { color: t.freqColor, fontFamily: t.font, fontSize: 14, fontWeight: 'bold' },
  icao:    { color: t.sectionColor, fontFamily: t.font, fontSize: 10, opacity: 0.8 },
  altCol:  { width: 78, alignItems: 'flex-end' },
  alt:     { color: t.btnText, fontFamily: t.font, fontSize: 13 },
  distCol: { width: 82, alignItems: 'flex-end' },
  dist:    { color: t.snrColor, fontFamily: t.font, fontSize: 13, fontWeight: 'bold' },
  sub:     { color: t.sectionColor, fontFamily: t.font, fontSize: 10, opacity: 0.75 },
  up:      { color: '#3ddc84' },
  down:    { color: '#4cc9f0' },
});
