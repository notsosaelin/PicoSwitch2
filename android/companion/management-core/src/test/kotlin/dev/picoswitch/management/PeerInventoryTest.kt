package dev.picoswitch.management

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
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
    ) = buildString {
        append("""{"id":"$id","addr":"$addr","tr":$tr,"role":"$role","bonded":true,"conn":$conn""")
        if (name != null) append(""","name":"$name"""")
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
