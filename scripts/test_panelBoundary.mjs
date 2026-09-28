// Runner for scripts/test_panelBoundary.tsx — see that file for what it proves.
//   node scripts/test_panelBoundary.mjs
// ★ Bundles with esbuild into the OS temp dir, aliasing react-native (and the storage module
//   crashGuard imports) to tiny stubs, so the REAL PanelBoundary/faultLog/crashGuard run in Node.
import { build } from 'esbuild';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { rmSync } from 'node:fs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(tmpdir(), `vibesdr-test-panelBoundary-${process.pid}.mjs`);
const stubs = {
  name: 'stubs',
  setup(b) {
    b.onResolve({ filter: /^(react-native|@react-native-async-storage\/async-storage)$/ }, (a) => ({ path: a.path, namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, (a) => ({
      loader: 'js',
      contents: a.path === 'react-native'
        ? `export const View='View', Text='Text', TouchableOpacity='TouchableOpacity';
           export const Alert={alert(){}}; export const Platform={OS:'ios'}; export const NativeModules={};`
        : `export default { setItem: async () => {}, getItem: async () => null, removeItem: async () => {} };`,
    }));
  },
};
await build({
  entryPoints: [path.join(root, 'scripts/test_panelBoundary.tsx')],
  bundle: true, platform: 'node', format: 'esm', outfile: out, jsx: 'automatic',
  plugins: [stubs], logLevel: 'error',
  banner: { js: "import { createRequire as __cr } from 'module'; const require = __cr(import.meta.url);" },
  define: { 'process.env.NODE_ENV': '"development"' },
});
try { await import(pathToFileURL(out).href); }
finally { process.on('exit', () => { try { rmSync(out); } catch (e) { console.error('cleanup', e); } }); }
