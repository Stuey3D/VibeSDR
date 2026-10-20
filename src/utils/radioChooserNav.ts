/**
 * ★★★ THE RADIO CHOOSER BY PAD AND KEYBOARD (controller audit §1, 2026-10-20) — the pure part, tested by
 * scripts/test_radio_chooser_nav.ts.
 *
 * Stuart: "selecting a vibeserver from the directory and it shows you the radios — you cannot use the controls to
 * select the radio as no selection box appears on screen". The chooser was in the screen's yield list (so the screen
 * stopped listening) and had no key handling of its own (so nobody started). SDRScreen now runs a `useListNav` over
 * the items this builds, in the order they are drawn:
 *
 *   ‹ SERVERS · each radio · [PIN field, UNLOCK] · [admin field, UNLOCK]
 *
 * ★ Blocked radios are FOCUSABLE — "listed and greyed, never hidden". Enter on one moves the highlight to the box that
 *   would open it (a PIN-locked radio → the PIN field; an occupied one → the admin field), so a member holding the
 *   right code can get from the row to the box without touching the screen. A radio nothing here can open (in use by
 *   another app, unsupported) does nothing on Enter.
 * ★ The highlight starts on the first radio that can be chosen; failing that, the first radio.
 */
export interface ChooserRadio {
  id: string;
  busy: boolean;
  unsupported: boolean;
  gated: boolean;
  inUseApp: boolean;
  blocked: boolean;
}

export type ChooserItem =
  | { k: 'back' }
  | { k: 'radio'; id: string }
  | { k: 'pin' } | { k: 'pinGo' }
  | { k: 'admin' } | { k: 'adminGo' };

export type ChooserAct =
  | { do: 'back' }
  | { do: 'choose'; id: string }
  | { do: 'focus'; index: number }
  | { do: 'field'; which: 'pin' | 'admin' }
  | { do: 'unlock'; which: 'pin' | 'admin' }
  | { do: 'none' };

/** The focus order, top to bottom as drawn. `showPin` = the PIN row is on screen. */
export function chooserItems(radios: readonly ChooserRadio[], showPin: boolean): ChooserItem[] {
  const out: ChooserItem[] = [{ k: 'back' }];
  for (const r of radios) out.push({ k: 'radio', id: r.id });
  if (showPin) out.push({ k: 'pin' }, { k: 'pinGo' });
  out.push({ k: 'admin' }, { k: 'adminGo' });
  return out;
}

/** Rows for useListNav's grid: a field and its UNLOCK share a row (left / right between them). */
export function chooserRowOf(items: readonly ChooserItem[], i: number): number {
  let row = -1;
  for (let j = 0; j <= i && j < items.length; j++) {
    const k = items[j].k;
    if (k === 'pinGo' || k === 'adminGo') continue;   // same row as the field before it
    row++;
  }
  return Math.max(0, row);
}

/** Where the highlight starts: the first radio that can be chosen, else the first radio, else ‹ SERVERS. */
export function chooserStart(items: readonly ChooserItem[], radios: readonly ChooserRadio[]): number {
  const byId = new Map(radios.map((r) => [r.id, r]));
  let firstRadio = -1;
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    if (it.k !== 'radio') continue;
    if (firstRadio < 0) firstRadio = i;
    if (!byId.get(it.id)?.blocked) return i;
  }
  return firstRadio >= 0 ? firstRadio : 0;
}

/** What Enter on item `i` does. */
export function chooserActivate(items: readonly ChooserItem[], radios: readonly ChooserRadio[], i: number): ChooserAct {
  const it = items[i];
  if (!it) return { do: 'none' };
  switch (it.k) {
    case 'back': return { do: 'back' };
    case 'pin': return { do: 'field', which: 'pin' };
    case 'admin': return { do: 'field', which: 'admin' };
    case 'pinGo': return { do: 'unlock', which: 'pin' };
    case 'adminGo': return { do: 'unlock', which: 'admin' };
    case 'radio': {
      const r = radios.find((x) => x.id === it.id);
      if (!r) return { do: 'none' };
      if (!r.blocked) return { do: 'choose', id: r.id };
      // Nothing here can open these: no box to send the highlight to.
      if (r.inUseApp || r.unsupported) return { do: 'none' };
      const want = r.gated ? 'pin' : 'admin';
      const at = items.findIndex((x) => x.k === want);
      return at >= 0 ? { do: 'focus', index: at } : { do: 'none' };
    }
  }
}
