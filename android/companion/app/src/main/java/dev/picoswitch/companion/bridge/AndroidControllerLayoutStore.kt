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

    /**
     * The on-screen controller's face presentation.
     *
     * SEPARATE PERSISTENCE, not a second mapping system: the value is the same
     * [ControllerFaceLayout] and it is applied by the same resolver. It needs its
     * own slot only because the per-source store is keyed by a physical device
     * descriptor and a touchscreen has none — and because letting a `null`
     * descriptor decide the drawn diamond's letters would mean the on-screen
     * legend was chosen by an accident of which pad happened to be plugged in.
     *
     * [ControllerFaceLayout.Auto] is deliberately not offered here. Auto exists
     * to guess what is PRINTED on hardware; a drawn control has no printed
     * legend, so the honest default is the presentation this project's target
     * console actually uses.
     */
    fun loadTouch(): ControllerFaceLayout = touchLayoutFrom(preferences.getString(TOUCH_KEY, null))

    fun saveTouch(layout: ControllerFaceLayout) {
        preferences.edit { putString(TOUCH_KEY, layout.key) }
    }

    private fun key(descriptor: String) = "source_$descriptor"

    companion object {
        internal const val FILE_NAME = "controller_layouts"
        internal const val TOUCH_KEY = "touch_gamepad"

        /** Nintendo labels, because the diamond being drawn is a Switch controller's. */
        val DEFAULT_TOUCH_LAYOUT = ControllerFaceLayout.Nintendo

        /**
         * Stored key -> the on-screen controller's face presentation.
         *
         * Pure, so the one rule that matters here is pinned without a device:
         * [ControllerFaceLayout.Auto] never survives. Auto resolves against a
         * physical source identity, and letting a drawn diamond inherit that
         * would mean its letters were chosen by which pad happened to be
         * connected — including "none", whose fallback is positional order.
         */
        internal fun touchLayoutFrom(key: String?): ControllerFaceLayout {
            val stored = ControllerFaceLayout.fromKey(key)
            return if (stored == ControllerFaceLayout.Auto) DEFAULT_TOUCH_LAYOUT else stored
        }
    }
}
