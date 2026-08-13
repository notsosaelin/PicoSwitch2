package dev.picoswitch.companion.data

import org.junit.Assert.*
import org.junit.Test

class AmiiboFilesTest {
    @Test fun `accepts only firmware supported sizes`() {
        for (size in listOf(540, 572, 2048)) {
            val data = validImage(size)
            AmiiboFiles.validate(data)
        }
        for (size in listOf(0, 539, 541, 1024)) {
            assertThrows(IllegalArgumentException::class.java) { AmiiboFiles.validate(ByteArray(size)) }
        }
    }

    @Test fun `extracts UID around BCC byte and plaintext figure id`() {
        val data = ByteArray(540)
        byteArrayOf(4,1,2,99,3,4,5,6).copyInto(data)
        byteArrayOf(0,1,2,3,4,5,6,7).copyInto(data, 0x54)
        val normalized = AmiiboFiles.normalizeImport(data)
        assertEquals("04010203040506", AmiiboFiles.uid(normalized))
        assertEquals("0001020304050607", AmiiboFiles.figureId(normalized))
    }

    @Test fun `crc uses standard uppercase eight digit representation`() {
        assertEquals("CBF43926", AmiiboFiles.crc32("123456789".encodeToByteArray()))
    }

    private fun validImage(size: Int) = ByteArray(size).apply {
        this[0] = 4
        if (size == 2048) { this[7] = 0; this[8] = 0x44 }
        else { this[3] = (0x88 xor 4).toByte(); this[8] = 0 }
        this[0x54] = 1
    }
}
