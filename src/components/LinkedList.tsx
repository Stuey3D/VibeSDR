/**
 * ★★★ ONE LIST ACROSS THE CREASE — the React side of utils/linkedListScroll (iPhone Duo half-open BIG; Stuart
 * 2026-11-26: one list spanning both panels is ONE continuous list; scrolling in either scrolls both).
 *
 * The same column is drawn in both panels (the body renders it twice: `part` 'upper' and 'lower'):
 *   · the UPPER is a ScrollView `seam` tall (the last row boundary that fits the panel — never a row cut at the fold),
 *     its column clipped at `maxY + seam` so it can scroll exactly as far as the lower;
 *   · the LOWER is a ScrollView the panel's height, its column pulled up by `seam` (and `endPad` of blank under the
 *     last row), so at the same offset it shows the rows that follow the upper's last;
 *   · both share one offset: whichever the finger is on DRIVES, the other follows (scrollTo, not animated); both snap to
 *     the same rest offsets (the fold on a row boundary — nothing left cut in half at the fold).
 * ★ The column's content style carries no bottom padding (the end is `endPad`, worked out from the rows).
 * The upper copy measures the rows (`box(k)` on each top-level block of the column, in drawing order); a pad / keyboard
 * row is brought into view with `reveal(k)` (linkedReveal: unmoved while it is whole in either panel).
 * ★ Every touchable in the column works in both panels (they are the same handlers); a focus ring is drawn in both,
 *   and only one copy of a row is ever in view.
 */
import React, { useCallback, useMemo, useRef, useState } from 'react';
import { ScrollView, View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import { linkedEndPad, linkedMaxY, linkedReveal, linkedSeam, linkedSnapOffsets, type RowBox } from '../utils/linkedListScroll';

export interface LinkedList {
  /** onLayout for block `k` of the column (attach in the UPPER copy only). */
  box: (k: number) => ((e: LayoutChangeEvent) => void) | undefined;
  /** How many blocks the column has this render (call while drawing the upper copy). */
  count: (n: number) => void;
  /** Bring block `k` into view whole (pad / keyboard focus). */
  reveal: (k: number) => void;
  /** The ScrollView for a panel, around its copy of the column. */
  Panel: (p: { part: 'upper' | 'lower'; style?: StyleProp<ViewStyle>; contentStyle?: StyleProp<ViewStyle>;
               children: React.ReactNode }) => React.ReactElement;
}

/** `upperH` / `lowerH`: the panels' body heights. `lowerRef`: the panel's own scroll ref (DecoderPanel's outputRef). */
export function useLinkedList(upperH: number, lowerH: number, lowerRef?: React.RefObject<ScrollView | null>,
                              onY?: (y: number) => void): LinkedList {
  const rows = useRef<RowBox[]>([]);
  const upper = useRef<ScrollView | null>(null);
  const ownLower = useRef<ScrollView | null>(null);
  const lower = lowerRef ?? ownLower;
  const y = useRef(0);
  const driver = useRef<'upper' | 'lower' | null>(null);
  const [geo, setGeo] = useState<{ seam: number; maxY: number; pad: number; offs: number[] }>(
    { seam: upperH, maxY: 0, pad: 0, offs: [0] });
  const geoRef = useRef(geo); geoRef.current = geo;
  const pending = useRef(false);
  const dims = useRef({ upperH, lowerH }); dims.current = { upperH, lowerH };

  const recompute = useCallback(() => {
    pending.current = false;
    const rs = rows.current.filter(Boolean);
    const { upperH: u, lowerH: l } = dims.current;
    const seam = linkedSeam(rs, u);
    const maxY = linkedMaxY(rs, seam, l);
    const pad = linkedEndPad(rs, seam, l, maxY);
    const offs = linkedSnapOffsets(rs, seam, maxY);
    const g = geoRef.current;
    if (g.seam === seam && g.maxY === maxY && g.pad === pad && g.offs.join() === offs.join()) return;
    setGeo({ seam, maxY, pad, offs });
  }, []);
  const later = useCallback(() => {
    if (pending.current) return;
    pending.current = true;
    requestAnimationFrame(recompute);
  }, [recompute]);
  // The panels' heights change with the window: the seam and the rest offsets with them.
  React.useEffect(() => { later(); }, [upperH, lowerH, later]);

  const boxCbs = useRef<((e: LayoutChangeEvent) => void)[]>([]);
  const box = useCallback((k: number) => {
    if (!boxCbs.current[k]) {
      boxCbs.current[k] = (e: LayoutChangeEvent) => {
        const { y: top, height: h } = e.nativeEvent.layout;
        const r = rows.current[k];
        if (r && r.top === top && r.h === h) return;
        rows.current[k] = { top, h };
        later();
      };
    }
    return boxCbs.current[k];
  }, [later]);
  const count = useCallback((n: number) => {
    if (rows.current.length > n) { rows.current.length = n; later(); }
  }, [later]);

  const scrollBoth = useCallback((to: number) => {
    y.current = to;
    driver.current = null;
    try { upper.current?.scrollTo({ y: to, animated: false }); } catch { /* detached */ }
    try { lower.current?.scrollTo({ y: to, animated: false }); } catch { /* detached */ }
    onY?.(to);
  }, [lower, onY]);
  const reveal = useCallback((k: number) => {
    const g = geoRef.current;
    const to = linkedReveal(rows.current, k, y.current, g.seam, dims.current.lowerH, g.offs);
    if (to !== y.current) scrollBoth(to);
  }, [scrollBoth]);

  const follow = useCallback((from: 'upper' | 'lower', e: any) => {
    if (driver.current !== from) return;   // ★ the follower's own (programmatic) scroll events are not a lead
    const at = e?.nativeEvent?.contentOffset?.y ?? 0;
    y.current = at;
    const other = from === 'upper' ? lower : upper;
    try { other.current?.scrollTo({ y: at, animated: false }); } catch { /* detached */ }
    onY?.(at);
  }, [lower, onY]);

  const Panel = useMemo(() => function LinkedPanel({ part, style, contentStyle, children }: {
    part: 'upper' | 'lower'; style?: StyleProp<ViewStyle>; contentStyle?: StyleProp<ViewStyle>; children: React.ReactNode;
  }) {
    const g = geoRef.current;
    const isUpper = part === 'upper';
    const sv = (
      <ScrollView ref={(isUpper ? upper : lower) as any}
        style={[style, { height: isUpper ? g.seam : dims.current.lowerH }]}
        showsVerticalScrollIndicator={!isUpper}
        scrollEventThrottle={16}
        snapToOffsets={g.offs.length > 1 ? g.offs : undefined}
        decelerationRate="fast"
        onScrollBeginDrag={() => { driver.current = part; }}
        onScroll={(e) => follow(part, e)}
        onMomentumScrollEnd={(e) => follow(part, e)}
        onScrollEndDrag={(e) => follow(part, e)}>
        {isUpper ? (
          // Clipped where the lower ends, so both reach the same last offset.
          <View style={[contentStyle, g.maxY > 0 && { height: g.maxY + g.seam, overflow: 'hidden' }]}>{children}</View>
        ) : (
          <View style={[contentStyle, { marginTop: -g.seam, paddingBottom: g.pad }]}>{children}</View>
        )}
      </ScrollView>
    );
    // The upper panel keeps its height; the sliver under the seam stays blank (no row cut at the fold).
    return isUpper ? <View style={{ height: dims.current.upperH }}>{sv}</View> : sv;
  }, [follow, lower]);

  return { box, count, reveal, Panel };
}
