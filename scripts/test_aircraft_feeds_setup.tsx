/**
 * test_aircraft_feeds_setup.tsx — ★★ beta96 follow-up (docs/v12/FEED-TEST.md): the phone's AIRCRAFT FEEDS card
 * (src/components/AircraftFeedsSetup.tsx), rendered with react-reconciler in plain Node (the test_panelBoundary pattern), with
 * a stand-in for the engine's feed test (the answers vibe_feed_test.h toJson gives). Proves: FIND → the line → ADD; nothing
 * found says what was tried; Advanced ADD tests first, a pass adds, a failure says why and offers ADD ANYWAY; TEST beside a
 * source; and with no engine test there is no FIND or TEST at all (no dead controls). Silent, no network.
 * Run: node scripts/test_aircraft_feeds_setup.mjs
 */
import React from 'react';
import Reconciler from 'react-reconciler';
import AircraftFeedsSetup from '../src/components/AircraftFeedsSetup';
import type { FeedSource } from '../src/utils/aircraftFeeds';

type Node = { type: string; props: any; children: Node[]; text?: string };
const noop = () => {};
const host: any = {
  supportsMutation: true, supportsPersistence: false, supportsHydration: false, isPrimaryRenderer: true,
  createInstance: (type: string, props: any): Node => ({ type, props, children: [] }),
  createTextInstance: (text: string): Node => ({ type: '#text', props: {}, children: [], text }),
  appendInitialChild: (p: Node, c: Node) => { p.children.push(c); },
  appendChild: (p: Node, c: Node) => { p.children.push(c); },
  appendChildToContainer: (p: Node, c: Node) => { p.children.push(c); },
  insertBefore: (p: Node, c: Node, b: Node) => { p.children.splice(p.children.indexOf(b), 0, c); },
  insertInContainerBefore: (p: Node, c: Node, b: Node) => { p.children.splice(p.children.indexOf(b), 0, c); },
  removeChild: (p: Node, c: Node) => { p.children.splice(p.children.indexOf(c), 1); },
  removeChildFromContainer: (p: Node, c: Node) => { p.children.splice(p.children.indexOf(c), 1); },
  commitUpdate: (n: Node, _t: string, _o: any, props: any) => { n.props = props; },
  commitTextUpdate: (n: Node, _o: string, t: string) => { n.text = t; },
  finalizeInitialChildren: () => false, prepareForCommit: () => null, resetAfterCommit: noop,
  getRootHostContext: () => ({}), getChildHostContext: (c: any) => c, shouldSetTextContent: () => false,
  clearContainer: (c: Node) => { c.children.length = 0; }, getPublicInstance: (n: Node) => n,
  scheduleTimeout: setTimeout, cancelTimeout: clearTimeout, noTimeout: -1,
  getCurrentUpdatePriority: () => cur, setCurrentUpdatePriority: (p: number) => { cur = p; },
  resolveUpdatePriority: () => cur || 32, maySuspendCommit: () => false, preloadInstance: () => true,
  startSuspendingCommit: noop, suspendInstance: noop, waitForCommitToBeReady: () => null,
  NotPendingTransition: null, HostTransitionContext: React.createContext(null), resetFormInstance: noop,
  requestPostPaintCallback: noop, shouldAttemptEagerTransition: () => false, trackSchedulerEvent: noop,
  resolveEventType: () => null, resolveEventTimeStamp: () => Date.now(), detachDeletedInstance: noop,
  hideInstance: noop, unhideInstance: noop, hideTextInstance: noop, unhideTextInstance: noop,
  resetTextContent: noop, commitMount: noop, beforeActiveInstanceBlur: noop, afterActiveInstanceBlur: noop,
  prepareScopeUpdate: noop, getInstanceFromNode: () => null, getInstanceFromScope: () => null,
  scheduleMicrotask: queueMicrotask, supportsMicrotasks: true,
};
let cur = 0;
const R = Reconciler(host);
function mount() {
  const container: Node = { type: 'root', props: {}, children: [] };
  // ★ Only UNCAUGHT errors count as escaping; a boundary's catch is reported to onCaughtError.
  const errors: unknown[] = [];
  const onUncaught = (e: unknown) => { errors.push(e); };
  const onOther = () => {};
  const root = R.createContainer(container, 0, null, false, null, '', onUncaught, onOther, onOther, null);
  return {
    container, errors,
    render: (el: React.ReactElement) => { R.updateContainerSync(el, root, null, null); R.flushSyncWork(); },
  };
}
const texts = (n: Node): string[] => [n.text ?? '', ...n.children.flatMap(texts)].filter(Boolean);
const find = (n: Node, pred: (n: Node) => boolean): Node | null =>
  pred(n) ? n : n.children.reduce<Node | null>((a, c) => a ?? find(c, pred), null);


// ── helpers over the rendered tree ──
const allText = (n: Node): string => texts(n).join('');
const findAll = (n: Node, pred: (n: Node) => boolean, out: Node[] = []): Node[] => { if (pred(n)) out.push(n); n.children.forEach((c) => findAll(c, pred, out)); return out; };
/** The touchable whose own text is exactly `label` (the nth of them). */
const button = (root: Node, label: string, nth = 0): Node | null =>
  findAll(root, (n) => n.type === 'TouchableOpacity' && allText(n) === label)[nth] ?? null;
const inputBy = (root: Node, pred: (p: any) => boolean): Node | null => find(root, (n) => n.type === 'TextInput' && pred(n.props));
const tick = async (ms = 0) => { await new Promise((r) => setTimeout(r, ms)); R.flushSyncWork(); await new Promise((r) => setTimeout(r, 0)); R.flushSyncWork(); };
const press = async (n: Node | null) => { if (!n) throw new Error('no such button'); n.props.onPress(); await tick(); };
const type = async (n: Node | null, t: string) => { if (!n) throw new Error('no such input'); n.props.onChangeText(t); await tick(); };

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { console.log((c ? '   ok   ' : '   FAIL ') + what); c ? passes++ : fails++; };
const C = { text: '#fff', textDim: '#999', border: '#333', trackOff: '#222', trackOn: '#fa0', thumb: '#fff', accent: '#fa0', warn: '#f55' };

(async () => {
  // ── a stand-in engine (the answers vibe_feed_test.h toJson gives) ──
  const asked: any[] = [];
  let next: any = null;
  const test = async (body: any) => { asked.push(body); return next; };
  let list: FeedSource[] = [];
  const m = mount();
  const draw = (withTest = true) => m.render(<AircraftFeedsSetup C={C} list={list} onChange={(l) => { list = l; draw(withTest); }} status={[]} error="" test={withTest ? test : undefined} />);
  draw();

  console.log('── FIND: an address, what is there, ADD ──');
  ok(!!button(m.container, 'FIND') && !find(m.container, (n) => n.type === 'TextInput' && /30005/.test(String(n.props.placeholder))), 'the card opens on the address and FIND; the manual form is folded under Advanced');
  ok(/Advanced — a feed that connects to this phone/.test(allText(m.container)), 'the Advanced fold is offered');
  await type(inputBy(m.container, (p) => p.accessibilityLabel === "The other receiver's address"), '192.168.86.11');
  next = { found: true, line: 'Found: UberSDR HFDL (port 8080) — 23 aircraft (14 with positions)', hint: '', tried: ['8080: UberSDR HFDL'],
           hits: [{ src: { fmt: 'uberhfdl', dir: 'connect', host: '192.168.86.11', port: 8080, label: 'hfdl', name: 'UberSDR HFDL' }, name: 'UberSDR HFDL',
                    line: 'UberSDR HFDL (port 8080) — 23 aircraft (14 with positions)' }] };
  await press(button(m.container, 'FIND'));
  ok(JSON.stringify(asked[0]) === '{"find":"192.168.86.11"}', `FIND asks the engine about the address (${JSON.stringify(asked[0])})`);
  ok(allText(m.container).includes('Found: UberSDR HFDL (port 8080) — 23 aircraft (14 with positions)'), 'the one line it found');
  await press(find(m.container, (n) => n.type === 'TouchableOpacity' && n.props.accessibilityLabel === 'Add UberSDR HFDL'));
  ok(list.length === 1 && list[0].fmt === 'uberhfdl' && list[0].name === 'UberSDR HFDL' && list[0].port === 8080, `★ ADD: ${JSON.stringify(list[0])}`);
  ok(allText(m.container).includes('Added. Saved when the server next starts.'), 'and says when it starts');

  console.log('── nothing found ──');
  await type(inputBy(m.container, (p) => p.accessibilityLabel === "The other receiver's address"), '192.168.86.13');
  next = { found: false, line: 'Nothing found on 192.168.86.13 — tried 30005: nothing listening · 30003: nothing listening · 8080: nothing listening · 80: nothing listening',
           hint: 'That address is this server. dumphfdl and dumpvdl2 connect OUT: open Advanced and choose “It connects to this server (listen on a port)”.', hits: [], tried: [] };
  await press(button(m.container, 'FIND'));
  ok(allText(m.container).includes('Nothing found on 192.168.86.13 — tried 30005: nothing listening') && allText(m.container).includes('open Advanced'), 'says what it tried, and where to go');
  ok(!find(m.container, (n) => n.type === 'TouchableOpacity' && /^Add /.test(String(n.props.accessibilityLabel))), 'no ADD for nothing');

  console.log('── Advanced: ADD tests first; a failure offers ADD ANYWAY ──');
  await press(find(m.container, (n) => n.type === 'TouchableOpacity' && /Advanced — /.test(allText(n))));
  ok(!!button(m.container, 'TEST') && !!button(m.container, 'ADD', 0), 'Advanced: TEST and ADD beside the manual form');
  await type(inputBy(m.container, (p) => /30005/.test(String(p.placeholder))), '192.168.86.12:30005');
  await type(inputBy(m.container, (p) => p.placeholder === 'e.g. PiAware in the loft'), 'PiAware');
  next = { ok: true, verdict: 'ok', line: 'Connected — 1,214 Beast messages in 3 s, 87 aircraft', hint: '' };
  const addBtn = () => findAll(m.container, (n) => n.type === 'TouchableOpacity' && allText(n) === 'ADD').pop() ?? null;
  await press(addBtn());
  await tick(5);
  ok(JSON.stringify(asked[asked.length - 1]) === '{"src":{"fmt":"beast","dir":"connect","port":30005,"label":"adsb","host":"192.168.86.12","name":"PiAware"}}', `ADD tests the source first (${JSON.stringify(asked[asked.length - 1])})`);
  ok(list.length === 2 && list[1].name === 'PiAware', '★ a passing test: added');
  ok(allText(m.container).includes('✓ Connected — 1,214 Beast messages in 3 s, 87 aircraft Added.'), 'with what the test found');
  await type(inputBy(m.container, (p) => /30005/.test(String(p.placeholder))), '192.168.86.40');
  next = { ok: false, verdict: 'unreachable', line: 'No route to 192.168.86.40, or it timed out (3 s)', hint: 'is that machine on, and on this network?' };
  await press(addBtn());
  await tick(5);
  ok(list.length === 2 && allText(m.container).includes('✗ No route to 192.168.86.40, or it timed out (3 s) — is that machine on'), '★ a failing test: not added, and why');
  ok(!!button(m.container, 'ADD ANYWAY'), '…ADD ANYWAY offered');
  await press(button(m.container, 'ADD ANYWAY'));
  ok(list.length === 3 && list[2].host === '192.168.86.40' && !button(m.container, 'ADD ANYWAY'), '★ ADD ANYWAY adds it, and goes');

  console.log('── TEST beside a source ──');
  next = { ok: true, verdict: 'ok', line: 'Connected — UberSDR HFDL, 23 aircraft (14 with positions)', hint: '' };
  await press(find(m.container, (n) => n.type === 'TouchableOpacity' && n.props.accessibilityLabel === 'Test UberSDR HFDL'));
  await tick(5);
  ok(JSON.stringify(asked[asked.length - 1].src) === JSON.stringify(list[0]) && allText(m.container).includes('✓ Connected — UberSDR HFDL, 23 aircraft'), 'its result under the row');

  console.log('── ★★ b96-2 "Not at this site": the tick, the tag ──');
  {
    let l2: FeedSource[] = [];
    const q = mount();
    const draw2 = () => q.render(<AircraftFeedsSetup C={C} list={l2} onChange={(l) => { l2 = l; draw2(); }} status={[]} error="" test={test} />);
    draw2();
    const tickBox = () => find(q.container, (x) => x.type === 'TouchableOpacity' && x.props.accessibilityRole === 'checkbox' && /^Not at this site/.test(String(x.props.accessibilityLabel)));
    ok(!!tickBox() && tickBox()!.props.accessibilityState?.checked === false, '"Not at this site" is a tick in the add form, unticked');
    ok(/Not at this site — its aircraft are shown but not counted in this server’s range outline/.test(allText(q.container)), '…with its words');
    await press(tickBox());
    ok(tickBox()!.props.accessibilityState?.checked === true, 'pressed: ticked');
    await type(inputBy(q.container, (p) => p.accessibilityLabel === "The other receiver's address"), '192.168.86.30');
    next = { found: true, line: 'Found: PiAware ADS-B (Beast, port 30005) — 40 aircraft', hint: '', tried: [],
             hits: [{ src: { fmt: 'beast', dir: 'connect', host: '192.168.86.30', port: 30005, label: 'adsb', name: 'Friend' }, name: 'Friend', line: 'PiAware ADS-B (Beast, port 30005) — 40 aircraft' }] };
    await press(button(q.container, 'FIND'));
    await press(find(q.container, (x) => x.type === 'TouchableOpacity' && x.props.accessibilityLabel === 'Add Friend'));
    ok(l2.length === 1 && l2[0].offsite === true, `★ the ADS-B feed added with the tick is off-site (${JSON.stringify(l2[0])})`);
    ok(tickBox()!.props.accessibilityState?.checked === false, '…and the tick clears for the next feed');
    ok(allText(q.container).includes('off-site'), '★ the list tags it "off-site"');
    await press(tickBox());
    await type(inputBy(q.container, (p) => p.accessibilityLabel === "The other receiver's address"), '192.168.86.11');
    next = { found: true, line: 'Found: UberSDR HFDL (port 8080) — 23 aircraft', hint: '', tried: [],
             hits: [{ src: { fmt: 'uberhfdl', dir: 'connect', host: '192.168.86.11', port: 8080, label: 'hfdl', name: 'UberSDR HFDL' }, name: 'UberSDR HFDL', line: 'UberSDR HFDL (port 8080) — 23 aircraft' }] };
    await press(button(q.container, 'FIND'));
    await press(find(q.container, (x) => x.type === 'TouchableOpacity' && x.props.accessibilityLabel === 'Add UberSDR HFDL'));
    ok(l2.length === 2 && l2[1].offsite === undefined, 'an HFDL feed never carries it (the outline is ADS-B only)');
    ok(findAll(q.container, (x) => x.type === 'Text' && allText(x) === 'off-site').length === 1, '…so one tag, on the ADS-B row only');
    ok(q.errors.length === 0, 'no error escaped');
  }

  console.log('── no engine test: no dead buttons ──');
  const n = mount();
  n.render(<AircraftFeedsSetup C={C} list={list} onChange={() => {}} status={[]} error="" />);
  ok(!button(n.container, 'FIND') && !button(n.container, 'TEST') && !find(n.container, (x) => x.type === 'TouchableOpacity' && /^Test /.test(String(x.props.accessibilityLabel))), 'without the engine\'s test: no FIND, no TEST');
  ok(!!button(n.container, 'ADD') && !!find(n.container, (x) => x.type === 'TextInput' && /30005/.test(String(x.props.placeholder))), '…the manual form, open, with ADD');
  ok(m.errors.length === 0 && n.errors.length === 0, 'no error escaped');
  console.log(`\ntest_aircraft_feeds_setup: ${passes} passed, ${fails} failed`);
  process.exit(fails ? 1 : 0);
})();
