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
     * A CONSTANT since Editor 2.0, and no longer a stored preference. It used to
     * be a user choice in the Touch Gamepad menu — Nintendo or Xbox letters on
     * the drawn diamond — and that question stopped being a product-level one
     * once the same menu could switch between four genuine controller
     * personalities: the letters belong to whichever controller is being
     * emulated, and offering them separately made it possible to label a Joy-Con
     * with an Xbox diamond.
     *
     * SEPARATE from the per-source store rather than a second mapping system:
     * the value is the same [ControllerFaceLayout], applied by the same
     * resolver. It needs its own answer only because that store is keyed by a
     * physical device descriptor and a touchscreen has none — and letting a
     * `null` descriptor decide the drawn diamond's letters would mean the
     * on-screen legend was chosen by an accident of which pad happened to be
     * plugged in.
     *
     * [ControllerFaceLayout.Auto] is deliberately not used. Auto exists to guess
     * what is PRINTED on hardware; a drawn control has no printed legend.
     */
    fun loadTouch(): ControllerFaceLayout = DEFAULT_TOUCH_LAYOUT

    private fun key(descriptor: String) = "source_$descriptor"

    companion object {
        internal const val FILE_NAME = "controller_layouts"

        /** Nintendo labels, because every controller this surface draws is one. */
        val DEFAULT_TOUCH_LAYOUT = ControllerFaceLayout.Nintendo
    }
}
