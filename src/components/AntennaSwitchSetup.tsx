/**
 * AntennaSwitchSetup — Stuart's flow for an external antenna switch, on the server screen (Lite and the app's server mode),
 * above the existing antenna details (2026-10-09, docs/v12/ANTENNA-SWITCH-ROTATOR.md §3a):
 *
 *   Set up an antenna switch?  [Yes] [No]
 *   [Search for switch…]  →  "Switch found on 192.168.86.77:1883 — Generic Antenna Switch Example (2 relays)"
 *   How many antennas are connected?  [−] 2 [+]
 *   Antenna 1  (Name) (which relay) (bands it is chosen for)
 *   Antenna 2  …
 *   [ ] Lock antenna controls behind the admin password
 *
 * ★ Everything is chosen from lists the search produced — a TV remote cannot comfortably type MQTT topics. A broker that
 *   the search cannot see (another subnet, a login) can be typed, and searched directly.
 * ★ The values travel as AntennaSwitchCfg (services/antennaSwitch.ts); the screen persists them and sends serverFields().
 */
import React, { useState } from 'react';
import { ActivityIndicator, NativeModules, Text, TextInput, TouchableOpacity, View } from 'react-native';
import {
  type AntennaSwitchCfg, type FoundSwitch, setAntennaCount, splitBroker,
} from '../services/antennaSwitch';

type Colours = { textDim: string; amber: string; border: string; green: string; gold: string };

export default function AntennaSwitchSetup({ value, onChange, C, F }: {
  value: AntennaSwitchCfg; onChange: (next: AntennaSwitchCfg) => void; C: Colours; F: string;
}) {
  const [busy, setBusy] = useState(false);
  const [found, setFound] = useState<FoundSwitch[] | null>(null);
  const [why, setWhy] = useState('');
  const [typed, setTyped] = useState(value.broker);

  const key = (label: string, on: boolean, onPress: () => void, extra?: object) => (
    <TouchableOpacity key={label} onPress={onPress} accessibilityRole="button"
      style={[{ borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 8,
                borderColor: on ? C.green : C.border, backgroundColor: on ? C.green + '18' : 'transparent' }, extra]}>
      <Text style={{ color: on ? C.green : C.gold, fontFamily: F, fontSize: 12 }}>{label}</Text>
    </TouchableOpacity>
  );
  const hint = (t: string) => <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11, marginTop: 6, lineHeight: 15 }}>{t}</Text>;
  const input = (v: string, set: (s: string) => void, ph: string, extra?: object) => (
    <TextInput value={v} onChangeText={set} placeholder={ph} placeholderTextColor={C.textDim}
      autoCapitalize="none" autoCorrect={false}
      style={[{ borderWidth: 1, borderRadius: 8, borderColor: C.border, color: C.amber, fontFamily: F,
                paddingHorizontal: 10, paddingVertical: 8, marginTop: 6 }, extra]} />
  );

  const search = async (host: string, port: number) => {
    const mod = (NativeModules as any).VibeLocalSdrModule;
    if (!mod?.antSwitchSearch) { setWhy('This build cannot search for a switch.'); return; }
    setBusy(true); setWhy(''); setFound(null);
    try {
      const j = JSON.parse(await mod.antSwitchSearch(host, port));
      const devs: FoundSwitch[] = Array.isArray(j.devices) ? j.devices : [];
      setFound(devs);
      if (!devs.length) setWhy(j.brokers?.length
        ? `A broker answered (${j.brokers.join(', ')}) but no switch announced itself on it. Tasmota needs "SetOption19 0" (its own discovery) on; Home-Assistant style switches need discovery on.`
        : 'No MQTT broker answered on this network (port 1883). If yours is elsewhere, type its address below.');
    } catch (e: any) {
      setWhy(`The search failed: ${e?.message ?? e}`);
    } finally { setBusy(false); }
  };

  const choose = (d: FoundSwitch) => {
    setTyped(d.broker);
    onChange(setAntennaCount({ ...value, broker: d.broker, deviceName: d.name, relays: d.relays, antennas: [] },
                             Math.min(Math.max(2, d.relays.length), 2)));
  };

  return (
    <View style={{ marginTop: 6, marginBottom: 10 }}>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>Set up an antenna switch?</Text>
      <View style={{ flexDirection: 'row', gap: 8, marginTop: 6 }}>
        {key('Yes', value.enabled, () => onChange({ ...value, enabled: true }))}
        {key('No', !value.enabled, () => onChange({ ...value, enabled: false }))}
      </View>
      {!value.enabled ? hint('For a relay box that switches between aerials over your network (MQTT — Tasmota, ESP32 and Home-Assistant-style switches). Listeners then get an Antenna choice.') : (<>
        <View style={{ flexDirection: 'row', gap: 8, marginTop: 10, alignItems: 'center' }}>
          {key(busy ? 'Searching…' : 'Search for switch…', false, () => { if (!busy) { const b = typed.trim(); if (b) { const { host, port } = splitBroker(b); void search(host, port); } else void search('', 1883); } })}
          {busy && <ActivityIndicator color={C.gold} />}
        </View>
        {input(typed, (t) => { setTyped(t); onChange({ ...value, broker: t }); }, 'Broker (blank = search this network), e.g. 192.168.86.77:1883')}
        <View style={{ flexDirection: 'row', gap: 8 }}>
          {input(value.user, (t) => onChange({ ...value, user: t }), 'Broker username (if any)', { flex: 1 })}
          {input(value.pass, (t) => onChange({ ...value, pass: t }), 'Password', { flex: 1 })}
        </View>
        {!!why && hint(why)}
        {found?.map((d) => (
          <TouchableOpacity key={d.id + d.broker} onPress={() => choose(d)} accessibilityRole="button"
            style={{ borderWidth: 1, borderRadius: 8, padding: 10, marginTop: 8,
                     borderColor: value.deviceName === d.name && value.broker === d.broker ? C.green : C.border }}>
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 13 }}>Switch found on {d.broker} — {d.name}</Text>
            <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11, marginTop: 2 }}>
              {d.relays.length} relay{d.relays.length === 1 ? '' : 's'} · {d.kind === 'tasmota' ? 'Tasmota' : 'Home-Assistant discovery'}
            </Text>
          </TouchableOpacity>
        ))}

        {value.relays.length > 0 && (<>
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, marginTop: 14 }}>
            {value.deviceName ? `${value.deviceName} — ` : ''}How many antennas are connected?
          </Text>
          <View style={{ flexDirection: 'row', gap: 8, marginTop: 6, alignItems: 'center' }}>
            {key('−', false, () => onChange(setAntennaCount(value, value.antennas.length - 1)))}
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 16, minWidth: 24, textAlign: 'center' }}>{value.antennas.length}</Text>
            {key('+', false, () => onChange(setAntennaCount(value, value.antennas.length + 1)))}
          </View>
          {value.antennas.map((a, i) => (
            <View key={i} style={{ borderWidth: 1, borderColor: C.border, borderRadius: 8, padding: 10, marginTop: 8 }}>
              <Text style={{ color: C.gold, fontFamily: F, fontSize: 12 }}>Antenna {i + 1}</Text>
              {input(a.name, (t) => { const an = value.antennas.slice(); an[i] = { ...a, name: t.slice(0, 40) }; onChange({ ...value, antennas: an }); },
                     'Name, e.g. VHF Vertical')}
              <View style={{ flexDirection: 'row', gap: 6, flexWrap: 'wrap', marginTop: 6 }}>
                {value.relays.map((r, k) => key(r.label || `Relay ${k + 1}`, a.relay === k,
                  () => { const an = value.antennas.slice(); an[i] = { ...a, relay: k }; onChange({ ...value, antennas: an }); }))}
              </View>
              {input(a.bands, (t) => { const an = value.antennas.slice(); an[i] = { ...a, bands: t.slice(0, 120) }; onChange({ ...value, antennas: an }); },
                     'Chosen automatically on (optional), e.g. 0-30MHz')}
            </View>
          ))}
          {hint('Names are what listeners see. "Chosen automatically on" switches to that antenna when somebody tunes into those frequencies — e.g. 0-30MHz, or 30MHz+.')}
          <View style={{ flexDirection: 'row', gap: 8, marginTop: 10 }}>
            {key(value.locked ? '✓ Antenna locked to the admin' : 'Lock antenna controls behind the admin password', value.locked,
                 () => onChange({ ...value, locked: !value.locked }))}
          </View>
          {value.locked && hint('Listeners see which antenna is in use but cannot change it; the per-band choices above still switch it.')}
        </>)}
      </>)}
    </View>
  );
}
