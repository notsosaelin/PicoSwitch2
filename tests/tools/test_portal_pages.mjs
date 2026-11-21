// Structure and management rules of the web portal's companion-style pages.
//
// The portal has no build step, so these are the checks a compiler and a
// component test would otherwise provide:
//
//   1. the script parses, every element it looks up exists, no id is duplicated;
//   2. the drawn keyboard is the one the companions draw;
//   3. binding destinations are exactly the firmware's;
//   4. paged reads accept the fixture's pages and REFUSE every malformed walk;
//   5. commands the pages send exist in the firmware dispatcher and are
//      reachable over Bluetooth.
//
//   node tests/tools/test_portal_pages.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import assert from "node:assert/strict";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const read = relative => readFileSync(join(root, relative), "utf8");
const html = read("web/index.html");
const scripts = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]);
assert.equal(scripts.length, 1, "the portal is one inline script");
const src = scripts[0];

// 1. Structure.
assert.doesNotThrow(() => new Function(src), "the portal script must parse");
const ids = [...html.slice(0, html.indexOf("<script")).matchAll(/\bid="([^"]+)"/g)].map(m => m[1]);
const duplicated = ids.filter((id, index) => ids.indexOf(id) !== index);
assert.deepEqual(duplicated, [], "duplicate element ids");
const idSet = new Set(ids);
const lookedUp = new Set([...src.matchAll(/\$\("#([A-Za-z0-9_]+)"\)/g)].map(m => m[1]));
for (const [, id] of src.matchAll(/setNotice\("#([A-Za-z0-9_]+)"/g)) lookedUp.add(id);
const missing = [...lookedUp].filter(id => !idSet.has(id));
assert.deepEqual(missing, [], "the script looks up elements that do not exist");

const pages = [...src.match(/const PAGE_NAMES = \[([^\]]+)\]/)[1].matchAll(/"(\w+)"/g)].map(m => m[1]);
assert.deepEqual(pages, ["play", "kbm", "amiibo", "custom", "adapter", "settings"],
  "the pages of docs/architecture/companion-shell-layout.md, in its order");
for (const page of pages) {
  assert.ok(idSet.has(`page-${page}`), `page-${page} is missing`);
  assert.ok(html.includes(`data-page="${page}"`), `no navigation item for ${page}`);
}
// Every tab has its panel.
for (const [, group, listBody] of html.matchAll(/data-tabs="(\w+)">([\s\S]*?)<\/div>/g))
  for (const [, tab] of listBody.matchAll(/data-tab="(\w+)"/g))
    assert.ok(html.includes(`data-panel="${group}:${tab}"`), `no panel for ${group}:${tab}`);

// 2. The keyboard. Parsed from the C# layout the Android parity test also reads.
const layout = read("companions/windows/src/PicoSwitch.Companion.Services/Presentation/KeyboardLayout.cs");
const rowConst = Object.fromEntries(
  [...layout.matchAll(/private const double (\w+) = ([\d.]+);/g)].map(m => [m[1], Number(m[2])]));
const parseKeys = body => [...body.matchAll(/new\((0x[0-9A-Fa-f]+|\d+), "((?:[^"\\]|\\.)*)", ([^)]*)\)/g)]
  .map(m => {
    const numbers = m[3].split(",").map(part => part.trim()).map(part => part in rowConst ? rowConst[part] : Number(part));
    return [Number(m[1]), m[2].replace(/\\\\/g, "\\"), numbers[0], numbers[1], numbers[2] ?? 1, numbers[3] ?? 1];
  });
const expected = {};
for (const m of layout.matchAll(
  /public static KeyboardCluster \w+ \{ get; \} = new\(\s*"(\w+)",\s*\[([\s\S]*?)\],\s*Columns: ([\d.]+),\s*Rows: ([\d.]+)\);/g))
  expected[m[1]] = { columns: Number(m[3]), rows: Number(m[4]), keys: parseKeys(m[2]) };
expected.Mouse = { columns: 10, rows: 1,
  keys: parseKeys(layout.match(/MouseButtons \{ get; \} =\s*\[([\s\S]*?)\];/)[1]) };
const tableSource = src.match(/const KBM_KEYBOARD = \{[\s\S]*?\n\};/);
assert.ok(tableSource, "KBM_KEYBOARD table missing");
const table = new Function(`${tableSource[0]}; return KBM_KEYBOARD;`)();
assert.deepEqual(Object.keys(table).sort(), Object.keys(expected).sort());
for (const cluster of Object.keys(expected)) {
  assert.ok(expected[cluster].keys.length > 0, `${cluster}: nothing parsed from the C# layout`);
  assert.deepEqual(table[cluster], expected[cluster],
    `${cluster} differs from KeyboardLayout.cs; regenerate the portal table`);
}
const usages = ["Main", "Navigation", "Numpad", "Other"].flatMap(c => table[c].keys.map(k => k[0]));
assert.equal(new Set(usages).size, usages.length, "a usage is drawn twice");
assert.ok(usages.every(usage => usage >= 0x04 && usage <= 0xE7), "a drawn usage is outside 0x04..0xE7");

// 3. Destinations: the firmware's table, nothing more and nothing less.
const kbmC = read("firmware/src/input/ns2_kbm.c");
const firmwareDestinations = [...kbmC.matchAll(/\{"([a-z0-9_]+)", NS2_DST_[A-Z0-9_]+\}/g)].map(m => m[1]).sort();
const portalDestinations = [...src.match(/const KBM_DESTINATIONS = \[([\s\S]*?)\n\];/)[1]
  .matchAll(/\["([a-z0-9_]+)",/g)].map(m => m[1]).sort();
assert.ok(firmwareDestinations.length >= 30);
assert.deepEqual(portalDestinations, firmwareDestinations);

// 4. Paged reads.
const pagesBlock = src.match(/\/\/ ---- management-pages ----([\s\S]*?)\/\/ ---- end management-pages ----/);
assert.ok(pagesBlock, "management-pages block missing");
const { readPaged, peerLabel, peerRoleLabel } =
  new Function(`${pagesBlock[1]}; return { readPaged, peerLabel, peerRoleLabel };`)();
const fixture = JSON.parse(read("contracts/management/protocol-v1.json"));
const replay = walk => {
  const sent = [];
  const send = async command => {
    sent.push(command);
    const step = walk.find(entry => entry.command === command);
    return step ? structuredClone(step.reply) : { error: "unknown command" };
  };
  return { send, sent };
};
{
  const { send, sent } = replay(fixture.pagination.peers);
  const result = await readPaged(send, "peers list", "peers");
  assert.equal(result.ok, true, result.error);
  assert.equal(result.rows.length, fixture.pagination.peers[0].reply.total);
  assert.deepEqual(sent, fixture.pagination.peers.map(entry => entry.command),
    "the walk must send exactly the fixture's commands");
}
{
  const { send, sent } = replay(fixture.pagination.kbm);
  const result = await readPaged(send, "kbm map kb", "bindings", 0);
  assert.equal(result.ok, true, result.error);
  assert.deepEqual(result.rows.map(row => row.src), ["key:1A", "key:16"]);
  assert.deepEqual(sent, fixture.pagination.kbm.map(entry => entry.command));
}
{
  const { send, sent } = replay(fixture.pagination.kbmProfiles);
  const result = await readPaged(send, "kbm profiles", "profiles", 0);
  assert.equal(result.ok, true, result.error);
  assert.deepEqual(sent, fixture.pagination.kbmProfiles.map(entry => entry.command));
}
// The page must start each walk the way the fixture does.
assert.ok(/`kbm map \$\{kbm\.layout\}`, "bindings", 0\)/.test(src));
assert.ok(/"kbm profiles", "profiles", 0\)/.test(src));
assert.ok(/"peers list", "peers"\)/.test(src));
const walkOf = (...replies) => {
  let index = 0;
  return async () => structuredClone(replies[Math.min(index++, replies.length - 1)]);
};
const page = (rows, next, total) => ({ total, rows, next });
const refuse = async (name, send) => {
  const result = await readPaged(send, "x", "rows");
  assert.equal(result.ok, false, `${name} must be a failure, not a shorter list`);
};
await refuse("a changed total", walkOf(page([1], 1, 2), page([2], null, 3)));
await refuse("a cursor that goes backwards", walkOf(page([1], 5, 3), page([2], 4, 3)));
await refuse("a cursor that repeats", walkOf(page([1], 1, 9), page([2], 1, 9)));
await refuse("an empty page with a cursor", walkOf(page([], 1, 2)));
await refuse("a final count below the total", walkOf(page([1], null, 2)));
await refuse("an error reply", walkOf({ error: "response_too_large", code: 413 }));
await refuse("no reply", walkOf(null));
await refuse("a missing list", walkOf({ total: 0, next: null }));
assert.equal((await readPaged(walkOf(page([], null, 0)), "x", "rows")).ok, true, "an empty inventory is valid");

// A peer is never labelled with its bare address, and class outranks name.
const peers = fixture.pagination.peers.flatMap(entry => entry.reply.peers);
const named = peers.find(peer => peer.class);
assert.equal(peerLabel(named), named.class);
assert.equal(peerLabel({ name: "Pad", addr: "001122334455" }), "Pad");
assert.equal(peerLabel({ vid: 1356, pid: 3302, addr: "001122334455" }), "Device 054C:0CE6");
const bare = peerLabel({ addr: "001122334455" });
assert.ok(bare.endsWith("4455") && !bare.includes("001122334455"));
assert.equal(peerRoleLabel("something_new"), "Not identified", "unknown roles degrade to unidentified");

// 5. Commands. Every literal command the pages send is one the firmware
//    dispatches, and one the Bluetooth allowlist admits.
const config = read("firmware/src/settings/config.c");
const allowlist = read("firmware/src/management/config_wireless_bridge.c");
const verbs = new Set();
for (const m of src.matchAll(/sendCmd\(\s*(?:`|")([a-z0-9]+)/g)) verbs.add(m[1]);
for (const m of src.matchAll(/(?:readPaged\([^,]+,\s*(?:`|")|kbmProfileCommand\(`)([a-z0-9]+)/g)) verbs.add(m[1]);
const bluetoothOnlyVerbs = [...verbs].filter(verb =>
  // USB-only research commands the diagnostics panel sends on serial.
  !["sw2cap", "audiostat", "fwreads"].includes(verb));
assert.ok(verbs.has("clink") && verbs.has("pairing") && verbs.has("peers") && verbs.has("kbm"));
for (const verb of verbs)
  assert.ok(new RegExp(`"${verb}[ "]`).test(config), `the firmware does not dispatch "${verb}"`);
for (const verb of bluetoothOnlyVerbs)
  assert.ok(new RegExp(`"${verb}[ "]`).test(allowlist), `"${verb}" is not on the Bluetooth allowlist`);

console.log(`portal_pages: ${ids.length} ids, ${usages.length} keys, ${portalDestinations.length} destinations, ` +
            `${verbs.size} command verbs; all checks passed`);
