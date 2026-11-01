/**
 * test-web-mem-budget.mjs — the web client's memory classes and history budgets (memBudget.ts).
 *
 *   node --no-warnings scripts/test-web-mem-budget.mjs
 *
 * Pure logic, no browser (and so silent). A 1 GB Pi or a 512 MB handheld must get a SMALL waterfall history; an
 * unknown browser on a desktop-sized screen keeps today's; the ring never drops below the display.
 */
import { memClassOf, HISTORY_BUDGET, ringRowsFor } from '../web/client/src/memBudget.ts';

let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

ok(memClassOf({ deviceMemory: 0.5 }) === 'tiny', '512 MB board → tiny');
ok(memClassOf({ deviceMemory: 1, screenW: 1920, screenH: 1080 }) === 'small', '1 GB Pi on a 1080p desktop → small (memory wins over screen)');
ok(memClassOf({ deviceMemory: 2 }) === 'medium', '2 GB → medium');
ok(memClassOf({ deviceMemory: 8 }) === 'full', '8 GB → full');
ok(memClassOf({ screenW: 640, screenH: 480 }) === 'small', 'unknown memory, 640×480 handheld → small');
ok(memClassOf({ screenW: 720, screenH: 720 }) === 'small', 'unknown memory, 720×720 → small');
ok(memClassOf({ screenW: 544, screenH: 960 }) === 'small', 'unknown memory, 960×544 portrait → small');
ok(memClassOf({ screenW: 1920, screenH: 1080 }) === 'full', 'unknown memory, 1080p → full (unknown is never a reason to cut)');
ok(memClassOf({}) === 'full', 'nothing known → full');

const ROW = 1024 + 1024 + 48;
ok(ringRowsFor(ROW, 900, 2560, HISTORY_BUDGET.full) === 2560, 'full: the old 2560-row ring, unchanged');
const small = ringRowsFor(ROW, 300, 2560, HISTORY_BUDGET.small);
ok(small >= 900 && small <= 1000 && small * ROW <= HISTORY_BUDGET.small, `small: ${small} rows within 2 MB`);
ok(ringRowsFor(ROW, 1400, 2560, HISTORY_BUDGET.tiny) === 1400, 'never fewer rows than the display, whatever the budget');

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
