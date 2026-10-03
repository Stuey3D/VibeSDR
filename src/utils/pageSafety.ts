/**
 * Helpers for the map WebView page (MapOverlay.tsx), kept free of React Native (and of imports) so node can
 * test them directly: node --no-warnings scripts/test_safe_url.ts
 */

/** ★★★ A VALUE WRITTEN INTO THE PAGE'S <script> AS A STRING LITERAL. The base URL and session id came from a
 *  server or a link; written raw between quotes, a `'` ended the string and the rest ran as code in a page
 *  that can read files. JSON.stringify makes a valid literal; '<' is escaped so `</script>` cannot close the
 *  block, and U+2028/2029 because they are legal in JSON but end a line in older JavaScript. */
export function jsStringLiteral(v: unknown): string {
  return JSON.stringify(typeof v === 'string' ? v : '')
    .replace(/</g, '\\u003c')
    .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
    .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
}

/** The instance paths the map page may ask React Native to fetch (hostFetch): its receiver description and
 *  the HFDL addon — exactly what the page requests today (/api/description, /addon/hfdl/aircraft,
 *  /addon/hfdl/groundstations). Nothing that could change the host or climb out of the path once appended
 *  to the base URL: no '@', '//', '\', '..' or whitespace. */
export function isAllowedHostFetchPath(path: unknown): boolean {
  if (typeof path !== 'string' || path.length > 200) return false;
  if (path !== '/api/description' && !path.startsWith('/addon/hfdl/')) return false;
  return !/[@\\\s]|\/\/|\.\./.test(path);
}
