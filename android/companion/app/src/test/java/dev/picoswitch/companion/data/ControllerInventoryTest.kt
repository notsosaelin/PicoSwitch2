package dev.picoswitch.companion.data

import dev.picoswitch.management.PeerInfo
import dev.picoswitch.management.PeerInventory
import dev.picoswitch.management.PeerRole
import dev.picoswitch.management.PeerTransport
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * How the Paired controllers card decides what goes where.
 *
 * The sectioning is what the user acts on, so the invariants here are about
 * consequences rather than layout: the phone must never be offered as a
 * controller, a remembered identity must never be presented as a live one, and
 * a device the adapter has genuinely forgotten must not sit under a heading
 * that claims it is still paired.
 */
class ControllerInventoryTest {
    private fun peer(
        id: String,
        address: String = "AABBCCDDEE01",
        role: PeerRole = PeerRole.PhysicalController,
        bonded: Boolean = true,
        connected: Boolean = false,
        name: String? = null,
        classification: String? = null,
    ) = PeerInfo(
        id = id,
        address = address,
        role = role,
        transports = setOf(PeerTransport.Classic),
        bonded = bonded,
        connected = connected,
        name = name,
        classification = classification,
    )

    private fun inventory(vararg peers: PeerInfo) =
        PeerInventory(peers.toList(), complete = true, total = peers.size)

    @Test fun `connected, saved and companion rows land in their own sections`() {
        val live = inventory(
            peer("p_conn", connected = true, classification = "Sony DualSense"),
            peer("p_saved", address = "AABBCCDDEE02", classification = "Switch Pro"),
            peer("p_phone", address = "AABBCCDDEE03", role = PeerRole.ManagementCompanion, connected = true),
        )
        val view = ControllerInventory.build(live, AdapterPeerHistory())
        assertEquals(listOf("p_conn"), view.connected.map { it.peerId })
        assertEquals(listOf("p_saved"), view.saved.map { it.peerId })
        assertEquals(listOf("p_phone"), view.companion.map { it.peerId })
        assertTrue(view.recent.isEmpty())
    }

    @Test fun `a peer the adapter no longer stores appears under Recent`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_1", name = "Pad", connected = true)), 1_000)
            .observing(inventory(), 2_000)
        val view = ControllerInventory.build(inventory(), history)
        val row = view.recent.single()
        assertEquals("Pad", row.displayName)
        assertFalse(row.bonded)
        assertTrue(row.historyOnly)
        // The adapter has no opinion at all about a peer it no longer stores, so
        // the live role must not claim one.
        assertEquals(PeerRole.Unknown, row.role)
        assertEquals(PeerRole.PhysicalController, row.rememberedRole)
    }

    @Test fun `a phone this app once managed with never appears under Recent`() {
        // Under "Recent" it would read as a controller the user unpaired.
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_phone", role = PeerRole.ManagementCompanion, connected = true)), 1_000)
            .observing(inventory(), 2_000)
        val view = ControllerInventory.build(inventory(), history)
        assertTrue(view.recent.isEmpty())
    }

    @Test fun `a rebooted adapter shows the remembered name and says it is remembered`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_1", classification = "Sony DualSense", connected = true)), 1_000)
        // After a reboot the adapter can see the key but cannot identify its owner.
        val view = ControllerInventory.build(inventory(peer("p_1", role = PeerRole.Unknown)), history)
        val row = view.saved.single()
        assertEquals("Sony DualSense", row.displayName)
        assertTrue(row.identifiedFromHistory)
        // The adapter's live answer is carried through untouched. The protocol
        // requires that `unknown` be rendered as unidentified, never promoted.
        assertEquals(PeerRole.Unknown, row.role)
        assertEquals(PeerRole.PhysicalController, row.rememberedRole)
    }

    @Test fun `a rebooted adapter still keeps this phone out of the controller list`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_phone", role = PeerRole.ManagementCompanion, connected = true)), 1_000)
        val view = ControllerInventory.build(inventory(peer("p_phone", role = PeerRole.Unknown)), history)
        assertTrue(view.saved.isEmpty())
        assertEquals(listOf("p_phone"), view.companion.map { it.peerId })
    }

    @Test fun `an adapter that can name a peer wins over a stale memory`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_1", classification = "Switch Pro", connected = true)), 1_000)
        val view = ControllerInventory.build(
            inventory(peer("p_1", classification = "Sony DualSense", connected = true)),
            history,
        )
        val row = view.connected.single()
        assertEquals("Sony DualSense", row.displayName)
        assertFalse(row.identifiedFromHistory)
    }

    @Test fun `an unknown peer this app has never seen stays unidentified`() {
        val view = ControllerInventory.build(inventory(peer("p_1", role = PeerRole.Unknown)), AdapterPeerHistory())
        val row = view.saved.single()
        assertEquals(PeerRole.Unknown, row.role)
        assertEquals(PeerRole.Unknown, row.rememberedRole)
        assertFalse(row.identifiedFromHistory)
        assertEquals("Controller • EE01", row.displayName)
    }

    @Test fun `a peer still in the inventory is never duplicated into Recent`() {
        // A record can be marked unbonded by an older read and then reappear;
        // showing it twice would offer two different truths about one device.
        val stale = AdapterPeerHistory(
            listOf(
                PeerHistoryRecord(
                    peerId = "p_1",
                    address = "AABBCCDDEE01",
                    provenRole = PeerRole.PhysicalController,
                    bonded = false,
                ),
            ),
        )
        val view = ControllerInventory.build(inventory(peer("p_1", name = "Pad")), stale)
        assertEquals(listOf("p_1"), view.saved.map { it.peerId })
        assertTrue(view.recent.isEmpty())
    }

    @Test fun `an empty everything is empty`() {
        assertTrue(ControllerInventory.build(inventory(), AdapterPeerHistory()).isEmpty)
    }
}
