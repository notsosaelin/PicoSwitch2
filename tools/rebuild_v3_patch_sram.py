p = 'E:/PicoSwitch2/dumps/kirby-warpstar-rebuilt-from-genuine.bin'
b = bytearray(open(p, 'rb').read())
# The genuine 0x21 device response for THIS physical machine, from the capture's
# 0x18 result buffer [19..50]. The page read never covers 0x3C0, so it has to be
# carried across separately.
dev = bytes.fromhex(
    "0200732AB41C4AC291B9A5983C039400C9000A50423457313720010102000000")
print("was:", b[0x3C0:0x3E0].hex().upper())
b[0x3C0:0x3E0] = dev
print("now:", b[0x3C0:0x3E0].hex().upper())
open(p, 'wb').write(bytes(b))

import binascii
print("uid   :", b[0:7].hex().upper())
print("size  :", len(b))
print("crc32 : %08x" % (binascii.crc32(bytes(b)) & 0xFFFFFFFF))
