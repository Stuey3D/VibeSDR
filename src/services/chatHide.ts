/**
 * ★★★ "HIDE THIS USER" — a listener's own block list for third-party chat (2026-10-05).
 *
 * WHY: the app draws chat typed by strangers on servers we do not run — FM-DX, OpenWebRX, UberSDR —
 * so a listener needs a way to stop seeing one of them (Google Play's user-generated-content
 * questions ask exactly this). VibeServer's own chat is canned (fixed phrases, ordinal people), so
 * there is nothing typed to hide there and the option is not offered in canned mode.
 *
 * ★★ SESSION ONLY, BY DESIGN (Stuart: "on most servers users just connect with a generic User2381
 *    User3 User848 not real names and it changes upon reconnection, so maybe make it just hide users
 *    for that session only"). A saved list would hide whoever inherits the name next time and grow
 *    forever; the hidden set lives in memory for one server connection and nothing else.
 *
 * ★★ PURELY CLIENT-SIDE. Nothing is sent to the server or anyone — hiding is not reporting.
 *
 * ★ The key is the sender's name EXACTLY as the server sent it. No case folding or trimming: two
 *   names that differ only in case are two people as far as the server is concerned, and hiding
 *   "user5" must not silently take "User5" with it.
 *
 * Pure logic only — tested by scripts/test_chat_hide.ts. The React side is `useHiddenChatUsers` in
 * src/components/ChatDrawer.tsx.
 */

/** The fields this file reads — structurally a ChatMessage (kept here so node can test it alone). */
export interface HideableMessage { type: 'own' | 'other' | 'system'; user?: string }

/** May this line offer "Hide <name>"? Only somebody else's line with a name on it — never your own
 *  (by type OR by name: a server echo of your handle is still you), never a system line. */
export function canHide(m: HideableMessage, myName: string | null | undefined): boolean {
  if (m.type !== 'other' || !m.user) return false;
  return m.user !== myName;
}

/** Is this line from a hidden sender? Own and system lines never are. */
export function isHidden(m: HideableMessage, hidden: ReadonlySet<string>): boolean {
  return m.type === 'other' && !!m.user && hidden.has(m.user);
}

/** The lines to draw. ★ Returns the SAME array when nothing is hidden, so a FlatList fed by it does
 *  not see a new `data` on every render of an ordinary chat. */
export function visibleMessages<M extends HideableMessage>(messages: M[], hidden: ReadonlySet<string>): M[] {
  if (hidden.size === 0) return messages;
  return messages.filter((m) => !isHidden(m, hidden));
}

/** The set with `name` added. Unchanged (same object) for an empty name or your own. */
export function withHidden(hidden: ReadonlySet<string>, name: string | undefined,
                           myName: string | null | undefined): ReadonlySet<string> {
  if (!name || name === myName || hidden.has(name)) return hidden;
  const next = new Set(hidden);
  next.add(name);
  return next;
}

/** The undo line under the thread — "1 hidden · Show". Empty when nobody is hidden (no line at all:
 *  a "0 hidden" control would be one whose every use is a no-op). */
export function hiddenSummary(n: number): string {
  return n > 0 ? `${n} hidden` : '';
}

/** The action's label, with the name in it, so a mis-aimed long-press says WHO before it acts. */
export function hideLabel(name: string): string {
  return `Hide ${name}`;
}
