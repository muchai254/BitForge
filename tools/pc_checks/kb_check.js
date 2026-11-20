// Parses KB_LAYOUT from ui.cpp (C string escapes) and checks that the pages
// are 28 characters each and cover all 94 printable non-space characters.
// Usage: node kb_check.js <path to ui.cpp>
'use strict';
const src = require('fs').readFileSync(process.argv[2], 'latin1');
const block = src.match(/KB_LAYOUT\[4\]\[KB_CHARS \+ 1\] = \{([\s\S]*?)\};/)[1];
const pages = [];
for (const line of block.split('\n')) {
  const start = line.indexOf('"');
  if (start < 0) continue;
  let s = '', i = start + 1;
  for (; i < line.length && line[i] !== '"'; i++) {
    if (line[i] === '\\') {
      i++;
      const e = line[i];
      if (e === '\\' || e === '"' || e === "'") s += e;
      else throw new Error('unexpected escape \\' + e + ' in: ' + line.trim());
    } else s += line[i];
  }
  pages.push(s);
}
const seen = new Set();
pages.forEach((p, n) => {
  console.log(`page ${n}: ${p.length} chars ${p.length === 28 ? 'OK' : 'WRONG'}: ${JSON.stringify(p.trimEnd())}`);
  for (const c of p) if (c !== ' ') {
    if (seen.has(c)) console.log(`  duplicate key: ${c}`);
    seen.add(c);
  }
});
const missing = [];
for (let c = 0x21; c <= 0x7e; c++) if (!seen.has(String.fromCharCode(c))) missing.push(String.fromCharCode(c));
console.log(`distinct keys ${seen.size} + space = ${seen.size + 1} of 95; missing: ${JSON.stringify(missing.join(''))}`);
