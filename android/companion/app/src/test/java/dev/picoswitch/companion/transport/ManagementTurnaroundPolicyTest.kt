package dev.picoswitch.companion.transport

import org.junit.Assert.assertEquals
import org.junit.Test

class ManagementTurnaroundPolicyTest {
    @Test fun `first command and settled carriers do not wait`() {
        assertEquals(0L, ManagementTurnaroundPolicy.delayMillis(1_000, 0))
        assertEquals(0L, ManagementTurnaroundPolicy.delayMillis(1_100, 1_000))
        assertEquals(0L, ManagementTurnaroundPolicy.delayMillis(1_500, 1_000))
    }

    @Test fun `immediate successor waits only for remaining turnaround`() {
        assertEquals(100L, ManagementTurnaroundPolicy.delayMillis(1_000, 1_000))
        assertEquals(97L, ManagementTurnaroundPolicy.delayMillis(1_003, 1_000))
        assertEquals(1L, ManagementTurnaroundPolicy.delayMillis(1_099, 1_000))
    }

    @Test fun `clock rollback cannot create an unbounded wait`() {
        assertEquals(0L, ManagementTurnaroundPolicy.delayMillis(900, 1_000))
    }
}
