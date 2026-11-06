package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Test

class AdapterBondWaitPolicyTest {
    @Test fun `bonded polling sample advances when broadcast is missing`() {
        val policy = AdapterBondWaitPolicy(AndroidBondState.None)

        assertEquals(AdapterBondWaitOutcome.Continue, policy.observe(AndroidBondState.None))
        assertEquals(AdapterBondWaitOutcome.Continue, policy.observe(AndroidBondState.Bonding))
        assertEquals(AdapterBondWaitOutcome.Bonded, policy.observe(AndroidBondState.Bonded))
    }

    @Test fun `initial none is grace state rather than immediate rejection`() {
        val policy = AdapterBondWaitPolicy(AndroidBondState.None)

        assertEquals(AdapterBondWaitOutcome.Continue, policy.observe(AndroidBondState.None))
    }

    @Test fun `none after observed bonding is authoritative rejection`() {
        val policy = AdapterBondWaitPolicy(AndroidBondState.Bonding)

        assertEquals(AdapterBondWaitOutcome.Rejected, policy.observe(AndroidBondState.None))
    }

    @Test fun `already bonding joins and accepts bonded completion`() {
        val policy = AdapterBondWaitPolicy(AndroidBondState.Bonding)

        assertEquals(AdapterBondWaitOutcome.Continue, policy.observe(AndroidBondState.Bonding))
        assertEquals(AdapterBondWaitOutcome.Bonded, policy.observe(AndroidBondState.Bonded))
    }
}
