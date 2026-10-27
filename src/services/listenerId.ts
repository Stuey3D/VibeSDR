/**
 * ★★★ THE APP'S LISTENER ID, wired to the app's storage — see listenerIdCore.ts for what it is and why.
 * Stuart, 2026-10-27: "2 radios one server is allowed unless server owner blocks the same connection using both radios
 * exactly how the web client handles it now".
 */
import 'react-native-get-random-values'; // polyfill for crypto.getRandomValues
import AsyncStorage from '@react-native-async-storage/async-storage';
import { v4 as uuidv4 } from 'uuid';
import { ListenerIdCore } from './listenerIdCore';

const core = new ListenerIdCore(
  { get: (k) => AsyncStorage.getItem(k), set: (k, v) => AsyncStorage.setItem(k, v) },
  () => uuidv4(),
);

/** Load (or make, once per install) the id. Called at start-up and awaited before every VibeServer connection. */
export const loadListenerId = (): Promise<string> => core.load();
/** The id if loaded, else ''. */
export const listenerIdNow = (): string => core.now();
/** `&bid=<id>` for a VibeServer socket URL, exactly as the web client sends it — '' until loaded. */
export const listenerIdQuery = (): string => core.query();
/** After a factory reset wipes storage: the next connection makes a new id. */
export const forgetListenerId = (): void => core.forget();
