// ★ The controller keyboard's keycap fonts carry every glyph its layouts draw (brief "Keycap fonts" TRAP: accents and
//   symbols must be in each font used, or fall back per character). QWERTY labels use Nixie One under the Nixie
//   display and Atkinson Hyperlegible otherwise; whatever neither carries must be in KB_SYSTEM_GLYPHS (system font).
import { readFileSync } from 'node:fs';
import { kbKeys, KB_SYSTEM_GLYPHS, type KbMode, type KbLayer } from '../src/utils/kbLayouts';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

/** The code points a TrueType font maps (cmap formats 4 and 12). */
function cmapOf(rel: string): Set<number> {
  const b = readFileSync(new URL('../' + rel, import.meta.url));
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const n = dv.getUint16(4); let cmapOff = -1;
  for (let i = 0; i < n; i++) { const r = 12 + i * 16; if (b.toString('latin1', r, r + 4) === 'cmap') cmapOff = dv.getUint32(r + 8); }
  const set = new Set<number>();
  if (cmapOff < 0) return set;
  const nt = dv.getUint16(cmapOff + 2);
  for (let i = 0; i < nt; i++) {
    const off = cmapOff + dv.getUint32(cmapOff + 4 + i * 8 + 4);
    const fmt = dv.getUint16(off);
    if (fmt === 4) {
      const segX2 = dv.getUint16(off + 6), ends = off + 14, starts = ends + segX2 + 2;
      for (let sgm = 0; sgm < segX2 / 2; sgm++) {
        const e = dv.getUint16(ends + sgm * 2), st = dv.getUint16(starts + sgm * 2);
        for (let c = st; c <= e && c !== 0xffff; c++) set.add(c);
      }
    } else if (fmt === 12) {
      const ng = dv.getUint32(off + 12);
      for (let g = 0; g < ng; g++) { const a = dv.getUint32(off + 16 + g * 12), z = dv.getUint32(off + 20 + g * 12); for (let c = a; c <= z; c++) set.add(c); }
    }
  }
  return set;
}

console.log('test_kb_fonts');
const glyphs = new Set<string>();
for (const m of ['search', 'text', 'password', 'url'] as KbMode[])
  for (const l of ['lower', 'upper', 'caps', 'symbols', 'accents', 'accentsUp'] as KbLayer[])
    for (const k of kbKeys(m, l)) for (const c of k.label) glyphs.add(c);
glyphs.add('•');   // the password dots
for (const f of ['assets/fonts/NixieOne-Regular.ttf', 'assets/fonts/AtkinsonHyperlegible-Regular.ttf']) {
  const cm = cmapOf(f);
  ok(cm.size > 100, `${f}: cmap read (${cm.size} code points)`);
  const missing = [...glyphs].filter((c) => !cm.has(c.codePointAt(0)!));
  const uncovered = missing.filter((c) => !KB_SYSTEM_GLYPHS.includes(c));
  ok(uncovered.length === 0, `${f}: every keycap glyph is in the font or drawn in the system font (missing: ${missing.join('') || 'none'})`);
  ok([...'éÉçñßøÆ'].every((c) => cm.has(c.codePointAt(0)!)), `${f}: the accents are real glyphs, not fallbacks`);
}
process.exit(fails ? 1 : 0);
