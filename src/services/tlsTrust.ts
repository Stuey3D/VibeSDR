/**
 * ★★★ TRUST ON FIRST USE, FOR A SERVER WITH ITS OWN CERTIFICATE (GitHub #52).
 *
 * casm's OpenWebRX+ served https on 8073 with a self-signed certificate — what the OWRX+ FAQ's cert.pem/key.pem
 * usually are — and the app could not connect: the platform refuses a certificate no recognised authority signed, and
 * it is right to. Like a browser or SSH, the app now shows THAT certificate (its SHA-256 fingerprint) and lets the
 * person trust it for THAT host:port only. Later connections accept that exact certificate and nothing else; a
 * different one is "this server's certificate has changed", never a silent fallback.
 *
 * The parts:
 *  - native enforcement, in the handshake: modules/vibe-tls-trust (iOS) and android/…/VibePinnedTls.kt — the system's
 *    verdict first, then only an exact, in-date pinned leaf for that host:port. The pins live there too
 *    (NSUserDefaults / SharedPreferences), so they are in force from launch, before any JS has run.
 *  - the decisions: src/utils/tlsPin.ts (pure, tested by scripts/test_tls_pin.ts).
 *  - this file: after a TLS connection FAILED, ask the native probe what the certificate was, decide, and put the
 *    prompt (components/TlsTrustPrompt.tsx, mounted once in App) in front of the person. Then log it.
 *
 * ★★ ONLY AFTER A FAILURE, ONLY FOR THINGS THE PERSON DID. The probe costs one handshake and is asked only when a
 *    connection the user started has already failed — never on a background refresh, which simply fails as before.
 * ★ The web client has no say in certificates (the browser owns them) — NativeModules.VibeTlsTrust is absent there
 *   and everything here is a no-op.
 */
import { NativeModules } from 'react-native';
import { decideTls, parseCertificate, pinKey, tlsTarget, formatFingerprint,
         type CertSummary, type TlsDecision, type TlsInspection } from '../utils/tlsPin';
import { noteDecision } from './protocolLog';
import { setTlsGate, TlsNotTrusted } from './sdrTypes';

export { TlsNotTrusted };
import { crumb } from './crumbs';

type PinEntry = { sha256: string; subject?: string; at?: number };
type NativeTls = {
  inspect(host: string, port: number): Promise<TlsInspection>;
  listPins(): Promise<Record<string, PinEntry>>;
  trust(host: string, port: number, sha256: string, subject: string): Promise<boolean>;
  forget(host: string, port: number): Promise<boolean>;
};
const Native = NativeModules.VibeTlsTrust as NativeTls | undefined;

/** The outcome of asking. 'none' = it was not a certificate we could do anything about. */
export type TlsOutcome = 'trusted' | 'declined' | 'refused' | 'none';

export interface TlsPrompt {
  host: string;
  port: number;
  decision: Exclude<TlsDecision, { kind: 'none' }>;
  cert: CertSummary | null;
  /** The person's answer: true = trust (offer / changed), anything else = no. */
  answer(trust: boolean): void;
}

let current: TlsPrompt | null = null;
const queue: TlsPrompt[] = [];
const listeners = new Set<(p: TlsPrompt | null) => void>();
const inFlight = new Map<string, Promise<TlsOutcome>>();
/** ★ A "Cancel" is remembered for a minute per host:port, so the screen that fails NEXT (the picker's probe, then the
 *  receiver screen's socket) does not ask the same question twice in a row. */
const declinedAt = new Map<string, number>();
const DECLINE_MEMORY_MS = 60_000;

function publish() { for (const l of listeners) { try { l(current); } catch {} } }

function show(p: TlsPrompt) {
  if (current) queue.push(p); else { current = p; publish(); }
}
function next() { current = queue.shift() ?? null; publish(); }

/** For TlsTrustPrompt: the prompt to show now (or null). Called at once with the current one. */
export function subscribeTlsPrompt(fn: (p: TlsPrompt | null) => void): () => void {
  listeners.add(fn);
  fn(current);
  return () => { listeners.delete(fn); };
}

function log(line: string) {
  noteDecision('tls', line);
  crumb(`tls ${line}`);
}

/**
 * ★★★ AFTER A CONNECTION TO `url` FAILED: was it the certificate? If it was one the person can trust (first use, or a
 *     changed certificate), ask — and on yes, pin it, so the caller can simply try again. A refusal we must not paper
 *     over (expired; a public certificate for another name) is explained, with no trust button. Anything else
 *     (plain http, unreachable, the system trusts it, the pin already matches) → 'none', and the caller reports its
 *     own error exactly as before.
 *  ★ One question per host:port at a time — two sockets failing together share the answer.
 */
export function checkTlsAfterFailure(url: string): Promise<TlsOutcome> {
  const t = tlsTarget(url);
  if (!t || !Native?.inspect) return Promise.resolve('none');
  const key = pinKey(t.host, t.port);
  const busy = inFlight.get(key);
  if (busy) return busy;
  const at = declinedAt.get(key);
  if (at && Date.now() - at < DECLINE_MEMORY_MS) return Promise.resolve('declined');
  const p = (async (): Promise<TlsOutcome> => {
    let r: TlsInspection;
    try { r = await Native.inspect(t.host, t.port); } catch { return 'none'; }
    const decision = decideTls(r);
    if (decision.kind === 'none') return 'none';
    const fp = formatFingerprint(decision.sha256);
    log(decision.kind === 'offer' ? `${key} not from a recognised authority, sha256 ${fp} — asking`
      : decision.kind === 'changed' ? `${key} CERTIFICATE CHANGED: was ${formatFingerprint(decision.previous)}, now ${fp} — refused until re-trusted`
      : `${key} refused (${decision.why}) sha256 ${fp}`);
    const cert = r.der ? parseCertificate(r.der) : null;
    const trust = await new Promise<boolean>((resolve) => {
      show({ host: t.host, port: t.port, decision, cert,
             answer: (yes) => { resolve(yes); next(); } });
    });
    if (decision.kind === 'refuse') { declinedAt.set(key, Date.now()); return 'refused'; }
    if (!trust) { declinedAt.set(key, Date.now()); log(`${key} not trusted (cancelled)`); return 'declined'; }
    try { await Native.trust(t.host, t.port, decision.sha256, cert?.subjectCN ?? ''); }
    catch (e) { log(`${key} trust FAILED to save: ${String((e as Error)?.message ?? e)}`); return 'declined'; }
    declinedAt.delete(key);
    log(`${key} trusted, sha256 ${fp}${decision.kind === 'changed' ? ' (replaces the old certificate)' : ''}`);
    return 'trusted';
  })().finally(() => { inFlight.delete(key); });
  inFlight.set(key, p);
  return p;
}

/** The pin for this address's host:port, or null. */
export async function pinForUrl(url: string): Promise<(PinEntry & { host: string; port: number }) | null> {
  const t = tlsTarget(url);
  if (!t || !Native?.listPins) return null;
  try {
    const all = await Native.listPins();
    const e = all?.[pinKey(t.host, t.port)];
    return e?.sha256 ? { ...e, host: t.host, port: t.port } : null;
  } catch { return null; }
}

/** Forget the trusted certificate for this address's host:port. True if there was one. */
export async function forgetPinForUrl(url: string): Promise<boolean> {
  const t = tlsTarget(url);
  if (!t || !Native?.forget) return false;
  try {
    const had = await Native.forget(t.host, t.port);
    if (had) log(`${pinKey(t.host, t.port)} trusted certificate forgotten`);
    return had;
  } catch { return false; }
}

// ★ Hand the check to sdrTypes' probes (it cannot import this file: the web client bundles sdrTypes). See setTlsGate.
setTlsGate(checkTlsAfterFailure);
