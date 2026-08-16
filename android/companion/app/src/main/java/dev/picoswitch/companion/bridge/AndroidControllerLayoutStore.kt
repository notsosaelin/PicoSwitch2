package dev.picoswitch.companion.bridge

import android.content.Context
import androidx.core.content.edit
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerLayoutStore

/** [ControllerLayoutStore] backed by Android shared preferences. */
class AndroidControllerLayoutStore(context: Context) : ControllerLayoutStore {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    override fun load(descriptor: String?): ControllerFaceLayout {
        if (descriptor.isNullOrBlank()) return ControllerFaceLayout.Auto
        return ControllerFaceLayout.fromKey(preferences.getString(key(descriptor), null))
    }

    override fun save(descriptor: String, layout: ControllerFaceLayout) {
        if (descriptor.isBlank()) return
        preferences.edit { putString(key(descriptor), layout.key) }
    }

    private fun key(descriptor: String) = "source_$descriptor"

    companion object {
        internal const val FILE_NAME = "controller_layouts"
    }
}
