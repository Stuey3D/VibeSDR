// dabBlocks.ts — Band III, and the two lookup tables that go with a multiplex.
//
// ★★★ MIRRORS vibe_dab_channels.h AND the web client's DAB_BLOCKS. Three readers of one table;
//     see AGENTS.md, "ONE RULE, TWO READERS". The one that gets missed is the OFFSET blocks —
//     10N, 11N and 12N sit 160 kHz above 10A/11A/12A, are easy to leave out of a hand-typed list,
//     and are genuinely on air.
//
// ★ The client needs its own copy because the block is what the USER picks: the server is told an
//   index, and the app has to draw the name and the frequency beside it before anything is tuned.

export interface DabBlock { name: string; hz: number }

export const DAB_BLOCKS: DabBlock[] = ([
  ['5A', 174928], ['5B', 176640], ['5C', 178352], ['5D', 180064],
  ['6A', 181936], ['6B', 183648], ['6C', 185360], ['6D', 187072],
  ['7A', 188928], ['7B', 190640], ['7C', 192352], ['7D', 194064],
  ['8A', 195936], ['8B', 197648], ['8C', 199360], ['8D', 201072],
  ['9A', 202928], ['9B', 204640], ['9C', 206352], ['9D', 208064],
  ['10A', 209936], ['10N', 210096], ['10B', 211648], ['10C', 213360], ['10D', 215072],
  ['11A', 216928], ['11N', 217088], ['11B', 218640], ['11C', 220352], ['11D', 222064],
  ['12A', 223936], ['12N', 224096], ['12B', 225648], ['12C', 227360], ['12D', 229072],
  ['13A', 230784], ['13B', 232496], ['13C', 234208], ['13D', 235776], ['13E', 237488],
  ['13F', 239200],
] as [string, number][]).map(([name, k]) => ({ name, hz: k * 1000 }));

/** Index of a block by name, or -1. */
export function dabBlockIndex(name: string): number {
  return DAB_BLOCKS.findIndex(b => b.name === name);
}

/** The block nearest a frequency (within 100 kHz), or -1 — used to name `altHz` from FIG 0/21. */
export function dabBlockAt(hz: number): number {
  return DAB_BLOCKS.findIndex(b => Math.abs(b.hz - hz) < 100_000);
}

/** TS 101 756 table 12 — the international programme types carried in FIG 0/17. */
export const DAB_PTY: string[] = [
  'None', 'News', 'Current Affairs', 'Information', 'Sport', 'Education', 'Drama', 'Culture',
  'Science', 'Varied', 'Pop Music', 'Rock Music', 'Easy Listening', 'Light Classical',
  'Serious Classical', 'Other Music', 'Weather', 'Finance', "Children's", 'Social Affairs',
  'Religion', 'Phone In', 'Travel', 'Leisure', 'Jazz', 'Country', 'National Music', 'Oldies',
  'Folk', 'Documentary',
];
