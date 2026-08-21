package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Regression coverage for the 2026-08-21 Adapter-page staleness: the console-slot controller
 * identity was only ever re-read by the manual Refresh button, so switching the active console
 * input left the Controller row showing whatever the last Refresh happened to see.
 *
 * The interesting part is the handover window. Taking ownership neutralizes slot 0 and waits for
 * the new owner's first fresh report, so the read taken immediately after a switch legitimately
 * returns no identity. A policy that reads once and believes that transitional answer is exactly
 * as broken as one that never reads at all.
 */
class ActiveControllerIdentityPolicyTest {

    private val physical = 2L
    private val controllerLink = 3L

    /**
     * The full sequence from the field:
     *   source A active -> request source B -> identity neutralized while awaiting B's first
     *   fresh report -> observer sees the transitional state -> first accepted B report arrives
     *   -> canonical identity becomes B -> observer converges with no further user action.
     */
    @Test
    fun `observer converges on the new owner without another user action`() {
        val policy = ActiveControllerIdentityPolicy()

        // A owns the console and its identity is published.
        assertTrue(policy.shouldRefresh(physical))
        policy.identityRead(physical, identityAttached = true)
        assertFalse("a settled identity must not be re-read", policy.shouldRefresh(physical))

        // The user switches to B. The change alone earns a read.
        assertTrue(policy.shouldRefresh(controllerLink))
        // Handover window: slot 0 is neutral until B's first fresh report.
        policy.identityRead(controllerLink, identityAttached = false)

        // The observer must keep asking rather than believing the transitional answer.
        assertTrue(policy.shouldRefresh(controllerLink))
        policy.identityRead(controllerLink, identityAttached = false)
        assertTrue(policy.shouldRefresh(controllerLink))

        // B's first accepted report lands and the adapter publishes its identity.
        policy.identityRead(controllerLink, identityAttached = true)
        assertFalse("converged; stop asking", policy.shouldRefresh(controllerLink))
    }

    /** The same in the other direction, so the fix is not Controller-Link-specific. */
    @Test
    fun `converges back to the physical controller too`() {
        val policy = ActiveControllerIdentityPolicy()

        assertTrue(policy.shouldRefresh(controllerLink))
        policy.identityRead(controllerLink, identityAttached = true)
        assertFalse(policy.shouldRefresh(controllerLink))

        assertTrue(policy.shouldRefresh(physical))
        policy.identityRead(physical, identityAttached = false)
        assertTrue(policy.shouldRefresh(physical))
        policy.identityRead(physical, identityAttached = true)
        assertFalse(policy.shouldRefresh(physical))
    }

    @Test
    fun `an unowned console settles on having no identity`() {
        val policy = ActiveControllerIdentityPolicy()
        assertTrue(policy.shouldRefresh(physical))
        policy.identityRead(physical, identityAttached = true)

        // Console input paused: no active source, so "no identity" IS the settled answer.
        assertTrue(policy.shouldRefresh(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE))
        policy.identityRead(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE, identityAttached = false)
        assertFalse(policy.shouldRefresh(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE))

        // ...and a lingering attached identity there is NOT settled; keep asking.
        val other = ActiveControllerIdentityPolicy()
        assertTrue(other.shouldRefresh(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE))
        other.identityRead(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE, identityAttached = true)
        assertTrue(other.shouldRefresh(ActiveControllerIdentityPolicy.NO_ACTIVE_SOURCE))
    }

    /**
     * A source that never publishes an identity must not turn this into an endless poll. The
     * management carrier is single-flight and shared with everything else.
     */
    @Test
    fun `convergence reads are bounded when the identity never arrives`() {
        val policy = ActiveControllerIdentityPolicy(maxConvergenceReads = 3)
        var reads = 0
        repeat(50) {
            if (policy.shouldRefresh(controllerLink)) {
                reads++
                policy.identityRead(controllerLink, identityAttached = false)
            }
        }
        assertEquals(3, reads)
    }

    @Test
    fun `a later switch earns a fresh convergence allowance`() {
        val policy = ActiveControllerIdentityPolicy(maxConvergenceReads = 2)
        repeat(5) {
            if (policy.shouldRefresh(controllerLink)) {
                policy.identityRead(controllerLink, identityAttached = false)
            }
        }
        assertFalse("allowance for this source is spent", policy.shouldRefresh(controllerLink))

        // Switching sources is a new question, not a continuation of the old one.
        assertTrue(policy.shouldRefresh(physical))
    }

    @Test
    fun `a read reported for a stale source cannot settle the current one`() {
        val policy = ActiveControllerIdentityPolicy()
        assertTrue(policy.shouldRefresh(controllerLink))

        // A late reply from the previous source must not mark the new one converged.
        policy.identityRead(physical, identityAttached = true)
        assertTrue(policy.shouldRefresh(controllerLink))
    }
}
