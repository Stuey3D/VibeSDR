// Which Lite release "Check for updates" offers (src/services/liteUpdate.ts).
import { parseLiteVersion, compareLite, newerLiteRelease, liteVersionLabel, type LiteRelease } from '../src/services/liteUpdate';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const P = (s: string) => parseLiteVersion(s)!;
const rel = (tag: string, size = 45_000_000): LiteRelease =>
  ({ tag, apkName: 'x.apk', apkUrl: `https://github.com/Stuey3D/VibeSDR/releases/download/${tag}/x.apk`, size, published: '' });

console.log('test_lite_update');
ok(!!parseLiteVersion('lite-v11.0.0-rc37') && P('lite-v11.0.0-rc37').preN === 37, 'a release tag parses (lite-v11.0.0-rc37)');
ok(P('11.0.0~rc37-lite').preN === 37 && P('11.0.0~rc37-lite').pre === 'rc', 'an installed versionName parses (11.0.0~rc37-lite)');
ok(P('11.0.0~rc34test1-lite').test && P('11.0.0~rc34test1-lite').preN === 34, 'a test build parses (rc34test1)');
ok(parseLiteVersion('banana') === null && parseLiteVersion('') === null, 'nonsense is not a version');

ok(compareLite(P('lite-v11.0.0-rc38'), P('11.0.0~rc37-lite')) > 0, 'rc38 > rc37');
ok(compareLite(P('lite-v11.0.0-rc10'), P('11.0.0~rc9-lite')) > 0, 'rc10 > rc9 (numbers, not text)');
ok(compareLite(P('lite-v11.0.0'), P('11.0.0~rc99-lite')) > 0, 'the final release > any rc');
ok(compareLite(P('lite-v11.0.0-rc1'), P('11.0.0~b9-lite')) > 0, 'rc1 > beta 9');
ok(compareLite(P('lite-v11.0.1-rc1'), P('11.0.0-lite')) > 0, '11.0.1 rc1 > 11.0.0');
ok(compareLite(P('lite-v11.0.0-rc34'), P('11.0.0~rc34test1-lite')) > 0, 'rc34 > its own test build');
ok(compareLite(P('lite-v11.0.0-rc37'), P('11.0.0~rc37-lite')) === 0, 'the same version is the same');

// Chicopee's tablet: the rc34test1 antenna-switch build, with rc37 published.
const list = [rel('lite-v11.0.0-rc37'), rel('lite-v11.0.0-rc34test1'), rel('lite-v11.0.0-rc36')];
const pick = newerLiteRelease(list, '11.0.0~rc34test1-lite');
ok(!!pick && pick.tag === 'lite-v11.0.0-rc37', 'rc34test1 installed → RC37 is offered');
ok(newerLiteRelease(list, '11.0.0~rc37-lite') === null, 'already on the newest → nothing offered');
ok(newerLiteRelease([rel('lite-v11.0.0-rc38test1')], '11.0.0~rc37-lite') === null, 'a TEST release is never offered');
ok(newerLiteRelease([rel('lite-v11.0.0-rc38', 0)], '11.0.0~rc37-lite') === null, 'a release with no size is not offered (cannot be checked)');
ok(newerLiteRelease([rel('lite-v11.0.0-rc36')], '11.0.0~rc37-lite') === null, 'an OLDER release is never offered (no downgrade)');
ok(newerLiteRelease(list, 'hand-built') === null, 'an unreadable installed version is not offered a guess');
ok(liteVersionLabel(P('lite-v11.0.0-rc37')) === 'RC37' && liteVersionLabel(P('lite-v11.0.0')) === '11.0.0', 'labels: RC37, 11.0.0');

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
