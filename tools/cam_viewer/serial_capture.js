// Serial capture: one timestamped line per received line, appended to a file.
// Usage (from tools/cam_viewer): node serial_capture.js COM6 115200 <outfile>
// Use this instead of a PowerShell System.IO.Ports.SerialPort loop, which can
// stall for good on long lines.
// DTR/RTS are left low so opening the port doesn't reset the board.
'use strict';
const fs = require('fs');
const { SerialPort } = require('serialport');
const [path, baud, out] = [process.argv[2], Number(process.argv[3]), process.argv[4]];
const port = new SerialPort({ path, baudRate: baud, autoOpen: false, hupcl: false });
let pending = '';
const stamp = () => new Date().toTimeString().slice(0, 8);
port.on('data', (d) => {
  pending += d.toString('latin1');
  let i;
  while ((i = pending.indexOf('\n')) >= 0) {
    const line = pending.slice(0, i).replace(/\r$/, '');
    pending = pending.slice(i + 1);
    fs.appendFileSync(out, `${stamp()} ${line}\n`);
  }
  if (pending.length > 8192) { fs.appendFileSync(out, `${stamp()} ${pending}\n`); pending = ''; }
});
port.on('error', (e) => fs.appendFileSync(out, `${stamp()} [capture] serial error: ${e.message}\n`));
// Keeps running when the board is unplugged: retries every second until the
// port is back (the same as viewer.js). Stop it to free the port for uploads.
let retry = null, waiting = false;
const reopen = () => { if (!retry) retry = setTimeout(() => { retry = null; open(); }, 1000); };
port.on('close', () => { fs.appendFileSync(out, `${stamp()} [capture] port closed; reconnecting\n`); pending = ''; reopen(); });
function open() {
  port.open((err) => {
    if (err) {
      if (!waiting) fs.appendFileSync(out, `${stamp()} [capture] cannot open ${path}: ${err.message}; retrying\n`);
      waiting = true;
      reopen();
      return;
    }
    waiting = false;
    port.set({ dtr: false, rts: false }, () => {});
    fs.appendFileSync(out, `${stamp()} [capture] opened ${path} at ${baud}\n`);
  });
}
open();
