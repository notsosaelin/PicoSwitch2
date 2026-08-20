package dev.picoswitch.companion.data

import org.junit.Assert.*
import org.junit.Test

class AdapterRelationshipLifecycleTest {
    private val adapter = AdapterRelationship("88:A2:9E:D1:77:78", 7, "PicoSwitch2")

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
}
