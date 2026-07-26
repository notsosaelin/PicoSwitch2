// Round-trip self-test for the browser amiibo crypto embedded in web/index.html.
//
// Extracts the marked `amiibo-crypto` block from the portal and runs it under
// Node's Web Crypto (globalThis.crypto). It proves the DRBG/AES-CTR/HMAC/layout
// wiring is internally consistent: pack -> unpack reproduces the plaintext and
// both HMACs verify. It does NOT prove agreement with Nintendo's retail keys —
// that requires a genuine key file and is what the portal's "verify against a
// genuine dump" step covers. Uses arbitrary 160-byte dummy master keys.
//
// Run: node tools/test_amiibo_crypto.mjs
import {readFileSync} from "node:fs";
import {webcrypto} from "node:crypto";
if (!globalThis.crypto) globalThis.crypto = webcrypto;

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const begin = "// === amiibo-crypto BEGIN";
const end = "// === amiibo-crypto END ===";
const block = html.slice(
  html.indexOf(begin), html.indexOf(end) + end.length);
if (!block.includes("async function amiiboPack"))
  throw new Error("could not extract amiibo-crypto block");

const exported = [
  "amiiboParseRetailKeys", "amiiboPack", "amiiboUnpack", "amiiboBuildPlaintext",
  "amiiboTagToInternal", "amiiboInternalToTag", "amiiboGenerate"
];
const mod = new Function(
  `${block}\nreturn {${exported.join(",")}};`)();

function assert(cond, msg) { if (!cond) { console.error("FAIL:", msg); process.exit(1); } }

// Build a plausible 160-byte key_retail.bin with the expected typeStrings so
// amiiboParseRetailKeys accepts it. Field layout: hmacKey[16], typeString[14],
// rfu, magicSize, magicBytes[16], xorPad[32].
function makeMasterKey(typeString, magicSize) {
  const b = new Uint8Array(80);
  crypto.getRandomValues(b);
  const ts = new TextEncoder().encode(typeString);
  b.set(ts.subarray(0, 13), 16);
  b[16 + Math.min(ts.length, 13)] = 0;   // NUL terminate within typeString[14]
  b[30] = 0;                              // rfu
  b[31] = magicSize;                      // magicBytesSize <= 16
  return b;
}
const dataKey = makeMasterKey("unfixed infos", 14);
const tagKey = makeMasterKey("locked secret", 16);
const retail = new Uint8Array(160);
retail.set(dataKey, 0);
retail.set(tagKey, 80);

const keys = mod.amiiboParseRetailKeys(retail);
assert(keys.data && keys.tag, "parsed retail keys");

// Order-independence: reversed halves must still parse to the same roles.
const reversed = new Uint8Array(160);
reversed.set(tagKey, 0);
reversed.set(dataKey, 80);
const keys2 = mod.amiiboParseRetailKeys(reversed);
assert(keys2.data.hmacKey[0] === keys.data.hmacKey[0], "reversed halves normalize");

const run = async () => {
  // Layout round-trip.
  const rand = new Uint8Array(540);
  crypto.getRandomValues(rand);
  const back = mod.amiiboInternalToTag(mod.amiiboTagToInternal(rand), rand);
  assert(Buffer.compare(Buffer.from(rand), Buffer.from(back)) === 0,
    "tag<->internal round-trips");

  // pack -> unpack reproduces plaintext and both HMACs verify.
  const plain = mod.amiiboBuildPlaintext("0100000000040002");
  const packed = await mod.amiiboPack(keys, plain);
  assert(packed.length === 540, "packed is 540 bytes");
  assert(Buffer.compare(Buffer.from(packed.subarray(0x54, 0x5C)),
    Buffer.from(plain.subarray(0x54, 0x5C))) === 0, "identity survives pack");
  assert(Buffer.compare(Buffer.from(packed.subarray(0, 9)),
    Buffer.from(plain.subarray(0, 9))) === 0, "UID/BCC survive pack");
  // The encrypted settings region must actually change.
  assert(Buffer.compare(Buffer.from(packed.subarray(0x20, 0x40)),
    Buffer.from(plain.subarray(0x20, 0x40))) !== 0, "settings encrypted");

  const un = await mod.amiiboUnpack(keys, packed);
  assert(un.tagHmacOk, "tag HMAC verifies");
  assert(un.dataHmacOk, "data HMAC verifies");
  // un.plain equals the built plaintext except the two HMAC regions, which
  // buildPlaintext leaves zero and pack fills (tag 0x034-0x053, 0x080-0x09F).
  const maskHmac = t => {
    const c = t.slice();
    c.fill(0, 0x034, 0x054);
    c.fill(0, 0x080, 0x0A0);
    return c;
  };
  assert(Buffer.compare(
    Buffer.from(maskHmac(un.plain)), Buffer.from(maskHmac(plain))) === 0,
    "decrypt reproduces plaintext (outside HMAC fields)");
  // Idempotence: re-packing the recovered plaintext reproduces the tag exactly.
  const repacked = await mod.amiiboPack(keys, un.plain);
  assert(Buffer.compare(Buffer.from(repacked), Buffer.from(packed)) === 0,
    "re-pack of unpacked plaintext is byte-identical");

  // Wrong keys must fail verification.
  const wrong = mod.amiiboParseRetailKeys((() => {
    const w = retail.slice(); w[0] ^= 0xFF; return w;
  })());
  const unWrong = await mod.amiiboUnpack(wrong, packed);
  assert(!unWrong.tagHmacOk || !unWrong.dataHmacOk, "wrong keys fail verify");

  // generate() produces a verifiable tag for the same keys.
  const gen = await mod.amiiboGenerate(keys, "0100000000040002");
  const unGen = await mod.amiiboUnpack(keys, gen);
  assert(unGen.tagHmacOk && unGen.dataHmacOk, "generated tag self-verifies");

  console.log("amiibo_crypto: all round-trip tests passed");
};
run().catch(e => { console.error("FAIL:", e); process.exit(1); });
