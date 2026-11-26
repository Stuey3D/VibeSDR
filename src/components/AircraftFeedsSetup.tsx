/**
 * AircraftFeedsSetup — V12 beta96 SERVER-LEVEL AIRCRAFT FEEDS on the Android server screen (docs/v12/ADSB-FEED.md §Feed
 * sources). The Setup page's "Aircraft feeds from other receivers" card (vibe_setup_page.h #feedsCard), the same words, for
 * the phone's Server mode and VibeServer Lite: another receiver's aircraft — a PiAware Pi's dump1090 (Beast) or HFDL from
 * dumphfdl (BaseStation text) — into this server's shared table, so its AIR & SEA map (and the community map) shows them,
 * labelled. Nothing to tune. Stored as ONE JSON list, exactly ServerConfig::aircraftFeeds (src/utils/aircraftFeeds.ts).
 * Also the standalone AIR & SEA map's limits ("Map viewers", "Map time limit") — AirSeaMapLimits below.
 */
import React, { useState } from 'react';
import { Text, TextInput, TouchableOpacity, View } from 'react-native';
import { feedDefaults, feedFromForm, feedTitle, mapCostLine, FEED_LABEL_WORD, FEED_MAX,
         type FeedDir, type FeedFmt, type FeedLabel, type FeedSource } from '../utils/aircraftFeeds';

type Ink = { text: string; textDim: string; border: string; trackOff: string; trackOn: string; thumb: string; accent?: string; warn?: string };

function Choice<T extends string>({ C, F, value, options, onPick }: { C: Ink; F?: string; value: T; options: Array<[T, string]>; onPick: (v: T) => void }) {
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', marginTop: 4 }}>
      {options.map(([v, label]) => (
        <TouchableOpacity key={v} onPress={() => onPick(v)} accessibilityRole="button" accessibilityState={{ selected: value === v }}
                          style={{ borderWidth: 1, borderColor: value === v ? (C.accent || C.trackOn) : C.border, borderRadius: 6,
                                   paddingHorizontal: 10, paddingVertical: 6, marginRight: 6, marginTop: 4 }}>
          <Text style={{ color: value === v ? (C.accent || C.text) : C.text, fontFamily: F, fontSize: 13 }}>{label}</Text>
        </TouchableOpacity>
      ))}
    </View>
  );
}

export default function AircraftFeedsSetup({ C, F, list, onChange, status, error }: {
  C: Ink; F?: string; list: FeedSource[]; onChange: (l: FeedSource[]) => void; status: string[]; error: string;
}) {
  const [fmt, setFmt] = useState<FeedFmt>('beast');
  const [dir, setDir] = useState<FeedDir>('connect');
  const [addr, setAddr] = useState('');
  const [port, setPort] = useState('32010');
  const [label, setLabel] = useState<FeedLabel>('hfdl');
  const [name, setName] = useState('');
  const [msg, setMsg] = useState('');
  const hint = { color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 6 } as const;
  const input = { color: C.text, borderColor: C.border, borderWidth: 1, borderRadius: 6, paddingHorizontal: 8, paddingVertical: 6, fontFamily: F, fontSize: 13 } as const;
  const lbl = (t: string) => <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 10 }}>{t}</Text>;
  const pickFmt = (v: FeedFmt) => { setFmt(v); const d = feedDefaults(v); setDir(d.dir); setPort(String(d.port)); if (v === 'sbs') setLabel(d.label); };
  const add = () => {
    const r = feedFromForm(fmt, dir, addr, port, label, name, list);
    if (r.error !== undefined) { setMsg(r.error); return; }
    onChange([...list, r.src]);
    setMsg('Added. Saved when the server next starts.'); setAddr(''); setName('');
  };
  return (
    <View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
        Aircraft another receiver decodes, shown on this server{'’'}s maps beside its own radio{'’'}s: a PiAware Pi{'’'}s dump1090
        (Beast), or HFDL from dumphfdl (BaseStation text) {'—'} on an UberSDR, for one. Nothing to tune: no radio is used. One
        aircraft heard both ways is one row with both sources.
      </Text>
      {list.length === 0 && <Text style={hint}>No feeds yet.</Text>}
      {list.map((f, i) => (
        <View key={`f${i}`} style={{ flexDirection: 'row', alignItems: 'center', marginTop: 8 }}>
          <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1 }}>
            {feedTitle(f)} <Text style={{ color: C.textDim, fontSize: 12 }}>{`${f.fmt === 'beast' ? 'Beast' : 'BaseStation'} · ${f.dir === 'listen' ? `listens on ${f.port}` : `${f.host}:${f.port}`} · ${FEED_LABEL_WORD[f.label]}`}</Text>
          </Text>
          <TouchableOpacity onPress={() => onChange(list.filter((_, k) => k !== i))} accessibilityRole="button" accessibilityLabel={`Remove ${feedTitle(f)}`}
                            style={{ borderWidth: 1, borderColor: C.border, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginLeft: 6 }}>
            <Text style={{ color: C.text, fontFamily: F, fontSize: 13 }}>REMOVE</Text>
          </TouchableOpacity>
        </View>
      ))}
      {status.map((l, i) => <Text key={`s${i}`} style={[hint, { color: C.accent || C.text }]}>{`Now: ${l}`}</Text>)}
      {list.length < FEED_MAX && (
        <View style={{ marginTop: 12, borderTopWidth: 1, borderTopColor: C.border, paddingTop: 8 }}>
          <Text style={{ color: C.text, fontFamily: F, fontSize: 14, fontWeight: '700' }}>+ Add aircraft feed</Text>
          {lbl('FORMAT')}
          <Choice C={C} F={F} value={fmt} onPick={pickFmt} options={[['beast', 'Beast (dump1090, readsb, PiAware)'], ['sbs', 'BaseStation / SBS text (dumphfdl, dumpvdl2)']]} />
          {lbl('DIRECTION')}
          <Choice C={C} F={F} value={dir} onPick={setDir} options={[['connect', 'This server connects to it'], ['listen', 'It connects to this server (listen on a port)']]} />
          <Text style={hint}>{fmt === 'sbs'
            ? (dir === 'listen' ? 'dumphfdl’s and dumpvdl2’s tcp output connect OUT to an address: choose this, and give the sender this phone’s address and the port.'
                                : 'For a program that serves BaseStation on a port (dump1090’s 30003, for one).')
            : (dir === 'listen' ? 'For a program that pushes Beast out to an address (readsb’s net-connector).' : 'dump1090-fa / readsb serve Beast on port 30005.')}</Text>
          {dir === 'connect' ? (<>
            {lbl('ADDRESS')}
            <TextInput value={addr} onChangeText={setAddr} autoCapitalize="none" autoCorrect={false} maxLength={120}
                       placeholder={fmt === 'sbs' ? 'e.g. 192.168.86.12:30003' : 'e.g. 192.168.86.12:30005'} placeholderTextColor={C.textDim} style={[input, { marginTop: 4 }]} />
          </>) : (<>
            {lbl('PORT')}
            <TextInput value={port} onChangeText={(t) => setPort(t.replace(/[^0-9]/g, ''))} keyboardType="number-pad" maxLength={5} style={[input, { width: 100, marginTop: 4 }]} />
            <Text style={hint}>Only this phone and your local network may connect; anyone else is refused (and logged).</Text>
          </>)}
          {fmt === 'sbs' && (<>
            {lbl('WHAT IT IS')}
            <Choice C={C} F={F} value={label} onPick={setLabel} options={[['hfdl', 'HFDL'], ['vdl2', 'VDL2'], ['acars', 'ACARS'], ['adsb', 'ADS-B']]} />
            <Text style={hint}>How its aircraft are labelled on the maps. HFDL, VDL2 and ACARS are never counted as ADS-B or sent to flight-tracking sites.</Text>
          </>)}
          {lbl('NAME')}
          <TextInput value={name} onChangeText={setName} maxLength={40} placeholder="e.g. PiAware in the loft" placeholderTextColor={C.textDim} style={[input, { marginTop: 4 }]} />
          <TouchableOpacity onPress={add} accessibilityRole="button" style={{ alignSelf: 'flex-start', borderWidth: 1, borderColor: C.accent || C.trackOn, borderRadius: 6, paddingHorizontal: 14, paddingVertical: 7, marginTop: 10 }}>
            <Text style={{ color: C.accent || C.text, fontFamily: F, fontSize: 13, fontWeight: '700' }}>ADD</Text>
          </TouchableOpacity>
          {!!msg && <Text style={hint}>{msg}</Text>}
          <Text style={hint}>
            dumphfdl: add --output decoded:basestation:tcp:address=THIS-PHONE,port=32010 to its options (Docker: EXTRA_ARGS), then add
            a BaseStation feed here that listens on 32010, labelled HFDL. Keep the phone on Wi-Fi: a feed from another receiver
            only works on the same network.
          </Text>
        </View>
      )}
      {!!error && <Text style={[hint, { color: C.warn || C.text }]}>{error}</Text>}
    </View>
  );
}

/** ★★ beta96 THE STANDALONE AIR & SEA MAP's limits — the Setup page's "Air & sea map" card (Map viewers, Map time limit). */
export function AirSeaMapLimits({ C, F, viewers, minutes, onViewers, onMinutes, status }: {
  C: Ink; F?: string; viewers: number; minutes: number; onViewers: (n: number) => void; onMinutes: (n: number) => void; status: string;
}) {
  const hint = { color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 6 } as const;
  const input = { color: C.text, borderColor: C.border, borderWidth: 1, borderRadius: 6, paddingHorizontal: 8, paddingVertical: 6, fontFamily: F, fontSize: 13, width: 90 } as const;
  return (
    <View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
        The landing page{'’'}s AIR &amp; SEA MAP shows this server{'’'}s aircraft, ships, radiosondes and VOR stations to anyone, live,
        without taking the radio. Each person looking at it uses some of your upload.
      </Text>
      <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 10 }}>
        <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1 }}>Map viewers</Text>
        <TextInput value={String(viewers)} keyboardType="number-pad" maxLength={4} accessibilityLabel="Map viewers"
                   onChangeText={(t) => onViewers(Math.max(0, Math.min(1000, Number(t.replace(/[^0-9]/g, '')) || 0)))} style={input} />
      </View>
      <Text style={hint}>How many may have the map open at once. 0 = no limit. The next one is told {'“'}The map is full {'—'} try again shortly{'”'}. Separate from the radios{'’'} listeners: 20 map viewers and a radio limited to 5 listeners means up to 25 people in all.</Text>
      <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 10 }}>
        <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1 }}>Map time limit (minutes)</Text>
        <TextInput value={String(minutes)} keyboardType="number-pad" maxLength={4} accessibilityLabel="Map time limit in minutes"
                   onChangeText={(t) => onMinutes(Math.max(0, Math.min(1440, Number(t.replace(/[^0-9]/g, '')) || 0)))} style={input} />
      </View>
      <Text style={hint}>How long one address may keep the map open. 0 = no limit. A minute before the end the map says so; at the end it stops.</Text>
      <Text style={hint}>{mapCostLine(viewers)}</Text>
      {!!status && <Text style={[hint, { color: C.accent || C.text }]}>{status}</Text>}
    </View>
  );
}
