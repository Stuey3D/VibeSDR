/**
 * The executable spec for src/components/PanelBoundary.tsx — ONE PANEL THAT THROWS CLOSES ONE PANEL.
 *
 * Run: node scripts/test_panelBoundary.mjs
 *      (bundles this file with esbuild, aliasing react-native to a few host-component names, and
 *       renders it with react-reconciler in plain Node — no simulator, no device, no audio.)
 *
 * The screen under test is shaped like SDRScreen: a spectrum that redraws on every frame, an audio
 * component, and an Advanced RDS panel fed by the same stream — and the RDS panel throws on a
 * wrong-typed field, which is exactly the fault that used to unmount the whole SDR screen.
 *
 * What it must prove:
 *   1. The throwing panel is replaced by its one-line notice; the spectrum and the audio stay mounted.
 *   2. The spectrum KEEPS DRAWING after the panel died (later frames still render).
 *   3. The fault is logged with the panel's name and counted (faultLog).
 *   4. RETRY remounts the panel fresh; once the data is good it renders again.
 *   5. CLOSE calls the parent's onClose.
 *   6. CONTROL: the same throw with NO boundary takes the whole root down — so (1) is not vacuous.
 *   7. autoRetry (the spectrum, controls, audio): a transient throw is remounted by itself.
 */
import React from 'react';
import Reconciler from 'react-reconciler';
import PanelBoundary from '../src/components/PanelBoundary';
import { faultSummary, _setFaultSink, _resetFaults } from '../src/services/faultLog';

// ── A minimal host: a tree of {type, props, children} we can search. ──────────────────────────
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

// ── The screen ───────────────────────────────────────────────────────────────────────────────
let spectrumDraws = 0;
function Spectrum({ frame }: { frame: number }) { spectrumDraws++; return <spectrum frame={frame} />; }
function Audio() { return <audio playing />; }
/** Trusts the wire, as AdvRdsPanel's fields did: ps must be a string. */
function AdvRds({ ps }: { ps: unknown }) { return <rds ps={(ps as string).trim()} />; }

function Screen({ frame, ps, guarded, onClose }: { frame: number; ps: unknown; guarded: boolean; onClose?: () => void }) {
  const panel = <AdvRds ps={ps} />;
  return (
    <screen>
      <Spectrum frame={frame} />
      <Audio />
      {guarded ? <PanelBoundary name="Advanced RDS" onClose={onClose}>{panel}</PanelBoundary> : panel}
    </screen>
  );
}

// ── Tests ────────────────────────────────────────────────────────────────────────────────────
let fails = 0;
const ok = (c: boolean, what: string) => { console.log((c ? '  ok   ' : '  FAIL ') + what); if (!c) fails++; };
const logged: string[] = [];
_setFaultSink((line) => { logged.push(line); });
// React logs the caught error itself; keep the run readable and silent.
const realErr = console.error; const realLog = console.log;
console.error = () => {};

(async () => {
  _resetFaults();
  let closed = 0;
  const m = mount();
  m.render(<Screen frame={1} ps="RADIO 1" guarded onClose={() => { closed++; }} />);
  ok(texts(m.container).length === 0 && !!find(m.container, (n) => n.type === 'rds'), 'healthy: RDS panel rendered');

  // The bad packet: ps arrives as a number.
  console.log = () => {};                     // recordCrash logs via console.log
  m.render(<Screen frame={2} ps={42} guarded onClose={() => { closed++; }} />);
  console.log = realLog;
  ok(!find(m.container, (n) => n.type === 'rds'), '1. the throwing panel is gone');
  ok(!!find(m.container, (n) => n.type === 'spectrum') && !!find(m.container, (n) => n.type === 'audio'),
     '1. spectrum and audio still mounted');
  ok(texts(m.container).join(' ').includes('Advanced RDS hit an error and was closed — the radio is still playing'),
     '1. one-line notice shown: "' + texts(m.container)[0] + '"');

  const before = spectrumDraws;
  for (let f = 3; f < 13; f++) m.render(<Screen frame={f} ps={42} guarded onClose={() => { closed++; }} />);
  const spec = find(m.container, (n) => n.type === 'spectrum');
  ok(spectrumDraws - before === 10 && spec?.props.frame === 12, `2. spectrum kept drawing (${spectrumDraws - before} frames after the fault)`);
  ok(!find(m.container, (n) => n.type === 'rds'), '2. and the dead panel did NOT come back by itself while the data is still bad');

  const f = faultSummary().find((e) => e.source === 'panel:Advanced RDS');
  ok(!!f && f.kind === 'render' && f.count === 1, '3. counted once under panel:Advanced RDS / render');
  ok(logged.length === 1 && logged[0].includes('panel:Advanced RDS') && /TypeError/.test(logged[0]),
     '3. logged with the panel name: ' + (logged[0] ?? '').slice(0, 110));

  // RETRY with good data again.
  m.render(<Screen frame={13} ps="RADIO 2" guarded onClose={() => { closed++; }} />);
  const retry = find(m.container, (n) => n.props?.accessibilityLabel === 'Retry Advanced RDS');
  ok(!!retry, '4. RETRY button present');
  retry!.props.onPress(); R.flushSyncWork();
  await new Promise((r) => setTimeout(r, 0)); R.flushSyncWork();
  ok(find(m.container, (n) => n.type === 'rds')?.props.ps === 'RADIO 2', '4. RETRY remounted the panel and it renders');

  // Throw again, then CLOSE.
  console.log = () => {};
  m.render(<Screen frame={14} ps={null} guarded onClose={() => { closed++; }} />);
  console.log = realLog;
  const close = find(m.container, (n) => n.props?.accessibilityLabel === 'Close Advanced RDS');
  close!.props.onPress(); R.flushSyncWork();
  await new Promise((r) => setTimeout(r, 0)); R.flushSyncWork();
  ok(closed === 1, '5. CLOSE called the parent onClose');
  ok(texts(m.container).length === 0 && !!find(m.container, (n) => n.type === 'spectrum'), '5. notice gone, spectrum still there');
  ok(m.errors.length === 0, 'no error escaped to the root');

  // 6. CONTROL — no boundary.
  const c = mount();
  c.render(<Screen frame={1} ps="RADIO 1" guarded={false} />);
  c.render(<Screen frame={2} ps={42} guarded={false} />);
  ok(!find(c.container, (n) => n.type === 'spectrum') && c.errors.length > 0,
     '6. CONTROL: without the boundary the same throw unmounts the spectrum and audio too');

  // 7. autoRetry — the spectrum's own boundary remounts it by itself after a transient throw.
  let bad = true;
  function FlakySpectrum() { if (bad) throw new RangeError('Offset is outside the bounds of the DataView'); return <spectrum ok />; }
  const a = mount();
  console.log = () => {};
  a.render(<screen><PanelBoundary name="Spectrum" autoRetry><FlakySpectrum /></PanelBoundary><Audio /></screen>);
  console.log = realLog;
  ok(texts(a.container).join(' ').includes('Spectrum hit an error and is restarting'), '7. autoRetry: notice says it is restarting');
  bad = false;
  await new Promise((r) => setTimeout(r, 1700)); R.flushSyncWork();
  ok(find(a.container, (n) => n.type === 'spectrum')?.props.ok === true && texts(a.container).length === 0,
     '7. autoRetry: the spectrum came back by itself, audio untouched throughout');

  console.error = realErr;
  _setFaultSink(null);
  console.log(fails ? `\n${fails} FAILED` : '\nall passed');
  process.exit(fails ? 1 : 0);
})();
