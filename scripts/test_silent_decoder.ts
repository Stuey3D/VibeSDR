/**
 * test_silent_decoder.ts — ★★ APRS AND METEOR LRPT ARE SILENT DECODERS (src/services/silentDecoder.ts), the one rule the
 * web page (web/client/src/main.ts silentDecoderSync) and the app (SDRScreen's silentMute effect) both run (2026-11-22).
 *
 * Stuart, 2026-11-22: "i guess for meteor lrpt the audio needs muting too" — Band's and APRS's rule: muted on the way in,
 * put back EXACTLY as it was on the way out (a listener who unmutes meanwhile may); with VFO B on only the VFO the decoder
 * runs on leaves the mix, and B coming or going moves the mute. The client's own mute — never a stopped stream.
 * Runs the rule itself (every path) and pins, in the sources, that both clients call it for activeDec 'lrpt' as for 'aprs'.
 * Run: npx tsx scripts/test_silent_decoder.ts
 */
import { readFileSync } from 'node:fs';
import { SilentMute, isSilentDecoder, silentMutedBanner, SILENT_DECODERS, type SilentState } from '../src/services/silentDecoder';
import type { VfoId } from '../src/services/vfoAB';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');

/** A fake client: the speaker and the server's per-VFO mix mutes, with a log of every call. */
function rig(startMuted: boolean | null = false) {
  const st = { spk: startMuted, mix: { A: false, B: false } as Record<VfoId, boolean>, log: [] as string[] };
  const io = {
    setSpeaker: (m: boolean) => { st.spk = m; st.log.push('spk:' + m); },
    setVfoMute: (v: VfoId, m: boolean) => { st.mix[v] = m; st.log.push(`${v}:${m}`); },
  };
  const sm = new SilentMute();
  const sync = (o: Partial<SilentState> & { dec?: string | null }) =>
    sm.sync({ open: isSilentDecoder(o.dec ?? null), abOn: !!o.abOn, vfo: o.vfo ?? 'A', speakerMuted: st.spk, reassert: o.reassert }, io);
  return { st, io, sm, sync };
}

// ── which decoders ──
ok(SILENT_DECODERS.length === 2 && isSilentDecoder('aprs') && isSilentDecoder('lrpt'), 'APRS and METEOR LRPT are silent');
ok(!isSilentDecoder('paging') && !isSilentDecoder('sstv') && !isSilentDecoder('rtty') && !isSilentDecoder(null) && !isSilentDecoder(''),
  'nothing else (PAGE has its own, unbreakable mute; SSTV, RTTY are listenable)');
ok(silentMutedBanner('lrpt') === '\u{1F507} AUDIO MUTED WHILE METEOR LRPT RUNS — TAP TO UNMUTE'
   && silentMutedBanner('aprs') === '\u{1F507} AUDIO MUTED WHILE APRS RUNS — TAP TO UNMUTE', 'the app banner names it and is a key');

// ── LRPT in and out, speaker unmuted before ──
{
  const { st, sm, sync } = rig(false);
  sync({ dec: 'lrpt' });
  ok(st.spk === true && sm.holdsSpeaker && sm.savedSpeaker === false, 'LRPT opens: speaker muted, "unmuted" saved');
  sync({ dec: 'lrpt' }); sync({ dec: 'lrpt' });
  ok(st.log.length === 1, 'a repeat sync (another decoder-button paint) does nothing');
  sync({ dec: null });
  ok(st.spk === false && !sm.holdsSpeaker && st.log.join() === 'spk:true,spk:false', 'LRPT closes: speaker back unmuted');
}
// ── speaker already muted before LRPT: stays muted after ──
{
  const { st, sync } = rig(true);
  sync({ dec: 'lrpt' }); sync({ dec: null });
  ok(st.spk === true, 'muted before LRPT → muted after');
}
// ── the listener unmutes during LRPT: it stands, and leaving puts back the state from BEFORE ──
{
  const { st, sm, sync } = rig(true);
  sync({ dec: 'lrpt' });
  st.spk = false; sm.listenerUnmuted();             // their speaker
  sync({ dec: 'lrpt' });
  ok(st.spk === false, 'web: an unmute during LRPT stands');
  sync({ dec: 'lrpt', reassert: true });
  ok(st.spk === false, 'app: …even with reassert on (the listener said so)');
  sync({ dec: null });
  ok(st.spk === true, 'leaving LRPT puts back exactly what it was before (muted)');
}
// ── the app: a reconnect brings the audio back unmuted — muted again; the web never reasserts ──
{
  const { st, sync } = rig(false);
  sync({ dec: 'lrpt', reassert: true });
  st.spk = false;                                   // every native start clears its mute
  sync({ dec: 'lrpt', reassert: true });
  ok(st.spk === true, 'app: unmuted by a reconnect (not the listener) → muted again');
  sync({ dec: null, reassert: true });
  ok(st.spk === false, '…and closing still puts back the original (unmuted)');
  const w = rig(false);
  w.sync({ dec: 'lrpt' }); w.st.spk = false; w.sync({ dec: 'lrpt' });
  ok(w.st.spk === false, 'web (no reassert): a sync never re-mutes behind the listener\'s back');
}
// ── no speaker yet: nothing saved, the next sync with a speaker latches ──
{
  const { st, sm, sync } = rig(null);
  sync({ dec: 'lrpt' });
  ok(!sm.holdsSpeaker && st.log.length === 0, 'no audio yet: nothing touched');
  st.spk = false; sync({ dec: 'lrpt' });
  ok(sm.holdsSpeaker && st.spk === true, 'audio arrives: muted then');
}
// ── APRS straight to LRPT (and back): the mute stays, the first saved state comes back at the end ──
{
  const { st, sm, sync } = rig(false);
  sync({ dec: 'aprs' }); sync({ dec: 'lrpt' });
  ok(st.spk === true && sm.savedSpeaker === false && st.log.length === 1, 'APRS → LRPT: stays muted, no flicker');
  sync({ dec: 'rtty' });
  ok(st.spk === false, '→ RTTY (listenable): speaker back as before APRS');
}
// ── ★★ VFO B on: LRPT mutes the VFO it decodes on, not the speaker ──
{
  const { st, sm, sync } = rig(false);
  sync({ dec: 'lrpt', abOn: true, vfo: 'A' });
  ok(st.spk === false && st.mix.A === true && st.mix.B === false && sm.holdsVfo === 'A', 'B on, LRPT on A: A leaves the mix, B and the speaker play');
  sync({ dec: null, abOn: true, vfo: 'A' });
  ok(st.mix.A === false && sm.holdsVfo === null && st.spk === false, 'LRPT closes: A back in the mix');
  sync({ dec: 'lrpt', abOn: true, vfo: 'B' });
  ok(st.mix.B === true && st.mix.A === false && st.spk === false, 'LRPT on B: B leaves the mix, not A');
  sync({ dec: 'lrpt', abOn: false, vfo: 'B' });
  ok(st.spk === true && sm.holdsVfo === null && sm.savedSpeaker === false && st.log[st.log.length - 1] === 'spk:true',
    'B turned off during LRPT: the mute moves to the speaker (no unmute sent to a VFO that is gone)');
  sync({ dec: 'lrpt', abOn: true, vfo: 'B' });
  ok(st.spk === false && st.mix.B === true, 'B back on: the speaker is put back and the mute moves to B');
  sync({ dec: null, abOn: true, vfo: 'B' });
  ok(st.spk === false && st.mix.B === false && st.mix.A === false, 'LRPT closes: everything as before');
}
// ── release (leaving the screen) puts the speaker back ──
{
  const { st, sm, io, sync } = rig(false);
  sync({ dec: 'lrpt' }); sm.release(io);
  ok(st.spk === false && !sm.holdsSpeaker, 'leaving the screen during LRPT: speaker as before');
}

// ── ★★★ both clients run THIS rule for LRPT as for APRS ──
const web = read('web/client/src/main.ts'), app = read('src/screens/SDRScreen.tsx');
const webFn = web.slice(web.indexOf('function silentDecoderSync()'), web.indexOf('function bandSpotsUi('));
ok(/from '\.\.\/\.\.\/\.\.\/src\/services\/silentDecoder\.ts'/.test(web), 'web imports the shared rule');
ok(/silentMute\.sync\(\{ open: isSilentDecoder\(activeDec\) && !!spec, abOn: !!spec\?\.abOn, vfo: decVfo, speakerMuted: audio \? audio\.muted : null \}/.test(webFn),
  'web: open = isSilentDecoder(activeDec) (LRPT from SATELLITE is activeDec too), on the decoding VFO, no reassert');
ok(/setSpeaker: \(m\) => \{ setMuted\(m\); updateMediaSession\(\); \}/.test(webFn) && !/setQuiet|\.close\(\)/.test(webFn),
  'web: the client\'s own mute (setMuted — the gain), never setQuiet or a closed stream');
ok(/decStep\('silentDecoderSync', silentDecoderSync\)/.test(web), 'web: every decoder change runs it (syncDecButtons)');
ok(/silentDecoderSync\(\);\s+\/\/ ★★ B came or went/.test(web.slice(web.indexOf('function abOnWatch('))), 'web: B coming or going runs it');
ok(!/aprsSilentSync|aprsMutedFrom|aprsMutedV/.test(web), 'web: no second (APRS-only) copy left');
// ★ the whole function (beta92 grew it: the pick's passband, the CW-beacon skimmer, the server's params)
const satF = web.slice(web.indexOf('function satFollow('), web.indexOf('\n}\n', web.indexOf('function satFollow(')));
ok(/activeDec = (?:d|satDecKey\(d\)[^;]*);/.test(satF) && /syncDecButtons\(\);/.test(satF), 'web: a SATELLITE pick sets activeDec and syncs (LRPT mutes from there)');
ok(/from '\.\.\/services\/silentDecoder'/.test(app), 'app imports the shared rule');
ok(/const silentOn = isSilentDecoder\(activeDecoder\);/.test(app)
   && /silentMute\.sync\(\{ open: silentOn, abOn, vfo: decVfo, speakerMuted: isMutedRef\.current, reassert: true \}/.test(app),
  'app: open = isSilentDecoder(activeDecoder), on decVfo, reasserted after a reconnect');
ok(/setVfoMute: \(v, m\) => \{ abClient\(\)\?\.abSetMute\?\.\(v, m\); \}/.test(app), 'app: VFO B on → the watch mute of that VFO');
ok(/\}, \[silentOn, abOn, decVfo, isMuted, abClient, silentMute\]\);/.test(app), 'app: re-run on the decoder, B, its VFO and every mute change');
ok(/if \(!e\.muted\) silentMute\.listenerUnmuted\(\);/.test(app), 'app: any unmute the listener makes (banner, lock screen, watch) stands');
ok(/const pagingOn = activeDecoder === 'paging';/.test(app), 'app: PAGE\'s unbreakable mute is PAGE\'s alone now');
ok(/isSilentDecoder\(activeDecoder\) && silentMute\.holdsSpeaker \? silentMutedBanner\(activeDecoder\)/.test(app), 'app: the banner names APRS / METEOR LRPT and is the tap-to-unmute key');
const about = read('src/components/AboutOverlay.tsx');
ok(/METEOR LRPT MUTES THE SPEAKER/.test(about) && /comes back exactly as it was/.test(about), 'About: the beta91 note');
ok(!/\b(color|center|analyze|behavior)\b/i.test(read('src/services/silentDecoder.ts').replace(/^import.*$/gm, '')), 'UK English');

console.log(`silent decoders (APRS, METEOR LRPT): ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
