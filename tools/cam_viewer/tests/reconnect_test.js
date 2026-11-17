// Checks viewer.js reconnects: the port is missing at start (open fails
// twice), then opens, then closes (FTDI unplugged), then opens again.
// Usage (from tools/cam_viewer): node tests/reconnect_test.js viewer.js
'use strict';
const Module = require('module');
const path = require('path');

let port = null, opens = 0, writes = [];
const origLoad = Module._load;
Module._load = function (req) {
  if (req === 'serialport') {
    return {
      SerialPort: class {
        constructor() { this.isOpen = false; this.h = {}; port = this; }
        on(ev, fn) { this.h[ev] = fn; }
        open(cb) {
          opens++;
          if (opens <= 2) return setImmediate(() => cb(new Error('File not found')));
          this.isOpen = true;
          setImmediate(() => cb(null));
        }
        close() { this.isOpen = false; setImmediate(() => this.h.close()); }
        set() {}
        write(c) { writes.push(c); }
      },
    };
  }
  if (req === 'http') return { createServer: () => ({ listen() {} }) };
  return origLoad.apply(this, arguments);
};
global.setInterval = () => 0;
const out = [];
console.log = console.error = (...a) => out.push(a.join(' '));

const target = path.resolve(process.argv[2] || 'viewer.js');
process.argv = process.argv.slice(0, 2); // viewer.js parses its own options
require(target);

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  await wait(2600); // two failed opens, 1 s apart, then success
  const first = opens === 3 && port.isOpen;
  port.close(); // unplugged
  await wait(1300);
  const again = opens === 4 && port.isOpen;
  const ok = first && again && out.some((l) => l.startsWith('reconnected')) && writes.filter((w) => w === 'p').length === 2 &&
    out.filter((l) => l.startsWith('cannot open')).length === 1;
  process.stdout.write(out.join('\n') + `\nopens ${opens}\n${ok ? 'PASS' : 'FAIL'}\n`);
  process.exit(ok ? 0 : 1);
})();
