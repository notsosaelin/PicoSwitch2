package dev.picoswitch.companion.data

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit
import dev.picoswitch.bridge.touch.TouchToolbarEdge
import dev.picoswitch.bridge.touch.TouchToolbarPlacement

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
     * Whether double-tapping a button and holding the second press toggles a
     * persistent hold.
     *
     * The DEFAULT for controls that state no preference of their own; the layout
     * editor's per-control choice overrides it either way. This is configuration
     * — what is currently latched is runtime state and is never written down.
     */
    val doubleTapHold: Boolean = true,

    /**
     * The user's chosen background image, as the platform's own reference.
     *
     * Stored as text and re-resolved on use rather than trusted: the picture can
     * be deleted, the grant can lapse, and the honest response to either is the
     * default background rather than a crash.
     */
    val backgroundImage: String? = null,

    /**
     * Where the layout editor's toolbar sits, in a landscape window.
     *
     * A preference rather than a constant because the right answer depends on
     * the device: which hand holds the phone, whether the window is a tall
     * freeform one, and which edge the system's own gesture areas are on.
     *
     * Two slots rather than one because a placement that suits a wide window
     * rarely suits a tall one — a toolbar docked to the right edge of a landscape
     * phone is over the face buttons once the same phone is turned upright, and
     * making the user move it back on every rotation is worse than remembering
     * both answers.
     */
    val editorToolbarLandscape: TouchToolbarPlacement = TouchToolbarPlacement.Default,

    /** Where the toolbar sits in a portrait window; see [editorToolbarLandscape]. */
    val editorToolbarPortrait: TouchToolbarPlacement = TouchToolbarPlacement.Default,

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

    /** The toolbar placement for the window shape currently on screen. */
    fun editorToolbar(landscape: Boolean): TouchToolbarPlacement =
        if (landscape) editorToolbarLandscape else editorToolbarPortrait

    fun withEditorToolbar(landscape: Boolean, placement: TouchToolbarPlacement) =
        if (landscape) copy(editorToolbarLandscape = placement)
        else copy(editorToolbarPortrait = placement)
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
        editorToolbarLandscape: String? = null,
        editorToolbarPortrait: String? = null,
        editorGrid: Boolean = false,
        editorSnap: Boolean = true,
        doubleTapHold: Boolean = true,
    ): TouchGamepadSettings = TouchGamepadSettings(
        controlOpacity = opacity.finiteOr(TouchGamepadSettings.DEFAULT_OPACITY)
            .coerceIn(TouchGamepadSettings.MIN_OPACITY, TouchGamepadSettings.MAX_OPACITY),
        backgroundDim = dim.finiteOr(TouchGamepadSettings.DEFAULT_DIM)
            .coerceIn(0f, TouchGamepadSettings.MAX_DIM),
        hapticsEnabled = haptics,
        stickDeadzone = deadzone.finiteOr(TouchGamepadSettings.DEFAULT_DEADZONE)
            .coerceIn(0f, TouchGamepadSettings.MAX_DEADZONE),
        backgroundImage = backgroundImage?.takeIf { it.isNotBlank() },
        editorToolbarLandscape = decodePlacement(editorToolbarLandscape),
        editorToolbarPortrait = decodePlacement(editorToolbarPortrait),
        editorGrid = editorGrid,
        editorSnap = editorSnap,
        doubleTapHold = doubleTapHold,
    )

    /**
     * A toolbar placement as one short, self-describing string.
     *
     * ```text
     * "bottom"          docked to that safe edge
     * "float:0.42,0.77" free, normalized within the safe region
     * ```
     *
     * Text rather than several preference keys so a placement is written and
     * read as ONE value: two keys could be half-updated by an interrupted
     * commit and describe a toolbar that is docked and floating at once.
     * Anything unrecognized falls back to the default rather than failing, since
     * the worst case is a toolbar the user has to move again.
     */
    fun encodePlacement(placement: TouchToolbarPlacement): String = when (placement) {
        is TouchToolbarPlacement.Docked -> placement.edge.key
        is TouchToolbarPlacement.Floating -> "$FLOAT_PREFIX${placement.x},${placement.y}"
    }

    fun decodePlacement(raw: String?): TouchToolbarPlacement {
        if (raw.isNullOrBlank()) return TouchToolbarPlacement.Default
        TouchToolbarEdge.fromKey(raw)?.let { return TouchToolbarPlacement.Docked(it) }
        if (!raw.startsWith(FLOAT_PREFIX)) return TouchToolbarPlacement.Default
        val parts = raw.removePrefix(FLOAT_PREFIX).split(',')
        if (parts.size != 2) return TouchToolbarPlacement.Default
        val x = parts[0].toFloatOrNull()?.takeIf { it.isFinite() } ?: return TouchToolbarPlacement.Default
        val y = parts[1].toFloatOrNull()?.takeIf { it.isFinite() } ?: return TouchToolbarPlacement.Default
        return TouchToolbarPlacement.Floating(x.coerceIn(0f, 1f), y.coerceIn(0f, 1f))
    }

    private const val FLOAT_PREFIX = "float:"

    private fun Float.finiteOr(fallback: Float) = if (isFinite()) this else fallback
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
        // The retired single-dock key is the fallback for both, so an upgrading
        // install keeps the edge it had until the toolbar is moved again.
        editorToolbarLandscape = preferences.getString(KEY_EDITOR_TOOLBAR_LANDSCAPE, null)
            ?: preferences.getString(KEY_EDITOR_DOCK, null),
        editorToolbarPortrait = preferences.getString(KEY_EDITOR_TOOLBAR_PORTRAIT, null)
            ?: preferences.getString(KEY_EDITOR_DOCK, null),
        editorGrid = preferences.getBoolean(KEY_EDITOR_GRID, false),
        editorSnap = preferences.getBoolean(KEY_EDITOR_SNAP, true),
        doubleTapHold = preferences.getBoolean(KEY_DOUBLE_TAP_HOLD, true),
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
            putString(
                KEY_EDITOR_TOOLBAR_LANDSCAPE,
                TouchGamepadSettingsCodec.encodePlacement(settings.editorToolbarLandscape),
            )
            putString(
                KEY_EDITOR_TOOLBAR_PORTRAIT,
                TouchGamepadSettingsCodec.encodePlacement(settings.editorToolbarPortrait),
            )
            putBoolean(KEY_EDITOR_GRID, settings.editorGrid)
            putBoolean(KEY_EDITOR_SNAP, settings.editorSnap)
            putBoolean(KEY_DOUBLE_TAP_HOLD, settings.doubleTapHold)
        }
    }

    companion object {
        internal const val FILE_NAME = "touch_gamepad"
        internal const val KEY_OPACITY = "control_opacity"
        internal const val KEY_DIM = "background_dim"
        internal const val KEY_HAPTICS = "haptics"
        internal const val KEY_DEADZONE = "stick_deadzone"
        internal const val KEY_BACKGROUND = "background_image"
        /** Retired: one dock for every window shape. Still READ, never written. */
        internal const val KEY_EDITOR_DOCK = "editor_toolbar_dock"
        internal const val KEY_EDITOR_TOOLBAR_LANDSCAPE = "editor_toolbar_landscape"
        internal const val KEY_EDITOR_TOOLBAR_PORTRAIT = "editor_toolbar_portrait"
        internal const val KEY_EDITOR_GRID = "editor_grid"
        internal const val KEY_EDITOR_SNAP = "editor_snap"
        internal const val KEY_DOUBLE_TAP_HOLD = "double_tap_hold"
    }
}
