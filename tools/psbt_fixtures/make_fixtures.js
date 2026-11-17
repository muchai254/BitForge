// Builds the PSBT fixtures and writes them, with the verdict
// each must get, to airgap_wallet/psbt_selftest_vectors.h for the on-device
// self-test (Dev tests -> PSBT self-test).
//
// The keys, scripts and fingerprint come from a real Sparrow PSBT of the
// signet test wallet (fingerprint 28bab9a6; public data only, no secrets).
// Most fixtures spend synthetic previous transactions built here, so txids
// are real hashes but the data stays small; one uses the real ones (prevtx/,
// see fetch_prevtx.js) as a memory test. Addresses are encoded here,
// independently of the firmware's library.
// Usage: node make_fixtures.js
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const ur = require('../ur/ur');

const sha256 = (b) => crypto.createHash('sha256').update(b).digest();
const sha256d = (b) => sha256(sha256(b));
const hex = (b) => Buffer.from(b).toString('hex');
const H = 0x80000000;

// Encoding
function varint(n) {
  if (n < 0xfd) return Buffer.from([n]);
  if (n <= 0xffff) { const b = Buffer.alloc(3); b[0] = 0xfd; b.writeUInt16LE(n, 1); return b; }
  const b = Buffer.alloc(5); b[0] = 0xfe; b.writeUInt32LE(n, 1); return b;
}
const u32 = (n) => { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; };
const u64 = (n) => { const b = Buffer.alloc(8); b.writeBigUInt64LE(BigInt(n)); return b; };
const withLen = (b) => Buffer.concat([varint(b.length), b]);
const kv = (type, keydata, val) => Buffer.concat([withLen(Buffer.concat([Buffer.from([type]), keydata || Buffer.alloc(0)])), withLen(val)]);
const SEP = Buffer.from([0]);
const txidLE = (displayHex) => Buffer.from(displayHex, 'hex').reverse();

function unsignedTx({ version = 2, inputs, outputs, locktime = 328945 }) {
  return Buffer.concat([
    u32(version), varint(inputs.length),
    ...inputs.map((i) => Buffer.concat([txidLE(i.txid), u32(i.vout), varint(0), u32(i.seq === undefined ? 0xfffffffd : i.seq)])),
    varint(outputs.length),
    ...outputs.map((o) => Buffer.concat([u64(o.amount), withLen(o.script)])),
    u32(locktime),
  ]);
}
function derValue(fp, p) { return Buffer.concat([fp, ...p.map((x) => u32(x))]); }

// spec: { inputs: [{txid, vout, seq, nw, w: {amount, script}, sighash, der: [{pub, fp, path}], extra: [Buffer kv]}],
//         outputs: [{amount, script, der: [...]}], version, locktime, globals: [Buffer kv] }
function build(spec) {
  const parts = [Buffer.from('psbt\xff', 'latin1'), kv(0x00, null, spec.rawTx || unsignedTx(spec)), ...(spec.globals || []), SEP];
  for (const i of spec.inputs) {
    if (i.nw) parts.push(kv(0x00, null, i.nw));
    if (i.w) parts.push(kv(0x01, null, Buffer.concat([u64(i.w.amount), withLen(i.w.script)])));
    if (i.sighash !== undefined) parts.push(kv(0x03, null, u32(i.sighash)));
    for (const d of i.der || []) parts.push(kv(0x06, d.pub, derValue(d.fp, d.path)));
    parts.push(...(i.extra || []), SEP);
  }
  for (const o of spec.outputs) {
    for (const d of o.der || []) parts.push(kv(0x02, d.pub, derValue(d.fp, d.path)));
    parts.push(...(o.extra || []), SEP);
  }
  return Buffer.concat(parts);
}

// Addresses, independent of the firmware.
const BECH32 = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';
function bech32Polymod(values) {
  const G = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3];
  let chk = 1;
  for (const v of values) {
    const top = chk >>> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (let i = 0; i < 5; i++) if ((top >>> i) & 1) chk ^= G[i];
  }
  return chk >>> 0;
}
function segwitAddress(prog) {
  const hrp = 'tb', data = [0];
  let acc = 0, bits = 0;
  for (const b of prog) { acc = (acc << 8) | b; bits += 8; while (bits >= 5) { bits -= 5; data.push((acc >>> bits) & 31); } acc &= (1 << bits) - 1; }
  if (bits) data.push((acc << (5 - bits)) & 31);
  const hrpExp = [...hrp].map((c) => c.charCodeAt(0) >> 5).concat([0], [...hrp].map((c) => c.charCodeAt(0) & 31));
  const pm = bech32Polymod(hrpExp.concat(data, [0, 0, 0, 0, 0, 0])) ^ 1;
  const chk = [];
  for (let i = 0; i < 6; i++) chk.push((pm >>> (5 * (5 - i))) & 31);
  return hrp + '1' + data.concat(chk).map((d) => BECH32[d]).join('');
}
function base58check(payload) {
  const full = Buffer.concat([payload, sha256d(payload).subarray(0, 4)]);
  const A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
  let n = BigInt('0x' + full.toString('hex')), s = '';
  while (n > 0n) { s = A[Number(n % 58n)] + s; n /= 58n; }
  for (const b of full) { if (b !== 0) break; s = '1' + s; }
  return s;
}
function addressOf(script) {
  if (script[0] === 0 && (script.length === 22 || script.length === 34)) return segwitAddress(script.subarray(2));
  if (script.length === 25) return base58check(Buffer.concat([Buffer.from([0x6f]), script.subarray(3, 23)]));
  if (script.length === 23) return base58check(Buffer.concat([Buffer.from([0xc4]), script.subarray(2, 22)]));
  throw new Error('no address');
}

// The real PSBT: keys and scripts.
function realPsbt() {
  const lines = fs.readFileSync(path.join(__dirname, '..', 'ur', 'fixtures', 'sparrow_psbt_2026-11-01.txt'), 'utf8').split('\n').filter(Boolean);
  const d = new ur.URDecoder();
  for (const l of lines) { if (d.isComplete()) break; d.receive(l.slice(l.indexOf(' ') + 1)); }
  return ur.unwrapBytes(d.result);
}
function readMaps(buf) {
  let p = 5;
  const vi = () => { const f = buf[p++]; if (f < 0xfd) return f; if (f === 0xfd) { const v = buf.readUInt16LE(p); p += 2; return v; } const v = buf.readUInt32LE(p); p += 4; return v; };
  const map = () => { const m = []; for (;;) { const kl = vi(); if (!kl) return m; const k = buf.subarray(p, p + kl); p += kl; const vl = vi(); const v = buf.subarray(p, p + vl); p += vl; m.push({ type: k[0], kd: k.subarray(1), val: v }); } };
  const global = map(), inputs = [map(), map()], outputs = [map(), map()];
  if (p !== buf.length) throw new Error('unexpected shape of the real PSBT');
  return { global, inputs, outputs };
}
const REAL = realPsbt();
const M = readMaps(REAL);
const derOf = (m) => { const e = m.find((x) => x.type === (m === M.outputs[0] || m === M.outputs[1] ? 0x02 : 0x06)); return { pub: Buffer.from(e.kd), fp: Buffer.from(e.val.subarray(0, 4)) }; };
const inDer = derOf(M.inputs[0]);            // m/84'/1'/0'/0/0
const changeDer = derOf(M.outputs[0]);       // m/84'/1'/0'/1/0
const recvDer = derOf(M.outputs[1]);         // m/84'/1'/0'/0/1
const FP = inDer.fp;
const OURS = Buffer.from(M.inputs[0].find((e) => e.type === 1).val.subarray(9)); // P2WPKH of /0/0
// Output scripts from the unsigned tx: its outputs follow the inputs.
const utx = M.global.find((e) => e.type === 0).val;
const CHANGE_SCRIPT = Buffer.concat([Buffer.from([0x00, 0x14]), utx.subarray(utx.length - 4 - 31 - 31 + 8 + 1 + 2, utx.length - 4 - 31 - 31 + 8 + 1 + 22)]);
const RECV_SCRIPT = Buffer.from(utx.subarray(utx.length - 4 - 22, utx.length - 4));
if (hex(CHANGE_SCRIPT) !== '0014c1bd73055945fa01af600797fe028445dba390ad' || hex(RECV_SCRIPT) !== '00145d05668b1170421007c344857cdb2d2f82866085') throw new Error('script offsets');
const pathIn = [84 + H, 1 + H, 0 + H, 0, 0], pathChange = [84 + H, 1 + H, 0 + H, 1, 0], pathRecv = [84 + H, 1 + H, 0 + H, 0, 1];
const inD = { pub: inDer.pub, fp: FP, path: pathIn };
const chD = { pub: changeDer.pub, fp: FP, path: pathChange };
const rcD = { pub: recvDer.pub, fp: FP, path: pathRecv };
// tpub of the account, from the PSBT's global xpub (forced to tpub version bytes).
const xpubEntry = M.global.find((e) => e.type === 1);
const TPUB = base58check(Buffer.concat([Buffer.from('043587cf', 'hex'), xpubEntry.kd.subarray(4)]));
const OTHER_P2WPKH = Buffer.concat([Buffer.from([0x00, 0x14]), Buffer.alloc(20, 0xab)]);
const OTHER_FP = Buffer.from('deadbeef', 'hex');
const OTHER_PUB = Buffer.concat([Buffer.from([0x02]), Buffer.alloc(32, 0x11)]);

// A synthetic previous transaction paying `amount` to `script` at output `vout`.
function prevTx(amount, script, vout = 0, salt = 1) {
  const outs = [];
  for (let i = 0; i <= vout; i++) outs.push(i === vout ? { amount, script } : { amount: 1000 + i, script: OTHER_P2WPKH });
  const raw = Buffer.concat([
    u32(2), varint(1), Buffer.alloc(32, salt), u32(0), varint(0), u32(0xffffffff),
    varint(outs.length), ...outs.map((o) => Buffer.concat([u64(o.amount), withLen(o.script)])), u32(0),
  ]);
  return { raw, txid: Buffer.from(sha256d(raw)).reverse().toString('hex'), vout };
}
const P1 = prevTx(329429, OURS, 0, 1), P2 = prevTx(1000, OURS, 1, 2);

// Honest templates.
const IN1 = () => ({ txid: P1.txid, vout: P1.vout, sighash: 1, der: [inD] });
const IN2 = () => ({ txid: P2.txid, vout: P2.vout, sighash: 1, der: [inD] });
const OUTS = () => [{ amount: 266124, script: CHANGE_SCRIPT, der: [chD] }, { amount: 50000, script: RECV_SCRIPT, der: [rcD] }];
const two = (m1 = {}, m2 = {}, outs = OUTS()) => ({
  inputs: [{ ...IN1(), nw: P1.raw, w: { amount: 329429, script: OURS }, ...m1 }, { ...IN2(), nw: P2.raw, w: { amount: 1000, script: OURS }, ...m2 }],
  outputs: outs,
});
const one = (m = {}, outs = OUTS()) => ({ inputs: [{ ...IN1(), w: { amount: 329429, script: OURS }, ...m }], outputs: outs });

// Output expectations.
const CHANGE = { kind: 'OUT_CHANGE', index: 0, script: CHANGE_SCRIPT, amount: 266124 };
const SELF = { kind: 'OUT_SELF', index: 1, script: RECV_SCRIPT, amount: 50000 };
const recipient = (script, amount, claim = false) => ({ kind: 'OUT_RECIPIENT', index: 0, script, amount, claim });

// The fixtures
const F = [];
const ok = (name, bytes, want) => F.push({ name, bytes, ok: true, ...want });
const refuse = (name, bytes, reason) => F.push({ name, bytes, ok: false, reason });
const HIGH_1PCT = 'over 1% of the amount sent';

// Real PSBT, exactly as Sparrow made it: 2 inputs, no previous txs, so it is refused.
refuse('real Sparrow PSBT, 2 inputs, no previous txs', REAL, 'has no previous transaction');
// Real PSBT with the real previous transactions added (22 KB: a memory test).
{
  const rd = (t) => Buffer.from(fs.readFileSync(path.join(__dirname, 'prevtx', `${t}.hex`), 'utf8').trim(), 'hex');
  const t0 = '4aab56134e0316665c38440fe73aab38f6eedb7c54db88951bae6d284f8c1d08', t1 = '3ed6cea830aa235ba6549f1f18909bd008cb9374079f842e414066b211766f4a';
  ok('real PSBT + real previous txs (22 KB)', build({
    inputs: [
      { txid: t0, vout: 1, nw: rd(t0), w: { amount: 1000, script: OURS }, sighash: 1, der: [inD] },
      { txid: t1, vout: 550, nw: rd(t1), w: { amount: 329429, script: OURS }, sighash: 1, der: [inD] },
    ],
    outputs: OUTS(),
  }), { fee: 14305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, SELF] });
}

// Amount rule ("option 2"): with 2+ inputs every input needs its full previous
// tx (witness_utxo amounts alone can lie); a single input may use witness_utxo
// only, shown as unverified.
ok('honest: 2 inputs with previous txs', build(two()), { fee: 14305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, SELF] });
ok('honest: 2 inputs, previous txs only (no witness_utxo)', build(two({ w: undefined }, { w: undefined })), { fee: 14305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, SELF] });
ok('option 2: 1 input, witness_utxo only: accepted, unverified', build(one()), { fee: 13305, sent: 50000, change: 266124, verified: false, high: HIGH_1PCT, outs: [CHANGE, SELF] });
ok('1 input with previous tx: verified', build(one({ nw: P1.raw })), { fee: 13305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, SELF] });
refuse('option 2: 2 inputs, one lacks the previous tx', build(two({}, { nw: undefined })), 'has no previous transaction');
refuse('option 2: 2 inputs, witness_utxo only', build(two({ nw: undefined }, { nw: undefined })), 'has no previous transaction');
refuse('1 input, no amount at all', build(one({ w: undefined })), 'no amount information');
refuse('previous tx does not match the prevout', build(two({ nw: P2.raw })), "doesn't match the coin");
refuse('previous tx has no such output', build(two({ vout: 5 })), 'has no output'); // prevout changed, txid still matches
refuse('previous tx output is not ours', (() => { const p = prevTx(329429, OTHER_P2WPKH, 0, 3); return build(two({ txid: p.txid, nw: p.raw, w: undefined })); })(), "isn't paid to this wallet");
refuse('FEE LIE: witness_utxo lower than the previous tx', build(two({ w: { amount: 319429, script: OURS } })), 'disagree');
refuse('FEE LIE: witness_utxo higher than the previous tx', build(two({}, { w: { amount: 5000, script: OURS } })), 'disagree');
refuse('witness_utxo script differs from the previous tx', build(two({ w: { amount: 329429, script: OTHER_P2WPKH } })), 'disagree');
{
  // Same lie with the keys in reverse order: uBitcoin lets the last one win; we must still refuse.
  const lying = kv(0x01, null, Buffer.concat([u64(319429), withLen(OURS)]));
  const spec = two({ w: undefined, nw: undefined, extra: [lying, kv(0x00, null, P1.raw)] });
  refuse('FEE LIE: witness_utxo before non_witness_utxo in the map', build(spec), 'disagree');
}
ok('single-input amount lie is accepted, flagged (signature would be invalid)', build(one({ w: { amount: 320000, script: OURS } })), { fee: 3876, sent: 50000, change: 266124, verified: false, high: HIGH_1PCT, outs: [CHANGE, SELF] });
refuse('outputs exceed inputs', build(one({ w: { amount: 100000, script: OURS } })), 'exceed inputs');
refuse('impossible input amount', build(one({ w: { amount: '2100000000000001', script: OURS } })), 'impossible amount');
refuse('impossible output amount', build(one({}, [{ amount: '2100000000000001', script: RECV_SCRIPT }])), 'impossible amount');

// Inputs must be ours.
refuse('foreign input: other fingerprint', build(two({ der: [{ ...inD, fp: OTHER_FP }] })), 'other fingerprint');
refuse('input key does not match our derivation', build(two({ der: [{ ...inD, pub: changeDer.pub }] })), "doesn't match this wallet");
refuse('input path outside the account', build(two({ der: [{ ...inD, path: [84 + H, 1 + H, 1 + H, 0, 0] }] })), "doesn't match this wallet");
refuse('input path on mainnet coin type', build(two({ der: [{ ...inD, path: [84 + H, 0 + H, 0 + H, 0, 0] }] })), "doesn't match this wallet");
refuse('input index too high', build(two({ der: [{ ...inD, path: [84 + H, 1 + H, 0 + H, 0, 10001] }] })), "doesn't match this wallet");
refuse('input without a derivation', build(two({ der: [] })), 'has 0 keys');
refuse('input with two keys', build(two({ der: [inD, { ...rcD }] })), 'has 2 keys');
refuse('witness_utxo script not ours (single input)', build(one({ w: { amount: 329429, script: OTHER_P2WPKH } })), "isn't paid to this wallet");

// Change must be proved, otherwise it's a recipient (with a warning if it claims to be ours).
const sentAll = 316124;
ok('CHANGE ATTACK: script replaced, derivation kept', build(two({}, {}, [{ amount: 266124, script: OTHER_P2WPKH, der: [chD] }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(OTHER_P2WPKH, 266124, true), SELF] });
ok('change: pubkey is not ours', build(two({}, {}, [{ amount: 266124, script: CHANGE_SCRIPT, der: [{ ...chD, pub: OTHER_PUB }] }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(CHANGE_SCRIPT, 266124, true), SELF] });
ok('change: wrong path for this key', build(two({}, {}, [{ amount: 266124, script: CHANGE_SCRIPT, der: [{ ...chD, path: [84 + H, 1 + H, 0 + H, 1, 7] }] }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(CHANGE_SCRIPT, 266124, true), SELF] });
ok('change: index too high', build(two({}, {}, [{ amount: 266124, script: CHANGE_SCRIPT, der: [{ ...chD, path: [84 + H, 1 + H, 0 + H, 1, 10001] }] }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(CHANGE_SCRIPT, 266124, true), SELF] });
ok('change: other fingerprint is a plain recipient', build(two({}, {}, [{ amount: 266124, script: CHANGE_SCRIPT, der: [{ ...chD, fp: OTHER_FP }] }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(CHANGE_SCRIPT, 266124, false), SELF] });
ok('change: no derivation is a plain recipient', build(two({}, {}, [{ amount: 266124, script: CHANGE_SCRIPT }, OUTS()[1]])),
  { fee: 14305, sent: sentAll, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(CHANGE_SCRIPT, 266124, false), SELF] });

// Other input rules.
refuse('sighash NONE', build(two({ sighash: 2 })), 'Only SIGHASH_ALL');
refuse('sighash SINGLE on the second input', build(two({}, { sighash: 3 })), 'Only SIGHASH_ALL');
refuse('sighash ALL|ANYONECANPAY', build(two({ sighash: 0x81 })), 'Only SIGHASH_ALL');
ok('sighash absent is fine', build(two({ sighash: undefined }, { sighash: undefined })), { fee: 14305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, SELF] });
refuse('input already finalized', build(two({ extra: [kv(0x08, null, Buffer.from([0x00]))] })), 'already signed');
refuse('input with a partial signature', build(two({ extra: [kv(0x02, inD.pub, Buffer.alloc(71, 1))] })), 'already signed');
refuse('input with a witness script', build(two({ extra: [kv(0x05, null, Buffer.from([0x51]))] })), "isn't a plain P2WPKH");
refuse('input with a redeem script', build(two({ extra: [kv(0x04, null, Buffer.from([0x51]))] })), "isn't a plain P2WPKH");

// Output types.
const P2TR = Buffer.concat([Buffer.from([0x51, 0x20]), Buffer.alloc(32, 7)]);
const OPRET = Buffer.from([0x6a, 0x04, 1, 2, 3, 4]);
const P2WSH = Buffer.concat([Buffer.from([0x00, 0x20]), Buffer.alloc(32, 9)]);
const P2PKH = Buffer.concat([Buffer.from([0x76, 0xa9, 0x14]), Buffer.alloc(20, 3), Buffer.from([0x88, 0xac])]);
const P2SH = Buffer.concat([Buffer.from([0xa9, 0x14]), Buffer.alloc(20, 4), Buffer.from([0x87])]);
refuse('taproot output', build(two({}, {}, [OUTS()[0], { amount: 50000, script: P2TR }])), "can't show");
refuse('OP_RETURN output', build(two({}, {}, [OUTS()[0], { amount: 50000, script: OPRET }])), "can't show");
refuse('empty output script', build(two({}, {}, [OUTS()[0], { amount: 50000, script: Buffer.alloc(0) }])), "can't show");
for (const [name, s] of [['P2WSH', P2WSH], ['P2PKH', P2PKH], ['P2SH', P2SH], ['P2WPKH', OTHER_P2WPKH]]) {
  ok(`${name} recipient, address encoded correctly`, build(two({}, {}, [OUTS()[0], { amount: 50000, script: s }])),
    { fee: 14305, sent: 50000, change: 266124, verified: true, high: HIGH_1PCT, outs: [CHANGE, recipient(s, 50000)] });
}

// Fee warnings. Inputs total 330,429.
const withFee = (fee, rcv = 50000) => build(two({}, {}, [{ amount: 330429 - rcv - fee, script: CHANGE_SCRIPT, der: [chD] }, { amount: rcv, script: OTHER_P2WPKH }]));
const outsFee = (fee, rcv = 50000) => [{ ...CHANGE, amount: 330429 - rcv - fee }, recipient(OTHER_P2WPKH, rcv)];
ok('normal fee: no warning', withFee(200), { fee: 200, sent: 50000, change: 280229, verified: true, high: '', outs: outsFee(200) });
ok('fee exactly 1% of sent: no warning', withFee(500), { fee: 500, sent: 50000, change: 279929, verified: true, high: '', outs: outsFee(500) });
ok('fee just over 1% of sent: warning', withFee(501), { fee: 501, sent: 50000, change: 279928, verified: true, high: HIGH_1PCT, outs: outsFee(501) });
ok('fee over 50,000 sats: warning', withFee(50001, 200000), { fee: 50001, sent: 200000, change: 80428, verified: true, high: 'over 50,000 sats', outs: outsFee(50001, 200000) });
ok('zero fee is allowed', withFee(0), { fee: 0, sent: 50000, change: 280429, verified: true, high: '', outs: outsFee(0) });

// Malformed data.
const G = build(two());
refuse('bad magic', Buffer.concat([Buffer.from('psbx\xff', 'latin1'), G.subarray(5)]), 'bad header');
refuse('empty', Buffer.alloc(0), 'bad header');
refuse('magic only', G.subarray(0, 5), 'cut short');
refuse('truncated in the middle', G.subarray(0, G.length >> 1), 'cut short');
refuse('truncated by one byte', G.subarray(0, G.length - 1), 'cut short');
refuse('trailing byte', Buffer.concat([G, Buffer.from([0])]), 'extra data');
refuse('no unsigned tx', Buffer.concat([Buffer.from('psbt\xff', 'latin1'), SEP]), 'no transaction');
refuse('duplicate unsigned tx', build({ ...two(), globals: [kv(0x00, null, unsignedTx(two()))] }), 'appears twice');
refuse('duplicate witness_utxo', build(two({ extra: [kv(0x01, null, Buffer.concat([u64(329429), withLen(OURS)]))] })), 'appears twice');
refuse('duplicate derivation', build(two({ der: [inD, inD] })), 'appears twice');
refuse('PSBT version 2', build({ ...two(), globals: [kv(0xfb, null, u32(2))] }), 'unsupported PSBT version');
refuse('huge length field', Buffer.concat([Buffer.from('psbt\xff', 'latin1'), Buffer.from([0x01, 0x00, 0xfe, 0xff, 0xff, 0xff, 0x7f])]), 'cut short');
refuse('8-byte length field', Buffer.concat([Buffer.from('psbt\xff', 'latin1'), Buffer.from([0xff, 255, 255, 255, 255, 255, 255, 255, 255])]), 'cut short');
refuse('malformed witness_utxo', build(two({ w: undefined, extra: [kv(0x01, null, Buffer.alloc(3))] })), 'malformed PSBT field');
refuse('malformed derivation value', build(two({ der: [], extra: [kv(0x06, inD.pub, Buffer.alloc(7))] })), 'malformed PSBT field');
refuse('derivation key not a compressed pubkey', build(two({ der: [], extra: [kv(0x06, Buffer.alloc(33, 4), derValue(FP, pathIn))] })), 'malformed PSBT field');
refuse('path too long', build(two({ der: [{ ...inD, path: new Array(11).fill(0) }] })), 'malformed PSBT field');
{
  const spec = two();
  const raw = unsignedTx(spec);
  // A scriptSig in the unsigned tx: the varint after the first prevout.
  const withSig = Buffer.concat([raw.subarray(0, 41), Buffer.from([1, 0x51]), raw.subarray(42)]);
  refuse('unsigned tx has a scriptSig', build({ ...spec, rawTx: withSig }), 'transaction inside');
  const segwit = Buffer.concat([raw.subarray(0, 4), Buffer.from([0, 1]), raw.subarray(4)]);
  refuse('unsigned tx in segwit format', build({ ...spec, rawTx: segwit }), 'transaction inside');
  refuse('unsigned tx with trailing bytes', build({ ...spec, rawTx: Buffer.concat([raw, Buffer.from([0])]) }), 'transaction inside');
  refuse('unsigned tx cut short', build({ ...spec, rawTx: raw.subarray(0, raw.length - 2) }), 'transaction inside');
  refuse('transaction version 3', build({ ...spec, version: 3 }), 'Unsupported transaction version');
}
refuse('previous tx is garbage', build(two({ nw: Buffer.alloc(40, 0xee) })), 'previous transaction in the PSBT');
refuse('previous tx cut short', build(two({ nw: P1.raw.subarray(0, P1.raw.length - 1) })), 'previous transaction in the PSBT');

// Size limits.
const manyIn = (n) => ({ inputs: Array.from({ length: n }, (_, i) => { const p = prevTx(1000, OURS, 0, i + 10); return { txid: p.txid, vout: 0, nw: p.raw, sighash: 1, der: [inD] }; }), outputs: [{ amount: 500, script: OTHER_P2WPKH }] });
const manyOut = (n) => one({}, Array.from({ length: n }, () => ({ amount: 1000, script: OTHER_P2WPKH })));
refuse('51 inputs', build(manyIn(51)), 'too many');
refuse('21 outputs', build(manyOut(21)), 'too many');
{
  const inputs = 50 * 1000, out = 500, fee = inputs - out;
  ok('50 inputs (the limit)', build(manyIn(50)), { fee, sent: out, change: 0, verified: true, high: HIGH_1PCT, outs: [recipient(OTHER_P2WPKH, out)] });
  const twenty = 20 * 1000, feeOut = 329429 - twenty;
  ok('20 outputs (the limit)', build(manyOut(20)), { fee: feeOut, sent: twenty, change: 0, verified: false, high: 'over 50,000 sats', outs: new Array(20).fill(recipient(OTHER_P2WPKH, 1000)) });
}

// Output
const lines = [];
lines.push('// Generated by tools/psbt_fixtures/make_fixtures.js. Do not edit. Public test data only.');
lines.push('#pragma once');
lines.push('#include <stdint.h>');
lines.push(`static const char PT_TPUB[] = "${TPUB}";`);
lines.push(`static const uint8_t PT_FP[4] = {${[...FP].map((b) => '0x' + b.toString(16)).join(', ')}};`);
lines.push('struct PtOutput { OutputKind kind; uint32_t index; bool claim; uint64_t amount; const char *address; };');
lines.push('struct PtCase { const char *name; const uint8_t *data; size_t len; bool ok; const char *reason; uint64_t fee, sent, change; bool verified; const char *high; uint32_t outCount; const PtOutput *outs; };');
F.forEach((c, i) => {
  // Long byte arrays are stored as hex rows to keep the header readable.
  const rows = [];
  for (let k = 0; k < c.bytes.length; k += 24) rows.push('  ' + [...c.bytes.subarray(k, k + 24)].map((b) => '0x' + b.toString(16)).join(',') + ',');
  lines.push(`static const uint8_t PT_D${i}[] = {\n${rows.join('\n') || '  0'}\n};`);
  if (c.ok) lines.push(`static const PtOutput PT_O${i}[] = {\n${c.outs.map((o) => `  {${o.kind}, ${o.index}, ${o.claim ? 'true' : 'false'}, ${o.amount}ull, "${addressOf(o.script)}"}`).join(',\n')},\n};`);
});
lines.push('static const PtCase PT_CASES[] = {');
F.forEach((c, i) => {
  const len = c.bytes.length;
  if (c.ok) lines.push(`  {${JSON.stringify(c.name)}, PT_D${i}, ${len}, true, "", ${c.fee}ull, ${c.sent}ull, ${c.change}ull, ${c.verified}, ${JSON.stringify(c.high)}, ${c.outs.length}, PT_O${i}},`);
  else lines.push(`  {${JSON.stringify(c.name)}, PT_D${i}, ${len}, false, ${JSON.stringify(c.reason)}, 0, 0, 0, false, "", 0, nullptr},`);
});
lines.push('};');
const dest = path.join(__dirname, '..', '..', 'airgap_wallet', 'psbt_selftest_vectors.h');
fs.writeFileSync(dest, lines.join('\n') + '\n');
const total = F.reduce((a, c) => a + c.bytes.length, 0);
console.log(`${F.length} fixtures (${F.filter((c) => c.ok).length} accepted, ${F.filter((c) => !c.ok).length} refused), ${total} PSBT bytes; ${dest}, ${fs.statSync(dest).size} bytes`);
console.log('tpub', TPUB);
