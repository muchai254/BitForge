// Checks headers from an Electrum server against their own claimed PoW target.
// Usage: node pow_check.js <host> <ssl port>
'use strict';
const tls = require('tls');
const crypto = require('crypto');
const [host, port] = [process.argv[2], Number(process.argv[3])];
const sha256d = (b) => crypto.createHash('sha256').update(crypto.createHash('sha256').update(b).digest()).digest();
function check(hex, height) {
  const h = Buffer.from(hex, 'hex');
  const hash = Buffer.from(sha256d(h)).reverse();
  const bits = h.readUInt32LE(72);
  const exp = bits >>> 24, mant = BigInt(bits & 0x007fffff);
  const target = exp <= 3 ? mant >> BigInt(8 * (3 - exp)) : mant << BigInt(8 * (exp - 3));
  const ok = BigInt('0x' + hash.toString('hex')) <= target;
  console.log(`height ${height}: hash ${hash.toString('hex')} bits 0x${bits.toString(16)} -> ${ok ? 'meets its target' : 'DOES NOT meet its target'}`);
  return { hash: hash.toString('hex'), prev: Buffer.from(h.subarray(4, 36)).reverse().toString('hex') };
}
const s = tls.connect({ host, port, servername: host, rejectUnauthorized: false, timeout: 20000 }, () => {
  s.write(JSON.stringify({ id: 1, method: 'server.version', params: ['probe', '1.4'] }) + '\n');
  s.write(JSON.stringify({ id: 2, method: 'blockchain.headers.subscribe', params: [] }) + '\n');
});
let buf = '', tip = 0;
s.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const msg = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
    if (msg.id === 2) {
      tip = msg.result.height;
      check(msg.result.hex, tip);
      s.write(JSON.stringify({ id: 3, method: 'blockchain.block.header', params: [1] }) + '\n');
      s.write(JSON.stringify({ id: 4, method: 'blockchain.block.headers', params: [tip - 4, 4] }) + '\n');
    } else if (msg.id === 3) {
      const r = check(msg.result, 1);
      console.log(`block 1 prev hash ${r.prev} (signet genesis is 00000008819873e925422c1ff0f99f7cc9bbb232af63a077a480a3633bee1ef6)`);
    } else if (msg.id === 4) {
      const hex = msg.result.hex;
      for (let k = 0; k < msg.result.count; k++) check(hex.slice(k * 160, (k + 1) * 160), tip - 4 + k);
      s.end(); process.exit(0);
    }
  }
});
s.on('timeout', () => { console.log('TIMEOUT'); process.exit(1); });
s.on('error', (e) => { console.log('ERROR', e.message); process.exit(1); });
