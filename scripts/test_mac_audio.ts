/**
 * MAC VOLUME + MUTE — the iPad app on a Mac (src/constants/macAudio.ts, src/services/macAudio.ts,
 * VibePowerModule.setMacOutputGain, the AUDIO popup and the deck's speaker key).
 *
 * Stuart, 2026-10-01: on a Mac the app has no per-app volume, so give it one — ONLY on a Mac ("a volume
 * control separate from the system volume could end up looking like a broken app if forgotten about"),
 * and show a mute on the speaker key as a prohibition sign in the legend colour ("Cannot use red").
 *
 * Proves:
 *   • shown ONLY on iOS-on-a-Mac — never Android, web, an iPhone or iPad, or an old binary;
 *   • the gain: unity off a Mac whatever is stored, 0 when muted, a cubic taper otherwise;
 *   • MUTE and the fader: moving the fader unmutes, unmuting a zeroed fader is audible, the muted
 *     legend shows for MUTE and for a fader left at zero;
 *   • stored values are checked (garbage falls back to today's full volume, unmuted);
 *   • the wiring: every native audio ingress ends in scheduleOut(), which plays only on the player wired
 *     to the main mixer that carries the gain; each new engine starts at the gain; recordings are taken
 *     BEFORE it; the method is exported to JS; the popup row is gated on the Mac; both deck keys draw the
 *     muted legend in the legend colour (no colour of its own) and the recording pulse is untouched;
 *   • the splash says SERVER LIST, not INSTANCE LIST.
 *
 * Run: node --no-warnings scripts/test_mac_audio.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  MAC_AUDIO_DEFAULT, MAC_UNMUTE_FLOOR, clampVolume, macOutputGain, macSilenced, macVolumeLabel,
  parseMacAudio, setMacVolume, showMacAudio, toggleMacMute,
} from '../src/constants/macAudio.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (what: string, got: number, want: number) => ok(`${what} (${got} ≈ ${want})`, Math.abs(got - want) < 1e-9);

// ── Where it shows ───────────────────────────────────────────────────────────────────────────────
eq('iOS on a Mac → shown', showMacAudio('ios', true), true);
eq('iPhone / iPad → hidden', showMacAudio('ios', false), false);
eq('Android (even if something claimed Mac) → hidden', showMacAudio('android', true), false);
eq('web → hidden', showMacAudio('web', true), false);
eq('an old binary (no getter → isMac undefined) → hidden', showMacAudio('ios', undefined as unknown as boolean), false);

// ── The gain ─────────────────────────────────────────────────────────────────────────────────────
eq('default is today: full, unmuted', MAC_AUDIO_DEFAULT, { volume: 1, muted: false });
eq('off a Mac the gain is unity whatever is stored', macOutputGain({ volume: 0.1, muted: true }, false), 1);
eq('muted → 0', macOutputGain({ volume: 0.8, muted: true }, true), 0);
eq('full → 1', macOutputGain({ volume: 1, muted: false }, true), 1);
near('half way is the cubic taper (≈ -18 dB)', macOutputGain({ volume: 0.5, muted: false }, true), 0.125);
eq('zero → silent', macOutputGain({ volume: 0, muted: false }, true), 0);
eq('out of range is clamped high', macOutputGain({ volume: 7, muted: false }, true), 1);
eq('out of range is clamped low', macOutputGain({ volume: -3, muted: false }, true), 0);
ok('the taper is monotonic', [0, .1, .2, .4, .6, .8, 1].every((v, i, a) => i === 0 ||
  macOutputGain({ volume: v, muted: false }, true) > macOutputGain({ volume: a[i - 1], muted: false }, true)));

// ── MUTE and the fader ───────────────────────────────────────────────────────────────────────────
eq('MUTE mutes and keeps the level', toggleMacMute({ volume: 0.6, muted: false }), { volume: 0.6, muted: true });
eq('MUTE again restores the level', toggleMacMute({ volume: 0.6, muted: true }), { volume: 0.6, muted: false });
eq('unmuting a fader at zero is audible', toggleMacMute({ volume: 0, muted: true }), { volume: MAC_UNMUTE_FLOOR, muted: false });
eq('MUTE on a zeroed, unmuted fader brings it up (it was already silent)', toggleMacMute({ volume: 0, muted: false }),
   { volume: MAC_UNMUTE_FLOOR, muted: false });
eq('moving the fader unmutes', setMacVolume({ volume: 0.2, muted: true }, 0.4), { volume: 0.4, muted: false });
eq('the fader clamps', setMacVolume(MAC_AUDIO_DEFAULT, 1.5), { volume: 1, muted: false });
eq('muted legend: MUTE', macSilenced({ volume: 0.5, muted: true }, true), true);
eq('muted legend: a fader left at zero', macSilenced({ volume: 0, muted: false }, true), true);
eq('no muted legend while playing', macSilenced({ volume: 0.5, muted: false }, true), false);
eq('never a muted legend off a Mac', macSilenced({ volume: 0, muted: true }, false), false);
eq('readout', macVolumeLabel({ volume: 0.426, muted: false }), '43%');
eq('readout muted', macVolumeLabel({ volume: 0.426, muted: true }), 'MUTED');

// ── Stored values ────────────────────────────────────────────────────────────────────────────────
eq('round trip', parseMacAudio(JSON.stringify({ volume: 0.3, muted: true })), { volume: 0.3, muted: true });
eq('garbage → default', parseMacAudio('{not json'), MAC_AUDIO_DEFAULT);
eq('null → default', parseMacAudio(null), MAC_AUDIO_DEFAULT);
eq('a string volume → default volume', parseMacAudio({ volume: '0.2', muted: 'yes' }), MAC_AUDIO_DEFAULT);
eq('NaN → default volume', parseMacAudio({ volume: NaN }), MAC_AUDIO_DEFAULT);
eq('clampVolume(undefined) → full', clampVolume(undefined), 1);

// ── The wiring ───────────────────────────────────────────────────────────────────────────────────
const swift = readFileSync('ios/VibeSDR/VibePowerModule.swift', 'utf8');
const objc  = readFileSync('ios/VibeSDR/VibePowerModule.m', 'utf8');
const strip = (s: string) => s.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
const sw = strip(swift);
const fnBody = (name: string) => {
  const i = sw.indexOf(`func ${name}(`);
  if (i < 0) return '';
  let depth = 0, j = sw.indexOf('{', i);
  for (let k = j; k < sw.length; k++) {
    if (sw[k] === '{') depth++;
    else if (sw[k] === '}' && --depth === 0) return sw.slice(j, k + 1);
  }
  return '';
};
eq('scheduleBuffer is called only inside scheduleOut (one player, one mixer)',
   (sw.match(/\.scheduleBuffer\(/g) || []).length, (fnBody('scheduleOut').match(/\.scheduleBuffer\(/g) || []).length);
ok('scheduleOut plays on playerNode', /guard let player = playerNode/.test(fnBody('scheduleOut')));
for (const ingress of ['handlePacket', 'playExternalBuffer', 'playFmdxPcm']) {
  const b = fnBody(ingress);
  ok(`${ingress} ends in scheduleOut`, /scheduleOut\(/.test(b));
  ok(`${ingress} records BEFORE the gain (recordings are pre-volume)`,
     b.indexOf('writeRecording(') >= 0 && b.indexOf('writeRecording(') < b.indexOf('scheduleOut('));
}
ok('external PCM and Opus both feed playExternalBuffer',
   /playExternalBuffer\(/.test(fnBody('feedExternalPcm')) && /playExternalBuffer\(/.test(fnBody('feedExternalOpus')));
const se = fnBody('startEngine');
ok('startEngine wires the player to the MAIN MIXER', /engine\.connect\(player, to: engine\.mainMixerNode/.test(se));
// ★ …unless a page we opened is playing (the Signal Identification Wiki, 2026-10-09): then a rebuilt engine stays silent
//   until that clip ends and pageUnduck fades it back to the Mac gain.
ok('every new engine starts at the Mac gain (silent only while a page we opened plays)',
   /engine\.mainMixerNode\.outputVolume = pageDucked \? 0 : macOutputGain/.test(se));
eq('AVAudioEngine() is created only in startEngine (and the silent keep-alive, which is not ours)',
   (sw.match(/AVAudioEngine\(\)/g) || []).length, 1);
const sg = fnBody('setMacOutputGain');
ok('setMacOutputGain sets the main mixer', /mainMixerNode\.outputVolume = /.test(sg));
ok('setMacOutputGain is unity off a Mac', /runsOnMac \?[^:]+: 1/.test(sg) && /isiOSAppOnMac/.test(sw));
ok('setMacOutputGain is exported to JS', /RCT_EXTERN_METHOD\(setMacOutputGain:\(nonnull NSNumber \*\)gain\)/.test(objc));
ok('setMacOutputGain never stops or pauses anything (mute is gain 0)', !/\.(stop|pause)\(\)/.test(sg));

const svc = readFileSync('src/services/macAudio.ts', 'utf8');
ok('the service pushes the gain only on a Mac', /function pushGain\(\): void \{\s*if \(!isMacHost\(\)\) return;/.test(svc));
ok('the service reads the existing deviceClass getter', /readNativeDeviceClass\(\)/.test(svc) && /\.isMac/.test(svc));
ok('persisted in AsyncStorage under the per-device key', /AsyncStorage\.setItem\(MAC_AUDIO_STORAGE_KEY/.test(svc));
ok('App.tsx loads it at launch', /initMacAudio\(\)/.test(readFileSync('App.tsx', 'utf8')));

const sheet = readFileSync('src/components/AudioSheet.tsx', 'utf8');
ok('the AUDIO popup draws VOLUME + MUTE only on a Mac', /\{mac\.onMac && \(\s*<View style=\{st\.bwRow\}>\s*<Text[^>]*>VOLUME<\/Text>/.test(sheet));
ok('…as the popup\'s own fader (NavSlider → PopupFader on silver / black)', /VOLUME<\/Text>\s*<NavSlider/.test(sheet));
ok('…with a MUTE key', /<Toggle label="MUTE" on=\{mac\.state\.muted\} onPress=\{toggleMacAudioMute\}/.test(sheet));

const bar = readFileSync('src/components/ControlsBar.tsx', 'utf8');
const mutedStrokes = /const AUDIO_MUTED_STROKES: IconStroke\[\] = \[([\s\S]*?)\];/.exec(bar)?.[1] ?? '';
ok('the muted legend is ring + slash + the speaker', /MUTE_RING/.test(mutedStrokes) && /MUTE_SLASH/.test(mutedStrokes)
   && /MUTE_BODY/.test(mutedStrokes) && /MUTE_W1/.test(mutedStrokes) && /MUTE_W2/.test(mutedStrokes));
ok('…in the LEGEND colour — no stroke carries a colour of its own (never red)', !/color\s*:/.test(mutedStrokes));
ok('the slash runs top-left to bottom-right', /MakeFromSVGString\('M3\.707 3\.707L16\.293 16\.293'\)/.test(bar));
eq('both deck speaker keys draw AudioKeyLegend with the Mac mute',
   (bar.match(/<AudioKeyLegend size=\{ICON_SZ\} progress=\{p\} asRecord=\{audioAsRecord\} muted=\{macMuted\} \/>/g) || []).length, 2);
eq('both bars read the Mac mute', (bar.match(/const macMuted = useMacSilenced\(\);/g) || []).length, 2);
ok('the recording pulse overlay on the speaker key is untouched',
   /borderColor: ct\.keyPulseRec, opacity: recPulse \}\]\} \/>\}>\s*\{p => <AudioKeyLegend/.test(bar));
ok('the recording pulse still breathes on isRecording alone', /if \(isRecording\) \{\s*const a = Animated\.loop/.test(bar));

const app = readFileSync('App.tsx', 'utf8');
ok('the splash says SERVER LIST', /useState\('CONNECTING TO SERVER LIST'\)/.test(app) && !/INSTANCE LIST/.test(app));

console.log(`${fails ? '✗' : '✓'} mac audio: ${passes} checks passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
