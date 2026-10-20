// tls-test-server.mjs — a stand-in for an OpenWebRX+ with its own certificate (GitHub #52), for the pin tests.
//
// OWRX+ wraps its listening socket in TLS when /etc/openwebrx/cert.pem + key.pem exist (openwebrx.py, Python's
// ssl.SSLContext(PROTOCOL_TLS_SERVER) — it serves the leaf alone, no chain). This does the same on 127.0.0.1:
//   GET /      → a page containing "openwebrx" (what detectServerType sniffs)
//   GET /ws/   → a WebSocket upgrade, then the server's hello: "CLIENT DE SERVER server=openwebrx version=v1.2.126"
// ★ Every request that REACHES it is printed ("REQ GET /", "UPGRADE /ws/"), so a test can prove a refused
//   certificate never let the request out — the cookie, the User-Agent, the admin credential — not just that the
//   client reported an error.
// Usage: node tls-test-server.mjs <cert.pem> <key.pem> <port> [<cert2> <key2> <port2> …]
import https from 'node:https';
import fs from 'node:fs';
import crypto from 'node:crypto';

const args = process.argv.slice(2);
if (args.length < 3 || args.length % 3) { console.error('usage: <cert> <key> <port> …'); process.exit(2); }
for (let i = 0; i < args.length; i += 3) {
  const [cert, key, port] = [args[i], args[i + 1], Number(args[i + 2])];
  const srv = https.createServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key) }, (req, res) => {
    console.log(`REQ ${port} ${req.method} ${req.url}`);
    res.writeHead(200, { 'Content-Type': 'text/html' });
    res.end('<html><head><title>OpenWebRX+ (test)</title></head><body>openwebrx</body></html>');
  });
  srv.on('upgrade', (req, sock) => {
    console.log(`UPGRADE ${port} ${req.url}`);
    const accept = crypto.createHash('sha1')
      .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
             + `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const msg = Buffer.from('CLIENT DE SERVER server=openwebrx version=v1.2.126');
    sock.write(Buffer.concat([Buffer.from([0x81, msg.length]), msg]));
    sock.on('data', () => {});
    sock.on('error', () => {});
  });
  srv.on('tlsClientError', (e) => console.log(`TLSERR ${port} ${e.code || e.message}`));
  srv.listen(port, '127.0.0.1', () => console.log(`LISTEN ${port}`));
}
