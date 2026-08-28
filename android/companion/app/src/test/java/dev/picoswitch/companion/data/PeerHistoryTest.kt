package dev.picoswitch.companion.data

import dev.picoswitch.management.PeerInfo
import dev.picoswitch.management.PeerInventory
import dev.picoswitch.management.PeerRole
import dev.picoswitch.management.PeerTransport
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Per-adapter controller history.
 *
 * The properties worth pinning are the ones where being wrong misleads the user
 * about their own hardware:
 *
 *  * an adapter that reboots and reports `unknown` must not erase what it once
 *    proved, or every saved controller loses its name after a power cycle;
 *  * a peer that disappears from a COMPLETE inventory has genuinely been
 *    unpaired and must move to Recent, because leaving it under Saved claims a
 *    pairing that no longer exists;
 *  * a partial read must never be recorded at all, because it is
 *    indistinguishable from the previous case and would unpair things on screen
 *    that were never unpaired;
 *  * the user's own phone must never end up in the controller list.
 */
class PeerHistoryTest {
    private fun peer(
        id: String,
        address: String = "AABBCCDDEE01",
        role: PeerRole = PeerRole.PhysicalController,
        transports: Set<PeerTransport> = setOf(PeerTransport.Classic),
        bonded: Boolean = true,
        connected: Boolean = false,
        name: String? = null,
        classification: String? = null,
        vendorId: Int = 0,
        productId: Int = 0,
    ) = PeerInfo(
        id = id,
        address = address,
        role = role,
        transports = transports,
        bonded = bonded,
        connected = connected,
        name = name,
        classification = classification,
        vendorId = vendorId,
        productId = productId,
    )

    private fun inventory(vararg peers: PeerInfo) =
        PeerInventory(peers.toList(), complete = true, total = peers.size)

    /* ------------------------------------------------------------ recording */

    @Test fun `a first inventory read remembers what the adapter reported`() {
        val history = AdapterPeerHistory().observing(
            inventory(peer("p_1", name = "Wireless Controller", classification = "Sony DualSense", connected = true)),
            nowMillis = 1_000,
        )
        val record = history.record("p_1")!!
        assertEquals("Sony DualSense", record.classification)
        assertEquals("Wireless Controller", record.lastKnownName)
        assertEquals(PeerRole.PhysicalController, record.provenRole)
        assertEquals(1_000L, record.firstSeenAtMillis)
        assertEquals(1_000L, record.lastConnectedAtMillis)
        assertTrue(record.bonded)
    }

    @Test fun `a reboot that reports unknown does not erase what was proven`() {
        // This is the whole reason history exists. The adapter classifies from
        // live evidence only, so after a power cycle a saved controller that has
        // not reconnected is reported unknown and nameless.
        val first = AdapterPeerHistory().observing(
            inventory(peer("p_1", classification = "Sony DualSense", connected = true)),
            nowMillis = 1_000,
        )
        val after = first.observing(
            inventory(peer("p_1", role = PeerRole.Unknown, classification = null, name = null)),
            nowMillis = 2_000,
        )
        val record = after.record("p_1")!!
        assertEquals("Sony DualSense", record.classification)
        assertEquals(PeerRole.PhysicalController, record.provenRole)
        // The connection timestamp does not advance for a peer that is merely
        // still bonded; only an actual connection moves it.
        assertEquals(1_000L, record.lastConnectedAtMillis)
        assertEquals(2_000L, record.lastSeenAtMillis)
    }

    @Test fun `a renamed controller takes the newer name`() {
        val first = AdapterPeerHistory().observing(inventory(peer("p_1", name = "Old")), 1_000)
        val after = first.observing(inventory(peer("p_1", name = "New")), 2_000)
        assertEquals("New", after.record("p_1")!!.lastKnownName)
    }

    @Test fun `a peer absent from a complete read stops being a saved pairing`() {
        val first = AdapterPeerHistory().observing(inventory(peer("p_1", name = "Pad")), 1_000)
        val after = first.observing(inventory(), 2_000)
        val record = after.record("p_1")!!
        assertFalse(record.bonded)
        // Kept, not deleted: knowing what an adapter used to have is the point.
        assertEquals("Pad", record.lastKnownName)
        assertEquals(listOf("p_1"), after.forgotten.map { it.peerId })
    }

    @Test fun `an incomplete read is refused rather than recorded`() {
        // Recording one would mark every unread peer as unpaired, telling the
        // user controllers were forgotten when nothing happened at all.
        val partial = PeerInventory(listOf(peer("p_1")), complete = false, total = 4)
        val error = runCatching { AdapterPeerHistory().observing(partial, 1_000) }.exceptionOrNull()
        assertTrue("expected a refusal, got $error", error is IllegalArgumentException)
    }

    @Test fun `transports accumulate across sessions`() {
        val first = AdapterPeerHistory().observing(
            inventory(peer("p_1", transports = setOf(PeerTransport.Classic))),
            1_000,
        )
        val after = first.observing(
            inventory(peer("p_1", transports = setOf(PeerTransport.Le))),
            2_000,
        )
        assertEquals(
            setOf(PeerTransport.Classic, PeerTransport.Le),
            after.record("p_1")!!.transports,
        )
    }

    @Test fun `the strongest role ever proven wins, whichever order it arrived in`() {
        val phoneFirst = AdapterPeerHistory()
            .observing(inventory(peer("p_1", role = PeerRole.ManagementCompanion)), 1_000)
            .observing(inventory(peer("p_1", role = PeerRole.ControllerLink)), 2_000)
        assertEquals(PeerRole.ManagementCompanion, phoneFirst.record("p_1")!!.provenRole)

        val linkFirst = AdapterPeerHistory()
            .observing(inventory(peer("p_1", role = PeerRole.ControllerLink)), 1_000)
            .observing(inventory(peer("p_1", role = PeerRole.ManagementCompanion)), 2_000)
        assertEquals(PeerRole.ManagementCompanion, linkFirst.record("p_1")!!.provenRole)
    }

    @Test fun `history is bounded and never evicts something still paired`() {
        var history = AdapterPeerHistory()
        // Fill well past the cap with peers that then all get unpaired.
        repeat(AdapterPeerHistory.MAX_RECORDS + 20) { index ->
            history = history.observing(inventory(peer("gone_$index")), (index + 1).toLong())
        }
        history = history.observing(inventory(peer("kept")), 10_000)
        assertTrue(history.records.size <= AdapterPeerHistory.MAX_RECORDS)
        assertNotNull("a still-bonded peer must never be evicted", history.record("kept"))
        assertTrue(history.record("kept")!!.bonded)
    }

    @Test fun `removing one row leaves the rest alone`() {
        val history = AdapterPeerHistory()
            .observing(inventory(peer("p_1"), peer("p_2", address = "AABBCCDDEE02")), 1_000)
            .without("p_1")
        assertNull(history.record("p_1"))
        assertNotNull(history.record("p_2"))
    }

    /* --------------------------------------------------------------- naming */

    @Test fun `a remembered name prefers the classification over the device's own claim`() {
        val record = AdapterPeerHistory()
            .observing(inventory(peer("p_1", name = "Wireless Controller", classification = "Sony DualSense")), 1_000)
            .record("p_1")!!
        assertEquals("Sony DualSense", record.rememberedName)
    }

    @Test fun `a peer with nothing to say falls back to a short suffix`() {
        val record = AdapterPeerHistory()
            .observing(inventory(peer("p_1", address = "AABBCCDDEEFF")), 1_000)
            .record("p_1")!!
        assertEquals("Controller • EEFF", record.rememberedName)
    }

    /* ------------------------------------------------------------ the book */

    @Test fun `history is per adapter and removing one adapter leaves the other`() {
        val a = AdapterId("AA:BB:CC:DD:EE:01")
        val b = AdapterId("AA:BB:CC:DD:EE:02")
        val book = PeerHistoryBook()
            .with(a, AdapterPeerHistory().observing(inventory(peer("p_a")), 1_000))
            .with(b, AdapterPeerHistory().observing(inventory(peer("p_b")), 1_000))
        val after = book.without(a)
        assertTrue(after.forAdapter(a).records.isEmpty())
        assertEquals(listOf("p_b"), after.forAdapter(b).records.map { it.peerId })
    }

    /* --------------------------------------------------------------- codec */

    @Test fun `a document survives a round trip intact`() {
        val id = AdapterId("AA:BB:CC:DD:EE:01")
        val book = PeerHistoryBook().with(
            id,
            AdapterPeerHistory().observing(
                inventory(
                    peer("p_1", name = "Pad", classification = "Sony DualSense", vendorId = 0x054C, productId = 0x0CE6, connected = true),
                    peer("p_2", address = "AABBCCDDEE02", role = PeerRole.ManagementCompanion, transports = setOf(PeerTransport.Le)),
                ),
                1_000,
            ),
        )
        val decoded = PeerHistoryCodec.decode(PeerHistoryCodec.encode(book))
        assertEquals(book, decoded)
    }

    @Test fun `an unreadable document costs history, never the launch`() {
        assertEquals(PeerHistoryBook(), PeerHistoryCodec.decode("not json at all"))
        assertEquals(PeerHistoryBook(), PeerHistoryCodec.decode(null))
        assertEquals(PeerHistoryBook(), PeerHistoryCodec.decode("{}"))
    }

    @Test fun `a future schema is refused rather than half-read`() {
        val text = PeerHistoryCodec.encode(
            PeerHistoryBook().with(AdapterId("AA:BB:CC:DD:EE:01"), AdapterPeerHistory()),
        ).replace("\"schema\":1", "\"schema\":99")
        assertEquals(PeerHistoryBook(), PeerHistoryCodec.decode(text))
    }

    @Test fun `one unreadable row does not cost the whole adapter`() {
        val id = AdapterId("AA:BB:CC:DD:EE:01")
        val good = PeerHistoryCodec.encode(
            PeerHistoryBook().with(id, AdapterPeerHistory().observing(inventory(peer("p_1", name = "Pad")), 1_000)),
        )
        // Splice in a row with no id, which is the one field nothing can work
        // without.
        val damaged = good.replace("\"peers\":[", "\"peers\":[{\"addr\":\"AA\"},")
        val decoded = PeerHistoryCodec.decode(damaged)
        assertEquals(listOf("p_1"), decoded.forAdapter(id).records.map { it.peerId })
    }

    @Test fun `names are re-sanitised on the way back off disk`() {
        // These strings started life as remote Bluetooth names. That they entered
        // clean is not proof the bytes on disk still are, and they go straight
        // back into the UI and the diagnostic log.
        val id = AdapterId("AA:BB:CC:DD:EE:01")
        val encoded = PeerHistoryCodec.encode(
            PeerHistoryBook().with(id, AdapterPeerHistory().observing(inventory(peer("p_1", name = "Pad")), 1_000)),
        ).replace("\"name\":\"Pad\"", "\"name\":\"Bad\\nInjected\"")
        val record = PeerHistoryCodec.decode(encoded).forAdapter(id).record("p_1")!!
        assertEquals("Bad Injected", record.lastKnownName)
    }

    private fun assertNotNull(message: String, value: Any?) = assertTrue(message, value != null)
    private fun assertNotNull(value: Any?) = assertTrue(value != null)
}
