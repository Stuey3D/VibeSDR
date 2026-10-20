/**
 * NavCard — a card drawn over a screen (the connection cards, the idle question) that a pad or keyboard can answer
 * (controller audit group B, 2026-10-20).
 *
 * ★ Before this the seven connection cards were not in the screen's yield list: the highlight walked the controls
 *   BEHIND the card, the card's own buttons were unreachable and Back only cleared the highlight. One component, one
 *   rule: the card registers as an overlay (PanelNav useOverlay — Back = its way out, usually SERVER LIST), its keys
 *   register in reading order (`CardKey` — a flat card: every direction walks the keys), and the highlight starts on
 *   the key marked `preferred` (the card's main answer — RECONNECT, RETRY) when a pad or keyboard is in use.
 * ★ Mount it only while the card is up (it is visible for as long as it is mounted). A touch user sees no change: the
 *   ring is drawn only when focused, and only a key press focuses anything.
 */
import React from 'react';
import { TouchableOpacity, type StyleProp, type ViewStyle } from 'react-native';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, useOverlay, usePanelNav } from './PanelNav';

export function NavCard({ onBack, onEnter, blockTuneZoom, name, children }: {
  onBack: () => void;
  onEnter?: () => void;
  blockTuneZoom?: boolean;
  name?: string;
  children: React.ReactNode;
}) {
  useOverlay(true, { onBack, onEnter, blockTuneZoom, name });
  const { navCtx } = usePanelNav(true, { flat: true });
  return <NavCtx.Provider value={navCtx}>{children}</NavCtx.Provider>;
}

type CardKeyProps = {
  style?: StyleProp<ViewStyle>;
  onPress: () => void;
  /** Where the highlight starts when the card opens under a pad or keyboard. */
  preferred?: boolean;
  /** Not in the focus order (a deliberately keyboard-closed key — see compatibility mode's CONTINUE). */
  skip?: boolean;
  disabled?: boolean;
  activeOpacity?: number;
  radius?: number;
  /** A ring colour for a key whose own colour is fixed (the map's green card: the focus green would vanish on it). */
  ringColor?: string;
  children: React.ReactNode;
  [k: string]: unknown;
};

/** A key on a NavCard: its own row of a flat card (all four directions walk the keys in reading order). */
export function CardKey(p: CardKeyProps) {
  return <NavRow><CardKeyInner {...p} /></NavRow>;
}

function CardKeyInner({ style, onPress, preferred, skip, disabled, children, activeOpacity = 0.85, radius = 8, ringColor, ...rest }: CardKeyProps) {
  const { focused, viewRef } = useNavButton(disabled ? undefined : onPress, skip, { preferred });
  const focusRing = useNavFocus().ring;
  const ring = ringColor ?? focusRing;
  return (
    <TouchableOpacity ref={viewRef as never} style={style} onPress={onPress} disabled={disabled}
                      activeOpacity={activeOpacity} {...rest}>
      {children}
      <NavRing on={focused} color={ring} radius={radius} />
    </TouchableOpacity>
  );
}

