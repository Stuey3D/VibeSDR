/**
 * THE SHARED-DIAL CONTRACT — NO CLIENT SENDS BACK A DIAL IT WAS GIVEN (src/services/audioTuneForward.ts, 2026-10-22).
 *
 * The Lenovo, RSP1A, shared dial, 00:07–00:10: Safari tuned 657 → 693 kHz and Stuart's V12 iPhone sent every step
 * back as a `tune` — sometimes a step BEHIND ("684 after 693"), dragging the radio back under the person tuning.
 * The app's LocalAudioPlayer forwarded every change of the screen's frequency, and adopting the room's dial is
 * a change. The server half (two real clients over real sockets, and the served web page) is
 * scripts/test-dial-echo.mjs.
 *
 * Proves:
 *   • the rule: the server's own dial (frequency + mode) is never forwarded, sealed or not; a person's change
 *     is — counted or not (the drum does not bump userTuneSeq); a step and straight back inside the round trip
 *     goes; a counted re-assert of the same values goes; nothing heard = the old rule;
 *   • the evidence, replayed with lagging renders: zero sends;
 *   • the WEB client: a real SpectrumClient fed another listener's walk sends nothing;
 *   • the wiring: LocalAudioPlayer asks the rule, VibeServerWsClient writes serverDial before the adopt emits,
 *     the converter maps it, the screen passes it in hardware Hz.
 *
 * Run: node --no-warnings scripts/test_dial_echo.ts
 */
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';
import { isServerDial, shouldForwardTune, OWN_TUNE_WINDOW_MS } from '../src/services/audioTuneForward.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const src = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');

// ── 1. the rule ──
const at = (hz: number, mode = 'am') => ({ frequency: hz, mode, bandwidthLow: -5000, bandwidthHigh: 5000 });
const j = (a: ReturnType<typeof at>) => JSON.stringify({ type: 'tune', ...a });
const base = { seqAdvanced: false, sealed: false, helloTune: j(at(648_000)), msSinceSend: Infinity };
const F = (asked: ReturnType<typeof at>, serverDial: unknown, o: Partial<typeof base> = {}) =>
  shouldForwardTune({ ...base, ...o, asked, serverDial: serverDial as never, askedJson: j(asked) });

ok(isServerDial(at(666_000), { frequency: 666_000, mode: 'am' }), 'the same frequency and mode = the server\'s dial');
ok(isServerDial(at(666_000), { frequency: 666_000.4, mode: 'AM' }), '…under a hertz, any case');
ok(!isServerDial(at(666_000), { frequency: 675_000, mode: 'am' }), 'another frequency is not');
ok(!isServerDial(at(666_000, 'usb'), { frequency: 666_000, mode: 'am' }), 'another mode is not');
ok(isServerDial({ ...at(666_000), bandwidthLow: -3000 }, { frequency: 666_000, mode: 'am', bandwidthLow: -5000, bandwidthHigh: 5000 }),
   'the passband is not compared (it has its own door, `bandwidth` on the spectrum socket)');
ok(!isServerDial(at(666_000), null) && !isServerDial(at(666_000), { frequency: 0 }), 'nothing heard: no opinion');

ok(!F(at(666_000), { frequency: 666_000, mode: 'am' }).send, 'unsealed: an ADOPTED dial is not sent back');
ok(!F(at(666_000), { frequency: 666_000, mode: 'am' }, { sealed: true }).send, 'sealed: an adopted dial does not unseal');
ok(F(at(675_000), { frequency: 666_000, mode: 'am' }).send, 'a person\'s step (no counter bump — the drum) is sent');
{
  const d = F(at(675_000), { frequency: 666_000, mode: 'am' }, { sealed: true });
  ok(d.send && d.unseal, 'sealed: a person\'s step sends and unseals');
}
ok(!F(at(648_000), null, { sealed: true }).send, 'sealed: the socket restating its hello is not sent');
ok(F(at(666_000), { frequency: 666_000, mode: 'am' }, { seqAdvanced: true }).send,
   'a COUNTED re-assert of the server\'s own values goes (a restore)');
ok(F(at(666_000), { frequency: 666_000, mode: 'am' }, { msSinceSend: OWN_TUNE_WINDOW_MS - 1 }).send,
   'a step and straight back inside our own window goes — the server had not confirmed the step yet');
ok(!F(at(666_000), { frequency: 666_000, mode: 'am' }, { msSinceSend: OWN_TUNE_WINDOW_MS }).send, '…and past it, a match is an adoption again');
ok(OWN_TUNE_WINDOW_MS < 1500, 'the window is shorter than the client\'s 1.5 s adopt settle (inside it no adopt can happen)');
ok(F(at(675_000), null).send, 'nothing heard from the server: the old rule (a change is sent)');

// ── 2. the evidence, replayed: Safari walks 657 → 693, the phone adopts each and renders it late ──
{
  const walk = [657, 666, 675, 684, 693, 702, 711, 720].map((k) => k * 1000);
  // Each render carries the snapshot of the config it answers; a lagging one lands after the next config.
  let sent = 0, sealed = true, lastSendAt = -Infinity;
  const hello = j(at(648_000));
  const effect = (hz: number) => {
    const asked = at(hz);
    const d = shouldForwardTune({ asked, serverDial: { frequency: hz, mode: 'am' }, seqAdvanced: false, sealed,
                                  helloTune: hello, askedJson: j(asked), msSinceSend: Date.now() - lastSendAt });
    if (d.unseal) sealed = false;
    if (d.send) { sent++; lastSendAt = Date.now(); }
  };
  for (const hz of walk) effect(hz);
  ok(sent === 0, `the Lenovo walk, adopted step by step: the phone sends ${sent} (it sent 10)`);
  // The OLD rule on the same walk, for the record: the seal opens on the first adopted value and never closes.
  let old = 0, oldSealed = true;
  for (const hz of walk) { const a = j(at(hz)); if (oldSealed) { if (a === hello) continue; oldSealed = false; } old++; }
  ok(old === walk.length, `…the old rule sent ${old} of ${walk.length} — the bug, reproduced`);
}

// ── 3. the web client, end to end: another listener's walk sends nothing ──
const g = globalThis as Record<string, unknown>;
g.window ??= globalThis;
g.location ??= { protocol: 'http:', host: 'x', search: '', href: 'http://x/' };
const tmp = mkdtempSync(path.join(os.tmpdir(), 'dial-echo-'));
await esbuild.build({ entryPoints: [fileURLToPath(new URL('../web/client/src/spectrum.ts', import.meta.url))],
  bundle: true, format: 'esm', platform: 'neutral', outfile: path.join(tmp, 'spectrum.mjs'), logLevel: 'error' });
const { SpectrumClient } = await import(pathToFileURL(path.join(tmp, 'spectrum.mjs')).href);
{
  const sent: Array<Record<string, unknown>> = [];
  let moved = 0;
  const sc = new SpectrumClient('ws://x/ws', { onDialMoved: () => { moved++; } } as never) as unknown as Record<string, any>;
  sc._send = (m: Record<string, unknown>) => { sent.push(m); };
  sc.frequency = 648_000; sc.mode = 'am'; sc.bandwidthLow = -5000; sc.bandwidthHigh = 5000;
  for (const k of [657, 666, 675, 684, 693]) {
    sc._handleText(JSON.stringify({ type: 'config', centerFreq: 648_000, binCount: 1024, binBandwidth: 2930,
      totalBandwidth: 3_000_000, maxBandwidth: 3_000_000, vfo: k * 1000, mode: 'am', locked: false, shared: true }));
  }
  ok(sc.frequency === 693_000 && moved === 5, `web: the readout followed every step (${sc.frequency / 1e3} kHz, ${moved} moves)`);
  const t = sent.filter((m) => m.type === 'tune' || m.type === 'mode' || m.type === 'bandwidth');
  ok(t.length === 0, `web: following sent NO tune, mode or bandwidth (${JSON.stringify(t).slice(0, 120) || 'nothing'})`);
}
rmSync(tmp, { recursive: true, force: true });

// ── 4. the wiring ──
const lap = src('src/components/LocalAudioPlayer.tsx');
const eff = lap.slice(lap.indexOf('const seqAdvanced'), lap.indexOf('}, [frequency, mode, bandwidthLow, bandwidthHigh, userTuneSeq]);'));
ok(/shouldForwardTune\(\{[^}]*serverDial/.test(eff), 'LocalAudioPlayer: the forwarding effect asks shouldForwardTune with serverDial');
ok(eff.indexOf('if (!d.send) return;') >= 0 && eff.indexOf('if (!d.send) return;') < eff.indexOf('sendLocalTune'),
   'LocalAudioPlayer: …before anything reaches sendLocalTune or the socket');
ok(/lastSendAt\.current = Date\.now\(\)/.test(eff), 'LocalAudioPlayer: stamps its own sends (the step-and-back window)');
const app = src('src/services/VibeServerWsClient.ts');
const sd = app.indexOf('this.status.serverDial = {');
ok(sd > 0 && sd < app.indexOf("noteDecision('vibe', `dial adopted"), 'app client: serverDial is written BEFORE the adopt emits (same snapshot)');
const conv = src('src/services/ConverterBackend.ts');
ok((conv.match(/serverDial: \{ \.\.\.s\.serverDial, frequency:/g) || []).length === 2, 'converter: both status maps carry serverDial in display Hz');
const scr = src('src/screens/SDRScreen.tsx');
ok(/serverDial=\{status\.serverDial[\s\S]{0,80}convToHardware\(status\.serverDial\.frequency/.test(scr),
   'screen: LocalAudioPlayer gets serverDial in HARDWARE Hz, like its frequency');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
