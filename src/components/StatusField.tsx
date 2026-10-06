/**
 * StatusField — one run of the controls' STATUS ROW in the Display's own cells (2026-10-06): VCR draws it through
 * SegField (DSEG14, every segment a ghost, the 60° mesh), DOT through DotField (Doto's 5 × 7 dots, every dot a ghost).
 * The cells are decided by constants/statusField.ts (pure, tested); this file draws them and the LOGOS.
 *
 * ★★ THE LOGOS ARE ELECTRODES on VCR, the RDS mark's treatment through the same code (vfdMesh.ts): each is cut into
 *   separate electrodes where a real annunciator's would be — the server mark's three nodes and three links, the
 *   bars, the ✕'s four arms, the gain arrow's stem and two heads, the bolt's two halves, the key's ring and its
 *   shaft, the listener's head and shoulders — ghosted at the text colour's α .10, lit in the run's colour, and
 *   meshed with the cells by SegField (its extras are inside the mesh). Not forked: SegField and DotField draw them.
 * ★ ONE CANVAS PER RUN — the runs are the row's own items (it drops them one by one), so they stay separate Views
 *   for its measuring. The MEASURING twin draws no canvas at all: inside StatusGhostContext a run is an empty box of
 *   its computed width (its width is arithmetic — n cells at a fixed pitch).
 */

import React, { useContext, useMemo } from 'react';
import { View } from 'react-native';
import { SegField, segCellX, type SegExtra } from './SegField';
import { DotField, type DotMark } from './DotField';
import { framePad, rect, segLogo, segLogoLit } from './statusLogos';
import { segFieldCells } from '../constants/displayText';
import { dotChar } from '../constants/dotField';
import {
  statusDotCols, statusDotMarks, statusDotPitch, statusDotSlots, statusLineH, statusSegFs, statusSegSlots, statusSegWidth,
  STATUS_SEG_ADV, type StatusPart,
} from '../constants/statusField';

/** True inside the status row's MEASURING twin: runs draw nothing but their box. */
export const StatusGhostContext = React.createContext(false);

const segRunCells = segFieldCells;

export interface StatusRunProps {
  parts: readonly StatusPart[];
  face: 'seg' | 'dot';
  /** The status display's type size (StatusDisplay.size) — the Doto size these cells replace. */
  size: number;
  color: string;
  glow: string | null;
  ghostColor: string;
  /** What a screen reader says (the run's text as written). */
  label?: string;
  /** VCR: frame these [first, last] cell spans (the DSP badges). */
  frames?: ReadonlyArray<readonly [number, number]>;
  opacity?: number;
}

export const StatusRun = React.memo(function StatusRun({ parts, face, size, color, glow, ghostColor, label, frames,
  opacity }: StatusRunProps) {
  const measuring = useContext(StatusGhostContext);
  const partsKey = JSON.stringify(parts);
  const framesKey = frames ? JSON.stringify(frames) : '';
  const lineH = statusLineH(size);
  const fs = statusSegFs(size), pitch = statusDotPitch(size);
  const slots = useMemo(() => (face === 'seg' ? statusSegSlots(parts, segRunCells) : statusDotSlots(parts, dotChar)),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [partsKey, face]);
  const n = slots.cells.length;
  const pad = face === 'seg' && frames && frames.length ? framePad(fs) + 0.5 : 0;

  // VCR: the logos' electrodes and the frames, built once per shape × place × size (path ops — never per reading:
  // the bars' q and the arrow's direction only change which are lit).
  const shapeKey = slots.logos.map(({ at, logo }) => `${logo.kind}@${at}`).join(',');
  const seg = useMemo(() => {
    if (face !== 'seg' || measuring) return null;
    const extras: SegExtra[] = [];
    const keys: string[] = [];
    for (const { at, logo } of slots.logos) {
      extras.push(...segLogo(logo, at, fs));
      keys.push(`${logo.kind}@${at}`);
    }
    for (const [a, b] of frames ?? []) {
      const fp = framePad(fs), w = Math.max(0.6, fs * 0.07);
      const x1 = segCellX(a, fs) - fp, x2 = segCellX(b, fs) + STATUS_SEG_ADV * fs + fp;
      const f = rect(x1, -fp, x2 - x1, fs + 2 * fp);
      extras.push({ path: f.stroke({ width: w }) ?? f, lit: true });
      keys.push(`frame${a}-${b}`);
    }
    return { extras, key: `${keys.join(',')}|${fs}` };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [face, measuring, shapeKey, framesKey, fs]);
  const segExtras = useMemo(() => {
    if (!seg) return [];
    const lits = slots.logos.flatMap(({ logo }) => segLogoLit(logo));
    return seg.extras.map((e, k) => (k < lits.length ? { path: e.path, lit: lits[k] } : e));
  }, [seg, slots]);

  // DOT: the text cells' columns, and the marks.
  const dot = useMemo(() => {
    if (face !== 'dot') return null;
    const cellCols: number[] = [], lit: string[] = [];
    slots.ghost.forEach((g, i) => { if (g) { cellCols.push(i * 6); lit.push(slots.cells[i]); } });
    return { cellCols, lit, marks: statusDotMarks(slots) as DotMark[] };
  }, [face, slots]);

  const width = face === 'seg' ? statusSegWidth(n, fs) : statusDotCols(n) * pitch;
  if (measuring) return <View style={{ width: width + 2 * pad, height: lineH }} />;
  return (
    <View style={{ height: lineH, justifyContent: 'center', paddingHorizontal: pad, opacity }}
          accessibilityRole="text" accessibilityLabel={label}>
      {face === 'seg' ? (
        <SegField fs={fs} width={width} ghost={slots.ghost} lit={slots.cells} gapColons={slots.colons}
          extras={segExtras} extrasKey={seg?.key ?? ''} color={color} glow={glow} ghostColor={ghostColor} />
      ) : dot && (
        <DotField pitch={pitch} cols={statusDotCols(n)} cellCols={dot.cellCols} lit={dot.lit} marks={dot.marks}
          color={color} glow={glow} ghostColor={ghostColor} />
      )}
    </View>
  );
});
