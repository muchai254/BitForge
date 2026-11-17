// BIP39 mnemonic + passphrase -> BIP32 master fingerprint, independently of
// the device (Node crypto + BigInt secp256k1). Usage: node fingerprint.js "<passphrase>"
'use strict';
const crypto = require('crypto');
const mnemonic = 'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about';
const pass = process.argv[2] ?? '';

const P = 2n ** 256n - 2n ** 32n - 977n;
const G = [0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798n,
           0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8n];
const mod = (a) => ((a % P) + P) % P;
const inv = (a) => { let [r0, r1, s0, s1] = [mod(a), P, 1n, 0n]; while (r1) { const q = r0 / r1; [r0, r1] = [r1, r0 - q * r1]; [s0, s1] = [s1, s0 - q * s1]; } return mod(s0); };
function add(p, q) {
  if (!p) return q; if (!q) return p;
  if (p[0] === q[0] && mod(p[1] + q[1]) === 0n) return null;
  const l = p[0] === q[0] ? mod(3n * p[0] * p[0] * inv(2n * p[1])) : mod((q[1] - p[1]) * inv(q[0] - p[0]));
  const x = mod(l * l - p[0] - q[0]);
  return [x, mod(l * (p[0] - x) - p[1])];
}
function mul(k, p) { let r = null; while (k > 0n) { if (k & 1n) r = add(r, p); p = add(p, p); k >>= 1n; } return r; }

// BIP39: PBKDF2-HMAC-SHA512, 2048 rounds, salt "mnemonic" + passphrase (NFKD; ASCII is unchanged).
const seed = crypto.pbkdf2Sync(Buffer.from(mnemonic.normalize('NFKD')), Buffer.from(('mnemonic' + pass).normalize('NFKD')), 2048, 64, 'sha512');
const I = crypto.createHmac('sha512', 'Bitcoin seed').update(seed).digest();
const k = BigInt('0x' + I.subarray(0, 32).toString('hex'));
const pub = mul(k, G);
const comp = Buffer.concat([Buffer.from([pub[1] & 1n ? 3 : 2]), Buffer.from(pub[0].toString(16).padStart(64, '0'), 'hex')]);
const h160 = crypto.createHash('ripemd160').update(crypto.createHash('sha256').update(comp).digest()).digest();
console.log(`passphrase ${JSON.stringify(pass)} (${pass.length} chars): fingerprint ${h160.subarray(0, 4).toString('hex')}`);
