/**
 * DoorConditions — PREDICTED vs ACTUAL band conditions, and where the receiver is.
 *
 * ★★★ THE WEB LANDING PAGE'S BLOCK, on the app's door. The value is the COMPARISON: the same four
 *     words in both columns (Excellent / Good / Fair / Poor), so a reader can see the prediction
 *     and this aerial disagree without learning two scales — and the dB beside the word, because
 *     the word is a bucket and the number is the measurement. Same thresholds as the web.
 * ★ The MEASURED list defines which bands exist here: it comes from what is inside the captured
 *   span, so an FM-only receiver shows nothing and an HF one shows its bands, with nothing in this
 *   component knowing the centre or the span. Silent on any failure.
 */
import React, { useEffect, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';

type Cond = { measured: { band: string; snrDb: number }[]; solar?: { sfi?: number; kp?: number; bands?: Record<string, string> } };
const rate = (db: number) => db >= 15 ? 'Excellent' : db >= 9 ? 'Good' : db >= 4 ? 'Fair' : 'Poor';

export default function DoorConditions({ base }: { base: string }) {
  const [c, setC] = useState<Cond | null>(null);
  const [loc, setLoc] = useState('');
  useEffect(() => {
    let dead = false;
    const b = base.replace(/\/+$/, '');
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), 8000);
    fetch(`${b}/vibeserver/conditions`, { signal: ctrl.signal }).then(r => r.ok ? r.json() : null)
      .then(j => { if (!dead && j && Array.isArray(j.measured) && j.measured.length) setC(j); })
      .catch(() => {});
    // ★ "Receiver: Moulton, United Kingdom IO92ng" — the web's locLine(), same precedence.
    fetch(`${b}/location`, { signal: ctrl.signal }).then(r => r.ok ? r.json() : null)
      .then(j => {
        if (dead || !j) return;
        const lat = typeof j.lat === 'number' ? j.lat : null, lon = typeof j.lon === 'number' ? j.lon : null;
        const grid = typeof j.grid === 'string' ? j.grid : '';
        const label = typeof j.label === 'string' && j.label ? j.label : (lat != null && lon != null ? `${lat.toFixed(2)}, ${lon.toFixed(2)}` : '');
        if (!label && !grid) return;
        const isGrid = /^[A-R]{2}[0-9]{2}([A-X]{2})?$/i.test(label);
        const country = typeof j.country === 'string' && j.country && !isGrid && !label.includes(j.country) ? `, ${j.country}` : '';
        setLoc(`${label}${country}${grid && !isGrid && grid !== label ? ` ${grid}` : ''}`.trim());
      })
      .catch(() => {});
    return () => { dead = true; clearTimeout(t); };
  }, [base]);
  if (!c && !loc) return null;
  return (
    <View style={s.wrap}>
      {!!loc && <Text style={s.loc}>Receiver: {loc}</Text>}
      {!!c && (<>
        <Text style={s.title}>BAND CONDITIONS{c.solar ? `  ·  SFI ${c.solar.sfi ?? '—'} · K ${c.solar.kp ?? '—'}` : ''}</Text>
        <View style={s.row}>
          <Text style={[s.cell, s.band]} />
          <Text style={[s.cell, s.head, s.div]}>PREDICTED</Text>
          <Text style={[s.cell, s.head]}>ACTUAL</Text>
        </View>
        {c.measured.map(m => (
          <View key={m.band} style={s.row}>
            <Text style={[s.cell, s.band]}>{m.band}</Text>
            <Text style={[s.cell, s.div]}>{c.solar?.bands?.[m.band] || '—'}</Text>
            <Text style={s.cell}>{rate(m.snrDb)}<Text style={s.dim}> ({m.snrDb.toFixed(0)} dB)</Text></Text>
          </View>
        ))}
      </>)}
    </View>
  );
}

const AMBER = '#ffb833';
const s = StyleSheet.create({
  wrap:  { alignItems: 'center', marginTop: 18, marginBottom: 8 },
  loc:   { color: 'rgba(255,184,51,0.6)', fontSize: 11, letterSpacing: 0.5, marginBottom: 10 },
  title: { color: 'rgba(255,184,51,0.8)', fontSize: 10, letterSpacing: 2, marginBottom: 4 },
  row:   { flexDirection: 'row', alignItems: 'center' },
  cell:  { color: 'rgba(255,220,160,0.92)', fontSize: 11, paddingVertical: 1, paddingHorizontal: 12, minWidth: 96 },
  head:  { opacity: 0.65 },
  band:  { color: AMBER, textAlign: 'right', minWidth: 48, paddingLeft: 0 },
  div:   { borderRightWidth: 1, borderRightColor: 'rgba(255,184,51,0.28)' },
  dim:   { opacity: 0.6 },
});
