// The web portal holds the user's amiibo library and keys and can command an
// adapter, so text that arrives from a device, a file or the network must never
// reach innerHTML unescaped. This pins the three places that build HTML from
// adapter replies, the escaping helper itself, and the page's
// Content-Security-Policy.
//
//   node tests/tools/test_portal_untrusted_text.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import assert from "node:assert/strict";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const html = readFileSync(join(root, "web", "index.html"), "utf8");

// 1. The helper escapes everything that can open markup or leave an attribute.
const helper = html.match(/const escapeHtml = [\s\S]*?;\s*\n/);
assert.ok(helper, "escapeHtml must be defined");
const escapeHtml = new Function(`${helper[0]}; return escapeHtml;`)();
const hostile = `<img src=x onerror="alert('x')">&`;
const escaped = escapeHtml(hostile);
for (const ch of ["<", ">", '"', "'"])
  assert.ok(!escaped.includes(ch), `escapeHtml left ${ch} in its output`);
assert.equal(escapeHtml(42), "42");

// 2. Every innerHTML assignment is either a constant or built by a function
//    that escapes. A new one must be added here deliberately.
const assignments = [...html.matchAll(/\.innerHTML\s*=\s*([^;]+);/g)].map(m => m[1]);
assert.equal(assignments.length, 3, "review any new innerHTML assignment, then update this count");
const body = name => {
  const start = html.indexOf(`function ${name}(`);
  assert.ok(start >= 0, `${name} not found`);
  return html.slice(start, html.indexOf("\n}\n", start));
};
const tagList = html.slice(html.indexOf("const tagList ="), html.indexOf("$(\"#capHandles\").innerHTML"));
assert.ok(/escapeHtml\(k\)/.test(tagList) && /escapeHtml\(v\)/.test(tagList));
const render = body("capRenderLive");
for (const field of ["e.us", "e.kind", "e.handle", "e.len", "e.orig_len"])
  assert.ok(render.includes(`escapeHtml(${field})`), `capRenderLive must escape ${field}`);
assert.ok(render.includes("escapeHtml(hexToText("), "marker text must be escaped");
assert.ok(!/\$\{e\.[a-z_]+\}/.test(render), "capRenderLive interpolates a raw record field");
const hex = body("capHexMarkup");
assert.ok(!/\$\{(pre|hi|post|hex)\}/.test(hex), "capHexMarkup interpolates raw hex");

// 3. The policy that limits what an injection could reach.
const csp = html.match(/http-equiv="Content-Security-Policy" content="([^"]+)"/);
assert.ok(csp, "the portal must declare a Content-Security-Policy");
for (const part of ["default-src 'none'", "base-uri 'none'", "form-action 'none'",
                    "connect-src https://amiiboapi.org https://www.amiiboapi.com"])
  assert.ok(csp[1].includes(part), `CSP is missing: ${part}`);
assert.ok(!/connect-src[^;]*\*/.test(csp[1]), "connect-src must not be a wildcard");

// 4. Catalog images load over https only.
assert.ok(/safeImageUrl\(metadata\?\.imgwebp \|\| metadata\?\.image\)/.test(html));

console.log("portal_untrusted_text: all checks passed");
