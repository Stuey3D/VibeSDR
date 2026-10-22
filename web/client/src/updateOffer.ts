/** ★★★ RECEIVE BETA UPDATES (Stuart, 2026-10-22: "user ticks it and then V12 gets offered, user unticks it V11 gets
 *  offered instead"). What the admin page SAYS about the server's next VibeServer, and what UPDATE VIBESERVER does —
 *  from `updates` in the admin status, which the Linux daemon reads from the system on every call (the beta apt source
 *  exists or not; the offer the root helper last worked out from apt). Pure, so scripts/test_update_offer.ts can walk
 *  every state.
 *  ★★ The one DOWNGRADE is its own action, `update-stable`, and its own label: a button that says "update" must never
 *     install an older version. The schedule only ever sends `update`, which cannot go backwards. */
export interface UpdateState {
  beta?: boolean;
  installed?: string;
  /** beta | stable | back-to-stable | none | unknown | '' (never checked, or checked under the other tick) */
  offer?: string;
  version?: string;
  stale?: boolean;
  checked?: number;
}

/** The one line under the buttons. */
export function updateOfferText(u: UpdateState | null | undefined): string {
  if (!u) return '';
  const on = u.installed ? ` This server runs ${u.installed}.` : '';
  switch (u.offer) {
    case 'beta':           return `${u.version} available (beta).${on}`;
    case 'stable':         return `${u.version} available.${on}`;
    case 'back-to-stable': return `${u.version} available — back to stable.${on} Your settings are kept.`;
    case 'none':           return `Up to date${u.installed ? ` (${u.installed}${u.beta ? ', beta updates on' : ''})` : ''}.`;
    case 'unknown':        return 'The update server did not list VibeServer — is this machine online? Try CHECK FOR UPDATES.';
    default:               return u.stale
      ? 'Beta updates were switched since the last check — press CHECK FOR UPDATES.'
      : 'Not checked yet — press CHECK FOR UPDATES.';
  }
}

/** What the update button is called and which maintenance action it sends. */
export function updateButton(u: UpdateState | null | undefined): { label: string; action: 'update' | 'update-stable'; confirm: string } {
  if (u && u.offer === 'back-to-stable' && u.version) {
    return {
      label: `BACK TO STABLE (${u.version})`,
      action: 'update-stable',
      confirm: `Go back to the stable VibeServer, ${u.version}?\n\nThis installs an OLDER version than the beta running now. `
             + 'Your settings are kept, and ticking Receive beta updates again brings the beta back.',
    };
  }
  return { label: 'UPDATE VIBESERVER', action: 'update', confirm: 'Update VibeServer to the latest version?' };
}

/** The question asked before the tick-box changes anything. */
export function betaConfirmText(turnOn: boolean): string {
  return turnOn
    ? 'Receive beta updates?\n\nThe next VibeServer (V12) is offered as soon as each beta is published — new features '
      + 'early, and occasionally a rough edge. Nothing is installed until you press UPDATE VIBESERVER (or your '
      + 'scheduled update runs).\n\nUntick it at any time to go back to the stable version; your settings are kept.'
    : 'Stop receiving beta updates?\n\nIf this server is running a beta, BACK TO STABLE then offers the newest stable '
      + 'version. Nothing changes until you press it.';
}
