/**
 * serverPrefsStore — the server's settings, kept in a FILE as well as in AsyncStorage.
 *
 * ★★★ WHY THIS EXISTS. AsyncStorage can stop answering — not throw, not return, just never settle.
 *   Stuart's Sony, twice (2026-09-22 and again 2026-09-26): the setup screen came up on defaults
 *   with "Your saved settings could not be read", and the server could not be started at all. The
 *   09-22 fix put an 8 s deadline on the read, which turned an infinite hang into a visible
 *   failure — an improvement, and still a dead end, because the only button offered was "try
 *   again" and trying again hit the same wedged database.
 *   ★★ Stuart, 2026-09-26: *"hey this item is fucked and you cannot start the server, but fuck are
 *      we gonna give you the tools to fix it, nah you're fucked"* — and he is right. A warning with
 *      no way to act on it is the same fault as a control whose every use is a no-op.
 *   ★★★ SO THE REAL FIX IS NOT A BETTER ERROR: it is not depending on one store. *"if there is an
 *      issue with settings saving we need to save them to a local storage space instead"* — this.
 *
 * ★★★ THE FILE IS THE RELIABLE SIDE, ASYNCSTORAGE IS THE COMPATIBLE ONE.
 *   Writes go to BOTH. Reads try AsyncStorage briefly and fall back to the file. That ordering is
 *   deliberate: AsyncStorage stays authoritative while it is healthy, so nothing about an existing
 *   install changes and no migration is needed — but the moment it stops answering, the settings
 *   are still there, still readable, and the server still starts.
 * ★★ A WEDGED STORE MUST COST MILLISECONDS, NOT SECONDS. The read deadline here is short (1.2 s)
 *    because the file behind it is instant: there is no reason to make a person wait 8 s for a
 *    database that has already failed when the answer is sitting in a JSON file. The 8 s deadline
 *    in ServerModeScreen stays as the outer net for everything else in that load.
 * ★ ONE FILE, NOT ONE PER KEY. 40 setItem calls at ~300 ms of debounce is one small write, where a
 *   file per key would be 40 opens on a TV's slow flash every time a slider moves.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import * as FileSystem from 'expo-file-system/legacy';

/** ★ Alongside the app's other data, not in a cache directory — a cache can be reclaimed by the
 *  system at any time, and settings that evaporate under disk pressure would be a worse bug than
 *  the one this file exists to fix. */
const FILE = `${FileSystem.documentDirectory}server-prefs.json`;

/** The whole mirror, in memory. `null` until the first read has looked at the file. */
let mirror: Record<string, string | null> | null = null;
/** True once we have tried the file, even if it was absent — so a missing file is not re-read. */
let mirrorLoaded = false;
let writeTimer: ReturnType<typeof setTimeout> | null = null;
let writing = false;

/** ★ A promise that loses a race after `ms`. Rejects rather than resolving to a default, so the
 *  caller can tell "storage did not answer" from "storage answered null", which are different
 *  facts and lead to different behaviour. */
const deadline = <T,>(p: Promise<T>, ms: number): Promise<T> =>
  Promise.race([p, new Promise<T>((_, rej) =>
    setTimeout(() => rej(new Error('storage did not answer in time')), ms))]);

async function loadMirror(): Promise<Record<string, string | null>> {
  if (mirrorLoaded && mirror) return mirror;
  mirrorLoaded = true;
  try {
    const info = await FileSystem.getInfoAsync(FILE);
    if (info.exists) {
      const raw = await FileSystem.readAsStringAsync(FILE);
      const parsed = JSON.parse(raw);
      if (parsed && typeof parsed === 'object') { mirror = parsed; return mirror!; }
    }
  } catch (e) {
    /* ★ A corrupt or unreadable mirror is not fatal — it is one of two stores. Say so and carry on
     *  with an empty one; the next write repairs it. ✗ Never let this throw into the caller: the
     *  whole point of this module is that a storage fault cannot stop the server starting. */
    console.warn('serverPrefs: mirror unreadable, starting a fresh one —', e);
  }
  mirror = mirror ?? {};
  return mirror;
}

/** ★ Debounced, and it writes the WHOLE map. Settings arrive in bursts (a screen load applies
 *  several at once), and one write per burst is what keeps this cheap on a TV. */
function scheduleWrite() {
  if (writeTimer) clearTimeout(writeTimer);
  writeTimer = setTimeout(async () => {
    writeTimer = null;
    if (writing || !mirror) return;
    writing = true;
    try {
      await FileSystem.writeAsStringAsync(FILE, JSON.stringify(mirror));
    } catch (e) {
      console.warn('serverPrefs: could not write the mirror —', e);
    } finally { writing = false; }
  }, 300);
}

/** Read a setting. AsyncStorage first, the file if it does not answer. Never throws. */
export async function prefGet(key: string): Promise<string | null> {
  try {
    const v = await deadline(AsyncStorage.getItem(key), 1200);
    /* ★★ KEEP THE MIRROR CURRENT FROM HEALTHY READS TOO. Without this an install that has never
     *   written a setting since this module shipped would have an empty file, and the first time
     *   AsyncStorage wedged the fallback would be blank — a fallback that only works after you
     *   have already changed something is not a fallback. */
    const m = await loadMirror();
    if (m[key] !== v) { m[key] = v; scheduleWrite(); }
    return v;
  } catch {
    const m = await loadMirror();
    const v = m[key] ?? null;
    console.warn(`serverPrefs: AsyncStorage did not answer for "${key}" — used the file mirror`);
    return v;
  }
}

/** Write a setting to BOTH stores. Never throws; the file write is the one that matters. */
export async function prefSet(key: string, value: string): Promise<void> {
  const m = await loadMirror();
  m[key] = value;
  scheduleWrite();
  /* ★ AsyncStorage second and its failure swallowed: the setting is already safe in the mirror, so
   *  a wedged database must not surface as a failed save to the person changing a slider. */
  try { await deadline(AsyncStorage.setItem(key, value), 1200); }
  catch { console.warn(`serverPrefs: AsyncStorage did not accept "${key}" — the file has it`); }
}

/** Remove a setting from both stores. Never throws. */
export async function prefRemove(key: string): Promise<void> {
  const m = await loadMirror();
  if (key in m) { delete m[key]; scheduleWrite(); }
  try { await deadline(AsyncStorage.removeItem(key), 1200); } catch { /* the mirror is authoritative */ }
}

/** ★★★ THE WAY OUT, for the setup screen's error path — see the note at the top.
 *  Wipes both stores so a wedged or corrupt database can be recovered FROM INSIDE THE APP, without
 *  a laptop and adb to rescue a television. Destructive on purpose and by request only. */
export async function prefResetAll(): Promise<void> {
  mirror = {}; mirrorLoaded = true;
  try { await FileSystem.deleteAsync(FILE, { idempotent: true }); } catch { /* already gone */ }
  try { await deadline(AsyncStorage.clear(), 2000); } catch { /* it is the broken one; the file is gone */ }
}

/** ★ Whether the FILE holds anything — lets the error path say "we have a copy" rather than
 *  offering a reset to somebody whose settings would actually survive a retry. */
export async function prefMirrorHasData(): Promise<boolean> {
  const m = await loadMirror();
  return Object.keys(m).length > 0;
}
