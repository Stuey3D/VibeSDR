/**
 * tv.ts — "are we on an Apple TV?", and the list of things that answer changes.
 *
 * ★★★ WHY A CONSTANT AND NOT A `Platform.OS === 'tvos'` CHECK SCATTERED AROUND.
 *     tvOS is the same JS bundle as iOS — `Platform.OS` is still `'ios'` there — so the only
 *     honest test is `Platform.isTV`. Putting it behind one name means the REASON each surface is
 *     cut is written down once, in `briefs/BRIEF-tvos-app.md`, instead of being re-derived (or guessed at)
 *     every time somebody touches a screen.
 *
 * ★★ WHAT IS CUT ON tvOS, AND WHY — the rule is: IF IT NEEDS A WEBVIEW OR LOCAL STORAGE, IT IS OUT.
 *   - **Spot maps** (`MapOverlay`) and the **in-app browser** (`BrowserOverlay`) are Leaflet/web in
 *     a `WebView`, and there is NO WKWebView on tvOS. Not "degraded" — absent from the platform.
 *   - **Kiwi compatibility mode** opens a Kiwi's own web UI in that same WebView. It fails twice:
 *     no webview, and a Kiwi web UI is not navigable from a remote even given one.
 *   - **Recordings**: tvOS has no user-facing persistent storage — an app's local data is a
 *     PURGEABLE cache the system may reclaim. A recorder that silently loses recordings is worse
 *     than no recorder.
 *   - **Share**: `UIActivityViewController` does not exist on tvOS. There is nowhere to share to.
 *   - **Server mode / local hardware**: an Apple TV has no USB, so it can never BE a receiver.
 *     ★ Note this does NOT cut the gain/AGC/notch controls — on a VibeServer those are REMOTE
 *       controls of the server's radio and are required. See the brief, §6.1.
 *
 * ★ Bookmarks and favourites still persist: they go through iCloud, exactly as the watch already
 *   consumes them. Small user-owned state that must survive is precisely what tvOS expects to sync.
 */
import { Platform } from 'react-native';

/** True on Apple TV. `Platform.OS` is still 'ios' there, so this is the only real test. */
export const IS_TV: boolean = Platform.isTV === true;

/** Anything that renders into a WebView. There is no WKWebView on tvOS. */
export const TV_HIDES_WEBVIEWS = IS_TV;

/** Recordings need durable local storage, which tvOS does not offer. */
export const TV_HIDES_RECORDINGS = IS_TV;

/** No share sheet exists on tvOS. */
export const TV_HIDES_SHARE = IS_TV;

/** An Apple TV cannot host a receiver — no USB. (Remote control of a server is unaffected.) */
export const TV_HIDES_SERVER_MODE = IS_TV;
