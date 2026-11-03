/**
 * ★★★ V12 BACKGROUND DECODING on the Android server screen (ServerModeScreen → RADIO SETTINGS → WHEN NOBODY IS LISTENING),
 * for the main app's server mode AND VibeServer Lite, which renders the same screen.
 *
 * The Linux/Mac Setup page's editor (vibe_setup_page.h, "Background Decoding", Radio tab beside the power save) drawn in
 * React Native: the same words, the same order — the switch, Stuart's headline and his sentence with the wait, "Keep
 * decoding stations still in range while listeners are on", then each PROFILE: what it decodes (stations on VFOs, ISM
 * sensors on a whole band, AIS ships — only what this radio can do), the RF Centre, the VFO rows in the order a listener
 * picks (frequency → demodulator → decoder), "+ Add another VFO", "+ DSC watch", and "Background profile active between
 * [00:00] - [00:00]" (UTC; equal = all day). "Want to add another background decoding profile?" appears only under a
 * profile that is not all day.
 * ★ The DECODERS come from the engine on this phone (caps, LocalSdrShim::backgroundCapsJson), never a list kept here.
 * ★ ONE radio per phone: no multi-radio tier list. The priority order (▲ / ▼) appears exactly where the Setup page draws
 *   it on a one-radio server — when the slots can run short (keep-in-range on, or a locked radio); otherwise the VFOs are
 *   simply capped at the decoders this phone runs at once.
 * ★ TV boxes run Lite: every control here is a TouchableOpacity / Switch / TvTextInput, which Android's own focus search
 *   (TvNav.kt) reaches with a D-pad — there is no drag gesture to need a stick; the ▲ / ▼ keys are the reorder.
 */
import React, { useState } from 'react';
import { View, Text, TouchableOpacity, Switch, StyleSheet } from 'react-native';
import TextInput from './TvTextInput';
import { meaningText } from '../constants/decoderTokens';
import {
  type BgCaps, type BgModel, type BgProfile, type BgVfo, BG_DEMODS, BG_ISM_WORDS, BG_WAIT_CHOICES,
  bgBandHint, bgBandReach, bgDecoderSetsFreq, bgDscWatch, bgLocal, bgNdbHint, bgNewProfile, bgSpotBand, bgTime,
  bgHalfWindow,
} from '../services/bgDecode';

type Props = {
  C: any; F: string | undefined;
  model: BgModel;
  /** Every change hands back a NEW model; the screen stores collectBg(model). */
  onChange: (m: BgModel) => void;
  caps: BgCaps;
  driver: string;
  /** What this radio may do in the background (services/bgDecode bgReach, minus what the owner switched off). */
  ismOk: boolean; aisOk: boolean; ndbOk: boolean; ndbBlocked: boolean; bandOk: boolean; dscOk: boolean;
  /** The capture rate the radio runs at (Hz); 0 = not known yet. */
  rate: number;
  /** A locked range: its centre (Hz) — background never moves it; 0 = not locked. */
  lockCentre: number;
  /** The listener slots of a locked radio (each free one is a background channel). */
  users: number;
  /** "Decoders at once" as typed (0 = this device's default, caps.decoderSlotsDefault). */
  decoderMax: number;
  /** ADS-B kept running with nobody connected — it owns the idle radio, so profiles do not run. */
  adsbKeeps: boolean;
  /** The engine's sentence when the profiles would be refused ('' = good). */
  problem: string;
};

const clone = (m: BgModel): BgModel => JSON.parse(JSON.stringify(m));

export default function BackgroundDecodingSetup(p: Props) {
  const { C, F, model, caps } = p;
  const [openDec, setOpenDec] = useState<string>('');   // "pi:vi" — the VFO whose decoder list is open
  const [dscMsg, setDscMsg] = useState<Record<number, string>>({});
  const set = (f: (m: BgModel) => void) => { const m = clone(model); f(m); p.onChange(m); };
  // ★ A change that moves rows (reorder, remove, a profile deleted, DSC watch) remounts the typed boxes, so no box keeps
  //   the text of the row that used to sit where it is.
  const [gen, setGen] = useState(0);
  const setS = (f: (m: BgModel) => void) => { setGen((g) => g + 1); set(f); };
  const hint = [st.hint, { color: C.textDim, fontFamily: F }];
  const badHint = [st.hint, { ...meaningText(C.m, C.bad), fontFamily: F }];
  const locked = p.lockCentre > 0;
  const slots = p.decoderMax > 0 ? p.decoderMax : caps.decoderSlotsDefault;
  // ★★ The priority list wherever slots can run short on a one-radio server (Stuart, 2026-10-22)
  const tiers = model.keepInRange || locked;
  const cap = tiers || !slots ? caps.maxVfos : Math.min(slots, caps.maxVfos);
  const ismBands = p.ismOk && !locked ? caps.ismBands : [];
  const aisOk = p.aisOk && !locked;
  const utcOff = -new Date().getTimezoneOffset();   // ★ the phone IS the server: its local time is the server's

  const key = (label: string, onPress: () => void, active = false, disabled = false, extra?: any) => (
    <TouchableOpacity onPress={disabled ? undefined : onPress} disabled={disabled}
      style={[st.chip, { borderColor: active ? C.keyBorderAct : C.keyBorder, opacity: disabled ? 0.45 : 1 }, extra]}>
      <Text style={{ color: active ? C.keyAct : C.key, fontFamily: F, fontSize: 12 }}>{label}</Text>
    </TouchableOpacity>
  );
  const link = (label: string, onPress: () => void) => (
    <TouchableOpacity onPress={onPress} style={{ paddingVertical: 8 }}>
      <Text style={{ color: C.key, fontFamily: F, fontSize: 13, textDecorationLine: 'underline' }}>{label}</Text>
    </TouchableOpacity>
  );

  const decodersFor = (pr: BgProfile, v: BgVfo) => caps.decoders.filter((d) =>
    d.modes.includes(v.mode)
    && (!d.band || p.bandOk || v.dec === d.id)
    && (!d.ndb || (p.ndbOk && !pr.vfos.some((o) => o !== v && o.dec === 'ndb-skim')) || v.dec === d.id));
  const decLabel = (id: string) => caps.decoders.find((d) => d.id === id)?.label ?? id;

  const vfoRow = (pr: BgProfile, pi: number, v: BgVfo, vi: number) => {
    const fixed = bgDecoderSetsFreq(v.dec);
    const centre = locked ? p.lockCentre : pr.centre;
    const bands = v.dec === 'spots-band' && caps.spotBands.length ? caps.spotBands : null;
    const open = openDec === `${pi}:${vi}`;
    const fits = decodersFor(pr, v);
    const h = v.dec === 'ndb-skim' ? bgNdbHint(p.ndbOk, p.ndbBlocked, centre, p.rate, locked)
            : v.dec === 'spots-band' ? bgBandHint(p.bandOk, caps.spotBands, v.hz, centre, p.rate) : null;
    const setV = (f: (v2: BgVfo, pr2: BgProfile) => void) => set((m) => f(m.profiles[pi].vfos[vi], m.profiles[pi]));
    return (
      <View key={vi} style={[st.vfo, { borderColor: C.border }]}>
        <View style={st.row}>
          {tiers && (<>
            <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }} accessibilityLabel={`Priority ${vi + 1}`}>{vi + 1}</Text>
            {key('▲', () => setS((m) => { const a = m.profiles[pi].vfos; const x = a.splice(vi, 1)[0]; a.splice(vi - 1, 0, x); }), false, vi === 0)}
            {key('▼', () => setS((m) => { const a = m.profiles[pi].vfos; const x = a.splice(vi, 1)[0]; a.splice(vi + 1, 0, x); }), false, vi === pr.vfos.length - 1)}
          </>)}
          <Text style={{ color: C.text, fontFamily: F, fontSize: 13 }}>VFO {vi + 1}</Text>
          {!bands && (
            <View style={[st.row, { flexShrink: 1 }]}>
              <TextInput editable={v.dec !== 'ndb-skim'}
                defaultValue={v.hz > 0 && v.dec !== 'ndb-skim' ? String(v.hz / 1e3) : ''}
                key={`f${gen}:${pi}:${vi}:${v.dec}`}
                onChangeText={(t) => { const hz = Math.round((parseFloat(t.replace(',', '.')) || 0) * 1e3); setV((x) => { x.hz = hz; }); }}
                keyboardType="decimal-pad" placeholder={v.dec === 'ndb-skim' ? 'auto' : '517.5'} placeholderTextColor={C.placeholderDim}
                style={[st.input, { width: 100, color: C.text, borderColor: C.border, fontFamily: F, opacity: v.dec === 'ndb-skim' ? 0.45 : 1 }]} />
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>kHz</Text>
            </View>
          )}
        </View>
        {bands && (
          <View style={[st.row, { flexWrap: 'wrap' }]}>
            {/* ★ a Band VFO picks its BAND, from the engine's own list — only the bands this capture holds */}
            {bands.filter((b) => bgBandReach(b, centre, p.rate) || bgSpotBand(bands, v.hz)?.id === b.id).map((b) =>
              <React.Fragment key={b.id}>{key(b.name + (bgBandReach(b, centre, p.rate) ? '' : ' (outside this window)'),
                () => setV((x) => { x.hz = b.hz; }), bgSpotBand(bands, v.hz)?.id === b.id)}</React.Fragment>)}
          </View>
        )}
        <View style={[st.row, { flexWrap: 'wrap' }]}>
          {Object.keys(BG_DEMODS).map((k) => <React.Fragment key={k}>{key(BG_DEMODS[k], () => setV((x, pr2) => {
            x.mode = k;
            // ★ only the decoders that make sense for this demodulator (Stuart) — refilled when it changes
            const ok = decodersFor(pr2, x);
            if (!ok.some((d) => d.id === x.dec)) x.dec = ok.length ? ok[0].id : '';
          }), v.mode === k, fixed)}</React.Fragment>)}
          {key(decLabel(v.dec) + (open ? '  ▴' : '  ▾'), () => setOpenDec(open ? '' : `${pi}:${vi}`), true)}
          {key('Remove', () => setS((m) => { const a = m.profiles[pi].vfos; a.splice(vi, 1); if (!a.length) a.push({ hz: 0, mode: 'usb', dec: 'navtex' }); }))}
        </View>
        {open && (
          <View style={{ gap: 4, marginTop: 4 }}>
            {fits.map((d) => (
              <TouchableOpacity key={d.id} onPress={() => { setOpenDec(''); setV((x) => {
                  x.dec = d.id;
                  if (bgDecoderSetsFreq(d.id)) {
                    x.mode = 'usb';
                    if (d.id === 'ndb-skim') x.hz = 0;
                    // ★ a row becoming Band lands on the first band this capture holds (never a stray NAVTEX frequency)
                    if (d.id === 'spots-band' && caps.spotBands.length && !bgSpotBand(caps.spotBands, x.hz)) {
                      const first = caps.spotBands.find((b) => bgBandReach(b, centre, p.rate));
                      x.hz = first ? first.hz : 0;
                    }
                  }
                }); }}
                style={[st.opt, { borderColor: v.dec === d.id ? C.keyBorderAct : C.border }]}>
                <Text style={{ color: v.dec === d.id ? C.keyAct : C.key, fontFamily: F, fontSize: 13 }}>{d.label}</Text>
                {v.dec === d.id && <Text style={{ color: C.keyAct, fontFamily: F }}>✓</Text>}
              </TouchableOpacity>
            ))}
          </View>
        )}
        {h && <Text style={h.bad ? badHint : hint}>{h.text}</Text>}
      </View>
    );
  };

  const profile = (pr: BgProfile, pi: number) => {
    const kind = pr.ism && ismBands.length ? 'ism' : pr.ais && aisOk ? 'ais' : pr.ism || pr.ais ? 'cannot' : 'vfo';
    const allDay = pr.from === pr.to;
    const over = !tiers && slots && pr.vfos.length > slots;
    return (
      <View key={pi} style={[st.card, { borderColor: C.border }]}>
        <View style={[st.row, { justifyContent: 'space-between' }]}>
          <Text style={{ color: C.text, fontFamily: F, fontSize: 14 }}>Profile {pi + 1}</Text>
          {key('Delete profile', () => setS((m) => { m.profiles.splice(pi, 1); }))}
        </View>
        {(ismBands.length > 0 || aisOk || !!pr.ism || !!pr.ais) && (<>
          <Text style={hint}>Decodes</Text>
          <View style={[st.row, { flexWrap: 'wrap' }]}>
            {key('Stations on VFOs (RF centre + VFOs)', () => set((m) => { const q = m.profiles[pi]; delete q.ism; delete q.ais; if (!q.vfos.length) q.vfos.push({ hz: 0, mode: 'usb', dec: 'navtex' }); }), kind === 'vfo')}
            {ismBands.length > 0 && key('ISM sensors on a whole band', () => set((m) => { const q = m.profiles[pi]; delete q.ais; q.ism = ismBands.includes('433') ? '433' : ismBands[0]; q.centre = 0; q.vfos = []; }), kind === 'ism')}
            {aisOk && key('AIS ships (161.975 + 162.025 MHz)', () => set((m) => { const q = m.profiles[pi]; delete q.ism; q.ais = true; q.centre = 0; q.vfos = []; }), kind === 'ais')}
          </View>
        </>)}
        {kind === 'cannot' && (
          <Text style={badHint}>{'This profile decodes ' + (pr.ism ? 'ISM' : 'AIS') + ', which this radio cannot do'
            + (locked ? ' — its range is locked, and background decoding never moves a locked radio.'
                      : pr.ais ? ' — it does not reach 162 MHz, or AIS is switched off on it.' : '.')
            + ' Choose “Stations on VFOs” or delete the profile.'}</Text>
        )}
        {kind === 'ais' && (
          <Text style={hint}>AIS takes the whole radio onto 162.000 MHz and decodes every ship on both channels {'—'} so it runs
            only while nobody is listening, and stops the moment anybody connects. The ships (each one{'’'}s latest report and a
            short track) are kept in the gallery for the SHIPPING MAP, which every radio on this server can show, beside NAVTEX and
            DSC. Each day{'’'}s AIS log and summary go to the gallery too.</Text>
        )}
        {kind === 'ism' && (<>
          <Text style={hint}>ISM band</Text>
          {ismBands.map((id) => (
            <TouchableOpacity key={id} onPress={() => set((m) => { m.profiles[pi].ism = id; })}
              style={[st.opt, { borderColor: pr.ism === id ? C.keyBorderAct : C.border }]}>
              <Text style={{ color: pr.ism === id ? C.keyAct : C.key, fontFamily: F, fontSize: 13, flex: 1 }}>{BG_ISM_WORDS[id] ?? id + ' MHz'}</Text>
              {pr.ism === id && <Text style={{ color: C.keyAct, fontFamily: F }}>✓</Text>}
            </TouchableOpacity>
          ))}
          <Text style={hint}>ISM takes the whole radio onto the band and decodes every sensor in it {'—'} so it runs only while
            nobody is listening, and stops the moment anybody connects. Each day{'’'}s readings and a summary go to the gallery.
            Tyre sensors, key fobs and remotes are only ever kept by nickname.</Text>
        </>)}
        {kind === 'vfo' && (<>
          {!locked && (
            <View style={st.row}>
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>RF Centre</Text>
              <TextInput defaultValue={pr.centre > 0 ? String(pr.centre / 1e3) : ''} key={`c${gen}:${pi}`}
                onChangeText={(t) => { const hz = Math.round((parseFloat(t.replace(',', '.')) || 0) * 1e3); set((m) => { m.profiles[pi].centre = hz; }); }}
                keyboardType="decimal-pad" placeholder="500" placeholderTextColor={C.placeholderDim}
                style={[st.input, { width: 120, color: C.text, borderColor: C.border, fontFamily: F }]} />
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>kHz</Text>
            </View>
          )}
          {pr.vfos.map((v, vi) => vfoRow(pr, pi, v, vi))}
          {over ? (
            <Text style={badHint}>{(() => {
              const o: string[] = []; for (let k = slots + 1; k <= pr.vfos.length; k++) o.push(String(k));
              return 'VFO ' + (o.length === 1 ? o[0] : o.slice(0, -1).join(', ') + ' and ' + o[o.length - 1])
                + ' won’t run — this server can run ' + slots + ' decoders.';
            })()}</Text>
          ) : null}
          {tiers && locked ? (
            <Text style={hint}>{'up to ' + Math.max(1, p.users) + ' background VFOs while nobody listens (this radio’s '
              + Math.max(1, p.users) + ' user slots)' + (slots ? ' · ' + slots + ' decoders on this server' : '') + ' · listeners always come first'}</Text>
          ) : tiers && slots ? (
            <Text style={hint}>{pr.vfos.length + (pr.vfos.length === 1 ? ' VFO' : ' VFOs') + ' · up to ' + Math.min(pr.vfos.length, slots)
              + ' of this server’s ' + slots + ' decoder slots · listeners always come first; the lowest in the list pauses when a listener needs a decoder'}</Text>
          ) : null}
          {pr.vfos.length < cap && link('+ Add another VFO', () => set((m) => { m.profiles[pi].vfos.push({ hz: 0, mode: 'usb', dec: 'navtex' }); }))}
          {pr.vfos.length < cap && p.dscOk && link('+ DSC watch', () => {
            const m = clone(model);
            const msg = bgDscWatch(m.profiles[pi], p.rate, locked, p.lockCentre, cap, p.driver);
            setDscMsg((d) => ({ ...d, [pi]: msg }));
            setGen((g) => g + 1);
            p.onChange(m);
          })}
          {!!dscMsg[pi] && <Text style={hint}>{dscMsg[pi]}</Text>}
        </>)}
        {/* times (UTC) with this server's local time beside */}
        <View style={[st.row, { flexWrap: 'wrap', marginTop: 6 }]}>
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>Background profile active between</Text>
          {(['from', 'to'] as const).map((w, i) => (
            <React.Fragment key={w}>
              {i === 1 && <Text style={{ color: C.textDim, fontFamily: F }}>-</Text>}
              <TextInput defaultValue={pr[w]} key={`${w}${gen}:${pi}`} maxLength={5}
                onEndEditing={(e) => { const t = bgTime(e.nativeEvent.text) ?? '00:00'; set((m) => { m.profiles[pi][w] = t; }); }}
                onSubmitEditing={(e) => { const t = bgTime(e.nativeEvent.text) ?? '00:00'; set((m) => { m.profiles[pi][w] = t; }); }}
                keyboardType="numbers-and-punctuation" placeholder="00:00" placeholderTextColor={C.placeholderDim}
                style={[st.input, { width: 78, textAlign: 'center', color: C.text, borderColor: C.border, fontFamily: F }]} />
            </React.Fragment>
          ))}
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
            {allDay ? 'UTC — all day' : 'UTC (' + bgLocal(pr.from, utcOff) + ' – ' + bgLocal(pr.to, utcOff) + ' server time)'}
          </Text>
        </View>
      </View>
    );
  };

  const half = bgHalfWindow(p.rate);
  return (
    <View style={{ gap: 6 }}>
      <View style={[st.row, { justifyContent: 'space-between' }]}>
        <Text style={{ color: C.text, fontFamily: F, fontSize: 15, flex: 1 }}>Background Decoding</Text>
        <Switch value={model.on}
          onValueChange={(v) => set((m) => { m.on = v; if (v && !m.profiles.length) m.profiles.push(bgNewProfile()); })}
          trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} />
      </View>
      <Text style={[hint, { color: C.text }]}>Background Decoding {'—'} make your radios work while the server is idle.</Text>
      <Text style={hint}>When the last user leaves the server after</Text>
      <View style={[st.row, { flexWrap: 'wrap' }]}>
        {(BG_WAIT_CHOICES.includes(model.waitMin) ? BG_WAIT_CHOICES : [...BG_WAIT_CHOICES, model.waitMin].sort((a, b) => a - b)).map((w) =>
          <React.Fragment key={w}>{key(w === 60 ? '1 hour' : w === 1 ? '1 minute' : `${w} minutes`, () => set((m) => { m.waitMin = w; }), model.waitMin === w)}</React.Fragment>)}
      </View>
      <Text style={hint}>
        the SDR will move to your selected RF Centre. Set an RF centre here and choose 1 or multiple VFO's and decoders for
        background decoding.
        {p.ndbOk ? ' A VFO can also be the NDB skimmer — every beacon in as much of the 190–535 kHz band as the profile’s RF centre holds, beside the other VFOs.' : ''}
        {ismBands.length ? ' Or let a profile decode ISM sensors (weather stations, tyre sensors, doorbells, and on 868 MHz water, heat and gas meters) on a whole band instead.' : ''}
        {'\n'}The moment anybody connects, background decoding stops and the radio goes back to exactly where it was left. What it
        decodes is kept in the gallery (GALLERY, under Server settings).
      </Text>
      <View style={[st.row, { justifyContent: 'space-between' }]}>
        <Text style={{ color: C.text, fontFamily: F, fontSize: 13, flex: 1 }}>Keep decoding stations still in range while listeners are on</Text>
        <Switch value={model.keepInRange} onValueChange={(v) => set((m) => { m.keepInRange = v; })}
          trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} />
      </View>
      {p.adsbKeeps && (
        <Text style={[hint, { borderLeftWidth: 3, borderLeftColor: C.warn, paddingLeft: 8 }]}>ADS-B is kept running on this radio
          when nobody is connected (Where listeners start), so these profiles do not run. Untick it there to decode in the
          background instead.</Text>
      )}
      {model.on && (<>
        {locked && (
          <Text style={hint}>{'This radio’s range is locked (' + (Math.max(0, p.lockCentre - half) / 1e6).toFixed(3) + '–'
            + ((p.lockCentre + half) / 1e6).toFixed(3) + ' MHz): background VFOs must sit inside it. They decode all through their profile’s hours, listeners or not — the radio is never moved.'}</Text>
        )}
        {tiers ? (
          <Text style={hint}>On CPU-limited servers with a limited number of decoders, real listeners get decoder priority {'—'} the
            lowest VFO in this list pauses when a listener needs a decoder and resumes when one is free.</Text>
        ) : slots ? (
          <Text style={hint}>{'This server can run ' + slots + (slots === 1 ? ' decoder' : ' decoders') + ' at once — that’s the most VFOs a profile can have.'}</Text>
        ) : null}
        {model.profiles.map(profile)}
        {model.profiles.length > 0 && model.profiles[model.profiles.length - 1].from !== model.profiles[model.profiles.length - 1].to
          && model.profiles.length < caps.maxProfiles
          && link('Want to add another background decoding profile?', () => set((m) => { m.profiles.push(bgNewProfile()); }))}
        {model.profiles.length === 0 && link('+ Add a profile', () => set((m) => { m.profiles.push(bgNewProfile()); }))}
        {!!p.problem && <Text style={badHint}>{p.problem}</Text>}
      </>)}
    </View>
  );
}

const st = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  card: { borderWidth: 1, borderRadius: 10, padding: 12, gap: 8, marginTop: 6 },
  vfo: { borderTopWidth: 1, paddingTop: 8, gap: 6 },
  chip: { borderWidth: 1, borderRadius: 14, paddingHorizontal: 10, paddingVertical: 6 },
  opt: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 10, paddingVertical: 8, fontSize: 15 },
  hint: { fontSize: 11.5, lineHeight: 16 },
});
