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
