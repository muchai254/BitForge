// Asks an Electrum server for its version and chain tip (read-only).
'use strict';
const tls = require('tls');
const [host, port] = [process.argv[2], Number(process.argv[3])];
const t0 = Date.now();
const s = tls.connect({ host, port, servername: host, rejectUnauthorized: false, timeout: 20000 }, () => {
  console.log(`TLS connected in ${Date.now() - t0} ms; cert subject: ${JSON.stringify(s.getPeerCertificate().subject)}`);
  s.write(JSON.stringify({ id: 1, method: 'server.version', params: ['probe', '1.4'] }) + '\n');
  s.write(JSON.stringify({ id: 2, method: 'blockchain.headers.subscribe', params: [] }) + '\n');
});
let buf = '';
s.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const msg = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
    if (msg.id === 1) console.log('server.version:', JSON.stringify(msg.result || msg.error));
    if (msg.id === 2) {
      const h = msg.result.hex;
      // Header bytes 4..36 = previous block hash (little-endian); bits at 72..76.
      const bits = Buffer.from(h, 'hex').readUInt32LE(72).toString(16);
      console.log(`tip height ${msg.result.height}, bits 0x${bits} (signet's fixed target is 0x1e0377ae)`);
      console.log(`round trip ${Date.now() - t0} ms`);
      s.end(); process.exit(0);
    }
  }
});
s.on('timeout', () => { console.log('TIMEOUT'); process.exit(1); });
s.on('error', (e) => { console.log('ERROR', e.message); process.exit(1); });
