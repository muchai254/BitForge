// BIP380 descriptor checksum (port of Bitcoin Core descriptor.cpp), for
// checking the C++ version and the descriptor the device will export.
// Usage: node desc_checksum.js [<descriptor>]   (always runs the self-test)
'use strict';
const INPUT = "0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\ ";
const CHECK = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';
function polymod(c, val) {
  const c0 = c >> 35n;
  c = ((c & 0x7ffffffffn) << 5n) ^ BigInt(val);
  if (c0 & 1n) c ^= 0xf5dee51989n;
  if (c0 & 2n) c ^= 0xa9fdca3312n;
  if (c0 & 4n) c ^= 0x1bab10e32dn;
  if (c0 & 8n) c ^= 0x3706b1677an;
  if (c0 & 16n) c ^= 0x644d626ffdn;
  return c;
}
function checksum(s) {
  let c = 1n, cls = 0, clscount = 0;
  for (const ch of s) {
    const pos = INPUT.indexOf(ch);
    if (pos < 0) return null;
    c = polymod(c, pos & 31);
    cls = cls * 3 + (pos >> 5);
    if (++clscount === 3) { c = polymod(c, cls); cls = 0; clscount = 0; }
  }
  if (clscount > 0) c = polymod(c, cls);
  for (let j = 0; j < 8; j++) c = polymod(c, 0);
  c ^= 1n;
  let out = '';
  for (let j = 0; j < 8; j++) out += CHECK[Number((c >> BigInt(5 * (7 - j))) & 31n)];
  return out;
}
const vectors = [
  ['raw(deadbeef)', '89f8spxm'],
  ["pkh([d34db33f/44'/0'/0']xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/1/*)", 'ml40v0wf'],
];
for (const [d, want] of vectors) { const got = checksum(d); console.log(`${got === want ? 'OK ' : 'BAD'} ${d.slice(0, 40)}… #${got} (expected #${want})`); }
if (process.argv[2]) console.log(process.argv[2] + '#' + checksum(process.argv[2]));
