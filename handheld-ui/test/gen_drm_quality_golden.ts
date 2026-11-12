/**
 * The R36S's DRM meter (src/drm_quality.h) against the app's (src/utils/drmQuality.ts): scripted `drm` report sequences
 * run through the TS meter; every push and what it said is written as one JSON line for drm_quality_test.cpp to replay
 * through the port. Run by run_drm_quality_test.sh.
 *   node --no-warnings handheld-ui/test/gen_drm_quality_golden.ts <out.jsonl>
 */
import { writeFileSync } from 'node:fs';
import { DrmQualityMeter, drmBoxReading, drmQualityLine, type DrmQualityReport } from '../../src/utils/drmQuality.ts';

const out: string[] = [];
function run(name: string, reports: Array<[number, DrmQualityReport]>) {
  const m = new DrmQualityMeter();
  for (const [t, r] of reports) {
    const q = m.push(r, t);
    out.push(JSON.stringify({ name, t, r, level: q.level, label: q.label, short: q.short, advice: q.advice, detail: q.detail ?? '',
                              box: drmBoxReading(q), line: drmQualityLine(q) }));
  }
}
const L = (o: DrmQualityReport): DrmQualityReport => ({ sync: true, locked: true, ...o });
// Stuart's Music4Joy: decoder 17–20 dB, xHE-AAC playing clean — then a fade, then the lock goes
{
  const seq: Array<[number, DrmQualityReport]> = [[0, { sync: false, locked: false }], [1200, { sync: true, locked: false, snr: 3 }]];
  let t = 1200, u = 0, f = 0;
  for (let i = 0; i < 8; i++) { u += 10; f += 3; t += 1200; seq.push([t, L({ msc: true, snr: 17 + (i % 4), mscMer: 16, mscQam: 16, facOk: f, facBad: 0, audioUnits: u, audioBad: 0, codec: 'xHE-AAC', playable: true })]); }
  for (let i = 1; i <= 4; i++) seq.push([t += 1200, L({ snr: 9, mscMer: 6, mscQam: 16, facOk: f + 3 * i, facBad: 0, audioUnits: u + 5 * i, audioBad: 5 * i, codec: 'xHE-AAC', playable: true })]);
  seq.push([t += 1200, { sync: false, locked: false, facOk: f + 12, facBad: 16 }]);
  run('music4joy', seq);
}
// 64-QAM marginal, no audio counters (an older server's report), then a new receiver
{
  const seq: Array<[number, DrmQualityReport]> = [];
  for (let i = 1; i <= 6; i++) seq.push([i * 1200, L({ snr: 12.3, mscMer: 11.25, mscQam: 64, facOk: 3 * i, facBad: i > 3 ? 1 : 0 })]);
  seq.push([8400, L({ snr: 12.3, mscMer: 9.5, mscQam: 64, facOk: 1, facBad: 0 })]);
  for (let i = 1; i <= 4; i++) seq.push([8400 + i * 1200, L({ snr: 8, mscMer: 9.5, mscQam: 64, facOk: 1 + 2 * i, facBad: i })]);
  run('older-64qam', seq);
}
// an AAC station on a server that cannot play it
{
  const seq: Array<[number, DrmQualityReport]> = [];
  for (let i = 1; i <= 5; i++) seq.push([i * 1200, L({ snr: 18.4, mscMer: 12.6, mscQam: 16, facOk: 3 * i, facBad: 0, codec: 'AAC', playable: false })]);
  run('unplayable', seq);
}
// heavy audio loss, recovering (hysteresis up takes three)
{
  const seq: Array<[number, DrmQualityReport]> = [];
  let u = 0, b = 0;
  for (let i = 1; i <= 10; i++) { u += 10; b += i < 5 ? 3 : 0; seq.push([i * 1200, L({ snr: 14.5, mscMer: 9, mscQam: 16, facOk: 3 * i, facBad: 0, audioUnits: u, audioBad: b, codec: 'AAC', playable: true })]); }
  run('loss-recovery', seq);
}
writeFileSync(process.argv[2], out.join('\n') + '\n');
console.log(`gen_drm_quality_golden: ${out.length} pushes`);
