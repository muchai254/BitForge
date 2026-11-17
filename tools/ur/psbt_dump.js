// Decodes UR frames captured by tools/cam_viewer (viewer.js --log) into the
// PSBT and dumps its BIP174 structure: for every input, whether
// non_witness_utxo is present, whether its txid matches the prevout, and its
// amount vs. witness_utxo; outputs with their BIP32 derivations; the fee.
// Usage: node psbt_dump.js scans.txt [ur-type]      (default type crypto-psbt)
// Signet test data only; the PSBT isn't secret but reveals wallet history.
'use strict';
const fs = require('fs');
const crypto = require('crypto');
const ur = require('./ur');

const lines = fs.readFileSync(process.argv[2], 'utf8').split('\n').filter(Boolean);
const want = (process.argv[3] || 'crypto-psbt').toLowerCase();
const frames = lines.map((l) => l.slice(l.indexOf(' ') + 1)).filter((t) => t.toLowerCase().startsWith(`ur:${want}/`));

// Different messages (e.g. densities) show up as different seqLen/checksum:
// decode each separately.
const groups = new Map();
for (const f of frames) {
  let u;
  try { u = ur.parseUr(f); } catch (e) { continue; }
  const k = u.part ? `${u.part.seqLen}/${u.part.checksum}` : 'single';
  if (!groups.has(k)) groups.set(k, []);
  groups.get(k).push(f);
}

const sha256d = (b) => crypto.createHash('sha256').update(crypto.createHash('sha256').update(b).digest()).digest();
class Reader {
  constructor(b) { this.b = b; this.p = 0; }
  u8() { return this.b[this.p++]; }
  u32() { const v = this.b.readUInt32LE(this.p); this.p += 4; return v; }
  u64() { const v = this.b.readBigUInt64LE(this.p); this.p += 8; return v; }
  bytes(n) { if (this.p + n > this.b.length) throw new Error('truncated'); const v = this.b.subarray(this.p, this.p + n); this.p += n; return v; }
  varint() { const f = this.u8(); if (f < 0xfd) return f; if (f === 0xfd) { const v = this.b.readUInt16LE(this.p); this.p += 2; return v; } if (f === 0xfe) return this.u32(); return Number(this.u64()); }
}
// Parses a transaction (with or without segwit marker). Returns the parts and
// its txid (hash of the serialization without witness data).
function parseTx(buf) {
  const r = new Reader(buf);
  const version = r.u32();
  let segwit = false;
  if (buf[r.p] === 0 && buf[r.p + 1] !== 0) { segwit = true; r.p += 2; }
  const ins = [], outs = [];
  const nIn = r.varint();
  for (let i = 0; i < nIn; i++) {
    const txid = Buffer.from(r.bytes(32)).reverse().toString('hex');
    const vout = r.u32();
    const script = r.bytes(r.varint());
    const seq = r.u32();
    ins.push({ txid, vout, scriptSigLen: script.length, seq });
  }
  const nOut = r.varint();
  for (let i = 0; i < nOut; i++) outs.push({ value: r.u64(), script: Buffer.from(r.bytes(r.varint())) });
  if (segwit) for (let i = 0; i < nIn; i++) { const n = r.varint(); for (let k = 0; k < n; k++) r.bytes(r.varint()); }
  const locktime = r.u32();
  // Rebuild the non-witness serialization for the txid.
  const parts = [Buffer.alloc(4)];
  parts[0].writeUInt32LE(version);
  const vi = (n) => (n < 0xfd ? Buffer.from([n]) : (() => { const b = Buffer.alloc(3); b[0] = 0xfd; b.writeUInt16LE(n, 1); return b; })());
  parts.push(vi(nIn));
  const rr = new Reader(buf); rr.p = 4 + (segwit ? 2 : 0); rr.varint();
  for (let i = 0; i < nIn; i++) { const start = rr.p; rr.bytes(36); rr.bytes(rr.varint()); rr.u32(); parts.push(buf.subarray(start, rr.p)); }
  const oStart = rr.p; rr.varint(); for (let i = 0; i < nOut; i++) { rr.u64(); rr.bytes(rr.varint()); } parts.push(buf.subarray(oStart, rr.p));
  const lt = Buffer.alloc(4); lt.writeUInt32LE(locktime); parts.push(lt);
  return { version, segwit, ins, outs, locktime, txid: Buffer.from(sha256d(Buffer.concat(parts))).reverse().toString('hex'), size: buf.length };
}
// PSBT key-value maps.
function readMap(r) {
  const m = [];
  for (;;) {
    const klen = r.varint();
    if (klen === 0) return m;
    const key = Buffer.from(r.bytes(klen));
    const val = Buffer.from(r.bytes(r.varint()));
    m.push({ type: key[0], keydata: key.subarray(1), val });
  }
}
const scriptType = (s) => (s.length === 22 && s[0] === 0 && s[1] === 20 ? 'P2WPKH' : s.length === 34 && s[0] === 0 && s[1] === 32 ? 'P2WSH' : s[0] === 0x51 && s.length === 34 ? 'P2TR' : 'other');
const path = (v) => { const fp = v.subarray(0, 4).toString('hex'); const ix = []; for (let i = 4; i < v.length; i += 4) { const n = v.readUInt32LE(i); ix.push(n >= 0x80000000 ? `${n - 0x80000000}'` : `${n}`); } return `[${fp}] m/${ix.join('/')}`; };

for (const [k, fs_] of groups) {
  const d = new ur.URDecoder();
  let used = 0;
  for (const f of fs_) { if (d.isComplete()) break; d.receive(f); used++; }
  console.log(`== message ${k}: ${fs_.length} frames captured, ${d.isComplete() ? `complete after ${used}` : 'NOT complete'}`);
  if (!d.isComplete() || d.result instanceof Error) { if (d.result instanceof Error) console.log('  ' + d.result.message); continue; }
  const psbt = ur.unwrapBytes(d.result);
  console.log(`  PSBT ${psbt.length} bytes, magic ${psbt.subarray(0, 5).toString('hex')} ${psbt.subarray(0, 5).equals(Buffer.from('70736274ff', 'hex')) ? 'OK' : 'BAD'}`);
  const r = new Reader(psbt); r.p = 5;
  const global = readMap(r);
  const utx = global.find((e) => e.type === 0x00);
  const tx = parseTx(utx.val);
  console.log(`  global keys: ${global.map((e) => '0x' + e.type.toString(16).padStart(2, '0')).join(' ')}; unsigned tx ${tx.ins.length} in / ${tx.outs.length} out, locktime ${tx.locktime}, version ${tx.version}`);
  for (const e of global.filter((e) => e.type === 0x01)) console.log(`    xpub ${path(e.val)}`);
  let inSum = 0n;
  tx.ins.forEach((inp, i) => {
    const m = readMap(r);
    const types = m.map((e) => '0x' + e.type.toString(16).padStart(2, '0'));
    console.log(`  input ${i}: prevout ${inp.txid}:${inp.vout}, sequence 0x${inp.seq.toString(16)}${inp.seq < 0xfffffffe ? ' (RBF)' : ''}; keys ${types.join(' ')}`);
    const nw = m.find((e) => e.type === 0x00), w = m.find((e) => e.type === 0x01), sh = m.find((e) => e.type === 0x03);
    if (nw) {
      const prev = parseTx(nw.val);
      const o = prev.outs[inp.vout];
      console.log(`    non_witness_utxo: PRESENT (${prev.size} bytes, ${prev.segwit ? 'with' : 'without'} witness data); txid ${prev.txid} ${prev.txid === inp.txid ? '== prevout ✓' : '!= prevout ✗'}; vout ${inp.vout}: ${o.value} sat, ${scriptType(o.script)}`);
      inSum += o.value;
      if (w) {
        const wv = w.val.readBigUInt64LE(0), ws = w.val.subarray(9);
        console.log(`    witness_utxo: ${wv} sat, ${scriptType(ws)}; ${wv === o.value && ws.equals(o.script) ? 'matches non_witness_utxo ✓' : 'DIFFERS from non_witness_utxo ✗'}`);
      }
    } else {
      console.log('    non_witness_utxo: MISSING ✗');
      if (w) { const wv = w.val.readBigUInt64LE(0); console.log(`    witness_utxo: ${wv} sat`); inSum += wv; }
    }
    if (sh) console.log(`    sighash type: ${sh.val.readUInt32LE(0)}`);
    for (const e of m.filter((e) => e.type === 0x06)) console.log(`    bip32 ${path(e.val)}`);
  });
  let outSum = 0n;
  tx.outs.forEach((o, i) => {
    const m = readMap(r);
    outSum += o.value;
    const der = m.filter((e) => e.type === 0x02).map((e) => path(e.val));
    console.log(`  output ${i}: ${o.value} sat, ${scriptType(o.script)} ${o.script.toString('hex')}${der.length ? `; bip32 ${der.join(', ')} (claims to be ours: verify!)` : ''}`);
  });
  console.log(`  inputs ${inSum} sat, outputs ${outSum} sat, fee ${inSum - outSum} sat; ${r.p === psbt.length ? 'parsed all bytes' : `${psbt.length - r.p} bytes left over`}`);
}
