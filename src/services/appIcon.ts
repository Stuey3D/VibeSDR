/**
 * appIcon — the ICON & ART colour (faceplate `iconColour`) to the native side. Stuart, 2026-10-04.
 *
 * ★ iOS and Android (Android's half 2026-10-05: launcher <activity-alias>es + the art, VibeAppIcon.kt /
 *   VibeStreamService). Every call is optional-chained, so an older binary, web and tests simply do nothing.
 * ★ The two systems change the icon differently: iOS at once, with its own alert; Android when the app next
 *   leaves the screen (switching the launcher alias on screen can close the app). MenuSheet's note says which.
 */
import { Platform } from 'react-native';
import { VibePowerModule } from '../components/AudioPlayer';
import type { IconColour } from '../constants/faceplate';

const mod = VibePowerModule as any;
const native = Platform.OS === 'ios' || Platform.OS === 'android';

/** The Now Playing art base in this colour — live, no prompt. */
export function setArtColour(c: IconColour): void {
  if (!native) return;
  try { mod?.setArtColour?.(c); } catch { /* an old binary: the art stays green */ }
}

/** Can this device change its app icon? (The system's answer; false where there is no native support.) */
export async function appIconSupported(): Promise<boolean> {
  if (!native || !mod?.appIconSupported) return false;
  try { return !!(await mod.appIconSupported()); } catch { return false; }
}

/** Change the app icon. iOS confirms with its own alert; Android applies it when the app next leaves the screen.
 *  Resolves false (and leaves the icon) on failure. */
export async function setAppIcon(c: IconColour): Promise<boolean> {
  if (!native || !mod?.setAppIcon) return false;
  try { await mod.setAppIcon(c); return true; } catch { return false; }
}
