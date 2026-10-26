/**
 * ★ V12 SCANNER PRESETS (brief §10.1) — AsyncStorage, the userBookmarks pattern: one versioned JSON list, load/save
 * helpers, GLOBAL across receivers (like global bookmarks) and validated against the receiver at START (scanner.ts
 * validateCandidates) — a "160 m Talkgroups" preset follows the listener between UberSDRs and refuses cleanly on an
 * airband-only OpenWebRX profile. The bundled starters are never stored (serialisePresets drops them).
 * ★ The session tag list and found list are NEVER written here (§10.2) — they live in the screen's controller ref.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { parsePresets, serialisePresets, type ScanPreset } from './scanner';

const KEY = 'lsv_scan_presets';

export async function loadScanPresets(): Promise<ScanPreset[]> {
  try { return parsePresets(await AsyncStorage.getItem(KEY)); } catch { return []; }
}

export async function saveScanPresets(list: ScanPreset[]): Promise<void> {
  try { await AsyncStorage.setItem(KEY, serialisePresets(list)); } catch { /* ★ a failed save keeps the in-memory list */ }
}
