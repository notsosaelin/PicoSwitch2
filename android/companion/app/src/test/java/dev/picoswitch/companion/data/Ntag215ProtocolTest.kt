package dev.picoswitch.companion.data

import java.io.IOException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Ntag215ProtocolTest {
    @Test
    fun `ordinary scan pins exact command sequence and assembles 540 bytes`() {
        val image = validImage()
        val fake = FakeTransceiver(image, signature = null)

        val result = Ntag215Protocol.read(fake)

        val success = assertSuccess(result)
        assertFalse(success.signatureIncluded)
        assertArrayEquals(image, success.bytes)
        assertEquals(36, fake.commands.size) // GET_VERSION + 33 READ + FAST_READ + READ_SIG
        assertArrayEquals(byteArrayOf(0x60), fake.commands.first())
        fake.commands.drop(1).dropLast(2).forEachIndexed { index, command ->
            assertArrayEquals(
                byteArrayOf(0x30, (index * 4).toByte()),
                command,
            )
        }
        assertArrayEquals(byteArrayOf(0x3A, 0x84.toByte(), 0x86.toByte()), fake.commands[34])
        assertArrayEquals(byteArrayOf(0x3C, 0x00), fake.commands.last())
    }

    @Test
    fun `exact READ_SIG appends 32 bytes and produces 572 bytes`() {
        val image = validImage()
        val signature = ByteArray(32) { (0xA0 + it).toByte() }
        val fake = FakeTransceiver(image, signature)

        val result = Ntag215Protocol.read(fake)

        val success = assertSuccess(result)
        assertTrue(success.signatureIncluded)
        assertArrayEquals(image + signature, success.bytes)
        assertArrayEquals(byteArrayOf(0x3C, 0x00), fake.commands.last())
    }

    @Test
    fun `unsupported or failed READ_SIG returns explicit 540 without zero padding`() {
        val image = validImage()
        val fakeShort = FakeTransceiver(image, signature = ByteArray(31) { 0x55 })
        val shortResult = assertSuccess(Ntag215Protocol.read(fakeShort))
        assertFalse(shortResult.signatureIncluded)
        assertEquals(540, shortResult.bytes.size)
        assertArrayEquals(image, shortResult.bytes)

        val fakeError = FakeTransceiver(image, signature = null, signatureError = true)
        val errorResult = assertSuccess(Ntag215Protocol.read(fakeError))
        assertFalse(errorResult.signatureIncluded)
        assertArrayEquals(image, errorResult.bytes)
    }

    @Test
    fun `includeSignature false does not issue READ_SIG`() {
        val fake = FakeTransceiver(validImage(), signature = ByteArray(32) { 0x11 })

        val success = assertSuccess(Ntag215Protocol.read(fake, includeSignature = false))

        assertEquals(540, success.bytes.size)
        assertFalse(fake.commands.any { it.contentEquals(byteArrayOf(0x3C, 0x00)) })
    }

    @Test
    fun `wrong version aborts before any page command`() {
        val fake = FakeTransceiver(validImage())
        fake.version = byteArrayOf(0x00, 0x04, 0x04, 0x02)

        val result = Ntag215Protocol.read(fake)

        assertRejected(result, Ntag215Rejection.GET_VERSION_RESPONSE_INVALID)
        assertEquals(1, fake.commands.size)
    }

    @Test
    fun `NTAG213 and NTAG216 versions are rejected before reads`() {
        for (storageCode in listOf(0x0F, 0x13)) {
            val fake = FakeTransceiver(validImage())
            fake.version = byteArrayOf(0x00, 0x04, 0x04, 0x02, 0x01, 0x00, storageCode.toByte(), 0x03)

            val result = Ntag215Protocol.read(fake)

            assertRejected(result, Ntag215Rejection.UNSUPPORTED_TAG)
            assertEquals(1, fake.commands.size)
        }
    }

    @Test
    fun `figure v3 version is rejected before NTAG215 reads`() {
        val fake = FakeTransceiver(validImage())
        fake.version = byteArrayOf(0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03)

        assertRejected(Ntag215Protocol.read(fake), Ntag215Rejection.FIGURE_V3_UNSUPPORTED)
        assertEquals(1, fake.commands.size)
    }

    @Test
    fun `short or overlong page response aborts immediately and never reaches FAST_READ`() {
        val fake = FakeTransceiver(validImage())
        fake.pageOverrides[0x10] = ByteArray(15)

        assertRejected(Ntag215Protocol.read(fake), Ntag215Rejection.READ_RESPONSE_INVALID)
        assertFalse(fake.commands.any { it.contentEquals(Ntag215Protocol.FAST_READ_COMMAND) })

        val overlong = FakeTransceiver(validImage())
        overlong.pageOverrides[0x10] = ByteArray(17)
        assertRejected(Ntag215Protocol.read(overlong), Ntag215Rejection.READ_RESPONSE_INVALID)
    }

    @Test
    fun `short or overlong FAST_READ response aborts before signature`() {
        for (length in listOf(11, 13)) {
            val fake = FakeTransceiver(validImage())
            fake.fastReadOverride = ByteArray(length)

            assertRejected(Ntag215Protocol.read(fake), Ntag215Rejection.FAST_READ_RESPONSE_INVALID)
            assertFalse(fake.commands.any { it.contentEquals(Ntag215Protocol.READ_SIG_COMMAND) })
        }
    }

    @Test
    fun `manufacturer and raw BCC validation reject without signature or repair`() {
        val manufacturer = validImage().apply { this[0] = 0x05 }
        val manufacturerFake = FakeTransceiver(manufacturer)
        assertRejected(Ntag215Protocol.read(manufacturerFake), Ntag215Rejection.MANUFACTURER_MISMATCH)
        assertFalse(manufacturerFake.commands.any { it.contentEquals(Ntag215Protocol.READ_SIG_COMMAND) })

        val bcc = validImage().apply { this[3] = (this[3] + 1).toByte() }
        val bccFake = FakeTransceiver(bcc)
        assertRejected(Ntag215Protocol.read(bccFake), Ntag215Rejection.BCC_INVALID)
        assertFalse(bccFake.commands.any { it.contentEquals(Ntag215Protocol.READ_SIG_COMMAND) })
        assertEquals(bcc[3], (0x8F + 1).toByte())
    }

    @Test
    fun `required transport failure aborts and no forbidden command is emitted`() {
        val fake = FakeTransceiver(validImage())
        fake.failPage = 0x20

        assertRejected(Ntag215Protocol.read(fake), Ntag215Rejection.TRANSPORT_ERROR)
        assertTrue(fake.commands.none { command ->
            command.firstOrNull()?.toInt()?.and(0xFF) in setOf(0xA2, 0x1B, 0xC0, 0xC2, 0xE0)
        })
        assertFalse(fake.commands.any { it.contentEquals(Ntag215Protocol.FAST_READ_COMMAND) })
    }

    @Test
    fun `GET_VERSION transport failure aborts with no later commands`() {
        val fake = FakeTransceiver(validImage(), versionError = true)

        assertRejected(Ntag215Protocol.read(fake), Ntag215Rejection.TRANSPORT_ERROR)
        assertEquals(1, fake.commands.size)
    }

    private fun assertSuccess(result: Ntag215ReadResult): Ntag215ReadResult.Success {
        assertTrue("expected success but got $result", result is Ntag215ReadResult.Success)
        return result as Ntag215ReadResult.Success
    }

    private fun assertRejected(result: Ntag215ReadResult, reason: Ntag215Rejection) {
        assertTrue("expected rejection but got $result", result is Ntag215ReadResult.Rejected)
        assertEquals(reason, (result as Ntag215ReadResult.Rejected).reason)
    }

    private class FakeTransceiver(
        private val image: ByteArray,
        private val signature: ByteArray? = ByteArray(0),
        private val signatureError: Boolean = false,
        private val versionError: Boolean = false,
    ) : Ntag215Transceiver {
        val commands = mutableListOf<ByteArray>()
        val pageOverrides = mutableMapOf<Int, ByteArray>()
        var fastReadOverride: ByteArray? = null
        var version: ByteArray = Ntag215Protocol.NTAG215_GET_VERSION.copyOf()
        var failPage: Int? = null

        override fun transceive(command: ByteArray): ByteArray {
            commands += command.copyOf()
            return when {
                command.contentEquals(Ntag215Protocol.GET_VERSION_COMMAND) -> {
                    if (versionError) throw IOException("GET_VERSION failed")
                    version.copyOf()
                }
                command.size == 2 && command[0].u8() == 0x30 -> {
                    val page = command[1].u8()
                    if (page == failPage) throw IOException("READ failed")
                    pageOverrides[page]?.copyOf()
                        ?: image.copyOfRange(page * 4, page * 4 + 16)
                }
                command.contentEquals(Ntag215Protocol.FAST_READ_COMMAND) -> {
                    fastReadOverride?.copyOf() ?: image.copyOfRange(0x84 * 4, 0x87 * 4)
                }
                command.contentEquals(Ntag215Protocol.READ_SIG_COMMAND) -> {
                    if (signatureError) throw IOException("READ_SIG unsupported")
                    signature?.copyOf() ?: throw IOException("READ_SIG unsupported")
                }
                else -> error("forbidden/unexpected NFC command: ${command.toHex()}")
            }
        }

        private fun Byte.u8(): Int = toInt() and 0xFF
        private fun ByteArray.toHex() = joinToString(" ") { "%02X".format(it.u8()) }
    }

    private fun validImage(): ByteArray = ByteArray(540).apply {
        this[0] = 0x04
        this[1] = 0x01
        this[2] = 0x02
        this[3] = (0x88 xor 0x04 xor 0x01 xor 0x02).toByte()
        this[4] = 0x03
        this[5] = 0x04
        this[6] = 0x05
        this[7] = 0x06
        this[8] = (0x03 xor 0x04 xor 0x05 xor 0x06).toByte()
        this[0x54] = 0x01
    }
}
