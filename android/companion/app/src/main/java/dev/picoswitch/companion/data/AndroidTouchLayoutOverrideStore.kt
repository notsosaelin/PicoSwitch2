package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit
import dev.picoswitch.bridge.touch.TouchLayoutOverride
import dev.picoswitch.bridge.touch.TouchLayoutOverrideJsonCodec
import dev.picoswitch.bridge.touch.TouchLayoutOverrideStore
import dev.picoswitch.bridge.touch.TouchOverrideDecodeResult
import dev.picoswitch.bridge.touch.TouchProfileId

/** One versioned, app-private sparse override document per touch profile. */
class AndroidTouchLayoutOverrideStore(context: Context) : TouchLayoutOverrideStore {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    override fun load(profileId: TouchProfileId): TouchOverrideDecodeResult? {
        val raw = preferences.getString(key(profileId), null) ?: return null
        val decoded = TouchLayoutOverrideJsonCodec.decode(raw)
        return when (decoded) {
            is TouchOverrideDecodeResult.Valid -> if (decoded.value.profileId == profileId) {
                decoded
            } else {
                TouchOverrideDecodeResult.Invalid("Stored layout belongs to another controller profile")
            }
            is TouchOverrideDecodeResult.Invalid -> decoded
        }
        // Deliberately do not delete an invalid/future raw document here. A
        // later app may understand it, and runtime safely uses the shipped default.
    }

    override fun save(value: TouchLayoutOverride) {
        preferences.edit { putString(key(value.profileId), TouchLayoutOverrideJsonCodec.encode(value)) }
    }

    override fun delete(profileId: TouchProfileId) {
        preferences.edit { remove(key(profileId)) }
    }

    private fun key(profileId: TouchProfileId) = "profile_${profileId.key}"

    companion object { internal const val FILE_NAME = "touch_layout_overrides" }
}
