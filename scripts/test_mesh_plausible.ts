// test_mesh_plausible.ts — ★★ V12 (Stuart 11-27) MESH NODES WITH IMPLAUSIBLE SELF-REPORTED LOCATIONS
// (src/utils/meshPlausible.ts): a claim more than 300 km from the route's FIRST PLACED hop (+250 km for each unplaced hop
// before it) is suspect; heard first-hand = against the receiver; no placed hop = no verdict. A suspect never counts as a
// furthest (meshRoute.ts meshFurthest).   npx tsx scripts/test_mesh_plausible.ts
import { meshSuspect, meshSuspectWords, MESH_SUSPECT_KM, MESH_SUSPECT_HOP_KM } from '../src/utils/meshPlausible';
import { meshFurthest, type MeshKnown } from '../src/utils/meshRoute';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const rx = { lat: 52.20, lon: -0.90 };   // a receiver in the East Midlands
const known: MeshKnown[] = [
  { id: 'aa11', name: 'FSM Repeater IP28', lat: 52.34, lon: 0.51 },   // Suffolk
  { id: 'c4d2', name: 'Lyneham RPTR2', lat: 51.50, lon: -1.96 },      // Wiltshire
  { id: 'b7e0', name: 'Rye Repeater', lat: 50.95, lon: 0.73 },        // East Sussex, the Channel coast
  { id: 'dd01', name: 'Twin A', lat: 52.0, lon: -1.0 }, { id: 'dd02', name: 'Twin B', lat: 53.0, lon: -2.0 },   // ambiguous "dd"
];
ok(MESH_SUSPECT_KM === 300 && MESH_SUSPECT_HOP_KM === 250, 'the constants: 300 km base, 250 km per unplaced hop');

// ★ G4XRK Mobile Repeater: Austria, an 8-hop route whose hop 1 is already a placed Suffolk repeater → limit 300 km
const g4 = { lat: 47.37, lon: 11.16, hops: 8, relays: ['aa', 'b7', 'c4', 'ee', 'ef', 'f0', 'f1', 'f2'], node: '9911' };
const s1 = meshSuspect(g4, known, rx);
ok(!!s1 && s1.refKind === 'hop' && s1.refName === 'FSM Repeater IP28' && s1.allowKm === 300 && s1.km > 900, `G4XRK (Austria, first placed hop Suffolk) is suspect (${JSON.stringify(s1)})`);
ok(!!s1 && /Location looks wrong: it reports \d+ mi away, but its first known repeater \(FSM Repeater IP28\) is \d+ mi from that position\. Probably an old or joke position\./.test(meshSuspectWords(s1, 1022, true)), `…and its card line reads (${s1 ? meshSuspectWords(s1, 1022, true) : '-'})`);

// ★ Santa (No Pwd): the North Pole, 10 hops, first placed hop c4 Lyneham after two unknowns → limit 800 km
const santa = { lat: 83.59, lon: -31.04, hops: 10, relays: ['01', '02', 'c4', 'aa', '03', '04', '05', '06', '07', '08'], node: '5a17' };
const s2 = meshSuspect(santa, known, rx);
ok(!!s2 && s2.refName === 'Lyneham RPTR2' && s2.allowKm === 800 && s2.km > 3000, `Santa (North Pole, first placed hop Lyneham) is suspect (${JSON.stringify(s2)})`);

// ★ legit and far: 150 km beyond its first placed hop (Lyneham), no unplaced hop before it → plausible
const far = { lat: 50.20, lon: -2.60, hops: 3, relays: ['c4', 'aa', 'b7'], node: '7001' };
ok(meshSuspect(far, known, rx) === null, 'a node 150 km past its first placed hop is plausible');

// ★ Stuart's Channel case: southern France via one unplaced repeater, then Rye → limit 550 km → plausible
const france = { lat: 47.90, lon: 1.90, hops: 3, relays: ['e1', 'b7', 'c4'], node: '8001' };   // Orléans, ~340 km from Rye
const s3 = meshSuspect(france, known, rx);
ok(s3 === null, `a node in France reached through an unplaced repeater before Rye is plausible (${JSON.stringify(s3)})`);
// …and Paris with two unplaced hops (limit 800) too
ok(meshSuspect({ lat: 48.86, lon: 2.35, hops: 4, relays: ['e1', 'e2', 'b7', 'c4'], node: '8002' }, known, rx) === null, 'Paris via two unplaced repeaters before Rye is plausible');
// …but the same France node with Rye as hop 1 (nothing to bridge the Channel unseen) is beyond 300 km
ok(!!meshSuspect({ ...france, relays: ['b7', 'c4'] }, known, rx), 'the same claim with Rye as its first hop (no unplaced link) is suspect');

// ★ an ambiguous hop counts as unplaced
const amb = meshSuspect({ lat: 47.90, lon: 1.90, hops: 3, relays: ['dd', 'b7', 'c4'], node: '8003' }, known, rx);
ok(amb === null, 'an ambiguous hop before the first placed one adds its 250 km');

// ★ heard first-hand: against the receiver, 300 km
ok(!!meshSuspect({ lat: 55.95, lon: -3.19, hops: 0 }, known, rx), 'a first-hand node claiming Edinburgh (~470 km) is suspect');
ok(meshSuspect({ lat: 51.50, lon: -0.12, hops: 0 }, known, rx) === null, 'a first-hand node claiming London (~110 km) is plausible');
const sd = meshSuspect({ lat: 55.95, lon: -3.19, hops: 0 }, known, rx);
ok(!!sd && /heard first-hand/.test(meshSuspectWords(sd, 470)), 'the first-hand card line says so');

// ★ no evidence: no placed hop on the route (or no route) → no verdict
ok(meshSuspect({ lat: 10, lon: 10, hops: 4, relays: ['e1', 'e2'], node: '1' }, known, rx) === null, 'no placed hop: no verdict');
ok(meshSuspect({ lat: 10, lon: 10, hops: 4 }, known, rx) === null, 'no route: no verdict');
// ★ a node's own id is never its relay
ok(meshSuspect({ lat: 47.37, lon: 11.16, hops: 2, relays: ['aa', 'c4'], node: 'aa11' }, known, rx)?.refName === 'Lyneham RPTR2', 'the node itself is never its own first placed hop');

// ★ a suspect never counts as the furthest
{
  const nodes = [{ ...g4, _sus: s1, name: 'G4XRK' }, { lat: 51.50, lon: -0.12, hops: 0, name: 'London' }];
  const f = meshFurthest(nodes, rx, (d) => d.name);
  ok(f.direct?.name === 'London' && f.mesh === null, `meshFurthest skips the suspect (${JSON.stringify(f)})`);
}

console.log(`test_mesh_plausible: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
