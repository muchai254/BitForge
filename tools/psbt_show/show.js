// Shows a saved PSBT file as an animated UR:CRYPTO-PSBT QR in a browser page,
// for the device's camera. Exists because Sparrow 2.5.5's QR leaves out the
// previous transactions, while its saved files include them. The PC is
// untrusted anyway: the device verifies everything, so a tampered file is refused.
// Usage: node show.js <file.psbt> [--http 8766] [--fragment 75] [--ms 500]
// Accepts binary, base64 or hex. Serves on localhost only.
'use strict';
const http = require('http');
const fs = require('fs');
const QRCode = require('qrcode');
const ur = require('../ur/ur');

const args = { http: 8766, fragment: 75, ms: 500 };
for (let i = 3; i < process.argv.length; i += 2) {
  const k = process.argv[i].replace(/^--/, '');
  if (!(k in args)) { console.error(`unknown option --${k}`); process.exit(1); }
  args[k] = parseInt(process.argv[i + 1], 10);
}
if (!process.argv[2]) { console.error('usage: node show.js <file.psbt>'); process.exit(1); }

let psbt = fs.readFileSync(process.argv[2]);
const text = psbt.toString('latin1').trim();
if (text.startsWith('cHNidP')) psbt = Buffer.from(text, 'base64');
else if (/^70736274ff/i.test(text)) psbt = Buffer.from(text, 'hex');
if (psbt.subarray(0, 5).toString('hex') !== '70736274ff') { console.error('not a PSBT'); process.exit(1); }

const enc = new ur.FountainEncoder(ur.cborBytes(psbt), args.fragment);
console.log(`${psbt.length} byte PSBT, ${enc.fragments.length} parts of ${enc.fragmentLen} bytes`);

const page = `<!doctype html><html><head><meta charset="utf-8"><title>PSBT QR</title><style>
body{margin:0;background:#fff;color:#333;font:15px system-ui,sans-serif;text-align:center}
#q svg{width:min(94vw,86vh);height:min(94vw,86vh)}</style></head><body><div id="q"></div><div id="s"></div><script>
async function tick(){try{const r=await fetch('/frame');document.getElementById('s').textContent=r.headers.get('X-Part');document.getElementById('q').innerHTML=await r.text();}
catch(e){document.getElementById('s').textContent='tool stopped';}setTimeout(tick,${args.ms});}tick();
</script></body></html>`;

http.createServer(async (req, res) => {
  if (req.url === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(page);
  } else if (req.url === '/frame') {
    const part = enc.nextPart();
    const s = ur.encodeUrPart('crypto-psbt', part).toUpperCase(); // QR alphanumeric mode
    const svg = await QRCode.toString(s, { type: 'svg', errorCorrectionLevel: 'L', margin: 4 });
    res.writeHead(200, { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'no-store', 'X-Part': `part ${part.seqNum} (${part.seqLen} parts, ${s.length} chars)` });
    res.end(svg);
  } else {
    res.writeHead(404);
    res.end();
  }
}).listen(args.http, '127.0.0.1', () => console.log(`open http://127.0.0.1:${args.http}/`));
