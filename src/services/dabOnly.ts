// dabOnly.ts — V12 DAB ONLY (Stuart, 2026-11-17: "if DAB is the only allowed mode then the exit button goes, also the exit
// button needs to be removed on existing servers that have been locked to DAB"). ONE RULE, TWO READERS: the web page's
// web/client/src/dabOnly.ts is the same function; scripts/test-dab-only-clients.ts holds both to the same answers.
/** Is this receiver DAB only for THIS listener? The server's word when it gave one (hwinfo `dabOnly`, false for the signed-in
 *  admin); an OLDER server never sends it, so its blocked modes decide — every demodulator an older server has (WFM, NFM, AM,
 *  USB, LSB) blocked and DAB not. The signed-in admin keeps EXIT DAB either way. */
export function dabOnlyFrom(known: boolean, said: boolean, blocked: Set<string>, admin: boolean): boolean {
  if (known) return said;
  if (admin) return false;
  return ['wfm', 'nfm', 'am', 'usb', 'lsb'].every((m) => blocked.has(m)) && !blocked.has('dab');
}
