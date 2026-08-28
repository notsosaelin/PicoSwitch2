package dev.picoswitch.management

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The peer-inventory wire contract, from the client's side.
 *
 * Two properties are worth more than the rest here. A peer list that silently
 * loses a row shows the user fewer saved controllers than the adapter actually
 * holds, and they will act on that. And a management or Controller Link
 * relationship rendered as a controller is a row the user would eventually try
 * to forget — which is their own phone.
 */
class PeerInventoryTest {
    private fun page(vararg peers: String, total: Int = peers.size, next: String = "null") =
        """{"v":1,"total":$total,"peers":[${peers.joinToString(",")}],"next":$next}"""

    private fun peer(
        id: String,
        addr: String = "AABBCCDDEEFF",
        role: String = "controller",
        tr: Int = 1,
        conn: Boolean = false,
        name: String? = null,
        classification: String? = null,
        vid: Int? = null,
        pid: Int? = null,
    ) = buildString {
        append("""{"id":"$id","addr":"$addr","tr":$tr,"role":"$role","bonded":true,"conn":$conn""")
        if (name != null) append(""","name":"$name"""")
        if (classification != null) append(""","class":"$classification"""")
        if (vid != null && pid != null) append(""","vid":$vid,"pid":$pid""")
        append("}")
    }

    /* ------------------------------------------------------------- parsing */

    @Test fun `a page decodes identity, role, transport and live state`() {
        val decoded = ManagementProtocol.peersPage(
            "peers list",
            page(peer("p_1", role = "controller", tr = 1, conn = true, name = "DualSense")),
        )
        val entry = decoded.entries.single()
        assertEquals("p_1", entry.id)
        assertEquals(PeerRole.PhysicalController, entry.role)
        assertEquals(setOf(PeerTransport.Classic), entry.transports)
        assertTrue(entry.connected)
        assertTrue(entry.bonded)
        assertEquals("DualSense", entry.name)
        assertNull(decoded.next)
    }

    @Test fun `a peer with both key records is one row on two transports`() {
        // The management phone's normal shape once Controller Link has run.
        val decoded = ManagementProtocol.peersPage("peers list", page(peer("p_1", tr = 3)))
        val entry = decoded.entries.single()
        assertEquals(setOf(PeerTransport.Classic, PeerTransport.Le), entry.transports)
        assertTrue(entry.multiTransport)
    }

    @Test fun `an unrecognised role is unknown rather than a parse failure`() {
        // A newer adapter is allowed to know roles this build does not. Refusing
        // the page would hide every peer on it.
        val decoded = ManagementProtocol.peersPage("peers list", page(peer("p_1", role = "headset")))
        assertEquals(PeerRole.Unknown, decoded.entries.single().role)
    }

    @Test fun `an absent name is absent, not an empty string`() {
        val decoded = ManagementProtocol.peersPage("peers list", page(peer("p_1")))
        assertNull(decoded.entries.single().name)
    }

    @Test fun `a peer with no id is rejected`() {
        // The id is the app's only stable handle; a row without one cannot be
        // acted on later and must not be silently displayed as if it could.
        assertThrowsShape("""{"v":1,"total":1,"peers":[{"addr":"AA","tr":1}],"next":null}""")
        assertThrowsShape("""{"v":1,"total":1,"peers":[{"id":"","addr":"AA","tr":1}],"next":null}""")
    }

    @Test fun `a repeated id inside one page is rejected`() {
        assertThrowsShape(page(peer("p_1"), peer("p_1"), total = 2))
    }

    @Test fun `a wrong envelope version is rejected`() {
        assertThrowsShape("""{"v":2,"total":0,"peers":[],"next":null}""")
    }

    @Test fun `a missing next cursor is rejected`() {
        assertThrowsShape("""{"v":1,"total":0,"peers":[]}""")
    }

    @Test fun `a page claiming more entries than the total is rejected`() {
        assertThrowsShape(page(peer("p_1"), peer("p_2"), total = 1))
    }

    @Test fun `an empty page with a non-null cursor is rejected`() {
        // It would make a client loop on the same cursor forever.
        assertThrowsShape("""{"v":1,"total":3,"peers":[],"next":1}""")
    }

    @Test fun `an empty inventory is a valid complete page`() {
        val decoded = ManagementProtocol.peersPage("peers list", """{"v":1,"total":0,"peers":[],"next":null}""")
        assertTrue(decoded.entries.isEmpty())
        assertEquals(0, decoded.total)
        assertNull(decoded.next)
    }

    private fun assertThrowsShape(reply: String) {
        val error = runCatching { ManagementProtocol.peersPage("peers list", reply) }.exceptionOrNull()
        assertTrue("expected a protocol rejection for $reply, got $error", error is ManagementException)
    }

    /* ---------------------------------------------------------- pagination */

    @Test fun `pagination follows the cursor and assembles every peer once`() = runTest {
        val channel = ScriptedPeerChannel(
            "peers list" to page(peer("p_1"), peer("p_2"), total = 4, next = "2"),
            "peers list 2" to page(peer("p_3"), peer("p_4"), total = 4),
        )
        val inventory = ManagementClient(channel).listPeers()
        assertEquals(listOf("p_1", "p_2", "p_3", "p_4"), inventory.peers.map { it.id })
        assertEquals(4, inventory.total)
        assertTrue(inventory.complete)
        channel.assertDrained()
    }

    @Test fun `a total that changes mid-pagination is a failure, not a shorter list`() = runTest {
        val channel = ScriptedPeerChannel(
            "peers list" to page(peer("p_1"), total = 3, next = "1"),
            "peers list 1" to page(peer("p_2"), total = 2),
        )
        assertPaginationFailure { ManagementClient(channel).listPeers() }
    }

    @Test fun `a non-progressing cursor is a failure`() = runTest {
        val channel = ScriptedPeerChannel(
            "peers list" to page(peer("p_1"), total = 2, next = "0"),
        )
        assertPaginationFailure { ManagementClient(channel).listPeers() }
    }

    @Test fun `a peer repeated across pages is a failure`() = runTest {
        val channel = ScriptedPeerChannel(
            "peers list" to page(peer("p_1"), total = 2, next = "1"),
            "peers list 1" to page(peer("p_1"), total = 2),
        )
        assertPaginationFailure { ManagementClient(channel).listPeers() }
    }

    @Test fun `a final count short of the declared total is a failure`() = runTest {
        // The row that never arrived is a saved controller the user would
        // conclude is already gone.
        val channel = ScriptedPeerChannel("peers list" to page(peer("p_1"), total = 2))
        assertPaginationFailure { ManagementClient(channel).listPeers() }
    }

    private suspend fun assertPaginationFailure(block: suspend () -> Unit) {
        val error = runCatching { block() }.exceptionOrNull()
        assertTrue("expected a pagination failure, got $error", error is ManagementPaginationException)
    }

    /* ----------------------------------------------------------- filtering */

    @Test fun `this phone never appears in the controller list`() {
        val inventory = PeerInventory(
            peers = listOf(
                PeerInfo("p_m", "AA", PeerRole.ManagementCompanion, setOf(PeerTransport.Le), bonded = true),
                PeerInfo("p_l", "BB", PeerRole.ControllerLink, setOf(PeerTransport.Classic), bonded = true),
                PeerInfo("p_c", "CC", PeerRole.PhysicalController, setOf(PeerTransport.Classic), bonded = true),
                PeerInfo("p_u", "DD", PeerRole.Unknown, setOf(PeerTransport.Classic), bonded = true),
            ),
            complete = true,
            total = 4,
        )
        // The gate: the management bond is not presented as a controller.
        assertEquals(listOf("p_c"), inventory.controllers.map { it.id })
        // Unknown is also kept out of the controller list. It is not a claim
        // that it isn't one -- it is a refusal to claim that it is.
        assertEquals(listOf("p_m", "p_l", "p_u"), inventory.companionsAndUnknown.map { it.id })
    }

    /* --------------------------------------------- classification (Phase 4) */

    @Test fun `a classified peer decodes its derived identity`() {
        val entry = ManagementProtocol.peersPage(
            "peers list",
            page(peer("p_1", name = "Wireless Controller", classification = "Sony DualSense", vid = 1356, pid = 3302)),
        ).entries.single()
        assertEquals("Sony DualSense", entry.classification)
        assertEquals(1356, entry.vendorId)
        assertEquals(3302, entry.productId)
        assertTrue(entry.hasUsbIdentity)
    }

    @Test fun `an unclassified peer reports null rather than an empty label`() {
        val entry = ManagementProtocol.peersPage("peers list", page(peer("p_1"))).entries.single()
        // Absent must stay distinguishable from blank: one means the adapter
        // cannot say, the other would render as a name made of nothing.
        assertNull(entry.classification)
        assertFalse(entry.hasUsbIdentity)
    }

    @Test fun `a page from firmware that predates classification still decodes`() {
        // The envelope version did not move for the Phase 4 additions, so an
        // older adapter's page must remain readable rather than becoming a
        // version mismatch that hides every peer on it.
        val decoded = ManagementProtocol.peersPage(
            "peers list",
            """{"v":1,"total":1,"peers":[{"id":"p_1","addr":"AABBCCDDEEFF","tr":1,"role":"controller","bonded":true,"conn":false}],"next":null}""",
        )
        assertEquals(1, decoded.entries.size)
        assertNull(decoded.entries.single().classification)
    }

    /* ------------------------------------------------------------- naming */

    @Test fun `a user alias outranks everything the adapter can say`() {
        assertEquals(
            "Player two",
            PeerNaming.label(
                address = "AABBCCDDEEFF",
                alias = "Player two",
                classification = "Sony DualSense",
                name = "Wireless Controller",
                vendorId = 1356,
                productId = 3302,
            ),
        )
    }

    @Test fun `a derived classification outranks the name the device claims`() {
        // The remote name is whatever the device says it is and its owner can
        // change it; the classification is what this adapter worked out.
        assertEquals(
            "Sony DualSense",
            PeerNaming.label(
                address = "AABBCCDDEEFF",
                classification = "Sony DualSense",
                name = "definitely not a controller",
            ),
        )
    }

    @Test fun `a device with only a USB identity is named by it, not by its address`() {
        assertEquals(
            "Device 054C:0CE6",
            PeerNaming.label(address = "AABBCCDDEEFF", vendorId = 0x054C, productId = 0x0CE6),
        )
    }

    @Test fun `a peer the adapter cannot name at all falls back to a short suffix`() {
        val label = PeerNaming.label(address = "AABBCCDDEEFF")
        assertEquals("Controller • EEFF", label)
        // Never the bare address: an address rendered where a name belongs
        // reads as a name, and this one is not one.
        assertFalse(label.contains("AABBCC"))
    }

    @Test fun `a blank name is treated as no name at all`() {
        assertEquals("Controller • EEFF", PeerNaming.label(address = "AABBCCDDEEFF", alias = "  ", name = ""))
    }


    /* ------------------------------------------ selective forget (Phase 5) */

    @Test fun `a forget reply carries the adapter's verified state`() {
        val outcome = ManagementProtocol.peersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":true,"id":"p_5E6F7A8B","result":"removed","bonded":false,"tr":0}""",
        )
        assertEquals("p_5E6F7A8B", outcome.peerId)
        assertEquals(PeerForgetResult.Removed, outcome.result)
        assertFalse(outcome.stillBonded)
        assertTrue(outcome.transports.isEmpty())
        assertTrue(outcome.result.succeeded)
    }

    @Test fun `already absent is a success, not a failure`() {
        // A management reply can be lost after the command has already run. A
        // retry must not tell the user the forget failed.
        val outcome = ManagementProtocol.peersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":true,"id":"p_5E6F7A8B","result":"already_absent","bonded":false,"tr":0}""",
        )
        assertEquals(PeerForgetResult.AlreadyAbsent, outcome.result)
        assertTrue(outcome.result.succeeded)
    }

    @Test fun `a refused management peer is reported as such, not as a generic error`() {
        val outcome = ManagementProtocol.peersForget(
            "peers forget p_1A2B3C4D",
            """{"ok":false,"id":"p_1A2B3C4D","result":"management_peer","bonded":true,"tr":3}""",
        )
        assertEquals(PeerForgetResult.ManagementPeer, outcome.result)
        assertFalse(outcome.result.succeeded)
        assertTrue(outcome.stillBonded)
    }

    @Test fun `a partial delete surfaces as incomplete with what remains`() {
        val outcome = ManagementProtocol.peersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":false,"id":"p_5E6F7A8B","result":"incomplete","bonded":true,"tr":1}""",
        )
        assertEquals(PeerForgetResult.Incomplete, outcome.result)
        assertFalse(outcome.result.succeeded)
        assertEquals(setOf(PeerTransport.Classic), outcome.transports)
    }

    @Test fun `an unrecognised outcome still yields the verified bond state`() {
        // A newer adapter may name an outcome this build does not know.
        // Rejecting the reply would leave the client unable to say whether the
        // delete happened; `bonded` is the part that decides what is shown.
        val outcome = ManagementProtocol.peersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":false,"id":"p_5E6F7A8B","result":"deferred","bonded":true,"tr":2}""",
        )
        assertEquals(PeerForgetResult.Unknown, outcome.result)
        assertTrue(outcome.stillBonded)
        assertFalse(outcome.result.succeeded)
    }

    @Test fun `a forget reply without the verified state is rejected`() {
        // Without `bonded` there is nothing to trust over the app's optimism.
        assertThrowsForget("""{"ok":true,"id":"p_5E6F7A8B","result":"removed"}""")
        assertThrowsForget("""{"ok":true,"result":"removed","bonded":false}""")
        assertThrowsForget("""{"ok":true,"id":"","result":"removed","bonded":false}""")
    }

    @Test fun `the forget command names the peer by its opaque id`() {
        assertEquals("peers forget p_1A2B3C4D", ManagementCommands.peersForget("p_1A2B3C4D"))
        // The id is the adapter's, not something the client may compose.
        listOf("p_1a2b3c4d", "p_123", "1A2B3C4D", "", "p_1A2B3C4D ").forEach { bad ->
            assertThrows(IllegalArgumentException::class.java) {
                ManagementCommands.peersForget(bad)
            }
        }
    }

    private fun assertThrowsForget(reply: String) {
        val error = runCatching {
            ManagementProtocol.peersForget("peers forget p_5E6F7A8B", reply)
        }.exceptionOrNull()
        assertTrue("expected a protocol rejection for $reply, got $error", error is ManagementException)
    }


    @Test fun `no field on a peer can carry key material`() {
        // Structural: PeerInfo has no key field, so a reply cannot introduce one
        // without this test having to change.
        val fields = PeerInfo::class.java.declaredFields.map { it.name.lowercase() }
        assertFalse(fields.any { it.contains("key") || it.contains("ltk") || it.contains("irk") })
    }

    /* ----------------------------------------------------------- commands */

    @Test fun `the command spells its cursor as a peer index`() {
        assertEquals("peers list", ManagementCommands.peersPage())
        assertEquals("peers list 0", ManagementCommands.peersPage(0))
        assertEquals("peers list 7", ManagementCommands.peersPage(7))
        assertTrue(runCatching { ManagementCommands.peersPage(-1) }.isFailure)
    }
}

private class ScriptedPeerChannel(vararg exchanges: Pair<String, String>) : ManagementChannel {
    private val pending = ArrayDeque(exchanges.toList())

    override suspend fun transact(command: String, timeoutMillis: Long): String {
        val next = pending.removeFirstOrNull() ?: error("Unexpected command: $command")
        assertEquals(next.first, command)
        return next.second
    }

    fun assertDrained() = assertTrue("Unconsumed exchanges: $pending", pending.isEmpty())
}
