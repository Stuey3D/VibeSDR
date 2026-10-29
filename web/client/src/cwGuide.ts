/* cwGuide.ts — ★★ V12 (2026-10-29) THE CW TERMS GUIDE on the web page, opened by CW TERMS GUIDE under the Signal ID Wiki
 * link in the mode menu (mobile.ts). The app's CwTermsGuide is the same guide; both read src/utils/cwVocab (the PLAIN key
 * and the server's language model read it too) — ONE source of the words.
 *
 *   a search box (searchCwTerms: code, other spellings, plain words, meaning, example) · each term with its code, plain
 *   words, meaning and an example · THE LOOK-ALIKES (DE is not a German callsign, 0 and O, prosigns printed as
 *   punctuation…) · RST in one line · how a callsign opens QRZ.com.
 * ★ Pad and keyboard (gamepad.ts): the guide is a [data-navscope] — the highlight moves between the search box, every
 *   entry (each a tab stop, scrolled into view) and CLOSE; back closes it (pushBack), Escape in the box leaves the box
 *   first, as everywhere. A click outside the card closes it too.
 */
import { CW_GUIDE_INTRO, CW_GUIDE_RST_LINE, CW_KIND_LABEL, matchCwLookalikes, searchCwTerms } from '../../../src/utils/cwVocab';
import { QRZ_SIGN_IN_NOTE } from '../../../src/utils/cwFormat';
import { pushBack } from './gamepad';

const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

export function cwGuideOpen(): boolean { return !!document.getElementById('cwGuide'); }

export function openCwGuide(): void {
  if (cwGuideOpen()) return;
  const back = document.activeElement as HTMLElement | null;
  const ov = document.createElement('div');
  ov.id = 'cwGuide';
  ov.dataset.navscope = '';
  ov.setAttribute('role', 'dialog');
  ov.setAttribute('aria-modal', 'true');
  ov.setAttribute('aria-label', 'CW terms guide');
  ov.innerHTML =
    '<div class="cwgCard">' +
      '<div class="cwgHead"><span class="cwgTitle">CW TERMS</span><span class="cwgHint">esc or backspace closes</span></div>' +
      '<input id="cwGuideSearch" type="search" autocomplete="off" autocapitalize="characters" spellcheck="false" ' +
        'placeholder="Search: QTH, 73, report, DE…" aria-label="Search the CW terms">' +
      '<div id="cwGuideList"></div>' +
      '<button type="button" class="cwgClose">CLOSE</button>' +
    '</div>';
  const list = ov.querySelector<HTMLElement>('#cwGuideList')!;
  const input = ov.querySelector<HTMLInputElement>('#cwGuideSearch')!;
  const render = () => {
    const q = input.value;
    const terms = searchCwTerms(q), looks = matchCwLookalikes(q);
    let h = '';
    if (!q.trim()) h += `<p class="cwgPara">${esc(CW_GUIDE_INTRO)}</p><p class="cwgPara cwgStrong">${esc(CW_GUIDE_RST_LINE)}</p>`;
    if (!terms.length && !looks.length) h += `<p class="cwgPara cwgNone">Nothing matches “${esc(q.trim())}”.</p>`;
    for (const t of terms) {
      h += '<div class="cwgTerm" tabindex="0">' +
        `<div class="cwgTermHead"><span class="cwgCode">${esc(t.code)}${t.aliases?.length ? ' · ' + esc(t.aliases.join(' · ')) : ''}</span>` +
        `<span class="cwgKind">${CW_KIND_LABEL[t.kind]}</span></div>` +
        (t.plain && t.plain !== '—' ? `<div class="cwgPlain">“${esc(t.plain)}”${t.ask ? ` · with ?: “${esc(t.ask)}”` : ''}</div>` : '') +
        `<div class="cwgPara">${esc(t.meaning)}</div>` +
        (t.example ? `<div class="cwgEx">${esc(t.example)}</div>` : '') +
        '</div>';
    }
    if (looks.length) h += '<div class="cwgSec">EASILY CONFUSED</div>';
    for (const l of looks) {
      h += `<div class="cwgTerm cwgLook" tabindex="0"><div class="cwgLookTitle">${esc(l.title)}</div><div class="cwgPara">${esc(l.text)}</div></div>`;
    }
    h += `<p class="cwgPara cwgNote">Tap a callsign in the CW, PSK, JTTY or digital spots lists to look it up on QRZ.com (it opens in a new tab). ${esc(QRZ_SIGN_IN_NOTE)}</p>`;
    list.innerHTML = h;
  };
  input.addEventListener('input', render);
  let unBack: () => void = () => {};
  const close = () => {
    unBack();
    ov.remove();
    try { back?.focus?.(); } catch { /* gone */ }
  };
  ov.querySelector<HTMLButtonElement>('.cwgClose')!.onclick = close;
  ov.addEventListener('click', (e) => { if (e.target === ov) close(); });
  render();
  document.body.appendChild(ov);
  unBack = pushBack(close);
  // ★ The search box takes the cursor at once with a mouse or a finger; a pad's highlight starts there too.
  if (!matchMedia('(pointer: coarse)').matches) input.focus();
}
