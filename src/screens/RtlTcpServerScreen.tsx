import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  View, Text, TextInput, TouchableOpacity, ScrollView, ActivityIndicator, StyleSheet, Alert,
  PermissionsAndroid, Platform, Switch,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { themeFor } from '../constants/theme';
import {
  startRtlTcpServer, stopRtlTcpServer, setServerSampleRate, getServerStatus,
  getServerName, saveServerName, formatBytes, BANDWIDTH_OPTIONS,
  getServerPersist, saveServerPersist,
  type ServerInfo, type ServerStatus,
} from '../services/rtlTcpServer';
import { advertiseRtlTcp, stopAdvertiseRtlTcp } from '../services/mdns';

type Props = NativeStackScreenProps<RootStackParamList, 'RtlTcpServer'>;

// The RTL-TCP server control screen.
//
// Ad-hoc mode (default): owns the full lifecycle — starts the server + mDNS
// advert on mount, tears both down on unmount, so leaving frees the dongle.
//
// Persist mode ("keep sharing"): the SERVER owns its own lifecycle, held up by
// the foreground service. The screen starts it if it isn't running, ADOPTS it if
// it is, and never stops it on unmount — only the explicit STOP button does.
// This is what a set-and-forget node needs (phone + dongle at an antenna base).
export default function RtlTcpServerScreen({ navigation, route }: Props) {
  const { colors: C, font: F } = themeFor();
  // The Server-mode picker owns the shared "auto-discovery" toggle; honour it
  // here (default on for a direct entry / legacy callers).
  const doAdvertise = route.params?.advertise !== false;
  const adv = useCallback((n: string, p: number) => { if (doAdvertise) advertiseRtlTcp(n, p); }, [doAdvertise]);
  const [name, setName]       = useState(route.params?.name ?? 'VibeSDR RTL-SDR');
  const [info, setInfo]       = useState<ServerInfo | null>(null);
  const [status, setStatus]   = useState<ServerStatus | null>(null);
  const [override, setOverride] = useState(0);
  const [starting, setStarting] = useState(true);
  const [error, setError]     = useState<string | null>(null);
  const [dropping, setDropping] = useState(false);   // drops grew since the last poll
  const [persist, setPersist] = useState(false);     // keep sharing after leaving
  const startedRef = useRef(false);
  const lastDropRef = useRef(0);
  // The unmount cleanup closes over the first render, so it can't read `persist`
  // state. Mirror it into a ref that the cleanup can see.
  const persistRef = useRef(false);

  // Start the server (once) + advertise — or ADOPT one already running (persist
  // mode left it up when the screen was last closed). Starting a second server on
  // the same dongle would fail on the USB open, so adopt must come first.
  useEffect(() => {
    if (startedRef.current) return;
    startedRef.current = true;
    let cancelled = false;
    (async () => {
      const persisted = await getServerPersist();
      if (cancelled) return;
      setPersist(persisted);
      persistRef.current = persisted;

      const existing = await getServerStatus();
      if (cancelled) return;
      if (existing?.running && existing.port > 0) {
        const adoptedName = await getServerName(route.params?.name ?? 'VibeSDR RTL-SDR');
        if (cancelled) return;
        setName(adoptedName);
        setInfo({ ip: existing.ip || '0.0.0.0', port: existing.port, name: adoptedName });
        setOverride(existing.overrideRate ?? 0);
        setStatus(existing);
        lastDropRef.current = existing.droppedBytes ?? 0;
        setStarting(false);
        adv(adoptedName, existing.port);   // idempotent; IP may have changed
        return;
      }

      const initialName = await getServerName(route.params?.name ?? 'VibeSDR RTL-SDR');
      if (cancelled) return;
      setName(initialName);
      // Android 13+: the FGS notification is suppressed without POST_NOTIFICATIONS.
      if (Platform.OS === 'android' && Platform.Version >= 33) {
        try { await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS); } catch {}
      }
      if (cancelled) return;
      try {
        const res = await startRtlTcpServer({ name: initialName });
        if (cancelled) return;
        setInfo(res);
        setStarting(false);
        adv(res.name, res.port);
      } catch (e: any) {
        if (cancelled) return;
        setStarting(false);
        // ★ An Airspy HF+ shares over rtl_tcp too — do not name the one radio this used to support.
        setError(e?.message ?? 'Could not start the server. Is a radio plugged in via USB OTG?');
      }
    })();
    return () => { cancelled = true; };
  }, []);

  // Teardown on unmount (any way of leaving the screen) — UNLESS persist mode is
  // on, in which case the server outlives the screen and only the explicit STOP
  // button ends it. Read the ref, not the state: this cleanup runs once, closed
  // over the first render's values.
  useEffect(() => () => {
    if (persistRef.current) return;   // keep sharing; FGS holds it up
    stopAdvertiseRtlTcp();
    stopRtlTcpServer();
  }, []);

  // Poll status for the live readout (client connected / actual bandwidth).
  useEffect(() => {
    if (starting || error) return;
    const t = setInterval(async () => {
      const s = await getServerStatus();
      if (!s) return;
      const dropped = s.droppedBytes ?? 0;
      setDropping(dropped > lastDropRef.current);
      lastDropRef.current = dropped;
      setStatus(s);
    }, 2000);
    return () => clearInterval(t);
  }, [starting, error]);

  const applyPersist = useCallback((on: boolean) => {
    setPersist(on);
    persistRef.current = on;
    saveServerPersist(on);
  }, []);

  const applyOverride = useCallback((value: number) => {
    setOverride(value);
    setServerSampleRate(value);
  }, []);

  const commitName = useCallback(() => {
    const n = name.trim() || 'VibeSDR RTL-SDR';
    setName(n);
    saveServerName(n);
    if (info) adv(n, info.port);
  }, [name, info]);

  const stopAndBack = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopRtlTcpServer();
    navigation.goBack();
  }, [navigation]);

  const clientLine = status?.client
    ? (status.clientAddr ? `Connected: ${status.clientAddr}` : 'Client connected')
    : 'Waiting for a client to connect…';
  const bwActual = status?.sampleRate
    ? `${(status.sampleRate / 1e6).toFixed(3).replace(/\.?0+$/, '')} MHz`
    : '—';

  const dropped = status?.droppedBytes ?? 0;
  const linkLine = !status?.client ? '—'
    : dropped === 0 ? 'Good'
    : `${formatBytes(dropped)} dropped`;
  const linkColor = !status?.client ? C.goldDim
    : dropping ? C.red
    : dropped > 0 ? C.amber
    : C.green;

  return (
    <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>RTL-TCP Server</Text>
        <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
          {persist
            ? "Sharing this phone's RTL-SDR over your network. Sharing continues in the background until you press STOP."
            : "Sharing this phone's RTL-SDR over your network. Leaving this screen stops the server and frees the dongle for use on this device."}
        </Text>

        {starting && (
          <View style={styles.center}>
            <ActivityIndicator color={C.amber} />
            <Text style={{ color: C.textDim, fontFamily: F, marginTop: 10 }}>Starting server…</Text>
          </View>
        )}

        {error && (
          <View style={[styles.card, { borderColor: C.red }]}>
            <Text style={{ color: C.red, fontFamily: F, fontSize: 14 }}>{error}</Text>
          </View>
        )}

        {info && !error && (
          <>
            {/* Live status */}
            <View style={[styles.card, { borderColor: C.borderBright }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>ADDRESS</Text>
                <Text style={[styles.value, { color: C.amber, fontFamily: F }]}>{info.ip}:{info.port}</Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>BANDWIDTH</Text>
                <Text style={[styles.value, { color: C.amber, fontFamily: F }]}>
                  {bwActual}{override > 0 ? ' (capped)' : ''}
                </Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>STATUS</Text>
                <Text style={[styles.value, { color: status?.client ? C.green : C.goldDim, fontFamily: F }]}>
                  {clientLine}
                </Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>LINK</Text>
                <Text style={[styles.value, { color: linkColor, fontFamily: F }]}>{linkLine}</Text>
              </View>
              {dropping && (
                <Text style={[styles.hint, { color: C.red, fontFamily: F, marginTop: 6 }]}>
                  ⚠ The network can't keep up — try a lower bandwidth setting, or move
                  closer to the router.
                </Text>
              )}
            </View>

            {/* Advertised name */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>ADVERTISED NAME</Text>
            <TextInput
              value={name}
              onChangeText={setName}
              onBlur={commitName}
              onSubmitEditing={commitName}
              placeholder="VibeSDR RTL-SDR"
              placeholderTextColor={C.goldDim}
              style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]}
            />
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              Shown to clients discovering this server on the network.
            </Text>

            {/* Persist ("keep sharing") — off = ad-hoc share that ends with this screen. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>KEEP SHARING</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Keep the server running
                </Text>
                <Switch
                  value={persist}
                  onValueChange={applyPersist}
                  trackColor={{ false: C.border, true: C.green }}
                  thumbColor={C.amber}
                />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {persist
                  ? 'Sharing continues when you leave this screen. The dongle stays busy, so this device can’t receive on it. Use STOP to end sharing.'
                  : 'Sharing stops when you leave this screen, freeing the dongle for use on this device.'}
              </Text>
            </View>

            {/* Bandwidth override */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>BANDWIDTH OVERRIDE</Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginBottom: 8 }]}>
              Default lets the client choose. Force a lower rate here if the connection struggles.
            </Text>
            {BANDWIDTH_OPTIONS.map(opt => {
              const active = override === opt.value;
              return (
                <TouchableOpacity key={opt.value}
                  style={[styles.optRow, { borderColor: active ? C.amber : C.border }]}
                  onPress={() => applyOverride(opt.value)}>
                  <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 15 }}>
                    {opt.label}
                  </Text>
                  {active && <Text style={{ color: C.amber, fontFamily: F }}>✓</Text>}
                </TouchableOpacity>
              );
            })}
          </>
        )}

        {/* Stop + back */}
        <TouchableOpacity style={[styles.stopBtn, { borderColor: C.red }]} onPress={stopAndBack}>
          <Text style={{ color: C.red, fontFamily: F, fontSize: 16 }}>
            {error ? '‹ Back to servers' : '■ Stop server & back to servers'}
          </Text>
        </TouchableOpacity>
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 13, lineHeight: 18, marginBottom: 16 },
  center: { alignItems: 'center', paddingVertical: 40 },
  card: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 16, gap: 10 },
  rowBetween: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  label: { fontSize: 11, letterSpacing: 1 },
  value: { fontSize: 15, flexShrink: 1, textAlign: 'right', marginLeft: 8 },
  section: { fontSize: 11, letterSpacing: 1, marginTop: 4, marginBottom: 6 },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16 },
  hint: { fontSize: 11.5, lineHeight: 16, marginTop: 6 },
  optRow: {
    borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 12,
    marginBottom: 8, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  stopBtn: {
    borderWidth: 1, borderRadius: 10, paddingVertical: 14, alignItems: 'center', marginTop: 20,
  },
});
