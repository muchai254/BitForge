// Checks bech32 addresses and derives m/.../0/0 and /1/0 P2WPKH addresses
// from a tpub (BIP32 public derivation, secp256k1 in BigInt). Public data only.
// Usage: node addr_check.js <tpub> [<receive /0/0 addr> <change /1/0 addr>]
//        node addr_check.js <tpub> --at <chain> <index> [<index>...]
'use strict';
const crypto = require('crypto');
const [tpub, recv, chg] = process.argv.slice(2);

// secp256k1
const P = 2n ** 256n - 2n ** 32n - 977n;
const N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141n;
const G = [0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798n,
           0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8n];
const mod = (a, m = P) => ((a % m) + m) % m;
const inv = (a) => { let [r0, r1, s0, s1] = [mod(a), P, 1n, 0n]; while (r1) { const q = r0 / r1; [r0, r1] = [r1, r0 - q * r1]; [s0, s1] = [s1, s0 - q * s1]; } return mod(s0); };
function add(p, q) {
  if (!p) return q; if (!q) return p;
  if (p[0] === q[0] && mod(p[1] + q[1]) === 0n) return null;
  const l = p[0] === q[0] ? mod(3n * p[0] * p[0] * inv(2n * p[1])) : mod((q[1] - p[1]) * inv(q[0] - p[0]));
  const x = mod(l * l - p[0] - q[0]);
  return [x, mod(l * (p[0] - x) - p[1])];
}
function mul(k, p) { let r = null; while (k > 0n) { if (k & 1n) r = add(r, p); p = add(p, p); k >>= 1n; } return r; }
function decompress(b) {
  const x = BigInt('0x' + b.subarray(1).toString('hex'));
  let y = 1n, base = mod(x ** 3n + 7n), e = (P + 1n) / 4n;
  while (e) { if (e & 1n) y = mod(y * base); base = mod(base * base); e >>= 1n; }
  if (Number(y & 1n) !== (b[0] & 1)) y = P - y;
  return [x, y];
}
const compress = (p) => Buffer.concat([Buffer.from([p[1] & 1n ? 3 : 2]), Buffer.from(p[0].toString(16).padStart(64, '0'), 'hex')]);

// base58check tpub
const A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
let n = 0n; for (const c of tpub) n = n * 58n + BigInt(A.indexOf(c));
const raw = Buffer.from(n.toString(16).padStart(164, '0'), 'hex');
const sha = (x) => crypto.createHash('sha256').update(x).digest();
if (!sha(sha(raw.subarray(0, 78))).subarray(0, 4).equals(raw.subarray(78))) throw new Error('tpub checksum');
const chain = raw.subarray(13, 45), key = raw.subarray(45, 78);

function ckdPub(cc, K, i) {
  const data = Buffer.concat([K, Buffer.from([i >>> 24, (i >>> 16) & 255, (i >>> 8) & 255, i & 255])]);
  const I = crypto.createHmac('sha512', cc).update(data).digest();
  const il = BigInt('0x' + I.subarray(0, 32).toString('hex'));
  if (il >= N) throw new Error('invalid child');
  return { cc: I.subarray(32), K: compress(add(mul(il, G), decompress(K))) };
}

// bech32 (BIP173)
const CH = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';
function polymod(v) { const g = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]; let c = 1; for (const x of v) { const b = c >>> 25; c = ((c & 0x1ffffff) << 5) ^ x; for (let i = 0; i < 5; i++) if ((b >>> i) & 1) c ^= g[i]; } return c >>> 0; }
const hrpExp = (h) => [...h].map((c) => c.charCodeAt(0) >> 5).concat([0], [...h].map((c) => c.charCodeAt(0) & 31));
function convert(data, from, to, pad) { let acc = 0, bits = 0; const out = []; for (const v of data) { acc = (acc << from) | v; bits += from; while (bits >= to) { bits -= to; out.push((acc >> bits) & ((1 << to) - 1)); } } if (pad && bits) out.push((acc << (to - bits)) & ((1 << to) - 1)); return out; }
function encode(hrp, prog) { const d = [0].concat(convert(prog, 8, 5, true)); const pm = polymod(hrpExp(hrp).concat(d, [0, 0, 0, 0, 0, 0])) ^ 1; const cs = []; for (let i = 0; i < 6; i++) cs.push((pm >>> (5 * (5 - i))) & 31); return hrp + '1' + d.concat(cs).map((x) => CH[x]).join(''); }
function checksumOk(a) { const i = a.lastIndexOf('1'); const hrp = a.slice(0, i); const d = [...a.slice(i + 1)].map((c) => CH.indexOf(c)); return d.every((x) => x >= 0) && polymod(hrpExp(hrp).concat(d)) === 1; }
const hash160 = (b) => crypto.createHash('ripemd160').update(sha(b)).digest();

if (recv === '--at') {
  const c = ckdPub(chain, key, Number(chg));
  for (const i of process.argv.slice(5).map(Number)) {
    console.log(`${chg === '0' ? 'receive' : 'change '} #${i} m/84'/1'/0'/${chg}/${i} ${encode('tb', hash160(ckdPub(c.cc, c.K, i).K))}`);
  }
  process.exit(0);
}
// With only a tpub: list receive 0..4 and change 0..1.
if (!recv) {
  for (const [chainIdx, count] of [[0, 5], [1, 2]]) {
    const c = ckdPub(chain, key, chainIdx);
    for (let i = 0; i < count; i++) {
      const addr = encode('tb', hash160(ckdPub(c.cc, c.K, i).K));
      console.log(`${chainIdx === 0 ? 'receive' : 'change '} #${i} m/84'/1'/0'/${chainIdx}/${i} ${addr}`);
    }
  }
  process.exit(0);
}
for (const [label, a] of [['receive', recv], ['change', chg]]) console.log(`${label} address bech32 checksum: ${checksumOk(a) ? 'OK' : 'BAD'}`);
for (const [label, chainIdx, given] of [['receive /0/0', 0, recv], ['change  /1/0', 1, chg]]) {
  const c = ckdPub(chain, key, chainIdx);
  const k = ckdPub(c.cc, c.K, 0);
  const addr = encode('tb', hash160(k.K));
  console.log(`${label}: derived ${addr} ${addr === given ? '== Sparrow ✓' : '!= Sparrow ' + given}`);
}
