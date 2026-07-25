/**
 * ServersChip — the discoverable "how do I leave the waterfall" control.
 *
 * Community feedback (OARC #sdr): experienced users couldn't find the route back
 * to the instance list — it was buried at the bottom of the menu's INSTANCE
 * section behind a hamburger glyph that read as "settings". The back-swipe is
 * deliberately consumed over the tuning area (it fought the drum), so an on-screen
 * affordance is the ONLY exit — burying it was the whole problem.
 *
 * This is a stateful two-tap control, NOT a double-tap gesture (no timing window):
 *   • Collapsed  →  [instance glyph] ‹ Servers        tap = expand (goes nowhere)
 *   • Expanded   →  ‹ Back to server list  (header, = exit, same x-anchor as the
 *                    chip, so "tap-tap in the same spot" is the muscle-memory exit)
 *                   ♡ Favourite this server  (toggles in place, stays open)
 *                   ☆ Set as default         (toggles in place, stays open)
 *                   ⌃ collapse handle
 * A stray tap only opens a dismissable panel — nothing destructive happens until a
 * second, deliberate tap on a labelled row. Dismiss: the collapse chevron, or a tap
 * anywhere outside (the header EXITS, it does not collapse — so the outside-tap and
 * chevron are the required non-exit escapes).
 *
 * Self-contained: renders its own full-screen catch-layer while expanded, so the
 * parent mounts it once and passes only the anchor offsets (§5.2) + handlers.
 */
import React, { useState, useCallback, useEffect, useRef } from 'react';
import { View, Text, Pressable, StyleSheet } from 'react-native';
import { Canvas, Rect, RadialGradient, vec } from '@shopify/react-native-skia';
import { useTheme } from '../contexts/ThemeContext';
import SectionIcon from './SectionIcon';

type Props = {
  /** Bump to open the menu from outside (keyboard Esc). */
  openToken?: number;
  top: number;               // anchor from the safe-area/panel top (§5.2)
  left: number;              // Math.max(margin, insets.left) — notch fix
  serverName: string;
  isFavourite: boolean;
  isDefault: boolean;
  onBack: () => void;
  onToggleFavourite: () => void;
  onSetDefault: () => void;
  /** Network receivers favourite from here; local USB/RTL-TCP favourite via the
   *  picker, so the row is hidden (parent passes false). */
  canFavourite?: boolean;
  /** Coachmark target — attached to the collapsed chip anchor so the first-run
   *  tour can spotlight it (tourRef from the parent). */
  anchorRef?: React.Ref<View>;
};

// Amber pill, shrunk ~12% from the first mockup (which read a touch chunky).
const AMBER_BORDER = 'rgba(255,160,0,0.85)';
const BACKING      = 'rgba(14,10,4,0.82)';   // legible over live green trace…
const SCRIM        = 'rgba(4,3,1,0.9)';       // …softened by the watchOS-style scrim
const SEP          = 'rgba(255,160,0,0.22)';
const SEP_STRONG   = 'rgba(255,160,0,0.5)';   // divides the exit from the toggles

export default function ServersChip({
  top, left, isFavourite, isDefault,
  onBack, onToggleFavourite, onSetDefault, canFavourite = true, anchorRef,
  openToken = 0,
}: Props) {
  const { theme: t } = useTheme();
  const [expanded, setExpanded] = useState(false);

  // ★ Opened from OUTSIDE by bumping `openToken` — the keyboard's Esc, which per the
  // brief opens the SERVERS menu when nothing else is open. A token rather than a
  // boolean prop so the chip keeps owning its own state: the parent asks once, and
  // closing it here does not have to be reported back and mirrored.
  const lastToken = useRef(openToken);
  useEffect(() => {
    if (openToken !== lastToken.current) { lastToken.current = openToken; setExpanded(true); }
  }, [openToken]);

  const amber = t.btnText;   // #ffb833
  const font  = t.font;      // Nixie One

  const collapse = useCallback(() => setExpanded(false), []);
  const onHeader = useCallback(() => { setExpanded(false); onBack(); }, [onBack]);

  // Soft watchOS-style shading behind the collapsed chip: a dark radial that fades
  // to transparent so the chip blends into the waterfall instead of floating as a
  // hard box — the same graduated scrim Apple puts behind the wrist clock/battery.
  const scrim = (w: number, h: number) => (
    <Canvas pointerEvents="none" style={[StyleSheet.absoluteFill, { margin: -SCRIM_PAD }]}>
      <Rect x={0} y={0} width={w + SCRIM_PAD * 2} height={h + SCRIM_PAD * 2}>
        <RadialGradient
          c={vec((w + SCRIM_PAD * 2) / 2, (h + SCRIM_PAD * 2) / 2)}
          r={(w + SCRIM_PAD * 2) / 2}
          colors={[SCRIM, 'rgba(4,3,1,0.55)', 'transparent']}
          positions={[0, 0.6, 1]}
        />
      </Rect>
    </Canvas>
  );

  return (
    // Full-screen, box-none so only the chip / dropdown / catch-layer take touches.
    <View style={StyleSheet.absoluteFill} pointerEvents="box-none">
      {/* Catch-layer: swallows the outside tap so dismissing never tunes/pans the
          waterfall, and carries a faint backdrop tint. Behind the dropdown. */}
      {expanded && (
        <Pressable
          style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.18)' }]}
          onPress={collapse}
          accessibilityRole="button"
          accessibilityLabel="Close server menu"
        />
      )}

      <View ref={anchorRef} collapsable={false} style={{ position: 'absolute', top, left }}>
        {!expanded ? (
          // ── Collapsed chip ──────────────────────────────────────────────────
          <Pressable
            onPress={() => setExpanded(true)}
            hitSlop={8}
            accessibilityRole="button"
            accessibilityLabel="Servers — tap to switch receiver or return to the list"
            style={({ pressed }) => [styles.chip, {
              borderColor: AMBER_BORDER, opacity: pressed ? 0.85 : 1,
            }]}
          >
            <MeasuredScrim render={scrim} />
            <SectionIcon name="instance" size={GLYPH} color={amber} />
            <Text style={[styles.chevron, { color: amber, fontFamily: font }]}>‹</Text>
            <Text style={[styles.label, { color: amber, fontFamily: font }]}>Servers</Text>
          </Pressable>
        ) : (
          // ── Expanded dropdown ───────────────────────────────────────────────
          <View style={[styles.menu, { borderColor: AMBER_BORDER }]}>
            {/* Header = the exit, at the chip's x-anchor (tap-tap same spot) */}
            <Pressable onPress={onHeader} style={styles.row}
              accessibilityRole="button" accessibilityLabel="Back to server list">
              <SectionIcon name="instance" size={GLYPH} color={amber} />
              <Text style={[styles.chevron, { color: amber, fontFamily: font }]}>‹</Text>
              <Text style={[styles.rowText, { color: amber, fontFamily: font }]}>Back to server list</Text>
            </Pressable>

            <View style={[styles.sep, { backgroundColor: SEP_STRONG }]} />

            {canFavourite && (
              <Pressable onPress={onToggleFavourite} style={styles.row}
                accessibilityRole="button"
                accessibilityLabel={isFavourite ? 'Remove from favourites' : 'Favourite this server'}>
                <Text style={[styles.rowGlyph, { color: amber, fontFamily: font }]}>{isFavourite ? '♥' : '♡'}</Text>
                <Text style={[styles.rowText, { color: amber, fontFamily: font }]}>{isFavourite ? 'Remove favourite' : 'Add favourite'}</Text>
              </Pressable>
            )}

            <Pressable onPress={onSetDefault} style={styles.row}
              accessibilityRole="button"
              accessibilityLabel={isDefault ? 'Clear default server' : 'Set as default server'}>
              <Text style={[styles.rowGlyph, { color: amber, fontFamily: font }]}>{isDefault ? '★' : '☆'}</Text>
              <Text style={[styles.rowText, { color: amber, fontFamily: font }]}>{isDefault ? 'Clear default' : 'Set as default'}</Text>
            </Pressable>

            {/* Collapse handle — the non-exit escape from an accidental open */}
            <Pressable onPress={collapse} style={styles.collapse} hitSlop={8}
              accessibilityRole="button" accessibilityLabel="Close server menu">
              <View style={[styles.grab, { backgroundColor: SEP }]} />
              <Text style={[styles.collapseChevron, { color: amber, fontFamily: font }]}>⌃</Text>
            </Pressable>
          </View>
        )}
      </View>
    </View>
  );
}

// Chip sizing — shrunk from the mockup so it doesn't dominate the spectrum.
const GLYPH     = 16;
const FONT_SZ   = 17;
const SCRIM_PAD = 14;   // how far the soft scrim bleeds past the pill

// The scrim Canvas needs the pill's measured size; grab it via onLayout and only
// render the gradient once we have it (a zero-size Canvas draws nothing).
function MeasuredScrim({ render }: { render: (w: number, h: number) => React.ReactNode }) {
  const [size, setSize] = useState<{ w: number; h: number } | null>(null);
  return (
    <View
      style={StyleSheet.absoluteFill}
      pointerEvents="none"
      onLayout={(e) => {
        const { width, height } = e.nativeEvent.layout;
        if (!size || Math.abs(size.w - width) > 1 || Math.abs(size.h - height) > 1) {
          setSize({ w: width, h: height });
        }
      }}
    >
      {size ? render(size.w, size.h) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  chip: {
    flexDirection: 'row', alignItems: 'center',
    paddingVertical: 6, paddingHorizontal: 11, borderRadius: 9,
    borderWidth: 1.2, backgroundColor: BACKING, gap: 6,
    // A little lift so the pill reads above the scrim.
    shadowColor: '#000', shadowOpacity: 0.5, shadowRadius: 6, shadowOffset: { width: 0, height: 1 },
  },
  chevron:  { fontSize: FONT_SZ, marginTop: -1 },
  label:    { fontSize: FONT_SZ, letterSpacing: 0.5 },
  menu: {
    minWidth: 234, borderRadius: 11, borderWidth: 1.2,
    backgroundColor: 'rgba(14,10,4,0.94)', paddingVertical: 4,
    shadowColor: '#000', shadowOpacity: 0.6, shadowRadius: 10, shadowOffset: { width: 0, height: 3 },
  },
  row: {
    flexDirection: 'row', alignItems: 'center', gap: 9,
    paddingVertical: 11, paddingHorizontal: 13,
  },
  rowGlyph:  { fontSize: 17, width: GLYPH, textAlign: 'center' },
  rowText:   { fontSize: 16, letterSpacing: 0.3 },
  sep:       { height: StyleSheet.hairlineWidth, marginHorizontal: 8, marginVertical: 1 },
  collapse:  { alignItems: 'center', paddingTop: 5, paddingBottom: 7 },
  grab:      { width: 34, height: 3, borderRadius: 2, marginBottom: 1 },
  collapseChevron: { fontSize: 15, marginTop: -3 },
});
