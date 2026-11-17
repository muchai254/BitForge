// Feeds a simulated CAM console stream into the real viewer.js parser.
// Usage (from tools/cam_viewer): node tests/viewer_parser_test.js viewer.js
'use strict';
const Module = require('module');
const path = require('path');

let onData = null;
let stateRef = null;
const origLoad = Module._load;
Module._load = function (req, parent, isMain) {
  if (req === 'serialport') {
    return {
      SerialPort: class {
        constructor() { this.isOpen = false; }
        on(ev, fn) { if (ev === 'data') onData = fn; }
        open() {}
        set() {}
        write() {}
      },
    };
  }
  if (req === 'http') {
    return { createServer: (h) => { stateRef = h; return { listen() {} }; } };
  }
  return origLoad.apply(this, arguments);
};
const realSetInterval = global.setInterval;
global.setInterval = () => 0;
const realLog = console.log;
const scans = [];
console.log = (s) => { if (String(s).startsWith('SCANNED')) scans.push(s); };

const target = path.resolve(process.argv[2]);
process.argv = process.argv.slice(0, 2);
require(target);

// Read /state through the stubbed HTTP handler.
function getState() {
  let body = '';
  stateRef({ url: '/state?since=0', method: 'GET' }, { writeHead() {}, end(b) { body = b; } });
  return JSON.parse(body);
}

// Deterministic PRNG.
let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);

function frame() {
  const data = Buffer.alloc(9600);
  for (let i = 0; i < data.length; i++) data[i] = Math.floor(rnd() * 256); // includes many 0x0a bytes
  return Buffer.concat([Buffer.from('@FRM4 160 120 9600 1\n'), data, Buffer.from('\n')]);
}

// 200 frames, with stats and QR echo lines in between.
const parts = [];
let expectedScans = 0;
for (let i = 0; i < 200; i++) {
  parts.push(frame());
  if (i % 10 === 5) parts.push(Buffer.from('[stats] 12.5 fps, decoded 1, sent 1\n'));
  if (i % 20 === 7) { parts.push(Buffer.from('QR 5 bytes crc 3610a686: hello\n')); expectedScans++; }
}
let stream = Buffer.concat(parts);

// Start in the middle of a frame (viewer started while the CAM was streaming).
stream = stream.subarray(5000);
expectedScans -= 0; // the first QR line is after frame 7, so it isn't cut off

// Drop single bytes at 10 random positions.
const drops = [];
for (let i = 0; i < 10; i++) drops.push(Math.floor(rnd() * stream.length));
drops.sort((a, b) => b - a);
let buf = Buffer.from(stream);
for (const d of drops) buf = Buffer.concat([buf.subarray(0, d), buf.subarray(d + 1)]);

// Deliver in random-sized chunks, like a serial port.
for (let off = 0; off < buf.length;) {
  const n = 1 + Math.floor(rnd() * 4096);
  onData(buf.subarray(off, off + n));
  off += n;
}

const s = getState();
console.log = realLog;
global.setInterval = realSetInterval;
console.log(`frames sent 200 (first one cut), bytes dropped at 10 places`);
console.log(`link: ok ${s.link.framesOk}, torn ${s.link.framesTorn}, junk lines ${s.link.junkLines}`);
console.log(`scans seen ${scans.length} of ${expectedScans}; last stats: "${s.stats}"`);
const okFrames = s.link.framesOk;
const pass = okFrames >= 200 - 1 - 10 - 1 && scans.length >= expectedScans - 1 && s.stats.startsWith('[stats]');
console.log(pass ? 'PASS' : 'FAIL');
process.exit(pass ? 0 : 1);
