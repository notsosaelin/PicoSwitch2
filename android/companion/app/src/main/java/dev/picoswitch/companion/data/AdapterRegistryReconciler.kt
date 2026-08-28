package dev.picoswitch.companion.data

/** One app-owned CompanionDeviceManager record, reduced to what the registry needs. */
data class SystemCompanionAssociation(
    val associationId: Int?,
    val address: String,
    val displayName: String? = null,
)

data class AdapterRegistryReconciliation(
    val registry: AdapterRegistry,
    /** Association state per known adapter. Adapters absent from the map are [CompanionAssociationState.Missing]. */
    val states: Map<AdapterId, CompanionAssociationState>,
    /** Adapters adopted from an association this app owns but had no record for. */
    val adopted: List<AdapterId>,
    /** Whether anything worth persisting changed. */
    val changed: Boolean,
) {
    fun stateOf(id: AdapterId?): CompanionAssociationState =
        id?.let { states[it] } ?: CompanionAssociationState.Unknown
}

/**
 * Reconcile the app's registry against the CompanionDeviceManager records this
 * app owns.
 *
 * WHY THIS REPLACED A SINGLE-RELATIONSHIP RECONCILER
 *
 * The previous `AdapterRelationshipReconciler` answered "which one adapter is
 * mine?", and therefore had to treat **more than one association as an error**:
 * two records meant `Ambiguous`, which the lifecycle mapped to
 * `RepairRequired`, whose repair flow deleted every app-owned association. That
 * is exactly the state a user with two adapters is permanently in, and it is
 * the reason owning two adapters previously required a Forget/Pair cycle.
 *
 * Plurality is now the normal case. What survives from the old rule is the part
 * that was actually right: **never silently pick one of several as the active
 * adapter.** Adoption creates records; it does not make a selection. The one
 * exception is a registry that ends up holding exactly one adapter and no
 * selection, which cannot be ambiguous.
 *
 * `Ambiguous` is redefined rather than removed. It now means what its name
 * always implied: two associations claim the *same* adapter identity, so which
 * association describes it cannot be decided from the records alone.
 *
 * Absent associations never delete a record. An adapter that is powered off, or
 * whose association the user removed in system settings, keeps its alias and
 * cached state — losing a user's name for their hardware because it was
 * unplugged would be its own defect.
 */
object AdapterRegistryReconciler {
    fun reconcile(
        registry: AdapterRegistry,
        associations: List<SystemCompanionAssociation>,
    ): AdapterRegistryReconciliation {
        val byIdentity = associations
            .mapNotNull { association ->
                AdapterId.fromAddress(association.address)?.let { it to association }
            }
            .groupBy({ it.first }, { it.second })

        var next = registry
        val states = mutableMapOf<AdapterId, CompanionAssociationState>()
        val adopted = mutableListOf<AdapterId>()

        for ((id, candidates) in byIdentity) {
            val existing = next.record(id)
            val chosen = choose(candidates, existing?.associationId)
            val merged = (existing ?: AdapterRecord(id = id, address = id.value)).copy(
                associationId = chosen.associationId ?: existing?.associationId,
                lastKnownName = AdapterAlias.sanitize(chosen.displayName)
                    ?: existing?.lastKnownName
                    ?: AdapterRecord.DEFAULT_PRODUCT_NAME,
            )
            if (existing == null) adopted += id
            next = next.with(merged)
            states[id] = if (candidates.size > 1) {
                CompanionAssociationState.Ambiguous
            } else {
                CompanionAssociationState.Present
            }
        }

        next.records.forEach { record ->
            states.putIfAbsent(record.id, CompanionAssociationState.Missing)
        }

        // A single known adapter and no selection is not a choice the user needs
        // to make. Two or more is, and this must not make it for them.
        if (next.activeId == null && next.records.size == 1) {
            next = next.selecting(next.records.single().id)
        }
        next = next.selecting(next.activeId)

        return AdapterRegistryReconciliation(
            registry = next,
            states = states,
            adopted = adopted,
            changed = next != registry,
        )
    }

    /**
     * The stored association ID stays authoritative when it is still present.
     *
     * Otherwise pick deterministically — an arbitrary choice that changes
     * between launches would make the same hardware look like different
     * adapters from one reconciliation to the next.
     */
    private fun choose(
        candidates: List<SystemCompanionAssociation>,
        storedAssociationId: Int?,
    ): SystemCompanionAssociation =
        candidates.firstOrNull { it.associationId != null && it.associationId == storedAssociationId }
            ?: candidates.minByOrNull { it.associationId ?: Int.MAX_VALUE }
            ?: candidates.first()
}
