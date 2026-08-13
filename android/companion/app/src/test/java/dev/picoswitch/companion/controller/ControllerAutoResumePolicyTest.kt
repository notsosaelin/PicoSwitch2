package dev.picoswitch.companion.controller

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerAutoResumePolicyTest {
    @Test fun `idle saved handheld may query after management reconnect`() {
        assertTrue(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, false, BridgePhase.Idle))
        assertTrue(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, false, BridgePhase.Ready))
    }

    @Test fun `missing relationship source or management cannot auto resume`() {
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(false, true, true, false, BridgePhase.Idle))
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, false, true, false, BridgePhase.Idle))
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, true, false, false, BridgePhase.Idle))
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, true, BridgePhase.Idle))
    }

    @Test fun `active bridge operation is never restarted`() {
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, false, BridgePhase.Registering))
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, false, BridgePhase.Connecting))
        assertFalse(ControllerAutoResumePolicy.canQueryAdapter(true, true, true, false, BridgePhase.Playing))
    }

    @Test fun `physical controller prevents handheld takeover`() {
        assertFalse(ControllerAutoResumePolicy.shouldAcquire(controllerAttached = true, bondedHostAvailable = true))
        assertFalse(ControllerAutoResumePolicy.shouldAcquire(controllerAttached = false, bondedHostAvailable = false))
        assertTrue(ControllerAutoResumePolicy.shouldAcquire(controllerAttached = false, bondedHostAvailable = true))
    }
}
