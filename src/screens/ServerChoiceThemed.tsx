/**
 * ★★★ beta96 the server entry's screens in the user's colours — the same wrapper as ServerModeThemed (Lite renders the
 * screens themselves with no tokens and gets the Lite palette). See ServerChoiceScreen, SimpleServerScreen,
 * SetupElsewhereScreen.
 */
import React from 'react';
import { useGlassTheme } from '../components/PopupShell';
import { TvTextInputTokens } from '../components/TvTextInput';
import ServerChoiceScreen from './ServerChoiceScreen';
import SimpleServerScreen from './SimpleServerScreen';
import SetupElsewhereScreen from './SetupElsewhereScreen';

function themed<P extends { tokens?: any }>(Screen: React.ComponentType<P>) {
  return function Themed(props: P) {
    const tokens = useGlassTheme();
    return <TvTextInputTokens value={tokens}><Screen {...props} tokens={tokens} /></TvTextInputTokens>;
  };
}
export const ServerChoiceThemed = themed(ServerChoiceScreen);
export const SimpleServerThemed = themed(SimpleServerScreen);
export const SetupElsewhereThemed = themed(SetupElsewhereScreen);
