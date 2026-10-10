/**
 * ★★★ PORTABLE WI-FI — Network | Own Wi-Fi (Stuart, 2026-10-10). The words the app's menu and the web page's menu both
 * use, so the two cannot drift. The server decides WHETHER the link is weak (vibepocket::linkAdvice, one rule); this
 * only says it. Pure — tested by scripts/test_pocket_connection.ts.
 *
 * Shown only on a pocket VibeServer reached directly on its own network: the server answers local:false through the
 * tunnel or from a public address, and refuses the switch there too.
 */
export interface PocketLink { signal: number | null; tx: number | null; rx: number | null; retry: number | null }
export interface PocketConn {
  pocket?: boolean; local?: boolean; mode?: string; hold?: boolean; apSet?: boolean;
  ssid?: string; apSsid?: string; link?: PocketLink | null; advice?: string;
}

/** Draw the choice at all? Only on a pocket box, on its own network. */
export const pocketHere = (c: PocketConn | null | undefined): boolean => !!(c && c.pocket && c.local);

/** Which side is lit. */
export const pocketOnOwn = (c: PocketConn): boolean => c.mode === 'fallback-ap' || c.mode === 'setup-ap';

/** The one line beside the choice: where the box is, and how good the link is. */
export function pocketLinkLine(c: PocketConn): string {
  if (pocketOnOwn(c)) return `On its own Wi-Fi${c.apSsid ? ` “${c.apSsid}”` : ''}${c.hold ? ' — held while the radio is in use' : ''}`;
  if (c.mode === 'connecting') return 'Joining a network…';
  if (c.mode !== 'client') return '';
  const l = c.link;
  const bits = [`On “${c.ssid || '?'}”`];
  if (l?.signal != null) bits.push(`${String(l.signal).replace('-', '−')} dBm`);
  if (l?.tx != null) bits.push(`${l.tx} Mbit/s`);
  if (l?.retry != null) bits.push(`${l.retry} % retries`);
  return bits.join(' · ');
}

export const POCKET_WEAK_TITLE = 'Weak Wi-Fi on this VibeServer Portable';
export const POCKET_WEAK_BODY = 'Switch it to its own Wi-Fi to avoid drop-outs.';
/** "Not now" quietens the notice for this long. */
export const POCKET_WEAK_SNOOZE_MS = 30 * 60 * 1000;

/** The confirmation before either switch — Stuart: "when switching you will need to exit this session and connect to the
 *  new captive hotspot then start again". */
export function pocketSwitchText(c: PocketConn, to: 'own' | 'network'): { title: string; body: string } {
  if (to === 'own') {
    const ap = c.apSsid ? `“${c.apSsid}”` : 'its own Wi-Fi';
    return {
      title: 'Switch to its own Wi-Fi?',
      body: `This VibeServer Portable will leave${c.ssid ? ` “${c.ssid}”` : ' your network'} and start ${ap}. `
          + `You will need to leave this session, join ${ap} on this device, and start again.\n\n`
          + 'It stays on its own Wi-Fi while the radio is in use, and goes back to your network after 30 minutes unused '
          + 'or when it is switched off and on.',
    };
  }
  return {
    title: 'Switch back to your network?',
    body: 'This VibeServer Portable will leave its own Wi-Fi and join the first of your saved networks in range. '
        + 'You will need to leave this session, join that network on this device, and start again.',
  };
}
