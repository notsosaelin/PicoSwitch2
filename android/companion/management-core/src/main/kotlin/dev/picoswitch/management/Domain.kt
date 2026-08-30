package dev.picoswitch.management

enum class Personality(val wireName: String) {
    Pro2("pro2"),
    GameCube("gc"),
    JoyConLeft("jcl"),
    JoyConRight("jcr"),
    Config("config"),
    Unknown("unknown");

    companion object {
        fun fromWire(value: String?) = entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

data class FirmwareInfo(
    val id: String = "",
    val product: String = "",
    val version: String = "",
    val bridgeContract: Int = 0,
    val build: String = "",
)

data class ControllerInfo(
    val name: String = "No controller",
    val vid: Int = 0,
    val pid: Int = 0,
    val batteryValid: Boolean = false,
    val batteryPercent: Int = 0,
    val charging: Boolean = false,
) {
    val attached: Boolean get() = vid != 0 || pid != 0 ||
        (name.isNotBlank() && name != "No controller")
}

data class RgbColor(val red: Int, val green: Int, val blue: Int) {
    init {
        require(red in 0..255 && green in 0..255 && blue in 0..255)
    }

    fun wire() = "$red $green $blue"
}

data class AdapterConfig(
    val bodyColor: RgbColor = RgbColor(0, 0, 0),
    val leftAccent: RgbColor = RgbColor(0, 0, 0),
    val rightAccent: RgbColor = RgbColor(0, 0, 0),
)

data class PersonalityState(
    val current: Personality = Personality.Unknown,
    val available: List<Personality> = emptyList(),
)

data class AmiiboUpload(val active: Boolean = false, val received: Int = 0, val size: Int = 0)

data class AmiiboStatus(
    val loaded: Boolean = false,
    val dirty: Boolean = false,
    val presented: Boolean = false,
    val v3Loaded: Boolean = false,
    val persisted: Boolean = false,
    val persistPending: Boolean = false,
    val size: Int = 0,
    val signature: Boolean = false,
    val hasSave2: Boolean = false,
    val usingSave2: Boolean = false,
    val generation: Long = 0,
    val payloadCrc: String = "00000000",
    val uid: String = "",
    val figureId: String = "",
    val upload: AmiiboUpload = AmiiboUpload(),
)

data class BondInfo(val index: Int, val address: String, val name: String? = null, val type: Int? = null)
data class BondPage(val entries: List<BondInfo>, val total: Int, val next: Int?)
data class BondEnumeration(val entries: List<BondInfo>, val complete: Boolean, val total: Int? = null)

/**
 * What a stored peer is to the user.
 *
 * `Unknown` is a real answer, not a parse failure. The adapter has no persistent
 * role metadata, so a bond whose owner has not been seen since the adapter
 * booted genuinely cannot be classified — and a controller list that guessed
 * would eventually offer to forget the user's own phone.
 *
 * Unrecognised wire values also land here, which is the same statement: this
 * build does not know what that is.
 */
enum class PeerRole(val wireName: String) {
    ManagementCompanion("management"),
    ControllerLink("controller_link"),
    PhysicalController("controller"),
    Unknown("unknown");

    companion object {
        fun fromWire(value: String?): PeerRole =
            entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

/**
 * One logical remote device the adapter knows.
 *
 * Not a bond row. One peer may hold a Classic link key, an LE bond, or both —
 * the management phone routinely holds both — and [transports] is how that is
 * expressed without showing one device twice.
 *
 * Contains no key material and never will: the firmware's peer record has
 * nowhere to put any.
 */
data class PeerInfo(
    /** Opaque, stable, firmware-assigned. Not a database index; those get reused. */
    val id: String,
    val address: String,
    val role: PeerRole = PeerRole.Unknown,
    val transports: Set<PeerTransport> = emptySet(),
    val bonded: Boolean = false,
    val connected: Boolean = false,
    /** Remote-supplied name, sanitised by the adapter. Whatever the device calls itself. */
    val name: String? = null,
    /**
     * What the adapter's own driver stack decided the device IS, e.g.
     * `Sony DualSense`. Derived identity rather than a claim by the device,
     * which is why it outranks [name] when labelling a controller.
     *
     * Null means the adapter cannot say. A bonded peer that is not connected has
     * no driver bound and therefore never carries one; that gap is what the
     * app-side history exists to cover.
     */
    val classification: String? = null,
    val vendorId: Int = 0,
    val productId: Int = 0,
) {
    /** A peer with entries on both transports, which selective forget must treat as one device. */
    val multiTransport: Boolean get() = transports.size > 1

    /** 0/0 means the adapter has no identity for this peer, not that it is device 0000:0000. */
    val hasUsbIdentity: Boolean get() = vendorId != 0 || productId != 0
}

/**
 * The display-name hierarchy for one remote device (design §20).
 *
 * Ordered by how much anyone can actually vouch for the answer:
 *
 *  1. a user alias, which is the user's own decision and outranks everything;
 *  2. the adapter's classification, derived from VID/PID and the HID descriptor
 *     rather than supplied by the device;
 *  3. the remote-supplied name, which is only ever a claim by the device;
 *  4. the USB identity, when the adapter has one but no driver name for it;
 *  5. a short identity suffix, so two unnamed devices stay distinguishable.
 *
 * The final fallback is deliberately not the bare address. An address rendered
 * where a name belongs reads as a name, and this one is not one.
 */
object PeerNaming {
    fun label(
        address: String,
        alias: String? = null,
        classification: String? = null,
        name: String? = null,
        vendorId: Int = 0,
        productId: Int = 0,
    ): String = alias?.takeIf(String::isNotBlank)
        ?: classification?.takeIf(String::isNotBlank)
        ?: name?.takeIf(String::isNotBlank)
        ?: usbIdentity(vendorId, productId)
        ?: "Controller • ${shortLabel(address)}"

    /** Four hex characters of the identity address. Presentation only, never identity. */
    fun shortLabel(address: String): String =
        address.filter { it.isLetterOrDigit() }.takeLast(4).uppercase().ifBlank { "????" }

    private fun usbIdentity(vendorId: Int, productId: Int): String? =
        if (vendorId == 0 && productId == 0) null
        else "Device %04X:%04X".format(vendorId, productId)
}

enum class PeerTransport(val bit: Int) {
    Classic(0x01),
    Le(0x02);

    companion object {
        fun fromMask(mask: Int): Set<PeerTransport> =
            entries.filter { mask and it.bit != 0 }.toSet()
    }
}

/**
 * What a forget attempt did, as the adapter verified it.
 *
 * Three of these are outcomes rather than errors, because "forget" asks for an
 * end state, not for an event.
 */
enum class PeerForgetResult(val wireName: String) {
    /** A record existed and is gone. The adapter re-enumerated to confirm it. */
    Removed("removed"),

    /**
     * Nothing to do, and a SUCCESS. A management reply can be lost after the
     * command already ran, so a retry must not report failure for completed
     * work.
     */
    AlreadyAbsent("already_absent"),

    /** The adapter refused: this peer is its management companion. */
    ManagementPeer("management_peer"),

    /** The delete ran and the peer still holds a credential. Never smoothed over. */
    Incomplete("incomplete"),

    /** A result this build does not recognise. Treated as "refresh and look". */
    Unknown("unknown");

    val succeeded: Boolean get() = this == Removed || this == AlreadyAbsent

    companion object {
        fun fromWire(value: String?): PeerForgetResult =
            entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

/**
 * The adapter's verified answer to one forget.
 *
 * [stillBonded] is the state the adapter observed AFTER deleting, not what it
 * intended. A client must trust this over its own optimism.
 */
data class PeerForgetOutcome(
    val peerId: String,
    val result: PeerForgetResult,
    val stillBonded: Boolean,
    val transports: Set<PeerTransport> = emptySet(),
)

/**
 * Where a remote controller-pairing operation has got to.
 *
 * The adapter runs ONE pairing state machine — the same one its own pairing
 * button drives — so these states describe that machine, not a second flow the
 * app owns.
 */
enum class PairingState(val wireName: String) {
    Idle("idle"),
    Discovering("discovering"),
    Connecting("connecting"),
    Paired("paired"),
    TimedOut("timed_out"),
    Cancelled("cancelled"),
    /** The adapter refused to start; [PairingStatus.reason] says why. */
    Blocked("blocked"),
    Unknown("unknown");

    /** Still running, so the app should keep polling. */
    val active: Boolean get() = this == Discovering || this == Connecting

    companion object {
        fun fromWire(value: String?): PairingState =
            entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

/** Machine-readable failure causes. The adapter names them; the app words them. */
enum class PairingReason(val wireName: String) {
    None("none"),
    NoController("no_controller"),
    ManagementDisabled("management_disabled"),
    Busy("busy"),
    LockedOut("locked_out"),
    /** Both security stores are full; the user must forget a controller first. */
    StorageFull("storage_full"),
    Unknown("unknown");

    companion object {
        fun fromWire(value: String?): PairingReason =
            entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

/**
 * One pairing operation as the adapter reports it.
 *
 * [operation] is a generation, not a handle. A status arriving for an older
 * operation — after an adapter switch, or a reply the app missed — must never
 * be allowed to describe the current one.
 */
data class PairingStatus(
    val operation: Long = 0,
    val state: PairingState = PairingState.Idle,
    val reason: PairingReason = PairingReason.None,
    val remainingMillis: Long = 0,
    val candidates: Int = 0,
) {
    val active: Boolean get() = state.active
}

data class PeerPage(val entries: List<PeerInfo>, val total: Int, val next: Int?)
data class PeerInventory(
    val peers: List<PeerInfo> = emptyList(),
    val complete: Boolean = false,
    val total: Int = 0,
) {
    /** What the Controllers page shows. Deliberately excludes this phone in either of its roles. */
    val controllers: List<PeerInfo> get() = peers.filter { it.role == PeerRole.PhysicalController }
    /** Companion/advanced rows: the management phone, Controller Link, and anything unclassified. */
    val companionsAndUnknown: List<PeerInfo> get() = peers.filterNot { it.role == PeerRole.PhysicalController }
}

data class AdapterInputSource(
    val id: Long,
    val connection: Int,
    val transport: Int,
    val generation: Long,
    val name: String,
)

data class AdapterInputState(
    val activeId: Long = 0,
    val pendingId: Long = 0,
    val explicit: Boolean = false,
    val awaitingFresh: Boolean = false,
    val transitions: Long = 0,
    val sources: List<AdapterInputSource> = emptyList(),
    val truncated: Boolean = false,
) {
    val activeSource: AdapterInputSource? get() = sources.firstOrNull { it.id == activeId }
}

enum class CapabilityState { Available, Unsupported, Unknown }

data class AdapterCapabilities(
    val core: CapabilityState = CapabilityState.Unknown,
    val personality: CapabilityState = CapabilityState.Unknown,
    val colors: CapabilityState = CapabilityState.Unknown,
    val amiibo: CapabilityState = CapabilityState.Unknown,
    val managementGate: CapabilityState = CapabilityState.Unknown,
    val bonds: CapabilityState = CapabilityState.Unknown,
    val peers: CapabilityState = CapabilityState.Unknown,
    /**
     * Selective forget and remote pairing, probed separately from [peers].
     *
     * An adapter can list peers without being able to forget one: `peers list`
     * shipped a phase before `peers forget`. Treating them as one capability
     * would either hide a working list or offer a Forget button that answers
     * `unknown command`, and design §87 is explicit that one missing capability
     * must not hide the whole adapter.
     */
    val peerForget: CapabilityState = CapabilityState.Unknown,
    val remotePairing: CapabilityState = CapabilityState.Unknown,
    val wake: CapabilityState = CapabilityState.Unknown,
    val activeInput: CapabilityState = CapabilityState.Unknown,
    val kbm: CapabilityState = CapabilityState.Unknown,
)

data class AdapterSnapshot(
    val firmware: FirmwareInfo = FirmwareInfo(),
    val controller: ControllerInfo = ControllerInfo(),
    val personality: PersonalityState = PersonalityState(),
    val config: AdapterConfig = AdapterConfig(),
    val amiibo: AmiiboStatus = AmiiboStatus(),
    val managementEnabled: Boolean? = null,
    val bonds: List<BondInfo> = emptyList(),
    val bondsComplete: Boolean? = null,
    val bondsTotal: Int? = null,
    /** Logical peers, as the adapter reports them. Never inferred by the app. */
    val peers: PeerInventory = PeerInventory(),
    val input: AdapterInputState = AdapterInputState(),
    val capabilities: AdapterCapabilities = AdapterCapabilities(),
    val refreshedAtMillis: Long = 0,
)

data class ManagementRefresh(
    val snapshot: AdapterSnapshot,
    val kbmStatus: KbmStatus? = null,
    val kbmMouse: KbmMouseConfig? = null,
)

enum class KbmMode(val wire: String) {
    Automatic("auto"),
    Controller("controller"),
    Keyboard("keyboard"),
    KeyboardMouse("kbmouse");

    companion object {
        fun fromWire(value: String?): KbmMode? = entries.firstOrNull { it.wire == value }
    }
}

/**
 * The SHAPE of a mapping: which source inputs exist and which canonical defaults
 * apply. Derived from which peer roles are filled and never chosen by the user —
 * asserting Keyboard + Mouse with no mouse would silently drop the right stick.
 *
 * Deliberately not called a profile. A profile is a named override set the user
 * selects WITHIN a layout; conflating the two is what let a binding be saved into
 * a mapping the adapter was not resolving, report success, and do nothing at the
 * console. The wire names are unchanged.
 */
enum class KbmProfile(val wire: String) {
    Keyboard("kb"),
    KeyboardMouse("kbm");

    companion object {
        fun fromWire(value: String?): KbmProfile? = entries.firstOrNull { it.wire == value }
    }
}

/** Reserved profile identities. Custom profiles are numbered from 2. */
object KbmProfileIds {
    const val NONE = 0

    /**
     * The built-in Default template of a layout. Not a stored profile: it
     * consumes no slot, cannot be renamed or deleted, and is always available as
     * the fallback.
     */
    const val DEFAULT = 1
}

/**
 * One named mapping the user can select, within one layout.
 *
 * [id] is stable across storage-slot reuse, so a cached draft can never come back
 * referring to an unrelated mapping. [revision] guards saves: a draft carries the
 * revision it was based on, and a mismatch is a conflict rather than a silent
 * overwrite of what another companion stored.
 */
data class KbmProfileInfo(
    val id: Int,
    val layout: KbmProfile,
    val name: String,
    val revision: Int,
    val overrides: Int,
    val fingerprint: Long,
) {
    /** Built-in Defaults are synthesised locally, never stored. */
    val builtin: Boolean get() = id == KbmProfileIds.DEFAULT
}

/**
 * What a layout is REALLY resolving against.
 *
 * [matchesSaved] is the field a UI must believe. An id alone cannot express
 * "saved but not applied": it survives a save that was never applied, and a
 * legacy per-binding write changes the realized mapping without touching any
 * saved profile at all.
 */
data class KbmActiveMapping(
    val layout: KbmProfile,
    val sourceId: Int,
    val revision: Int,
    val fingerprint: Long,
    val matchesSaved: Boolean,
)

/** The adapter's profile library and both realized mappings. */
data class KbmProfiles(
    val profiles: List<KbmProfileInfo> = emptyList(),
    val active: List<KbmActiveMapping> = emptyList(),
    val max: Int = 0,
) {
    /** True when the adapter reported a profile library at all. */
    val supported: Boolean get() = max > 0

    val full: Boolean get() = max > 0 && profiles.size >= max

    fun activeFor(layout: KbmProfile): KbmActiveMapping? =
        active.firstOrNull { it.layout == layout }

    fun find(id: Int): KbmProfileInfo? = profiles.firstOrNull { it.id == id }

    /**
     * Every profile a layout can offer, with its built-in Default first.
     *
     * Default is synthesised rather than read: the adapter does not store it,
     * which is exactly what keeps all six slots available to the user.
     */
    fun forLayout(layout: KbmProfile): List<KbmProfileInfo> =
        listOf(
            KbmProfileInfo(
                id = KbmProfileIds.DEFAULT,
                layout = layout,
                name = "Default",
                revision = 0,
                overrides = 0,
                fingerprint = 0,
            ),
        ) + profiles.filter { it.layout == layout }
}

data class KbmStatus(
    val mode: KbmMode = KbmMode.Automatic,
    val modeOverride: KbmMode = KbmMode.Automatic,
    val profile: KbmProfile = KbmProfile.Keyboard,
    val keyboardConnected: Boolean = false,
    val mouseConnected: Boolean = false,
    val nativeMouseOutput: Boolean = false,
    val keyboardConn: Int = 0,
    val mouseConn: Int = 0,
    val keyboardReports: Long = 0,
    val mouseReports: Long = 0,
    val rejectedMode: Long = 0,
    val rejectedDuplicate: Long = 0,
    val rejectedNotOwner: Long = 0,
    val rollover: Long = 0,
    val roleLosses: Long = 0,
    val mapGeneration: Long = 0,
    val publishes: Long = 0,
    val recenters: Long = 0,

    // What the live layout is REALLY running. activeMatchesSaved is the one a UI
    // must believe: false after the source profile was edited and saved without
    // applying, and after a legacy per-binding write mutated the realized
    // mapping. An id alone expresses neither.
    val activeProfile: Int = 0,
    val activeProfileName: String = "",
    val activeRevision: Int = 0,
    val activeFingerprint: Long = 0,
    val activeMatchesSaved: Boolean = false,
) {
    val anyDeviceConnected: Boolean get() = keyboardConnected || mouseConnected
}

enum class KbmSourceKind { Key, MouseButton }

data class KbmSource(val kind: KbmSourceKind, val code: Int) {
    init {
        require(
            (kind == KbmSourceKind.Key && code in KEY_USAGE_MIN..KEY_USAGE_MAX) ||
                (kind == KbmSourceKind.MouseButton && code in MOUSE_BUTTON_MIN..MOUSE_BUTTON_MAX),
        )
    }

    val wire: String
        get() = when (kind) {
            KbmSourceKind.Key -> "key:%02X".format(code)
            KbmSourceKind.MouseButton -> "mouse:$code"
        }

    companion object {
        const val KEY_USAGE_MIN = 0x04
        const val KEY_USAGE_MAX = 0xE7
        const val MOUSE_BUTTON_MIN = 1
        const val MOUSE_BUTTON_MAX = 5

        fun parse(text: String): KbmSource? {
            val prefix = text.substringBefore(':').lowercase()
            val value = text.substringAfter(':', "")
            return when (prefix) {
                "key" -> value.toIntOrNull(16)?.takeIf { it in KEY_USAGE_MIN..KEY_USAGE_MAX }
                    ?.let { KbmSource(KbmSourceKind.Key, it) }
                "mouse" -> value.toIntOrNull()?.takeIf { it in MOUSE_BUTTON_MIN..MOUSE_BUTTON_MAX }
                    ?.let { KbmSource(KbmSourceKind.MouseButton, it) }
                else -> null
            }
        }
    }
}

enum class KbmDestination(val wire: String) {
    None("none"),
    A("a"), B("b"), X("x"), Y("y"),
    L("l"), R("r"), Zl("zl"), Zr("zr"), Gl("gl"), Gr("gr"),
    L3("l3"), R3("r3"),
    DUp("dup"), DDown("ddown"), DLeft("dleft"), DRight("dright"),
    Minus("minus"), Plus("plus"), Home("home"), Capture("capture"), C("c"),
    LStickUp("lstick_up"), LStickDown("lstick_down"),
    LStickLeft("lstick_left"), LStickRight("lstick_right"),
    RStickUp("rstick_up"), RStickDown("rstick_down"),
    RStickLeft("rstick_left"), RStickRight("rstick_right");

    companion object {
        fun fromWire(value: String?): KbmDestination? = entries.firstOrNull { it.wire == value }
    }
}

data class KbmBinding(val source: KbmSource, val destination: KbmDestination, val custom: Boolean)

data class KbmMapping(
    val profile: KbmProfile,
    val bindings: List<KbmBinding> = emptyList(),
    val loaded: Boolean = false,
) {
    val keyBindings: List<KbmBinding> get() = bindings.filter { it.source.kind == KbmSourceKind.Key }
    val mouseBindings: List<KbmBinding> get() = bindings.filter { it.source.kind == KbmSourceKind.MouseButton }
    val customCount: Int get() = bindings.count { it.custom }
}

data class KbmMapPage(
    val profile: KbmProfile,
    val page: Int,
    val pageSize: Int,
    val total: Int,
    val bindings: List<KbmBinding>,
    val more: Boolean,
)

data class KbmMouseConfig(
    val sensitivityX: Int = 0,
    val sensitivityY: Int = 0,
    val velocityWindowMs: Int = 0,
    val invertX: Boolean = false,
    val invertY: Boolean = false,
    val antiDeadzone: Int = 0,
    val sensitivityMin: Int = 0,
    val sensitivityMax: Int = 0,
    val velocityWindowMinMs: Int = 0,
    val velocityWindowMaxMs: Int = 0,
    val antiDeadzoneMax: Int = 0,
) {
    val axesLinked: Boolean get() = sensitivityX == sensitivityY
    fun multiplier(raw: Int): Double = raw / 256.0
    val ranged: Boolean get() = sensitivityMax > sensitivityMin
}

enum class KbmMouseField(val wire: String) {
    Sensitivity("sensitivity"),
    SensitivityX("sensitivityx"),
    SensitivityY("sensitivityy"),
    VelocityWindow("recenter"),
    InvertX("invertx"),
    InvertY("inverty"),
    AntiDeadzone("antideadzone");

    companion object {
        /**
         * Every profile-owned mouse setting as (field, value), for writing a
         * whole mapping in one staged transaction.
         *
         * X and Y are sent separately rather than through the combined
         * [Sensitivity], which sets both: a profile may legitimately carry
         * different axis sensitivities, and the combined form would flatten them.
         */
        fun profileOwned(mouse: KbmMouseConfig): List<Pair<KbmMouseField, Int>> = listOf(
            SensitivityX to mouse.sensitivityX,
            SensitivityY to mouse.sensitivityY,
            VelocityWindow to mouse.velocityWindowMs,
            InvertX to if (mouse.invertX) 1 else 0,
            InvertY to if (mouse.invertY) 1 else 0,
            AntiDeadzone to mouse.antiDeadzone,
        )
    }
}

enum class WakeResult { Pending, Advertised, ConsoleAwake, NoIdentity, RadioBusy, Unknown }
data class WakeStatus(
    val result: WakeResult,
    val consoleAsleep: Boolean,
    val identityValid: Boolean,
    val attempts: Long,
    val lastAttemptMs: Long = 0,
)

data class CommandAcknowledgement(
    val queued: Boolean = false,
    val switching: Boolean = false,
    val unchanged: Boolean = false,
    val reenumerating: Boolean = false,
    val enabled: Boolean? = null,
    val requested: Long? = null,
)

enum class PersistenceState { Accepted, Queued }
data class PersistenceAcknowledgement(val state: PersistenceState, val requestId: Long? = null)
data class PersistenceStatus(val pending: Boolean, val requested: Long, val completed: Long)

data class AmiiboDownload(val bytes: ByteArray, val generation: Long, val payloadCrc: String?)

enum class ColorTarget(val command: String) {
    Body("body"),
    LeftAccent("jcl"),
    RightAccent("jcr"),
}
