/**
 * VTSBar — accessibility-skin popup notification bar (lsv-a11y-notif parity).
 * Pops up above the controls pill for 8s when:
 *   - the tuned frequency lands on / near a bookmark (station name + offset)
 *   - a band-plan boundary is crossed (band info, ham conditions colouring)
 * The skin's "TAP ◄ ► TO JUMP" tuning-guide hint is intentionally NOT ported
 * (it was erratic on the popup bar).
 * Overflowing text slides across once, like the skin's a11y-scrolling.
 */

import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, Image, ScrollView, StyleSheet, Text } from 'react-native';

// Official RDS mark — shown in place of the text badge when the live data is
// genuine RDS (black logo sits in a white pill against the dark bar).
const RDS_LOGO = require('../../assets/rds-logo.png');

// Bookmark-source marks (uniform with the RDS logo): the backend logo for a
// server bookmark, an "EiBi" text mark for the on-device EiBi schedule, and a
// phone glyph for the user's own (local) bookmarks.
const SERVER_LOGOS: Record<string, any> = {
  ubersdr: require('../../assets/logo_ubersdr.png'),
  owrx:    require('../../assets/logo_owrx.png'),
  kiwi:    require('../../assets/logo_kiwi.png'),
};

export interface VtsNotifData {
  key:        number;   // bump to re-trigger even with identical text
  name:       string;
  secondary?: string;   // overlap band names (band notifs only)
  offset?:    string;   // "-1.2kHz" distance to the station
  tuneDir?:   'left' | 'right';  // which way to tune to reach it
  kind:       'station-on' | 'station-off' | 'band';
  color?:     string;   // band-condition override for the primary text
  hold?:      boolean;  // stay up (no auto-dismiss) — digital-voice caller display
  badge?:     string;   // live-data tag (e.g. 'RDS', 'DMR') — shown before the name
  source?:    'eibi' | 'server' | 'user';  // bookmark origin → source icon
  flag?:      string;   // transmitter-country flag (EiBi bookmarks / RDS)
  logoUrl?:   string;   // resolved WFM RDS station logo (radio-browser favicon)
}

const NOTIF_MS = 8000;

const COL = {
  onTune:  'rgba(80,220,100,0.95)',
  offTune: 'rgba(255,200,80,0.95)',
  band:    '#ffe566',
  dim:     'rgba(255,255,255,0.35)',
  sub:     'rgba(255,255,255,0.55)',
};

export default function VTSBar({ notif, bottom, serverType, onHeight }:
    { notif: VtsNotifData | null; bottom: number; serverType?: string; onHeight?: (h: number) => void }) {
  const [shown, setShown] = useState<VtsNotifData | null>(null);
  const shownRef = useRef<VtsNotifData | null>(null);
  const fade    = useRef(new Animated.Value(0)).current;
  const slide   = useRef(new Animated.Value(0)).current;
  const hideRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [areaW, setAreaW] = useState(0);
  const [textW, setTextW] = useState(0);

  // Report height 0 the moment we're hidden, so anything stacked above us (the decoder box)
  // drops back down instead of floating over the gap where the VTS used to be.
  useEffect(() => { if (!shown) onHeight?.(0); }, [shown, onHeight]);

  const dismiss = () => {
    if (hideRef.current) { clearTimeout(hideRef.current); hideRef.current = null; }
    Animated.timing(fade, { toValue: 0, duration: 300, useNativeDriver: true })
      .start(() => { setShown(null); shownRef.current = null; });
  };

  useEffect(() => {
    if (!notif) {
      // Explicit clear (e.g. live data ended / mode change) — a held (live) notif
      // has no auto-dismiss timer, so fade it out here. Timed notifs self-dismiss.
      if (shownRef.current?.hold) dismiss();
      return;
    }
    setShown(notif);
    shownRef.current = notif;
    setTextW(0);
    slide.setValue(0);
    fade.setValue(0);
    Animated.timing(fade, { toValue: 1, duration: 180, useNativeDriver: true }).start();
    if (hideRef.current) { clearTimeout(hideRef.current); hideRef.current = null; }
    // Live data (RDS/DMR/DAB) holds on screen; static (bookmark/band) times out.
    if (!notif.hold) {
      hideRef.current = setTimeout(() => {
        Animated.timing(fade, { toValue: 0, duration: 300, useNativeDriver: true })
          .start(() => { setShown(null); shownRef.current = null; });
      }, NOTIF_MS);
    }
    return () => { if (hideRef.current) clearTimeout(hideRef.current); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [notif?.key, notif === null]);

  // Slide for overflowing text. Held (live) notifs loop continuously so the long
  // RDS radiotext keeps marqueeing; timed notifs do the skin's one-shot a11y slide.
  useEffect(() => {
    if (!shown || !areaW || !textW || textW <= areaW) return;
    const dist = textW - areaW;
    slide.setValue(0);
    if (shown.hold) {
      const dur = Math.max(2200, dist * 16);   // scroll speed scales with length
      const anim = Animated.loop(Animated.sequence([
        Animated.delay(1200),
        Animated.timing(slide, { toValue: -dist, duration: dur, easing: Easing.linear, useNativeDriver: true }),
        Animated.delay(1500),
        Animated.timing(slide, { toValue: 0, duration: 0, useNativeDriver: true }),
      ]));
      anim.start();
      return () => anim.stop();
    }
    Animated.timing(slide, {
      toValue: -dist,
      duration: NOTIF_MS - 2500,
      delay: 900,
      easing: Easing.linear,
      useNativeDriver: true,
    }).start();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shown?.key, areaW, textW]);

  if (!shown) return null;

  const onTune  = shown.kind === 'station-on';
  const isBand  = shown.kind === 'band';
  const nameCol = shown.color ?? (isBand ? COL.band : onTune ? COL.onTune : COL.offTune);
  // Arrows: green pair when on tune; otherwise the side you need to tune
  // toward lights amber, the other dims (skin vts-arrow-active/dim)
  const leftCol  = onTune ? COL.onTune : shown.tuneDir === 'left' ? COL.offTune : COL.dim;
  const rightCol = onTune ? COL.onTune : shown.tuneDir === 'right' ? COL.offTune : COL.dim;
  const tuneLeft = shown.tuneDir === 'left';
  const overflow = textW > areaW && areaW > 0;

  return (
    <Animated.View style={[styles.wrap, { bottom, opacity: fade }]} pointerEvents="none"
      onLayout={(e) => onHeight?.(e.nativeEvent.layout.height)}>
      <Text style={[styles.arrow, { color: leftCol }]}>◄</Text>
      {/* Source mark: live-data badge (RDS logo / text) wins; otherwise the
          bookmark-origin icon — backend logo, EiBi mark, or phone glyph. */}
      {shown.logoUrl
        ? <Image source={{ uri: shown.logoUrl }} style={styles.staLogo} resizeMode="contain" />
        : shown.badge === 'RDS'
        ? <Image source={RDS_LOGO} style={styles.rdsLogo} resizeMode="contain" />
        : !!shown.badge
          ? <Text style={styles.badge}>{shown.badge}</Text>
          : shown.source === 'server'
            ? <Image source={SERVER_LOGOS[serverType ?? 'ubersdr'] ?? SERVER_LOGOS.ubersdr}
                style={[styles.srcLogo, serverType === 'owrx' && styles.srcLogoLight]} resizeMode="contain" />
            : shown.source === 'eibi'
              ? <Text style={styles.eibiMark}>EiBi</Text>
              : shown.source === 'user'
                ? <Text style={styles.phoneMark}>📱</Text>
                : null}
      {!!shown.flag && <Text style={styles.flag}>{shown.flag}</Text>}
      {!!shown.offset && tuneLeft && <Text style={styles.offset}>{shown.offset}</Text>}
      {/* Horizontal ScrollView = unconstrained content width, so the text
          measures at its TRUE size (a plain View clamps Text to the parent
          width and the overflow slide never triggers). scrollEnabled off —
          the slide is driven by the Animated translateX. */}
      <ScrollView
        horizontal
        scrollEnabled={false}
        showsHorizontalScrollIndicator={false}
        style={styles.nameArea}
        contentContainerStyle={!overflow ? styles.nameCentre : undefined}
        onLayout={(e: { nativeEvent: { layout: { width: number } } }) => setAreaW(e.nativeEvent.layout.width)}
        onContentSizeChange={(w: number) => setTextW(w)}
      >
        <Animated.View style={overflow ? { transform: [{ translateX: slide }] } : undefined}>
          <Text style={[styles.name, { color: nameCol }]} numberOfLines={1}>
            {shown.name}
            {shown.secondary ? <Text style={styles.secondary}>{'  │  ' + shown.secondary}</Text> : null}
          </Text>
        </Animated.View>
      </ScrollView>
      {!!shown.offset && shown.tuneDir === 'right' && <Text style={styles.offset}>{shown.offset}</Text>}
      <Text style={[styles.arrow, { color: rightCol }]}>►</Text>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  wrap: {
    position: 'absolute',
    left: 14,
    right: 14,
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: 11,
    paddingHorizontal: 12,
    borderRadius: 12,
    backgroundColor: 'rgba(8,10,14,0.94)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.22)',
    zIndex: 60,
  },
  arrow: {
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 15,
    paddingHorizontal: 4,
  },
  badge: {
    color: '#0a0a0a',
    backgroundColor: '#52dc64',   // live-data accent (matches on-tune green)
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 0.5,
    overflow: 'hidden',
    borderRadius: 4,
    paddingHorizontal: 5,
    paddingVertical: 2,
    marginRight: 4,
  },
  rdsLogo: {
    width: 46,
    height: 15,
    backgroundColor: '#ffffff',
    borderRadius: 4,
    paddingHorizontal: 3,
    marginRight: 5,
  },
  staLogo: {
    width: 20,
    height: 20,
    backgroundColor: '#ffffff',
    borderRadius: 4,
    marginRight: 5,
  },
  srcLogo: {
    width: 26,
    height: 16,
    marginRight: 5,
  },
  // OWRX's logo is a black antenna that vanishes on the dark bar — sit it on a
  // light chip, same as the menu footer.
  srcLogoLight: {
    backgroundColor: '#ffffff',
    borderRadius: 4,
    paddingHorizontal: 3,
    width: 30,
  },
  eibiMark: {
    color: '#0a0a0a',
    backgroundColor: '#7fb3ff',   // distinct from the green live badge
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 10.5,
    fontWeight: '700',
    letterSpacing: 0.3,
    overflow: 'hidden',
    borderRadius: 4,
    paddingHorizontal: 5,
    paddingVertical: 2,
    marginRight: 5,
  },
  phoneMark: {
    fontSize: 14,
    marginRight: 4,
  },
  flag: {
    fontSize: 15,
    marginRight: 3,
  },
  offset: {
    color: 'rgba(255,200,80,0.85)',
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 13,
    paddingHorizontal: 2,
  },
  nameArea: {
    flex: 1,
    marginHorizontal: 6,
  },
  nameCentre: {
    flexGrow: 1,
    justifyContent: 'center',
  },
  name: {
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 16,
    letterSpacing: 0.5,
  },
  secondary: {
    color: COL.sub,
    fontSize: 14,
  },
});
