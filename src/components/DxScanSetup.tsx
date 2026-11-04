/**
 * DxScanSetup — the IDLE DX SCANNER's settings on the Android server screen (V12, 2026-11-04 — docs/v12/DX-SCANNER.md).
 * The Setup page's DX Scanner block (vibe_setup_page.h, beside Background Decoding), in its words, for the phone's Server
 * mode and VibeServer Lite: one switch, and under it the bands, the wait, the FM raster and "only the owner may view the
 * DX Log". Stored as ONE JSON string, exactly RadioConfig::dxScan (vibe_dxscan.h parseConfig) — '' = off, as before it
 * existed.
 * ★ Not drawn on an Icom (no IQ to sweep); an Airspy HF+ is offered FM only (it cannot hold a DAB multiplex) —
 *   AGENTS.md: no control that only works on another radio.
 */
import React from 'react';
import { Switch, Text, TouchableOpacity, View } from 'react-native';

export interface DxScanModel { on: boolean; bands: 'both' | 'fm' | 'dab'; waitMin: number; raster: 50 | 100; viewAdmin: boolean;
                               /** kept as saved, not drawn (proof clips are opt-in and not offered yet; the opening gap) */
                               rest?: Record<string, unknown> }
export function parseDxScan(json: string): DxScanModel {
  let j: any = null;
  try { j = json ? JSON.parse(json) : null; } catch { j = null; }
  const c = j && typeof j === 'object' ? j : {};
  const rest: Record<string, unknown> = {};
  if (c.proof === true) rest.proof = true;
  if (typeof c.gapMin === 'number' && c.gapMin !== 30) rest.gapMin = c.gapMin;
  return { on: c.on === true, bands: c.bands === 'fm' || c.bands === 'dab' ? c.bands : 'both',
           waitMin: c.waitMin > 0 ? c.waitMin : 5, raster: Number(c.raster) === 50 ? 50 : 100, viewAdmin: c.viewAdmin === true, rest };
}
/** '' when off and never set — the radio's config stays as it was before this existed. */
export function collectDxScan(m: DxScanModel, wasSet: boolean): string {
  if (!m.on && !wasSet) return '';
  return JSON.stringify({ on: m.on, bands: m.bands, waitMin: m.waitMin, raster: m.raster, viewAdmin: m.viewAdmin, ...(m.rest || {}) });
}

const WAITS = [1, 2, 5, 10, 15, 30, 60];
type Ink = { text: string; textDim: string; border: string; trackOff: string; trackOn: string; thumb: string; section?: string; accent?: string };

export default function DxScanSetup({ C, F, model, onChange, driver, rate }: {
  C: Ink; F?: string; model: DxScanModel; onChange: (m: DxScanModel) => void; driver: string; rate: number;
}) {
  const fmOnly = driver === 'airspyhf';
  const bands = fmOnly ? 'fm' : model.bands;
  const set = (p: Partial<DxScanModel>) => onChange({ ...model, ...p });
  const chip = (label: string, on: boolean, press: () => void, key: string) => (
    <TouchableOpacity key={key} onPress={press} accessibilityRole="button" accessibilityState={{ selected: on }}
                      style={{ borderWidth: 1, borderColor: on ? (C.accent || C.trackOn) : C.border, borderRadius: 6,
                               paddingHorizontal: 10, paddingVertical: 6, marginRight: 6, marginBottom: 6 }}>
      <Text style={{ color: on ? (C.accent || C.trackOn) : C.text, fontFamily: F, fontSize: 13 }}>{label}</Text>
    </TouchableOpacity>
  );
  // ★ The hop count, as the server plans it (vibe_dxscan.h planFm)
  const half = 0.4 * rate - 100e3, r = model.raster * 1e3;
  const perHop = half >= r ? Math.floor((2 * half) / r) + 1 : 0;
  const hops = perHop ? Math.ceil(((108e6 - 87.5e6) / r + 1) / perHop) : 0;
  return (
    <View>
      <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}>
        <Text style={{ color: C.text, fontFamily: F, fontSize: 15, flex: 1, paddingRight: 12 }}>DX Scanner</Text>
        <Switch value={model.on} onValueChange={(v) => set({ on: v })}
                trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} accessibilityLabel="DX Scanner" />
      </View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 8 }}>
        {`DX Scanner — catch Sporadic-E, tropo and meteor scatter while the radio is idle. When nobody has been listening for `
         + `${model.waitMin === 60 ? '1 hour' : `${model.waitMin} minute${model.waitMin === 1 ? '' : 's'}`} the radio slowly sweeps `
         + `${bands === 'both' ? 'Broadcast FM and DAB' : bands === 'fm' ? 'Broadcast FM' : 'DAB'} and keeps every station it identifies `
         + '(the RDS PI on FM, the multiplex on DAB) in a DX Log, apart from your bookmarks. Strong local stations are logged once and then '
         + 'stay out of the way; the first day builds that baseline. The moment anybody connects, the scan stops and the radio goes back to '
         + 'exactly where it was left. Background Decoding profiles come first.'}
      </Text>
      {model.on && (
        <View style={{ marginTop: 10 }}>
          {!fmOnly && (
            <>
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginBottom: 4 }}>BANDS</Text>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
                {chip('FM AND DAB', bands === 'both', () => set({ bands: 'both' }), 'both')}
                {chip('FM ONLY', bands === 'fm', () => set({ bands: 'fm' }), 'fm')}
                {chip('DAB ONLY', bands === 'dab', () => set({ bands: 'dab' }), 'dab')}
              </View>
            </>
          )}
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginBottom: 4, marginTop: 4 }}>START AFTER NOBODY HAS LISTENED FOR</Text>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
            {(WAITS.includes(model.waitMin) ? WAITS : [...WAITS, model.waitMin].sort((a, b) => a - b)).map((w) =>
              chip(w === 60 ? '1 HOUR' : `${w} MIN`, model.waitMin === w, () => set({ waitMin: w }), `w${w}`))}
          </View>
          {bands !== 'dab' && (
            <>
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginBottom: 4, marginTop: 4 }}>FM CHANNEL RASTER</Text>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
                {chip('100 kHz', model.raster === 100, () => set({ raster: 100 }), 'r100')}
                {chip('50 kHz (regions that use it)', model.raster === 50, () => set({ raster: 50 }), 'r50')}
              </View>
              {hops > 0 && (
                <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 2 }}>
                  {`At ${(rate / 1e6).toFixed(3)} MS/s FM takes about ${hops} hops of ~4 s.`}
                </Text>
              )}
            </>
          )}
          <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 8 }}>
            <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1, paddingRight: 12 }}>Only the owner may view the DX Log</Text>
            <Switch value={model.viewAdmin} onValueChange={(v) => set({ viewAdmin: v })}
                    trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} accessibilityLabel="Only the owner may view the DX Log" />
          </View>
        </View>
      )}
    </View>
  );
}
