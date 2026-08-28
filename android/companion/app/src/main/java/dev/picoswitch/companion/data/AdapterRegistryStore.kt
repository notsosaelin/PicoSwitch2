package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit

/**
 * Where the adapter registry lives on disk.
 *
 * Deliberately thin. Everything that can be got wrong — schema, migration,
 * sanitisation, tolerance of a damaged document — is in [AdapterRegistryCodec],
 * which has no Android dependency and is covered by ordinary JVM tests. This
 * class only decides *where* and *when*.
 *
 * ```text
 * adapter_registry/document        this store, schema 1
 * adapter_relationship             the single-adapter store it replaces, read once
 * ```
 *
 * The legacy file is read exactly once, when no registry document exists yet,
 * and is then left alone forever. Not clearing it is the point: if a migration
 * ever produces something wrong, the original relationship is still on disk to
 * read back by hand, and it costs one small preferences file to keep.
 */
class AdapterRegistryStore(context: Context) {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
    private val legacy = context.getSharedPreferences(LEGACY_FILE_NAME, Context.MODE_PRIVATE)

    fun load(): AdapterRegistry {
        preferences.getString(KEY_DOCUMENT, null)?.let { return AdapterRegistryCodec.decode(it) }
        val migrated = AdapterRegistryCodec.migrate(loadLegacyRelationship())
        // Write the migrated document even when it is empty, so a fresh install
        // does not re-read the legacy store on every launch.
        save(migrated)
        return migrated
    }

    fun save(registry: AdapterRegistry) {
        preferences.edit { putString(KEY_DOCUMENT, AdapterRegistryCodec.encode(registry)) }
    }

    /** True when this install has a registry document of its own; tests and diagnostics only. */
    fun hasDocument(): Boolean = preferences.contains(KEY_DOCUMENT)

    private fun loadLegacyRelationship(): AdapterRelationship? {
        val address = legacy.getString(LEGACY_KEY_ADDRESS, null) ?: return null
        return AdapterRelationship(
            address = address.uppercase(),
            associationId = legacy.getInt(LEGACY_KEY_ASSOCIATION_ID, NO_ASSOCIATION).takeUnless { it == NO_ASSOCIATION },
            displayName = legacy.getString(LEGACY_KEY_NAME, null)?.take(80).orEmpty()
                .ifBlank { AdapterRecord.DEFAULT_PRODUCT_NAME },
        )
    }

    companion object {
        internal const val FILE_NAME = "adapter_registry"
        internal const val LEGACY_FILE_NAME = "adapter_relationship"
        private const val KEY_DOCUMENT = "document"
        private const val LEGACY_KEY_ADDRESS = "address"
        private const val LEGACY_KEY_ASSOCIATION_ID = "association_id"
        private const val LEGACY_KEY_NAME = "name"
        private const val NO_ASSOCIATION = -1
    }
}
