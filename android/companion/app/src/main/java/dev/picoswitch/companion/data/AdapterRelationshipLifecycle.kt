package dev.picoswitch.companion.data

import dev.picoswitch.companion.transport.AdapterResetSignature

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
    FirstPair("first-pair"),
    ForegroundAuto("foreground-auto"),
    Manual("manual"),
    AfterBond("after-bond"),
    AfterPersonality("after-personality"),
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
            message = "Looking for PicoSwitch2. Keep the adapter in pairing mode.",
        )
        return generation
    }

    /** Feed the exact device obtained from the management-service BLE scan into bond progression. */
    @Synchronized
    fun deviceDiscovered(
        discoveryGeneration: Long,
        relationship: AdapterRelationship,
        bond: AndroidBondState,
    ): AdapterLifecycleDecision = associationCreated(
        associationGeneration = discoveryGeneration,
        relationship = relationship,
        bond = bond,
        associationState = CompanionAssociationState.Missing,
        connectReason = AdapterConnectReason.FirstPair,
    )

    @Synchronized
    fun associationCreated(
        associationGeneration: Long,
        relationship: AdapterRelationship,
        bond: AndroidBondState,
        associationState: CompanionAssociationState = CompanionAssociationState.Present,
        connectReason: AdapterConnectReason = AdapterConnectReason.AfterBond,
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
        val attempt = AdapterConnectionAttempt(generation, connectReason, relationship)
        activeAttempt = attempt
        return when (bond) {
            AndroidBondState.Bonded -> {
                status = status.copy(
                    phase = AdapterRelationshipPhase.Connecting,
                    reason = attempt.reason,
                    bond = bond,
                    companionAssociation = associationState,
                    message = "Secure pairing complete; connecting management.",
                )
                AdapterLifecycleDecision.Connect(attempt)
            }
            AndroidBondState.None, AndroidBondState.Bonding -> {
                status = status.copy(
                    phase = AdapterRelationshipPhase.Bonding,
                    reason = attempt.reason,
                    bond = bond,
                    companionAssociation = associationState,
                    message = "Waiting for Android secure pairing to complete.",
                )
                AdapterLifecycleDecision.AwaitBond(attempt, startBond = bond == AndroidBondState.None)
            }
            AndroidBondState.Unknown -> {
                val message = "Android's Bluetooth pairing state could not be read."
                status = status.copy(
                    phase = AdapterRelationshipPhase.Failed,
                    bond = bond,
                    companionAssociation = associationState,
                    message = message,
                )
                activeAttempt = null
                AdapterLifecycleDecision.RepairRequired(message)
            }
        }
    }

    /**
     * No explicit LE bond call was available, so the management GATT connection itself must provoke
     * SMP. Moves the attempt from Bonding to Connecting without inventing a bond state: Android
     * will still broadcast BOND_BONDED, but it does so *during* the connect rather than before it.
     *
     * Only the compatibility path takes this route. It is a phase transition, never a bond claim.
     */
    @Synchronized
    fun bondDelegatedToGatt(attemptGeneration: Long): AdapterLifecycleDecision {
        val attempt = activeAttempt ?: return AdapterLifecycleDecision.Ignored
        if (attempt.generation != attemptGeneration ||
            generation != attemptGeneration ||
            status.phase != AdapterRelationshipPhase.Bonding
        ) return AdapterLifecycleDecision.Ignored

        status = status.copy(
            phase = AdapterRelationshipPhase.Connecting,
            message = "Confirm Android's pairing request to finish setting up the adapter.",
        )
        return AdapterLifecycleDecision.Connect(attempt)
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
                val message = PAIRING_FAILED_MESSAGE
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

    /**
     * @param bondMismatch the peer rejected or lacked our link key while Android
     *   still holds a bond -- see AdapterResetSignature. This is what a firmware
     *   install looks like from the phone, and it is terminal for the saved
     *   relationship: retrying cannot succeed, because the adapter has no key to
     *   authenticate with. Escalate straight to repair rather than leaving the
     *   attempt in Failed, where foreground-auto will simply try again. Six such
     *   attempts across fourteen minutes were observed on 2026-08-23 before
     *   Android dropped its own bond and repair finally triggered.
     */
    @Synchronized
    fun connectionFailed(
        connectionGeneration: Long,
        message: String,
        bondMismatch: Boolean = false,
    ): AdapterLifecycleDecision {
        val attempt = activeAttempt ?: return AdapterLifecycleDecision.Ignored
        if (attempt.generation != connectionGeneration) return AdapterLifecycleDecision.Ignored
        activeAttempt = null
        if (bondMismatch) {
            status = status.copy(
                phase = AdapterRelationshipPhase.RepairRequired,
                message = AdapterResetSignature.REPAIR_MESSAGE,
            )
            return AdapterLifecycleDecision.RepairRequired(AdapterResetSignature.REPAIR_MESSAGE)
        }
        status = status.copy(phase = AdapterRelationshipPhase.Failed, message = message)
        return AdapterLifecycleDecision.Ignored
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

    companion object {
        const val PAIRING_FAILED_MESSAGE =
            "Couldn’t pair with the adapter. Make sure its pairing mode is active, then try again."
    }
}
