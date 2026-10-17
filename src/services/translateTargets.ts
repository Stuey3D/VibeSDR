/**
 * ★★ TRANSLATE TO: ONE ROW PER LANGUAGE (Stuart, 2026-10-17 night — the list showed Arabic (Saudi Arabia) + Arabic (UAE),
 * Chinese + Chinese (Hong Kong) + Chinese (Taiwan)). Apple's Translation names a target per variant; the person choosing
 * wants a language. Pure, so it is tested (scripts/test_translateTargets.ts) — no React Native imports.
 *
 * Distinct written forms stay only where Apple's Translation genuinely writes differently AND a reader needs the choice:
 *   • Chinese: Simplified vs Traditional (Taiwan and Hong Kong both read Traditional — one row);
 *   • Portuguese: Brazil vs Portugal;
 *   • English: UK vs US (spelling).
 * Every other language is one row. Within a row the id sent to the translator is the phone's own variant when it is in
 * that row (deviceTranslateTo: pt-BR in Brazil, zh-HK in Hong Kong), else the bare code, else the first offered — and
 * the phone's row comes first. `ids` lists every variant folded in, so a target stored by an older build ('ar-AE')
 * still finds its row and its ✓.
 */
export interface TranslateTarget { id: string; name: string; installed?: boolean }
export interface TranslateRow extends TranslateTarget { ids: string[] }

const lc = (id: string) => id.toLowerCase().replace(/_/g, '-');
const baseOf = (id: string) => lc(id).split('-')[0];
/** The kept forms: base → [suffix, the ids it covers in preference order]. */
const FORMS: Record<string, [string, string[]][]> = {
  zh: [['Simplified', ['zh', 'zh-hans', 'zh-cn', 'zh-sg', 'zh-hans-cn']], ['Traditional', ['zh-hant', 'zh-tw', 'zh-hk', 'zh-mo', 'zh-hant-tw', 'zh-hant-hk']]],
  pt: [['Brazil', ['pt', 'pt-br']], ['Portugal', ['pt-pt']]],
  en: [['US', ['en', 'en-us']], ['UK', ['en-gb']]],
};
const plain = (name: string) => name.replace(/\s*\(.*\)\s*$/, '');

export function translateRows(list: TranslateTarget[], deviceTarget: string): TranslateRow[] {
  const dev = lc(deviceTarget || '');
  const groups = new Map<string, TranslateTarget[]>();
  for (const t of list) {
    if (!t.id) continue;
    const b = baseOf(t.id);
    if (!groups.has(b)) groups.set(b, []);
    groups.get(b)!.push(t);
  }
  const rows: TranslateRow[] = [];
  const pickRep = (members: TranslateTarget[], prefer: string[]) =>
    members.find(m => lc(m.id) === dev)
    ?? prefer.map(p => members.find(m => lc(m.id) === p)).find(Boolean)
    ?? members.find(m => !lc(m.id).includes('-'))
    ?? [...members].sort((a, b) => a.id.localeCompare(b.id))[0];
  for (const [b, members] of groups) {
    // ★ A form only needs naming when the list really offers two (Android's ML Kit has one English, one Chinese).
    const forms = FORMS[b]?.filter(([, covers]) => members.some(m => covers.includes(lc(m.id))));
    const name = plain(members.find(m => !lc(m.id).includes('-'))?.name ?? members[0].name);
    if (!forms || forms.length < 2) {
      const rep = pickRep(members, [b]);
      rows.push({ id: rep.id, name, installed: rep.installed, ids: members.map(m => m.id) });
      continue;
    }
    const used = new Set<string>();
    for (const [suffix, covers] of forms) {
      const inForm = members.filter(m => covers.includes(lc(m.id)));
      if (!inForm.length) continue;
      inForm.forEach(m => used.add(m.id));
      const rep = pickRep(inForm, covers);
      rows.push({ id: rep.id, name: `${name} (${suffix})`, installed: rep.installed, ids: inForm.map(m => m.id) });
    }
    // A variant outside the kept forms (en-AU, say) gets a row only if it is the phone's own.
    for (const m of members) {
      if (!used.has(m.id) && lc(m.id) === dev) rows.push({ id: m.id, name: m.name, installed: m.installed, ids: [m.id] });
    }
  }
  const mine = rows.find(r => r.ids.some(i => lc(i) === dev))
    ?? rows.find(r => !!dev && baseOf(r.id) === baseOf(dev) && !lc(r.id).includes('-'));
  const rest = rows.filter(r => r !== mine).sort((a, b) => a.name.localeCompare(b.name));
  return mine ? [mine, ...rest] : rest;
}

/** The row a stored target belongs to ('ar-AE' → the Arabic row), or undefined. */
export function rowFor(rows: TranslateRow[], id: string): TranslateRow | undefined {
  return id ? rows.find(r => r.ids.some(i => lc(i) === lc(id))) : undefined;
}
