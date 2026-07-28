// Proves the firmware's NTAG215-compatibility view of a v3 (NTAG I2C 2K) amiibo
// is crypto-equivalent to the tag itself: removing the 64-byte block inserted at
// 0x80 must make the STANDARD tag_to_internal produce the byte-identical internal
// buffer that the v3 (tag_v3, +0x40 shift) path produces from the full tag.
// If that holds, a console validating our view as a plain NTAG215 computes the
// same HMACs the real tag was signed with. Reuses the portal's amiitool port.
// Also validates the full 64-byte SRAM response trailer (CRC-16/MCRF4XX).
// Run: node tools/test_ns2_v3_compat_view.mjs <dump-directory>
// Or set PICOSWITCH2_V3_DUMP_DIR.
import {readFileSync, readdirSync} from "node:fs";
import {join} from "node:path";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const begin = "// === amiibo-decrypt BEGIN";
const end = "// === amiibo-decrypt END ===";
const block = html.slice(html.indexOf(begin), html.indexOf(end) + end.length);
const {amiiboTagToInternal} = new Function(
  `${block}\nreturn {amiiboTagToInternal};`)();

// Mirror of ns2_v3_build_compat540() in src/switch_pro2/switch_pro2.c
const SPLIT = 0x80, SHIFT = 0x40, RAW = 540;
function compat540(v3) {
  const out = new Uint8Array(RAW);
  out.set(v3.subarray(0, SPLIT), 0);
  out.set(v3.subarray(SPLIT + SHIFT, SPLIT + SHIFT + (RAW - SPLIT)), SPLIT);
  return out;
}

function crc16Mcrf4xx(bytes) {
  let crc = 0xFFFF;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit++)
      crc = (crc >>> 1) ^ ((crc & 1) ? 0x8408 : 0);
  }
  return crc & 0xFFFF;
}

const dir = process.argv[2] ?? process.env.PICOSWITCH2_V3_DUMP_DIR;
if (!dir) {
  console.error(
    "usage: node tools/test_ns2_v3_compat_view.mjs <dump-directory>\n" +
    "       or set PICOSWITCH2_V3_DUMP_DIR");
  process.exit(2);
}
const files = [];
for (const sub of readdirSync(dir))
  for (const f of readdirSync(join(dir, sub)))
    if (f.endsWith(".bin")) files.push(join(dir, sub, f));

let checked = 0;
for (const f of files) {
  const v3 = new Uint8Array(readFileSync(f));
  if (v3.length !== 2048) { console.error("FAIL: not 2048:", f); process.exit(1); }
  const viaV3 = amiiboTagToInternal(v3, true);
  const viaCompat = amiiboTagToInternal(compat540(v3), false);
  if (Buffer.compare(Buffer.from(viaV3), Buffer.from(viaCompat)) !== 0) {
    console.error("FAIL: internal buffers differ for", f);
    process.exit(1);
  }
  const sram = v3.subarray(0x3C0, 0x400);
  const calculated = crc16Mcrf4xx(sram.subarray(0, 62));
  const stored = (sram[62] << 8) | sram[63];
  if (calculated !== stored) {
    console.error(
      `FAIL: SRAM CRC mismatch for ${f}: calculated ${calculated.toString(16)}, ` +
      `stored ${stored.toString(16)}`);
    process.exit(1);
  }
  checked++;
}
console.log(
  `v3_compat_view: ${checked} dumps — standard view is crypto-identical and ` +
  "all complete SRAM responses have valid CRC-16/MCRF4XX");
