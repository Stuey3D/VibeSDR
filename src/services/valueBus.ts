/**
 * valueBus — a value that changes often, delivered to the ONE component that draws it.
 *
 * ★★★ WHY NOT useState ON THE SCREEN. SDRScreen is the root of ~1,800 lines of JSX. A value that
 *     arrives five times a second (the RDS analyser's `rdsx`, every other spectrum frame) held in
 *     the screen's state re-renders that whole tree five times a second for a panel that is one
 *     leaf of it. The meter bus in ControlsBar (createMeterBus) already solves this for the
 *     signal meter; this is the same idea with a type parameter, so the analyser can use it too.
 * ★ Subscribers get every emit; the last value is kept so a late subscriber starts current.
 */
import { useEffect, useState } from 'react';

export interface ValueBus<T> {
  value: T;
  subs: Set<(v: T) => void>;
  emit: (v: T) => void;
}

export function createValueBus<T>(initial: T): ValueBus<T> {
  const bus: ValueBus<T> = {
    value: initial,
    subs: new Set(),
    emit(v: T) { bus.value = v; bus.subs.forEach(f => f(v)); },
  };
  return bus;
}

/** Subscribe a component to a bus. Re-renders THAT component only. */
export function useBusValue<T>(bus: ValueBus<T> | undefined): T | undefined {
  const [v, setV] = useState<T | undefined>(bus ? bus.value : undefined);
  useEffect(() => {
    if (!bus) return;
    const f = (nv: T) => setV(nv);
    bus.subs.add(f);
    setV(bus.value);
    return () => { bus.subs.delete(f); };
  }, [bus]);
  return bus ? v : undefined;
}
