// web/community/kit.ts — ★★ V12 beta94 THE COMMUNITY MAP's DRAWING KIT: the server maps' OWN icons, colours and cards, for
// the directory's community map page (directory/public/community-kit.js is this, bundled by scripts/build-community-kit.mjs).
// ★★ ONE RULE, THREE READERS: the NAVTEX / shipping map's hazard icons, station masts, forecast pills and cards
//    (navtexIcons, navtexMap, navtexWarn), the AIS hull, the APRS symbols, the sonde balloon, the MeshCore node marker and
//    the DSC pin are the very functions the web client and the app draw with — the community map never redraws them.
// ★ PURE: no DOM, no React Native. Nothing here fetches.
export { navWarnSanitise, navWarnItems, navWarnVisible, navWarnCard, navMetSanitise, navMetItems, navMetVisible, navMetCard, navMetIcon } from '../../src/services/navtexWarn';
export { navSanitise, navSites, navIcon, navCard, navName, navAgo, NAV_BAND_COLOURS } from '../../src/services/navtexMap';
export { NAV_HAZARD_COLOURS, navHazardMarker } from '../../src/services/navtexIcons';
export { navtexSubject } from '../../src/utils/navtex';
export { aisIcon, aisColour } from '../../src/services/tracks';
export { aprsIcon, aprsColour } from '../../src/utils/aprsSymbols';
export { sondeIcon } from '../../src/services/sondes';
export { ismMapIcon } from '../../src/services/ism';
export { dscIcon } from '../../src/services/shippingMap';
// ★★ V12 beta94 THE KEY (Stuart 11-25: "missing the legend for what the icons mean"): the server maps' OWN legend rows and
//    glyphs — the shipping / NAVTEX map's KEY (shipKeyHtml: shipLegendRows, the hazards, the not-working marks, the wind
//    arrows, the sea areas) — so the community map's KEY says exactly what the servers' does, with the same drawings.
export { shipLegendRows, windLegendRows } from '../../src/services/shippingMap';
export { seaLegendRows } from '../../src/services/seaAreas';
export { NAV_HAZARDS, NAV_MARK_ROWS, navGlyphSvg, navMarkRowSvg } from '../../src/services/navtexIcons';
export { AIS_COLOURS } from '../../src/services/tracks';
// ★★ V12 beta96 THE AIRCRAFT GLYPHS (Stuart 11-26: "the correct icon for the aircraft type like the ADS-B map"): the ADS-B
//    layer's own class silhouettes and its ONE rule (iconClassOf — the emitter category, else the aircraft data, else the
//    type's family, else the jet), lifted from web/mapkit/adsblayer.js's ACICONS block by build-community-kit.mjs.
export { iconClassOf as aircraftIconClass, acIconSvg as aircraftIconSvg, AC_CLASS_NAME as AIRCRAFT_CLASS_NAMES, ICONS as AIRCRAFT_ICONS } from 'vibe:acicons';
