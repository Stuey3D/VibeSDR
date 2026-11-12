/**
 * GOLDEN DATA for the handheld Advanced RDS port's two shared formatters (handheld-ui/src/adv_rds_core.h): the APP's
 * own src/services/lrMeter.ts and src/services/mpxPower.ts, unmodified, over a sweep of inputs. adv_rds_test.cpp
 * replays them through the C++ and must print the same. Strings are written raw (UTF-8), '|' separated.
 *   L have lDev rDev seen | show lpct ltext ltone rpct rtext rtone     lrDisplay (have=0: the fields absent)
 *   P db secs | value settling                                         mpxPowerParts
 * Run: node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_ards_golden.ts <outFile>
 */
import { writeFileSync } from 'node:fs';
import { lrDisplay } from '../../src/services/lrMeter.ts';
import { mpxPowerParts } from '../../src/services/mpxPower.ts';

const out: string[] = [];
const devs = [-1, 0, 0.4, 12, 67.5, 74.9, 75, 75.4, 75.6, 80, 82, 82.4, 83, 99, 100, 140];
for (const seen of [false, true]) {
  out.push(`L|0|0|0|${seen ? 1 : 0}|` + fmtLr(lrDisplay(undefined, undefined, seen)));
  for (const l of devs) for (const r of [devs[(devs.indexOf(l) + 3) % devs.length]])
    out.push(`L|1|${l}|${r}|${seen ? 1 : 0}|` + fmtLr(lrDisplay(l, r, seen)));
}
function fmtLr(x: ReturnType<typeof lrDisplay>): string {
  if (!x) return '0';
  return ['1', x.l.pct, x.l.text, x.l.tone, x.r.pct, x.r.text, x.r.tone].join('|');
}
for (const db of [-12.34, -0.4, -0.05, 0, 0.04, 2.25, 6.6, 12])
  for (const secs of [0, 0.4, 1, 15.6, 44, 59.4, 59.6, 60, 61])
    { const p = mpxPowerParts(db, secs); out.push(`P|${db}|${secs}|${p.value}|${p.settling}`); }
const p = mpxPowerParts(NaN, 30); out.push(`P|NaN|30|${p.value}|${p.settling}`);
writeFileSync(process.argv[2], out.join('\n') + '\n');
