/**
 * THE SERVING PHONE'S WATERFALL RATE TIERS (src/services/fpsTiers.ts) — the 10-fps default (Stuart, 2026-10-20).
 * Labels renamed Full/Half/Quarter → Smoother · 20 / Default · 10 / Half · 5; the STORED keys are unchanged, so an
 * owner's saved 'full' still serves 20 and a new server starts on Default · 10.
 * Run: node --no-warnings scripts/test_fps_tiers.ts
 */
import { FPS_TIERS, DEFAULT_FPS_TIER, fpsForTier } from '../src/services/fpsTiers.ts';
let fails = 0, passes = 0;
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.log(`  FAIL ${what}`); } };
ok('three tiers, in order 20 / 10 / 5', JSON.stringify(FPS_TIERS.map(t => t.fps)) === '[20,10,5]');
ok("'full' → Smoother · 20 fps, serving 20", FPS_TIERS[0].key === 'full' && FPS_TIERS[0].label === 'Smoother · 20 fps' && fpsForTier('full') === 20);
ok("'half' → Default · 10 fps, serving 10", FPS_TIERS[1].key === 'half' && FPS_TIERS[1].label === 'Default · 10 fps' && fpsForTier('half') === 10);
ok("'quarter' → Half · 5 fps, serving 5", FPS_TIERS[2].key === 'quarter' && FPS_TIERS[2].label === 'Half · 5 fps' && fpsForTier('quarter') === 5);
ok('a new server starts on Default · 10', DEFAULT_FPS_TIER === 'half' && fpsForTier(DEFAULT_FPS_TIER) === 10);
ok('an unknown stored key falls back to the default 10', fpsForTier('bogus' as never) === 10);
ok('no label says Full or Quarter any more', !FPS_TIERS.some(t => /Full|Quarter/.test(t.label)));
console.log(`fps tiers: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
