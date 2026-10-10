// The Recordings list: by server, then newest first (src/services/recordingGroups.ts).
import { groupRecordings, NO_SERVER } from '../src/services/recordingGroups';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const r = (name: string, mtime: number) => ({ name, mtime });

console.log('test_recording_groups');
const recs = [r('a', 100), r('b', 300), r('c', 200), r('d', 50), r('e', 400), r('old', 10)];
const server: Record<string, string> = { a: 'Pi 500', b: 'Pi 500', c: 'airspy kiwi', d: 'Zed', e: 'airspy kiwi' };
const g = groupRecordings(recs, (n) => server[n]);
ok(g.map((x) => x.server).join('|') === 'airspy kiwi|Pi 500|Zed|', 'servers A–Z (ignoring case), no-server group last');
ok(g[0].recs.map((x) => x.name).join('') === 'ec', 'within a server, newest first');
ok(g[1].recs.map((x) => x.name).join('') === 'ba', '…for every server');
ok(g[3].server === NO_SERVER && g[3].recs[0].name === 'old', 'a recording with no server recorded is kept, under its own heading');

const inRx = groupRecordings(recs, (n) => server[n], 'Zed');
ok(inRx[0].server === 'Zed', "inside a receiver, that receiver's recordings come first");
ok(inRx.map((x) => x.server).join('|') === 'Zed|airspy kiwi|Pi 500|', '…then the others A–Z, no-server last');
ok(groupRecordings([], () => undefined).length === 0, 'no recordings → no groups');
ok(groupRecordings([r('x', 1)], () => '  ').length === 1 && groupRecordings([r('x', 1)], () => '  ')[0].server === NO_SERVER,
   'a blank server name counts as none');

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
