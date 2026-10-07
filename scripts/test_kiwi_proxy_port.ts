import { withKiwiProxyPort as k } from '../src/utils/kiwiProxy.ts';
const cases: [string,string][] = [
 ['http://db3th.proxy.kiwisdr.com','http://db3th.proxy.kiwisdr.com:8073'],
 ['http://db3th.proxy.kiwisdr.com/','http://db3th.proxy.kiwisdr.com:8073/'],
 ['http://db3th.proxy.kiwisdr.com:8073','http://db3th.proxy.kiwisdr.com:8073'],
 ['http://x.proxy.kiwisdr.com:8074/','http://x.proxy.kiwisdr.com:8074/'],
 ['ws://22178.proxy.kiwisdr.com','ws://22178.proxy.kiwisdr.com:8073'],
 ['22178.proxy.kiwisdr.com','22178.proxy.kiwisdr.com:8073'],
 ['https://X.Proxy.KiwiSDR.com/?f=7795','https://X.Proxy.KiwiSDR.com:8073/?f=7795'],
 ['http://kiwisdr.areg.org.au:8074','http://kiwisdr.areg.org.au:8074'],
 ['http://proxy.kiwisdr.com.evil.net','http://proxy.kiwisdr.com.evil.net'],
 ['http://sdr.example.com','http://sdr.example.com'],
 ['',''],
];
// proxy.kiwisdr.com listens on 8073 only, but the public list names 47 % of Kiwis without a port (2026-10-07).
let bad=0; for (const [i,w] of cases){ const g=k(i); if(g!==w){bad++;console.log('FAIL',i,'→',g,'want',w);} }
console.log(bad?`${bad} FAIL`:`all ${cases.length} ok`); if (bad) process.exit(1);
