// CarFM: permanent in-car install behavior (spec §5c).
//
// When autostart is on, the app auto-connects a plugged-in RTL-SDR on launch and
// drops straight into the FM face — so a cold boot with the ignition comes up
// playing, with no manual "start" step. Without a dongle attached it's a no-op
// and the normal instance picker shows, so leaving it on is harmless.
//
// Defaults ON: this fork's whole purpose is the car FM radio. A settings toggle
// can flip it off for phone/dev use.

import AsyncStorage from '@react-native-async-storage/async-storage';

const KEY = '@vibesdr/car_autostart';

export async function getCarAutostart(): Promise<boolean> {
  try {
    const v = await AsyncStorage.getItem(KEY);
    return v == null ? true : v === '1';
  } catch {
    return true;
  }
}

export async function setCarAutostart(on: boolean): Promise<void> {
  try { await AsyncStorage.setItem(KEY, on ? '1' : '0'); } catch {}
}
