package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Reconciliation of the registry against the app's CompanionDeviceManager
 * records.
 *
 * This replaces `AdapterRelationshipReconcilerTest`. Three of its four cases are
 * carried forward unchanged in meaning, because they were about identity
 * resolution and identity resolution did not change. The fourth — "multiple
 * associations are ambiguous rather than first wins" — described a product where
 * owning two adapters was an error state, and is replaced by the two cases that
 * preserve what it was actually protecting: several adapters are now normal, and
 * the app still must not pick one of them as active on the user's behalf.
 */
class AdapterRegistryReconcilerTest {
    private val addressA = "88:A2:9E:D1:77:78"
    private val addressB = "88:A2:9E:D1:77:79"
    private val a = AdapterId.fromAddress(addressA)!!
    private val b = AdapterId.fromAddress(addressB)!!

    private fun record(id: AdapterId, associationId: Int? = null, alias: String? = null) =
        AdapterRecord(id = id, address = id.value, associationId = associationId, userAlias = alias)

    @Test fun `a stored association id stays authoritative when it is still present`() {
        val registry = AdapterRegistry(listOf(record(a, associationId = 7)), a)
        val result = AdapterRegistryReconciler.reconcile(
            registry,
            listOf(
                SystemCompanionAssociation(9, addressA, "Address only"),
                SystemCompanionAssociation(7, addressA, "Exact"),
            ),
        )
        assertEquals(7, result.registry.record(a)?.associationId)
    }

    @Test fun `a changed association id is reconciled by address`() {
        val registry = AdapterRegistry(listOf(record(a, associationId = 2)), a)
        val result = AdapterRegistryReconciler.reconcile(
            registry,
            listOf(SystemCompanionAssociation(7, addressA, "PicoSwitch2")),
        )
        assertEquals(7, result.registry.record(a)?.associationId)
        assertEquals(CompanionAssociationState.Present, result.stateOf(a))
    }

    @Test fun `one owned association recovers an empty registry and becomes active`() {
        val result = AdapterRegistryReconciler.reconcile(
            AdapterRegistry(),
            listOf(SystemCompanionAssociation(7, addressA, "PicoSwitch2")),
        )
        assertEquals(listOf(a), result.registry.records.map { it.id })
        assertEquals(listOf(a), result.adopted)
        // One adapter and no selection is not a choice; two would be.
        assertEquals(a, result.registry.activeId)
    }

    @Test fun `several associations become several adapters, and none is chosen automatically`() {
        val result = AdapterRegistryReconciler.reconcile(
            AdapterRegistry(),
            listOf(
                SystemCompanionAssociation(7, addressA, "One"),
                SystemCompanionAssociation(8, addressB, "Two"),
            ),
        )
        // This is the case the old reconciler called Ambiguous and escalated to
        // RepairRequired, which is what forced the Forget/Pair cycle.
        assertEquals(setOf(a, b), result.registry.records.map { it.id }.toSet())
        assertEquals(CompanionAssociationState.Present, result.stateOf(a))
        assertEquals(CompanionAssociationState.Present, result.stateOf(b))
        // What the old rule was actually protecting: never silently elect one.
        assertNull(result.registry.activeId)
    }

    @Test fun `two records claiming one adapter is what ambiguous now means`() {
        val result = AdapterRegistryReconciler.reconcile(
            AdapterRegistry(),
            listOf(
                SystemCompanionAssociation(7, addressA, "One"),
                SystemCompanionAssociation(8, addressA, "Duplicate"),
            ),
        )
        assertEquals(listOf(a), result.registry.records.map { it.id })
        assertEquals(CompanionAssociationState.Ambiguous, result.stateOf(a))
    }

    @Test fun `a duplicate association resolves deterministically across launches`() {
        val associations = listOf(
            SystemCompanionAssociation(9, addressA, "Nine"),
            SystemCompanionAssociation(3, addressA, "Three"),
        )
        val first = AdapterRegistryReconciler.reconcile(AdapterRegistry(), associations)
        val second = AdapterRegistryReconciler.reconcile(AdapterRegistry(), associations.reversed())
        assertEquals(
            first.registry.record(a)?.associationId,
            second.registry.record(a)?.associationId,
        )
    }

    @Test fun `an adapter with no association keeps its record, alias and selection`() {
        val registry = AdapterRegistry(listOf(record(a, 7, "Living Room"), record(b, 8, "Bedroom")), a)
        val result = AdapterRegistryReconciler.reconcile(
            registry,
            listOf(SystemCompanionAssociation(8, addressB, "Bedroom")),
        )
        // Losing an association -- powered off, removed in system settings --
        // must not delete what the user named their hardware.
        assertEquals("Living Room", result.registry.record(a)?.userAlias)
        assertEquals(CompanionAssociationState.Missing, result.stateOf(a))
        assertEquals(a, result.registry.activeId)
    }

    @Test fun `reconciling one adapter never disturbs another`() {
        val registry = AdapterRegistry(listOf(record(a, 7, "Living Room"), record(b, 8, "Bedroom")), b)
        val result = AdapterRegistryReconciler.reconcile(
            registry,
            listOf(SystemCompanionAssociation(99, addressA, "Living Room")),
        )
        assertEquals(99, result.registry.record(a)?.associationId)
        assertEquals(8, result.registry.record(b)?.associationId)
        assertEquals("Bedroom", result.registry.record(b)?.userAlias)
        assertEquals(b, result.registry.activeId)
    }

    @Test fun `an adapter's own name updates while the user's alias does not`() {
        val registry = AdapterRegistry(listOf(record(a, 7, "Living Room")), a)
        val result = AdapterRegistryReconciler.reconcile(
            registry,
            listOf(SystemCompanionAssociation(7, addressA, "PicoSwitch2 A1B2")),
        )
        val row = result.registry.record(a)!!
        assertEquals("PicoSwitch2 A1B2", row.lastKnownName)
        assertEquals("Living Room", row.userAlias)
        assertEquals("Living Room", row.displayName)
    }

    @Test fun `an association with an unusable address is ignored`() {
        val result = AdapterRegistryReconciler.reconcile(
            AdapterRegistry(),
            listOf(SystemCompanionAssociation(7, "not-an-address", "Junk")),
        )
        assertTrue(result.registry.records.isEmpty())
        assertNull(result.registry.activeId)
    }

    @Test fun `an unchanged reconciliation is not a write`() {
        val registry = AdapterRegistry(listOf(record(a, 7, "Living Room")), a)
        val associations = listOf(SystemCompanionAssociation(7, addressA, "PicoSwitch2"))
        val once = AdapterRegistryReconciler.reconcile(registry, associations)
        // The association refresh runs on every foreground pass; it must not
        // rewrite the document forever.
        val twice = AdapterRegistryReconciler.reconcile(once.registry, associations)
        assertTrue(!twice.changed)
    }
}
