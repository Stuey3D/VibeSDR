/**
 * ★★★ RECEIVE BETA UPDATES — what the admin page says and what its update button sends (web/client/src/updateOffer.ts),
 * for every offer the root helper can write (vibeserver-maintenance write_offer; its choice is tested in
 * vibeserver/linux/test_vibeserver_maintenance.py).
 *
 * ★★ The rule that matters most: a button labelled UPDATE never sends the downgrade, and BACK TO STABLE always does.
 *
 * Run: node --no-warnings scripts/test_update_offer.ts
 */
import { updateOfferText, updateButton, betaConfirmText } from '../web/client/src/updateOffer.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, got?: unknown) {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${got === undefined ? '' : `\n   got ${JSON.stringify(got)}`}`);
}

// ── The line under the buttons ────────────────────────────────────────────────────────────────
{
  const t = updateOfferText({ beta: true, installed: '11.0.10-4', offer: 'beta', version: '12.0.0~beta1-1' });
  ok('beta on, a beta offered: "12.0.0~beta1-1 available (beta)"', t.startsWith('12.0.0~beta1-1 available (beta).'), t);
  ok('  and names what runs now', t.includes('This server runs 11.0.10-4'), t);
}
{
  const t = updateOfferText({ beta: false, installed: '12.0.0~beta1-1', offer: 'back-to-stable', version: '11.0.10-4' });
  ok('beta off on a beta: "11.0.10-4 available — back to stable"', t.startsWith('11.0.10-4 available — back to stable.'), t);
  ok('  and says the settings are kept', t.includes('settings are kept'), t);
}
{
  const t = updateOfferText({ beta: false, installed: '11.0.10-3', offer: 'stable', version: '11.0.10-4' });
  ok('a stable update: "11.0.10-4 available"', t.startsWith('11.0.10-4 available.') && !t.includes('beta'), t);
}
ok('up to date, beta off', updateOfferText({ beta: false, installed: '11.0.10-4', offer: 'none' }) === 'Up to date (11.0.10-4).');
ok('up to date, beta on — says so', updateOfferText({ beta: true, installed: '12.0.0~beta1-1', offer: 'none' })
   === 'Up to date (12.0.0~beta1-1, beta updates on).');
ok('never checked', updateOfferText({ beta: false, offer: '' }).startsWith('Not checked yet'));
ok('checked under the other tick (stale): asks for a check, never shows the old offer',
   updateOfferText({ beta: true, offer: '', stale: true }).startsWith('Beta updates were switched'));
ok('the repository listed nothing: never "up to date"', /online/.test(updateOfferText({ offer: 'unknown' })));
ok('no updates object (an older or non-Linux server): nothing at all', updateOfferText(null) === '' && updateOfferText(undefined) === '');

// ── The button ────────────────────────────────────────────────────────────────────────────────
{
  const b = updateButton({ beta: false, offer: 'back-to-stable', version: '11.0.10-4', installed: '12.0.0~beta1-1' });
  ok('back to stable: its own label, with the version', b.label === 'BACK TO STABLE (11.0.10-4)', b.label);
  ok('  sends update-stable (the downgrade), and asks first', b.action === 'update-stable' && /OLDER/.test(b.confirm), b);
}
for (const offer of ['beta', 'stable', 'none', 'unknown', '']) {
  const b = updateButton({ beta: offer === 'beta', offer, version: '12.0.0~beta1-1' });
  ok(`offer "${offer}": UPDATE VIBESERVER sends update, never the downgrade`, b.label === 'UPDATE VIBESERVER' && b.action === 'update', b);
}
ok('a server that does not offer update-stable (the page passes null): plain update',
   updateButton(null).action === 'update');
ok('back-to-stable with no version is never sent', updateButton({ offer: 'back-to-stable' }).action === 'update');

// ── The question before the tick changes anything ─────────────────────────────────────────────
ok('ticking says nothing installs until UPDATE is pressed', /Nothing is installed until/.test(betaConfirmText(true)));
ok('unticking says how to get back', /BACK TO STABLE/.test(betaConfirmText(false)));

console.log(fails ? `\n${fails} FAILED, ${passes} passed` : `update offer: all ${passes} passed`);
process.exit(fails ? 1 : 0);
