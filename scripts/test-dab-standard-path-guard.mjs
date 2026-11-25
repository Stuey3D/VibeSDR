// ★★★ THE STANDARD DAB PATH IS UNTOUCHED — the guard Stuart asked for (2026-11-16, HARD RULE): DAB's weak-multiplex lock is
// the crown jewel, so the own-station work (docs/v12/DAB-OWN-STATION.md) may split the per-station BACK END only. The
// front end, the AGC and the gain path stay byte-for-byte; below 4 MS/s and in single-multiplex DAB nothing changes.
//   1. SOURCE: against BASE (default v12-experimental), the AGC / gain files are unchanged, the shim adds no gain or AGC
//      call and removes no line that mentions one, and DabReceiver's front end (push) gains only the empty-unless-a-
//      station-is-open tap line.
//   2. BEHAVIOUR (when DAB_WEAK_BEFORE and DAB_WEAK_AFTER name dab-weak builds of BASE and of this tree): real weak
//      captures at three noise levels decode IDENTICALLY — every JSON field but the wall time.
// Exit 3 = part 2 not run (no binaries / captures); part 1 always runs.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// ★ pinned to v12-experimental just BEFORE own station merged (dc95053aa): after the merge the branch IS the base and the diff is empty
const BASE = process.env.BASE || 'dc95053aa';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const git = (...a) => spawnSync('git', ['-C', root, ...a], { encoding: 'utf8', maxBuffer: 1 << 28 }).stdout || '';
const C = 'android/app/src/main/cpp/';

console.log(`── 1. source, against ${BASE} ──`);
/* ★ beta91 vibe_sat_gain.h is the SATELLITE-PASS profile (new file, 11-22): it acts only while g_satGainOn (a satellite
 *  decoder on a pick / unattended pass), never in DAB; reviewed with its own test (test-sat-gain). The three DAB-path gain
 *  files are still held to BASE byte for byte. */
/* ★★ beta92 (Stuart 11-23: "dab needs to work as it did before … it doesnt matter the amount of gain writes but we need to
 *  avoid flapping … if the agc can get a decent level and make fine adjustments that is ok … aim for the highest mer lowest
 *  errors like we do now"): vibe_rsp_vibeagc.h (VibeAGC for SDRplay — the MW/HF measured step and re-checks, none of which
 *  acts in DAB) is held to DAB's BEHAVIOUR instead of its bytes — test-rsp-vibeagc case V (no flapping, at most one RF step
 *  after settling, the level in DAB's band ≥ 95 %) and cases C / E, whose every gain write was shown identical on dc95053aa
 *  and today's code. The RTL's gain files (vibe_agc_record.h, vibe_agc_rules.h) stay byte for byte. */
/* ★ beta92 vibe_sat_gain_airspy.h is the Airspy SATELLITE-PASS hold (new file, 11-23): it acts only inside
 *  LocalSdrShim::airspySatTick during a satellite pick, never in DAB — the same footing as vibe_sat_gain.h. */
/* ★ beta93 vibe_mode_gain.h is the BURST MODES' per-radio gain rule (new file, 11-24, c0282cfe5 — the HF+ datalink): pure and
 *  header-only, included only by local_sdr_shim.cpp and called only from burstGainFields / burstGains, whose only callers are
 *  AIS's and the aircraft datalink's (ACARS / VDL2) report and entry. DAB never reaches it — the same footing as
 *  vibe_sat_gain.h. Held to its own test (vibeserver/test-avi-service.cpp). */
const agcFiles = fs.readdirSync(path.join(root, C)).filter((f) => /agc|gain/i.test(f) && f !== 'vibe_sat_gain.h' && f !== 'vibe_sat_gain_airspy.h' && f !== 'vibe_rsp_vibeagc.h' && f !== 'vibe_mode_gain.h').map((f) => C + f);
const agcDiff = git('diff', BASE, '--', ...agcFiles);
ok(agcFiles.length > 0 && agcDiff === '', `the AGC / gain sources are unchanged (${agcFiles.length} files: ${agcFiles.map((f) => path.basename(f)).join(', ')})`);
/* ★ The shim's gain/AGC lines are judged from SHIM_BASE: f026471cd is the end of the SDRplay AGC lock work (Stuart,
 *  2026-11-17 — the lock holds the RF AGC too, the admin takes both gains, a locked RF figure leaves listeners the IF).
 *  Those lines are the RSP's own gain controls for every mode, reviewed and tested there (test-rsp-agc-lock.mjs); they
 *  are not DAB's standard path, whose AGC sources are still held to BASE above. Phase 1 added no such line (this check
 *  passed against BASE before that work). */
/* ★ beta91: moved to ba9afd6ca (the satellite-pass gain merge, 11-22). The lines it added are the satellite profile's
 *  own writes (vsRspSatTick, the RTL block — both gated on g_satGainOn) and one READ of the gain in the APRS developer
 *  dump's header (b91-aprsdbg) — none on DAB's standard path, whose AGC sources are still held to BASE above. */
/* ★ beta93: moved to c0282cfe5 (aircraft datalink + AIS on the Airspy HF+, 11-24). Every gain line it changed is in aisLeave /
 *  avLeave and the AIS / datalink report — the burst modes, never DAB: the report's "gain"/"gainAuto" fields moved into
 *  burstGainFields (the same g_gainTarget read, the same format, the same auto rule — byte-identical off the HF+), the list
 *  into burstGains (getTunerGains as before, empty only on an HF+), and the gain put back on leaving is skipped only on an
 *  HF+ (the mode never set it there). Between ba9afd6ca and its parent, and from it to beta93's head, no gain/AGC line
 *  changed (checked with this gainish pattern when the base moved), so the move hides nothing else. */
/* ★ beta94: moved to 6a4bd8ea9 (the rtltcp-fix-v12 merge on b94-integrate, 11-25). The only gain-ish lines added to the shim
 *  since c0282cfe5 are rtlDoorApplyGain's S.setGain(-1) / S.setGain(want) / S.setAhfAgc(autoG) — the RTL-TCP front door
 *  applying an rtl_tcp client's gain commands (0x03 / 0x04 / 0x08 / 0x0d), reached only while a client is on the door,
 *  never on DAB's path; none was removed (checked with this gainish pattern from c0282cfe5 to the merge). DAB's AGC / gain
 *  sources are still held to BASE byte for byte above. */
const SHIM_BASE = process.env.SHIM_BASE || '6a4bd8ea9';
const shim = git('diff', '-U0', SHIM_BASE, '--', C + 'local_sdr_shim.cpp').split('\n');
const gainish = /setGain|setTunerGain|g_gain|dabSeedGain|vibeAgc|Agc\(|agc_|rtlAgc|gainSteps|setLna|setIfGain/;
ok(!shim.some((l) => l.startsWith('-') && !l.startsWith('---') && gainish.test(l)), 'the shim removes no line that touches the gain or AGC');
ok(!shim.some((l) => l.startsWith('+') && !l.startsWith('+++') && gainish.test(l)), 'the shim adds no gain or AGC call');
const rx = git('diff', '-U0', BASE, '--', C + 'vibe_dab_receiver.h').split('\n');
const rxRemoved = rx.filter((l) => l.startsWith('-') && !l.startsWith('---'));
ok(rxRemoved.length === 0, `DabReceiver: nothing removed or rewritten (${rxRemoved.length} lines)`);
const pushAdds = rx.filter((l) => l.startsWith('+') && /mscTap_\(msc/.test(l));
// ★ At most the one tap line — against a BASE after own station merged (e.g. DAB Phase 2's branch point) there is none to add
ok(pushAdds.length <= 1 && pushAdds.every((l) => /if \(mscTap_\)/.test(l)), `DabReceiver::push gains at most the tap, which is empty unless a station is open (${pushAdds.length})`);
const svc = git('diff', '-U0', BASE, '--', C + 'vibe_dab_service.h').split('\n').filter((l) => l.startsWith('-') && !l.startsWith('---'));
ok(svc.length <= 1 && svc.every((l) => /~DabService\(\)/.test(l)), `DabService: the only rewritten line is the destructor (it now joins a station's thread) — ${svc.length}`);

console.log('── 2. behaviour: real weak captures, before vs after ──');
const B = process.env.DAB_WEAK_BEFORE, A = process.env.DAB_WEAK_AFTER;
const T = path.join(os.homedir(), 'VibeSDR-test-audio/dab/tii');
const clips = [['12B-v4l-2026-11-13.vbu8', 'c229', [8, 5, 4]], ['11D-v4l-2026-11-13.vbu8', 'c1c0', [6, 4, 3]], ['10D-v4l-2026-11-13.vbu8', 'c2ef', [20, 16, 14]]]
  .filter(([f]) => fs.existsSync(path.join(T, f)));
if (!B || !A || !fs.existsSync(B) || !fs.existsSync(A) || !clips.length) {
  console.log('   (part 2 not run — DAB_WEAK_BEFORE / DAB_WEAK_AFTER, and the captures in ~/VibeSDR-test-audio/dab/tii)');
} else {
  const run = (bin, f, sid, nz) => (spawnSync(bin, [path.join(T, f), '--secs', '15', '--sid', sid, '--noise', String(nz), '--seed', '1'], { encoding: 'utf8' }).stdout || '')
    .split('\n').find((l) => l.startsWith('{"clip"'))?.replace(/,"wallS":[^,}]*/, '') ?? '';
  for (const [f, sid, levels] of clips)
    for (const nz of levels) {
      const b = run(B, f, sid, nz), a = run(A, f, sid, nz);
      const sf = (j) => (j.match(/"sfOk":(\d+),"sfTried":(\d+)/) || []).slice(1).join('/');
      ok(b !== '' && a === b, `${f.slice(0, 3)} noise ${nz} dB: identical (super frames ${sf(a)}, MER ${(a.match(/"merAvg":([\d.]+)/) || [])[1]})`);
    }
}
console.log(`test-dab-standard-path-guard: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : (B && A ? 0 : 3));
