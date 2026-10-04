/**
 * appIcon — the ICON & ART colour (faceplate `iconColour`) to the native side. Stuart, 2026-10-04.
 *
 * ★ iOS only for now (the first test build); Android's launcher aliases and art follow later. Every call is
 *   optional-chained, so an older binary, Android today, web and tests simply do nothing.
 */
import { Platform } from 'react-native';
import { VibePowerModule } from '../components/AudioPlayer';
import type { IconColour } from '../constants/faceplate';

const mod = VibePowerModule as any;

/** The Now Playing art base in this colour — live, no prompt. */
export function setArtColour(c: IconColour): void {
  if (Platform.OS !== 'ios') return;
  try { mod?.setArtColour?.(c); } catch { /* an old binary: the art stays green */ }
}

/** Can this device change its app icon? (The system's answer; false where there is no native support.) */
export async function appIconSupported(): Promise<boolean> {
  if (Platform.OS !== 'ios' || !mod?.appIconSupported) return false;
  try { return !!(await mod.appIconSupported()); } catch { return false; }
}

/** Change the app icon. iOS confirms with its own alert. Resolves false (and leaves the icon) on failure. */
export async function setAppIcon(c: IconColour): Promise<boolean> {
  if (Platform.OS !== 'ios' || !mod?.setAppIcon) return false;
  try { await mod.setAppIcon(c); return true; } catch { return false; }
}
