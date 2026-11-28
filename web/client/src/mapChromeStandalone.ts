/**
 * mapChromeStandalone.ts — ★★★ b97 THE MAP CARD SYSTEM FOR A PAGE OF ITS OWN: the amateur / spots map's window (about:blank,
 * written by the opener) and the directory's community map page run mapChromeApi.ts from ONE plain script,
 * window.VibeMapChrome. Built by scripts/gen-mapchrome-source.mjs (never by hand):
 *   · web/client/src/generated/mapChromeSource.ts — MAP_CHROME_JS, a string the spots window inlines (lazy-loaded with the map);
 *   · directory/public/map-chrome.js — the same text, a static file the community map page loads (main branch, like
 *     community-kit.js).
 * ★ ONE SOURCE: the card system is mapChrome.ts / mapChromeApi.ts, exactly what the radio page's maps run. The page's own CSS
 *   the chrome leans on (index.html's .edgeDock — the health pill's edge dock — and the controller highlight) comes with it
 *   (vibe:pagecss, lifted from web/client/index.html by the generator), so a page with none of the radio page's CSS still
 *   draws the same chips.
 * ★ What a standalone page cannot have is stubbed by the generator (the radio page's gamepad, chunk cache, Leaflet asset,
 *   GPU map probe, the track / ADS-B layer sources): these maps bring their own map (mountMapChrome's hostMap).
 */
import { mountMapChrome, mapRowHtml } from './mapChromeApi';
import { esc, ago } from './mapChrome';
import { MAP_AUTOFIT_JS } from '../../../src/utils/mapAutoFit';
// @ts-ignore — a virtual module the generator provides (index.html's CSS the chrome needs)
import PAGE_CSS from 'vibe:pagecss';

function boot() {
  if (typeof document === 'undefined') return;
  if (!document.getElementById('vibeMapChromePageCss')) {
    const st = document.createElement('style');
    st.id = 'vibeMapChromePageCss';
    st.textContent = String(PAGE_CSS || '');
    document.head.appendChild(st);
  }
  const w = window as any;
  if (!w.VibeAutoFit) { const el = document.createElement('script'); el.textContent = MAP_AUTOFIT_JS; document.head.appendChild(el); }
}
boot();

export { mountMapChrome, mapRowHtml, esc, ago };
