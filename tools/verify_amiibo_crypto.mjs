// Verify amiibo HMACs for a file or a directory tree of dumps.
//
// Why: "the console rejected it" has many possible causes, and cryptographic
// invalidity is the cheapest one to rule in or out. Doing so offline, before a
// hardware test, keeps console runs pointed at real unknowns. This is how the
// retail-keys hypothesis was disproven for v3 (docs/Amiibo-v3.md 18.1a): all 16
// downloaded Kirby Air Riders dumps verify, including ones the console refused.
//
// Reuses the portal's amiitool port from web/index.html so this measures exactly
// what the portal measures -- no second implementation to drift.
//
// Verification only. This script never writes a tag and never generates one; see
// the constraint recorded in docs/switch2/amiibo-identity-and-generation.md.
//
// Run: node tools/verify_amiibo_crypto.mjs <path> [key_retail.bin]
import {readFileSync, readdirSync, statSync} from "node:fs";
import {join} from "node:path";
import {webcrypto} from "node:crypto";
if (!globalThis.crypto) globalThis.crypto = webcrypto;

const target = process.argv[2];
const keyPath = process.argv[3] ?? "C:/Users/notso/Downloads/key_retail.bin";
if (!target) {
  console.error("usage: node tools/verify_amiibo_crypto.mjs <file-or-dir> [key_retail.bin]");
  process.exit(2);
}

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const begin = "// === amiibo-decrypt BEGIN", end = "// === amiibo-decrypt END ===";
let block = html.slice(html.indexOf(begin), html.indexOf(end) + end.length);
// The block relies on amiiboConcatBytes, which lives in the ZIP block.
block = "function amiiboConcatBytes(parts){let t=0;for(const p of parts)t+=p.length;" +
  "const o=new Uint8Array(t);let n=0;for(const p of parts){o.set(p,n);n+=p.length;}return o;}\n" + block;
const M = new Function(
  `${block}\nreturn {amiiboParseRetailKeys,amiiboDecryptInternal,amiiboReadRegisterInfo};`)();

const keys = M.amiiboParseRetailKeys(new Uint8Array(readFileSync(keyPath)));
const hex = (u) => Buffer.from(u).toString("hex").toUpperCase();

function collect(p) {
  if (statSync(p).isFile()) return [p];
  return readdirSync(p).flatMap((e) => collect(join(p, e)));
}

let pass = 0, fail = 0;
for (const file of collect(target).filter((f) => f.toLowerCase().endsWith(".bin"))) {
  const bytes = new Uint8Array(readFileSync(file));
  // 540 = NTAG215, 2048 = v3 / NTAG I2C Plus 2K. The v3 flag shifts the data
  // HMAC to 0x0C0 and the encrypted section to 0x0E0 (+0x40 vs NTAG215).
  if (bytes.length !== 540 && bytes.length !== 2048) {
    console.log(`${file}  SKIP (${bytes.length} B: not 540 or 2048)`);
    continue;
  }
  const label = `${file.replace(/\\/g, "/").split("/").slice(-2).join("/")}`;
  let r;
  try {
    r = await M.amiiboDecryptInternal(keys, bytes, bytes.length === 2048);
  } catch (err) {
    console.log(`${label.padEnd(46)} ERROR ${err.message}`);
    fail++;
    continue;
  }
  let who = "";
  try {
    const info = M.amiiboReadRegisterInfo(r.internal);
    if (info && (info.nickname || info.owner)) {
      who = `  nick="${info.nickname ?? ""}" owner="${info.owner ?? ""}"`;
    }
  } catch { /* an unwritten tag carries no register info */ }
  console.log(`${label.padEnd(46)} ${bytes.length}B uid=${hex(bytes.subarray(0, 7))} ` +
    `HMAC ${r.ok ? "VALID  " : "INVALID"}${who}`);
  r.ok ? pass++ : fail++;
}
console.log(`\n${pass} valid, ${fail} invalid`);
process.exit(fail ? 1 : 0);
