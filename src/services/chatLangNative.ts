/**
 * chatLangNative — the APP's inputs to the chat's language (dialChat.ts chatLangChoice): the device's language list
 * and the language picked in the chat drawer. The browser has its own (web/client/src/chat.ts); the server's "cc"
 * arrives in the dial message and is fed in by SDRScreen.
 *
 * ★★★ NO NEW NATIVE MODULE. expo-localization is not installed, and a native dependency means pod install and a Cloud
 *     build. React Native already carries what is needed:
 *       iOS / iPadOS / Mac / tvOS — Settings 'AppleLanguages': the person's OWN preferred-language list, unfiltered.
 *         ✗ Not Intl / NSLocale.currentLocale: an app that ships one .lproj is told its own language (English), not
 *           the person's — on a Brazilian iPhone it would answer "en-BR".
 *       Android — I18nManager's localeIdentifier ("pt_BR"), the device's locale.
 *       Either, failing — Intl's resolved locale, which is at least never wrong about English.
 * ★ Read once at launch: changing the device's language restarts the app on both platforms.
 */
import { I18nManager, Platform, Settings } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { setChatLangInputs, isChatLang, type ChatLang } from './dialChat';

/** The chat drawer's language menu, remembered on this device. Absent / 'auto' = the automatic order. */
export const CHAT_LANG_KEY = 'vibe.chat.lang';

export function deviceLanguageTags(): string[] {
  const out: string[] = [];
  try {
    if (Platform.OS === 'ios') {
      const l = Settings.get('AppleLanguages');
      if (Array.isArray(l)) out.push(...l.filter((x): x is string => typeof x === 'string'));
    }
  } catch { /* Settings absent on this build — fall through */ }
  try {
    if (Platform.OS === 'android') {
      const id = (I18nManager.getConstants?.() as { localeIdentifier?: string } | undefined)?.localeIdentifier;
      if (id) out.push(id);
    }
  } catch { /* fall through */ }
  if (!out.length) {
    try { const l = Intl.DateTimeFormat().resolvedOptions().locale; if (l) out.push(l); } catch { /* English, then */ }
  }
  return out;
}

/** At launch: the device's list now, the remembered pick as soon as storage answers. */
export function initChatLanguage(): void {
  setChatLangInputs({ device: deviceLanguageTags() });
  AsyncStorage.getItem(CHAT_LANG_KEY)
    .then((v) => { if (isChatLang(v)) setChatLangInputs({ picked: v }); })
    .catch(() => { /* not remembered — the automatic order */ });
}

/** The drawer's menu: a language, or 'auto'. Changes only how THIS device draws the chat — ids are what is sent. */
export function pickChatLanguage(v: ChatLang | 'auto'): void {
  setChatLangInputs({ picked: v === 'auto' ? null : v });
  (v === 'auto' ? AsyncStorage.removeItem(CHAT_LANG_KEY) : AsyncStorage.setItem(CHAT_LANG_KEY, v)).catch(() => {});
}
