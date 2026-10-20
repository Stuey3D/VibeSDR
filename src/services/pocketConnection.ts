/**
 * ★★★ PORTABLE WI-FI — each saved network BY NAME | Own Wi-Fi (Stuart, 2026-10-10; by name 2026-10-11: "Stuey3D's
 * iPhone | Meow! | Own WiFi"). The words the app's menu and the web page's menu both
 * use, so the two cannot drift. The server decides WHETHER the link is weak (vibepocket::linkAdvice, one rule); this
 * only says it. Pure — tested by scripts/test_pocket_connection.ts.
 *
 * Shown only on a pocket VibeServer reached directly on its own network: the server answers local:false through the
 * tunnel or from a public address, and refuses the switch there too.
 */
export interface PocketLink {
  signal: number | null; tx: number | null; rx: number | null; retry: number | null;
  /** ★ 11.0.9: what `retry` counts. A Pi's own Wi-Fi (brcmfmac) reports no retries, only sends that FAILED after every
   *  retry — so the figure is that, and says so. Absent (an older server) = retries. */
  retryKind?: 'retries' | 'failed';
}
export interface PocketNet { rank: number; ssid: string }
export interface PocketConn {
  pocket?: boolean; local?: boolean; mode?: string; hold?: boolean; apSet?: boolean;
  ssid?: string; apSsid?: string; link?: PocketLink | null; advice?: string;
  nets?: PocketNet[];          // the saved networks, in order (an older server sends none: NETWORK | OWN WI-FI)
  holdNet?: string;            // a network chosen in the menu, held while the radio is in use
  trying?: string; tryLeft?: number;   // the network chosen in the menu that the box is still looking for
  // ★★★ THE WIRED BOX (Stuart, 2026-10-13). An older server sends none of these: Wi-Fi present and on.
  hasWifi?: boolean;           // false = no Wi-Fi radio at all (a Pi 2 on Ethernet)
  wifiOff?: boolean;           // the owner switched Wi-Fi off (the box is wired)
  wired?: boolean;             // an Ethernet link is up
}
/** A choice in the menu: the box's own Wi-Fi, the saved order (older servers), or one saved network. */
export type PocketChoice = 'own' | 'network' | PocketNet;

/** How long the box looks for a chosen network — the root service's TRY_SEC. */
export const POCKET_TRY_SEC = 90;

/** Which network button is lit: the one the box is on (by name), when it is on a network. */
export const pocketOnNet = (c: PocketConn, n: PocketNet): boolean => c.mode === 'client' && c.ssid === n.ssid;

/** Draw the choice at all? Only on a pocket box, on its own network. */
export const pocketHere = (c: PocketConn | null | undefined): boolean => !!(c && c.pocket && c.local);

/** ★★★ Draw the NETWORK CHOOSER? Not on a box with no Wi-Fi, nor one whose owner switched it off (Stuart, 2026-10-13:
 *  "the user must have the ability to hide the wifi option when not in use") — a chooser whose every press is refused
 *  is a dead control. Still polled, so it comes back the moment Wi-Fi is switched on again. */
export const pocketWifiShown = (c: PocketConn | null | undefined): boolean =>
  pocketHere(c) && c!.hasWifi !== false && c!.wifiOff !== true;

/** Which side is lit. */
export const pocketOnOwn = (c: PocketConn): boolean => c.mode === 'fallback-ap' || c.mode === 'setup-ap';

/** The one line beside the choice: where the box is, and how good the link is. */
export function pocketLinkLine(c: PocketConn): string {
  if (c.trying) return `Looking for “${c.trying}”${c.tryLeft ? ` — ${c.tryLeft} s left` : ''} · turn its hotspot on now`;
  if (pocketOnOwn(c)) return `On its own Wi-Fi${c.apSsid ? ` “${c.apSsid}”` : ''}${c.hold ? ' — held while the radio is in use' : ''}`;
  if (c.mode === 'connecting') return 'Joining a network…';
  if (c.mode !== 'client') return '';
  const l = c.link;
  const bits = [`On “${c.ssid || '?'}”`];
  if (l?.signal != null) bits.push(`${String(l.signal).replace('-', '−')} dBm`);
  if (l?.tx != null) bits.push(`${l.tx} Mbit/s`);
  if (l?.retry != null) bits.push(`${l.retry} % ${l.retryKind === 'failed' ? 'failed sends' : 'retries'}`);
  return bits.join(' · ') + (c.holdNet && c.holdNet === c.ssid ? ' — held while the radio is in use' : '');
}

export const POCKET_WEAK_TITLE = 'Weak Wi-Fi on this VibeServer Portable';
export const POCKET_WEAK_BODY = 'Switch it to its own Wi-Fi to avoid drop-outs.';
/** "Not now" quietens the notice for this long. */
export const POCKET_WEAK_SNOOZE_MS = 30 * 60 * 1000;

/** The confirmation before either switch — Stuart: "when switching you will need to exit this session and connect to the
 *  new captive hotspot then start again". */
export function pocketSwitchText(c: PocketConn, to: PocketChoice): { title: string; body: string } {
  if (typeof to === 'object') {
    // ★★★ Stuart, 2026-10-11: "it comes up and says the server will try to access this network for 30 or a minute etc
    //     then it will fall back to the default connection order. This should give users enough time to switch to the
    //     settings and enable personal hotspot on their device."
    const n = `“${to.ssid}”`;
    // ★★★ FROM ITS OWN WI-FI THE BOX LEAVES IT AT ONCE (Stuart's road test, 2026-10-14). The phone pressing is on that
    //     Wi-Fi, and an iPhone does not run its Personal Hotspot while its Wi-Fi is joined to another network — so the
    //     box drops its own Wi-Fi to let the phone host, and brings it back if the network never appears.
    const own = pocketOnOwn(c);
    const here = own ? 'its own Wi-Fi' : c.ssid ? `“${c.ssid}”` : 'where it is';
    return {
      title: `Switch to ${n}?`,
      body: (own ? `This VibeServer Portable will switch its own Wi-Fi off now, so this device leaves it, and look for ${n} `
                 : `This VibeServer Portable will look for ${n} `)
          + `for the next ${POCKET_TRY_SEC} seconds and join it as soon as it `
          + `appears. If ${n} is a phone's hotspot, turn it on now (on an iPhone: Settings › Personal Hotspot › Allow `
          + `Others to Join).\n\nIf it does not find ${n} in time, ${own ? 'its own Wi-Fi comes back' : `it stays on ${here}`}. `
          + `Once it has switched, leave this `
          + `session and start again from the server list on a device connected to ${n}.\n\n`
          + `It stays on ${n} while the radio is in use, and goes back to the usual order after 30 minutes unused or when `
          + 'it is switched off and on.',
    };
  }
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
