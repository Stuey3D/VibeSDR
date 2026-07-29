// VibeServer PIN client handshake. Before opening the spectrum/audio WebSockets,
// the client fetches a single-use nonce from GET /vibeserver/auth and computes
// HMAC-SHA256(pin, nonce). The token — not the PIN — is appended to the WS URLs
// (?vs_nonce=&vs_auth=), so the secret never crosses the wire. A server with no
// PIN replies { required:false } and we send nothing (behaves as plain UberSDR).

// ── Pure-JS SHA-256 + HMAC-SHA256 (no native/crypto dependency) ──────────────
const K = new Uint32Array([
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);

const rotr = (x: number, n: number) => (x >>> n) | (x << (32 - n));

function sha256(msg: Uint8Array): Uint8Array {
  const H = new Uint32Array([
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const ml = msg.length * 8;
  const withPad = ((msg.length + 8) >> 6) + 1;          // 64-byte blocks
  const buf = new Uint8Array(withPad * 64);
  buf.set(msg);
  buf[msg.length] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(buf.length - 4, ml >>> 0);
  dv.setUint32(buf.length - 8, Math.floor(ml / 0x100000000));
  const w = new Uint32Array(64);
  for (let off = 0; off < buf.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >>> 3);
      const s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >>> 10);
      w[i] = (w[i-16] + s0 + w[i-7] + s1) >>> 0;
    }
    let [a,b,c,d,e,f,g,h] = H;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + K[i] + w[i]) >>> 0;
      const S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      h=g; g=f; f=e; e=(d + t1) >>> 0; d=c; c=b; b=a; a=(t1 + t2) >>> 0;
    }
    H[0]=(H[0]+a)>>>0; H[1]=(H[1]+b)>>>0; H[2]=(H[2]+c)>>>0; H[3]=(H[3]+d)>>>0;
    H[4]=(H[4]+e)>>>0; H[5]=(H[5]+f)>>>0; H[6]=(H[6]+g)>>>0; H[7]=(H[7]+h)>>>0;
  }
  const out = new Uint8Array(32);
  const odv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) odv.setUint32(i * 4, H[i]);
  return out;
}

function hmacSha256(key: Uint8Array, msg: Uint8Array): Uint8Array {
  let k = key.length > 64 ? sha256(key) : key;
  const block = new Uint8Array(64); block.set(k);
  const ipad = new Uint8Array(64), opad = new Uint8Array(64);
  for (let i = 0; i < 64; i++) { ipad[i] = block[i] ^ 0x36; opad[i] = block[i] ^ 0x5c; }
  const inner = sha256(concat(ipad, msg));
  return sha256(concat(opad, inner));
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length); out.set(a); out.set(b, a.length); return out;
}
const enc = (s: string) => new Uint8Array(Array.from(s, c => c.charCodeAt(0) & 0xff));
const toHex = (b: Uint8Array) => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');

/** Compute the token a VibeServer expects for (pin, nonce). Exposed for tests. */
export function vibeAuthToken(pin: string, nonce: string): string {
  return toHex(hmacSha256(enc(pin), enc(nonce)));
}

/**
 * Resolve the auth query suffix for a VibeServer at `baseUrl` (http://host:port).
 * Returns e.g. "&vs_nonce=abc&vs_auth=def" to append to a WS URL, or "" when the
 * server needs no PIN. Throws only on a network failure the caller can surface.
 */
/**
 * Does the VibeServer at `baseUrl` want a PIN?
 *
 * A server DISCOVERED over mDNS carries this in its TXT record, but one the user
 * TYPED into the Custom-server box has no TXT to read — so ask it. Without this we
 * would either prompt for a PIN on an open server (annoying) or never prompt on a
 * locked one (a failed connect with no explanation). Throws on a network failure,
 * which the caller surfaces as "can't reach it".
 */
export async function vibeServerNeedsPin(baseUrl: string): Promise<boolean> {
  const base = baseUrl.replace(/\/+$/, '');
  const resp = await fetch(`${base}/vibeserver/auth`);
  const j = await resp.json() as { required?: boolean };
  return !!j?.required;
}

/** ★★ ADMIN OVERRIDE CREDENTIALS for the connect URL — a nonce and an HMAC, never the
 *  password. The override has to be decided BEFORE the slot is claimed, so it cannot use the
 *  admin_unlock message (a busy server will not open the socket that carries it); but putting
 *  the password in the URL would put it in the clear on plain HTTP and into every proxy and
 *  server log on the way, which is exactly wrong for a receiver on the public internet.
 *  ★ Same scheme as the PIN: the server issues a single-use nonce, we return
 *  HMAC-SHA256(password, nonce), and the secret never crosses the link. The server applies the
 *  same brute-force lockout it uses everywhere else.
 *  Returns '' when the server issues no nonce (too old to support this). */
export async function resolveVibeAdminAuth(baseUrl: string, password: string): Promise<string> {
  if (!password) return '';
  const base = baseUrl.replace(/\/+$/, '');
  try {
    const resp = await fetch(`${base}/vibeserver/auth`, { cache: 'no-store' });
    const j = await resp.json() as { nonce?: string };
    // ★ Note we do NOT check `required`: that flag is about the PIN. The configuration this
    // exists for is a public receiver with NO PIN and an admin password.
    if (!j?.nonce) return '';
    const token = vibeAuthToken(password, j.nonce);
    return `&vs_admin_nonce=${encodeURIComponent(j.nonce)}&vs_admin_auth=${token}`;
  } catch { return ''; }
}

export async function resolveVibeAuth(baseUrl: string, pin: string): Promise<string> {
  const base = baseUrl.replace(/\/+$/, '');
  const resp = await fetch(`${base}/vibeserver/auth`);
  const j = await resp.json() as { required?: boolean; nonce?: string };
  if (!j?.required || !j.nonce) return '';
  const token = vibeAuthToken(pin ?? '', j.nonce);
  return `&vs_nonce=${encodeURIComponent(j.nonce)}&vs_auth=${token}`;
}
