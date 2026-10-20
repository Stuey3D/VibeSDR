// ★★★ The server list's focus order under a REAL React renderer (V12 489, 2026-10-20 — Stuart's PS pad on the Mac:
// "the green box doesn't keep up with the screen scrolling … I cannot select anything", "it skips past the bottom few
// discovered servers", "it's not expanding the directories with the X button").
//
// Root cause (src/utils/renderBank.ts): the picker claimed its focus slots into a plain ref, emptied at the top of its
// render, and its key handlers read that same ref. A same-value setState (`setRowSub(0)` after every highlight move)
// makes React call the screen again and bail out WITHOUT rendering its children — so the bank kept only the slots the
// screen claims itself and lost every row claimed by a child (ChooserRow, ChooserKey). The ring stayed where React drew
// it, while reveal and Enter worked from the wrong index.
//
// This drives a model of the picker's structure — slots claimed by the screen, rows and keys claimed by child components,
// list cells after them, the footer last, DISCOVERED rows arriving after mount — through the REAL key bus, fed by the
// REAL pad mapper (✕ on a stick pad, R on a no-stick pad), and checks after every key that what is lit, what reveal
// scrolls to (pickerZone on the committed counts) and what Enter presses are the same thing.
import React, { useEffect, useRef, useState } from 'react';
import { readFileSync } from 'node:fs';
import { mount, settle, renderErrors, type HostNode } from './lib/reactHarness';
import { useRenderBank } from '../src/utils/renderBank';
import { pickerZone } from '../src/utils/pickerNav';
import { KeyBus } from '../src/utils/keyActions';
import { PadMapper, PAD_BIT, type PadReport } from '../src/utils/padActions';
import { OverlayStack } from '../src/utils/overlayStack';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const h = React.createElement;

const SLOTS = ['RECORDINGS', 'cog', 'auto ✕', 'URL', 'CONNECT'];
const DISC = ['Stuey3D UberSDR', 'vibeserverpi2', 'lenovo5', 'vibeserver-pi500', 'vibeserver-portable', 'Bravia', 'Xcover 4S'];
const LIST = ['hdr:CUSTOM SERVERS', 'OWRX', 'UberSDR Local', 'Moto G35', 'Pi500', 'OWRX Local', 'Mac', 'Portable', '192.168',
              'hdr:FAVOURITES', 'MLA30+', 'Newcastle', 'Delta loop', 'Manchester'];
const FOOT = ['call sign', 'dir:UberSDR', 'dir:KiwiSDR', 'dir:OpenWebRX'];

function expectedOrder(disc: string[]): string[] {
  return [...SLOTS, '+ Add custom server', ...disc.flatMap((d) => [d, d + ' ☆']), '↻ Search again', ...LIST, ...FOOT];
}

/** A PureComponent list like VirtualizedList: header element, a cell per row (rendered by renderItem), footer element. */
class List extends React.PureComponent<{ header: React.ReactNode; footer: React.ReactNode; n: number; renderItem: (i: number) => React.ReactNode; extra: unknown }> {
  render() {
    const cells = [];
    for (let i = 0; i < this.props.n; i++) cells.push(h(Cell, { key: i, i, renderItem: this.props.renderItem }));
    return h('list', null, this.props.header, ...cells, this.props.footer);
  }
}
const Cell = React.memo(({ i, renderItem }: { i: number; renderItem: (i: number) => React.ReactNode }) => renderItem(i) as React.ReactElement);

type Kind = 'committed' | 'legacy';
interface Api {
  navFocus: number; setNavFocus: (n: number) => void; setDisc: (d: string[]) => void;
  hdrDone: { current: Array<() => void> }; ftrDone: { current: Array<() => void> }; zone: { h: number; f: number };
}

function makePicker(kind: Kind, pressed: string[], apiOut: { current: Api | null }) {
  return function Picker() {
    const [navFocus, setNavFocus] = useState(-1);
    const [rowSub, setRowSub] = useState(0);
    const [disc, setDisc] = useState(DISC.slice(0, 5));       // two more arrive "from Bonjour" after mount
    const [zone, setZone] = useState({ h: 0, f: 0 });
    const zoneRef = useRef(zone); zoneRef.current = zone;
    let hdrStage: { current: Array<() => void> }, ftrStage: { current: Array<() => void> };
    let hdrDone: { current: Array<() => void> }, ftrDone: { current: Array<() => void> };
    const publish = () => {
      const hN = hdrDone.current.length, fN = ftrDone.current.length;
      if (hN !== zoneRef.current.h || fN !== zoneRef.current.f) setZone({ h: hN, f: fN });
    };
    if (kind === 'committed') {
      // The picker since the fix (InstancePickerScreen: hdrBank / ftrBank).
      const hb = useRenderBank<() => void>();
      const fb = useRenderBank<() => void>(() => publish());
      hdrStage = hb.stage; ftrStage = fb.stage; hdrDone = hb.committed; ftrDone = fb.committed;
    } else {
      // The picker at 31c7279a: one plain ref per zone, emptied here, read by the key handlers.
      const hr = useRef<Array<() => void>>([]); const fr = useRef<Array<() => void>>([]);
      hr.current = []; fr.current = [];
      hdrStage = hdrDone = hr; ftrStage = ftrDone = fr;
      useEffect(() => { publish(); });
    }
    // ★ 31c7279a's trigger: a same-value setState after every highlight move.
    useEffect(() => { setRowSub(0); }, [navFocus]);
    apiOut.current = { navFocus, setNavFocus, setDisc, hdrDone, ftrDone, zone };

    // Claimed by the SCREEN's own render (chooserSlot).
    const slot = (id: string) => {
      const i = hdrStage.current.length;
      hdrStage.current.push(() => pressed.push(id));
      return h('slot', { key: id, id, ring: navFocus === i });
    };
    // Claimed by CHILD components (ChooserRow / ChooserKey) — new types every render, as on the screen.
    const Row = ({ id, foot, children }: { id: string; foot?: boolean; children?: React.ReactNode }) => {
      const bank = foot ? ftrStage.current : hdrStage.current;
      const my = bank.length;
      bank.push(() => pressed.push(id));
      const g = foot ? hdrStage.current.length + LIST.length + my : my;
      return h('row', { id, ring: navFocus === g }, children);
    };
    const Key = ({ id }: { id: string }) => {
      const my = hdrStage.current.length;
      hdrStage.current.push(() => pressed.push(id));
      return h('key', { id, ring: navFocus === my });
    };
    const header = h('view', null,
      h(Row, { id: '+ Add custom server' }),
      ...disc.map((d) => h(Row, { key: d, id: d }, h(Key, { id: d + ' ☆' }))),
      h(Row, { id: '↻ Search again' }));
    const footer = h('view', null, ...FOOT.map((f) => h(Row, { key: f, id: f, foot: true })));
    // Cells: the committed header count (InstancePickerScreen renderItem).
    const renderItem = (i: number) => h('row', { id: LIST[i], ring: i + hdrDone.current.length === navFocus });
    return h('screen', null, ...SLOTS.map(slot), h(List, { header, footer, n: LIST.length, renderItem, extra: [navFocus, rowSub] }));
  };
}

/** The pad → key bus path of src/utils/hardwareKeys.ts (runPad), with the picker's list handler on the bus. */
function wire(api: { current: Api | null }, pressed: string[]) {
  const bus = new KeyBus();
  const reveals: Array<ReturnType<typeof pickerZone>> = [];
  bus.add('down', (e) => {
    const a = api.current!;
    const total = a.zone.h + LIST.length + a.zone.f;
    const at = (i: number) => pickerZone(i, a.hdrDone.current.length, LIST.length, a.ftrDone.current.length);
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const cur = a.navFocus;
      const next = cur < 0 ? 0 : Math.max(0, Math.min(total - 1, cur + (e.key === 'ArrowDown' ? 1 : -1)));
      a.setNavFocus(next);
      reveals.push(at(next));                       // what the screen's reveal scrolls to
      return true;
    }
    if (e.key === 'Enter') {
      const z = at(a.navFocus);
      if (!z) return true;
      if (z.zone === 'header') a.hdrDone.current[z.at]?.();
      else if (z.zone === 'footer') a.ftrDone.current[z.at]?.();
      else pressed.push(LIST[z.at]);
      return true;
    }
  });
  const pad = new PadMapper();
  const feed = (r: PadReport) => {
    for (const o of pad.feed(r, Date.now())) {
      if ('kind' in o) bus.dispatch(o.kind, { key: o.key, plain: false, typing: false, pad: true });
    }
  };
  const tap = (id: string, sticks: boolean, bit: number) => { feed({ id, sticks, b: bit }); feed({ id, sticks, b: 0 }); };
  return {
    reveals,
    down: () => tap('ps', true, PAD_BIT.down),
    up: () => tap('ps', true, PAD_BIT.up),
    enterCross: () => tap('ps', true, PAD_BIT.south),      // ✕ — enter on a stick pad
    enterR: () => tap('snes', false, PAD_BIT.r1),          // R — enter on a no-stick pad
  };
}

const lit = (nodes: HostNode[]) => nodes.filter((n) => n.props?.ring).map((n) => String(n.props.id));

async function walk(kind: Kind) {
  const pressed: string[] = [];
  const api: { current: Api | null } = { current: null };
  const m = mount(h(makePicker(kind, pressed, api)));
  await settle();
  const pad = wire(api, pressed);
  let order = expectedOrder(DISC.slice(0, 5));
  const problems: string[] = [];
  const reached = new Set<string>();
  const check = (why: string) => {
    const a = api.current!;
    const want = order[a.navFocus];
    const l = lit(m.all());
    if (l.length !== 1 || l[0] !== want) problems.push(`${why}: focus ${a.navFocus} should light "${want}", lit [${l.join(', ')}]`);
    // Reveal: the zone the handlers compute must be the zone of the row that is lit.
    const truth = pickerZone(a.navFocus, order.length - LIST.length - FOOT.length, LIST.length, FOOT.length);
    const got = pad.reveals[pad.reveals.length - 1];
    if (JSON.stringify(truth) !== JSON.stringify(got)) problems.push(`${why}: reveal went to ${JSON.stringify(got)} for "${want}" (${JSON.stringify(truth)})`);
    if (l.length === 1 && l[0] === want) reached.add(want);
  };
  // Down from the top, two DISCOVERED servers arriving part-way (Bonjour answers after the list is up).
  for (let s = 0; s < order.length + 3; s++) {
    if (s === 4) { api.current!.setDisc(DISC.slice()); await settle(); order = expectedOrder(DISC); }
    pad.down(); await settle(); check(`down #${s}`);
  }
  const downReached = new Set(reached);
  reached.clear();
  reached.add(order[api.current!.navFocus]);      // where the walk up starts (the last footer row)
  for (let s = 0; s < order.length + 3; s++) { pad.up(); await settle(); check(`up #${s}`); }
  // Enter, both pads' enter, on the rows Stuart could not use.
  const enterOn = async (id: string, press: () => void, label: string) => {
    const target = order.indexOf(id);
    while (api.current!.navFocus < target) { pad.down(); await settle(); }
    while (api.current!.navFocus > target) { pad.up(); await settle(); }
    const before = pressed.length;
    press(); await settle();
    const got = pressed.slice(before);
    if (got.length !== 1 || got[0] !== id) problems.push(`Enter (${label}) on "${id}" pressed [${got.join(', ')}]`);
  };
  for (const id of ['Stuey3D UberSDR', 'Bravia', 'Xcover 4S ☆', '↻ Search again', 'hdr:FAVOURITES', 'Manchester', 'dir:KiwiSDR', 'CONNECT']) {
    await enterOn(id, pad.enterCross, '✕, stick pad');
    await enterOn(id, pad.enterR, 'R, no-stick pad');
  }
  m.unmount();
  return { problems, downReached, upReached: reached, order };
}

async function main() {
  console.log('test_render_bank');

  // ── The trap, reproduced: 31c7279a's plain-ref bank under the same moves. ──
  const legacy = await walk('legacy');
  ok(legacy.problems.length > 0, `the old plain-ref bank goes wrong under a same-value setState (${legacy.problems.length} faults, e.g. "${legacy.problems[0] ?? '—'}")`);

  // ── The fix. ──
  const fixed = await walk('committed');
  for (const p of fixed.problems.slice(0, 12)) console.log('      ' + p);
  ok(fixed.problems.length === 0, 'every move: the lit row is the focused row, and reveal goes to that row\'s zone');
  const missing = (s: Set<string>) => fixed.order.filter((id) => !s.has(id));
  ok(missing(fixed.downReached).length === 0, `going DOWN every control and row is lit in turn, the late DISCOVERED servers included${missing(fixed.downReached).length ? ' — missed ' + missing(fixed.downReached).join(', ') : ''}`);
  ok(missing(fixed.upReached).length === 0, `going UP likewise${missing(fixed.upReached).length ? ' — missed ' + missing(fixed.upReached).join(', ') : ''}`);
  ok(!fixed.problems.some((p) => p.startsWith('Enter')), 'Enter presses the lit row — a server, Search again, a group header, a DIRECTORY — on ✕ (stick pad) and R (no-stick pad)');
  ok(renderErrors().length === 0, 'no render errors');

  // ── A fresh screen has no overlay over it (the list yields while any is open). ──
  const st = new OverlayStack();
  ok(st.count() === 0, 'a fresh overlay stack is empty — nothing swallows Enter on a fresh picker');

  // ── Every bank that claims during render goes through useRenderBank (no plain-ref bank left to rot). ──
  for (const f of ['src/screens/InstancePickerScreen.tsx', 'src/components/FreqModal.tsx', 'src/components/DecoderPanel.tsx', 'src/components/LocalHardwarePanel.tsx']) {
    const src = readFileSync(new URL('../' + f, import.meta.url), 'utf8');
    const resets = src.split('\n').filter((l) => /^\s+\w+\.current = \[\];\s*$/.test(l));
    ok(resets.length === 0 && src.includes('useRenderBank'), `${f}: claims through useRenderBank, no bank emptied in render${resets.length ? ' — ' + resets.map((l) => l.trim()).join(' ') : ''}`);
  }
  process.exit(fails ? 1 : 0);
}
void main();
