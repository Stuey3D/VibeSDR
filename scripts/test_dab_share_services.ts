// DAB services structural sharing (src/services/dabShareServices.ts)
// run: node --no-warnings scripts/test_dab_share_services.ts
import { shareDabServices } from '../src/services/dabShareServices.ts';
let fails = 0, passes = 0;
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.log(`✗ ${what}`); } };

type S = { sid: number; label: string; codec: string; subch: number; pi?: number[]; fm?: number[]; dls?: string };
const mk = (): S[] => [
  { sid: 1, label: 'Radio A', codec: 'AAC', subch: 1, pi: [0xc201], fm: [98.1] },
  { sid: 2, label: 'Radio B', codec: 'MP2', subch: 2 },
];
const a = mk(), b = mk();
ok('identical list keeps the old array', shareDabServices(a, b) === a);
ok('no previous list -> next', shareDabServices(undefined, b) === b);
ok('empty previous list -> next', shareDabServices([], b) === b);

const c = mk(); c[1] = { ...c[1], label: 'Radio B2' };
const r = shareDabServices(a, c);
ok('a changed service gives a new array', r !== a && r !== c);
ok('the unchanged service keeps its object', r[0] === a[0]);
ok('the changed service is the new one', r[1] === c[1] && r[1].label === 'Radio B2');

const d = mk(); d[0] = { ...d[0], pi: [0xc202] };
ok('a changed nested array is a change', shareDabServices(a, d)[0] === d[0]);

const e = [mk()[1], mk()[0]];
const re = shareDabServices(a, e);
ok('reordered: new array in the new order', re !== a && re[0] === a[1] && re[1] === a[0]);

const f = [...mk(), { sid: 3, label: 'Radio C', codec: 'AAC', subch: 3 }];
const rf = shareDabServices(a, f);
ok('added service: new array, old objects kept', rf !== a && rf[0] === a[0] && rf[1] === a[1] && rf[2] === f[2]);

const g = [{ ...mk()[0], dls: 'Now playing' }, mk()[1]];
ok('an added field is a change', shareDabServices(a, g)[0] === g[0]);

const h = mk().slice(0, 1);
ok('a removed service gives a new array', shareDabServices(a, h) !== a && shareDabServices(a, h)[0] === a[0]);

console.log(`dab services sharing: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
