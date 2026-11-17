// Downloads previous transactions (public signet data) from an Electrum
// server and saves them as prevtx/<txid>.hex, after checking that each one
// really hashes to its txid. Used to build PSBT fixtures that carry
// non_witness_utxo (Sparrow strips it from QR exports).
// Usage: node fetch_prevtx.js <txid> [<txid> ...]
//        (server: signet-electrumx.wakiyamap.dev:50002, the one Sparrow uses)
'use strict';
const tls = require('tls');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const HOST = 'signet-electrumx.wakiyamap.dev', PORT = 50002;
const txids = process.argv.slice(2);
if (!txids.length || !txids.every((t) => /^[0-9a-f]{64}$/.test(t))) {
  console.error('usage: node fetch_prevtx.js <txid> [...]');
  process.exit(1);
}
const outDir = path.join(__dirname, 'prevtx');
fs.mkdirSync(outDir, { recursive: true });

// txid = double SHA-256 of the serialization without segwit marker, flag and
// witnesses, byte-reversed.
function txidOf(hex) {
  const b = Buffer.from(hex, 'hex');
  let p = 4;
  const segwit = b[4] === 0 && b[5] !== 0;
  if (segwit) p += 2;
  const start = p;
  const varint = () => { const f = b[p++]; if (f < 0xfd) return f; if (f === 0xfd) { p += 2; return b.readUInt16LE(p - 2); } p += 4; return b.readUInt32LE(p - 4); };
  const nIn = varint();
  for (let i = 0; i < nIn; i++) { p += 36; const n = varint(); p += n + 4; } // not p += varint(): p is read first
  const nOut = varint();
  for (let i = 0; i < nOut; i++) { p += 8; const n = varint(); p += n; }
  const end = p;
  if (segwit) for (let i = 0; i < nIn; i++) { const n = varint(); for (let k = 0; k < n; k++) { const m = varint(); p += m; } }
  const body = Buffer.concat([b.subarray(0, 4), b.subarray(start, end), b.subarray(p, p + 4)]);
  const h = crypto.createHash('sha256').update(crypto.createHash('sha256').update(body).digest()).digest();
  return Buffer.from(h).reverse().toString('hex');
}

const s = tls.connect({ host: HOST, port: PORT, servername: HOST, rejectUnauthorized: false, timeout: 20000 }, () => {
  s.write(JSON.stringify({ id: 0, method: 'server.version', params: ['airgap-fixtures', '1.4'] }) + '\n');
  txids.forEach((t, i) => s.write(JSON.stringify({ id: i + 1, method: 'blockchain.transaction.get', params: [t] }) + '\n'));
});
let buf = '', left = txids.length, bad = 0;
s.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const msg = JSON.parse(buf.slice(0, i));
    buf = buf.slice(i + 1);
    if (msg.id === 0) continue;
    const want = txids[msg.id - 1];
    if (msg.error || typeof msg.result !== 'string') { console.log(`${want}: ERROR ${JSON.stringify(msg.error)}`); bad++; }
    else if (txidOf(msg.result) !== want) { console.log(`${want}: hash MISMATCH, not saved`); bad++; }
    else {
      fs.writeFileSync(path.join(outDir, `${want}.hex`), msg.result + '\n');
      console.log(`${want}: ${msg.result.length / 2} bytes, txid verified, saved`);
    }
    if (--left === 0) { s.end(); process.exit(bad ? 1 : 0); }
  }
});
s.on('timeout', () => { console.log('TIMEOUT'); process.exit(1); });
s.on('error', (e) => { console.log('ERROR', e.message); process.exit(1); });
