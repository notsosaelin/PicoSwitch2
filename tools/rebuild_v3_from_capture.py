import json, glob, os

f = 'E:/PicoSwitch2/dumps/v3-genuine-capture-2026-07-27.jsonl'
recs = [json.loads(l) for l in open(f)
        if l.strip().startswith('{"trace":"record"')]

# The 4-block descriptor read at seq 36 is the widest the console ever asks for.
buf = bytearray()
for r in recs:
    if r['seq'] >= 36 and r['dir'] == 'device_to_console' and r['sub'] == 0x15:
        p = bytes.fromhex(r['payload'])
        ln = p[9] | (p[10] << 8)
        buf += p[11:11 + ln]
        if p[8] & 1:
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
d = r"C:\Users\notso\Downloads\02.07.26 Kirby Air Riders amiibo"
src = [p for p in glob.glob(os.path.join(d, "**", "*.bin"), recursive=True)
       if "Kirby & Warp" in p][0]
donor = open(src, 'rb').read()
filled = 0
for i in range(2048):
    if not covered[i]:
        image[i] = donor[i]
        filled += 1
print("filled %d uncovered bytes from %s" % (filled, os.path.basename(src)))

out = 'E:/PicoSwitch2/dumps/kirby-warpstar-rebuilt-from-genuine.bin'
open(out, 'wb').write(bytes(image))
print("wrote", out)
