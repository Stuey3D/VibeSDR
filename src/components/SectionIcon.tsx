/**
 * SectionIcon — small monochrome stroke glyphs for menu section headers, drawn
 * in the section-label colour to match the app's existing icon language
 * (VfoLockIcon, the Skia controls glyphs). One glyph per section so the dense
 * menu is scannable at a glance regardless of the reader's language.
 *
 * Two icons form an "input │ output" family sharing a centre divider:
 *   nr       — noisy waveform │ clean sine   (noise reduction)
 *   decoder  — radio wave     │ letter A      (signal → character)
 */
import React from 'react';
import Svg, { Path, Circle, Rect } from 'react-native-svg';

export type SectionIconName =
  | 'audio' | 'nr' | 'decoder' | 'station' | 'spectrum' | 'controls'
  | 'maps' | 'profile' | 'admin' | 'instance' | 'hardware' | 'dab'
  | 'spots' | 'server' | 'monitor' | 'zoomIn' | 'zoomOut';

const DEFAULT_COLOR = 'rgba(180,190,210,0.85)';

export default function SectionIcon({
  name, size = 16, color = DEFAULT_COLOR,
}: { name: SectionIconName; size?: number; color?: string }) {
  const p = (d: string, w = 1.7) => (
    <Path d={d} fill="none" stroke={color} strokeWidth={w} strokeLinecap="round" strokeLinejoin="round" />
  );
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24">
      {glyph(name, color, p)}
    </Svg>
  );
}

function glyph(
  name: SectionIconName,
  color: string,
  p: (d: string, w?: number) => React.ReactElement,
) {
  switch (name) {
    case 'audio':      // speaker + sound waves
      return <>{p('M4 9h3l4-4v14l-4-4H4z')}{p('M15.5 9a3.5 3.5 0 0 1 0 6')}{p('M18 6.5a7 7 0 0 1 0 11')}</>;
    case 'nr':         // noisy waveform │ clean sine
      return <>{p('M1.6 13 3 12l1.5 3.8L6 7.4l1.6 9.4L9 12.6h1.2')}{p('M12 6v12')}{p('M14.3 12.6c1.1-2.7 2.3-2.7 3.5 0s2.3 2.7 3.5 0')}</>;
    case 'decoder':    // radio wave │ letter A
      return <>{p('M1.8 12.5c1.2-3 2.4-3 3.6 0s2.4 3 3.6 0', 1.8)}{p('M12 6v12', 1.8)}{p('M14.2 18 17 7.2 19.8 18', 1.8)}{p('M15.1 14.2h3.8', 1.8)}</>;
    case 'station':    // location pin
      return <>{p('M12 21s-6-5.3-6-10a6 6 0 1 1 12 0c0 4.7-6 10-6 10Z')}<Circle cx={12} cy={11} r={2} fill="none" stroke={color} strokeWidth={1.7} /></>;
    case 'zoomIn':     // magnifier with + — the zoom drum's glass (DrumWheel buildIconPath), at 24
      return <><Circle cx={10} cy={10} r={6.5} fill="none" stroke={color} strokeWidth={1.9} />{p('M14.8 14.8 21 21', 2.2)}{p('M7 10h6M10 7v6', 1.9)}</>;
    case 'zoomOut':    // magnifier with −
      return <><Circle cx={10} cy={10} r={6.5} fill="none" stroke={color} strokeWidth={1.9} />{p('M14.8 14.8 21 21', 2.2)}{p('M7 10h6', 1.9)}</>;
    case 'spectrum':   // spectrum bars
      return p('M4 15v3M8 9v9M12 5v13M16 11v7M20 8v10');
    case 'controls':   // faders
      return <>{p('M6 4v16M12 4v16M18 4v16')}<Circle cx={6} cy={9} r={2} fill="none" stroke={color} strokeWidth={1.7} /><Circle cx={12} cy={15} r={2} fill="none" stroke={color} strokeWidth={1.7} /><Circle cx={18} cy={7} r={2} fill="none" stroke={color} strokeWidth={1.7} /></>;
    case 'maps':       // folded map
      return <>{p('M9 4 3 6v14l6-2 6 2 6-2V4l-6 2-6-2Z', 1.6)}{p('M9 4v14M15 6v14', 1.6)}</>;
    case 'profile':    // dropdown list: radio header (fat bar) → indented profile rows
      return <>{p('M4 6h16', 4.4)}{p('M8.5 13h11', 2.8)}{p('M12.5 18.4h7', 2.8)}</>;
    case 'admin':      // wrench
      return p('M15.5 4.5a4 4 0 0 0-5 5L4 16v4h4l6.5-6.5a4 4 0 0 0 5-5l-2.8 2.8-2.2-.6-.6-2.2 2.6-2.8Z');
    case 'instance':   // network nodes
      return <><Circle cx={6} cy={7} r={2} fill="none" stroke={color} strokeWidth={1.7} /><Circle cx={18} cy={7} r={2} fill="none" stroke={color} strokeWidth={1.7} /><Circle cx={12} cy={18} r={2} fill="none" stroke={color} strokeWidth={1.7} />{p('M8 7h8M7.2 8.7 10.8 16.3M16.8 8.7 13.2 16.3')}</>;
    case 'hardware':   // IC chip with pins
      return <><Rect x={6.5} y={6.5} width={11} height={11} rx={1.5} fill="none" stroke={color} strokeWidth={1.7} />{p('M9.5 6.5V3.5M14.5 6.5V3.5M9.5 20.5v-3M14.5 20.5v-3M6.5 9.5h-3M6.5 14.5h-3M20.5 9.5h-3M20.5 14.5h-3')}</>;
    case 'dab':        // broadcast arcs
      return <><Circle cx={6} cy={18} r={1.5} fill={color} />{p('M6 13a5 5 0 0 1 5 5')}{p('M6 8a10 10 0 0 1 10 10')}</>;
    case 'spots':      // signal spots (map markers)
      return <><Circle cx={6} cy={16} r={1.5} fill={color} /><Circle cx={12} cy={8.5} r={1.5} fill={color} /><Circle cx={17.5} cy={14} r={1.5} fill={color} />{p('M7.3 15.2 10.9 9.6M13.2 9.4 16.4 12.8', 1.3)}</>;
    case 'monitor':    // display / monitor on a stand
      return <><Rect x={3} y={4.5} width={18} height={12} rx={1.5} fill="none" stroke={color} strokeWidth={1.7} />{p('M9 20.5h6M12 16.5v4')}</>;
    case 'server':     // server rack (OpenWebRX)
      return <><Rect x={4} y={4.5} width={16} height={6.5} rx={1.5} fill="none" stroke={color} strokeWidth={1.7} /><Rect x={4} y={13} width={16} height={6.5} rx={1.5} fill="none" stroke={color} strokeWidth={1.7} /><Circle cx={7.5} cy={7.75} r={0.8} fill={color} /><Circle cx={7.5} cy={16.25} r={0.8} fill={color} />{p('M11 7.75h5M11 16.25h5', 1.5)}</>;
  }
}
