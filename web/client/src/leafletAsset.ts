/**
 * leafletAsset.ts — where THIS server keeps its copy of Leaflet.
 *
 * ★★★ LEAFLET IS SERVED BY THE RECEIVER ITSELF, NOT BY unpkg.com (security audit, 2026-10-03).
 *     It used to be a <script src="https://unpkg.com/leaflet@1.9.4/..."> with no integrity, and it
 *     ran ON THE RECEIVER'S ORIGIN — the origin that holds the admin ticket. Whoever could change
 *     what unpkg served (a compromised package, the CDN, anyone on the path) could run script as the
 *     owner. scripts/build-web.mjs now compiles the exact 1.9.4 files (the directory's vendored copy,
 *     checked against Leaflet's published SHA-256 at build time) into the server as /vs/ assets with
 *     hashed names, and hands their URLs and SRI hashes to the bundle through `define`.
 *  ★ It also means the map works on a LAN with no route to the internet, which the unpkg copy never
 *    did (admin.ts said as much: "Do not claim this page works offline until Leaflet is bundled").
 *  ★ `typeof` guard: a test harness that bundles a source file without build-web's define gets null
 *    (no Leaflet map) rather than a ReferenceError.
 */
declare const __VS_LEAFLET__: { js: string; css: string; jsSri: string; cssSri: string };

export interface LeafletAsset { js: string; css: string; jsSri: string; cssSri: string }

export const LEAFLET_ASSET: LeafletAsset | null =
  typeof __VS_LEAFLET__ !== 'undefined' ? __VS_LEAFLET__ : null;

/** Load Leaflet into `doc` (this page by default) from `origin` + its /vs/ path. Resolves true once
 *  window.L exists, false on any failure or after `timeoutMs` — a map that cannot load must cost
 *  nothing but a map. */
export function loadLeafletInto(doc: Document = document, origin = '', timeoutMs = 6000): Promise<boolean> {
  const win = doc.defaultView as (Window & { L?: unknown }) | null;
  if (win?.L) return Promise.resolve(true);
  const a = LEAFLET_ASSET;
  if (!a) { console.error('Leaflet: this build carries no Leaflet asset'); return Promise.resolve(false); }
  return new Promise<boolean>((resolve) => {
    const css = doc.createElement('link');
    css.rel = 'stylesheet';
    css.href = origin + a.css;
    css.integrity = a.cssSri;
    doc.head.appendChild(css);
    const js = doc.createElement('script');
    js.src = origin + a.js;
    js.integrity = a.jsSri;
    js.onload = () => resolve(true);
    js.onerror = () => { console.error('Leaflet: ' + js.src + ' did not load'); resolve(false); };
    setTimeout(() => resolve(!!win?.L), timeoutMs);
    doc.head.appendChild(js);
  });
}
