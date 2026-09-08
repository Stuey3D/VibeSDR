/**
 * DabPlusBadge — the official WorldDAB DAB+ logo, as a badge.
 *
 * Drawn ONLY beside a receiver whose own status says it can decode DAB (`dab: true` in its
 * /vibeserver.json): it is a statement about that receiver, not decoration. The artwork is the
 * toolkit's colour SVG, unaltered — see assets/branding/dabplus/README.md for the terms, including
 * the 32 px minimum width the guide sets for screens.
 */
import React from 'react';
import { SvgXml } from 'react-native-svg';
import { DABPLUS_LOGO_SVG } from '../assets/dabplusLogo';

export default function DabPlusBadge({ width = 36 }: { width?: number }) {
  // The master is 100 x 59.
  return <SvgXml xml={DABPLUS_LOGO_SVG} width={width} height={Math.round(width * 0.59)} />;
}
