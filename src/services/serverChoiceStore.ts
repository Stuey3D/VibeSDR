/**
 * ★★★ beta96 the server entry's storage and the native calls it needs (Lite-safe: React Native + AsyncStorage only).
 * The rules are in ../utils/serverChoice.ts (pure, tested); this file only reads, writes and asks.
 */
import { DeviceEventEmitter, NativeModules, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { CHOICE_KEY, MDNS_KEY, parseChoice, simpleStartConfig, type ServerChoice } from '../utils/serverChoice';

const Local: any = (NativeModules as any).VibeLocalSDR;

export async function getChoice(): Promise<ServerChoice | null> {
  try { return parseChoice(await AsyncStorage.getItem(CHOICE_KEY)); } catch { return null; }
}
export async function setChoice(c: ServerChoice): Promise<void> {
  try { await AsyncStorage.setItem(CHOICE_KEY, c); } catch { /* the list shows again next time */ }
}
export async function getMdnsName(): Promise<string> {
  try { return (await AsyncStorage.getItem(MDNS_KEY)) ?? ''; } catch { return ''; }
}
export async function setMdnsName(v: string): Promise<void> {
  try { await AsyncStorage.setItem(MDNS_KEY, v); } catch { /* sent with Start anyway */ }
}

/** What the server's one settings store says (VibeRemoteSetup) — null where there is no server process API. */
export async function storeFacts(): Promise<{ hasAdminPassword: boolean; remote: boolean; simple: boolean; hasStore: boolean } | null> {
  if (Platform.OS !== 'android' || typeof Local?.serverSettingsGet !== 'function') return null;
  try {
    const j = JSON.parse(await Local.serverSettingsGet());
    const s = j?.settings ?? {};
    return { hasAdminPassword: !!j?.hasAdminPassword, remote: !!j?.remote, simple: s.simple === true,
             hasStore: !!s && Object.keys(s).length > 0 };
  } catch { return null; }
}

/** ★ Start a VibeServer Simple: only its four go to the server, which merges them into the store (VibeServerOps). */
export async function startSimpleServer(o: { name: string; mdnsName: string; rate: number; topRate: number; fps: number }) {
  const cfg = simpleStartConfig(o);
  return await Local.startVibeServer(cfg) as { ip: string; port: number; name: string };
}

// ── "Set up on another device" — first-time setup only ─────────────────────────────────────────────────────────────
export const setupCodeSupported = () => Platform.OS === 'android' && typeof Local?.setupCodeIssue === 'function';
export async function setupCodeIssue(): Promise<string> { return String(await Local.setupCodeIssue()); }
export async function setupCodeCancel(): Promise<void> { try { await Local?.setupCodeCancel?.(); } catch { /* nothing live */ } }
export type SetupCodeState = {
  active: boolean; secondsLeft: number; wrong: number; session: boolean; peer: string; ended: string; passwordSet: boolean;
};
export async function setupCodeState(): Promise<{ state: SetupCodeState; hasAdminPassword: boolean; remote: boolean } | null> {
  try {
    const j = JSON.parse(String(await Local.setupCodeState()));
    return { state: j.state ?? {}, hasAdminPassword: !!j.hasAdminPassword, remote: !!j.remote };
  } catch { return null; }
}

/** ★ The first admin password set from another device reaches the phone's own copy (vs_adminpw), so a Start from Full
 *  sends it. Installed once; harmless where nothing is ever emitted. */
let adoptInstalled = false;
export function adoptRemoteAdminPassword() {
  if (adoptInstalled) return;
  adoptInstalled = true;
  DeviceEventEmitter.addListener('VibeServerEvent', (e: any) => {
    if (!e || e.type !== 'adminPassword') return;
    try {
      const pw = String(JSON.parse(e.json)?.password ?? '');
      if (pw) void AsyncStorage.setItem('vs_adminpw', pw);
    } catch { /* not ours */ }
  });
}

/** The line every status screen shows once an admin password exists (Stuart, 2026-11-26). */
export function setupAddressLine(ip: string, port: number, mdnsHost: string): string {
  const a = ip && port ? `http://${ip}:${port}/setup` : '';
  const b = mdnsHost && port ? `${mdnsHost}.local:${port}/setup` : '';
  return 'Change settings from another device: ' + [a, b].filter(Boolean).join(' or ')
    + ' — sign in with your admin password.';
}

/* ★★★ beta96 CHANGE ADMIN PASSWORD / PIN ON THE DEVICE (DeviceCredentials) — no old password: the device is the proof.
 *  Each goes to all three places it lives: the screen's own copy (AsyncStorage), the server's one settings store (which
 *  the restore and the /setup page use — settings.put merges), and the running engine, live. */
export async function saveAdminPasswordOnDevice(pw: string): Promise<void> {
  await AsyncStorage.setItem('vs_adminpw', pw);
  try { await Local?.serverSettingsPut?.(JSON.stringify({ adminPassword: pw }), false); } catch { /* the next Start sends it */ }
  try { Local?.setVibeServerAdminSecret?.(pw); } catch { /* not running */ }
}
export async function savePinOnDevice(pin: string): Promise<void> {
  await AsyncStorage.multiSet([['vs_pin', pin], ['vs_pinmode', pin ? 'custom' : 'off']]);
  try { await Local?.serverSettingsPut?.(JSON.stringify({ pin }), false); } catch { /* the next Start sends it */ }
  try { Local?.setVibeServerPin?.(pin); } catch { /* not running */ }
}
