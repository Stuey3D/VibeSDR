/**
 * autoModes.ts — ★★★ AN AUTO DECODER'S FALLBACK CONTROLS, AS RTTY HAS THEM (Stuart, 2026-10-31: "dont forget with any
 * decoders that are auto mode we can pop open extra controls underneath like we do for RTTY if auto fails to decode").
 *
 * RTTY's pattern: under the key, a first row with AUTO and the presets, then the manual rows; a hand-picked value turns
 * AUTO off, and under AUTO nothing manual is lit. Here the same for a decoder whose manual choices are the SERVER'S DATA
 * (PAGE's `pagingModes`, the text modes' `mfskModes`): every entry carries a `group` —
 *
 *   first row  (PROTOCOL / FAMILY): AUTO, then each group ("POCSAG", "FLEX" · "MFSK", "OLIVIA", "CONTESTIA", "THOR");
 *   second row (RATE / MODE):       the chosen group's own entries — "ANY" (that group's AUTO) first, then each one —
 *                                   drawn only when the group has more than one (FLEX names its own speed in every
 *                                   frame, so it has no RATE row: a row whose every choice is a no-op is not offered).
 *
 * Shared by the app (ModeSelector) and the web page (main.ts). Nothing here lists a mode: the server's list is the truth.
 */
export interface GroupedMode { id: string; label: string; group: string }

/** A server mode list: `[{id, label, group?}]`, AUTO first. Short ids and labels only; anything else is dropped. */
export function parseGroupedModes(v: unknown, max = 48): GroupedMode[] {
  if (!Array.isArray(v)) return [];
  const out: GroupedMode[] = [];
  for (const m of v) {
    const id = typeof m?.id === 'string' ? m.id : '';
    const label = typeof m?.label === 'string' ? m.label : '';
    const group = typeof m?.group === 'string' ? m.group : '';
    if (/^[a-z0-9-]{1,24}$/.test(id) && /^[A-Za-z0-9 /.-]{1,24}$/.test(label) && /^[A-Za-z0-9 ]{0,16}$/.test(group))
      out.push({ id, label, group });
    if (out.length >= max) break;
  }
  return out;
}
/** The first row's groups, in the server's order (AUTO is not one of them). */
export function modeGroups(list: readonly GroupedMode[]): string[] {
  const g: string[] = [];
  for (const m of list) if (m.group && !g.includes(m.group)) g.push(m.group);
  return g;
}
/** The group `id` belongs to; '' for AUTO (or an id the list does not have). */
export function groupOfMode(list: readonly GroupedMode[], id: string): string {
  return list.find(m => m.id === id)?.group ?? '';
}
/** A group's own entries (its "ANY" first, as the server sends them). */
export function modesInGroup(list: readonly GroupedMode[], group: string): GroupedMode[] {
  return group ? list.filter(m => m.group === group) : [];
}
/** What pressing a group's key chooses: its first entry (the group's own AUTO, "ANY"). */
export function firstOfGroup(list: readonly GroupedMode[], group: string): string {
  return modesInGroup(list, group)[0]?.id ?? 'auto';
}
/** The second row is drawn only when the chosen group has a real choice to make. */
export function showSecondRow(list: readonly GroupedMode[], id: string): boolean {
  return modesInGroup(list, groupOfMode(list, id)).length > 1;
}
/** A mode's full name for the status line ("POCSAG 1200", "OLIVIA 8/250", "FLEX"); AUTO → "AUTO". */
export function fullModeName(list: readonly GroupedMode[], id: string): string {
  const m = list.find(x => x.id === id);
  if (!m || !m.group) return 'AUTO';
  return m.label === 'ANY' ? m.group : `${m.group} ${m.label}`;
}
