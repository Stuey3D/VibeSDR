/**
 * AntennaBandsEditor — what the aerial COVERS and what FILTERS are fitted, on Lite's / the app's server screen
 * (2026-10-09). Stuart: "Both android versions are missing the antenna and filter details, I've just changed the antenna
 * on the Sony but no option to say it has an FM filter". The Linux setup page has had these since 2026-10-06; the server
 * turns them into the listener's notice ("FM band-stop filter fitted — reception here is deliberately reduced", "Outside
 * this antenna's range") and the landing/directory card.
 *
 * ★ The text format is the shared one (src/utils/antennaBands.ts): only FINISHED rows are emitted, canonicalised through
 *   the parser, so a half-typed row is never stored and never reaches a listener.
 * ★ Presets fill a row the owner can edit — the same lists as the setup page (ANTENNA_RANGE_PRESETS / _FILTER_PRESETS).
 */
import React, { useEffect, useRef, useState } from 'react';
import { ScrollView, Text, TextInput, TouchableOpacity, View } from 'react-native';
import {
  ANTENNA_FILTER_PRESETS, ANTENNA_RANGE_PRESETS, FILTER_KIND_LABEL, type BandUnit, type FilterKind,
  fmtInUnit, formatAntennaFilters, formatAntennaRanges, parseAntennaFilters, parseAntennaRanges,
} from '../utils/antennaBands';

type Colours = { textDim: string; amber: string; border: string; green: string; gold: string };
type RangeRow = { name: string; lo: string; hi: string; unit: BandUnit };
type FilterRow = { kind: FilterKind; lo: string; hi: string; unit: BandUnit; name: string };
const KINDS: FilterKind[] = ['bandstop', 'bandpass', 'highpass', 'lowpass'];
const UNITS: BandUnit[] = ['kHz', 'MHz'];

const rangesFrom = (t: string): RangeRow[] => parseAntennaRanges(t).map((r) => ({
  name: r.name, lo: fmtInUnit(r.loHz, r.unit), hi: fmtInUnit(r.hiHz, r.unit), unit: r.unit }));
const filtersFrom = (t: string): FilterRow[] => parseAntennaFilters(t).map((f) => ({
  kind: f.kind, unit: f.unit, name: f.name,
  lo: f.kind === 'lowpass' ? fmtInUnit(f.hiHz, f.unit) : fmtInUnit(f.loHz, f.unit),
  hi: f.kind === 'bandstop' || f.kind === 'bandpass' ? fmtInUnit(f.hiHz, f.unit) : '' }));
const rangesText = (rows: RangeRow[]) => formatAntennaRanges(parseAntennaRanges(
  rows.filter((r) => r.lo.trim() && r.hi.trim()).map((r) => `${r.lo.trim()}-${r.hi.trim()}${r.unit} ${r.name.trim()}`).join('; ')));
const filtersText = (rows: FilterRow[]) => formatAntennaFilters(parseAntennaFilters(
  rows.filter((f) => f.lo.trim() && ((f.kind !== 'bandstop' && f.kind !== 'bandpass') || f.hi.trim()))
      .map((f) => `${f.kind} ${f.kind === 'bandstop' || f.kind === 'bandpass' ? `${f.lo.trim()}-${f.hi.trim()}` : f.lo.trim()}${f.unit} ${f.name.trim()}`)
      .join('; ')));

export default function AntennaBandsEditor({ ranges, filters, onChange, C, F }: {
  ranges: string; filters: string; onChange: (ranges: string, filters: string) => void; C: Colours; F: string;
}) {
  const [rr, setRr] = useState<RangeRow[]>(() => rangesFrom(ranges));
  const [fr, setFr] = useState<FilterRow[]>(() => filtersFrom(filters));
  const [presets, setPresets] = useState<'' | 'range' | 'filter'>('');
  // ★ Re-read when the stored text arrives (the screen loads it after mount) — but never over the owner's own typing.
  const last = useRef({ r: rangesText(rangesFrom(ranges)), f: filtersText(filtersFrom(filters)) });
  useEffect(() => {
    if (ranges !== last.current.r) { setRr(rangesFrom(ranges)); last.current.r = ranges; }
    if (filters !== last.current.f) { setFr(filtersFrom(filters)); last.current.f = filters; }
  }, [ranges, filters]);
  const emit = (r: RangeRow[], f: FilterRow[]) => {
    const rt = rangesText(r), ft = filtersText(f);
    last.current = { r: rt, f: ft };
    onChange(rt, ft);
  };
  const setR = (r: RangeRow[]) => { setRr(r); emit(r, fr); };
  const setF = (f: FilterRow[]) => { setFr(f); emit(rr, f); };

  const chip = (label: string, on: boolean, onPress: () => void) => (
    <TouchableOpacity key={label} onPress={onPress} accessibilityRole="button"
      style={{ borderWidth: 1, borderRadius: 7, paddingHorizontal: 9, paddingVertical: 6,
               borderColor: on ? C.green : C.border, backgroundColor: on ? C.green + '18' : 'transparent' }}>
      <Text style={{ color: on ? C.green : C.gold, fontFamily: F, fontSize: 12 }}>{label}</Text>
    </TouchableOpacity>
  );
  const field = (v: string, set: (t: string) => void, ph: string, w?: number, num = false) => (
    <TextInput value={v} onChangeText={set} placeholder={ph} placeholderTextColor={C.textDim}
      keyboardType={num ? 'decimal-pad' : 'default'} autoCorrect={false}
      style={{ borderWidth: 1, borderRadius: 7, borderColor: C.border, color: C.amber, fontFamily: F,
               paddingHorizontal: 8, paddingVertical: 6, ...(w ? { width: w } : { flex: 1, minWidth: 110 }) }} />
  );
  const hint = (t: string) => <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11, marginTop: 4, lineHeight: 15 }}>{t}</Text>;

  return (
    <View style={{ marginTop: 12 }}>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>Frequency ranges this aerial covers (optional)</Text>
      {hint('A listener tuned outside all of them is told "Outside this antenna\'s range — reception may be poor".')}
      {rr.map((r, i) => (
        <View key={`r${i}`} style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 6, alignItems: 'center' }}>
          {field(r.lo, (t) => { const n = rr.slice(); n[i] = { ...r, lo: t }; setR(n); }, 'from', 70, true)}
          <Text style={{ color: C.textDim }}>–</Text>
          {field(r.hi, (t) => { const n = rr.slice(); n[i] = { ...r, hi: t }; setR(n); }, 'to', 70, true)}
          {UNITS.map((u) => chip(u, r.unit === u, () => { const n = rr.slice(); n[i] = { ...r, unit: u }; setR(n); }))}
          {field(r.name, (t) => { const n = rr.slice(); n[i] = { ...r, name: t.slice(0, 40) }; setR(n); }, 'name, e.g. 2 m')}
          {chip('✕', false, () => setR(rr.filter((_, k) => k !== i)))}
        </View>
      ))}
      <View style={{ flexDirection: 'row', gap: 8, marginTop: 8 }}>
        {chip('+ Add range', false, () => setR([...rr, { name: '', lo: '', hi: '', unit: 'MHz' }]))}
        {chip('Fill in a band…', presets === 'range', () => setPresets(presets === 'range' ? '' : 'range'))}
      </View>
      {presets === 'range' && ANTENNA_RANGE_PRESETS.map(([group, list]) => (
        <View key={group} style={{ marginTop: 6 }}>
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11 }}>{group}</Text>
          <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={{ gap: 6, paddingVertical: 4 }}>
            {list.map(([name, lo, hi, unit]) => chip(name, false, () => { setR([...rr, { name, lo, hi, unit }]); setPresets(''); }))}
          </ScrollView>
        </View>
      ))}

      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 14 }}>Filters fitted (optional)</Text>
      {hint('A listener tuned into one is told, e.g. "FM band-stop filter fitted — reception here is deliberately reduced", so they blame the filter, not the receiver.')}
      {fr.map((f, i) => (
        <View key={`f${i}`} style={{ borderWidth: 1, borderColor: C.border, borderRadius: 8, padding: 8, marginTop: 6 }}>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
            {KINDS.map((k) => chip(FILTER_KIND_LABEL[k] ?? k, f.kind === k, () => { const n = fr.slice(); n[i] = { ...f, kind: k }; setF(n); }))}
          </View>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 6, alignItems: 'center' }}>
            {field(f.lo, (t) => { const n = fr.slice(); n[i] = { ...f, lo: t }; setF(n); },
                   f.kind === 'highpass' ? 'passes above' : f.kind === 'lowpass' ? 'passes below' : 'from', 80, true)}
            {(f.kind === 'bandstop' || f.kind === 'bandpass') && (<>
              <Text style={{ color: C.textDim }}>–</Text>
              {field(f.hi, (t) => { const n = fr.slice(); n[i] = { ...f, hi: t }; setF(n); }, 'to', 70, true)}
            </>)}
            {UNITS.map((u) => chip(u, f.unit === u, () => { const n = fr.slice(); n[i] = { ...f, unit: u }; setF(n); }))}
            {chip('✕', false, () => setF(fr.filter((_, k) => k !== i)))}
          </View>
          <View style={{ marginTop: 6, flexDirection: 'row' }}>
            {field(f.name, (t) => { const n = fr.slice(); n[i] = { ...f, name: t.slice(0, 40) }; setF(n); }, 'name, e.g. FM band-stop')}
          </View>
        </View>
      ))}
      <View style={{ flexDirection: 'row', gap: 8, marginTop: 8 }}>
        {chip('+ Add filter', false, () => setF([...fr, { kind: 'bandstop', lo: '', hi: '', unit: 'MHz', name: '' }]))}
        {chip('Fill in a filter…', presets === 'filter', () => setPresets(presets === 'filter' ? '' : 'filter'))}
      </View>
      {presets === 'filter' && (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 6 }}>
          {ANTENNA_FILTER_PRESETS.map(([label, kind, lo, hi, unit, name]) =>
            chip(label, false, () => { setF([...fr, { kind, lo, hi, unit, name }]); setPresets(''); }))}
        </View>
      )}
    </View>
  );
}
