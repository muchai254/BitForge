// Reads a viewer --log file and reports how well the camera reads an animated
// UR QR (e.g. Sparrow's "UR:CRYPTO-PSBT/<seq>-<count>/<bytewords>"):
// frames read, distinct sequence numbers, read rate, and how long until as
// many distinct frames as the message has fragments arrived (the minimum a
// fountain decoder needs; it usually needs a few more).
// Usage: node ur_stats.js scans.txt
'use strict';
const lines = require('fs').readFileSync(process.argv[2], 'utf8').split('\n').filter(Boolean);
const frames = [];
let other = 0;
for (const line of lines) {
  const sp = line.indexOf(' ');
  const ms = Number(line.slice(0, sp));
  const m = /^ur:([a-z0-9-]+)\/(\d+)-(\d+)\/([a-z]+)$/i.exec(line.slice(sp + 1));
  if (!m) { other++; continue; }
  frames.push({ ms, type: m[1].toLowerCase(), seq: Number(m[2]), count: Number(m[3]), body: m[4].length });
}
if (!frames.length) { console.log(`no UR frames (${other} other scans)`); process.exit(0); }

// Group by message (type + count); a new PSBT shows up as a new group.
const groups = new Map();
for (const f of frames) {
  const key = `${f.type} ${f.count}`;
  if (!groups.has(key)) groups.set(key, []);
  groups.get(key).push(f);
}
for (const [key, fs] of groups) {
  const [type, count] = key.split(' ');
  const seqs = new Set();
  let enoughAt = null;
  for (const f of fs) {
    seqs.add(f.seq);
    if (enoughAt === null && seqs.size >= Number(count)) enoughAt = f.ms;
  }
  const span = (fs[fs.length - 1].ms - fs[0].ms) / 1000;
  const sorted = [...seqs].sort((a, b) => a - b);
  console.log(`ur:${type}, ${count} fragments per message, frame body ${Math.min(...fs.map((f) => f.body))}-${Math.max(...fs.map((f) => f.body))} bytewords chars`);
  console.log(`  frames read: ${fs.length}, distinct sequence numbers: ${seqs.size} (${sorted[0]}..${sorted[sorted.length - 1]})`);
  console.log(`  over ${span.toFixed(1)} s: ${(fs.length / Math.max(span, 0.001)).toFixed(2)} frames/s read, ${(seqs.size / Math.max(span, 0.001)).toFixed(2)} new frames/s`);
  console.log(enoughAt === null
    ? `  never got ${count} distinct frames`
    : `  ${count} distinct frames after ${((enoughAt - fs[0].ms) / 1000).toFixed(1)} s from the first one`);
}
if (other) console.log(`(${other} non-UR scans ignored)`);
