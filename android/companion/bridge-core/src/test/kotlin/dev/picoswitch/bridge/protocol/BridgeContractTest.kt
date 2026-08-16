package dev.picoswitch.bridge.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest

/**
 * Firmware/app bridge-contract skew detection.
 *
 * The failure being prevented: the app was updated, the adapter kept older
 * firmware, exact descriptor identification failed, and the firmware fell back
 * to the v1 generic profile. Buttons and sticks kept working while battery,
 * motion and rumble disappeared together — with nothing anywhere saying why.
 */
class BridgeContractTest {

    @Test fun `equal contracts are compatible`() {
        val result = BridgeContract.evaluate(BridgeContract.VERSION, connected = true)
        assertEquals(BridgeContract.Compatibility.Compatible(BridgeContract.VERSION), result)
        assertTrue(BridgeContract.isProvenCompatible(result))
        assertTrue(result.summary, result.summary.contains("compatible"))
    }

    @Test fun `older firmware is an explicit mismatch that names the fix`() {
        val result = BridgeContract.evaluate(BridgeContract.VERSION - 1, connected = true)
        val mismatch = result as BridgeContract.Compatibility.Mismatch
        assertEquals(BridgeContract.VERSION - 1, mismatch.firmware)
        assertEquals(BridgeContract.VERSION, mismatch.companion)
        assertTrue(mismatch.firmwareIsOlder)
        assertFalse(BridgeContract.isProvenCompatible(result))
        // The summary must name BOTH the symptom and the action, because the
        // symptom alone reads as unrelated feature loss.
        assertTrue(mismatch.summary, mismatch.summary.contains("INCOMPATIBLE"))
        assertTrue(mismatch.summary, mismatch.summary.contains("Flash current firmware"))
        assertTrue(mismatch.summary, mismatch.summary.contains("battery, motion and rumble"))
    }

    @Test fun `newer firmware points at the app instead of the firmware`() {
        val result = BridgeContract.evaluate(BridgeContract.VERSION + 1, connected = true)
        val mismatch = result as BridgeContract.Compatibility.Mismatch
        assertFalse(mismatch.firmwareIsOlder)
        assertTrue(mismatch.summary, mismatch.summary.contains("companion build"))
    }

    /**
     * The critical case. Firmware that reports nothing predates contract
     * reporting, so it is necessarily older than this app. Treating silence as
     * agreement is precisely how the original incident stayed invisible.
     */
    @Test fun `firmware that reports no contract is never called compatible`() {
        listOf(null, 0, -1).forEach { reported ->
            val result = BridgeContract.evaluate(reported, connected = true)
            assertTrue("reported=$reported", result is BridgeContract.Compatibility.Unknown)
            assertFalse("reported=$reported", BridgeContract.isProvenCompatible(result))
            assertTrue(result.summary, result.summary.contains("UNVERIFIED"))
        }
    }

    @Test fun `nothing connected is not a compatibility claim either way`() {
        val result = BridgeContract.evaluate(BridgeContract.VERSION, connected = false)
        assertEquals(BridgeContract.Compatibility.NotConnected, result)
        assertFalse(BridgeContract.isProvenCompatible(result))
    }

    /**
     * THE version-bump guard: ANY byte of the descriptor, not a chosen few.
     *
     * An earlier version of this test pinned the three bytes that happened to
     * change for contract 3, which only catches the change you already thought
     * of. A digest over the whole descriptor catches all 161. Change one byte
     * anywhere and this fails until someone bumps VERSION and registers the new
     * digest, which is precisely the deliberate act that was missing when 14
     * buttons became 15 and shipped against older firmware.
     */
    @Test fun `any descriptor byte change requires a contract version bump`() {
        val expected = BridgeContract.expectedDescriptorDigest
        assertNotNull(
            "contract ${BridgeContract.VERSION} has no registered descriptor digest; " +
                "add one to BridgeContract.DESCRIPTOR_DIGESTS",
            expected,
        )
        val actual = sha256(BridgeHidDescriptor.bytes)
        assertEquals(
            """
            |The HID descriptor no longer matches the digest registered for bridge
            |contract ${BridgeContract.VERSION}.
            |
            |If the change was INTENTIONAL and is observable by a peer, this guard is
            |firing correctly. Do all of:
            |  1. bump ANDROID_BRIDGE_CONTRACT_VERSION in
            |     tools/fixtures/android_controller_hid.h
            |  2. bump BridgeContract.VERSION to match
            |  3. add the new digest to BridgeContract.DESCRIPTOR_DIGESTS:
            |       ${BridgeContract.VERSION + 1} to "$actual",
            |  4. reflash the adapter before testing the new APK -- a skew here
            |     silently disables battery, motion and rumble while buttons keep working.
            |
            |If the change was NOT intentional, revert the descriptor.
            """.trimMargin(),
            expected,
            actual,
        )
    }

    /**
     * Guards the guard: a digest test is worthless if it silently digests
     * nothing, and readable anchors document what contract 3 actually is.
     */
    @Test fun `the digest covers the whole descriptor`() {
        assertEquals(3, BridgeContract.VERSION)
        assertEquals(161, BridgeHidDescriptor.bytes.size)
        // Contract 3's defining bytes, kept as documentation. The digest above is
        // what enforces; these say what changed and where.
        assertEquals(0x0F.toByte(), BridgeHidDescriptor.bytes[36]) // Usage Maximum (15)
        assertEquals(0x0F.toByte(), BridgeHidDescriptor.bytes[44]) // Report Count (15)
        assertEquals(0x01.toByte(), BridgeHidDescriptor.bytes[50]) // one pad bit

        // Flipping any single byte must move the digest -- including bytes far
        // from the ones above, which is the whole point.
        val baseline = sha256(BridgeHidDescriptor.bytes)
        listOf(0, 7, 80, 120, 160).forEach { index ->
            val mutated = BridgeHidDescriptor.bytes.copyOf()
            mutated[index] = (mutated[index].toInt() xor 0x01).toByte()
            assertNotEquals("byte $index is not covered by the digest", baseline, sha256(mutated))
        }
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes)
            .joinToString("") { "%02x".format(it) }
}
