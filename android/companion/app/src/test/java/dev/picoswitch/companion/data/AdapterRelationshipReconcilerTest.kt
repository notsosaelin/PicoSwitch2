package dev.picoswitch.companion.data

import org.junit.Assert.*
import org.junit.Test

class AdapterRelationshipReconcilerTest {
    private val saved = AdapterRelationship("88:A2:9E:D1:77:78", 7, "Saved")
    private val exact = SystemCompanionAssociation(7, "88:A2:9E:D1:77:78", "Exact")

    @Test fun `stored association id is authoritative`() {
        val result = AdapterRelationshipReconciler.reconcile(saved, listOf(
            SystemCompanionAssociation(9, saved.address, "Address only"), exact,
        ))
        assertEquals(AdapterRelationshipReconciliation.Source.ExactAssociationId, result.source)
        assertEquals(7, result.relationship?.associationId)
        assertEquals("Exact", result.relationship?.displayName)
    }

    @Test fun `stored address reconciles a changed association id`() {
        val result = AdapterRelationshipReconciler.reconcile(saved.copy(associationId = 2), listOf(exact))
        assertEquals(AdapterRelationshipReconciliation.Source.AddressMatch, result.source)
        assertEquals(7, result.relationship?.associationId)
    }

    @Test fun `one app association can recover an absent store`() {
        val result = AdapterRelationshipReconciler.reconcile(null, listOf(exact))
        assertEquals(AdapterRelationshipReconciliation.Source.OnlyAssociation, result.source)
        assertEquals(exact.address, result.relationship?.address)
    }

    @Test fun `multiple associations are ambiguous rather than first wins`() {
        val result = AdapterRelationshipReconciler.reconcile(null, listOf(
            exact, SystemCompanionAssociation(8, "88:A2:9E:D1:77:79"),
        ))
        assertEquals(CompanionAssociationState.Ambiguous, result.associationState)
        assertNull(result.relationship)
    }
}
