// Development viewer for the airgap_qrcam ESP32-CAM.
// Reads the CAM's UART0 console (USB-TTL adapter), shows the live preview in a
// browser page on http://127.0.0.1:<http>/ and highlights every decoded QR code.
//
// Usage: node viewer.js [--port COM3] [--baud 2000000] [--http 8765] [--log scans.txt]
// --log appends "<ms since start> <full decoded text>" for every scan (the
// console shows only the first 80 characters). Never use it with real seeds:
// the CAM doesn't echo SeedQRs, but other secrets could be scanned.
//
// Development tool only.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');

const args = { port: 'COM3', baud: 2000000, http: 8765, log: '' };
for (let i = 2; i < process.argv.length; i += 2) {
  const k = process.argv[i].replace(/^--/, ''), v = process.argv[i + 1];
  if (!(k in args)) { console.error(`unknown option --${k}`); process.exit(1); }
  args[k] = k === 'port' || k === 'log' ? v : parseInt(v, 10);
}
const startMs = Date.now();

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

// State shared with the web page.
const state = {
  frame: null,        // { w, h, data, codes, seq }
  frameSeq: 0,
  stats: '',
  events: [],         // decoded QR codes
  log: [],            // other console lines
  connected: false,
};
function addLog(line) {
  state.log.push(line);
  if (state.log.length > 200) state.log.shift();
}

// Serial parsing: text lines, plus preview frames:
// "@FRM4 w h len codes\n" + len bytes (4-bit, two pixels per byte) + "\n"
//
// If bytes are lost, a line can start with leftover binary, so markers are
// searched for anywhere in the line, not only at its start; the reader then
// resyncs at the very next frame or text line. A frame whose bytes aren't
// followed by its "\n" lost data on the way: it's counted and not shown.
let pending = Buffer.alloc(0);
let need = 0, frameHdr = null;
const link = { framesOk: 0, framesTorn: 0, junkLines: 0 };

// True if the line is mostly printable text (not leftover frame data).
function isText(line) {
  let printable = 0;
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c >= 0x20 && c <= 0x7e) printable++;
  }
  return printable >= line.length * 0.95;
}

function handleLine(raw) {
  const at = raw.lastIndexOf('@FRM4 ');
  if (at >= 0) {
    const frm = /^@FRM4 (\d+) (\d+) (\d+) (\d+)$/.exec(raw.slice(at));
    if (frm) {
      const [w, h, len, codes] = frm.slice(1).map(Number);
      if (len === (w * h) / 2 && w % 2 === 0 && w * h <= 640 * 480) { frameHdr = { w, h, codes }; need = len; }
      if (at > 0) link.junkLines++;
      return;
    }
  }
  const qr = /QR (\d+) bytes crc ([0-9a-f]{8}): (.*)$/.exec(raw);
  const statsAt = raw.indexOf('[stats]');
  const line = qr ? raw.slice(qr.index) : statsAt >= 0 ? raw.slice(statsAt) : raw;
  if (qr) {
    const len = Number(qr[1]), crc = qr[2], text = qr[3];
    const bytes = Buffer.from(text, 'latin1');
    // The echo replaces non-printable bytes with '.', so a mismatch only means
    // something when the text has no '.' (then the echo is exact). A match is
    // always proof. Otherwise the CRC can't be checked here (crcOk = null).
    const match = bytes.length === len && crc32(bytes).toString(16).padStart(8, '0') === crc;
    const crcOk = match ? true : bytes.length === len && !text.includes('.') ? false : null;
    const ev = { id: state.events.length + 1, time: new Date().toLocaleTimeString(), len, crc, text, crcOk };
    state.events.push(ev);
    console.log(`SCANNED #${ev.id} (${len} bytes, crc ${crc}${crcOk === true ? ' OK' : crcOk === false ? ' MISMATCH' : ''}): ${text.slice(0, 80)}`);
    if (args.log) fs.appendFileSync(args.log, `${Date.now() - startMs} ${text}\n`);
    return;
  }
  if (line.startsWith('[stats]')) { state.stats = line; return; }
  if (!line.length) return;
  if (!isText(line)) { link.junkLines++; return; }
  addLog(line);
  console.log(line);
}

// 4-bit pixels (high nibble first) to 8-bit, 0..15 -> 0..255.
function unpack4(raw) {
  const out = Buffer.alloc(raw.length * 2);
  for (let i = 0; i < raw.length; i++) {
    out[2 * i] = (raw[i] >> 4) * 17;
    out[2 * i + 1] = (raw[i] & 15) * 17;
  }
  return out;
}

function onData(chunk) {
  pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
  for (;;) {
    if (need > 0) {
      if (pending.length < need + 1) return;
      if (pending[need] === 0x0a) {
        state.frame = { ...frameHdr, data: unpack4(pending.subarray(0, need)), seq: ++state.frameSeq };
        link.framesOk++;
        pending = pending.subarray(need + 1);
      } else {
        // Bytes were lost, so the frame really ended (with its "\n") somewhere
        // inside what we took as its data. Drop the frame and re-read those
        // same bytes as lines: the next header is found right after it.
        link.framesTorn++;
      }
      need = 0;
      continue;
    }
    const nl = pending.indexOf(0x0a);
    if (nl < 0) {
      if (pending.length > 16384) pending = Buffer.alloc(0); // runaway line: drop
      return;
    }
    const line = pending.subarray(0, nl).toString('latin1').replace(/\r$/, '');
    pending = pending.subarray(nl + 1);
    handleLine(line);
  }
}

// Serial port. Reconnects by itself: unplugging the FTDI (which also powers the CAM)
// closes the port, and the viewer then retries every second until it's back.
// A port that stays open but goes quiet (seen on Windows after a power cycle)
// is closed and reopened too: the CAM always streams preview frames while
// asked, so 8 s without any byte means the link is dead.
const RETRY_MS = 1000, SILENT_MS = 8000;
const port = new SerialPort({ path: args.port, baudRate: args.baud, autoOpen: false, hupcl: false });
let lastDataMs = 0, retryTimer = null, everConnected = false, waitingLogged = false;
port.on('data', (chunk) => { lastDataMs = Date.now(); onData(chunk); });
port.on('error', (e) => { console.error('serial error:', e.message); });
port.on('close', () => {
  state.connected = false;
  console.error('serial port closed; reconnecting...');
  scheduleOpen();
});
function scheduleOpen() {
  if (!retryTimer) retryTimer = setTimeout(() => { retryTimer = null; openPort(); }, RETRY_MS);
}
function openPort() {
  if (port.isOpen) return;
  port.open((err) => {
    if (err) {
      if (!everConnected && !waitingLogged) console.error(`cannot open ${args.port}: ${err.message}; retrying every ${RETRY_MS / 1000} s`);
      waitingLogged = true;
      scheduleOpen();
      return;
    }
    // Don't touch DTR/RTS: on some adapters they would reset the CAM.
    port.set({ dtr: false, rts: false }, () => {});
    // Drop a half-received line or frame from before the disconnect.
    pending = Buffer.alloc(0); need = 0; frameHdr = null;
    lastDataMs = Date.now();
    state.connected = true;
    console.log(`${everConnected ? 'reconnected' : 'connected'} to ${args.port} at ${args.baud} baud`);
    everConnected = true; waitingLogged = false;
    port.write('p');
  });
}
openPort();
setInterval(() => {
  if (port.isOpen && Date.now() - lastDataMs > SILENT_MS) {
    console.error(`no data from ${args.port} for ${SILENT_MS / 1000} s; reopening the port`);
    port.close(); // the 'close' handler reconnects
  }
}, 1000);
// Ask for preview now and keep asking (the CAM forgets after a reset).
setInterval(() => { if (port.isOpen) port.write('p'); }, 2000);
// Link health, so lost bytes at this baud rate show up as numbers.
setInterval(() => {
  console.log(`[link] frames ok ${link.framesOk}, torn ${link.framesTorn}, junk lines ${link.junkLines}`);
}, 10000);

// 8-bit grayscale BMP for the browser.
function toBmp(f) {
  const rowSize = (f.w + 3) & ~3;
  const pixelBytes = rowSize * f.h;
  const offset = 14 + 40 + 1024;
  const b = Buffer.alloc(offset + pixelBytes);
  b.write('BM', 0, 'latin1');
  b.writeUInt32LE(b.length, 2);
  b.writeUInt32LE(offset, 10);
  b.writeUInt32LE(40, 14);
  b.writeInt32LE(f.w, 18);
  b.writeInt32LE(f.h, 22);       // positive height: rows stored bottom-up
  b.writeUInt16LE(1, 26);
  b.writeUInt16LE(8, 28);
  b.writeUInt32LE(pixelBytes, 34);
  b.writeUInt32LE(256, 46);
  for (let i = 0; i < 256; i++) b.writeUInt32LE((i << 16) | (i << 8) | i, 54 + i * 4);
  for (let y = 0; y < f.h; y++) {
    f.data.copy(b, offset + (f.h - 1 - y) * rowSize, y * f.w, (y + 1) * f.w);
  }
  return b;
}

// HTTP server, localhost only.
const page = fs.readFileSync(path.join(__dirname, 'index.html'));
http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(page);
  } else if (url.pathname === '/frame.bmp') {
    if (!state.frame) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': 'image/bmp', 'Cache-Control': 'no-store' });
    res.end(toBmp(state.frame));
  } else if (url.pathname === '/state') {
    const since = Number(url.searchParams.get('since') || 0);
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify({
      connected: state.connected,
      frameSeq: state.frameSeq,
      codes: state.frame ? state.frame.codes : 0,
      stats: state.stats,
      link,
      events: state.events.filter((e) => e.id > since),
      log: state.log.slice(-8),
    }));
  } else if (url.pathname === '/cmd' && req.method === 'POST') {
    const c = url.searchParams.get('c');
    // l = flash LED; image tuning: e/E ae level,
    // c/C contrast, b/B brightness, x auto/manual exposure, a/A exposure value, r reset.
    if (['l', 'e', 'E', 'c', 'C', 'b', 'B', 'x', 'a', 'A', 'r'].includes(c) && port.isOpen) port.write(c);
    res.writeHead(204); res.end();
  } else {
    res.writeHead(404); res.end();
  }
}).listen(args.http, '127.0.0.1', () => {
  console.log(`viewer: http://127.0.0.1:${args.http}/`);
});
