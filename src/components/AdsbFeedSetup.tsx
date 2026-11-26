/**
 * AdsbFeedSetup — V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS on the Android server screen (docs/v12/ADSB-FEED.md). The
 * Setup page's card (vibe_setup_page.h #adsbFeedCard), the same words, for the phone's Server mode and VibeServer Lite:
 * a local Beast feed for feeder programs on the network (FlightAware's PiAware with receiver-type other), and pushes to
 * flight-tracking sites. Stored as ONE JSON string, exactly RadioConfig::adsbFeed (src/utils/adsbFeed.ts). OFF by default.
 * ★ Drawn only where the radio could run ADS-B (adsbFeedOffered) — AGENTS.md: no control that only works on another radio.
 */
import React from 'react';
import { Switch, Text, TextInput, TouchableOpacity, View } from 'react-native';
import { newUuid, type AdsbFeedModel, type AdsbFeedRow } from '../utils/adsbFeed';

type Ink = { text: string; textDim: string; border: string; trackOff: string; trackOn: string; thumb: string; accent?: string; warn?: string };

export default function AdsbFeedSetup({ C, F, model, onChange, status, error, keepRunning }: {
  C: Ink; F?: string; model: AdsbFeedModel; onChange: (m: AdsbFeedModel) => void;
  status: string; error: string; keepRunning: boolean;
}) {
  const set = (p: Partial<AdsbFeedModel>) => onChange({ ...model, ...p, wasSet: true });
  const setRow = (i: number, p: Partial<AdsbFeedRow>) => set({ rows: model.rows.map((r, k) => (k === i ? { ...r, ...p } : r)) });
  const hint = { color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 6 } as const;
  const sw = (label: string, value: boolean, on: (v: boolean) => void) => (
    <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 10 }}>
      <Text style={{ color: C.text, fontFamily: F, fontSize: 14, flex: 1, paddingRight: 12 }}>{label}</Text>
      <Switch value={value} onValueChange={on} trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb} accessibilityLabel={label} />
    </View>
  );
  const input = { color: C.text, borderColor: C.border, borderWidth: 1, borderRadius: 6, paddingHorizontal: 8, paddingVertical: 6, fontFamily: F, fontSize: 13 } as const;
  const key = (label: string, press: () => void, k: string) => (
    <TouchableOpacity key={k} onPress={press} accessibilityRole="button"
                      style={{ borderWidth: 1, borderColor: C.border, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginLeft: 6 }}>
      <Text style={{ color: C.text, fontFamily: F, fontSize: 13 }}>{label}</Text>
    </TouchableOpacity>
  );
  return (
    <View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
        Send the aircraft this radio hears to flight-tracking sites, or let a feeder program on your network read them
        (FlightAware{'’'}s PiAware, for one).
      </Text>
      {!keepRunning && (
        <Text style={hint}>
          Only while ADS-B is running on this radio. To feed all day, use ADS-B as the landing and switch on Keep ADS-B running
          when nobody is connected (WHERE LISTENERS START).
        </Text>
      )}
      {sw('Serve a Beast feed for feeder programs on your network', model.serve, (v) => set({ serve: v }))}
      {model.serve && (
        <View style={{ marginLeft: 12 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 8 }}>
            <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginRight: 8 }}>PORT</Text>
            <TextInput value={String(model.port)} keyboardType="number-pad" maxLength={5}
                       onChangeText={(t) => set({ port: Number(t.replace(/[^0-9]/g, '')) || 0 })} style={[input, { width: 90 }]} />
          </View>
          <Text style={hint}>30005 is the usual Beast port.</Text>
          {sw('Allow any address, not only this network', model.open, (v) => set({ open: v }))}
          <Text style={hint}>
            Off: only this phone and your local network can connect. On: anyone who can reach this port can read it {'—'} only
            for a feeder elsewhere that you run yourself.
          </Text>
          <Text style={hint}>
            FlightAware: install PiAware from FlightAware{'’'}s own instructions on a computer on this network, then
            {' '}sudo piaware-config receiver-type other, sudo piaware-config receiver-host {'<this phone’s address>'},
            {' '}sudo piaware-config receiver-port {model.port || 30005}, restart piaware and claim the feeder on flightaware.com.
            Sites with their own feed program (ADS-B Exchange{'’'}s, for its MLAT and stats page) can read this port the same way.
          </Text>
        </View>
      )}
      {sw('Send to flight-tracking sites', model.push, (v) => set({ push: v }))}
      {model.push && (
        <View style={{ marginLeft: 12 }}>
          {model.rows.map((r, i) => (
            <View key={`r${i}`} style={{ marginTop: 8 }}>
              <View style={{ flexDirection: 'row', alignItems: 'center' }}>
                <Switch value={r.on}
                        onValueChange={(v) => setRow(i, { on: v, uuid: v && !r.uuid && r.preset?.uuid === 'auto' ? newUuid() : r.uuid })}
                        trackColor={{ false: C.trackOff, true: C.trackOn }} thumbColor={C.thumb}
                        accessibilityLabel={r.preset ? r.preset.name : `Send to ${r.host || 'this site'}`} />
                {r.preset ? (
                  <Text style={{ color: C.text, fontFamily: F, fontSize: 14, marginLeft: 8, flex: 1 }}>
                    {r.preset.name} <Text style={{ color: C.textDim, fontSize: 12 }}>{`${r.host}:${r.port}`}</Text>
                  </Text>
                ) : (
                  <View style={{ flexDirection: 'row', alignItems: 'center', flex: 1, marginLeft: 8 }}>
                    <TextInput value={r.host} placeholder="host.example.org" autoCapitalize="none" autoCorrect={false}
                               onChangeText={(t) => setRow(i, { host: t.trim() })} style={[input, { flex: 1 }]} />
                    <TextInput value={String(r.port)} keyboardType="number-pad" maxLength={5}
                               onChangeText={(t) => setRow(i, { port: Number(t.replace(/[^0-9]/g, '')) || 0 })} style={[input, { width: 70, marginLeft: 6 }]} />
                    {key('Remove', () => set({ rows: model.rows.filter((_, k) => k !== i) }), `d${i}`)}
                  </View>
                )}
              </View>
              {r.on && (
                <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 4, marginLeft: 8 }}>
                  <TextInput value={r.uuid} maxLength={36} autoCapitalize="none" autoCorrect={false}
                             placeholder={r.preset?.uuid === 'paste' ? 'station UUID from your map.flights station page' : 'station UUID (optional)'}
                             onChangeText={(t) => setRow(i, { uuid: t.trim() })} style={[input, { flex: 1 }]} />
                  {r.preset?.uuid !== 'paste' && key('New', () => setRow(i, { uuid: newUuid() }), `u${i}`)}
                </View>
              )}
            </View>
          ))}
          {model.rows.length < 12 && (
            <TouchableOpacity onPress={() => set({ rows: [...model.rows, { preset: null, on: true, host: '', port: 30004, uuid: '' }] })}
                              accessibilityRole="button" style={{ marginTop: 8 }}>
              <Text style={{ color: C.accent || C.trackOn, fontFamily: F, fontSize: 13 }}>+ Add another site (host and port)</Text>
            </TouchableOpacity>
          )}
          <Text style={hint}>
            Each site gets a stream with repeats thinned to two a second per aircraft and message kind, as their own feed programs
            send. A station UUID identifies your feed to the site. VibeServer makes one for you where the site{'’'}s own program
            would; for map.flights paste the one from your station page there. ADS-B Exchange: your aircraft reach their map this
            way, but their MLAT and their feeder stats page need their own feed program (it can read the Beast port above).
          </Text>
          {/* ★ beta96 Stuart 11-26: "push on mobile data is fine but maybe on android warn about data use" */}
          <Text style={hint}>
            On mobile data: each site you send to uses a few KB/s while ADS-B is running {'—'} fine on most plans. Over Wi-Fi it
            uses none of your mobile allowance.
          </Text>
        </View>
      )}
      {!!error && <Text style={[hint, { color: C.warn || C.text }]}>{error}</Text>}
      {!!status && <Text style={[hint, { color: C.accent || C.text }]}>{status}</Text>}
      <Text style={hint}>
        What is sent: the replies aircraft broadcast (identity, position, altitude, speed, squawk and the rest) as this radio
        decoded them, with the signal level and a timestamp from the radio{'’'}s own sample clock. Nothing about your listeners.
        VibeServer never sends this receiver{'’'}s position: a site knows where you are only if you enter it in your account there.
        MLAT (multilateration) is not supported {'—'} leave a site{'’'}s MLAT setting off.
      </Text>
    </View>
  );
}
