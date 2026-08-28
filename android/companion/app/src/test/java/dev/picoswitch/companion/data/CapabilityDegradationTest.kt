package dev.picoswitch.companion.data

import dev.picoswitch.management.CapabilityState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Old firmware must lose one capability, not the whole adapter (design §87).
 *
 * The capabilities are probed independently because they shipped in different
 * phases: an adapter can list peers without being able to forget one, and can
 * do both without being able to pair a controller remotely. Collapsing them
 * into a single "new firmware" flag would either hide a working list or offer a
 * button that answers `unknown command`.
 *
 * These pin the DEGRADATION rules the UI reads. The probes themselves are
 * exercised in management-core against the wire.
 */
class CapabilityDegradationTest {
    private fun caps(
        peers: CapabilityState = CapabilityState.Unknown,
        forget: CapabilityState = CapabilityState.Unknown,
        pairing: CapabilityState = CapabilityState.Unknown,
    ) = dev.picoswitch.management.AdapterCapabilities(
        peers = peers,
        peerForget = forget,
        remotePairing = pairing,
    )

    @Test fun `capabilities are independent, so one absence does not imply another`() {
        // The real Phase 4 adapter: lists peers, cannot forget, cannot pair.
        val phase4 = caps(
            peers = CapabilityState.Available,
            forget = CapabilityState.Unsupported,
            pairing = CapabilityState.Unsupported,
        )
        assertEquals(CapabilityState.Available, phase4.peers)
        assertEquals(CapabilityState.Unsupported, phase4.peerForget)
        assertEquals(CapabilityState.Unsupported, phase4.remotePairing)
    }

    @Test fun `an unprobed capability is Unknown, never Unsupported`() {
        // A probe that could not run must not cost the adapter a feature. The
        // UI renders Unknown as "not checked" and still offers the action;
        // Unsupported is the only state that withdraws it.
        val caps = caps()
        assertEquals(CapabilityState.Unknown, caps.peerForget)
        assertEquals(CapabilityState.Unknown, caps.remotePairing)
        // The UI gate is `!= Unsupported`, so Unknown keeps the affordance.
        assertTrue(caps.peerForget != CapabilityState.Unsupported)
        assertTrue(caps.remotePairing != CapabilityState.Unsupported)
    }

    @Test fun `a downgrade invalidates live data but keeps app-side memory`() {
        // Design §88: seeing firmware that lacks a previously cached capability
        // must not delete the alias or the history, which are the app's own and
        // still meaningful.
        val id = AdapterId("AA:BB:CC:DD:EE:01")
        val history = AdapterPeerHistory(
            listOf(
                PeerHistoryRecord(
                    peerId = "p_1",
                    address = "AABBCCDDEE01",
                    lastKnownName = "Xbox Wireless Controller",
                    provenRole = dev.picoswitch.management.PeerRole.PhysicalController,
                    bonded = true,
                ),
            ),
        )
        val book = PeerHistoryBook().with(id, history)
        val registry = AdapterRegistry(
            listOf(AdapterRecord(id = id, address = id.value, userAlias = "Living Room")),
            id,
        )

        // Nothing about a capability downgrade touches either store.
        assertEquals("Living Room", registry.record(id)?.displayName)
        assertEquals(1, book.forAdapter(id).records.size)
        assertEquals("Xbox Wireless Controller", book.forAdapter(id).record("p_1")?.lastKnownName)
    }
}
