// Convert a v3 (NTAG I2C 2K) amiibo into a structurally valid 540-byte NTAG215
// tag, re-signed with the owner's retail keys.
//
// Why: a v3 tag is a standard amiibo with 64 bytes inserted at 0x80, so removing
// that block yields the classic layout -- but the UID block is wrong for an
// NTAG215 (a v3 UID is 7 contiguous bytes with no BCC interleave), and byte 3
// feeds the key-derivation seed, so the BCC cannot simply be patched. With the
// retail keys we can rebuild the UID block properly AND re-sign, producing a tag
// that is correct on both counts.
//
// The rider's amiibo ID is preserved exactly (including the 0x03 format byte);
// only the tag container changes. The machine/Figure Player lives in the v3 SRAM
// block and is NOT represented -- this is rider identity only.
//
// Requires: web/index.html (amiitool port) and the user's own key_retail.bin.
// Run: node tools/amiibo_v3_to_540_resign.mjs
import {readFileSync, writeFileSync, readdirSync} from "node:fs";
import {join} from "node:path";
import {webcrypto} from "node:crypto";
if (!globalThis.crypto) globalThis.crypto = webcrypto;

const html = readFileSync("E:/PicoSwitch2/web/index.html","utf8");
const b="// === amiibo-decrypt BEGIN", e="// === amiibo-decrypt END ===";
let blk = html.slice(html.indexOf(b), html.indexOf(e)+e.length);
blk = "function amiiboConcatBytes(p){let t=0;for(const x of p)t+=x.length;const o=new Uint8Array(t);let n=0;for(const x of p){o.set(x,n);n+=x.length;}return o;}\n"+blk;
const M = new Function(`${blk}\nreturn {amiiboParseRetailKeys,amiiboDecryptInternal,amiiboTagToInternal,amiiboDeriveKeys,amiiboHmacSha256,amiiboAesCtr};`)();
const keys = M.amiiboParseRetailKeys(new Uint8Array(readFileSync("C:/Users/notso/Downloads/key_retail.bin")));

// internal(0x208) -> 540-byte NTAG215 tag
function internalToTag(i){const t=new Uint8Array(540);
  t.set(i.subarray(0x000,0x008),0x008); t.set(i.subarray(0x008,0x028),0x080);
  t.set(i.subarray(0x028,0x04C),0x010); t.set(i.subarray(0x04C,0x1B4),0x0A0);
  t.set(i.subarray(0x1B4,0x1D4),0x034); t.set(i.subarray(0x1D4,0x1DC),0x000);
  t.set(i.subarray(0x1DC,0x208),0x054); return t;}

const dir="C:/Users/notso/Downloads/02.07.26 Kirby Air Riders amiibo";
const out=[];
for (const sub of readdirSync(dir)) {
  for (const f of readdirSync(join(dir,sub))) {
    if(!f.endsWith(".bin")) continue;
    const v3=new Uint8Array(readFileSync(join(dir,sub,f)));
    // 1. decrypt with v3 offsets -> plaintext internal
    const {internal, ok} = await M.amiiboDecryptInternal(keys, v3, true);
    if(!ok){console.log("decrypt FAILED",f);continue;}
    // 2. rebuild the UID block as a proper NTAG215 (BCC interleave)
    const uid=v3.subarray(0,7);
    const head=new Uint8Array(8);
    head[0]=uid[0];head[1]=uid[1];head[2]=uid[2];
    head[3]=0x88^uid[0]^uid[1]^uid[2];            // BCC0
    head[4]=uid[3];head[5]=uid[4];head[6]=uid[5];head[7]=uid[6];
    const plain=internal.slice(0);
    plain.set(head,0x1D4);                         // new UID block feeds the key seed
    // 3. re-sign + re-encrypt against the NEW seed
    const tagKeys=await M.amiiboDeriveKeys(keys.tag,plain);
    const dataKeys=await M.amiiboDeriveKeys(keys.data,plain);
    plain.set(await M.amiiboHmacSha256(tagKeys.hmacKey, plain.subarray(0x1D4,0x208)),0x1B4);
    plain.set(await M.amiiboHmacSha256(dataKeys.hmacKey, plain.subarray(0x029,0x208)),0x008);
    plain.set(await M.amiiboAesCtr(dataKeys.aesKey,dataKeys.aesIV,plain.subarray(0x02C,0x1B4)),0x02C);
    const tag=internalToTag(plain);
    tag[8]=tag[4]^tag[5]^tag[6]^tag[7];            // BCC1
    // 4. verify as a plain NTAG215
    const v=await M.amiiboDecryptInternal(keys,tag,false);
    const bcc0ok = tag[3]===(0x88^tag[0]^tag[1]^tag[2]);
    const bcc1ok = tag[8]===(tag[4]^tag[5]^tag[6]^tag[7]);
    const id=Buffer.from(tag.subarray(0x54,0x5C)).toString("hex");
    console.log(`${f.padEnd(30)} HMAC ${v.ok?"VALID":"INVALID"}  BCC0 ${bcc0ok?"ok":"BAD"}  BCC1 ${bcc1ok?"ok":"BAD"}  id=${id}`);
    if(v.ok&&bcc0ok&&bcc1ok) out.push([f,tag]);
  }
}
const dest="E:/PicoSwitch2/dumps/kirby-resigned-540";
try{readdirSync(dest);}catch{ (await import("node:fs")).mkdirSync(dest,{recursive:true}); }
for(const [f,t] of out) writeFileSync(join(dest,f.replace(".bin"," (540 resigned).bin")),Buffer.from(t));
console.log(`\nwrote ${out.length} re-signed 540-byte tags to ${dest}`);
