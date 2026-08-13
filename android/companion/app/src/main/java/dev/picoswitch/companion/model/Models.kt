package dev.picoswitch.companion.model

enum class ConnectionPhase { Idle, Scanning, Connecting, Connected, Reconnecting, Disconnecting, Failed }

data class ConnectionState(
    val phase: ConnectionPhase = ConnectionPhase.Idle,
    val deviceName: String? = null,
    val address: String? = null,
    val message: String? = null,
    val attempt: Int = 0,
) {
    val connected: Boolean get() = phase == ConnectionPhase.Connected
}

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

data class FirmwareInfo(val id: String = "", val product: String = "", val version: String = "")

data class ControllerInfo(
    val name: String = "No controller",
    val vid: Int = 0,
    val pid: Int = 0,
    val batteryValid: Boolean = false,
    val batteryPercent: Int = 0,
    val charging: Boolean = false,
) {
    val attached: Boolean get() = vid != 0 || pid != 0 || name != "No controller"
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

enum class CapabilityState { Available, Unsupported, Unknown }

data class AdapterCapabilities(
    val core: CapabilityState = CapabilityState.Unknown,
    val personality: CapabilityState = CapabilityState.Unknown,
    val colors: CapabilityState = CapabilityState.Unknown,
    val amiibo: CapabilityState = CapabilityState.Unknown,
    val managementGate: CapabilityState = CapabilityState.Unknown,
    val bonds: CapabilityState = CapabilityState.Unknown,
    val wake: CapabilityState = CapabilityState.Unknown,
)

data class AdapterSnapshot(
    val firmware: FirmwareInfo = FirmwareInfo(),
    val controller: ControllerInfo = ControllerInfo(),
    val personality: PersonalityState = PersonalityState(),
    val config: AdapterConfig = AdapterConfig(),
    val amiibo: AmiiboStatus = AmiiboStatus(),
    val managementEnabled: Boolean? = null,
    val bonds: List<BondInfo> = emptyList(),
    val capabilities: AdapterCapabilities = AdapterCapabilities(),
    val refreshedAtMillis: Long = 0,
)

data class AmiiboLibraryItem(
    val id: String,
    val displayName: String,
    val fileName: String,
    val size: Int,
    val crc32: String,
    val uid: String,
    val figureId: String,
    val importedAtMillis: Long,
    val updatedAtMillis: Long = importedAtMillis,
    val dirtyFromAdapter: Boolean = false,
)

data class OperationProgress(
    val label: String,
    val completed: Int,
    val total: Int,
) {
    val fraction: Float get() = if (total <= 0) 0f else completed.toFloat() / total.toFloat()
}
