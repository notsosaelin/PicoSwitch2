// The web portal's Controller Link encoder, held to the same fixtures as the
// companions.
//
// The portal is the fourth implementation of the bridge report (C, Kotlin, C#,
// JavaScript). Agreeing on what the report DESCRIBES is not agreeing on how it
// is FILLED, and a wrong bit is invisible until a console misbehaves, so this
// runs the page's own code against contracts/bridge_report_goldens.csv and pins
// its constants to the firmware headers and the management fixture.
//
//   node tests/tools/test_portal_controller_link.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import assert from "node:assert/strict";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const read = relative => readFileSync(join(root, relative), "utf8");
const html = read("web/index.html");

const block = html.match(/\/\/ ---- controller-link-codec ----([\s\S]*?)\/\/ ---- end controller-link-codec ----/);
assert.ok(block, "the controller-link-codec block is missing from web/index.html");
const codec = new Function(`${block[1]}
  return { CL_BRIDGE_CONTRACT, CL_VERSION, CL_OP_STATE, CL_OP_OUTPUT, CL_PAYLOAD_BYTES,
           CL_FRAME_BYTES, CL_ANALOG_EPSILON, CL_BUTTON, clHat, clNeutralState,
           clEncodeReport, clFrame, clDecodeOutput, clStick, clTrigger,
           clStateFromGamepad, clDigitalChanged, clAnalogChanged, clShouldSend };`)();
const hex = bytes => [...bytes].map(b => b.toString(16).padStart(2, "0")).join("").toUpperCase();

// 1. Constants come from the contracts, not from memory.
const define = (text, name) => {
  const match = text.match(new RegExp(`#define\\s+${name}\\s+(0x[0-9A-Fa-f]+|\\d+)u?\\b`));
  assert.ok(match, `${name} not found`);
  return Number(match[1]);
};
const bridge = read("contracts/android_controller_hid.h");
const companionLink = read("firmware/src/management/ns2_companion_link.h");
assert.equal(codec.CL_BRIDGE_CONTRACT, define(bridge, "ANDROID_BRIDGE_CONTRACT_VERSION"),
  "the portal encodes a different bridge contract than the firmware");
assert.equal(codec.CL_VERSION, define(companionLink, "NS2_COMPANION_LINK_VERSION"));
assert.equal(codec.CL_OP_STATE, define(companionLink, "NS2_COMPANION_LINK_OP_STATE"));
assert.equal(codec.CL_OP_OUTPUT, define(companionLink, "NS2_COMPANION_LINK_OP_OUTPUT"));
assert.equal(codec.CL_PAYLOAD_BYTES, define(companionLink, "NS2_COMPANION_LINK_PAYLOAD_BYTES"));
assert.equal(codec.CL_FRAME_BYTES,
  define(companionLink, "NS2_COMPANION_LINK_HEADER_BYTES") + codec.CL_PAYLOAD_BYTES);

const fixture = JSON.parse(read("contracts/management/protocol-v1.json"));
for (const [name, field] of [["CONFIG_BLE_SERVICE_UUID", "serviceUuid"], ["CONFIG_BLE_RX_UUID", "rxUuid"],
                             ["CONFIG_BLE_TX_UUID", "txUuid"],
                             ["CONFIG_BLE_CL_IN_UUID", "controllerLinkInUuid"],
                             ["CONFIG_BLE_CL_OUT_UUID", "controllerLinkOutUuid"]]) {
  const match = html.match(new RegExp(`const ${name} = "([0-9a-f-]+)"`));
  assert.ok(match, `${name} not declared`);
  assert.equal(match[1], fixture.ble[field], `${name} differs from the management fixture`);
}

// The button ordinals are the wire bits. Compare with the C# enum every other
// test trusts.
const model = read("companions/windows/src/PicoSwitch.Bridge.Core/Core/ControllerModel.cs");
const enumBody = model.match(/public enum ControllerButton\s*\{([\s\S]*?)\}/)[1];
const ordinals = Object.fromEntries([...enumBody.matchAll(/(\w+)\s*=\s*(\d+)/g)].map(m => [m[1], Number(m[2])]));
assert.deepEqual(codec.CL_BUTTON, ordinals, "button ordinals differ from ControllerButton");

// 2. The goldens. Rows that carry motion or battery are skipped: a browser has
//    neither, and the encoder writes neither.
const rows = read("contracts/bridge_report_goldens.csv").split(/\r?\n/)
  .filter(line => line && !line.startsWith("#"));
let checked = 0;
for (const line of rows) {
  const [name, lx, ly, rx, ry, lt, rt, buttons, dpad, motion, battery, , v2] = line.split(",");
  if (motion !== "-" || battery !== "-") continue;
  let bits = 0;
  if (buttons !== "-") for (const button of buttons.split("|")) {
    assert.ok(button in codec.CL_BUTTON, `${name}: unknown button ${button}`);
    bits |= 1 << codec.CL_BUTTON[button];
  }
  const state = {
    lx: Number(lx), ly: Number(ly), rx: Number(rx), ry: Number(ry), lt: Number(lt), rt: Number(rt),
    buttons: bits >>> 0,
    up: dpad.includes("U"), right: dpad.includes("R"), down: dpad.includes("D"), left: dpad.includes("L"),
  };
  assert.equal(hex(codec.clEncodeReport(state)), v2, `golden ${name}`);
  checked++;
}
assert.ok(checked >= 30, `only ${checked} goldens applied; the fixture format may have changed`);

// 3. Framing.
const frame = codec.clFrame(codec.clEncodeReport(codec.clNeutralState()), 0x1234);
assert.equal(frame.length, 30);
assert.deepEqual([...frame.subarray(0, 4)], [1, 1, 0x34, 0x12]);
assert.equal(hex(frame.subarray(4)), "8080808000000000000800000000000000000000000000000000",
  "neutral must be exactly what the firmware publishes as neutral");

// 4. Feedback decode: tolerant of length, strict about version and opcode.
assert.deepEqual(codec.clDecodeOutput(Uint8Array.of(1, 0x81, 2, 200, 100, 3, 1)),
  { reportId: 2, left: 200, right: 100, player: 3, motionWanted: true });
assert.equal(codec.clDecodeOutput(Uint8Array.of(1, 0x81, 2)).left, 0);
assert.equal(codec.clDecodeOutput(Uint8Array.of(2, 0x81, 2, 1, 1, 1, 0)), null, "wrong version");
assert.equal(codec.clDecodeOutput(Uint8Array.of(1, 0x01, 2, 1, 1, 1, 0)), null, "wrong opcode");
assert.equal(codec.clDecodeOutput(Uint8Array.of(1, 0x81, 2, 1, 1, 1, 0, 9)), null, "too long");
assert.equal(codec.clDecodeOutput(Uint8Array.of(1)), null);

// 5. Gamepad -> state. The standard mapping is positional, so the default keeps
//    position: the bottom button reaches the console's bottom button, B.
const pad = (pressed, axes = [0, 0, 0, 0], values = {}) => ({
  mapping: "standard", axes,
  buttons: Array.from({ length: 18 }, (_, i) =>
    ({ pressed: pressed.includes(i), value: values[i] ?? (pressed.includes(i) ? 1 : 0) })),
});
const bit = name => 1 << codec.CL_BUTTON[name];
assert.equal(codec.clStateFromGamepad(pad([0]), "position", 0).buttons, bit("B"));
assert.equal(codec.clStateFromGamepad(pad([1]), "position", 0).buttons, bit("A"));
assert.equal(codec.clStateFromGamepad(pad([2]), "position", 0).buttons, bit("Y"));
assert.equal(codec.clStateFromGamepad(pad([3]), "position", 0).buttons, bit("X"));
assert.equal(codec.clStateFromGamepad(pad([0]), "label", 0).buttons, bit("A"));
assert.equal(codec.clStateFromGamepad(pad([3]), "label", 0).buttons, bit("Y"));
assert.equal(codec.clStateFromGamepad(pad([4, 5, 8, 9, 10, 11, 16, 17]), "position", 0).buttons,
  bit("L1") | bit("R1") | bit("Select") | bit("Start") | bit("LeftStick") | bit("RightStick") |
  bit("Home") | bit("Capture"));
// The two layouts must never agree for a face button: collapsing them is the
// mistake docs/bridge/PROTOCOL.md 3.2 records.
for (const index of [0, 1, 2, 3])
  assert.notEqual(codec.clStateFromGamepad(pad([index]), "position", 0).buttons,
                  codec.clStateFromGamepad(pad([index]), "label", 0).buttons);
// On-screen console buttons are OR'd in, and survive with no pad input.
assert.equal(codec.clStateFromGamepad(pad([]), "position", bit("C")).buttons, bit("C"));
// Triggers are analog only, as in the Windows backend; L2/R2 bits stay clear.
const triggers = codec.clStateFromGamepad(pad([6, 7], [0, 0, 0, 0], { 6: 0.5, 7: 1 }), "position", 0);
assert.equal(triggers.buttons, 0);
assert.deepEqual([triggers.lt, triggers.rt], [128, 255]);
// Sticks: centre is exactly 128, the ends reach 1 and 255, out-of-range clamps.
assert.deepEqual([codec.clStick(0), codec.clStick(-1), codec.clStick(1), codec.clStick(5), codec.clStick(NaN)],
  [128, 1, 255, 255, 128]);
const dpad = codec.clStateFromGamepad(pad([12, 15]), "position", 0);
assert.deepEqual([dpad.up, dpad.right, dpad.down, dpad.left], [true, true, false, false]);
assert.equal(codec.clEncodeReport(dpad)[9], 1, "up + right is hat 1");
assert.equal(codec.clHat(true, false, true, false), 8, "opposites cancel");

// 6. Send policy.
const neutral = codec.clNeutralState();
const pressed = { ...neutral, buttons: bit("A") };
const nudged = { ...neutral, lx: 129 };
const moved = { ...neutral, lx: 131 };
assert.equal(codec.clShouldSend(neutral, null, 0), true, "the first state always goes");
assert.equal(codec.clShouldSend(pressed, neutral, 1), false);
assert.equal(codec.clShouldSend(pressed, neutral, 2), true, "a press waits at most 2 ms");
assert.equal(codec.clShouldSend(nudged, neutral, 50), false, "below the epsilon is not movement");
assert.equal(codec.clShouldSend(moved, neutral, 7), false);
assert.equal(codec.clShouldSend(moved, neutral, 8), true, "analog is capped at one frame per 8 ms");
assert.equal(codec.clShouldSend(neutral, neutral, 99), false);
assert.equal(codec.clShouldSend(neutral, neutral, 100), true,
  "unchanged state is resent inside the adapter's stale watchdog");
assert.ok(100 * 2 < define(companionLink, "NS2_COMPANION_LINK_STALE_MS"),
  "two keepalives must fit inside the watchdog");

console.log(`portal_controller_link: ${checked} goldens and all checks passed`);
