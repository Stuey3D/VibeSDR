/**
 * openWebPage — a page someone else publishes (the Signal Identification Wiki), shown as THEY publish it.
 *
 * ★★ Stuart, 2026-10-09: "can we include links to the wiki if we open them in our own browser view … so we are not
 *    modifying the pages in any way". The system in-app browser is exactly that: SFSafariViewController on iPhone and
 *    iPad, Chrome Custom Tabs on Android. The real site, its own address bar, its ads and attribution, the reader's own
 *    browser — nothing framed, injected or copied. Never a WebView that hides the address or restyles the page.
 * ★ On a Mac (the iPad app on Apple silicon) the page goes to the default browser, which is how a Mac opens a link.
 * ★ Any failure falls back to the system browser: a link that does nothing is the one outcome not allowed.
 */
import { Linking, Platform } from 'react-native';
import * as WebBrowser from 'expo-web-browser';
import { isMacHost } from '../services/macAudio';

export async function openWebPage(url: string): Promise<void> {
  if (!/^https:\/\//.test(url)) return;
  if (Platform.OS === 'web' || isMacHost()) { Linking.openURL(url).catch(() => {}); return; }
  try {
    await WebBrowser.openBrowserAsync(url, { dismissButtonStyle: 'done', enableBarCollapsing: true });
  } catch {
    Linking.openURL(url).catch(() => {});
  }
}
