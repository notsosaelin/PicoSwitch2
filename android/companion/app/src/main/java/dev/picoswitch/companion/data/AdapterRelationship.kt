package dev.picoswitch.companion.data

/**
 * One adapter, as the connection lifecycle sees it.
 *
 * This is the value [AdapterRelationshipCoordinator] progresses through
 * association, bonding and connection. It is deliberately NOT the registry
 * record: the coordinator owns one attempt at a time and has no business
 * knowing about aliases, cached firmware, or the other adapters the user owns.
 * [AdapterRecord.toRelationship] is the one bridge between the two.
 *
 * Persistence lives in [AdapterRegistryStore]. The single-adapter
 * `AdapterRelationshipStore` that used to be here was the storage half of a
 * one-adapter product; its preferences file is still read once, by the
 * registry's migration, and is never written again.
 */
data class AdapterRelationship(
    val address: String,
    val associationId: Int? = null,
    val displayName: String = AdapterRecord.DEFAULT_PRODUCT_NAME,
)
