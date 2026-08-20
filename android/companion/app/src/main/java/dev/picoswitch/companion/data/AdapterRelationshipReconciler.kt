package dev.picoswitch.companion.data

data class SystemCompanionAssociation(
    val associationId: Int?,
    val address: String,
    val displayName: String? = null,
)

data class AdapterRelationshipReconciliation(
    val relationship: AdapterRelationship?,
    val associationState: CompanionAssociationState,
    val source: Source,
) {
    enum class Source { ExactAssociationId, AddressMatch, OnlyAssociation, SavedWithoutAssociation, None, Ambiguous }
}

/** Deterministic reconciliation of the app's one saved adapter with app-owned CDM records. */
object AdapterRelationshipReconciler {
    fun reconcile(
        saved: AdapterRelationship?,
        associations: List<SystemCompanionAssociation>,
    ): AdapterRelationshipReconciliation {
        if (saved != null) {
            saved.associationId?.let { id ->
                associations.singleOrNull { it.associationId == id }?.let { association ->
                    return AdapterRelationshipReconciliation(
                        relationship = association.toRelationship(saved),
                        associationState = CompanionAssociationState.Present,
                        source = AdapterRelationshipReconciliation.Source.ExactAssociationId,
                    )
                }
            }
            associations.singleOrNull { it.address.equals(saved.address, true) }?.let { association ->
                return AdapterRelationshipReconciliation(
                    relationship = association.toRelationship(saved),
                    associationState = CompanionAssociationState.Present,
                    source = AdapterRelationshipReconciliation.Source.AddressMatch,
                )
            }
            return AdapterRelationshipReconciliation(
                relationship = saved,
                associationState = CompanionAssociationState.Missing,
                source = AdapterRelationshipReconciliation.Source.SavedWithoutAssociation,
            )
        }

        if (associations.size == 1) {
            return AdapterRelationshipReconciliation(
                relationship = associations.single().toRelationship(null),
                associationState = CompanionAssociationState.Present,
                source = AdapterRelationshipReconciliation.Source.OnlyAssociation,
            )
        }
        if (associations.size > 1) {
            return AdapterRelationshipReconciliation(
                relationship = null,
                associationState = CompanionAssociationState.Ambiguous,
                source = AdapterRelationshipReconciliation.Source.Ambiguous,
            )
        }
        return AdapterRelationshipReconciliation(
            relationship = null,
            associationState = CompanionAssociationState.Missing,
            source = AdapterRelationshipReconciliation.Source.None,
        )
    }

    private fun SystemCompanionAssociation.toRelationship(saved: AdapterRelationship?) = AdapterRelationship(
        address = address.uppercase(),
        associationId = associationId ?: saved?.associationId,
        displayName = displayName?.takeIf(String::isNotBlank) ?: saved?.displayName ?: "PicoSwitch2",
    )
}
