// Checks the ESP32-CAM -> wallet link (CAM GPIO14) against docs/protocol.md §1.
// Wire the USB-TTL adapter's RX to CAM GPIO14 (temporarily), then:
//
//   node link_check.js [--port COM3] [--baud 115200]
//
// Every frame is validated with the wallet's receiver rules and
// printed as OK or REJECT <reason>. A summary is printed every 10 s and on
// Ctrl+C. Development tool only.
'use strict';
const { SerialPort } = require('serialport');

const args = { port: 'COM3', baud: 115200 };
for (let i = 2; i < process.argv.length; i += 2) {
  const k = process.argv[i].replace(/^--/, ''), v = process.argv[i + 1];
  if (!(k in args)) { console.error(`unknown option --${k}`); process.exit(1); }
  args[k] = k === 'port' ? v : parseInt(v, 10);
}

const MAX_PAYLOAD = 2953;
const MAX_LINE = 13 + 4 * Math.ceil(MAX_PAYLOAD / 3) + 1; // protocol.md: line buffer cap

// CRC-32/ISO-HDLC, same as airgap_qrcam/link_frame.cpp.
function crc32(buf) {
  let crc = 0xFFFFFFFF;
  for (const b of buf) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
if (crc32(Buffer.from('123456789')).toString(16) !== 'cbf43926') throw new Error('crc32 self-test failed');

// Validates one line that starts with '#' (without its '\n').
// Returns { ok: true, type, payload, crc } or { ok: false, reason }.
function checkFrame(line) {
  const m = /^#([A-Z]{2}),([^,]*),([^,]*),/.exec(line.toString('latin1'));
  if (!m) return { ok: false, reason: 'bad header' };
  const [head, type, lenStr, crcStr] = m;
  if (type !== 'QR' && type !== 'QB') return { ok: false, reason: `unknown type ${type}` };
  if (!/^[1-9][0-9]{0,3}$/.test(lenStr)) return { ok: false, reason: `bad len "${lenStr}"` };
  const len = Number(lenStr);
  if (len > MAX_PAYLOAD) return { ok: false, reason: `len ${len} > ${MAX_PAYLOAD}` };
  if (!/^[0-9a-f]{8}$/.test(crcStr)) return { ok: false, reason: `bad crc field "${crcStr}"` };
  const body = line.subarray(head.length);
  let payload;
  if (type === 'QR') {
    if (body.length !== len) return { ok: false, reason: `payload is ${body.length} bytes, len says ${len}` };
    for (const b of body) if (b < 0x20 || b > 0x7e) return { ok: false, reason: 'non-printable byte in #QR payload' };
    payload = body;
  } else {
    const b64 = body.toString('latin1');
    if (b64.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(b64)) return { ok: false, reason: 'invalid base64' };
    payload = Buffer.from(b64, 'base64');
    if (payload.length !== len) return { ok: false, reason: `base64 decodes to ${payload.length} bytes, len says ${len}` };
  }
  const crc = crc32(payload).toString(16).padStart(8, '0');
  if (crc !== crcStr) return { ok: false, reason: `crc mismatch (computed ${crc}, frame says ${crcStr})` };
  return { ok: true, type, payload, crc };
}

const counts = { ok: 0, rejected: 0, oversize: 0, skippedBytes: 0 };
const reasons = {};
let pending = Buffer.alloc(0);

function reject(reason) {
  counts.rejected++;
  reasons[reason.replace(/[0-9a-f]{8}|\d+/g, 'N')] = (reasons[reason.replace(/[0-9a-f]{8}|\d+/g, 'N')] || 0) + 1;
  console.log(`REJECT: ${reason}`);
}

// Splits the stream into lines starting at '#'. Bytes before a '#' are skipped
// (resync). A line longer than MAX_LINE is dropped.
function onData(chunk) {
  pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
  for (;;) {
    const start = pending.indexOf(0x23); // '#'
    if (start < 0) { counts.skippedBytes += pending.length; pending = Buffer.alloc(0); return; }
    if (start > 0) { counts.skippedBytes += start; pending = pending.subarray(start); }
    const nl = pending.indexOf(0x0a);
    if (nl < 0) {
      if (pending.length > MAX_LINE) {
        counts.oversize++;
        reject(`line longer than ${MAX_LINE} bytes without "\\n"`);
        pending = pending.subarray(1); // resync at the next '#'
        continue;
      }
      return;
    }
    const line = pending.subarray(0, nl);
    const r = checkFrame(line);
    if (r.ok) {
      pending = pending.subarray(nl + 1);
      counts.ok++;
      const text = r.payload.toString('latin1').replace(/[^\x20-\x7e]/g, '.');
      console.log(`OK #${counts.ok} ${r.type} ${r.payload.length} bytes crc ${r.crc}: ${text.slice(0, 100)}`);
    } else {
      // A truncated frame runs into the next one, so resync at the next '#'
      // (which may be inside this line), not at the next '\n'. A '#' that
      // doesn't start a valid header is just a payload byte: skip it quietly.
      if (r.reason === 'bad header') counts.skippedBytes++;
      else reject(r.reason);
      pending = pending.subarray(1);
    }
  }
}

function summary() {
  console.log(`[summary] ok ${counts.ok}, rejected ${counts.rejected}, oversize ${counts.oversize}, ` +
              `bytes skipped (not part of a frame) ${counts.skippedBytes}` +
              (Object.keys(reasons).length ? `, reasons ${JSON.stringify(reasons)}` : ''));
}

const port = new SerialPort({ path: args.port, baudRate: args.baud, autoOpen: false, hupcl: false });
port.on('data', onData);
port.on('error', (e) => console.error('serial error:', e.message));
port.on('close', () => { console.error('serial port closed'); summary(); process.exit(0); });
port.open((err) => {
  if (err) { console.error(`cannot open ${args.port}: ${err.message}`); process.exit(1); }
  port.set({ dtr: false, rts: false }, () => {});
  console.log(`link check on ${args.port} at ${args.baud} baud; show QR codes to the camera`);
});
setInterval(summary, 10000);
process.on('SIGINT', () => { summary(); process.exit(0); });

module.exports = { checkFrame, crc32 };
