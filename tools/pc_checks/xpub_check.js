// Decodes a BIP32 extended public key (base58check) and prints its fields.
'use strict';
const crypto = require('crypto');
const A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
const s = process.argv[2];
let n = 0n;
for (const c of s) { const v = A.indexOf(c); if (v < 0) throw new Error('bad char ' + c); n = n * 58n + BigInt(v); }
let hex = n.toString(16); if (hex.length % 2) hex = '0' + hex;
let b = Buffer.from(hex, 'hex');
const zeros = s.match(/^1*/)[0].length; b = Buffer.concat([Buffer.alloc(zeros), b]);
const data = b.subarray(0, b.length - 4), chk = b.subarray(b.length - 4);
const sha = (x) => crypto.createHash('sha256').update(x).digest();
const ok = sha(sha(data)).subarray(0, 4).equals(chk);
console.log('length', data.length, 'checksum', ok ? 'OK' : 'BAD');
const ver = data.readUInt32BE(0).toString(16).padStart(8, '0');
console.log('version 0x' + ver, ver === '043587cf' ? '(tpub: testnet/signet public)' : '');
console.log('depth', data[4], '(3 = m/84\'/1\'/0\')');
console.log('parent fingerprint', data.subarray(5, 9).toString('hex'), '(of m/84\'/1\', not the master)');
const child = data.readUInt32BE(9);
console.log('child number', child >= 0x80000000 ? `${child - 0x80000000}' (hardened)` : child);
console.log('pubkey prefix', data[45].toString(16), '(02 or 03 = compressed)');
