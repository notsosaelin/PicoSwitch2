// Round-trip test for the browser amiibo decrypt + register-info extraction in
// web/index.html. Extracts the marked `amiibo-decrypt` block, encrypts a
// plaintext carrying a known nickname/owner with dummy keys (using the block's
// own primitives), then decrypts and confirms the fields read back. Proves the
// offset plumbing self-consistently; real-key correctness is confirmed against a
// genuine console-written amiibo. Run: node tools/test_amiibo_decrypt.mjs
import {readFileSync} from "node:fs";
import {webcrypto} from "node:crypto";
if (!globalThis.crypto) globalThis.crypto = webcrypto;

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const begin = "// === amiibo-decrypt BEGIN";
const end = "// === amiibo-decrypt END ===";
let block = html.slice(html.indexOf(begin), html.indexOf(end) + end.length);
// The block uses amiiboConcatBytes (defined in the ZIP block) — provide it.
block = "function amiiboConcatBytes(parts){let t=0;for(const p of parts)t+=p.length;" +
  "const o=new Uint8Array(t);let n=0;for(const p of parts){o.set(p,n);n+=p.length;}return o;}\n" + block;

const exported = ["amiiboParseRetailKeys", "amiiboDecryptInternal",
  "amiiboReadRegisterInfo", "amiiboTagToInternal", "amiiboDeriveKeys",
  "amiiboHmacSha256", "amiiboAesCtr"];
const mod = new Function(`${block}\nreturn {${exported.join(",")}};`)();

function assert(c, m) { if (!c) { console.error("FAIL:", m); process.exit(1); } }

function makeMasterKey(typeString, magicSize) {
  const b = new Uint8Array(80); crypto.getRandomValues(b);
  const ts = new TextEncoder().encode(typeString);
  b.set(ts.subarray(0, 13), 16); b[16 + Math.min(ts.length, 13)] = 0;
  b[30] = 0; b[31] = magicSize; return b;
}
const retail = new Uint8Array(160);
retail.set(makeMasterKey("unfixed infos", 14), 0);
retail.set(makeMasterKey("locked secret", 16), 80);
const keys = mod.amiiboParseRetailKeys(retail);

// internal(0x208) -> tag(540); inverse of amiiboTagToInternal (non-v3).
function internalToTag(i) {
  const t = new Uint8Array(540);
  t.set(i.subarray(0x000, 0x008), 0x008);
  t.set(i.subarray(0x008, 0x028), 0x080);
  t.set(i.subarray(0x028, 0x04C), 0x010);
  t.set(i.subarray(0x04C, 0x1B4), 0x0A0);
  t.set(i.subarray(0x1B4, 0x1D4), 0x034);
  t.set(i.subarray(0x1D4, 0x1DC), 0x000);
  t.set(i.subarray(0x1DC, 0x208), 0x054);
  return t;
}
function putUtf16(buf, off, str, le) {
  for (let k = 0; k < str.length; k++) {
    const c = str.charCodeAt(k);
    if (le) { buf[off + k*2] = c & 0xFF; buf[off + k*2 + 1] = c >> 8; }
    else { buf[off + k*2] = c >> 8; buf[off + k*2 + 1] = c & 0xFF; }
  }
}

const run = async () => {
  const nickname = "Sparky";
  const owner = "Miles";
  const internal = new Uint8Array(0x208);
  crypto.getRandomValues(internal.subarray(0x1E8, 0x208)); // keygen salt
  internal[0x2C] = 0x10;                                   // "set up" flag
  putUtf16(internal, 0x38, nickname, false);              // nickname UTF-16BE
  putUtf16(internal, 0x4C + 0x1A, owner, true);           // Mii name UTF-16LE

  // Pack (encrypt + sign) with the block's own primitives.
  const tagKeys = await mod.amiiboDeriveKeys(keys.tag, internal);
  const dataKeys = await mod.amiiboDeriveKeys(keys.data, internal);
  internal.set(await mod.amiiboHmacSha256(tagKeys.hmacKey,
    internal.subarray(0x1D4, 0x208)), 0x1B4);
  internal.set(await mod.amiiboHmacSha256(dataKeys.hmacKey,
    internal.subarray(0x029, 0x208)), 0x008);
  internal.set(await mod.amiiboAesCtr(dataKeys.aesKey, dataKeys.aesIV,
    internal.subarray(0x02C, 0x1B4)), 0x02C);
  const encTag = internalToTag(internal);

  // Decrypt and read register info.
  const {internal: dec, ok} = await mod.amiiboDecryptInternal(keys, encTag, false);
  assert(ok, "HMACs verify on decrypt");
  const info = mod.amiiboReadRegisterInfo(dec);
  assert(info.nickname === nickname, `nickname '${info.nickname}' !== '${nickname}'`);
  assert(info.owner === owner, `owner '${info.owner}' !== '${owner}'`);
  assert(info.setUp === true, "setUp true");

  // Wrong keys must fail HMAC verification.
  const wrong = mod.amiiboParseRetailKeys((() => { const w = retail.slice(); w[0] ^= 0xFF; return w; })());
  const un = await mod.amiiboDecryptInternal(wrong, encTag, false);
  assert(!un.ok, "wrong keys fail HMAC verify");

  // Uninitialized amiibo (settings flag bit 4 clear) must report no owner/nickname
  // even when the decrypted region carries leftover bytes — this is the reported
  // "mojibake owner/nickname" case for never-registered dumps.
  const raw = new Uint8Array(0x208);
  crypto.getRandomValues(raw.subarray(0x1E8, 0x208));
  raw[0x2C] = 0x00;                                  // not set up
  putUtf16(raw, 0x38, nickname, false);             // stray bytes present
  putUtf16(raw, 0x4C + 0x1A, owner, true);
  const rawTagKeys = await mod.amiiboDeriveKeys(keys.tag, raw);
  const rawDataKeys = await mod.amiiboDeriveKeys(keys.data, raw);
  raw.set(await mod.amiiboHmacSha256(rawTagKeys.hmacKey, raw.subarray(0x1D4, 0x208)), 0x1B4);
  raw.set(await mod.amiiboHmacSha256(rawDataKeys.hmacKey, raw.subarray(0x029, 0x208)), 0x008);
  raw.set(await mod.amiiboAesCtr(rawDataKeys.aesKey, rawDataKeys.aesIV,
    raw.subarray(0x02C, 0x1B4)), 0x02C);
  const unset = await mod.amiiboDecryptInternal(keys, internalToTag(raw), false);
  assert(unset.ok, "unset amiibo HMACs verify");
  const unsetInfo = mod.amiiboReadRegisterInfo(unset.internal);
  assert(unsetInfo.setUp === false, "unset amiibo reports setUp false");
  assert(unsetInfo.nickname === "", "unset amiibo yields no nickname");
  assert(unsetInfo.owner === "", "unset amiibo yields no owner");

  console.log("amiibo_decrypt: all tests passed");
};
run().catch(e => { console.error("FAIL:", e); process.exit(1); });
