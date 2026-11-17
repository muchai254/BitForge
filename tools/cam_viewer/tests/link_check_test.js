// Offline test of link_check.js against valid frames, 8 malformed kinds and a
// truncated frame followed by a good one. Usage (from tools/cam_viewer):
//   node tests/link_check_test.js link_check.js
'use strict';
const Module = require('module');
let onData = null;
const orig = Module._load;
Module._load = function (req) {
  if (req === 'serialport') return { SerialPort: class { on(e, f) { if (e === 'data') onData = f; } open() {} set() {} } };
  return orig.apply(this, arguments);
};
global.setInterval = () => 0;
const out = [];
console.log = (s) => out.push(String(s));
const target = require('path').resolve(process.argv[2]);
process.argv = process.argv.slice(0, 2);
const { crc32 } = require(target);
// Same as writeLinkFrame() in link_frame.cpp.
function frame(p) {
  const printable = [...p].every((b) => b >= 0x20 && b <= 0x7e);
  const crc = crc32(p).toString(16).padStart(8, '0');
  return Buffer.from(`#${printable ? 'QR' : 'QB'},${p.length},${crc},${printable ? p.toString('latin1') : p.toString('base64')}\n`, 'latin1');
}
const vcard = Buffer.from('BEGIN:VCARD\nVERSION:3.0\nN:Sample;Alex\nEND:VCARD');
const big = Buffer.from('x'.repeat(2953));
const good = Buffer.concat([
  Buffer.from('boot noise \x00\xff'), frame(Buffer.from('hello')), frame(Buffer.from('a#b,c,d')), frame(vcard), frame(big),
]);
const bad = [
  '#QR,5,00000000,hello\n',                 // crc mismatch
  '#QR,05,3610a686,hello\n',                // leading zero
  '#QR,6,3610a686,hello\n',                 // len mismatch
  '#QR,2954,3610a686,x\n',                  // len too big
  '#QR,5,3610A686,hello\n',                 // uppercase crc
  '#QX,5,3610a686,hello\n',                 // unknown type
  '#QB,3,00000000,AAA\n',                   // invalid base64 length
  '#QR,5,3610a686,hel\x01o\n',              // non-printable in QR
];
const torn = Buffer.from('#QR,5,3610a686,he'); // truncated, then a good frame
const all = Buffer.concat([good, ...bad.map((s) => Buffer.from(s, 'latin1')), torn, frame(Buffer.from('after'))]);
for (let i = 0; i < all.length; i += 7) onData(all.subarray(i, i + 7));
process.stdout.write(out.join('\n') + '\n');
