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

// amiiboAppDataLabel lives outside the marked block (it consults the catalog's
// title-ID map). Pull it in with an empty map so the AppID and "None" paths are
// exercised here; the catalog path is covered by test_amiibo_games.mjs.
function extractFn(name) {
  const start = html.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`${name} not found in web/index.html`);
  let depth = 0, i = html.indexOf("{", start);
  for (; i < html.length; i++) {
    if (html[i] === "{") depth++;
    else if (html[i] === "}" && --depth === 0) break;
  }
  return html.slice(start, i + 1);
}
block += "\nconst amiiboTitleIdToGame = new Map();\n" + extractFn("amiiboAppDataLabel");

const exported = ["amiiboParseRetailKeys", "amiiboDecryptInternal",
  "amiiboReadRegisterInfo", "amiiboTagToInternal", "amiiboDeriveKeys",
  "amiiboHmacSha256", "amiiboAesCtr", "amiiboPackInternal",
  "amiiboInternalToTag", "amiiboInitializeImage", "amiiboDecodeDate",
  "AMIIBO_APP_IDS", "amiiboAppDataLabel"];
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

  // ---- initialization (wipe + re-sign) ----------------------------------
  // Build a fully populated amiibo: owner, nickname, dates, write counter,
  // title id and game data, so the wipe has something to actually remove.
  const full = new Uint8Array(0x208);
  crypto.getRandomValues(full.subarray(0x1E8, 0x208));
  full[0x2C] = 0x10 | 0x20;             // registered + AppData initialized
  full[0x2D] = 0x31;                                  // country
  full[0x30] = 0x20; full[0x31] = 0x43;               // setup date
  full[0x32] = 0x2E; full[0x33] = 0x8C;               // last write date
  putUtf16(full, 0x38, nickname, false);
  putUtf16(full, 0x4C + 0x1A, owner, true);
  full.set([0x00,0x01,0x00,0x00,0x03,0x4E,0x0B,0x00], 0xAC);  // title id
  full[0xB4] = 0x01; full[0xB5] = 0x2C;               // write counter 300
  full.set([0x10, 0x20, 0x30, 0x40], 0xB6);           // app id
  crypto.getRandomValues(full.subarray(0xDC, 0x1B4)); // AppData
  const fullTag = await mod.amiiboPackInternal(keys, full, new Uint8Array(540), false);

  // Sanity: the packed image must decrypt and read back what we put in.
  const before = await mod.amiiboDecryptInternal(keys, fullTag, false);
  assert(before.ok, "packed full amiibo verifies");
  const beforeInfo = mod.amiiboReadRegisterInfo(before.internal);
  assert(beforeInfo.owner === owner, "pack round-trips the owner");
  assert(beforeInfo.writeCounter === 300, `write counter ${beforeInfo.writeCounter} !== 300`);
  assert(beforeInfo.lastWriteDate !== null, "a written amiibo has a last-write date");
  assert(beforeInfo.hasAppData, "AppData present before initialization");
  assert(beforeInfo.hasTitleId, "title id present before initialization");

  // Initialize, then verify it still passes its own HMACs and is truly blank.
  const wiped = await mod.amiiboInitializeImage(keys, fullTag, false);
  const after = await mod.amiiboDecryptInternal(keys, wiped, false);
  assert(after.ok, "initialized amiibo still passes both HMACs");
  const afterInfo = mod.amiiboReadRegisterInfo(after.internal);
  assert(afterInfo.setUp === false, "initialized amiibo is not registered");
  assert(afterInfo.owner === "", "initialized amiibo has no owner");
  assert(afterInfo.nickname === "", "initialized amiibo has no nickname");
  assert(afterInfo.lastWriteDate === null, "last-write date reset to NULL");
  assert(afterInfo.setupDate === null, "setup date reset to NULL");
  assert(afterInfo.writeCounter === 0, "write counter reset");
  assert(!afterInfo.hasAppData, "game data wiped");
  assert(!afterInfo.hasTitleId, "title id wiped");

  // The identity must survive: UID, the amiibo identity block, and every byte
  // outside the encrypted settings region are what make it the same amiibo.
  // TAG offsets (not internal): the UID + plaintext head, the amiibo identity
  // block, and the tail past the mapped regions. tag[0x014..0x054] and
  // tag[0x080..0x208] legitimately change -- they are the encrypted settings
  // region and the two re-computed HMACs.
  const sameRegions = [[0x000, 0x014], [0x054, 0x080], [0x208, 0x21C]];
  for (const [from, to] of sameRegions)
    for (let k = from; k < Math.min(to, 540); k++)
      assert(wiped[k] === fullTag[k],
        `byte 0x${k.toString(16)} outside the settings region changed`);

  // Initializing twice must be idempotent.
  const again = await mod.amiiboInitializeImage(keys, wiped, false);
  assert(again.every((b, k) => b === wiped[k]), "initialization is idempotent");

  // v3 (2 KB Air Riders) carries additional writable game state beyond the
  // ordinary encrypted amiibo body. Initialization clears those captured
  // ranges and the independent sector-1 generation while preserving the chip
  // configuration and 64-byte machine/SRAM identity block.
  const v3Original = new Uint8Array(2048);
  v3Original.set([0xA5, 0x00, 0x06, 0x00], 0x10);
  v3Original.fill(0xA1, 0x248, 0x388);
  v3Original.fill(0xB2, 0x388, 0x3C0);
  v3Original.fill(0xC3, 0x3C0, 0x400);
  v3Original.fill(0xD4, 0x400, 0x800);
  v3Original.set([0xA5, 0x00, 0x04, 0x00], 0x400);
  v3Original.set([0xA5, 0x00, 0x07, 0x00], 0x590);
  const v3Internal = full.slice();
  v3Internal.set([0xA5, 0x00, 0x06, 0x00], 0x028);
  const v3Full = await mod.amiiboPackInternal(keys, v3Internal, v3Original, true);
  const v3Wiped = await mod.amiiboInitializeImage(keys, v3Full, true);
  const v3After = await mod.amiiboDecryptInternal(keys, v3Wiped, true);
  assert(v3After.ok, "initialized v3 amiibo still passes both HMACs");
  assert(v3Wiped[0x10] === 0xA5 && v3Wiped[0x11] === 0x00 &&
         v3Wiped[0x12] === 0 && v3Wiped[0x13] === 0,
    "v3 page 4 returns to its initial A5 00 00 00 state");
  for (const [from, to, label] of [
    [0x248, 0x388, "sector-0 extended game state"],
    [0x400, 0x800, "complete allocation-independent sector-1 state"],
  ])
    assert(v3Wiped.subarray(from, to).every(value => value === 0),
      `v3 ${label} cleared`);
  for (const [from, to, value, label] of [
    [0x388, 0x3C0, 0xB2, "chip configuration"],
    [0x3C0, 0x400, 0xC3, "machine/SRAM identity"],
  ])
    assert(v3Wiped.subarray(from, to).every(byte => byte === value),
      `v3 ${label} preserved`);
  const v3Again = await mod.amiiboInitializeImage(keys, v3Wiped, true);
  assert(v3Again.every((byte, index) => byte === v3Wiped[index]),
    "v3 initialization is idempotent");

  // A tampered image must be refused rather than re-signed into something valid.
  const tampered = fullTag.slice();
  tampered[0x0A5] ^= 0xFF;                            // inside encrypted data
  let refused = false;
  try { await mod.amiiboInitializeImage(keys, tampered, false); }
  catch { refused = true; }
  assert(refused, "a dump failing its HMAC must not be re-signed");

  // AppID identifies the single owning game. An amiibo holds one application's
  // data at a time, so this is a lookup, not a list.
  assert(beforeInfo.appId === "10203040", `appId ${beforeInfo.appId}`);
  // Flags are zeroed, so bit5 is clear and the AppID is not surfaced at all.
  assert(afterInfo.appId === "", "initialization clears the AppID");
  assert(mod.AMIIBO_APP_IDS.get("10110E00") === "Super Smash Bros.",
    "known AppID resolves to its game");

  // An amiibo registered but with NO game data must not surface a title ID.
  // Regression: the AppData region of a never-written amiibo holds uninitialized
  // bytes, and scanning it for non-zero reported "has data", then rendered the
  // leftovers as a title ID ("11D10F5819F48509" on a real Charizard dump, which
  // matches no Nintendo prefix). 3dbrew: bit5 of the settings flags is the
  // authority, not the contents of the region.
  const noApp = new Uint8Array(0x208);
  crypto.getRandomValues(noApp.subarray(0x1E8, 0x208));
  noApp[0x2C] = 0x10;                              // registered, bit5 clear
  putUtf16(noApp, 0x38, nickname, false);
  putUtf16(noApp, 0x4C + 0x1A, owner, true);
  crypto.getRandomValues(noApp.subarray(0xAC, 0x1B4));  // leftover junk
  noApp[0x30] = 0x20; noApp[0x31] = 0x43;
  const noAppTag = await mod.amiiboPackInternal(keys, noApp, new Uint8Array(540), false);
  const noAppDec = await mod.amiiboDecryptInternal(keys, noAppTag, false);
  assert(noAppDec.ok, "registered-without-appdata amiibo verifies");
  const noAppInfo = mod.amiiboReadRegisterInfo(noAppDec.internal);
  assert(noAppInfo.setUp === true, "still reports registered");
  assert(noAppInfo.owner === owner, "owner still readable without app data");
  assert(noAppInfo.hasAppData === false,
    "bit5 clear must mean no app data, whatever the region contains");
  assert(noAppInfo.hasTitleId === false, "no title ID surfaced without app data");
  assert(noAppInfo.titleId === "", "title ID must be blank without app data");
  assert(noAppInfo.appId === "", "AppID must be blank without app data");
  assert(mod.amiiboAppDataLabel(noAppInfo) === "None",
    `label should be None (got '${mod.amiiboAppDataLabel(noAppInfo)}')`);
  // Dates are only loaded when bit4 or bit5 is set (3dbrew); bit4 is set here.
  assert(noAppInfo.setupDate === "2016-02-03", "setup date still read via bit4");

  // Neither bit set: dates must not be reported at all.
  const blank = new Uint8Array(0x208);
  crypto.getRandomValues(blank.subarray(0x1E8, 0x208));
  blank[0x30] = 0x20; blank[0x31] = 0x43;          // junk in the date field
  const blankTag = await mod.amiiboPackInternal(keys, blank, new Uint8Array(540), false);
  const blankInfo = mod.amiiboReadRegisterInfo(
    (await mod.amiiboDecryptInternal(keys, blankTag, false)).internal);
  assert(blankInfo.setupDate === null,
    "no dates when neither bit4 nor bit5 is set");

  // Date decode edges.
  assert(mod.amiiboDecodeDate(0x00, 0x00) === null, "0x0000 is the NULL date");
  assert(mod.amiiboDecodeDate(0xFF, 0xFF) === null, "0xFFFF is not a date");
  assert(mod.amiiboDecodeDate(0x20, 0x43) === "2016-02-03",
    `date decode got ${mod.amiiboDecodeDate(0x20, 0x43)}`);

  console.log("amiibo_decrypt: all tests passed");
};
run().catch(e => { console.error("FAIL:", e); process.exit(1); });
