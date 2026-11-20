// Uniform Resources (BCR-2020-005) + fountain codes (BCR-2020-006): a
// reference decoder for BitForge, ported from Blockchain Commons' bc-ur
// C++ library (BSD-2-Clause-Plus-Patent, see LICENSE-bc-ur.txt). It is the
// yardstick for the wallet's C++ port: same algorithms, same test
// vectors (test.js). The encoder serves the upstream tests and psbt_show.
'use strict';
const crypto = require('crypto');

// CRC-32 (ISO-HDLC)
function crc32(buf) {
  let crc = 0xffffffff;
  for (const b of buf) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}
const u32be = (n) => Buffer.from([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);

// Bytewords (bytewords.cpp)
const BYTEWORDS =
  'ableacidalsoapexaquaarchatomauntawayaxisbackbaldbarnbeltbetabiasbluebodybragbrewbulbbuzzcalmcashcatschefcityclaw' +
  'codecolacookcostcruxcurlcuspcyandarkdatadaysdelidicedietdoordowndrawdropdrumdulldutyeacheasyechoedgeepicevenexam' +
  'exiteyesfactfairfernfigsfilmfishfizzflapflewfluxfoxyfreefrogfuelfundgalagamegeargemsgiftgirlglowgoodgraygrimguru' +
  'gushgyrohalfhanghardhawkheathelphighhillholyhopehornhutsicedideaidleinchinkyintoirisironitemjadejazzjoinjoltjowl' +
  'judojugsjumpjunkjurykeepkenokeptkeyskickkilnkingkitekiwiknoblamblavalazyleaflegsliarlimplionlistlogoloudloveluau' +
  'lucklungmainmanymathmazememomenumeowmildmintmissmonknailnavyneednewsnextnoonnotenumbobeyoboeomitonyxopenovalowls' +
  'paidpartpeckplaypluspoempoolposepuffpumapurrquadquizraceramprealredorichroadrockroofrubyruinrunsrustsafesagascar' +
  'setssilkskewslotsoapsolosongstubsurfswantacotasktaxitenttiedtimetinytoiltombtoystriptunatwinuglyundouniturgeuser' +
  'vastveryvetovialvibeviewvisavoidvowswallwandwarmwaspwavewaxywebswhatwhenwhizwolfworkyankyawnyellyogayurtzapszero' +
  'zestzinczonezoom';
const MINIMAL = new Map(); // "first+last letter" -> byte
for (let i = 0; i < 256; i++) MINIMAL.set(BYTEWORDS[i * 4] + BYTEWORDS[i * 4 + 3], i);
const WORD = new Map();
for (let i = 0; i < 256; i++) WORD.set(BYTEWORDS.slice(i * 4, i * 4 + 4), i);

function bytewordsEncode(buf, style) {
  const withCrc = Buffer.concat([buf, u32be(crc32(buf))]);
  const words = [...withCrc].map((b) => BYTEWORDS.slice(b * 4, b * 4 + 4));
  if (style === 'minimal') return words.map((w) => w[0] + w[3]).join('');
  return words.join(style === 'uri' ? '-' : ' ');
}

// Decodes and checks the trailing CRC-32. Throws on any error.
function bytewordsDecode(str, style) {
  const s = str.toLowerCase();
  let bytes;
  if (style === 'minimal') {
    if (s.length % 2) throw new Error('bytewords: odd length');
    bytes = [];
    for (let i = 0; i < s.length; i += 2) {
      const v = MINIMAL.get(s.slice(i, i + 2));
      if (v === undefined) throw new Error('bytewords: invalid word');
      bytes.push(v);
    }
  } else {
    bytes = s.split(style === 'uri' ? '-' : ' ').map((w) => {
      const v = WORD.get(w);
      if (v === undefined) throw new Error('bytewords: invalid word');
      return v;
    });
  }
  if (bytes.length < 5) throw new Error('bytewords: too short');
  const body = Buffer.from(bytes.slice(0, -4));
  if (!Buffer.from(bytes.slice(-4)).equals(u32be(crc32(body)))) throw new Error('bytewords: bad checksum');
  return body;
}

// Xoshiro256** (xoshiro256.cpp)
const M64 = (1n << 64n) - 1n;
const rotl = (x, k) => ((x << BigInt(k)) | (x >> BigInt(64 - k))) & M64;
class Xoshiro256 {
  // seed: a Buffer (hashed with SHA-256), a string (its bytes, hashed), or a
  // uint32 (its 4 big-endian bytes, hashed), as in the C++ constructors.
  constructor(seed) {
    let bytes = seed;
    if (typeof seed === 'string') bytes = Buffer.from(seed, 'utf8');
    else if (typeof seed === 'number') bytes = u32be(seed);
    const d = crypto.createHash('sha256').update(bytes).digest();
    this.s = [0, 1, 2, 3].map((i) => d.readBigUInt64BE(i * 8));
  }
  next() {
    const s = this.s;
    const result = (rotl((s[1] * 5n) & M64, 7) * 9n) & M64;
    const t = (s[1] << 17n) & M64;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }
  // C++: next() / ((double)UINT64_MAX + 1). Number(BigInt) rounds to
  // nearest like the C++ uint64 -> double conversion.
  nextDouble() { return Number(this.next()) / 18446744073709551616; }
  nextInt(low, high) { return Math.floor(this.nextDouble() * (high - low + 1)) + low; }
  nextByte() { return this.nextInt(0, 255); }
  nextData(n) { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = this.nextByte(); return b; }
}

// Random sampler (random-sampler.cpp, Vose's alias method)
class RandomSampler {
  constructor(probs) {
    const sum = probs.reduce((a, b) => a + b, 0);
    const n = probs.length;
    const P = probs.map((p) => (p * n) / sum);
    const S = [], L = [];
    for (let i = n - 1; i >= 0; i--) (P[i] < 1 ? S : L).push(i);
    const pr = new Array(n).fill(0), al = new Array(n).fill(0);
    while (S.length && L.length) {
      const a = S.pop(), g = L.pop();
      pr[a] = P[a];
      al[a] = g;
      P[g] += P[a] - 1;
      (P[g] < 1 ? S : L).push(g);
    }
    while (L.length) pr[L.pop()] = 1;
    while (S.length) pr[S.pop()] = 1;
    this.probs = pr;
    this.aliases = al;
  }
  next(rng) {
    const r1 = rng(), r2 = rng();
    const i = Math.floor(this.probs.length * r1);
    return r2 < this.probs[i] ? i : this.aliases[i];
  }
}

// Fragment choice (fountain-utils.cpp/.hpp)
function shuffled(items, rng) {
  const remaining = items.slice(), result = [];
  while (remaining.length) {
    const index = rng.nextInt(0, remaining.length - 1);
    result.push(remaining.splice(index, 1)[0]);
  }
  return result;
}
function chooseDegree(seqLen, rng) {
  const probs = [];
  for (let i = 1; i <= seqLen; i++) probs.push(1 / i);
  return new RandomSampler(probs).next(() => rng.nextDouble()) + 1;
}
// Returns the sorted fragment indexes mixed into part seqNum.
function chooseFragments(seqNum, seqLen, checksum) {
  if (seqNum <= seqLen) return [seqNum - 1];
  const rng = new Xoshiro256(Buffer.concat([u32be(seqNum), u32be(checksum)]));
  const degree = chooseDegree(seqLen, rng);
  const idx = [];
  for (let i = 0; i < seqLen; i++) idx.push(i);
  return shuffled(idx, rng).slice(0, degree).sort((a, b) => a - b);
}

// Minimal CBOR: unsigned ints, byte strings, arrays (all a part needs)
function cborHead(major, n) {
  if (n < 24) return Buffer.from([(major << 5) | n]);
  if (n < 0x100) return Buffer.from([(major << 5) | 24, n]);
  if (n < 0x10000) return Buffer.from([(major << 5) | 25, n >> 8, n & 255]);
  return Buffer.concat([Buffer.from([(major << 5) | 26]), u32be(n)]);
}
const cborUint = (n) => cborHead(0, n);
const cborBytes = (b) => Buffer.concat([cborHead(2, b.length), b]);
// Reads one head at buf[pos]; returns { major, value, pos } (value for ints/lengths).
function cborRead(buf, pos) {
  if (pos >= buf.length) throw new Error('cbor: truncated');
  const ib = buf[pos++], major = ib >> 5, info = ib & 31;
  let value;
  if (info < 24) value = info;
  else if (info === 24) { value = buf[pos]; pos += 1; }
  else if (info === 25) { value = buf.readUInt16BE(pos); pos += 2; }
  else if (info === 26) { value = buf.readUInt32BE(pos); pos += 4; }
  else throw new Error('cbor: unsupported length'); // 8-byte ints aren't needed here
  if (pos > buf.length) throw new Error('cbor: truncated');
  return { major, value, pos };
}
function cborReadBytes(buf, pos) {
  const h = cborRead(buf, pos);
  if (h.major !== 2) throw new Error('cbor: expected a byte string');
  if (h.pos + h.value > buf.length) throw new Error('cbor: truncated');
  return { bytes: buf.subarray(h.pos, h.pos + h.value), pos: h.pos + h.value };
}

// A fountain part: CBOR [seqNum, seqLen, messageLen, checksum, data].
function encodePart(p) {
  return Buffer.concat([cborHead(4, 5), cborUint(p.seqNum), cborUint(p.seqLen), cborUint(p.messageLen),
                        cborUint(p.checksum), cborBytes(p.data)]);
}
function decodePart(cbor) {
  let h = cborRead(cbor, 0);
  if (h.major !== 4 || h.value !== 5) throw new Error('part: not a 5-element array');
  const f = [];
  let pos = h.pos;
  for (let i = 0; i < 4; i++) {
    h = cborRead(cbor, pos);
    if (h.major !== 0) throw new Error('part: expected an unsigned int');
    f.push(h.value);
    pos = h.pos;
  }
  const d = cborReadBytes(cbor, pos);
  if (d.pos !== cbor.length) throw new Error('part: trailing bytes');
  return { seqNum: f[0], seqLen: f[1], messageLen: f[2], checksum: f[3], data: Buffer.from(d.bytes) };
}

// Fountain encoder (fountain-encoder.cpp)
function findNominalFragmentLength(messageLen, minLen, maxLen) {
  const maxCount = Math.floor(messageLen / minLen);
  let len;
  for (let count = 1; count <= maxCount; count++) {
    len = Math.ceil(messageLen / count);
    if (len <= maxLen) break;
  }
  return len;
}
function partitionMessage(message, fragLen) {
  const out = [];
  for (let i = 0; i < message.length; i += fragLen) {
    const f = Buffer.alloc(fragLen);
    message.copy(f, 0, i, Math.min(i + fragLen, message.length));
    out.push(f);
  }
  return out;
}
class FountainEncoder {
  constructor(message, maxFragmentLen, firstSeqNum = 0, minFragmentLen = 10) {
    this.messageLen = message.length;
    this.checksum = crc32(message);
    this.fragmentLen = findNominalFragmentLength(message.length, minFragmentLen, maxFragmentLen);
    this.fragments = partitionMessage(message, this.fragmentLen);
    this.seqNum = firstSeqNum;
  }
  nextPart() {
    this.seqNum = (this.seqNum + 1) >>> 0;
    const idx = chooseFragments(this.seqNum, this.fragments.length, this.checksum);
    const data = Buffer.alloc(this.fragmentLen);
    for (const i of idx) for (let k = 0; k < data.length; k++) data[k] ^= this.fragments[i][k];
    return { seqNum: this.seqNum, seqLen: this.fragments.length, messageLen: this.messageLen, checksum: this.checksum, data };
  }
}

// Fountain decoder (fountain-decoder.cpp)
const keyOf = (idx) => idx.join(',');
const isStrictSubset = (a, b) => a.length < b.length && a.every((x) => b.includes(x));
const xorBuf = (a, b) => { const r = Buffer.from(a); for (let i = 0; i < r.length; i++) r[i] ^= b[i]; return r; };

class FountainDecoder {
  constructor() {
    this.expected = null; // { seqLen, messageLen, checksum, fragmentLen }
    this.received = new Set(); // simple fragment indexes we have
    this.simple = new Map();   // key -> part
    this.mixed = new Map();    // key -> part
    this.queue = [];
    this.result = null;        // Buffer, or Error
    this.processed = 0;
  }
  isComplete() { return this.result !== null; }
  // Fraction 0..1 (the C++ estimate: processed / (1.75 * seqLen), capped at 0.99).
  estimatedPercentComplete() {
    if (this.isComplete()) return 1;
    if (!this.expected) return 0;
    return Math.min(0.99, this.processed / (this.expected.seqLen * 1.75));
  }
  receivePart(p) {
    if (this.isComplete()) return false;
    if (!this.validate(p)) return false;
    this.queue.push({ idx: chooseFragments(p.seqNum, p.seqLen, p.checksum), data: p.data });
    while (!this.isComplete() && this.queue.length) this.processQueueItem();
    this.processed++;
    return true;
  }
  validate(p) {
    if (!this.expected) {
      if (p.seqLen < 1 || p.data.length < 1) return false;
      this.expected = { seqLen: p.seqLen, messageLen: p.messageLen, checksum: p.checksum, fragmentLen: p.data.length };
      return true;
    }
    const e = this.expected;
    return e.seqLen === p.seqLen && e.messageLen === p.messageLen && e.checksum === p.checksum && e.fragmentLen === p.data.length;
  }
  processQueueItem() {
    const part = this.queue.shift();
    if (part.idx.length === 1) this.processSimple(part);
    else this.processMixed(part);
  }
  reduceBy(a, b) {
    if (isStrictSubset(b.idx, a.idx)) return { idx: a.idx.filter((x) => !b.idx.includes(x)), data: xorBuf(a.data, b.data) };
    return a;
  }
  reduceMixedBy(p) {
    const reduced = [...this.mixed.values()].map((m) => this.reduceBy(m, p));
    this.mixed = new Map();
    for (const r of reduced) {
      if (r.idx.length === 1) this.queue.push(r);
      else this.mixed.set(keyOf(r.idx), r);
    }
  }
  processSimple(p) {
    const index = p.idx[0];
    if (this.received.has(index)) return;
    this.simple.set(keyOf(p.idx), p);
    this.received.add(index);
    if (this.received.size === this.expected.seqLen) {
      const parts = [...this.simple.values()].sort((a, b) => a.idx[0] - b.idx[0]);
      const message = Buffer.concat(parts.map((x) => x.data)).subarray(0, this.expected.messageLen);
      this.result = crc32(message) === this.expected.checksum ? Buffer.from(message) : new Error('fountain: invalid checksum');
    } else {
      this.reduceMixedBy(p);
    }
  }
  processMixed(p) {
    if (this.mixed.has(keyOf(p.idx))) return;
    let p2 = p;
    for (const s of this.simple.values()) p2 = this.reduceBy(p2, s);
    for (const m of this.mixed.values()) p2 = this.reduceBy(p2, m);
    if (p2.idx.length === 1) {
      this.queue.push(p2);
    } else {
      this.reduceMixedBy(p2);
      this.mixed.set(keyOf(p2.idx), p2);
    }
  }
}

// UR strings (ur-decoder.cpp / ur-encoder.cpp)
// "ur:<type>/<body>" (single part) or "ur:<type>/<seq>-<len>/<body>". Case-insensitive.
function parseUr(str) {
  const s = str.toLowerCase();
  if (!s.startsWith('ur:')) throw new Error('ur: missing "ur:" scheme');
  const comps = s.slice(3).split('/');
  if (comps.length < 2 || comps.length > 3) throw new Error('ur: wrong number of path components');
  const type = comps[0];
  if (!/^[a-z0-9-]+$/.test(type)) throw new Error('ur: invalid type');
  if (comps.length === 2) return { type, single: bytewordsDecode(comps[1], 'minimal') };
  const m = /^(\d+)-(\d+)$/.exec(comps[1]);
  if (!m) throw new Error('ur: invalid sequence component');
  const part = decodePart(bytewordsDecode(comps[2], 'minimal'));
  if (part.seqNum !== Number(m[1]) || part.seqLen !== Number(m[2])) throw new Error('ur: sequence component does not match the part');
  return { type, part };
}
function encodeUrPart(type, p) { return `ur:${type}/${p.seqNum}-${p.seqLen}/${bytewordsEncode(encodePart(p), 'minimal')}`; }
function encodeUrSingle(type, cbor) { return `ur:${type}/${bytewordsEncode(cbor, 'minimal')}`; }

class URDecoder {
  constructor() { this.type = null; this.fountain = new FountainDecoder(); this.result = null; }
  isComplete() { return this.result !== null; }
  // Returns true if the string was accepted (a new, consistent part).
  receive(str) {
    if (this.isComplete()) return false;
    let u;
    try { u = parseUr(str); } catch (e) { return false; }
    if (this.type && u.type !== this.type) return false;
    this.type = u.type;
    if (u.single) { this.result = u.single; return true; }
    const ok = this.fountain.receivePart(u.part);
    if (this.fountain.isComplete()) this.result = this.fountain.result;
    return ok;
  }
}

// The CBOR payload of ur:bytes / ur:crypto-psbt / ur:psbt is one byte string.
function unwrapBytes(cbor) {
  const d = cborReadBytes(cbor, 0);
  if (d.pos !== cbor.length) throw new Error('cbor: trailing bytes after the byte string');
  return Buffer.from(d.bytes);
}

module.exports = {
  crc32, bytewordsEncode, bytewordsDecode, Xoshiro256, RandomSampler, shuffled, chooseDegree, chooseFragments,
  findNominalFragmentLength, partitionMessage, FountainEncoder, FountainDecoder, encodePart, decodePart,
  parseUr, encodeUrPart, encodeUrSingle, URDecoder, cborBytes, unwrapBytes,
};
