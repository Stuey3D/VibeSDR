/**
 * auth.ts — VibeServer PIN handshake (browser).
 *
 * Uses the app's own pure-JS HMAC (src/services/vibeAuth.ts), NOT WebCrypto.
 * That is deliberate: `crypto.subtle` only exists in a SECURE CONTEXT, and a
 * VibeServer is reached over plain http:// at a LAN IP — so subtle is undefined
 * there and every PIN handshake would throw. (It works on localhost, which is
 * treated as secure — so this fails only on the real device, never in dev.)
 *
 * Shim contract (local_sdr_shim.cpp:370,1622):
 *   GET /vibeserver/auth -> {"required":false}
 *                        -> {"required":true,"nonce":"<32 hex chars>"}
 *   token = hex(HMAC-SHA256(key = PIN utf8, msg = nonce AS ITS ASCII HEX TEXT))
 *   then every WS carries ?vs_nonce=<nonce>&vs_auth=<token>
 *
 * The nonce is a reusable session credential (1h TTL) shared by both sockets
 * and across reconnects — fetch it once, not per socket.
 */

export interface AuthState {
  /** Query suffix to append to WS paths, e.g. "vs_nonce=..&vs_auth=..", or ''. */
  query: string;
  required: boolean;
  /** Seconds remaining on a brute-force lockout for THIS client, 0 if none. The server
   *  tells us, because a WebSocket error carries no status code — without it a locked-out
   *  browser can only report "wrong PIN", and the user retypes a correct one forever. */
  lockedFor?: number;
}

// HMAC-SHA256(pin, nonce) — the app's implementation, shared verbatim. The nonce
// is HMAC'd as its ASCII hex TEXT, not decoded to bytes.
import { vibeAuthToken } from '../../../src/services/vibeAuth';
export { vibeAuthToken };

/** Ask the server whether a PIN is needed, and for the session nonce. */
export async function fetchAuthChallenge(
  base: string,
): Promise<{ required: boolean; nonce: string; lockedFor: number }> {
  const r = await fetch(`${base}/vibeserver/auth`, { cache: 'no-store' });
  if (!r.ok) throw new Error(`auth probe failed: HTTP ${r.status}`);
  const j = await r.json();
  return {
    required: !!j.required,
    nonce: j.nonce ?? '',
    lockedFor: Number(j.lockedFor) || 0,
  };
}

/**
 * Resolve credentials for a server. Returns the WS query suffix.
 * Throws if a PIN is required but none/incorrect was supplied — note the shim
 * only rejects at WS-upgrade time (401), so a wrong PIN surfaces there, not here.
 */
export async function resolveAuth(base: string, pin: string): Promise<AuthState> {
  const { required, nonce, lockedFor } = await fetchAuthChallenge(base);
  if (!required) return { query: '', required: false };
  // Say so plainly. The WS upgrade would answer 429, but a WebSocket error carries no
  // status code — so the client used to report "wrong PIN" and leave the user retyping
  // a correct one until the backoff quietly expired.
  if (lockedFor > 0) {
    throw new Error(
      `Too many failed attempts — locked out for ${lockedFor}s. The PIN may well be right.`);
  }
  if (!pin) throw new Error('PIN required');
  const token = vibeAuthToken(pin, nonce);
  return { query: `vs_nonce=${nonce}&vs_auth=${token}`, required: true, lockedFor: 0 };
}

/** ★★ ADMIN OVERRIDE credentials for the connect URL — a nonce and an HMAC, NEVER the
 *  password itself. Taking over an occupied radio has to be decided before the slot is
 *  claimed, so it cannot wait for the admin_unlock message: a busy server never opens the
 *  socket that would carry it. But a password in a URL travels in the clear on plain HTTP and
 *  lands in every proxy and server log on the way — unacceptable for a receiver on the public
 *  internet (Stuart, 2026-07-27).
 *  ★ The server issues a nonce even when NO PIN is set, which is precisely the public-receiver
 *  case: open to every listener, closed to anyone touching the hardware. */
export async function resolveAdminOverride(base: string, password: string): Promise<string> {
  if (!password) return '';
  try {
    const { nonce } = await fetchAuthChallenge(base);
    if (!nonce) return '';        // server too old to issue one
    const token = vibeAuthToken(password, nonce);
    return `vs_admin_nonce=${encodeURIComponent(nonce)}&vs_admin_auth=${token}`;
  } catch { return ''; }
}

/** Append the auth suffix to a WS path, picking '?' or '&' correctly. */
export function withAuth(path: string, auth: AuthState): string {
  if (!auth.query) return path;
  // The /ws/audio 401 bug: path had no query, so '&vs_nonce' was invalid.
  return path + (path.includes('?') ? '&' : '?') + auth.query;
}
