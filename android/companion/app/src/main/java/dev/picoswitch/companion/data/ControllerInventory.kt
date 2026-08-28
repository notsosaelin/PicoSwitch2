package dev.picoswitch.companion.data

import dev.picoswitch.management.PeerInfo
import dev.picoswitch.management.PeerInventory
import dev.picoswitch.management.PeerNaming
import dev.picoswitch.management.PeerRole
import dev.picoswitch.management.PeerTransport

/**
 * Where one peer belongs on the Paired controllers card.
 *
 * The sections are decided by BOND AND CONNECTION STATE, not by role, because
 * those are the two questions the user is actually asking: is it working right
 * now, is it still paired, or is it merely something this adapter used to know.
 * Role only decides one thing here — whether the row is a controller at all, or
 * the user's own phone, which never belongs in a controller list.
 */
enum class PeerSection {
    /** Connected to the adapter right now. */
    Connected,

    /** The adapter holds a security record; the controller is simply not here. */
    Saved,

    /** No security record any more. Remembered by this app, not by the adapter. */
    Recent,

    /** This phone, in either of its relationships. Never offered as a controller. */
    Companion,
}

/**
 * One row, resolved from the adapter's live answer and this app's memory.
 *
 * [role] is always the adapter's live classification and is never rewritten;
 * [rememberedRole] is what this app has previously seen proven. When they differ
 * the UI says so rather than presenting memory as current fact — the management
 * protocol requires that `unknown` be rendered as unidentified, and it is.
 */
data class PeerListing(
    val peerId: String,
    val address: String,
    val displayName: String,
    val section: PeerSection,
    /** The adapter's live answer, verbatim. */
    val role: PeerRole,
    /** The strongest role this app has ever seen the adapter prove. */
    val rememberedRole: PeerRole,
    val transports: Set<PeerTransport>,
    val connected: Boolean,
    val bonded: Boolean,
    val classification: String? = null,
    val lastConnectedAtMillis: Long? = null,
    /**
     * The adapter could not identify this peer and the label came from memory.
     * Presentation MUST attribute it; a remembered identity shown as a live one
     * is exactly the promotion the protocol forbids.
     */
    val identifiedFromHistory: Boolean = false,
    /** True for a row that exists only in app history, whose only action is to forget it. */
    val historyOnly: Boolean = false,
)

data class ControllerInventoryView(
    val connected: List<PeerListing> = emptyList(),
    val saved: List<PeerListing> = emptyList(),
    val recent: List<PeerListing> = emptyList(),
    val companion: List<PeerListing> = emptyList(),
) {
    val isEmpty: Boolean
        get() = connected.isEmpty() && saved.isEmpty() && recent.isEmpty() && companion.isEmpty()
}

/**
 * Build the Paired controllers view from what the adapter reports and what this
 * app remembers.
 *
 * The adapter's inventory is authoritative about existence, bonding and live
 * connection. History contributes exactly two things: a readable label for a
 * peer the adapter cannot currently identify, and the rows for peers the
 * adapter no longer holds a key for at all.
 */
object ControllerInventory {
    fun build(inventory: PeerInventory, history: AdapterPeerHistory): ControllerInventoryView {
        val live = inventory.peers.map { peer -> listing(peer, history.record(peer.id)) }
        val liveIds = inventory.peers.mapTo(mutableSetOf()) { it.id }
        val recent = history.forgotten
            .filterNot { it.peerId in liveIds }
            // A phone this app once managed the adapter with is not a controller
            // the user forgot; it would read as one under "Recent".
            .filterNot { it.isCompanionRole }
            .map(::historyListing)
            .sortedByDescending { it.lastConnectedAtMillis ?: 0 }

        return ControllerInventoryView(
            connected = live.filter { it.section == PeerSection.Connected }.sortedBy { it.displayName },
            saved = live.filter { it.section == PeerSection.Saved }.sortedBy { it.displayName },
            recent = recent,
            companion = live.filter { it.section == PeerSection.Companion }.sortedBy { it.displayName },
        )
    }

    private fun listing(peer: PeerInfo, remembered: PeerHistoryRecord?): PeerListing {
        val rememberedRole = remembered?.provenRole ?: PeerRole.Unknown
        // Companion membership uses the strongest evidence from either source.
        // Excluding the user's own phone from the controller list on remembered
        // evidence is safe in the direction that matters: the cost of being
        // wrong is a phone shown under "This phone", and the cost of the
        // opposite mistake is offering to forget the management relationship.
        val effectiveRole = strongerRole(peer.role, rememberedRole)
        val companion = effectiveRole == PeerRole.ManagementCompanion ||
            effectiveRole == PeerRole.ControllerLink
        val section = when {
            companion -> PeerSection.Companion
            peer.connected -> PeerSection.Connected
            else -> PeerSection.Saved
        }
        val liveName = PeerNaming.label(
            address = peer.address,
            classification = peer.classification,
            name = peer.name,
            vendorId = peer.vendorId,
            productId = peer.productId,
        )
        // Fall back to memory only when the adapter offered nothing at all, so a
        // name the adapter can currently see always wins over a stale one.
        val adapterNamedIt = peer.classification != null || peer.name != null || peer.hasUsbIdentity
        val rememberedName = remembered?.rememberedName
        return PeerListing(
            peerId = peer.id,
            address = peer.address,
            displayName = if (adapterNamedIt || rememberedName == null) liveName else rememberedName,
            section = section,
            role = peer.role,
            rememberedRole = rememberedRole,
            transports = peer.transports,
            connected = peer.connected,
            bonded = peer.bonded,
            classification = peer.classification ?: remembered?.classification,
            lastConnectedAtMillis = remembered?.lastConnectedAtMillis,
            identifiedFromHistory = !adapterNamedIt && rememberedName != null,
        )
    }

    private fun historyListing(record: PeerHistoryRecord) = PeerListing(
        peerId = record.peerId,
        address = record.address,
        displayName = record.rememberedName,
        section = PeerSection.Recent,
        // The adapter has no opinion at all about a peer it no longer stores.
        role = PeerRole.Unknown,
        rememberedRole = record.provenRole,
        transports = record.transports,
        connected = false,
        bonded = false,
        classification = record.classification,
        lastConnectedAtMillis = record.lastConnectedAtMillis,
        identifiedFromHistory = true,
        historyOnly = true,
    )
}
