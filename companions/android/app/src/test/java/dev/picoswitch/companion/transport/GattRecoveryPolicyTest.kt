package dev.picoswitch.companion.transport

import org.junit.Assert.*
import org.junit.Test

class GattRecoveryPolicyTest {
    @Test fun `generic 133 receives exactly one clean retry`() {
        val error = GattTransportException("Bluetooth error 133", 133, GattFailureStage.Connect)
        assertTrue(GattRecoveryPolicy.shouldRetry(error, 0))
        assertFalse(GattRecoveryPolicy.shouldRetry(error, 1))
    }

    @Test fun `public timeout and congestion statuses are recoverable`() {
        assertTrue(GattRecoveryPolicy.isRetryable(GattTransportException("timeout", 0x93, GattFailureStage.Connect)))
        assertTrue(GattRecoveryPolicy.isRetryable(GattTransportException("congested", 0x8f, GattFailureStage.Connect)))
    }

    @Test fun `protocol and subscription failures do not loop`() {
        assertFalse(GattRecoveryPolicy.isRetryable(GattTransportException("missing service", null, GattFailureStage.Services)))
        assertFalse(GattRecoveryPolicy.isRetryable(IllegalStateException("bad identity")))
    }

    @Test fun `only current open GATT generation owns callbacks`() {
        assertTrue(GattCallbackAuthority.isAuthoritative(8, 8, callbackOwnerClosed = false))
        assertFalse(GattCallbackAuthority.isAuthoritative(9, 8, callbackOwnerClosed = false))
        assertFalse(GattCallbackAuthority.isAuthoritative(8, 8, callbackOwnerClosed = true))
        assertFalse(GattCallbackAuthority.isAuthoritative(null, 8, callbackOwnerClosed = false))
    }
}
