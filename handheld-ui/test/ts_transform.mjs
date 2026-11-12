// For gen_kb_golden.ts: node's built-in TS support only STRIPS types, and src/utils/gamepadNav.ts (which kbLayouts.ts
// imports) uses a constructor parameter property. A module node refuses that way is transpiled with the repo's own
// `typescript` instead — the source itself is not changed. Use with ts_resolve.mjs: --import ./ts_resolve.mjs --import ./ts_transform.mjs
import { register } from 'node:module';
import { pathToFileURL } from 'node:url';
register('data:text/javascript,' + encodeURIComponent(`
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
const ROOT = ${JSON.stringify(pathToFileURL(new URL('../../', import.meta.url).pathname).href)};
export async function load(url, ctx, next) {
  try { return await next(url, ctx); }
  catch (e) {
    if (e && e.code === 'ERR_UNSUPPORTED_TYPESCRIPT_SYNTAX' && url.endsWith('.ts')) {
      const ts = createRequire(ROOT)('typescript');
      const out = ts.transpileModule(readFileSync(fileURLToPath(url), 'utf8'), {
        compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022, verbatimModuleSyntax: false },
        fileName: fileURLToPath(url),
      });
      return { format: 'module', source: out.outputText, shortCircuit: true };
    }
    throw e;
  }
}`));
