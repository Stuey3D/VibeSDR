/**
 * ★★★ PER-SERVER CERTIFICATE PINS — THE DECISIONS (GitHub #52).
 *
 * casm's OpenWebRX+ answered https on 8073 with the self-signed pair it was set up with, and the app could not
 * connect: the platform rightly refuses a certificate no recognised authority signed. Like a browser or SSH, the app
 * now shows THAT certificate's SHA-256 fingerprint and lets the person trust it for THAT host:port only.
 *
 * The native halves enforce it (modules/vibe-tls-trust/VibeTlsCore.m, android/…/VibePinnedTls.kt): system trust
 * first, then only an exact, in-date pinned leaf for that host:port. This file holds what the PROMPT needs to decide
 * and show — the key, the fingerprint's formatting, what the certificate says about itself, and which prompt (if any)
 * a failed connection deserves.
 *
 * Pure: no React Native imports, so Node runs scripts/test_tls_pin.ts against it directly.
 */

/** "host:port" — host lower-cased, IPv6 brackets and a trailing dot removed. The native key is built the same way. */
export function pinKey(host: string, port: number): string {
  let h = (host || '').trim().toLowerCase();
  if (h.startsWith('[') && h.endsWith(']')) h = h.slice(1, -1);
  h = h.replace(/\.+$/, '');
  return `${h}:${port}`;
}

/** A SHA-256 fingerprint, normalised: 64 lower-case hex characters, or '' when it is not one. */
export function normaliseFingerprint(fp: string): string {
  const h = (fp || '').replace(/[^0-9a-f]/gi, '').toLowerCase();
  return h.length === 64 ? h : '';
}

/** The way browsers and `openssl -fingerprint` show it: upper-case byte pairs joined by colons. */
export function formatFingerprint(fp: string): string {
  const h = normaliseFingerprint(fp);
  return h ? h.toUpperCase().match(/../g)!.join(':') : '';
}

/** ★ Split for a narrow screen: 16 bytes a line, so 32 bytes is two lines that never wrap mid-pair. */
export function fingerprintLines(fp: string, perLine = 16): string[] {
  const pairs = formatFingerprint(fp).split(':').filter(Boolean);
  const out: string[] = [];
  for (let i = 0; i < pairs.length; i += perLine) out.push(pairs.slice(i, i + perLine).join(':'));
  return out;
}

// ── what the certificate says about itself (for the prompt's text only — never for a trust decision) ─────────────

export interface CertSummary {
  subjectCN: string;
  issuerCN: string;
  /** subjectAltName dNSName and iPAddress entries, in order. */
  names: string[];
  notBefore: Date | null;
  notAfter: Date | null;
  /** Issuer and subject are the same name — what a self-signed certificate looks like. */
  selfSigned: boolean;
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function b64ToBytes(s: string): Uint8Array {
  const clean = (s || '').replace(/[^A-Za-z0-9+/]/g, '');
  const out = new Uint8Array(Math.floor(clean.length * 3 / 4));
  let o = 0, buf = 0, bits = 0;
  for (let i = 0; i < clean.length; i++) {
    buf = (buf << 6) | B64.indexOf(clean[i]);
    bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (buf >> bits) & 0xff; }
  }
  return out.subarray(0, o);
}

interface Tlv { tag: number; start: number; end: number; hdr: number }

/** One DER element at `p`: tag, where its content starts and ends. Throws on anything malformed. */
function tlv(d: Uint8Array, p: number): Tlv {
  if (p + 2 > d.length) throw new Error('short');
  const tag = d[p];
  let len = d[p + 1];
  let hdr = 2;
  if (len & 0x80) {
    const n = len & 0x7f;
    if (n < 1 || n > 4 || p + 2 + n > d.length) throw new Error('bad length');
    len = 0;
    for (let i = 0; i < n; i++) len = len * 256 + d[p + 2 + i];
    hdr = 2 + n;
  }
  const start = p + hdr;
  const end = start + len;
  if (end > d.length) throw new Error('overrun');
  return { tag, start, end, hdr };
}

function children(d: Uint8Array, t: Tlv): Tlv[] {
  const out: Tlv[] = [];
  for (let p = t.start; p < t.end;) { const c = tlv(d, p); out.push(c); p = c.end; }
  return out;
}

function str(d: Uint8Array, t: Tlv): string {
  let s = '';
  for (let i = t.start; i < t.end; i++) s += String.fromCharCode(d[i]);
  try { return decodeURIComponent(escape(s)); } catch { return s; }   // UTF8String → JS string; Latin-1 left as is
}

function sameBytes(d: Uint8Array, a: Tlv, b: Tlv): boolean {
  if (a.end - a.start !== b.end - b.start) return false;
  for (let i = 0; i < a.end - a.start; i++) if (d[a.start + i] !== d[b.start + i]) return false;
  return true;
}

/** CN out of a Name (SEQUENCE OF SET OF SEQUENCE { OID, value }). OID 2.5.4.3 = 55 04 03. */
function commonName(d: Uint8Array, name: Tlv): string {
  for (const rdn of children(d, name)) for (const atv of children(d, rdn)) {
    const [oid, val] = children(d, atv);
    if (oid && val && oid.end - oid.start === 3 && d[oid.start] === 0x55 && d[oid.start + 1] === 0x04 && d[oid.start + 2] === 0x03)
      return str(d, val);
  }
  return '';
}

function time(d: Uint8Array, t: Tlv): Date | null {
  const s = str(d, t);
  const m = t.tag === 0x17
    ? /^(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)?Z$/.exec(s)
    : /^(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)?(?:\.\d+)?Z$/.exec(s);
  if (!m) return null;
  let y = parseInt(m[1], 10);
  if (t.tag === 0x17) y += y < 50 ? 2000 : 1900;                     // RFC 5280 §4.1.2.5.1
  return new Date(Date.UTC(y, parseInt(m[2], 10) - 1, parseInt(m[3], 10), parseInt(m[4], 10), parseInt(m[5], 10), parseInt(m[6] ?? '0', 10)));
}

function ipText(d: Uint8Array, t: Tlv): string {
  const b = Array.from(d.subarray(t.start, t.end));
  if (b.length === 4) return b.join('.');
  if (b.length === 16) {
    const g: string[] = [];
    for (let i = 0; i < 16; i += 2) g.push(((b[i] << 8) | b[i + 1]).toString(16));
    return g.join(':').replace(/(^|:)0(:0)+(:|$)/, '::');
  }
  return '';
}

/** Read the leaf the native probe returned (base64 DER). null when it cannot be read — the prompt then shows only the
 *  fingerprint, which is the part that matters. */
export function parseCertificate(derBase64: string): CertSummary | null {
  try {
    const d = b64ToBytes(derBase64);
    const cert = tlv(d, 0);
    const tbs = children(d, cert)[0];
    const f = children(d, tbs);
    let i = 0;
    if (f[0] && f[0].tag === 0xa0) i++;                              // [0] version
    const issuer = f[i + 2], validity = f[i + 3], subject = f[i + 4];
    const [nb, na] = children(d, validity);
    const names: string[] = [];
    const ext = f.slice(i + 6).find(x => x.tag === 0xa3);            // [3] extensions
    if (ext) {
      const list = children(d, ext)[0];
      for (const e of children(d, list)) {
        const parts = children(d, e);
        const oid = parts[0];
        // 2.5.29.17 subjectAltName = 55 1D 11; the value is the last element (an OCTET STRING)
        if (oid.end - oid.start === 3 && d[oid.start] === 0x55 && d[oid.start + 1] === 0x1d && d[oid.start + 2] === 0x11) {
          const oct = parts[parts.length - 1];
          const seq = tlv(d, oct.start);
          for (const gn of children(d, seq)) {
            if (gn.tag === 0x82) names.push(str(d, gn));
            else if (gn.tag === 0x87) { const ip = ipText(d, gn); if (ip) names.push(ip); }
          }
        }
      }
    }
    return {
      subjectCN: commonName(d, subject), issuerCN: commonName(d, issuer), names,
      notBefore: nb ? time(d, nb) : null, notAfter: na ? time(d, na) : null,
      selfSigned: sameBytes(d, issuer, subject),
    };
  } catch { return null; }
}

/** Does the certificate name this host? SANs when it has any, else the CN — the usual rules, one wildcard label.
 *  ★ For the prompt's WORDS only ("it is issued to openwebrx, not this address"); the pin never depends on it. */
export function certNamesHost(c: CertSummary, host: string): boolean {
  const h = pinKey(host, 0).replace(/:0$/, '');
  const list = c.names.length ? c.names : (c.subjectCN ? [c.subjectCN] : []);
  return list.some(n => {
    const name = n.toLowerCase().replace(/\.$/, '');
    if (name === h) return true;
    if (name.startsWith('*.')) {
      const rest = name.slice(1);                                    // ".example.org"
      return h.endsWith(rest) && h.indexOf('.') === h.length - rest.length;
    }
    return false;
  });
}

// ── which prompt ─────────────────────────────────────────────────────────────────────────────────────────────────

/** What the native probe (VibeTlsTrust.inspect) reports about host:port. */
export interface TlsInspection {
  systemTrusted: boolean;
  /** ok | untrusted | expired | hostname | unreachable | error */
  reason: string;
  sha256: string;
  der?: string;
  pinned?: string | null;
  message?: string;
}

export type TlsDecision =
  /** Not a certificate problem we can help with: the system trusts it, the pin matches, or nothing answered. */
  | { kind: 'none' }
  /** First use: not from a recognised authority, no pin — offer "Trust this server". */
  | { kind: 'offer'; sha256: string }
  /** A pin exists and this is a different certificate — warn, show both, require re-trust. */
  | { kind: 'changed'; sha256: string; previous: string }
  /** A problem trust on first use must not paper over: out of date, or a public certificate for another name. */
  | { kind: 'refuse'; why: 'expired' | 'hostname'; sha256: string };

/**
 * ★★★ THE RULES, in one place:
 *  - the system trusts it → nothing to ask (a pin is never needed for a certificate the platform accepts);
 *  - expired / not yet valid → refused, pinned or not (trust on first use replaces "who signed it", not "is it current");
 *  - a publicly trusted certificate for ANOTHER name → refused: that is a misconfiguration or an interception, and the
 *    pin exists for certificates nobody vouches for, not for ones somebody vouches for elsewhere;
 *  - untrusted + no pin → offer; untrusted + the pin → nothing to ask (the failure was something else);
 *    untrusted + a different pin → "this server's certificate has changed". Never silently.
 *  ★ A self-signed certificate that does not name the address (OWRX+ owners' usual `openssl req -x509` one names
 *    "openwebrx") is still offered: the person is trusting THAT certificate for THIS host:port, which is exactly the
 *    question a name match would have answered.
 */
export function decideTls(r: TlsInspection | null | undefined, pinned?: string | null): TlsDecision {
  if (!r || r.systemTrusted) return { kind: 'none' };
  const fp = normaliseFingerprint(r.sha256);
  if (!fp) return { kind: 'none' };
  const pin = normaliseFingerprint(pinned ?? r.pinned ?? '');
  if (r.reason === 'expired') return { kind: 'refuse', why: 'expired', sha256: fp };
  if (r.reason === 'hostname') return { kind: 'refuse', why: 'hostname', sha256: fp };
  if (r.reason !== 'untrusted') return { kind: 'none' };
  if (!pin) return { kind: 'offer', sha256: fp };
  if (pin === fp) return { kind: 'none' };
  return { kind: 'changed', sha256: fp, previous: pin };
}

/** host and port of an http(s)/ws(s) address, when it is TLS — the only kind a certificate can be asked about. */
export function tlsTarget(url: string): { host: string; port: number } | null {
  const m = /^(https|wss):\/\/(\[[^\]]+\]|[^/:?#]+)(?::(\d+))?/i.exec((url || '').trim());
  if (!m) return null;
  const port = m[3] ? parseInt(m[3], 10) : 443;
  if (!(port > 0 && port < 65536)) return null;
  const host = m[2].startsWith('[') ? m[2].slice(1, -1) : m[2];
  return { host, port };
}
