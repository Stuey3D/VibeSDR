/**
 * ★★★ PORTABLE WI-FI in the app (Stuart, 2026-10-10) — the cog menu's Network | Own Wi-Fi, and the "weak Wi-Fi" pop-up
 * while listening. Words: services/pocketConnection.ts (shared with the served page). Whether the link is weak: the
 * server (one rule).
 *
 * ★ Costs nothing anywhere else: ONE request per receiver. Unless the answer is "a pocket box, and you are on its own
 *   network" (never through the tunnel — the server says local:false there), nothing is drawn and nothing is polled.
 *   On a pocket box: one small GET every 30 s while the app is in front.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { Alert, AppState } from 'react-native';
import {
  pocketHere, pocketOnOwn, pocketLinkLine, pocketSwitchText,
  POCKET_WEAK_TITLE, POCKET_WEAK_BODY, POCKET_WEAK_SNOOZE_MS, type PocketConn,
} from '../services/pocketConnection';

const originOf = (base: string) => (String(base || '').match(/^(https?:\/\/[^/]+)/) ?? [])[1] ?? '';

async function fetchConn(origin: string): Promise<PocketConn | null> {
  try {
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), 4000);
    const r = await fetch(`${origin}/vibeserver/pocket/connection`, { signal: ac.signal });
    clearTimeout(t);
    return r.ok ? ((await r.json()) as PocketConn) : null;
  } catch { return null; }
}

/** `base`: the receiver's address (null = not a VibeServer). `onSwitched`: leave this session (Stuart: "you will need
 *  to exit this session and connect to the new … hotspot then start again"). */
export function usePocketWifi(base: string | null, onSwitched: () => void) {
  const [conn, setConn] = useState<PocketConn | null>(null);
  const origin = base ? originOf(base) : '';
  const switching = useRef(false);
  const snoozedUntil = useRef(0);
  const askedThisSpell = useRef(false);

  useEffect(() => {
    setConn(null);
    switching.current = false;
    if (!origin) return;
    let dead = false;
    let timer: ReturnType<typeof setInterval> | null = null;
    const poll = async () => {
      if (dead || switching.current || AppState.currentState !== 'active') return;
      const c = await fetchConn(origin);
      if (!dead && pocketHere(c)) setConn(c);
    };
    void (async () => {
      const c = await fetchConn(origin);
      if (dead || !pocketHere(c)) return;          // ★ not a pocket box, or not on its network: nothing more, ever
      setConn(c);
      timer = setInterval(poll, 30000);
    })();
    return () => { dead = true; if (timer) clearInterval(timer); };
  }, [origin]);

  const choose = useCallback((to: 'own' | 'network') => {
    if (!conn || (to === 'own') === pocketOnOwn(conn)) return;
    const t = pocketSwitchText(conn, to);
    Alert.alert(t.title, t.body, [
      { text: 'Cancel', style: 'cancel' },
      { text: to === 'own' ? 'Switch to own Wi-Fi' : 'Switch back', onPress: async () => {
        try {
          const r = await fetch(`${origin}/vibeserver/pocket/connection`, {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ to }) });
          if (!r.ok) {
            const j: any = await r.json().catch(() => ({}));
            Alert.alert('Not switched', j?.error || `The box refused (${r.status}).`);
            return;
          }
          switching.current = true;
          Alert.alert('Switching', to === 'own'
            ? `Join “${conn.apSsid || 'its own Wi-Fi'}” in Settings › Wi-Fi, then start again from the server list.`
            : 'Join your network in Settings › Wi-Fi, then start again from the server list.',
            [{ text: 'OK', onPress: onSwitched }]);
        } catch {
          Alert.alert('Not switched', 'The box did not answer.');
        }
      } },
    ]);
  }, [conn, origin, onSwitched]);

  // ★ The pop-up while listening: once per weak spell; "Not now" quietens it for 30 minutes.
  useEffect(() => {
    if (!conn) return;
    const weak = conn.advice === 'weak' && !!conn.apSet && !pocketOnOwn(conn);
    if (!weak) { askedThisSpell.current = false; return; }
    if (askedThisSpell.current || switching.current || Date.now() < snoozedUntil.current) return;
    askedThisSpell.current = true;
    Alert.alert(POCKET_WEAK_TITLE, POCKET_WEAK_BODY, [
      { text: 'Not now', style: 'cancel', onPress: () => { snoozedUntil.current = Date.now() + POCKET_WEAK_SNOOZE_MS; } },
      { text: 'Switch to own Wi-Fi', onPress: () => choose('own') },
    ]);
  }, [conn, choose]);

  /** What the cog menu needs, or null to draw nothing. */
  const menu = conn ? {
    onOwn: pocketOnOwn(conn), canOwn: !!conn.apSet, line: pocketLinkLine(conn),
    weak: conn.advice === 'weak' && !pocketOnOwn(conn), onChoose: choose,
  } : null;
  return { menu };
}
