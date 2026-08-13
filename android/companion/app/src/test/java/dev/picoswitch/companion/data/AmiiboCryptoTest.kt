package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCryptoState
import dev.picoswitch.companion.model.AmiiboTagType
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AmiiboCryptoTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `portal compatible retail key requires 160 bytes and both master labels`() {
        val raw = retailKeys()
        val parsed = AmiiboCrypto.parseRetailKeys(raw)
        assertEquals("unfixed-info", parsed.data.typeString.takeWhile { it.toInt() != 0 }.toByteArray().decodeToString())
        assertEquals("locked-secret", parsed.tag.typeString.takeWhile { it.toInt() != 0 }.toByteArray().decodeToString())

        assertThrows(IllegalArgumentException::class.java) {
            AmiiboCrypto.parseRetailKeys(raw.copyOf(159))
        }
        val wrong = raw.copyOf().apply { this[16] = "other".first().code.toByte() }
        assertThrows(IllegalArgumentException::class.java) { AmiiboCrypto.parseRetailKeys(wrong) }
    }

    @Test fun `reversed retail masters are normalized without retaining raw key in model`() {
        val ordered = retailKeys()
        val reversed = ByteArray(160).apply {
            ordered.copyInto(this, 0, 80, 160)
            ordered.copyInto(this, 80, 0, 80)
        }
        val parsed = AmiiboCrypto.parseRetailKeys(reversed)
        assertArrayEquals(ordered.copyOfRange(0, 16), parsed.data.hmacKey)
        assertArrayEquals(ordered.copyOfRange(80, 96), parsed.tag.hmacKey)
    }

    @Test fun `identity exposes the portal fields for ordinary and figure v3 images`() {
        val ordinary = validImage(540).apply {
            this[0x54] = 0x00
            this[0x55] = 0x01
            this[0x56] = 0x02
            this[0x57] = 0x03
            this[0x58] = 0x10
            this[0x59] = 0x11
            this[0x5A] = 0x12
            this[0x5B] = 0x13
            this[0x5C] = 0x14
            this[0x5D] = 0x15
            this[0x5E] = 0x16
            this[0x5F] = 0x17
        }
        val id = AmiiboCrypto.identity(ordinary)
        assertEquals("04010203040506", id.uid)
        assertEquals("0001020310111213", id.figureId)
        assertEquals(AmiiboTagType.Ntag215, id.tagType)
        assertEquals("Power-Up Band", id.typeName)
        assertEquals("0001", id.characterGameCode)
        assertEquals(2, id.characterVariant)
        assertEquals("1011", id.modelNumber)
        assertEquals(0x12, id.seriesCode)
        assertEquals(0x13, id.formatVersion)
        assertEquals("14151617", id.extendedVariant)

        val v3 = validImage(2048)
        assertEquals(AmiiboTagType.FigureV3, AmiiboCrypto.identity(v3).tagType)
        assertEquals("04000000000000", AmiiboCrypto.identity(v3).uid)
    }

    @Test fun `date decoding follows portal null and packed date rules`() {
        assertNull(AmiiboCrypto.decodeDate(0x00, 0x00))
        assertNull(AmiiboCrypto.decodeDate(0xFF, 0xFF))
        assertEquals("2016-02-03", AmiiboCrypto.decodeDate(0x20, 0x43))
        assertNull(AmiiboCrypto.decodeDate(0x20, 0x60))
    }

    @Test fun `invalid hmac never surfaces decrypted fields`() {
        val details = AmiiboCrypto.readDetails(validImage(), AmiiboCrypto.parseRetailKeys(retailKeys()))
        assertEquals(AmiiboCryptoState.Invalid, details.crypto)
        assertEquals("", details.owner)
        assertEquals("", details.nickname)
        assertNull(details.writeCounter)
    }

    @Test fun `portal generated golden vector decrypts and exposes register fields`() {
        val root = requireNotNull(javaClass.getResourceAsStream("/amiibo-portal-golden.json"))
            .use { Json.parseToJsonElement(it.readBytes().decodeToString()).jsonObject }
        val keys = AmiiboCrypto.parseRetailKeys(hex(root.getValue("keysHex").jsonPrimitive.content))
        val details = AmiiboCrypto.readDetails(hex(root.getValue("tagHex").jsonPrimitive.content), keys)

        assertEquals(AmiiboCryptoState.Valid, details.crypto)
        assertEquals("04010203040506", details.uid)
        assertEquals("0001020310111213", details.figureId)
        assertEquals("Miles", details.owner)
        assertEquals("Sparky", details.nickname)
        assertTrue(details.setUp)
        assertEquals("2016-02-03", details.setupDate)
        assertEquals("2023-04-12", details.lastWriteDate)
        assertEquals(300, details.writeCounter)
        assertEquals("00010000034E0B00", details.titleId)
        assertEquals("10203040", details.appId)
        assertEquals("Unrecognised game (title 00010000034E0B00)", details.appDataLabel)
    }

    @Test fun `key store atomically persists only validated local key and can forget it`() {
        val store = AmiiboKeyStore(temporary.newFolder("keys"))
        assertFalse(store.exists())
        store.import(retailKeys())
        assertTrue(store.exists())
        assertNotNull(store.read())
        assertArrayEquals(retailKeys().copyOfRange(0, 16), store.read()!!.data.hmacKey)
        store.clear()
        assertFalse(store.exists())
        assertNull(store.read())
    }

    private fun retailKeys(): ByteArray {
        val output = ByteArray(160)
        master("unfixed-info", 0x11).copyInto(output, 0)
        master("locked-secret", 0x22).copyInto(output, 80)
        return output
    }

    private fun master(label: String, seed: Int): ByteArray = ByteArray(80).apply {
        repeat(size) { index -> this[index] = ((index * 17 + seed) and 0xFF).toByte() }
        label.encodeToByteArray().copyInto(this, 16)
        this[16 + label.length] = 0
        this[31] = 16
    }

    private fun validImage(size: Int = 540) = ByteArray(size).apply {
        this[0] = 4
        if (size == 2048) {
            this[7] = 0
            this[8] = 0x44
        } else {
            this[1] = 1
            this[2] = 2
            this[3] = (0x88 xor 4 xor 1 xor 2).toByte()
            this[4] = 3
            this[5] = 4
            this[6] = 5
            this[7] = 6
            this[8] = (3 xor 4 xor 5 xor 6).toByte()
        }
        this[0x54] = 1
    }

    private fun hex(value: String): ByteArray {
        require(value.length % 2 == 0)
        return ByteArray(value.length / 2) { index ->
            value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }
}
