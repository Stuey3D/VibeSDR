/**
 * useFieldPopupNav — the pad and keyboard for a small pop-up of one or more text fields and a row of keys (Cancel /
 * Connect): PasswordModal, IdentModal, VibePinPrompt, the picker's Add-server and Edit-favourite boxes (controller
 * audit group B, 2026-10-20).
 *
 * ★ Every one of these had no keys at all and was in no yield list: arrows and Enter went to the list BEHIND it (a
 *   stranger's receiver could connect under a password box), and on Android a pad's B is consumed natively, so there
 *   was no way out at all. Now: an overlay (Back = Cancel), a list over [fields…, keys…] with the keys on one row,
 *   starting on the first field. Enter on a field puts the cursor in it (`onField` — the controller keyboard plugs in
 *   there for a pad); Enter on a key presses it.
 */
import React from 'react';
import { View, type StyleProp, type ViewStyle } from 'react-native';
import { NavRing, useListNav, useNavFocus, useOverlay } from './PanelNav';

export interface FieldPopupNavOpts {
  name: string;
  /** How many text fields, top to bottom, before the keys. */
  fields: number;
  /** The keys on the bottom row, left to right — what Enter on each does. */
  keys: Array<() => void>;
  /** Enter on field `i` (focus it, or open the controller keyboard). */
  onField: (i: number) => void;
  /** Back with nothing left to peel: Cancel. */
  onBack: () => void;
  /** Where the highlight starts (default the first field). */
  startAt?: number;
  /** Extra rows: the row of each FIELD (default one row each). Keys always share the row after the last field. */
  fieldRow?: (i: number) => number;
}

export function useFieldPopupNav(visible: boolean, o: FieldPopupNavOpts) {
  const ref = React.useRef(o); ref.current = o;
  useOverlay(visible, { name: o.name, onBack: () => ref.current.onBack(), blockTuneZoom: true });
  const n = o.fields + o.keys.length;
  const lastFieldRow = o.fields > 0 ? (o.fieldRow ? o.fieldRow(o.fields - 1) : o.fields - 1) : -1;
  const focus = useListNav(
    visible, n,
    (i) => {
      const c = ref.current;
      if (i < c.fields) c.onField(i); else c.keys[i - c.fields]?.();
    },
    undefined, undefined,
    // Back passes on to the overlay (Cancel).
    () => false,
    {
      rowOf: (i) => {
        const c = ref.current;
        return i < c.fields ? (c.fieldRow ? c.fieldRow(i) : i) : lastFieldRow + 1;
      },
      startAt: o.startAt ?? 0,
    },
  );
  const ring = useNavFocus().ring;
  return {
    ring,
    /** Is field `i` highlighted? */
    field: (i: number) => focus === i,
    /** Is key `i` highlighted? */
    key: (i: number) => focus === o.fields + i,
  };
}

/** A wrapper that draws the highlight round a key with no focus style of its own (PopupKey, a plain Touchable). */
export function NavWrap({ on, color, radius = 8, style, children }: {
  on: boolean; color: string; radius?: number; style?: StyleProp<ViewStyle>; children: React.ReactNode;
}) {
  return (
    <View style={style}>
      {children}
      <NavRing on={on} color={color} radius={radius} inset={-4} />
    </View>
  );
}
