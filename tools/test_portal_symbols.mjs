// Catches calls to functions that do not exist in web/index.html.
//
// The portal is a single self-contained page with no build step and no type
// checking, so an undefined call only surfaces at runtime, in whatever path
// happens to reach it. That is how `setStatus is not defined` shipped: it sat on
// the last line of the initialization handler, so nothing could hit it until
// someone actually initialized an amiibo — and it then reported a *successful*
// operation as a failure.
//
// Deliberately a name-existence check, not scope analysis: it asks only "is this
// name defined somewhere in the file, or a known global?". That catches typos
// and renames with no parser dependency. Definitions are gathered from the raw
// source, so the check can only ever miss a problem, never invent one.
//
// Run: node tools/test_portal_symbols.mjs
import {readFileSync} from "node:fs";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const blocks = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]);
if (!blocks.length) { console.error("FAIL: no <script> block found"); process.exit(1); }
const src = blocks.join("\n");

const defined = new Set();
for (const re of [
  /\bfunction\s*\*?\s*([A-Za-z_$][\w$]*)\s*\(/g,         // function declarations
  /\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=/g,       // assigned bindings
  /\bclass\s+([A-Za-z_$][\w$]*)/g,
  /\b([A-Za-z_$][\w$]*)\s*:\s*(?:async\s*)?function\b/g, // object methods
]) for (const m of src.matchAll(re)) defined.add(m[1]);

// Parameters are callable too — promise executors' resolve/reject, callbacks.
for (const re of [
  /\bfunction\s*\*?\s*[A-Za-z_$][\w$]*\s*\(([^)]*)\)/g,  // named
  /\bfunction\s*\(([^)]*)\)/g,                           // anonymous
  /\(([^()]*)\)\s*=>/g,                                  // parenthesised arrow
]) for (const m of src.matchAll(re))
  for (const id of m[1].matchAll(/[A-Za-z_$][\w$]*/g)) defined.add(id[0]);
for (const m of src.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) defined.add(m[1]);

// Available without being declared here.
const globals = new Set([
  "if", "for", "while", "switch", "catch", "return", "typeof", "function",
  "new", "delete", "void", "in", "of", "do", "else", "await", "yield", "throw",
  "case", "instanceof", "super", "this", "constructor", "get", "set", "static",
  "async",
  "Array", "Object", "String", "Number", "Boolean", "Math", "JSON", "Date",
  "Map", "Set", "WeakMap", "WeakSet", "Promise", "Error", "TypeError",
  "RangeError", "RegExp", "Symbol", "BigInt", "Proxy", "Reflect", "Intl",
  "Uint8Array", "Uint16Array", "Uint32Array", "Int8Array", "Int16Array",
  "Int32Array", "Float32Array", "Float64Array", "ArrayBuffer", "DataView",
  "TextEncoder", "TextDecoder", "Blob", "File", "FileReader", "URL",
  "URLSearchParams", "AbortController", "Response", "Request", "Headers",
  "parseInt", "parseFloat", "isNaN", "isFinite", "encodeURIComponent",
  "decodeURIComponent", "encodeURI", "decodeURI", "structuredClone", "atob", "btoa",
  "window", "document", "navigator", "location", "history", "console", "alert",
  "confirm", "prompt", "fetch", "setTimeout", "clearTimeout", "setInterval",
  "clearInterval", "requestAnimationFrame", "cancelAnimationFrame", "queueMicrotask",
  "localStorage", "sessionStorage", "indexedDB", "crypto", "performance",
  "CustomEvent", "Event", "EventTarget", "MutationObserver", "IntersectionObserver",
  "getComputedStyle", "matchMedia", "DOMParser", "Image", "Audio", "Notification",
  "DecompressionStream", "CompressionStream", "WebSocket", "Worker", "BroadcastChannel",
]);

// Blank out comment and string contents with a small character scanner.
//
// A regex cannot do this reliably — an earlier attempt swallowed real code that
// followed an awkward template literal — and getting it wrong matters in both
// directions: too little stripping reads prose like "amiibo (v3)" or CSS like
// "rgb(0,0,0)" as call sites, too much hides real ones. Newlines are preserved
// so reported line numbers stay meaningful.
const NEWLINE = String.fromCharCode(10);
const BACKSLASH = String.fromCharCode(92);
function blankLiterals(text) {
  let out = "", state = "code", depth = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i], next = text[i + 1];
    const blank = (ch) => (ch === NEWLINE ? ch : " ");
    if (state === "code") {
      if (c === "/" && next === "/") { state = "line"; out += "  "; i++; continue; }
      if (c === "/" && next === "*") { state = "block"; out += "  "; i++; continue; }
      if (c === "'" || c === '"') { state = c; out += " "; continue; }
      if (c === "`") { state = "tpl"; depth = 0; out += " "; continue; }
      out += c;
    } else if (state === "line") {
      if (c === NEWLINE) { state = "code"; out += c; } else out += " ";
    } else if (state === "block") {
      if (c === "*" && next === "/") { state = "code"; out += "  "; i++; }
      else out += blank(c);
    } else if (state === "'" || state === '"') {
      if (c === BACKSLASH) { out += "  "; i++; continue; }
      if (c === state) state = "code";
      out += blank(c);
    } else if (state === "tpl") {
      // ${ ... } holds real code, so let it through and track brace nesting.
      if (c === BACKSLASH) { out += "  "; i++; continue; }
      if (c === "$" && next === "{") { state = "tplcode"; depth = 1; out += "  "; i++; continue; }
      if (c === "`") { state = "code"; out += " "; continue; }
      out += blank(c);
    } else if (state === "tplcode") {
      if (c === "{") depth++;
      else if (c === "}" && --depth === 0) { state = "tpl"; out += " "; continue; }
      out += c;
    }
  }
  return out;
}
const scan = blankLiterals(src);

const offenders = new Map();
const callRe = /(?<![.\w$)\]])([A-Za-z_$][\w$]*)\s*\(/g;
scan.split(NEWLINE).forEach((line, index) => {
  for (const m of line.matchAll(callRe)) {
    const name = m[1];
    if (defined.has(name) || globals.has(name)) continue;
    if (!offenders.has(name)) offenders.set(name, index + 1);
  }
});

if (offenders.size) {
  console.error("FAIL: calls to names never defined in web/index.html:");
  for (const [name, line] of offenders)
    console.error(`  ${name}()  — first seen around script line ${line}`);
  console.error(NEWLINE + "If one is a legitimate global, add it to the globals " +
                "list in tools/test_portal_symbols.mjs.");
  process.exit(1);
}
console.log(`portal_symbols: ${defined.size} names in scope, no undefined calls`);
