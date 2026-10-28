/** ★ V12 which servers send the full-capture row — wired to the app's storage. See wideCapsCore.ts. */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { WideCapsCore } from './wideCapsCore';

const core = new WideCapsCore({ get: (k) => AsyncStorage.getItem(k), set: (k, v) => AsyncStorage.setItem(k, v) });

/** Load the remembered list (awaited before a VibeServer connection, beside the listener id). */
export const loadWideCaps = (): Promise<void> => core.load();
/** `&wide=1` for a server that advertised SPWB v2, else ''. */
export const wideSuffix = (baseUrl: string): string => core.suffix(baseUrl);
/** Note what the server advertised (hwinfo `wide`, or the preflight's). */
export const noteWideCaps = (baseUrl: string, wide: unknown): boolean => core.note(baseUrl, wide);
