package dev.picoswitch.companion.data

/** Product-level Android truth; deliberately independent of Bluetooth framework constants. */
enum class AndroidBondState { Unknown, None, Bonding, Bonded }

enum class AdapterRelationshipPhase {
    NoRelationship,
    Idle,
    Associating,
    Bonding,
    Connecting,
    Connected,
    Failed,
    RepairRequired,
}

enum class AdapterConnectReason(val diagnosticName: String) {
    ForegroundAuto("foreground-auto"),
    Manual("manual"),
    AfterBond("after-bond"),
}

data class AdapterRelationshipStatus(
    val phase: AdapterRelationshipPhase,
    val generation: Long = 0,
    val reason: AdapterConnectReason? = null,
    val bond: AndroidBondState = AndroidBondState.Unknown,
    val companionAssociation: CompanionAssociationState = CompanionAssociationState.Unknown,
    val message: String? = null,
) {
    val attemptActive: Boolean get() = phase == AdapterRelationshipPhase.Associating ||
        phase == AdapterRelationshipPhase.Bonding || phase == AdapterRelationshipPhase.Connecting
}

enum class CompanionAssociationState { Unknown, Missing, Present, Ambiguous }

data class AdapterConnectionAttempt(
    val generation: Long,
    val reason: AdapterConnectReason,
    val relationship: AdapterRelationship,
)

sealed interface AdapterLifecycleDecision {
    data object Ignored : AdapterLifecycleDecision
    data class AwaitBond(val attempt: AdapterConnectionAttempt, val startBond: Boolean) : AdapterLifecycleDecision
    data class Connect(val attempt: AdapterConnectionAttempt) : AdapterLifecycleDecision
    data class RelationshipMetadataUpdated(val relationship: AdapterRelationship) : AdapterLifecycleDecision
    data class RepairRequired(val message: String) : AdapterLifecycleDecision
}

/**
 * Single owner for app-level association, bond, and management-connect progression.
 *
 * Android may deliver one association through both the CDM callback and Activity result. It may
 * also deliver bond broadcasts while the Activity is re-entering the foreground. Every entry point
 * is reduced here so only the current generation can advance and duplicate callbacks are inert.
 */
class AdapterRelationshipCoordinator(initialRelationship: AdapterRelationship?) {
    private var generation = 0L
    private var savedRelationship = initialRelationship
    private var candidate: AdapterRelationship? = null
    private var activeAttempt: AdapterConnectionAttempt? = null

    var status: AdapterRelationshipStatus = AdapterRelationshipStatus(
        phase = if (initialRelationship == null) AdapterRelationshipPhase.NoRelationship else AdapterRelationshipPhase.Idle,
    )
        private set

    @Synchronized
    fun beginAssociation(): Long {
        generation += 1
        candidate = null
        activeAttempt = null
        status = AdapterRelationshipStatus(
            phase = AdapterRelationshipPhase.Associating,
            generation = generation,
            message = "Choose PicoSwitch2 in Android's secure pairing screen.",
        )
        return generation
    }

    @Synchronized
    fun associationCreated(
        associationGeneration: Long,
        relationship: AdapterRelationship,
        bond: AndroidBondState,
    ): AdapterLifecycleDecision {
        if (associationGeneration != generation || status.phase != AdapterRelationshipPhase.Associating) {
            // The second API-33 completion is expected. It may improve the association ID, but it
            // must never start a second bond or GATT connection.
            if (associationGeneration == generation && candidate?.address.equals(relationship.address, true)) {
                candidate = merge(candidate!!, relationship)
                if (status.phase == AdapterRelationshipPhase.Connected) {
                    savedRelationship = candidate
                    return AdapterLifecycleDecision.RelationshipMetadataUpdated(candidate!!)
                }
            }
            return AdapterLifecycleDecision.Ignored
        }

        candidate = relationship
        val attempt = AdapterConnectionAttempt(generation, AdapterConnectReason.AfterBond, relationship)
        activeAttempt = attempt
        return when (bond) {
            AndroidBondState.Bonded -> {
                status = status.copy(
                    phase = AdapterRelationshipPhase.Connecting,
                    reason = attempt.reason,
                    bond = bond,
                    companionAssociation = CompanionAssociationState.Present,
                    message = "Secure pairing complete; connecting management.",
                )
                AdapterLifecycleDecision.Connect(attempt)
            }
            AndroidBondState.None, AndroidBondState.Bonding -> {
                status = status.copy(
                    phase = AdapterRelationshipPhase.Bonding,
                    reason = attempt.reason,
                    bond = bond,
                    companionAssociation = CompanionAssociationState.Present,
                    message = "Waiting for Android secure pairing to complete.",
                )
                AdapterLifecycleDecision.AwaitBond(attempt, startBond = bond == AndroidBondState.None)
            }
            AndroidBondState.Unknown -> {
                val message = "Android's Bluetooth pairing state could not be read."
                status = status.copy(
                    phase = AdapterRelationshipPhase.Failed,
                    bond = bond,
                    companionAssociation = CompanionAssociationState.Present,
                    message = message,
                )
                activeAttempt = null
                AdapterLifecycleDecision.RepairRequired(message)
            }
        }
    }

    @Synchronized
    fun bondChanged(address: String, bond: AndroidBondState): AdapterLifecycleDecision {
        val attempt = activeAttempt
        if (attempt == null) {
            if (savedRelationship?.address.equals(address, ignoreCase = true) && bond == AndroidBondState.None) {
                val message = "Android no longer has a Bluetooth bond for this saved adapter."
                status = status.copy(
                    phase = AdapterRelationshipPhase.RepairRequired,
                    bond = bond,
                    message = message,
                )
                return AdapterLifecycleDecision.RepairRequired(message)
            }
            return AdapterLifecycleDecision.Ignored
        }
        if (status.phase != AdapterRelationshipPhase.Bonding ||
            !attempt.relationship.address.equals(address, ignoreCase = true)
        ) return AdapterLifecycleDecision.Ignored

        status = status.copy(bond = bond)
        return when (bond) {
            AndroidBondState.Bonded -> {
                status = status.copy(
                    phase = AdapterRelationshipPhase.Connecting,
                    message = "Secure pairing complete; connecting management.",
                )
                AdapterLifecycleDecision.Connect(attempt)
            }
            AndroidBondState.None -> {
                val message = "Android pairing did not complete. Open the adapter pairing window and try Repair pairing."
                status = status.copy(phase = AdapterRelationshipPhase.RepairRequired, message = message)
                activeAttempt = null
                AdapterLifecycleDecision.RepairRequired(message)
            }
            AndroidBondState.Bonding, AndroidBondState.Unknown -> AdapterLifecycleDecision.Ignored
        }
    }

    @Synchronized
    fun requestReconnect(
        relationship: AdapterRelationship,
        reason: AdapterConnectReason,
        bond: AndroidBondState,
    ): AdapterLifecycleDecision {
        if (status.attemptActive || status.phase == AdapterRelationshipPhase.Connected) {
            return AdapterLifecycleDecision.Ignored
        }
        generation += 1
        val attempt = AdapterConnectionAttempt(generation, reason, relationship)
        activeAttempt = attempt
        candidate = relationship
        return when (bond) {
            AndroidBondState.Bonded -> {
                status = AdapterRelationshipStatus(
                    phase = AdapterRelationshipPhase.Connecting,
                    generation = generation,
                    reason = reason,
                    bond = bond,
                    companionAssociation = status.companionAssociation,
                    message = "Connecting to ${relationship.displayName}.",
                )
                AdapterLifecycleDecision.Connect(attempt)
            }
            AndroidBondState.Bonding -> {
                status = AdapterRelationshipStatus(
                    phase = AdapterRelationshipPhase.Bonding,
                    generation = generation,
                    reason = reason,
                    bond = bond,
                    companionAssociation = status.companionAssociation,
                    message = "Waiting for Android secure pairing to complete.",
                )
                AdapterLifecycleDecision.AwaitBond(attempt, startBond = false)
            }
            AndroidBondState.None -> {
                val message = "Android no longer has a Bluetooth bond for this saved adapter."
                status = AdapterRelationshipStatus(
                    phase = AdapterRelationshipPhase.RepairRequired,
                    generation = generation,
                    reason = reason,
                    bond = bond,
                    companionAssociation = status.companionAssociation,
                    message = message,
                )
                activeAttempt = null
                AdapterLifecycleDecision.RepairRequired(message)
            }
            AndroidBondState.Unknown -> {
                val message = "Android's Bluetooth pairing state could not be read."
                status = AdapterRelationshipStatus(
                    phase = AdapterRelationshipPhase.Failed,
                    generation = generation,
                    reason = reason,
                    bond = bond,
                    companionAssociation = status.companionAssociation,
                    message = message,
                )
                activeAttempt = null
                AdapterLifecycleDecision.RepairRequired(message)
            }
        }
    }

    @Synchronized
    fun connectionSucceeded(connectionGeneration: Long): AdapterRelationship? {
        val attempt = activeAttempt ?: return null
        if (attempt.generation != connectionGeneration || status.phase != AdapterRelationshipPhase.Connecting) return null
        val verified = candidate ?: attempt.relationship
        savedRelationship = verified
        activeAttempt = null
        status = status.copy(phase = AdapterRelationshipPhase.Connected, message = null)
        return verified
    }

    @Synchronized
    fun connectionFailed(connectionGeneration: Long, message: String) {
        val attempt = activeAttempt ?: return
        if (attempt.generation != connectionGeneration) return
        activeAttempt = null
        status = status.copy(phase = AdapterRelationshipPhase.Failed, message = message)
    }

    /** A verified session ended without deleting any relationship truth. */
    @Synchronized
    fun connectionEnded(message: String? = null): Boolean {
        if (status.phase != AdapterRelationshipPhase.Connected) return false
        generation += 1
        activeAttempt = null
        candidate = null
        status = status.copy(
            phase = if (savedRelationship == null) AdapterRelationshipPhase.NoRelationship else AdapterRelationshipPhase.Idle,
            generation = generation,
            reason = null,
            message = message,
        )
        return true
    }

    @Synchronized
    fun associationFailed(associationGeneration: Long, message: String) {
        if (associationGeneration != generation || status.phase != AdapterRelationshipPhase.Associating) return
        activeAttempt = null
        candidate = null
        status = status.copy(
            phase = if (savedRelationship == null) AdapterRelationshipPhase.NoRelationship else AdapterRelationshipPhase.Idle,
            message = message,
        )
    }

    @Synchronized
    fun updateAssociationState(state: CompanionAssociationState) {
        status = status.copy(companionAssociation = state)
    }

    @Synchronized
    fun cancelAndRetainRelationship(message: String? = null): Long {
        generation += 1
        activeAttempt = null
        candidate = null
        status = AdapterRelationshipStatus(
            phase = if (savedRelationship == null) AdapterRelationshipPhase.NoRelationship else AdapterRelationshipPhase.Idle,
            generation = generation,
            companionAssociation = status.companionAssociation,
            message = message,
        )
        return generation
    }

    @Synchronized
    fun forget() {
        generation += 1
        savedRelationship = null
        candidate = null
        activeAttempt = null
        status = AdapterRelationshipStatus(
            phase = AdapterRelationshipPhase.NoRelationship,
            generation = generation,
            companionAssociation = CompanionAssociationState.Missing,
        )
    }

    @Synchronized
    fun restore(
        relationship: AdapterRelationship?,
        associationState: CompanionAssociationState,
        bond: AndroidBondState = AndroidBondState.Unknown,
    ) {
        savedRelationship = relationship
        status = status.copy(
            phase = when {
                status.attemptActive -> status.phase
                status.phase == AdapterRelationshipPhase.Connected && relationship != null -> AdapterRelationshipPhase.Connected
                relationship == null && associationState == CompanionAssociationState.Ambiguous -> AdapterRelationshipPhase.RepairRequired
                relationship == null -> AdapterRelationshipPhase.NoRelationship
                else -> AdapterRelationshipPhase.Idle
            },
            companionAssociation = associationState,
            bond = bond,
            message = when {
                relationship == null && associationState == CompanionAssociationState.Ambiguous ->
                    "Android has multiple PicoSwitch2 companion associations; choose Repair pairing before reconnecting."
                status.phase == AdapterRelationshipPhase.Connected -> status.message
                else -> null
            },
        )
    }

    private fun merge(old: AdapterRelationship, new: AdapterRelationship): AdapterRelationship = old.copy(
        associationId = new.associationId ?: old.associationId,
        displayName = new.displayName.takeIf(String::isNotBlank) ?: old.displayName,
    )
}
