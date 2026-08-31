package dev.picoswitch.companion.lab

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import dev.picoswitch.companion.data.AmiiboInteractionState
import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.ui.AppOverlay
import dev.picoswitch.companion.ui.AppSection
import dev.picoswitch.companion.ui.CompanionApp
import dev.picoswitch.companion.ui.CompanionViewModel
import dev.picoswitch.companion.ui.applyEdgeToEdgeChrome

/**
 * Debug-only layout lab.
 *
 * Renders the real application shell against a synthetic connected adapter --
 * a keyboard, a mouse, a mapping, mouse tuning, an Amiibo library -- so that
 * every screen can be inspected at every window shape without hardware. The
 * alternative is inspecting only the disconnected empty states, which is
 * exactly the half of each screen that has no layout in it.
 *
 * Not in the release variant. Launch it with:
 *
 *   adb shell am start -n dev.picoswitch.companion.debug/dev.picoswitch.companion.lab.LayoutLabActivity \
 *       --es section KEYBOARD [--es overlay DIAGNOSTICS] [--ez touch true] [--es personality gc]
 *
 * `--ez touch true` opens the on-screen controller directly, which is how its
 * geometry is inspected at an arbitrary window size without a paired adapter.
 */
class LayoutLabActivity : ComponentActivity() {
    private val viewModel: CompanionViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The lab exists to inspect the REAL shell, so it must have the real
        // window chrome: without this its insets would differ from the product's
        // and every layout judgement made here would be made against the wrong
        // rectangle.
        applyEdgeToEdgeChrome()
        val section = intent.getStringExtra("section")
            ?.let { name -> AppSection.entries.firstOrNull { it.name.equals(name, true) } }
            ?: AppSection.Adapter
        val overlay = intent.getStringExtra("overlay")
            ?.let { name -> AppOverlay.entries.firstOrNull { it.name.equals(name, true) } }
            ?: AppOverlay.None
        val empty = intent.getBooleanExtra("empty", false)

        // `--ei library 1200` to browse at the size real collections reach.
        val library = sampleLibrary(
            intent.getIntExtra("library", names.size).coerceIn(1, 5000),
        )
        val personality = Personality.fromWire(intent.getStringExtra("personality"))
            .takeIf { it in TOUCH_LAB_PERSONALITIES }
            ?: Personality.Pro2

        viewModel.applyLayoutLabState { state ->
            state.copy(
                section = section,
                overlay = overlay,
                connection = ConnectionState(
                    phase = ConnectionPhase.Connected,
                    deviceName = "PicoSwitch2",
                    address = "F4:12:FA:9C:03:7B",
                ),
                adapterRelationship = null,
                snapshot = sampleSnapshot(personality),
                kbm = if (empty) emptyKbm() else sampleKbm(),
                library = if (empty) emptyList() else library,
                amiiboCatalogEntries = if (empty) emptyMap() else sampleCatalog(library),
                amiiboInteraction = AmiiboInteractionState(
                    focusedId = if (empty) null else library.first().id,
                ),
                selectedAmiiboCatalog = if (empty) null else sampleCatalog(library).values.first(),
                amiiboKeysLoaded = true,
                nfcScan = NfcScanStatus(NfcScanPhase.Idle),
            )
        }

        // The on-screen controller is a full-screen mode rather than a section,
        // so the lab enters it the same way the product does. That is the point:
        // inspecting a mock of it would inspect the mock.
        if (intent.getBooleanExtra("touch", false)) viewModel.enterTouchGamepad()

        setContent {
            CompanionApp(
                viewModel = viewModel,
                onConnectAdapter = {}, onPairAdapter = {}, onRepairAdapter = {}, onImportAmiibo = {},
                onImportAmiiboFolder = {}, onExportAmiiboArchive = {}, onScanAmiibo = {},
                onImportAmiiboKeys = {}, onPrepareController = {}, onOpenTouchGamepad = {},
                onPickTouchBackground = {}, onExportDiagnostics = {},
            )
        }
    }
}

private fun sampleSnapshot(personality: Personality) = AdapterSnapshot(
    firmware = FirmwareInfo(
        id = "picoswitch", product = "PicoSwitch Config", version = "2.0",
        bridgeContract = 3, build = "a1b9f42+dirty",
    ),
    controller = ControllerInfo(
        name = "DualSense Wireless Controller", vid = 0x054C, pid = 0x0CE6,
        batteryValid = true, batteryPercent = 72, charging = true,
    ),
    personality = PersonalityState(
        current = personality,
        available = listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight),
    ),
    config = AdapterConfig(
        bodyColor = RgbColor(0x2D, 0x2D, 0x2D),
        leftAccent = RgbColor(0x9B, 0xE1, 0xE6),
        rightAccent = RgbColor(0xFF, 0x8C, 0x5F),
    ),
    amiibo = AmiiboStatus(
        loaded = true, presented = true, persisted = true, size = 540, signature = true,
        generation = 12, uid = "04A1B2C3D4E580", figureId = "0100000000040002",
    ),
    managementEnabled = true,
    bonds = listOf(
        BondInfo(0, "AA:BB:CC:DD:EE:FF", "Pixel 9 Pro", 1),
        BondInfo(1, "11:22:33:44:55:66", null, 0),
    ),
    bondsComplete = true,
    bondsTotal = 2,
    // The connection indices here match sampleKbm()'s keyboardConn/mouseConn so
    // the Devices rows exercise the real name resolution rather than a
    // hard-coded label.
    input = AdapterInputState(
        activeId = 1, pendingId = 0, transitions = 4,
        sources = listOf(
            AdapterInputSource(1, 0, 1, 3, "DualSense Wireless Controller"),
            AdapterInputSource(2, 1, 1, 1, "ROG FALCHION RX"),
            AdapterInputSource(3, 2, 1, 1, "ROG KERIS II ACE"),
        ),
    ),
    capabilities = AdapterCapabilities(
        core = CapabilityState.Available, personality = CapabilityState.Available,
        colors = CapabilityState.Available, amiibo = CapabilityState.Available,
        managementGate = CapabilityState.Available, bonds = CapabilityState.Available,
        wake = CapabilityState.Available, activeInput = CapabilityState.Available,
    ),
)

private val TOUCH_LAB_PERSONALITIES = setOf(
    Personality.Pro2,
    Personality.GameCube,
    Personality.JoyConLeft,
    Personality.JoyConRight,
)

private fun sampleKbm(): KbmState {
    fun key(code: Int, destination: KbmDestination, custom: Boolean = false) =
        KbmBinding(KbmSource(KbmSourceKind.Key, code), destination, custom)
    fun mouse(button: Int, destination: KbmDestination, custom: Boolean = false) =
        KbmBinding(KbmSource(KbmSourceKind.MouseButton, button), destination, custom)

    val keyboardMouse = KbmMapping(
        profile = KbmProfile.KeyboardMouse,
        bindings = listOf(
            key(0x1A, KbmDestination.LStickUp), key(0x16, KbmDestination.LStickDown),
            key(0x04, KbmDestination.LStickLeft), key(0x07, KbmDestination.LStickRight),
            key(0x2C, KbmDestination.B), key(0x09, KbmDestination.A),
            key(0x08, KbmDestination.X), key(0xE1, KbmDestination.Y),
            key(0x14, KbmDestination.L), key(0x15, KbmDestination.R),
            key(0x1E, KbmDestination.Zl), key(0x20, KbmDestination.Zr),
            key(0xE0, KbmDestination.L3), key(0x06, KbmDestination.C),
            key(0x28, KbmDestination.Plus), key(0x2A, KbmDestination.Minus),
            key(0x29, KbmDestination.Home, custom = true), key(0x45, KbmDestination.Capture),
            mouse(1, KbmDestination.Zr), mouse(2, KbmDestination.Zl),
            mouse(3, KbmDestination.R3), mouse(4, KbmDestination.Y, custom = true),
            mouse(5, KbmDestination.B),
        ),
        loaded = true,
    )
    val keyboard = KbmMapping(
        profile = KbmProfile.Keyboard,
        bindings = keyboardMouse.keyBindings + listOf(
            key(0x0C, KbmDestination.RStickUp), key(0x0E, KbmDestination.RStickDown),
            key(0x0D, KbmDestination.RStickLeft), key(0x0F, KbmDestination.RStickRight),
        ),
        loaded = true,
    )
    return KbmState(
        status = KbmStatus(
            mode = KbmMode.KeyboardMouse,
            modeOverride = KbmMode.Automatic,
            profile = KbmProfile.KeyboardMouse,
            keyboardConnected = true,
            mouseConnected = true,
            keyboardConn = 1,
            mouseConn = 2,
            keyboardReports = 4821,
            mouseReports = 91240,
            rejectedDuplicate = 3,
            mapGeneration = 7,
            publishes = 96061,
            recenters = 214,
        ),
        mouse = KbmMouseConfig(
            sensitivityX = 512, sensitivityY = 512, velocityWindowMs = 120,
            invertX = false, invertY = false, antiDeadzone = 6,
            sensitivityMin = 16, sensitivityMax = 8192,
            velocityWindowMinMs = 10, velocityWindowMaxMs = 2000, antiDeadzoneMax = 50,
        ),
        mappings = mapOf(KbmProfile.Keyboard to keyboard, KbmProfile.KeyboardMouse to keyboardMouse),
        available = CapabilityState.Available,
        dirty = true,
    )
}

/** Keyboard connected, mouse absent -- the ordinary partial state. */
private fun emptyKbm() = sampleKbm().copy(
    status = sampleKbm().status.copy(
        mouseConnected = false,
        mode = KbmMode.Keyboard,
        profile = KbmProfile.Keyboard,
    ),
    mappings = emptyMap(),
    dirty = false,
)

private val names = listOf(
    "Link" to "The Legend of Zelda",
    "Zelda" to "The Legend of Zelda",
    "Mario" to "Super Mario",
    "Luigi" to "Super Mario",
    "Bowser" to "Super Mario",
    "Kirby" to "Kirby",
    "Meta Knight" to "Kirby",
    "Samus Aran" to "Metroid",
    "Pikachu" to "Pokémon",
    "Isabelle" to "Animal Crossing",
    "Villager" to "Animal Crossing",
    "Inkling Girl" to "Splatoon",
    "Captain Falcon" to "F-Zero",
    "Fox McCloud" to "Star Fox",
)

/**
 * A synthetic library of [count] entries.
 *
 * SCALE IS A LAYOUT PROPERTY. Fourteen tiles cannot show whether the browser
 * stays smooth at the size real collections reach — a thousand-plus — and the
 * difference between "skips recomposition" and "recomposes every visible item on
 * every state change" is invisible until there are enough items for it to cost
 * anything. Pass `--ei library 1200` to reproduce that, and measure with
 * `dumpsys gfxinfo`.
 *
 * Beyond the named figures the entries repeat with distinct ids, which is all
 * the browser needs: it keys on id and renders a name.
 */
private fun sampleLibrary(count: Int = names.size): List<AmiiboLibraryItem> =
    (0 until count).map { index ->
        val (name, _) = names[index % names.size]
        val label = if (index < names.size) name else "$name ${index / names.size + 1}"
        AmiiboLibraryItem(
            id = "0100%04X000%04X".format(index, index),
            displayName = label,
            fileName = "$label.bin",
            size = if (index % 5 == 0) 572 else 540,
            crc32 = "%08X".format(0x1234_5678L + index),
            uid = "04A1B2C3D4%04X".format(index),
            figureId = "0100%04X000%04X".format(index % names.size, index % names.size),
            importedAtMillis = 1_760_000_000_000L - index * 86_400_000L,
            typeName = "Figure",
            characterGameCode = "%04X".format(index),
        )
    }

private fun sampleCatalog(library: List<AmiiboLibraryItem>): Map<String, AmiiboCatalogEntry> =
    library.mapIndexed { index, item ->
        val (name, series) = names[index % names.size]
        item.id to AmiiboCatalogEntry(
            id = item.id,
            character = name,
            gameSeries = series,
            amiiboSeries = "Super Smash Bros.",
            type = "Figure",
            releaseDate = "2015-01-01",
            // Deliberately empty: the lab has no network, and the placeholder
            // is the state the grid has to look right in anyway.
            imageUrl = "",
            name = name,
        )
    }.toMap()
