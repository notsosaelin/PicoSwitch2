package dev.picoswitch.bridge.session

import org.junit.Assert.*
import org.junit.Test

class SessionResumePolicyTest {
    @Test fun `idle saved handheld may query after management reconnect`() {
        assertTrue(SessionResumePolicy.canQueryAdapter(true, true, true, false, BridgeLinkPhase.Idle))
        assertTrue(SessionResumePolicy.canQueryAdapter(true, true, true, false, BridgeLinkPhase.Ready))
    }

    @Test fun `missing relationship source or management cannot auto resume`() {
        assertFalse(SessionResumePolicy.canQueryAdapter(false, true, true, false, BridgeLinkPhase.Idle))
        assertFalse(SessionResumePolicy.canQueryAdapter(true, false, true, false, BridgeLinkPhase.Idle))
        assertFalse(SessionResumePolicy.canQueryAdapter(true, true, false, false, BridgeLinkPhase.Idle))
        assertFalse(SessionResumePolicy.canQueryAdapter(true, true, true, true, BridgeLinkPhase.Idle))
    }

    @Test fun `active bridge operation is never restarted`() {
        assertFalse(SessionResumePolicy.canQueryAdapter(true, true, true, false, BridgeLinkPhase.Registering))
        assertFalse(SessionResumePolicy.canQueryAdapter(true, true, true, false, BridgeLinkPhase.Connecting))
        assertFalse(SessionResumePolicy.canQueryAdapter(true, true, true, false, BridgeLinkPhase.Playing))
    }

    @Test fun `physical controller prevents handheld takeover`() {
        assertFalse(SessionResumePolicy.shouldAcquire(activeSourceId = 7, bondedHostAvailable = true))
        assertFalse(SessionResumePolicy.shouldAcquire(activeSourceId = 0, bondedHostAvailable = false))
        assertTrue(SessionResumePolicy.shouldAcquire(activeSourceId = 0, bondedHostAvailable = true))
    }

    @Test fun `sole ready source may become active only while arbiter is unowned`() {
        assertEquals(29L, SessionResumePolicy.soleSourceToActivate(0, listOf(29)))
        assertNull(SessionResumePolicy.soleSourceToActivate(8, listOf(29)))
        assertNull(SessionResumePolicy.soleSourceToActivate(0, emptyList()))
        assertNull(SessionResumePolicy.soleSourceToActivate(0, listOf(29, 30)))
    }
}
