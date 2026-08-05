// Exercise GET/POST /vibeserver/config, including the auth gate.
import crypto from 'node:crypto';
const PORT = process.argv[2], HOST='127.0.0.1', PASS='secret';
const base = `http://${HOST}:${PORT}`;
async function nonce(){ const r=await fetch(`${base}/vibeserver/auth`); return (await r.json()).nonce; }
function hmac(secret,n){ return crypto.createHmac('sha256',secret).update(n).digest('hex'); }
async function authQ(pass=PASS){ const n=await nonce(); return `vs_admin_nonce=${n}&vs_admin_auth=${hmac(pass,n)}`; }

console.log('1. GET with NO credentials');
let r = await fetch(`${base}/vibeserver/config`);
console.log('   ->', r.status, (await r.text()).slice(0,60));

console.log('2. GET with WRONG password');
r = await fetch(`${base}/vibeserver/config?${await authQ('wrong')}`);
console.log('   ->', r.status, (await r.text()).slice(0,60));

console.log('3. GET with the right password');
r = await fetch(`${base}/vibeserver/config?${await authQ()}`);
const cfg = await r.json();
console.log('   ->', r.status, 'name=', JSON.stringify(cfg.name), 'users=', cfg.users, 'configured=', cfg.configured);

console.log('4. POST a change (name + users)');
const body = JSON.stringify({ name: 'Renamed By API', users: 7, lockFreq: 6500000 });
r = await fetch(`${base}/vibeserver/config?${await authQ()}`, {method:'POST', body});
console.log('   ->', r.status, await r.text());

console.log('5. POST a CONTRADICTORY change (users 9, no locked centre)');
r = await fetch(`${base}/vibeserver/config?${await authQ()}`, {method:'POST', body: JSON.stringify({users:9, lockFreq:0})});
console.log('   ->', r.status, await r.text());
