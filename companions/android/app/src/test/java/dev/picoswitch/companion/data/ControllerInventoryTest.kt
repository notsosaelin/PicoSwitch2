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
        assertEquals(listOf("p_saved"), view.paired.map { it.peerId })
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
        val row = view.paired.single()
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
        assertTrue(view.paired.isEmpty())
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

    @Test fun `an unknown peer this app has never seen is labelled, not renamed`() {
        // It is a paired controller (see the bonded-but-unnamed test), and the
        // label must not invent an identity: no name, no promoted role, just a
        // short suffix so two unnamed devices stay distinguishable.
        val view = ControllerInventory.build(inventory(peer("p_1", role = PeerRole.Unknown)), AdapterPeerHistory())
        val row = view.paired.single()
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
        assertEquals(listOf("p_1"), view.paired.map { it.peerId })
        assertTrue(view.recent.isEmpty())
    }

    /* --------------------- identity canonicalisation (Phase 4 regression) */

    @Test fun `one phone reported under one identity is one management peer`() {
        // The firmware now emits the management observation under the RESOLVED
        // identity, so the bond row and the live row share an address and merge.
        // Before that the phone arrived as an identity row (bonded, role
        // unknown) plus an RPA row (role management, no stored key), and the
        // identity half landed under saved controllers.
        val merged = PeerInfo(
            id = "p_ident",
            address = "2C002A159CF7",
            role = PeerRole.ManagementCompanion,
            transports = setOf(PeerTransport.Classic, PeerTransport.Le),
            bonded = true,
            connected = true,
        )
        val view = ControllerInventory.build(inventory(merged), AdapterPeerHistory())
        assertEquals(listOf("p_ident"), view.companion.map { it.peerId })
        assertTrue(view.paired.isEmpty())
        assertTrue(view.connected.isEmpty())
        assertFalse(view.hasControllers)
        // CTKD data survives the merge: one device, both transports, one row.
        assertEquals(
            setOf(PeerTransport.Classic, PeerTransport.Le),
            view.companion.single().transports,
        )
    }

    @Test fun `the management phone never reaches the controller sections`() {
        val view = ControllerInventory.build(
            inventory(peer("p_phone", role = PeerRole.ManagementCompanion, connected = true)),
            AdapterPeerHistory(),
        )
        assertTrue(view.connected.isEmpty())
        assertTrue(view.paired.isEmpty())
        assertTrue(view.recent.isEmpty())
        assertTrue(view.hasDiagnosticPeers)
    }

    @Test fun `a bonded peer the adapter cannot name is still a paired controller`() {
        // The DualSense case, observed on hardware: bonded on both transports,
        // role `unknown` purely because it was not connected at the instant of
        // the inventory. Routing that to Diagnostics hid real hardware from the
        // person who paired it -- and after a reboot EVERY paired controller
        // looks exactly like this until it reconnects.
        val view = ControllerInventory.build(
            inventory(peer("p_raw", role = PeerRole.Unknown)),
            AdapterPeerHistory(),
        )
        assertEquals(listOf("p_raw"), view.paired.map { it.peerId })
        assertTrue(view.unattributed.isEmpty())
        assertTrue(view.hasControllers)
        assertFalse(view.hasDiagnosticPeers)
        // Reported honestly rather than promoted: the adapter's answer is
        // carried through untouched.
        assertEquals(PeerRole.Unknown, view.paired.single().role)
    }

    @Test fun `a remembered controller still counts as a controller after a reboot`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_1", classification = "Sony DualSense", connected = true)), 1_000)
        val view = ControllerInventory.build(inventory(peer("p_1", role = PeerRole.Unknown)), history)
        assertEquals(listOf("p_1"), view.paired.map { it.peerId })
        assertTrue(view.unattributed.isEmpty())
        assertEquals("Sony DualSense", view.paired.single().displayName)
    }

    @Test fun `a connected controller with no credential yet is mid-pairing, not unpaired`() {
        // Classic controllers really do pass through this between the ACL and
        // the link key. It is a controller, it is connected, and the user has
        // nothing to do about it.
        val view = ControllerInventory.build(
            inventory(peer("p_new", bonded = false, connected = true, name = "DualSense Edge")),
            AdapterPeerHistory(),
        )
        val row = view.connected.single()
        assertFalse(row.bonded)
        assertTrue(row.connected)
        assertTrue(view.unattributed.isEmpty())
    }

    @Test fun `controller classification, name and identity survive the split`() {
        val view = ControllerInventory.build(
            inventory(
                PeerInfo(
                    id = "p_pad",
                    address = "987A14C17834",
                    role = PeerRole.PhysicalController,
                    transports = setOf(PeerTransport.Classic, PeerTransport.Le),
                    bonded = true,
                    connected = true,
                    name = "Xbox Wireless Controller",
                    classification = "Xbox Wireless Controller (BLE)",
                    vendorId = 0x045E,
                    productId = 0x0B13,
                ),
            ),
            AdapterPeerHistory(),
        )
        val row = view.connected.single()
        assertEquals("Xbox Wireless Controller (BLE)", row.displayName)
        assertEquals(setOf(PeerTransport.Classic, PeerTransport.Le), row.transports)
        assertTrue(view.hasControllers)
    }


    @Test fun `an empty everything is empty`() {
        assertTrue(ControllerInventory.build(inventory(), AdapterPeerHistory()).isEmpty)
    }
}
