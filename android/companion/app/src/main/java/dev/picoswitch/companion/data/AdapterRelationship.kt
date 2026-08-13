package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit

data class AdapterRelationship(
    val address: String,
    val associationId: Int? = null,
    val displayName: String = "PicoSwitch2",
)

class AdapterRelationshipStore(context: Context) {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(): AdapterRelationship? {
        val address = preferences.getString(KEY_ADDRESS, null)?.takeIf { MAC.matches(it) } ?: return null
        return AdapterRelationship(
            address = address.uppercase(),
            associationId = preferences.getInt(KEY_ASSOCIATION_ID, NO_ASSOCIATION).takeUnless { it == NO_ASSOCIATION },
            displayName = preferences.getString(KEY_NAME, null)?.take(80).orEmpty().ifBlank { "PicoSwitch2" },
        )
    }

    fun save(relationship: AdapterRelationship) {
        require(MAC.matches(relationship.address)) { "Invalid Bluetooth address" }
        preferences.edit {
            putString(KEY_ADDRESS, relationship.address.uppercase())
            putString(KEY_NAME, relationship.displayName.take(80))
            putInt(KEY_ASSOCIATION_ID, relationship.associationId ?: NO_ASSOCIATION)
        }
    }

    fun clear() = preferences.edit { clear() }

    companion object {
        internal const val FILE_NAME = "adapter_relationship"
        private const val KEY_ADDRESS = "address"
        private const val KEY_ASSOCIATION_ID = "association_id"
        private const val KEY_NAME = "name"
        private const val NO_ASSOCIATION = -1
        private val MAC = Regex("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}")
    }
}
