package dev.picoswitch.companion.data

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit

/**
 * Everything the on-screen controller lets the user change.
 *
 * CONFIGURATION ONLY. Nothing here describes what is currently held — no contact
 * identifiers, no pressed buttons, no stick position, no direction. Gameplay
 * state is never persisted, because restoring it would mean a process that came
 * back from the dead immediately telling the console a button is down.
 *
 * Defaults are chosen so the controller is playable the first time it is opened
 * without visiting this screen at all.
 */
data class TouchGamepadSettings(
    /** Control opacity over the background. */
    val controlOpacity: Float = DEFAULT_OPACITY,

    /** How much the background image is dimmed so controls stay legible. */
    val backgroundDim: Float = DEFAULT_DIM,

    /** Local touch feedback. Console rumble is unrelated and always on. */
    val hapticsEnabled: Boolean = true,

    /** Inner fraction of a stick's travel that reads as centre. */
    val stickDeadzone: Float = DEFAULT_DEADZONE,

    /**
     * The user's chosen background image, as the platform's own reference.
     *
     * Stored as text and re-resolved on use rather than trusted: the picture can
     * be deleted, the grant can lapse, and the honest response to either is the
     * default background rather than a crash.
     */
    val backgroundImage: String? = null,

    /**
     * Where the layout editor's floating toolbar sits.
     *
     * A preference rather than a constant because the right answer depends on
     * the device: which hand holds the phone, whether the window is a tall
     * freeform one, and which edge the system's own gesture areas are on.
     */
    val editorToolbarDock: TouchEditorDock = TouchEditorDock.Bottom,

    /** Draw the editor's alignment grid. */
    val editorGrid: Boolean = false,

    /** Pull an edited control onto nearby guides. Independent of [editorGrid]. */
    val editorSnap: Boolean = true,
) {
    companion object {
        const val DEFAULT_OPACITY = 0.85f
        const val MIN_OPACITY = 0.25f
        const val MAX_OPACITY = 1f

        const val DEFAULT_DIM = 0.35f
        const val MAX_DIM = 0.85f

        /**
         * A touchscreen stick has no spring, so the gate only has to cover the
         * few pixels a resting thumb wanders. A large one would cost real range.
         */
        const val DEFAULT_DEADZONE = 0.05f
        const val MAX_DEADZONE = 0.20f

        val Default = TouchGamepadSettings()
    }
}

/**
 * Preference values -> settings, kept pure.
 *
 * Separated from the store so clamping and defaulting are testable with no
 * device, and so a stored value that has drifted out of range — an older build,
 * a hand-edited file — resolves to something usable rather than to a controller
 * with an 80% deadzone.
 */
object TouchGamepadSettingsCodec {

    fun decode(
        opacity: Float,
        dim: Float,
        haptics: Boolean,
        deadzone: Float,
        backgroundImage: String?,
        editorToolbarDock: String? = null,
        editorGrid: Boolean = false,
        editorSnap: Boolean = true,
    ): TouchGamepadSettings = TouchGamepadSettings(
        controlOpacity = opacity.finiteOr(TouchGamepadSettings.DEFAULT_OPACITY)
            .coerceIn(TouchGamepadSettings.MIN_OPACITY, TouchGamepadSettings.MAX_OPACITY),
        backgroundDim = dim.finiteOr(TouchGamepadSettings.DEFAULT_DIM)
            .coerceIn(0f, TouchGamepadSettings.MAX_DIM),
        hapticsEnabled = haptics,
        stickDeadzone = deadzone.finiteOr(TouchGamepadSettings.DEFAULT_DEADZONE)
            .coerceIn(0f, TouchGamepadSettings.MAX_DEADZONE),
        backgroundImage = backgroundImage?.takeIf { it.isNotBlank() },
        editorToolbarDock = TouchEditorDock.fromKey(editorToolbarDock),
        editorGrid = editorGrid,
        editorSnap = editorSnap,
    )

    private fun Float.finiteOr(fallback: Float) = if (isFinite()) this else fallback
}

/**
 * Which edge the layout editor's floating toolbar docks to.
 *
 * Persisted by [key], never by ordinal: reordering this enum must not silently
 * move somebody's toolbar to a different edge.
 */
enum class TouchEditorDock(val key: String, val title: String) {
    Bottom("bottom", "Bottom"),
    Top("top", "Top"),
    Left("left", "Left"),
    Right("right", "Right");

    val vertical: Boolean get() = this == Left || this == Right

    companion object {
        fun fromKey(value: String?): TouchEditorDock =
            entries.firstOrNull { it.key == value } ?: Bottom
    }
}

/** [TouchGamepadSettings] persisted by the platform. */
class TouchGamepadSettingsStore(context: Context) {
    private val preferences: SharedPreferences =
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(): TouchGamepadSettings = TouchGamepadSettingsCodec.decode(
        opacity = preferences.getFloat(KEY_OPACITY, TouchGamepadSettings.DEFAULT_OPACITY),
        dim = preferences.getFloat(KEY_DIM, TouchGamepadSettings.DEFAULT_DIM),
        haptics = preferences.getBoolean(KEY_HAPTICS, true),
        deadzone = preferences.getFloat(KEY_DEADZONE, TouchGamepadSettings.DEFAULT_DEADZONE),
        backgroundImage = preferences.getString(KEY_BACKGROUND, null),
        editorToolbarDock = preferences.getString(KEY_EDITOR_DOCK, null),
        editorGrid = preferences.getBoolean(KEY_EDITOR_GRID, false),
        editorSnap = preferences.getBoolean(KEY_EDITOR_SNAP, true),
    )

    fun save(settings: TouchGamepadSettings) {
        preferences.edit {
            putFloat(KEY_OPACITY, settings.controlOpacity)
            putFloat(KEY_DIM, settings.backgroundDim)
            putBoolean(KEY_HAPTICS, settings.hapticsEnabled)
            putFloat(KEY_DEADZONE, settings.stickDeadzone)
            if (settings.backgroundImage == null) {
                remove(KEY_BACKGROUND)
            } else {
                putString(KEY_BACKGROUND, settings.backgroundImage)
            }
            putString(KEY_EDITOR_DOCK, settings.editorToolbarDock.key)
            putBoolean(KEY_EDITOR_GRID, settings.editorGrid)
            putBoolean(KEY_EDITOR_SNAP, settings.editorSnap)
        }
    }

    companion object {
        internal const val FILE_NAME = "touch_gamepad"
        internal const val KEY_OPACITY = "control_opacity"
        internal const val KEY_DIM = "background_dim"
        internal const val KEY_HAPTICS = "haptics"
        internal const val KEY_DEADZONE = "stick_deadzone"
        internal const val KEY_BACKGROUND = "background_image"
        internal const val KEY_EDITOR_DOCK = "editor_toolbar_dock"
        internal const val KEY_EDITOR_GRID = "editor_grid"
        internal const val KEY_EDITOR_SNAP = "editor_snap"
    }
}
