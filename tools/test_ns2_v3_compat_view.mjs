// Proves the firmware's NTAG215-compatibility view of a v3 (NTAG I2C 2K) amiibo
// is crypto-equivalent to the tag itself: removing the 64-byte block inserted at
// 0x80 must make the STANDARD tag_to_internal produce the byte-identical internal
// buffer that the v3 (tag_v3, +0x40 shift) path produces from the full tag.
// If that holds, a console validating our view as a plain NTAG215 computes the
// same HMACs the real tag was signed with. Reuses the portal's amiitool port.
// Run: node tools/test_ns2_v3_compat_view.mjs
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

const dir = "C:/Users/notso/Downloads/02.07.26 Kirby Air Riders amiibo";
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
  checked++;
}
console.log(`v3_compat_view: ${checked} dumps — standard view is crypto-identical to the v3 tag`);
