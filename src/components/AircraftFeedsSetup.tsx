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
import { feedAddrWhy, feedDefaults, feedFindWords, feedFmtWord, feedFromForm, feedIsAdsb, feedKey, feedTestWords, feedTitle, feedWithSite, mapCostLine, FEED_LABEL_WORD, FEED_MAX,
         type FeedDir, type FeedFmt, type FeedHit, type FeedLabel, type FeedSource } from '../utils/aircraftFeeds';

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

/** ★★ beta96 follow-up (Stuart 11-27: "we need to test the connection before saving" · "the easiest way possible please";
 *  docs/v12/FEED-TEST.md). `test` asks the engine (VibeLocalSDR.feedTest → vibe_feed_test.h): {src} tests one source,
 *  {find} looks at an address. Without it (an engine that cannot), the form is the plain manual one — no dead TEST/FIND. */
export type FeedTestFn = (body: { src: FeedSource } | { find: string }) => Promise<any>;

export default function AircraftFeedsSetup({ C, F, list, onChange, status, error, test }: {
  C: Ink; F?: string; list: FeedSource[]; onChange: (l: FeedSource[]) => void; status: string[]; error: string; test?: FeedTestFn;
}) {
  const [fmt, setFmt] = useState<FeedFmt>('beast');
  const [dir, setDir] = useState<FeedDir>('connect');
  const [addr, setAddr] = useState('');
  const [port, setPort] = useState('32010');
  const [label, setLabel] = useState<FeedLabel>('hfdl');
  const [name, setName] = useState('');
  const [msg, setMsg] = useState('');
  const [find, setFind] = useState('');
  const [findMsg, setFindMsg] = useState('');
  const [hits, setHits] = useState<FeedHit[]>([]);
  const [busy, setBusy] = useState(false);
  const [advanced, setAdvanced] = useState(!test);
  const [pending, setPending] = useState<FeedSource | null>(null);
  const [tested, setTested] = useState<Record<string, { ok: boolean; text: string }>>({});
  const [offsite, setOffsite] = useState(false);   // ★★ b96-2 "Not at this site" for the feed being added
  const hint = { color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 6 } as const;
  const input = { color: C.text, borderColor: C.border, borderWidth: 1, borderRadius: 6, paddingHorizontal: 8, paddingVertical: 6, fontFamily: F, fontSize: 13 } as const;
  const btn = (primary: boolean) => ({ alignSelf: 'flex-start' as const, borderWidth: 1, borderColor: primary ? (C.accent || C.trackOn) : C.border, borderRadius: 6,
                                       paddingHorizontal: 14, paddingVertical: 7, marginTop: 10, marginRight: 8, opacity: busy ? 0.5 : 1 });
  const btnText = (primary: boolean) => ({ color: primary ? (C.accent || C.text) : C.text, fontFamily: F, fontSize: 13, fontWeight: '700' as const });
  const lbl = (t: string) => <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 10 }}>{t}</Text>;
  const pickFmt = (v: FeedFmt) => { setFmt(v); const d = feedDefaults(v); setDir(d.dir); setPort(String(d.port)); if (v === 'sbs') setLabel(d.label); };
  const addOne = (src0: FeedSource, say: (t: string) => void, lead = '') => {
    const src = feedWithSite(src0, offsite);   // ★★ b96-2 the tick (ADS-B feeds only — the outline is ADS-B only)
    if (list.length >= FEED_MAX) { say(`Not added — at most ${FEED_MAX} aircraft feeds.`); return false; }
    if (list.some((x) => feedKey(x) === feedKey(src))) { say('That feed is already in the list.'); return false; }
    onChange([...list, src]);
    setOffsite(false);   // ★ one feed's tick: the next starts at this site
    say(`${lead ? lead + ' ' : ''}Added. Saved when the server next starts.`);
    return true;
  };
  const ask = async (body: { src: FeedSource } | { find: string }) => { try { return await test!(body); } catch { return null; } };
  const doFind = async () => {
    const why = feedAddrWhy(find);
    if (why) { setFindMsg(why); return; }
    setBusy(true); setHits([]); setFindMsg(`Looking at ${find.trim()}… (about 5 s)`);
    const w = feedFindWords(await ask({ find: find.trim() }));
    setBusy(false); setHits(w.hits); setFindMsg(w.text);
  };
  const formSrc = () => feedFromForm(fmt, dir, addr, port, label, name, list);
  const doTest = async (andAdd: boolean) => {
    setPending(null);
    const r = formSrc();
    if (r.error !== undefined) { setMsg((andAdd ? 'Not added — ' : 'Not tested — ') + r.error); return; }
    if (!test) { if (andAdd && addOne(r.src, setMsg)) { setAddr(''); setName(''); } return; }
    setBusy(true); setMsg('Testing… (up to 5 s)');
    const w = feedTestWords(await ask({ src: r.src }));
    setBusy(false); setMsg(w.text);
    if (!andAdd) return;
    if (!w.ok) { setPending(r.src); return; }
    if (addOne(r.src, setMsg, w.text)) { setAddr(''); setName(''); }
  };
  const testRow = async (f: FeedSource) => {
    setTested((t) => ({ ...t, [feedKey(f)]: { ok: true, text: 'Testing… (up to 5 s)' } }));
    const w = feedTestWords(await ask({ src: f }));
    setTested((t) => ({ ...t, [feedKey(f)]: w }));
  };
  return (
    <View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
        Aircraft another receiver decodes, shown on this server{'’'}s maps beside its own radio{'’'}s: a PiAware Pi{'’'}s dump1090
        (Beast), an UberSDR{'’'}s HFDL add-on, or HFDL from dumphfdl (BaseStation text). Nothing to tune: no radio is used. One
        aircraft heard both ways is one row with both sources.
      </Text>
      {list.length === 0 && <Text style={hint}>No feeds yet.</Text>}
      {list.map((f, i) => {
        const t = tested[feedKey(f)];
        return (
          <View key={`f${i}`} style={{ marginTop: 8 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center' }}>
              <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1 }}>
                {feedTitle(f)} {f.offsite && feedIsAdsb(f) && (
                  <Text accessibilityLabel="off-site: its aircraft are shown but not counted in this server’s range outline"
                        style={{ color: C.accent || C.text, fontSize: 11, borderWidth: 1, borderColor: C.accent || C.border }}>off-site</Text>
                )}{f.offsite && feedIsAdsb(f) ? ' ' : ''}<Text style={{ color: C.textDim, fontSize: 12 }}>{`${feedFmtWord(f.fmt)} · ${f.dir === 'listen' ? `listens on ${f.port}` : `${f.host}:${f.port}`} · ${FEED_LABEL_WORD[f.label]}`}</Text>
              </Text>
              {!!test && (
                <TouchableOpacity onPress={() => void testRow(f)} accessibilityRole="button" accessibilityLabel={`Test ${feedTitle(f)}`}
                                  style={{ borderWidth: 1, borderColor: C.border, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginLeft: 6 }}>
                  <Text style={{ color: C.text, fontFamily: F, fontSize: 13 }}>TEST</Text>
                </TouchableOpacity>
              )}
              <TouchableOpacity onPress={() => onChange(list.filter((_, k) => k !== i))} accessibilityRole="button" accessibilityLabel={`Remove ${feedTitle(f)}`}
                                style={{ borderWidth: 1, borderColor: C.border, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginLeft: 6 }}>
                <Text style={{ color: C.text, fontFamily: F, fontSize: 13 }}>REMOVE</Text>
              </TouchableOpacity>
            </View>
            {!!t && <Text style={[hint, { color: t.ok ? (C.accent || C.text) : (C.warn || C.text) }]}>{t.text}</Text>}
          </View>
        );
      })}
      {status.map((l, i) => <Text key={`s${i}`} style={[hint, { color: C.accent || C.text }]}>{`Now: ${l}`}</Text>)}
      {list.length < FEED_MAX && (
        <View style={{ marginTop: 12, borderTopWidth: 1, borderTopColor: C.border, paddingTop: 8 }}>
          <Text style={{ color: C.text, fontFamily: F, fontSize: 14, fontWeight: '700' }}>+ Add aircraft feed</Text>
          {/* ★★ b96-2 "NOT AT THIS SITE" (Stuart chose option 1, 11-27): one tick for the feed being added — FIND's ADD and
              Advanced's ADD both read it; stored on an ADS-B feed only (the Setup page's #feedOffsite) */}
          <TouchableOpacity onPress={() => setOffsite(!offsite)} accessibilityRole="checkbox" accessibilityState={{ checked: offsite }}
                            accessibilityLabel="Not at this site — its aircraft are shown but not counted in this server’s range outline"
                            style={{ flexDirection: 'row', alignItems: 'flex-start', marginTop: 10 }}>
            <Text style={{ color: offsite ? (C.accent || C.trackOn) : C.textDim, fontFamily: F, fontSize: 16, marginRight: 8 }}>{offsite ? '☑' : '☐'}</Text>
            <Text style={{ color: C.text, fontFamily: F, fontSize: 13, flex: 1 }}>Not at this site — its aircraft are shown but not counted in this server{'’'}s range outline</Text>
          </TouchableOpacity>
          <Text style={hint}>For a receiver somewhere else (a friend{'’'}s PiAware across town). Only ADS-B draws the range outline, so this applies to ADS-B feeds; an aircraft this server also hears itself still counts.</Text>
          {!!test && (<>
            {lbl('THE OTHER RECEIVER’S ADDRESS')}
            <TextInput value={find} onChangeText={setFind} autoCapitalize="none" autoCorrect={false} maxLength={120} accessibilityLabel="The other receiver's address"
                       placeholder="e.g. 192.168.86.12" placeholderTextColor={C.textDim} style={[input, { marginTop: 4 }]} onSubmitEditing={() => void doFind()} />
            <Text style={hint}>
              The phone looks for PiAware or dump1090 (Beast, port 30005), BaseStation text (30003) and an UberSDR{'’'}s HFDL add-on
              (8080) there, and fills in the rest. An UberSDR needs no change: its HFDL add-on already shares its aircraft. Add :port
              for an unusual port.
            </Text>
            <TouchableOpacity onPress={() => void doFind()} disabled={busy} accessibilityRole="button" style={btn(true)}>
              <Text style={btnText(true)}>FIND</Text>
            </TouchableOpacity>
            {!!findMsg && <Text style={hint}>{findMsg}</Text>}
            {hits.map((h, i) => (
              <View key={`h${i}`} style={{ flexDirection: 'row', alignItems: 'center', marginTop: 6 }}>
                <Text style={{ color: C.text, fontFamily: F, fontSize: 13, flex: 1 }}>{h.line}</Text>
                <TouchableOpacity accessibilityRole="button" accessibilityLabel={`Add ${h.name || feedTitle(h.src)}`}
                                  onPress={() => { const src = { ...h.src, ...(h.name ? { name: h.name } : {}) };
                                                   if (addOne(src, setFindMsg)) { const rest = hits.filter((_, k) => k !== i); setHits(rest); if (!rest.length) setFind(''); } }}
                                  style={{ borderWidth: 1, borderColor: C.accent || C.trackOn, borderRadius: 6, paddingHorizontal: 12, paddingVertical: 6, marginLeft: 6 }}>
                  <Text style={btnText(true)}>ADD</Text>
                </TouchableOpacity>
              </View>
            ))}
            <TouchableOpacity onPress={() => setAdvanced(!advanced)} accessibilityRole="button" style={{ marginTop: 12 }}>
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 13 }}>{advanced ? '▾' : '▸'} Advanced — a feed that connects to this phone, or set it by hand</Text>
            </TouchableOpacity>
          </>)}
          {advanced && (<>
            {lbl('FORMAT')}
            <Choice C={C} F={F} value={fmt} onPick={pickFmt} options={[['beast', 'Beast (dump1090, readsb, PiAware)'], ['uberhfdl', 'UberSDR HFDL add-on'], ['sbs', 'BaseStation / SBS text (dumphfdl, dumpvdl2)']]} />
            {fmt !== 'uberhfdl' && (<>
              {lbl('DIRECTION')}
              <Choice C={C} F={F} value={dir} onPick={setDir} options={[['connect', 'This server connects to it'], ['listen', 'It connects to this server (listen on a port)']]} />
            </>)}
            <Text style={hint}>{fmt === 'uberhfdl'
              ? 'The phone reads the UberSDR’s HFDL add-on every 5 seconds (its web port, usually 8080). Nothing to change on the UberSDR.'
              : fmt === 'sbs'
              ? (dir === 'listen' ? 'dumphfdl’s and dumpvdl2’s tcp output connect OUT to an address: choose this, and give the sender this phone’s address and the port.'
                                  : 'For a program that serves BaseStation on a port (dump1090’s 30003, for one).')
              : (dir === 'listen' ? 'For a program that pushes Beast out to an address (readsb’s net-connector).' : 'dump1090-fa / readsb serve Beast on port 30005.')}</Text>
            {dir === 'connect' || fmt === 'uberhfdl' ? (<>
              {lbl('ADDRESS')}
              <TextInput value={addr} onChangeText={setAddr} autoCapitalize="none" autoCorrect={false} maxLength={120}
                         placeholder={fmt === 'sbs' ? 'e.g. 192.168.86.12:30003' : fmt === 'uberhfdl' ? 'e.g. 192.168.86.11:8080' : 'e.g. 192.168.86.12:30005'} placeholderTextColor={C.textDim} style={[input, { marginTop: 4 }]} />
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
            <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
              {!!test && (
                <TouchableOpacity onPress={() => void doTest(false)} disabled={busy} accessibilityRole="button" style={btn(false)}>
                  <Text style={btnText(false)}>TEST</Text>
                </TouchableOpacity>
              )}
              <TouchableOpacity onPress={() => void doTest(true)} disabled={busy} accessibilityRole="button" style={btn(true)}>
                <Text style={btnText(true)}>ADD</Text>
              </TouchableOpacity>
              {!!pending && (
                <TouchableOpacity onPress={() => { if (addOne(pending, setMsg)) { setAddr(''); setName(''); } setPending(null); }} accessibilityRole="button" style={btn(false)}>
                  <Text style={btnText(false)}>ADD ANYWAY</Text>
                </TouchableOpacity>
              )}
            </View>
            {!!msg && <Text style={hint}>{msg}</Text>}
            {!!test && <Text style={hint}>ADD tests the feed first. If the test fails you are told why, and may add it anyway (the other receiver may just be off).</Text>}
            <Text style={hint}>
              dumphfdl: add --output decoded:basestation:tcp:address=THIS-PHONE,port=32010 to its options (Docker: EXTRA_ARGS), then add
              a BaseStation feed here that listens on 32010, labelled HFDL. Keep the phone on Wi-Fi: a feed from another receiver
              only works on the same network.
            </Text>
          </>)}
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
