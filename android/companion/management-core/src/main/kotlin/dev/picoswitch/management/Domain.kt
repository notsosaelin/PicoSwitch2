package dev.picoswitch.management

enum class Personality(val wireName: String, val title: String) {
    Pro2("pro2", "Pro Controller 2"),
    GameCube("gc", "GameCube"),
    JoyConLeft("jcl", "Joy-Con 2 (L)"),
    JoyConRight("jcr", "Joy-Con 2 (R)"),
    Config("config", "Configuration"),
    Unknown("unknown", "Unknown");

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
    fun argb() = 0xFF000000L.or((red.toLong() shl 16)).or((green.toLong() shl 8)).or(blue.toLong())
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
    val wake: CapabilityState = CapabilityState.Unknown,
    val activeInput: CapabilityState = CapabilityState.Unknown,
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

enum class KbmProfile(val wire: String) {
    Keyboard("kb"),
    KeyboardMouse("kbm");

    companion object {
        fun fromWire(value: String?): KbmProfile? = entries.firstOrNull { it.wire == value }
    }
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
    AntiDeadzone("antideadzone"),
}

enum class WakeResult { Pending, Advertised, ConsoleAwake, NoIdentity, RadioBusy, Unknown }
data class WakeStatus(
    val result: WakeResult,
    val consoleAsleep: Boolean,
    val identityValid: Boolean,
    val attempts: Long,
)

data class CommandAcknowledgement(
    val queued: Boolean = false,
    val switching: Boolean = false,
    val unchanged: Boolean = false,
    val reenumerating: Boolean = false,
    val enabled: Boolean? = null,
)

enum class PersistenceState { Accepted, Queued }
data class PersistenceAcknowledgement(val state: PersistenceState)

data class AmiiboDownload(val bytes: ByteArray, val generation: Long, val payloadCrc: String?)

enum class ColorTarget(val command: String) {
    Body("body"),
    LeftAccent("jcl"),
    RightAccent("jcr"),
}
