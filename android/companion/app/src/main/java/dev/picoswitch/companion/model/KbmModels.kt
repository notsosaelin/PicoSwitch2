package dev.picoswitch.companion.model

/**
 * Client model of the adapter's Keyboard / Keyboard + Mouse configuration.
 *
 * The firmware owns every rule here -- which sources are bindable, which
 * destinations exist, what the accepted numeric ranges are, and what the
 * defaults are. This file therefore holds no duplicated policy: it names the
 * wire values, translates them into product language, and carries the ranges
 * the adapter itself reported. The one thing it does add is presentation
 * grouping and human labels, which the wire deliberately does not carry.
 *
 * Wire surface (src/config.c `cmd_kbm`):
 *   kbm status                     -> live roles, effective mode, counters
 *   kbm mode                       -> effective mode + persisted override
 *   kbm mode <auto|controller|keyboard|kbmouse>
 *   kbm map <kb|kbm> [page]        -> paginated effective bindings
 *   kbm bind <kb|kbm> <src> <dst|none|default>
 *   kbm reset <kb|kbm|all>
 *   kbm mouse                      -> mouse translation config AND its limits
 *   kbm mouse <field> <value>
 */

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

/**
 * The adapter's input mode.
 *
 * [Automatic] is the persisted default and is an *override* value, not a live
 * state: the adapter infers the live mode from which roles are actually
 * admitted. The UI therefore shows the user's choice and the resulting
 * effective mode as two separate facts, because they legitimately differ (a
 * keyboard alone under Automatic runs the Keyboard profile).
 */
typealias KbmMode = dev.picoswitch.management.KbmMode

val KbmMode.title: String get() = when (this) {
    KbmMode.Automatic -> "Automatic"
    KbmMode.Controller -> "Controller"
    KbmMode.Keyboard -> "Keyboard"
    KbmMode.KeyboardMouse -> "Keyboard + Mouse"
}

val KbmMode.description: String get() = when (this) {
    KbmMode.Automatic -> "Match whatever you connect"
    KbmMode.Controller -> "Ignore keyboards and mice"
    KbmMode.Keyboard -> "Keyboard drives both sticks"
    KbmMode.KeyboardMouse -> "Mouse aims, keyboard moves"
}

/**
 * A mapping profile. These are genuinely independent layouts rather than one
 * being a superset of the other -- under Keyboard the keyboard carries both
 * sticks, under Keyboard + Mouse the mouse owns aiming -- so the editor never
 * merges them.
 */
typealias KbmProfile = dev.picoswitch.management.KbmProfile

val KbmProfile.title: String get() = when (this) {
    KbmProfile.Keyboard -> "Keyboard"
    KbmProfile.KeyboardMouse -> "Keyboard + Mouse"
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/**
 * Live Keyboard/Mouse state as reported by `kbm status`.
 *
 * The connection indices, generation counters and rejection tallies the
 * firmware also reports are arbitration internals; they are carried here for
 * Diagnostics only and never surface on the product page.
 */
typealias KbmStatus = dev.picoswitch.management.KbmStatus

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

typealias KbmSourceKind = dev.picoswitch.management.KbmSourceKind

/**
 * One bindable physical input, in the adapter's own vocabulary.
 *
 * [code] is a HID Usage Page 0x07 id for a key and a 1-based button number for
 * a mouse. [wire] reproduces the exact `key:NN` / `mouse:N` text the firmware
 * parses, so no screen ever assembles that string itself.
 */
typealias KbmSource = dev.picoswitch.management.KbmSource

val KbmSource.label: String get() = when (kind) {
    KbmSourceKind.Key -> KeyboardKeys.label(code)
    KbmSourceKind.MouseButton -> MouseButtons.label(code)
}

/**
 * HID keyboard usage ids in product language.
 *
 * Only the names are here. Which usages are *bindable* stays with the firmware
 * (`ns2_kbm_source_valid`); this table exists so the editor never shows a user
 * a hexadecimal usage id. An unlisted usage falls back to a neutral
 * "Key 0xNN" rather than being given an invented name.
 */
object KeyboardKeys {
    /** Offered in the picker, grouped the way a keyboard is actually laid out. */
    data class Group(val title: String, val codes: List<Int>)

    private val names: Map<Int, String> = buildMap {
        // Letters 0x04..0x1D are A..Z in usage order.
        ('A'..'Z').forEachIndexed { index, letter -> put(0x04 + index, letter.toString()) }
        // Digits 0x1E..0x26 are 1..9, then 0 at 0x27.
        (1..9).forEachIndexed { index, digit -> put(0x1E + index, digit.toString()) }
        put(0x27, "0")
        put(0x28, "Enter"); put(0x29, "Esc"); put(0x2A, "Backspace"); put(0x2B, "Tab")
        put(0x2C, "Space"); put(0x2D, "-"); put(0x2E, "="); put(0x2F, "[")
        put(0x30, "]"); put(0x31, "\\"); put(0x33, ";"); put(0x34, "'")
        put(0x35, "`"); put(0x36, ","); put(0x37, "."); put(0x38, "/")
        put(0x39, "Caps Lock")
        (1..12).forEachIndexed { index, number -> put(0x3A + index, "F$number") }
        put(0x46, "Print Screen"); put(0x47, "Scroll Lock"); put(0x48, "Pause")
        put(0x49, "Insert"); put(0x4A, "Home"); put(0x4B, "Page Up")
        put(0x4C, "Delete"); put(0x4D, "End"); put(0x4E, "Page Down")
        put(0x4F, "Right Arrow"); put(0x50, "Left Arrow")
        put(0x51, "Down Arrow"); put(0x52, "Up Arrow")
        put(0x53, "Num Lock"); put(0x54, "Numpad /"); put(0x55, "Numpad *")
        put(0x56, "Numpad -"); put(0x57, "Numpad +"); put(0x58, "Numpad Enter")
        (1..9).forEachIndexed { index, digit -> put(0x59 + index, "Numpad $digit") }
        put(0x62, "Numpad 0"); put(0x63, "Numpad .")
        put(0xE0, "Left Ctrl"); put(0xE1, "Left Shift"); put(0xE2, "Left Alt"); put(0xE3, "Left Meta")
        put(0xE4, "Right Ctrl"); put(0xE5, "Right Shift"); put(0xE6, "Right Alt"); put(0xE7, "Right Meta")
    }

    fun label(code: Int): String = names[code] ?: "Key 0x%02X".format(code)

    /** True when the id has a real name; used to keep the picker free of unknowns. */
    fun isNamed(code: Int): Boolean = names.containsKey(code)

    val groups: List<Group> = listOf(
        Group("Letters", (0x04..0x1D).toList()),
        Group("Numbers", (0x1E..0x27).toList()),
        Group("Modifiers", listOf(0xE1, 0xE0, 0xE2, 0xE3, 0xE5, 0xE4, 0xE6, 0xE7)),
        Group("Editing", listOf(0x2C, 0x28, 0x29, 0x2A, 0x2B, 0x49, 0x4C, 0x4A, 0x4D, 0x4B, 0x4E)),
        Group("Arrows", listOf(0x52, 0x51, 0x50, 0x4F)),
        Group("Function", (0x3A..0x45).toList()),
        Group("Punctuation", listOf(0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38)),
        Group("Numpad", listOf(0x54, 0x55, 0x56, 0x57, 0x58, 0x62) + (0x59..0x61).toList() + listOf(0x63)),
    )

    /** Every named, bindable usage, ordered as the groups present them. */
    val allBindable: List<Int> = groups.flatMap { it.codes }.distinct()
        .filter { it in KbmSource.KEY_USAGE_MIN..KbmSource.KEY_USAGE_MAX && isNamed(it) }
}

/** The standard five-button mouse contract the firmware accepts. */
object MouseButtons {
    private val names = mapOf(
        1 to "Left Button",
        2 to "Right Button",
        3 to "Middle Button",
        4 to "Back (Mouse 4)",
        5 to "Forward (Mouse 5)",
    )

    fun label(code: Int): String = names[code] ?: "Mouse $code"

    val all: List<Int> = names.keys.sorted()
}

// ---------------------------------------------------------------------------
// Destinations
// ---------------------------------------------------------------------------

/**
 * A controller output a source can be bound to.
 *
 * The wire names are the firmware's `KBM_DESTINATION_NAMES` table verbatim.
 * [group] exists purely so a 30-entry picker can be scanned; it carries no
 * behaviour.
 */
typealias KbmDestination = dev.picoswitch.management.KbmDestination

val KbmDestination.title: String get() = when (this) {
    KbmDestination.None -> "Unassigned"
    KbmDestination.A -> "A"; KbmDestination.B -> "B"; KbmDestination.X -> "X"; KbmDestination.Y -> "Y"
    KbmDestination.L -> "L"; KbmDestination.R -> "R"; KbmDestination.Zl -> "ZL"; KbmDestination.Zr -> "ZR"
    KbmDestination.Gl -> "GL (left grip)"; KbmDestination.Gr -> "GR (right grip)"
    KbmDestination.L3 -> "Left Stick Click"; KbmDestination.R3 -> "Right Stick Click"
    KbmDestination.DUp -> "D-Pad Up"; KbmDestination.DDown -> "D-Pad Down"
    KbmDestination.DLeft -> "D-Pad Left"; KbmDestination.DRight -> "D-Pad Right"
    KbmDestination.Minus -> "Minus"; KbmDestination.Plus -> "Plus"; KbmDestination.Home -> "Home"
    KbmDestination.Capture -> "Capture"; KbmDestination.C -> "C (GameChat)"
    KbmDestination.LStickUp -> "Left Stick Up"; KbmDestination.LStickDown -> "Left Stick Down"
    KbmDestination.LStickLeft -> "Left Stick Left"; KbmDestination.LStickRight -> "Left Stick Right"
    KbmDestination.RStickUp -> "Right Stick Up"; KbmDestination.RStickDown -> "Right Stick Down"
    KbmDestination.RStickLeft -> "Right Stick Left"; KbmDestination.RStickRight -> "Right Stick Right"
}

val KbmDestination.group: String get() = when (this) {
    KbmDestination.None -> "Other"
    KbmDestination.A, KbmDestination.B, KbmDestination.X, KbmDestination.Y -> "Face buttons"
    KbmDestination.L, KbmDestination.R, KbmDestination.Zl, KbmDestination.Zr,
    KbmDestination.Gl, KbmDestination.Gr -> "Shoulders & triggers"
    KbmDestination.L3, KbmDestination.R3 -> "Stick clicks"
    KbmDestination.DUp, KbmDestination.DDown, KbmDestination.DLeft, KbmDestination.DRight -> "D-Pad"
    KbmDestination.Minus, KbmDestination.Plus, KbmDestination.Home,
    KbmDestination.Capture, KbmDestination.C -> "System"
    KbmDestination.LStickUp, KbmDestination.LStickDown,
    KbmDestination.LStickLeft, KbmDestination.LStickRight -> "Left stick"
    KbmDestination.RStickUp, KbmDestination.RStickDown,
    KbmDestination.RStickLeft, KbmDestination.RStickRight -> "Right stick"
}

/** Assignable choices, grouped for the picker. [KbmDestination.None] is separate. */
val kbmDestinationGroups: List<Pair<String, List<KbmDestination>>>
    get() = KbmDestination.entries.filter { it != KbmDestination.None }.groupBy { it.group }.toList()

/**
 * The product-visible name of an admitted keyboard or mouse, if it can be
 * established.
 *
 * `kbm status` reports only a transport connection index per role. The peer
 * names live in the separate `input sources` reply, keyed by the same index --
 * `ns2_input_source_key_t.dev_addr` is documented as the transport connection
 * index, and `ns2_kbm_runtime` submits its events into that same arbiter. When
 * the two replies do not agree on an index, this returns null so the row shows
 * a plain "Connected" rather than borrowing a name from an unrelated source.
 * Showing the attached game controller's name there was exactly that mistake.
 */
fun resolveKbmDeviceName(
    connected: Boolean,
    conn: Int,
    sources: List<AdapterInputSource>,
): String? {
    if (!connected) return null
    return sources.firstOrNull { it.connection == conn }?.name?.takeIf(String::isNotBlank)
}

/** One row of `kbm map`: what the adapter currently does with one input. */
typealias KbmBinding = dev.picoswitch.management.KbmBinding

/** One profile's complete effective binding set, assembled from every page. */
typealias KbmMapping = dev.picoswitch.management.KbmMapping

/** One page of `kbm map <profile> <page>`. */
typealias KbmMapPage = dev.picoswitch.management.KbmMapPage

// --- profile library -------------------------------------------------------
// A profile is a NAMED user mapping within a layout, and it is the user's
// choice. The layout above is the SHAPE of the mapping and is derived from
// which roles are filled. Save stores a profile; Apply is what changes the
// console. Those three distinctions are the feature.

/** One named mapping the user can select, within one layout. */
typealias KbmProfileInfo = dev.picoswitch.management.KbmProfileInfo

/** What a layout is REALLY resolving against, and whether it still matches. */
typealias KbmActiveMapping = dev.picoswitch.management.KbmActiveMapping

/** The adapter's profile library and both realized mappings. */
typealias KbmProfiles = dev.picoswitch.management.KbmProfiles

/** Reserved profile identities; Default is a template, not a stored profile. */
typealias KbmProfileIds = dev.picoswitch.management.KbmProfileIds

/** A local, editable copy of one profile. Editing it sends nothing. */
typealias KbmDraft = dev.picoswitch.management.KbmDraft

/** Where an open draft stands against adapter truth. */
typealias KbmDraftState = dev.picoswitch.management.KbmDraftState

// ---------------------------------------------------------------------------
// Mouse translation
// ---------------------------------------------------------------------------

/**
 * Mouse-to-stick translation settings and the ranges the adapter reported for
 * them.
 *
 * The limits travel with the values on purpose (see `ns2_kbm_mouse_format`): a
 * client that kept its own copy of the accepted range would refuse values a
 * newer firmware had widened. Nothing here re-declares a bound.
 */
typealias KbmMouseConfig = dev.picoswitch.management.KbmMouseConfig

/** The wire field names `kbm mouse <field> <value>` accepts. */
typealias KbmMouseField = dev.picoswitch.management.KbmMouseField

/**
 * Maps the wide sensitivity range onto a slider position.
 *
 * The accepted range spans 16..8192 -- a factor of 512 -- so a linear slider
 * spends nine tenths of its travel above any usable setting and cannot resolve
 * the low end at all. A logarithmic position gives constant *relative*
 * precision instead: one slider step is the same percentage change everywhere,
 * which is how sensitivity is actually perceived.
 *
 * Pure and total so the round trip is unit-testable; both directions clamp to
 * the adapter-reported bounds rather than the app's assumptions.
 */
object SensitivityScale {
    fun toPosition(value: Int, min: Int, max: Int): Float {
        if (max <= min) return 0f
        val clamped = value.coerceIn(min, max)
        val span = Math.log(max.toDouble() / min.toDouble())
        if (span <= 0.0) return 0f
        return (Math.log(clamped.toDouble() / min.toDouble()) / span).toFloat().coerceIn(0f, 1f)
    }

    fun fromPosition(position: Float, min: Int, max: Int): Int {
        if (max <= min) return min
        val clamped = position.coerceIn(0f, 1f)
        val span = Math.log(max.toDouble() / min.toDouble())
        val value = min.toDouble() * Math.exp(clamped.toDouble() * span)
        return Math.round(value).toInt().coerceIn(min, max)
    }

    /** One slider notch, chosen so a drag resolves ~1% steps across the range. */
    fun steps(min: Int, max: Int): Int {
        if (max <= min) return 0
        val decades = Math.log(max.toDouble() / min.toDouble()) / Math.log(10.0)
        return (decades * 40).toInt().coerceIn(0, 400)
    }
}

/**
 * Everything the Keyboard & Mouse page needs, and whether it is saved.
 *
 * [dirty] means "changed on this connection and not persisted since". The
 * adapter applies KB/M changes to RAM immediately and only writes flash on
 * `save`, and the protocol offers no way to ask whether an arbitrary runtime
 * value matches what is stored -- so a fresh connection starts clean by
 * definition rather than by inference, and the UI never claims a value is
 * stored merely because it took effect.
 */
/**
 * The Keyboard and Mouse screen's top-level state. Exactly one is true at a time.
 *
 * Explicit because the implicit version shipped a bad failure: the screen
 * inferred readiness from flags and quietly degraded to a pre-profile editor
 * whenever the profile contract did not answer. When a protocol defect made the
 * read fail, the user saw the old half-working mapping page with no profile
 * controls and no statement that anything had gone wrong — indistinguishable
 * from the feature not having been built.
 *
 * There is no legacy fallback. This companion targets ONE firmware contract.
 */
enum class KbmReadiness {
    /** Never read this session. */
    NotRead,

    /** Read in progress. */
    Loading,

    /** The current contract loaded. The only state with a usable screen. */
    Ready,

    /**
     * The adapter answered, but does not implement a command the current
     * contract requires. Its firmware predates the profile system.
     */
    FirmwareUpdateRequired,

    /**
     * The adapter implements the contract but returned data this build could not
     * use — malformed, incomplete or inconsistent. A defect, not a version gap.
     */
    Error,
}

data class KbmState(
    val status: KbmStatus = KbmStatus(),
    val mouse: KbmMouseConfig = KbmMouseConfig(),
    val mappings: Map<KbmProfile, KbmMapping> = emptyMap(),
    val available: CapabilityState = CapabilityState.Unknown,
    val dirty: Boolean = false,
    val saving: Boolean = false,
    val loading: Boolean = false,

    val readiness: KbmReadiness = KbmReadiness.NotRead,

    /**
     * Why the screen is not Ready, in developer terms. Shown on the screen and
     * copyable; the generic banner it replaces cost a hardware round trip to turn
     * into a diagnosis.
     */
    val fault: String = "",

    /**
     * The adapter's profile library and both realized mappings. Required by the
     * current contract: an adapter that cannot list them is reported as needing a
     * firmware update, never silently treated as having none.
     */
    val profiles: KbmProfiles = KbmProfiles(),

    /**
     * The local, unsaved copy of the profile being edited.
     *
     * Every edit in the UI mutates THIS and nothing else. No management command
     * is sent until the user saves, which is what makes Save and Discard mean
     * anything and what stops a flash erase per keystroke.
     */
    val draft: KbmDraft? = null,
) {
    fun mapping(profile: KbmProfile): KbmMapping = mappings[profile] ?: KbmMapping(profile)

    /** The layout whose bindings are currently in force on the adapter. */
    val activeProfile: KbmProfile get() = status.profile

    /** Where the open draft stands against adapter truth. */
    fun draftState(connected: Boolean): KbmDraftState =
        draft?.stateAgainst(profiles, connected)
            ?: if (connected) KbmDraftState.Clean else KbmDraftState.Disconnected

    /**
     * The bindings the editor should show: the DRAFT when one is open, so an
     * edit appears immediately without any adapter write.
     */
    fun editorBindings(layout: KbmProfile): List<KbmBinding> =
        draft?.takeIf { it.layout == layout }?.bindings ?: mapping(layout).bindings

    /**
     * Mouse tuning only affects the translated-stick path. When the adapter is
     * emitting native pointer reports the controls remain visible but are
     * honestly marked as not currently in effect.
     */
    val mouseTuningInEffect: Boolean get() = !status.nativeMouseOutput
}
