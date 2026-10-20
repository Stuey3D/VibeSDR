/**
 * PadGuide — THE CONTROLLER GUIDE: the pad drawn, numbered, with what each control does (Stuart approved the prototype
 * 2026-10-19: "controller guides look fine"). The drawings and every word are src/utils/padGuide.ts — the web page draws
 * the same data — so this file only lays them out and draws them with react-native-svg.
 *
 *  • Opened by SELECT on a game controller, which also closes it, and by the menu's CONTROLLER GUIDE key (hardwareKeys
 *    addPadGuideListener / openPadGuide). Back closes it; the D-pad left / right (or the arrow keys) moves between the
 *    two tabs, up / down scroll it. It opens on the layout in use and follows a HOLD START switch while open.
 *  • Mounted once, at the root (App.tsx), beside PadNotice, so SELECT works on every screen.
 * ★★ THE LAYOUT FOLLOWS THE WINDOW — the guide's own width (guideColumns), never the device: three columns with leader
 *    lines when there is room (an iPad, a Mac window, a phone's landscape on a big phone), the pad over a numbered list
 *    when there is not, one column on a phone held upright. The leader lines are measured from where the labels and
 *    the drawing ARE (onLayout), every time the layout changes — never fixed points.
 * ★ The popups' chassis, as KeyboardShortcuts: PopupScrim (dims only with Transparency ON), the glass card made
 *   opaque with Transparency OFF, and on silver / black the plate with the guide in a recessed window and dome keys.
 * ★ Colours by meaning: green = an action (the D-pad focus green, ink.focus), amber = BACK, the text colour = SELECT /
 *   START; the drawing keeps the prototype's warm greys, which sit on the dark card and in the dark window alike.
 */
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Modal, Pressable, ScrollView, StyleSheet, Text, View, type LayoutChangeEvent } from 'react-native';
import Svg, { Circle, G, Path, Rect, Text as SvgText } from 'react-native-svg';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useTheme } from '../contexts/ThemeContext';
import { useRepeatingKeys, noteTouchInteraction } from './PanelNav';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText,
  PopupKey, PopupPlate, PopupScrim, PopupWindow, POPUP_FONT, type PopupTokens,
} from './PopupShell';
import { addPadGuideListener, addPadNoticeListener } from '../utils/hardwareKeys';
import {
  PAD_GUIDES, PAD_LAYOUTS, guideColumns, guideTabStep, leaderPath, viewToBox,
  type ArtRole, type ArtShape, type GuideItem, type PadLayout,
} from '../utils/padGuide';

/** The prototype's drawing colours; `action` comes from the theme (the focus green). */
const ART: Record<Exclude<ArtRole, 'action' | 'none'>, string> = {
  body: '#2b2620', edge: '#6b6055', key: '#3d362f', keyEdge: '#8a7d6f', well: '#1a1713', actionBg: '#1f3a26',
};
const LINE = '#8d8173';
const NUM = 22;                 // the number badge, pt

type Lay = { x: number; y: number; w: number; h: number };
const lay = (e: LayoutChangeEvent): Lay => { const l = e.nativeEvent.layout; return { x: l.x, y: l.y, w: l.width, h: l.height }; };

function Shape({ sh, green }: { sh: ArtShape; green: string }) {
  const c = (r?: ArtRole) => (!r ? undefined : r === 'none' ? 'none' : r === 'action' ? green : ART[r]);
  const paint = { fill: c(sh.fill), stroke: c(sh.stroke), strokeWidth: sh.stroke ? sh.sw ?? 1 : undefined };
  if (sh.k === 'rect') {
    return <Rect x={sh.x} y={sh.y} width={sh.w} height={sh.h} rx={sh.rx} {...paint}
                 transform={sh.rot ? `rotate(${sh.rot[0]} ${sh.rot[1]} ${sh.rot[2]})` : undefined} />;
  }
  if (sh.k === 'circle') return <Circle cx={sh.cx} cy={sh.cy} r={sh.r} {...paint} />;
  return <Path d={sh.d} {...paint} />;
}

export default function PadGuide() {
  const [open, setOpen] = useState(false);
  const [layout, setLayout] = useState<PadLayout>('sticks');
  const openRef = useRef(false); openRef.current = open;
  const close = useCallback(() => setOpen(false), []);

  // SELECT toggles, the menu key opens; HOLD START moves the tab while open.
  useEffect(() => {
    const g = addPadGuideListener((cmd, p) => {
      if (cmd === 'toggle' && openRef.current) { setOpen(false); return; }
      setLayout(p); setOpen(true);
    });
    const n = addPadNoticeListener((_t, p) => { if (openRef.current) setLayout(p); });
    return () => { g.remove(); n.remove(); };
  }, []);

  if (!open) return null;
  return <GuideModal layout={layout} setLayout={setLayout} onClose={close} />;
}

function GuideModal({ layout, setLayout, onClose }: {
  layout: PadLayout; setLayout: (l: PadLayout) => void; onClose: () => void;
}) {
  const { theme: t } = useTheme();
  const s = usePopupStyles(makeS);
  const pt = usePopupTheme();
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(16, false);
  const insets = useSafeAreaInsets();
  const ff = pt.metal ? POPUP_FONT : t.font;
  const green = pt.ink.focus;
  const amber = pt.gold.amber;
  const ink = pt.metal ? pt.winText : pt.ink.text;
  const dim = pt.metal ? pt.winDim : pt.ink.textDim;
  const g = PAD_GUIDES[layout];

  const scroll = useRef<ScrollView | null>(null);
  const y = useRef(0);
  const layoutRef = useRef(layout); layoutRef.current = layout;

  // ★ The guide is a picture, not a form: left / right are the tabs, up / down scroll, back closes, enter does nothing
  //   (consumed, so it cannot press a control hidden under the guide). Tune and zoom pass through — try the stick and
  //   watch the dial move behind it.
  useRepeatingKeys(true, (k: string) => {
    if (k === 'Back') { onClose(); return true; }
    if (k === 'Enter') return true;
    if (k === 'ArrowLeft' || k === 'ArrowRight') { setLayout(guideTabStep(layoutRef.current, k === 'ArrowRight' ? 1 : -1)); return true; }
    if (k === 'ArrowUp' || k === 'ArrowDown') {
      y.current = Math.max(0, y.current + (k === 'ArrowDown' ? 120 : -120));
      scroll.current?.scrollTo({ y: y.current, animated: false });
      return true;
    }
  }, ['ArrowUp', 'ArrowDown'], { priority: 100 });

  // ── Layout, measured ──
  const [bodyW, setBodyW] = useState(0);
  const [winH, setWinH] = useState(0);
  const cols = bodyW ? guideColumns(bodyW) : 1;
  const [row, setRow] = useState<Lay | null>(null);
  const [pad, setPad] = useState<Lay | null>(null);
  const [colL, setColL] = useState<Lay | null>(null);
  const [colR, setColR] = useState<Lay | null>(null);
  const labs = useRef<Record<string, Lay>>({});
  const nums = useRef<Record<string, Lay>>({});
  const [tick, setTick] = useState(0);
  const bump = useCallback(() => setTick((n) => n + 1), []);
  // ★ Measurements are kept per layout and column count, so a tab switch never reads the other tab's positions.
  const mk = (i: number) => `${layout}:${cols}:${i}`;

  const [vx, vy, vw, vh] = g.view;
  const aspect = vh / vw;
  // The pad's size: its column's width; capped by height in the list layouts so the list starts on screen.
  const padW = (() => {
    if (!bodyW) return 0;
    const avail = cols === 3 ? bodyW * (2.2 / 4.2) - 20 : bodyW;
    const cap = cols === 3 ? (winH ? (winH - 140) / aspect : avail) : (winH ? (winH * 0.5) / aspect : avail);
    return Math.max(120, Math.min(avail, cap));
  })();

  const lines = useMemo(() => {
    if (cols !== 3 || !row || !pad || !colL || !colR) return '';
    let d = '';
    g.items.forEach((it, i) => {
      const l = labs.current[mk(i)], n = nums.current[mk(i)];
      if (!l || !n) return;
      const col = it.side === 'left' ? colL : colR;
      const nx = col.x + l.x + n.x, ny = col.y + l.y + n.y;
      const [px, py] = viewToBox(g.view, pad.w, pad.h, it.at[0], it.at[1]);
      const x1 = it.side === 'left' ? nx + n.w + 6 : nx - 6;
      d += leaderPath(pad.x + px, pad.y + py, x1, ny + n.h / 2, it.side);
    });
    return d;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cols, row, pad, colL, colR, tick, layout]);

  const label = (it: GuideItem, i: number, side: 'left' | 'right') => (
    <View key={mk(i)} style={[s.lab, cols === 3 && side === 'left' && s.labRev]}
          onLayout={(e) => { labs.current[mk(i)] = lay(e); bump(); }}>
      <View style={[s.num, { borderColor: green, backgroundColor: ART.actionBg }]}
            onLayout={(e) => { nums.current[mk(i)] = lay(e); bump(); }}>
        <Text style={[s.numTxt, { color: green }]}>{i + 1}</Text>
      </View>
      <View style={s.labTxt}>
        <Text style={[s.labT, { fontFamily: ff, color: it.kind === 'back' ? amber : it.kind === 'meta' ? ink : green },
                      cols === 3 && side === 'left' && s.right]}>{it.t}</Text>
        {it.s ? <Text style={[s.labS, { fontFamily: ff, color: ink }, cols === 3 && side === 'left' && s.right]}>{it.s}</Text> : null}
      </View>
    </View>
  );
  const column = (side: 'left' | 'right') => g.items.map((it, i) => (it.side === side ? label(it, i, side) : null));

  const drawing = (
    <Svg width={padW} height={padW * aspect} viewBox={`${vx} ${vy} ${vw} ${vh}`} accessibilityLabel={g.title}>
      {g.art.map((sh, i) => <Shape key={i} sh={sh} green={green} />)}
      {g.items.map((it, i) => (
        <G key={`m${i}`}>
          <Circle cx={it.at[0]} cy={it.at[1]} r={13} fill={ART.actionBg} stroke={green} strokeWidth={2.5} />
          <SvgText x={it.at[0]} y={it.at[1] + 6} fontSize={17} fontWeight="700" fill={green} textAnchor="middle">{String(i + 1)}</SvgText>
        </G>
      ))}
    </Svg>
  );

  return (
    <Modal visible transparent animationType="fade" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <PopupScrim style={s.backdrop} color="rgba(0,0,0,0.6)" onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View pointerEvents="box-none" style={[s.frame, {
        paddingTop: Math.max(insets.top, 12) + 4, paddingBottom: Math.max(insets.bottom, 12) + 4,
        paddingLeft: Math.max(insets.left, 12), paddingRight: Math.max(insets.right, 12) }]}>
        <View style={[s.card, { borderColor: pt.ink.border }, surf.opaque && !pt.metal && { backgroundColor: surf.fill(CARD_BG) },
                      metalFrame]} onTouchStart={noteTouchInteraction}>
          <PopupPlate radius={16} />
          <View style={s.head}>
            <Text style={[s.title, { fontFamily: ff }]}>CONTROLLER GUIDE</Text>
            <Text style={[s.hint, { fontFamily: ff }]} numberOfLines={1}>◄ ► layout · back or SELECT closes</Text>
          </View>
          <View style={s.tabs}>
            {PAD_LAYOUTS.map((l) => pt.metal ? (
              <PopupKey key={l} label={PAD_GUIDES[l].tab} active={l === layout} pip onPress={() => setLayout(l)}
                        height={30} style={s.tabMetal} />
            ) : (
              <Pressable /* nav-skip: left/right switch the tabs */ key={l} onPress={() => setLayout(l)} accessibilityRole="button" accessibilityState={{ selected: l === layout }}
                         style={[s.tab, l === layout ? { borderColor: pt.gold.selBorder, backgroundColor: pt.ink.keyBgAct }
                                                     : { borderColor: pt.ink.keyBorder }]}>
                <Text style={[s.tabTxt, { fontFamily: t.font, color: l === layout ? pt.gold.sel : pt.ink.key }]}>{PAD_GUIDES[l].tab}</Text>
              </Pressable>
            ))}
          </View>
          <PopupWindow style={s.bodyWrap} metalStyle={s.bodyWin} onLayout={(e) => setWinH(e.nativeEvent.layout.height)}>
            <ScrollView ref={scroll} style={s.body} scrollEventThrottle={32}
                        onScroll={(e) => { y.current = e.nativeEvent.contentOffset.y; }}
                        contentContainerStyle={s.bodyIn}>
              <View onLayout={(e) => setBodyW(e.nativeEvent.layout.width)}>
                <Text style={[s.gTitle, { fontFamily: ff, color: amber }]}>{g.title}</Text>
                <Text style={[s.gSub, { fontFamily: ff, color: dim }]}>{g.sub}</Text>
                {cols === 3 ? (
                  <View style={s.row} onLayout={(e) => setRow(lay(e))}>
                    <View style={[s.col, s.colL]} onLayout={(e) => setColL(lay(e))}>{column('left')}</View>
                    <View style={s.padCell} onLayout={(e) => setPad(lay(e))}>{drawing}</View>
                    <View style={[s.col, s.colR]} onLayout={(e) => setColR(lay(e))}>{column('right')}</View>
                    <Svg pointerEvents="none" style={StyleSheet.absoluteFill}>
                      <Path d={lines} fill="none" stroke={LINE} strokeWidth={1.5} />
                    </Svg>
                  </View>
                ) : (
                  <>
                    <View style={s.padTop}>{drawing}</View>
                    <View style={[s.list, cols === 2 && s.list2]}>
                      <View style={[s.listCol, cols === 2 && s.listCol2]}>{column('left')}</View>
                      <View style={[s.listCol, cols === 2 && s.listCol2]}>{column('right')}</View>
                    </View>
                  </>
                )}
                <Text style={[s.foot, { fontFamily: ff, color: dim }]}>{g.foot}</Text>
                {/* ★ The controller keyboard (decided 10-20) — what each control does while typing. */}
                {g.keyboard.map((line, i) => (
                  <Text key={i} style={[s.foot, { fontFamily: ff, color: dim, marginTop: i ? 2 : 12 }]}>{line}</Text>
                ))}
              </View>
            </ScrollView>
          </PopupWindow>
          {pt.metal ? (
            <PopupKey label="CLOSE" onPress={onClose} height={32} style={{ alignSelf: 'center', width: 110, marginVertical: 10 }} />
          ) : (
            <Pressable /* nav-skip: Back / SELECT close it */ style={s.closeBtn} onPress={onClose} onTouchStart={noteTouchInteraction}>
              <Text style={[s.closeTxt, { fontFamily: t.font }]}>CLOSE</Text>
            </Pressable>
          )}
        </View>
      </View>
    </Modal>
  );
}

/** Today's card glass — made opaque with Transparency OFF (KeyboardShortcuts' own). */
const CARD_BG = 'rgba(8,10,9,0.98)';

const makeS = (pt: PopupTokens) => StyleSheet.create({
  backdrop: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  frame: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center' },
  card: {
    flex: 1, width: '100%', maxWidth: 1240,
    backgroundColor: CARD_BG, borderWidth: 1, borderRadius: 12, overflow: 'hidden',
  },
  head: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10,
    paddingHorizontal: 14, paddingVertical: 10,
    ...(pt.metal ? { borderBottomWidth: 0 } : { borderBottomWidth: 1, borderBottomColor: pt.ink.divider }),
  },
  title: onMetal(pt, { color: pt.ink.text, fontSize: 13, letterSpacing: 3, fontWeight: '700' }, engraveText(pt)),
  hint:  onMetal(pt, { color: pt.ink.textDim, fontSize: 10, letterSpacing: 0.5, flexShrink: 1 }, engraveText(pt, pt.note)),
  tabs: { flexDirection: 'row', justifyContent: 'center', gap: 8, paddingVertical: 8, flexWrap: 'wrap' },
  tab: { borderWidth: 1, borderRadius: 8, paddingVertical: 7, paddingHorizontal: 14 },
  tabTxt: { fontSize: 12, fontWeight: '700', letterSpacing: 1 },
  tabMetal: { width: 130 },
  bodyWrap: { flex: 1 },
  bodyWin:  { marginHorizontal: 10 },
  body:  { flex: 1, paddingHorizontal: 14 },
  bodyIn: { flexGrow: 1, justifyContent: 'center', paddingVertical: 12 },
  gTitle: { textAlign: 'center', fontSize: 20, fontWeight: '700', letterSpacing: 3 },
  gSub: { textAlign: 'center', fontSize: 12, marginTop: 4, marginBottom: 8 },
  foot: { textAlign: 'center', fontSize: 12, lineHeight: 18, marginTop: 12 },
  row: { flexDirection: 'row', alignItems: 'center' },
  col: { flex: 1, gap: 16 },
  colL: { alignItems: 'flex-end', paddingRight: 10 },
  colR: { alignItems: 'flex-start', paddingLeft: 10 },
  padCell: { flex: 2.2, alignItems: 'center' },
  padTop: { alignItems: 'center', marginBottom: 12 },
  list: { gap: 12 },
  list2: { flexDirection: 'row', gap: 16 },
  listCol: { gap: 12 },
  listCol2: { flex: 1 },
  lab: { flexDirection: 'row', alignItems: 'flex-start', gap: 8, maxWidth: 300 },
  labRev: { flexDirection: 'row-reverse' },
  labTxt: { flexShrink: 1 },
  right: { textAlign: 'right' },
  num: { width: NUM, height: NUM, borderRadius: NUM / 2, borderWidth: 1.5, alignItems: 'center', justifyContent: 'center' },
  numTxt: { fontSize: 11, fontWeight: '700' },
  labT: { fontSize: 14, fontWeight: '700', letterSpacing: 0.4 },
  labS: { fontSize: 12, opacity: 0.85, marginTop: 1 },
  closeBtn: {
    borderTopWidth: 1, borderTopColor: pt.ink.divider,
    paddingVertical: 14, alignItems: 'center',
  },
  closeTxt: { color: pt.gold.close, fontSize: 12, letterSpacing: 2, fontWeight: '700' },
});
