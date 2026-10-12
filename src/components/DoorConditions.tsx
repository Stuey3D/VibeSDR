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
import React, { useEffect, useMemo, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { withReadAuth } from '../services/vibeAuth';
import { useGlassTheme, type PopupTokens } from './PopupShell';

type Cond = { measured: { band: string; snrDb: number }[]; solar?: { sfi?: number; kp?: number; bands?: Record<string, string> } };
const rate = (db: number) => db >= 15 ? 'Excellent' : db >= 9 ? 'Good' : db >= 4 ? 'Fair' : 'Poor';

export default function DoorConditions({ base, readAuth = '' }: { base: string; readAuth?: string }) {
  const pt = useGlassTheme();
  const s = useMemo(() => makeStyles(pt), [pt]);
  const [c, setC] = useState<Cond | null>(null);
  const [loc, setLoc] = useState('');
  useEffect(() => {
    let dead = false;
    const b = base.replace(/\/+$/, '');
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), 8000);
    // ★ PIN proof on both: a PIN-locked server gates them since RC4 (audit 2026-10-03).
    fetch(withReadAuth(`${b}/vibeserver/conditions`, readAuth), { signal: ctrl.signal }).then(r => r.ok ? r.json() : null)
      .then(j => { if (!dead && j && Array.isArray(j.measured) && j.measured.length) setC(j); })
      .catch(() => {});
    // ★ "Receiver: Moulton, United Kingdom IO92ng" — the web's locLine(), same precedence.
    fetch(withReadAuth(`${b}/location`, readAuth), { signal: ctrl.signal }).then(r => r.ok ? r.json() : null)
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
  }, [base, readAuth]);
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

/* ★★ THE USER'S TEXT COLOUR (Stuart, 2026-10-12: "yes colours apply everywhere including directory if possible") —
 *  was a fixed amber. Every line here is a read-out, so all of it is TEXT (useGlassTheme: the glass skin whatever the
 *  chassis — the door is never plated); the four condition words carry no colour of their own, so nothing here is a
 *  meaning to protect. Built once per token object (the tokens are memoised per setting). */
const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  wrap:  { alignItems: 'center', marginTop: 18, marginBottom: 8 },
  loc:   { color: pt.ink.textDim, fontSize: 11, letterSpacing: 0.5, marginBottom: 10 },
  title: { color: pt.ink.section, fontSize: 10, letterSpacing: 2, marginBottom: 4 },
  row:   { flexDirection: 'row', alignItems: 'center' },
  cell:  { color: pt.ink.textSoft, fontSize: 11, paddingVertical: 1, paddingHorizontal: 12, minWidth: 96 },
  head:  { opacity: 0.65 },
  band:  { color: pt.ink.text, textAlign: 'right', minWidth: 48, paddingLeft: 0 },
  div:   { borderRightWidth: 1, borderRightColor: pt.ink.divider },
  dim:   { opacity: 0.6 },
});
