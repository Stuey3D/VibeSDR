/**
 * ServerModeThemed — the main app's "Use as server" route: ServerModeScreen in the user's colours.
 *
 * ★★ WHY A WRAPPER: VibeServer Lite bundles ServerModeScreen itself (lite/app/index.js) on React Native 0.73 with no
 *   Reanimated or Skia, so the screen must never import PopupShell / FaceplateContext. The main app reads the glass
 *   tokens here — the same `useGlassTheme()` the directory uses (text = TEXT colour, keys = CONTROLS colour, meanings
 *   by the clash rule, whatever the chassis: this screen is never plated) — and hands them in. Lite passes nothing and
 *   gets constants/serverPalette `LITE_TOKENS`.
 */
import React from 'react';
import ServerModeScreen from './ServerModeScreen';
import { useGlassTheme } from '../components/PopupShell';
import { TvTextInputTokens } from '../components/TvTextInput';

export default function ServerModeThemed(props: React.ComponentProps<typeof ServerModeScreen>) {
  const tokens = useGlassTheme();
  // ★ The TV text fields (TvTextInput, Lite-safe) take the same tokens through their own context.
  return (
    <TvTextInputTokens value={tokens}>
      <ServerModeScreen {...props} tokens={tokens} />
    </TvTextInputTokens>
  );
}
