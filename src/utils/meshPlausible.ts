/**
 * meshPlausible.ts — ★★ V12 (Stuart 11-27, approved) MESH NODES WITH IMPLAUSIBLE SELF-REPORTED LOCATIONS.
 *
 * A MeshCore / Meshtastic node REPORTS its own position; some are jokes ("Santa" at the North Pole) and some are stale (a
 * mobile repeater still set to where it was last summer). The route says where it really is: its packet came in through
 * repeaters whose positions we know. So a node's claim is SUSPECT when it lies more than MESH_SUSPECT_KM from the FIRST
 * PLACED hop on its route (the known repeater nearest the node's end), allowing MESH_SUSPECT_HOP_KM more for every unknown
 * or ambiguous hop between the node and that repeater. A node heard first-hand (0 hops) is judged against the receiver
 * with the same MESH_SUSPECT_KM. No placed hop and not first-hand = no evidence either way = not suspect.
 *
 * A suspect node STAYS on the map (a "?" on its icon, a dimmed label, a line on its card) but never counts towards a
 * "furthest", STATS' FURTHEST, the FIT / AUTO framing or a range outline; the LIST and LAYERS still count it.
 * One rule for the web's ISM maps and the community map's mesh layer (and the app when it ports).
 */
import { meshKm, meshDist, meshIsDirect, type MeshKnown } from './meshRoute';

/** ★ THE CONSTANTS (one place): how far from its first placed repeater a node may claim to be, and the extra per hop of
 *  air we cannot place between them. */
export const MESH_SUSPECT_KM = 300;
export const MESH_SUSPECT_HOP_KM = 250;   // ★ Stuart 11-27: every unplaced repeater could be a long sea / hill-top link (the Channel)

export interface MeshSuspect {
  /** what the claim is measured against: the first placed repeater on the route, or the receiver (heard first-hand) */
  refKind: 'hop' | 'rx';
  refName: string;
  /** km from the claimed position to the reference, and what was allowed */
  km: number;
  allowKm: number;
}

type Node = { lat?: number; lon?: number; relays?: string[]; node?: string; hops?: number; direct?: number };

/** The suspect verdict, or null (believable, or nothing to judge by). `known` = the placeable repeaters / room servers
 *  (meshRoute's MeshKnown — the same list the ROUTE button uses); `rx` = the receiver. */
export function meshSuspect(d: Node, known: readonly MeshKnown[], rx: { lat: number; lon: number } | null): MeshSuspect | null {
  if (d.lat === undefined || d.lon === undefined || !Number.isFinite(d.lat) || !Number.isFinite(d.lon)) return null;
  if (meshIsDirect(d)) {
    if (!rx) return null;
    const km = meshKm(rx, d.lat, d.lon);
    return km > MESH_SUSPECT_KM ? { refKind: 'rx', refName: 'the receiver', km, allowKm: MESH_SUSPECT_KM } : null;
  }
  const rel = d.relays;
  if (!rel || !rel.length) return null;
  const self = (d.node || '').toLowerCase();
  for (let j = 0; j < rel.length; j++) {
    const h = rel[j];
    const cands = known.filter((k) => k.id.toLowerCase().startsWith(h) && k.id.toLowerCase() !== self);
    if (cands.length !== 1) continue;   // unknown or ambiguous: one more hop of allowance
    const c = cands[0];
    const km = meshKm({ lat: c.lat, lon: c.lon }, d.lat, d.lon);
    const allowKm = MESH_SUSPECT_KM + MESH_SUSPECT_HOP_KM * j;
    return km > allowKm ? { refKind: 'hop', refName: c.name, km, allowKm } : null;
  }
  return null;
}

/** The card's line: "Location looks wrong: it reports 635 mi away, but its first known repeater (FSM Repeater IP28) is
 *  590 mi from that position. Probably an old or joke position." `rxKm` = the claimed distance from the receiver. */
export function meshSuspectWords(s: MeshSuspect, rxKm: number | null, miles = false): string {
  const away = rxKm != null ? `it reports ${meshDist(rxKm, miles)} away, but ` : '';
  const ref = s.refKind === 'hop'
    ? `its first known repeater (${s.refName}) is ${meshDist(s.km, miles)} from that position`
    : `it was heard first-hand, and that position is ${meshDist(s.km, miles)} from the receiver`;
  const t = `Location looks wrong: ${away}${ref}. Probably an old or joke position.`;
  return t.charAt(0).toUpperCase() + t.slice(1);
}
