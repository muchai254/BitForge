// Runs ur.js against bc-ur's own test vectors (vectors.json, extracted from
// upstream test/test.cpp), then decoder checks for what the device must do.
// Usage: node test.js
'use strict';
const ur = require('./ur');
const V = require('./vectors.json');

let failed = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${ok || !detail ? '' : '  ' + detail}`);
  if (!ok) failed++;
}
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const hex = (b) => Buffer.from(b).toString('hex');
const makeMessage = (len, seed = 'Wolf') => new ur.Xoshiro256(seed).nextData(len);
function throws(fn) { try { fn(); return false; } catch (e) { return true; } }

// Upstream vectors.
for (const [s, h] of V.crc32) check(`crc32 "${s}"`, ur.crc32(Buffer.from(s)).toString(16).padStart(8, '0') === h);

{
  const b = V.bytewords1, input = Buffer.from(b.input);
  check('bytewords 1 encode standard/minimal', ur.bytewordsEncode(input, 'standard') === b.standard && ur.bytewordsEncode(input, 'minimal') === b.minimal);
  check('bytewords 1 decode standard/minimal', hex(ur.bytewordsDecode(b.standard, 'standard')) === hex(input) && hex(ur.bytewordsDecode(b.minimal, 'minimal')) === hex(input));
  check('bytewords 1 bad checksum refused', throws(() => ur.bytewordsDecode(b.badMinimal, 'minimal')));
  check('bytewords too short refused', throws(() => ur.bytewordsDecode('wolf', 'standard')) && throws(() => ur.bytewordsDecode('', 'standard')));
}
{
  const b = V.bytewords2, input = Buffer.from(b.input);
  check('bytewords 2 encode', ur.bytewordsEncode(input, 'standard') === b.standard && ur.bytewordsEncode(input, 'minimal') === b.minimal);
  check('bytewords 2 decode', hex(ur.bytewordsDecode(b.standard, 'standard')) === hex(input) && hex(ur.bytewordsDecode(b.minimal, 'minimal')) === hex(input));
}
{
  const r = new ur.Xoshiro256('Wolf'), got = [];
  for (let i = 0; i < 100; i++) got.push(Number(r.next() % 100n));
  check('xoshiro256 seeded "Wolf" (next % 100)', eq(got, V.rng1));
}
{
  const r = new ur.Xoshiro256(ur.crc32(Buffer.from('Wolf'))), got = [];
  for (let i = 0; i < 100; i++) got.push(Number(r.next() % 100n));
  check('xoshiro256 seeded crc32("Wolf")', eq(got, V.rng2));
}
{
  const r = new ur.Xoshiro256('Wolf'), got = [];
  for (let i = 0; i < 100; i++) got.push(r.nextInt(1, 10));
  check('xoshiro256 next_int(1, 10)', eq(got, V.rng3));
}
check('find_nominal_fragment_length', ur.findNominalFragmentLength(12345, 1005, 1955) === 1764 && ur.findNominalFragmentLength(12345, 1005, 30000) === 12345);
{
  const s = new ur.RandomSampler([1, 2, 4, 8]), r = new ur.Xoshiro256('Wolf'), got = [];
  for (let i = 0; i < 500; i++) got.push(s.next(() => r.nextDouble()));
  check('random sampler (500 samples)', eq(got, V.sampler));
}
{
  const r = new ur.Xoshiro256('Wolf'), got = [];
  for (let i = 0; i < 10; i++) got.push(ur.shuffled([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], r));
  check('shuffle', eq(got, V.shuffle));
}
{
  const m = makeMessage(1024), len = ur.findNominalFragmentLength(m.length, 10, 100);
  const frags = ur.partitionMessage(m, len);
  check('partition (make_message(1024), 10..100)', eq(frags.map(hex), V.partition));
  const degrees = [];
  for (let n = 1; n <= 200; n++) degrees.push(ur.chooseDegree(frags.length, new ur.Xoshiro256(`Wolf-${n}`)));
  check('choose_degree (200)', eq(degrees, V.degrees));
  const sets = [];
  for (let s = 1; s <= 30; s++) sets.push(ur.chooseFragments(s, frags.length, ur.crc32(m)));
  check('choose_fragments (30)', eq(sets, V.fragments));
}
{
  const enc = new ur.FountainEncoder(makeMessage(256), 30), desc = [], cbor = [];
  const enc2 = new ur.FountainEncoder(makeMessage(256), 30);
  for (let i = 0; i < 20; i++) {
    const p = enc.nextPart();
    desc.push(`seqNum:${p.seqNum}, seqLen:${p.seqLen}, messageLen:${p.messageLen}, checksum:${p.checksum}, data:${hex(p.data)}`);
    cbor.push(hex(ur.encodePart(enc2.nextPart())));
  }
  check('fountain encoder parts (20)', eq(desc, V.encoderParts));
  check('fountain encoder part CBOR (20)', eq(cbor, V.encoderCbor));
}
{
  const cbor = ur.cborBytes(makeMessage(50));
  check('single-part UR encode', ur.encodeUrSingle('bytes', cbor) === V.singlePartUr);
  const d = new ur.URDecoder();
  d.receive(V.singlePartUr);
  check('single-part UR decode', d.isComplete() && hex(d.result) === hex(cbor));
}
{
  const cbor = ur.cborBytes(makeMessage(256)), enc = new ur.FountainEncoder(cbor, 30), got = [];
  for (let i = 0; i < 20; i++) got.push(ur.encodeUrPart('bytes', enc.nextPart()));
  check('multipart UR encode (20 strings)', eq(got, V.urParts));
}

// Decoder checks.
{
  // The 20 upstream multipart strings: in order, then only the mixed ones
  // (seq 10..20), then upper case (as Sparrow's QRs are).
  const want = hex(ur.cborBytes(makeMessage(256)));
  const run = (parts) => { const d = new ur.URDecoder(); for (const p of parts) if (d.receive(p) && d.isComplete()) break; return d; };
  let d = run(V.urParts);
  check('decode upstream multipart strings (1-9 pure, in order)', d.isComplete() && hex(d.result) === want);
  d = run(V.urParts.slice(9));
  check(`decode from mixed parts only (seq 10..20): ${d.isComplete() ? 'complete' : 'incomplete'}`, !d.isComplete() || hex(d.result) === want);
  d = run(V.urParts.map((s) => s.toUpperCase()));
  check('decode upper-case strings', d.isComplete() && hex(d.result) === want);
}
{
  // Large message, many fragments, start deep in the stream, drop 30% of
  // parts at random, feed duplicates: must still decode exactly.
  const msg = makeMessage(32767), cbor = ur.cborBytes(msg), enc = new ur.FountainEncoder(cbor, 1000, 100);
  const d = new ur.URDecoder(), drop = new ur.Xoshiro256('drop');
  let sent = 0, last = null;
  while (!d.isComplete() && sent < 10000) {
    const s = ur.encodeUrPart('bytes', enc.nextPart());
    sent++;
    if (drop.nextDouble() < 0.3) continue;
    d.receive(s);
    if (last) d.receive(last); // duplicate of the previous part
    last = s;
  }
  check(`32767-byte message, 30% loss + duplicates (${sent} parts sent, ${d.fountain.processed} accepted)`,
        d.isComplete() && hex(ur.unwrapBytes(d.result)) === hex(msg));
}
{
  // Corruption must be refused: one changed character (bytewords CRC), a
  // part from another message, a wrong type, a sequence component that lies.
  const p = V.urParts[0];
  const bad = p.slice(0, -10) + (p[p.length - 10] === 'a' ? 'e' : 'a') + p.slice(-9);
  check('one changed character refused', throws(() => ur.parseUr(bad)));
  const other = ur.encodeUrPart('bytes', new ur.FountainEncoder(ur.cborBytes(makeMessage(256, 'Other')), 30).nextPart());
  const d = new ur.URDecoder();
  d.receive(V.urParts[0]);
  check('part of a different message ignored', d.receive(other) === false);
  check('different UR type ignored', d.receive(V.urParts[1].replace('ur:bytes/', 'ur:crypto-psbt/')) === false);
  check('lying sequence component refused', throws(() => ur.parseUr(V.urParts[1].replace('/2-9/', '/3-9/'))));
}

console.log(failed ? `\n${failed} FAILED` : '\nall passed');
process.exit(failed ? 1 : 0);
