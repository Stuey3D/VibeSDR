const { getDefaultConfig } = require('expo/metro-config');

const config = getDefaultConfig(__dirname);

// Reanimated v4, react-native-worklets, and Skia ship with raw 'worklet'
// directives that must pass through the Babel/worklets plugin.
// Extend Metro's transform ignore pattern to include these packages.
config.transformer = config.transformer ?? {};
config.transformer.transformIgnorePatterns = [
  'node_modules/(?!(' +
  [
    'react-native',
    '@react-native',
    '@react-navigation',
    'expo',
    '@expo',
    'react-native-reanimated',
    '@shopify/react-native-skia',
    'react-native-gesture-handler',
    'react-native-screens',
    'react-native-safe-area-context',
    '@react-native-async-storage',
    'react-native-get-random-values',
    'expo-blur',
    'expo-keep-awake',
    'uuid',
  ].join('|') +
  ')/)',
];

// ★ The GPU map's bundled files (assets/mapgl, see scripts/sync-mapgl-assets.mjs) ship as native assets:
//   PMTiles tile packs and MapLibre glyph PBFs. WebP is already an image asset.
config.resolver.assetExts = [...config.resolver.assetExts, 'pmtiles', 'pbf', 'txt'];   // .txt = the map page's scripts, see sync-mapgl-assets

module.exports = config;
