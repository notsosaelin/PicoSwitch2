package dev.picoswitch.companion.data

import org.junit.Assert.*
import org.junit.Test

class AdapterRelationshipLifecycleTest {
    private val adapter = AdapterRelationship("88:A2:9E:D1:77:78", 7, "PicoSwitch2")

    @Test fun `BLE discovered unbonded device starts exactly one secure bond`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        val decision = coordinator.deviceDiscovered(
            generation,
            adapter.copy(associationId = null),
            AndroidBondState.None,
        ) as AdapterLifecycleDecision.AwaitBond
        assertTrue(decision.startBond)
        assertEquals(AdapterConnectReason.FirstPair, decision.attempt.reason)
        assertEquals(AdapterRelationshipPhase.Bonding, coordinator.status.phase)
        assertEquals(CompanionAssociationState.Missing, coordinator.status.companionAssociation)
    }

    @Test fun `BLE discovered bonded device connects without another bond`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        val decision = coordinator.deviceDiscovered(
            generation,
            adapter.copy(associationId = null),
            AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect
        assertEquals(AdapterConnectReason.FirstPair, decision.attempt.reason)
        assertEquals(AdapterRelationshipPhase.Connecting, coordinator.status.phase)
    }

    @Test fun `BLE discovered device already bonding is joined without restart`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        val decision = coordinator.deviceDiscovered(
            generation,
            adapter.copy(associationId = null),
            AndroidBondState.Bonding,
        ) as AdapterLifecycleDecision.AwaitBond
        assertFalse(decision.startBond)
        assertEquals(AdapterRelationshipPhase.Bonding, coordinator.status.phase)
    }

    @Test fun `rejected BLE bond reports pairing mode guidance and never connects`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        coordinator.deviceDiscovered(generation, adapter.copy(associationId = null), AndroidBondState.None)
        val failure = coordinator.bondChanged(adapter.address, AndroidBondState.None)
            as AdapterLifecycleDecision.RepairRequired
        assertEquals(AdapterRelationshipCoordinator.PAIRING_FAILED_MESSAGE, failure.message)
        assertEquals(AdapterRelationshipPhase.RepairRequired, coordinator.status.phase)
        assertNull(coordinator.connectionSucceeded(generation))
    }

    @Test fun `bond delegated to GATT advances the same attempt to connecting`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        coordinator.deviceDiscovered(generation, adapter.copy(associationId = null), AndroidBondState.None)

        val decision = coordinator.bondDelegatedToGatt(generation) as AdapterLifecycleDecision.Connect

        assertEquals(generation, decision.attempt.generation)
        assertEquals(AdapterConnectReason.FirstPair, decision.attempt.reason)
        assertEquals(AdapterRelationshipPhase.Connecting, coordinator.status.phase)
        // It is a phase transition, not a bond claim: Android still owns the bond state.
        assertEquals(AndroidBondState.None, coordinator.status.bond)
    }

    @Test fun `bond delegation is inert outside its own bonding attempt`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        coordinator.deviceDiscovered(generation, adapter.copy(associationId = null), AndroidBondState.None)

        assertTrue(coordinator.bondDelegatedToGatt(generation - 1) is AdapterLifecycleDecision.Ignored)
        assertEquals(AdapterRelationshipPhase.Bonding, coordinator.status.phase)

        coordinator.bondDelegatedToGatt(generation)
        // Already connecting: a duplicate delegation must not start a second connect.
        assertTrue(coordinator.bondDelegatedToGatt(generation) is AdapterLifecycleDecision.Ignored)
    }

    @Test fun `duplicate association completion advances only once`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        assertTrue(coordinator.associationCreated(generation, adapter, AndroidBondState.Bonding) is AdapterLifecycleDecision.AwaitBond)
        assertSame(AdapterLifecycleDecision.Ignored, coordinator.associationCreated(generation, adapter, AndroidBondState.Bonding))
        val connect = coordinator.bondChanged(adapter.address, AndroidBondState.Bonded)
        assertTrue(connect is AdapterLifecycleDecision.Connect)
        assertSame(AdapterLifecycleDecision.Ignored, coordinator.bondChanged(adapter.address, AndroidBondState.Bonded))
    }

    @Test fun `late duplicate can add association id without reconnecting`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        val legacy = adapter.copy(associationId = null)
        val connect = coordinator.associationCreated(
            generation,
            legacy,
            AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect
        assertEquals(legacy, coordinator.connectionSucceeded(connect.attempt.generation))

        val update = coordinator.associationCreated(
            generation,
            adapter,
            AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.RelationshipMetadataUpdated
        assertEquals(adapter.associationId, update.relationship.associationId)
        assertEquals(AdapterRelationshipPhase.Connected, coordinator.status.phase)
    }

    @Test fun `bonding never starts GATT before bonded broadcast`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        val decision = coordinator.associationCreated(generation, adapter, AndroidBondState.Bonding)
        assertEquals(false, (decision as AdapterLifecycleDecision.AwaitBond).startBond)
        assertEquals(AdapterRelationshipPhase.Bonding, coordinator.status.phase)
    }

    @Test fun `manual reconnect cannot overlap foreground auto reconnect`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        assertTrue(coordinator.requestReconnect(adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded) is AdapterLifecycleDecision.Connect)
        assertSame(AdapterLifecycleDecision.Ignored, coordinator.requestReconnect(adapter, AdapterConnectReason.Manual, AndroidBondState.Bonded))
    }

    @Test fun `stale completion cannot overwrite newer attempt`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        val first = coordinator.requestReconnect(adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded) as AdapterLifecycleDecision.Connect
        coordinator.cancelAndRetainRelationship()
        val second = coordinator.requestReconnect(adapter, AdapterConnectReason.Manual, AndroidBondState.Bonded) as AdapterLifecycleDecision.Connect
        assertNull(coordinator.connectionSucceeded(first.attempt.generation))
        assertEquals(adapter, coordinator.connectionSucceeded(second.attempt.generation))
        assertEquals(AdapterRelationshipPhase.Connected, coordinator.status.phase)
    }

    @Test fun `disconnect invalidates bond and connect callbacks but retains relationship`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        coordinator.requestReconnect(adapter, AdapterConnectReason.Manual, AndroidBondState.Bonding)
        coordinator.cancelAndRetainRelationship()
        assertSame(AdapterLifecycleDecision.Ignored, coordinator.bondChanged(adapter.address, AndroidBondState.Bonded))
        assertEquals(AdapterRelationshipPhase.Idle, coordinator.status.phase)
    }

    @Test fun `missing Android bond becomes explicit repair state`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        assertTrue(coordinator.requestReconnect(adapter, AdapterConnectReason.Manual, AndroidBondState.None) is AdapterLifecycleDecision.RepairRequired)
        assertEquals(AdapterRelationshipPhase.RepairRequired, coordinator.status.phase)
    }

    @Test fun `foreground reentry cannot race a bond wait`() {
        val coordinator = AdapterRelationshipCoordinator(null)
        val generation = coordinator.beginAssociation()
        coordinator.associationCreated(generation, adapter, AndroidBondState.Bonding)
        assertSame(
            AdapterLifecycleDecision.Ignored,
            coordinator.requestReconnect(adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded),
        )
        assertEquals(AdapterRelationshipPhase.Bonding, coordinator.status.phase)
    }

    @Test fun `bond removal outside an attempt marks saved relationship for repair`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        assertTrue(coordinator.bondChanged(adapter.address, AndroidBondState.None) is AdapterLifecycleDecision.RepairRequired)
        assertEquals(AdapterRelationshipPhase.RepairRequired, coordinator.status.phase)
    }

    @Test fun `ended verified connection retains relationship and permits reconnect`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        val first = coordinator.requestReconnect(
            adapter,
            AdapterConnectReason.Manual,
            AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect
        assertEquals(adapter, coordinator.connectionSucceeded(first.attempt.generation))

        assertTrue(coordinator.connectionEnded("Connection lost"))
        assertEquals(AdapterRelationshipPhase.Idle, coordinator.status.phase)
        assertTrue(
            coordinator.requestReconnect(
                adapter,
                AdapterConnectReason.Manual,
                AndroidBondState.Bonded,
            ) is AdapterLifecycleDecision.Connect,
        )
    }

    @Test fun `a key failure against a live bond escalates straight to repair`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        val decision = coordinator.requestReconnect(
            adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect

        val outcome = coordinator.connectionFailed(
            decision.attempt.generation, "connect failed", bondMismatch = true,
        )

        // Not Failed. Failed leaves foreground-auto free to try again, which is
        // how six attempts across fourteen minutes happened on 2026-08-23 while
        // the adapter had no key to authenticate with at all.
        assertTrue(outcome is AdapterLifecycleDecision.RepairRequired)
        assertEquals(AdapterRelationshipPhase.RepairRequired, coordinator.status.phase)
        assertEquals(
            dev.picoswitch.companion.transport.AdapterResetSignature.REPAIR_MESSAGE,
            coordinator.status.message,
        )
    }

    @Test fun `an ordinary connect failure stays retryable`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        val decision = coordinator.requestReconnect(
            adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect

        val outcome = coordinator.connectionFailed(
            decision.attempt.generation, "stack error 133", bondMismatch = false,
        )

        assertTrue(outcome is AdapterLifecycleDecision.Ignored)
        assertEquals(AdapterRelationshipPhase.Failed, coordinator.status.phase)
        assertEquals("stack error 133", coordinator.status.message)
    }

    @Test fun `a stale generation cannot force repair`() {
        val coordinator = AdapterRelationshipCoordinator(adapter)
        val decision = coordinator.requestReconnect(
            adapter, AdapterConnectReason.ForegroundAuto, AndroidBondState.Bonded,
        ) as AdapterLifecycleDecision.Connect

        val outcome = coordinator.connectionFailed(
            decision.attempt.generation + 99, "connect failed", bondMismatch = true,
        )

        assertTrue(outcome is AdapterLifecycleDecision.Ignored)
        assertEquals(AdapterRelationshipPhase.Connecting, coordinator.status.phase)
    }
}
