package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Regression coverage for the 2026-08-21 fresh-pair failure: `createBond()` (TRANSPORT_AUTO) runs
 * BR/EDR SSP against a dual-mode adapter record, the firmware's Classic admission gate correctly
 * refuses it, and Android reports "incorrect PIN or passkey". Every mechanism this policy can pick
 * must be LE, and TRANSPORT_AUTO must never reappear as a fallback.
 */
class AdapterBondStartPolicyTest {

    private class FakePlatform(
        val le: Boolean?,
        val type: String = "dual",
    ) : AdapterBondStarter.Platform {
        var leCalls = 0
        override fun createBondOnLe(): Boolean? { leCalls++; return le }
        override fun cachedDeviceTypeName(): String = type
    }

    @Test
    fun `an accepted LE createBond owns the procedure`() {
        val platform = FakePlatform(le = true)
        val result = AdapterBondStarter(platform).start()

        assertEquals(AdapterBondMechanism.LeCreateBond, result.mechanism)
        assertTrue(result.startedExplicitBond)
        assertFalse(result.delegatesToGatt)
        assertEquals(1, platform.leCalls)
        assertTrue("the cached device type must stay attributable", result.detail.contains("type=dual"))
    }

    @Test
    fun `a platform without the LE entry point delegates to the public GATT path`() {
        val result = AdapterBondStarter(FakePlatform(le = null)).start()

        assertEquals(AdapterBondMechanism.LeGattInitiated, result.mechanism)
        assertTrue(result.delegatesToGatt)
        assertFalse(result.startedExplicitBond)
        assertTrue(result.detail.contains("le=unavailable"))
    }

    @Test
    fun `a working entry point that refuses is reported, not papered over with GATT`() {
        val result = AdapterBondStarter(FakePlatform(le = false)).start()

        assertEquals(AdapterBondMechanism.Unavailable, result.mechanism)
        assertFalse(result.startedExplicitBond)
        assertFalse(result.delegatesToGatt)
        assertTrue(result.detail.contains("le=refused"))
    }

    @Test
    fun `a throwing platform degrades to the public GATT path instead of crashing`() {
        val throwing = object : AdapterBondStarter.Platform {
            override fun createBondOnLe(): Boolean? = throw SecurityException("hidden api blocked")
            override fun cachedDeviceTypeName(): String = throw SecurityException("no permission")
        }
        val result = AdapterBondStarter(throwing).start()

        assertEquals(AdapterBondMechanism.LeGattInitiated, result.mechanism)
        assertTrue(result.detail.contains("unreadable"))
    }

    @Test
    fun `no mechanism is ever TRANSPORT_AUTO`() {
        // The enum itself is the guarantee: a future edit that reintroduces an auto-transport
        // fallback has to add a constant here, and this fails first.
        assertEquals(
            listOf("le-create-bond", "le-gatt-initiated", "none"),
            AdapterBondMechanism.entries.map { it.diagnosticName },
        )
    }
}
