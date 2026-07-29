import AsyncStorage from '@react-native-async-storage/async-storage';

// A single, global "name or callsign" identity for KiwiSDR-protocol connections — saved and
// remembered like the VibeServer PIN. Some Kiwi owners enable "Require name/callsign entry to
// connect" (admin control, v1.666+), sent on the wire as `SET ident_user=<name>`; and separately,
// anonymous non-browser connections are a common blacklist target — so NOT sending an ident is
// itself a cause of refusals. We capture it once, remember it, and send it on every Kiwi connect.
//
// Global, not per-server: your callsign is the same everywhere. Editable later (see IdentModal)
// in case it ever needs changing.

const KEY = 'vibe.kiwi.ident';

// Kept comfortably under the 16-char minimum an owner can cap the field at, and stripped of
// characters that could break the space-delimited `SET ident_user=` line. Any non-blank string
// is accepted by Kiwi (a licensed callsign is not required).
export function sanitizeIdent(raw: string): string {
  return raw.replace(/[^A-Za-z0-9/_-]/g, '').slice(0, 16);
}

export async function getKiwiIdent(): Promise<string> {
  try { return (await AsyncStorage.getItem(KEY)) ?? ''; } catch { return ''; }
}

export async function setKiwiIdent(value: string): Promise<void> {
  try { await AsyncStorage.setItem(KEY, sanitizeIdent(value)); } catch { /* best effort */ }
}
