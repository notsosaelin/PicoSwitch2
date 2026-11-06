package dev.picoswitch.companion.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OperationAdmissionGateTest {
    @Test
    fun tapBurstAdmitsOnlyOneWorkflowUntilRelease() {
        val gate = OperationAdmissionGate()

        assertTrue(gate.tryAcquire())
        repeat(20) { assertFalse(gate.tryAcquire()) }

        gate.release()
        assertTrue(gate.tryAcquire())
    }
}
