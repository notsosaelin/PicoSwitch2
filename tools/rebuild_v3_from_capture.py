import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(
    description="Rebuild the genuine-capture v3 fixture with a complete donor image.")
parser.add_argument(
    "donor_directory", type=Path,
    help="Directory recursively containing a Kirby & Warp Star 2048-byte dump")
parser.add_argument(
    "--capture", type=Path,
    default=REPO / "dumps" / "v3-genuine-capture-2026-07-27.jsonl")
parser.add_argument(
    "--output", type=Path,
    default=REPO / "dumps" / "kirby-warpstar-rebuilt-from-genuine.bin")
args = parser.parse_args()

recs = [json.loads(l) for l in args.capture.open(encoding="utf-8")
        if l.strip().startswith('{"trace":"record"')]


def response_data(record):
    payload = bytes.fromhex(record['payload'])
    length = payload[9] | (payload[10] << 8)
    return payload[8], payload[11:11 + length]


# The 4-block descriptor read at seq 36 is the widest the console ever asks for.
buf = bytearray()
for r in recs:
    if r['seq'] >= 36 and r['dir'] == 'device_to_console' and r['sub'] == 0x15:
        flags, data = response_data(r)
        buf += data
        if flags & 1:
            break
buf = bytes(buf)
prefix, tag = buf[:60], buf[60:]
print("captured buffer %d = prefix 60 + tag %d" % (len(buf), len(tag)))
print("signature (prefix[19:51]):", prefix[19:51].hex().upper())

# Descriptor ranges 00-3B, 3C-77, 78-91, E2-E6 -> linear image offsets.
ranges = [(0x00, 0x3B), (0x3C, 0x77), (0x78, 0x91), (0xE2, 0xE6)]
image = bytearray(2048)
covered = bytearray(2048)
pos = 0
for st, en in ranges:
    n = (en - st + 1) * 4
    image[st * 4:st * 4 + n] = tag[pos:pos + n]
    covered[st * 4:st * 4 + n] = b'\x01' * n
    print("  pages %02X-%02X -> image[0x%03X:0x%03X] (%d B)"
          % (st, en, st * 4, st * 4 + n, n))
    pos += n

print("uid from rebuilt image:", image[0:7].hex().upper())
print("coverage: %d / 2048 bytes" % sum(covered))
print("SRAM 0x3C0 region covered:", bool(covered[0x3C0]))

# Fill the uncovered remainder from the matching downloaded dump so the image is
# structurally complete; every byte the console actually reads comes from the
# genuine capture above.
matches = [
    path for path in args.donor_directory.rglob("*.bin")
    if "Kirby & Warp" in path.name
]
if len(matches) != 1:
    raise SystemExit(
        "expected exactly one Kirby & Warp Star donor, found %d" % len(matches))
src = matches[0]
donor = src.read_bytes()
if len(donor) != 2048:
    raise SystemExit("donor must be exactly 2048 bytes: %s" % src)
filled = 0
for i in range(2048):
    if not covered[i]:
        image[i] = donor[i]
        filled += 1
print("filled %d uncovered bytes from %s" % (filled, src.name))

# The 0x21 result is an independent 83-byte buffer: a 19-byte controller
# header followed by the complete 64-byte SRAM response. It is not part of the
# descriptor page ranges above. Copying only its first 32 bytes while retaining
# the donor's CRC produced a malformed research image whose body calculated to
# 0x7AC4 but stored 0xE511.
result = bytearray()
armed = False
for r in recs:
    if r['dir'] == 'console_to_device' and r['sub'] == 0x21:
        armed = True
        continue
    if armed and r['dir'] == 'device_to_console' and r['sub'] == 0x15:
        flags, data = response_data(r)
        result += data
        if flags & 1:
            break
assert len(result) == 83, "expected 83-byte 0x21 result, got %d" % len(result)
assert result[0] == 0x18 and result[18] == 0x06
image[0x3C0:0x400] = result[19:83]
covered[0x3C0:0x400] = b'\x01' * 64
print("SRAM response:", image[0x3C0:0x400].hex().upper())

args.output.write_bytes(bytes(image))
print("wrote", args.output)
