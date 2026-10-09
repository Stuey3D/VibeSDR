/**
 * openWebPage — a page someone else publishes (the Signal Identification Wiki), shown as THEY publish it.
 *
 * ★★ Stuart, 2026-10-09: open it "in the same way as if a link was clicked and it opened in Safari itself", so the site
 *    keeps its ad revenue and control of its pages and the reader's own ad blockers still apply. The system in-app
 *    browsers are exactly that — SFSafariViewController on iPhone and iPad (Done top left, ‹ › at the bottom), Chrome
 *    Custom Tabs on Android (✕ / the back gesture) — the real browser, nothing framed, injected or copied. NOT our own
 *    web view (BrowserOverlay, for receivers' admin pages): Safari's content blockers do not apply inside one.
 * ★ On a Mac (the iPad app on Apple silicon) there is no SFSafariViewController: Safari itself opens.
 * ★★ THE AUDIO. While the page is open, its clips take the audio session / focus; the native side fades the radio out
 *    for them and back in after (VibePowerModule / VibeStreamService setPageOpen). It must be told when the page
 *    CLOSES: iOS resolves openBrowserAsync on Done; Android resolves at once, so there the close is the app coming
 *    back to the foreground.
 * ★ Any failure falls back to the system browser: a link that does nothing is the one outcome not allowed.
 */
import { AppState, Linking, NativeModules, Platform } from 'react-native';
import * as WebBrowser from 'expo-web-browser';
import { isMacHost } from '../services/macAudio';

function setPageOpen(open: boolean) {
  try {
    if (Platform.OS === 'ios') NativeModules.VibePowerModule?.setPageOpen?.(open);
    else if (Platform.OS === 'android') NativeModules.VibeStreamModule?.setPageOpen?.(open);
  } catch { /* no native side (web / tests) */ }
}

export async function openWebPage(url: string): Promise<void> {
  if (!/^https:\/\//.test(url)) return;
  if (Platform.OS === 'web' || isMacHost()) { Linking.openURL(url).catch(() => {}); return; }
  setPageOpen(true);
  if (Platform.OS === 'android') {
    // ★ The tab sends us to the background; coming back to 'active' is the tab closing.
    let left = false;
    const sub = AppState.addEventListener('change', (st) => {
      if (st !== 'active') { left = true; return; }
      if (left) { sub.remove(); setPageOpen(false); }
    });
  }
  try {
    await WebBrowser.openBrowserAsync(url, { dismissButtonStyle: 'done', enableBarCollapsing: true });
    if (Platform.OS === 'ios') setPageOpen(false);
  } catch {
    setPageOpen(false);
    Linking.openURL(url).catch(() => {});
  }
}
