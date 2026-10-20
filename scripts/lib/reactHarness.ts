/**
 * A real React renderer for tests (react-reconciler — the same React the app runs, 19.x), with host elements that are
 * plain objects. For logic that only shows up in how React RENDERS: bailed-out renders, commit order, effects — the
 * things a pure-module test cannot see. No React Native: tests draw lowercase host tags ('row', 'view') with props.
 *
 * ★ react-reconciler is in node_modules through @shopify/react-native-skia; it is not an app dependency.
 */
import React from 'react';
// @ts-ignore — no types shipped for the reconciler here
import Reconciler from 'react-reconciler';
// @ts-ignore
import { DefaultEventPriority } from 'react-reconciler/constants';

export interface HostNode { type: string; props: Record<string, any>; children: HostNode[] }
interface Container { children: HostNode[] }

let priority = DefaultEventPriority;
const noop = () => {};
const host: any = {
  supportsMutation: true, supportsPersistence: false, supportsHydration: false, isPrimaryRenderer: true,
  createInstance: (type: string, props: any): HostNode => ({ type, props, children: [] }),
  createTextInstance: (text: string): HostNode => ({ type: '#text', props: { text }, children: [] }),
  appendInitialChild: (p: HostNode, c: HostNode) => { p.children.push(c); },
  appendChild: (p: HostNode, c: HostNode) => { p.children.push(c); },
  appendChildToContainer: (p: Container, c: HostNode) => { p.children.push(c); },
  insertBefore: (p: HostNode, c: HostNode, b: HostNode) => { p.children.splice(p.children.indexOf(b), 0, c); },
  insertInContainerBefore: (p: Container, c: HostNode, b: HostNode) => { p.children.splice(p.children.indexOf(b), 0, c); },
  removeChild: (p: HostNode, c: HostNode) => { p.children = p.children.filter((x) => x !== c); },
  removeChildFromContainer: (p: Container, c: HostNode) => { p.children = p.children.filter((x) => x !== c); },
  commitUpdate: (i: HostNode, _t: string, _o: any, n: any) => { i.props = n; },
  commitTextUpdate: (i: HostNode, _o: string, n: string) => { i.props = { text: n }; },
  finalizeInitialChildren: () => false, shouldSetTextContent: () => false,
  getRootHostContext: () => ({}), getChildHostContext: () => ({}), getPublicInstance: (i: any) => i,
  prepareForCommit: () => null, resetAfterCommit: noop, preparePortalMount: noop,
  clearContainer: (c: Container) => { c.children = []; },
  scheduleTimeout: setTimeout, cancelTimeout: clearTimeout, noTimeout: -1,
  supportsMicrotasks: true, scheduleMicrotask: queueMicrotask,
  getCurrentEventPriority: () => priority, setCurrentUpdatePriority: (p: number) => { priority = p; },
  getCurrentUpdatePriority: () => priority, resolveUpdatePriority: () => priority,
  getInstanceFromNode: () => null, beforeActiveInstanceBlur: noop, afterActiveInstanceBlur: noop,
  prepareScopeUpdate: noop, getInstanceFromScope: () => null, detachDeletedInstance: noop,
  maySuspendCommit: () => false, preloadInstance: () => true, startSuspendingCommit: noop, suspendInstance: noop,
  waitForCommitToBeReady: () => null, NotPendingTransition: null, HostTransitionContext: React.createContext(null),
  resetFormInstance: noop, requestPostPaintCallback: noop, shouldAttemptEagerTransition: () => false,
  trackSchedulerEvent: noop, resolveEventType: () => null, resolveEventTimeStamp: () => -1.1,
  hideInstance: noop, unhideInstance: noop, hideTextInstance: noop, unhideTextInstance: noop,
  resetTextContent: noop, commitMount: noop,
};
const R = (Reconciler as any)(host);

export interface Mounted {
  root: Container;
  /** Every host node, depth first. */
  all(): HostNode[];
  unmount(): void;
}

const errors: unknown[] = [];
export function renderErrors(): unknown[] { return errors; }

export function mount(el: React.ReactElement): Mounted {
  const root: Container = { children: [] };
  const fiberRoot = R.createContainer(root, 1 /* concurrent, as the app */, null, false, null, '',
    (e: unknown) => errors.push(e), (e: unknown) => errors.push(e), (e: unknown) => errors.push(e), null);
  R.updateContainer(el, fiberRoot, null, null);
  return {
    root,
    all() {
      const out: HostNode[] = [];
      const walk = (n: HostNode) => { out.push(n); n.children.forEach(walk); };
      root.children.forEach(walk);
      return out;
    },
    unmount() { R.updateContainer(null, fiberRoot, null, null); },
  };
}

/** Let React finish every render and effect that is pending (microtasks and timers). */
export async function settle(rounds = 6): Promise<void> {
  for (let i = 0; i < rounds; i++) await new Promise((r) => setTimeout(r, 0));
}
