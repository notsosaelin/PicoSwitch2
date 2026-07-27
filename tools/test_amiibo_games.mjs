// Tests the AmiiboAPI ?showgames reduction in web/index.html: the title-ID ->
// game-name map used to name whichever game wrote an amiibo's save data, and the
// per-amiibo compatible-games lists.
//
// Runs against a cached copy of the real API response when one is present
// (tools/fixtures/amiibo-showgames.json), else against a small inline fixture so
// the test still works offline and in CI. Fetch a fresh copy with:
//   curl -s "https://amiiboapi.org/api/amiibo/?showgames" -o tools/fixtures/amiibo-showgames.json
//
// Run: node tools/test_amiibo_games.mjs
import {readFileSync, existsSync} from "node:fs";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");

// Pull the pieces under test straight out of the page so they cannot drift.
function extract(name) {
  const start = html.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`${name} not found in web/index.html`);
  let depth = 0, i = html.indexOf("{", start);
  const from = i;
  for (; i < html.length; i++) {
    if (html[i] === "{") depth++;
    else if (html[i] === "}" && --depth === 0) break;
  }
  return html.slice(start, i + 1);
}
const platforms = html.slice(
  html.indexOf("const AMIIBO_GAME_PLATFORMS"),
  html.indexOf("];", html.indexOf("const AMIIBO_GAME_PLATFORMS")) + 2);

const mod = new Function(`
  let amiiboTitleIdToGame = new Map();
  ${platforms}
  ${extract("amiiboAbsorbGames")}
  return {amiiboAbsorbGames, getMap: () => amiiboTitleIdToGame,
          resetMap: () => { amiiboTitleIdToGame = new Map(); },
          AMIIBO_GAME_PLATFORMS};
`)();

let failures = 0;
const check = (cond, msg) => { if (!cond) { console.error("FAIL:", msg); failures++; } };

const fixturePath = new URL("./fixtures/amiibo-showgames.json", import.meta.url);
const live = existsSync(fixturePath);
const catalog = live
  ? JSON.parse(readFileSync(fixturePath, "utf8"))
  : {amiibo: [{
      head: "01000000", tail: "00000002", name: "Test",
      games3DS: [{gameID: ["0004000000188B00"], gameName: "Mario Sports Superstars"}],
      gamesSwitch: [{gameID: ["010015100B514000", "0100000000010000"],
                     gameName: "Super Mario Bros. Wonder"}],
      gamesSwitch2: [],
      gamesWiiU: [{gameID: ["0005000010144F00"], gameName: "Splatoon"}],
    }]};

mod.resetMap();
const items = catalog.amiibo;
for (const item of items) mod.amiiboAbsorbGames(item);
const map = mod.getMap();

// The bulky raw arrays must be gone; only compact name lists survive caching.
for (const field of ["games3DS", "gamesWiiU", "gamesSwitch", "gamesSwitch2"])
  check(items.every(i => i[field] === undefined),
        `${field} must be stripped from the cached item`);

// Title IDs are the whole point: they are what an amiibo stores at 0xAC.
check(map.size > 0, "the title-ID map must not be empty");
for (const [id, name] of map) {
  check(/^[0-9A-F]{16}$/.test(id),
        `title ID '${id}' must be 16 uppercase hex digits (the 0xAC format)`);
  check(typeof name === "string" && name.length > 0,
        `title ID ${id} must map to a non-empty game name`);
}

// Per-platform name lists, deduplicated, keyed by the display label.
const labels = new Set(mod.AMIIBO_GAME_PLATFORMS.map(([, label]) => label));
for (const item of items) {
  if (!item.games) continue;
  for (const [label, names] of Object.entries(item.games)) {
    check(labels.has(label), `unexpected platform label '${label}'`);
    check(Array.isArray(names) && names.length > 0,
          `${label} list on ${item.name} must be non-empty when present`);
    check(new Set(names).size === names.length,
          `${label} list on ${item.name} must be deduplicated`);
  }
}

// Known-value spot checks that hold for both the live catalog and the fixture.
check(map.get("010015100B514000") === "Super Mario Bros. Wonder",
      `Switch title ID lookup (got ${map.get("010015100B514000")})`);
check(map.get("0004000000188B00") === "Mario Sports Superstars",
      `3DS title ID lookup (got ${map.get("0004000000188B00")})`);

// The map must be small enough to persist alongside the catalog without
// bloating it — that is why the raw arrays are dropped.
const mapBytes = JSON.stringify(Object.fromEntries(map)).length;
check(mapBytes < 200000, `title-ID map is ${mapBytes} bytes; expected well under 200 KB`);

// A lookup miss must be a miss, not a wrong answer.
check(map.get("FFFFFFFFFFFFFFFF") === undefined, "unknown title ID must not resolve");

if (failures) {
  console.error(`amiibo_games: ${failures} FAILURE(S)`);
  process.exit(1);
}
console.log(`amiibo_games: all tests passed (${live ? "live catalog" : "inline fixture"}, ` +
            `${items.length} amiibo, ${map.size} title IDs, ${mapBytes} B map)`);
