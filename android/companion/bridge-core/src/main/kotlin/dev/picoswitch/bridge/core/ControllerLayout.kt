package dev.picoswitch.bridge.core

/**
 * Physical face-button LAYOUT, kept separate from logical button semantics.
 *
 * ## The contract this file has to satisfy
 *
 * The bridge wire contract is fixed and LOGICAL: HID usages 1..4 are Nintendo
 * A/B/X/Y. Descriptor-proven bridge input is routed straight to
 * `NS2_DST_A/B/X/Y` by `ns2_resolve_button_destination()`, bypassing the
 * positional B/A/Y/X map that directly paired physical controllers use. So
 * everything here has one job: get each ORIGIN's face input into that logical
 * contract.
 *
 * ## Why the two origins need OPPOSITE corrections
 *
 * A physical face KEY arrives as a platform key code, and what that code means
 * depends on the source device's plastic:
 *
 *  * A positional/Xbox-style source names the BOTTOM face button `A`, so its
 *    `A` is the console's B and its `X` is the console's Y. It needs the swap.
 *  * A Nintendo-labelled handheld names its keys after the PRINTED legend: the
 *    button printed `A` — the right-hand one — reports as `A`. It is already
 *    logical and must be left alone. This is not a guess: the first AYN Thor
 *    in-game pass forwarded key codes untranslated and came out inverted, which
 *    is only possible if the handheld reports by legend rather than by position
 *    (`docs/bluetooth/android-controller-bridge.md`).
 *
 * An on-screen face POSITION has no plastic at all. It has four slots and a
 * presentation preference that decides which letter is DRAWN in each, and the
 * letter drawn is the letter sent. Under a Nintendo presentation the south slot
 * is drawn `B` and must send B; under an Xbox presentation it is drawn `A` and
 * must send A.
 *
 * Those are opposite functions of the same [ControllerFaceLayout]. Collapsing
 * them into one mapper is what broke both origins in turn: correcting the
 * on-screen pad inverted every physical face key, because one shared mapper can
 * only be right for one origin at a time. Keep them apart.
 *
 * ```text
 * physical key code  -> mapPhysicalFaceKey   -\
 *                                              >-- LOGICAL button -> HID usage 1..4
 * on-screen position -> mapTouchFacePosition -/
 * ```
 */
enum class ControllerFaceLayout(
    val key: String,
    val title: String,
    val description: String,
) {
    Auto("auto", "Auto", "Use a known handheld profile, otherwise the platform's standard button positions"),
    Nintendo("nintendo", "Nintendo", "Match Nintendo-style A/B and X/Y printed labels"),
    Xbox("xbox", "Xbox", "Match the platform's positional (Xbox-style) face-button order"),
    ;

    companion object {
        fun fromKey(value: String?): ControllerFaceLayout = entries.firstOrNull { it.key == value } ?: Auto
    }
}

/**
 * Stable identity of a host input source.
 *
 * [descriptor] is whatever opaque, stable string the platform uses to recognize
 * the same physical device across reconnects (Android's `InputDevice.descriptor`,
 * a Linux `/dev/input/by-id` name, a Windows HID instance path). The bridge never
 * parses it; it only compares and persists it.
 */
data class ControllerSourceIdentity(
    val descriptor: String,
    val name: String,
    val vendorId: Int,
    val productId: Int,
)

/**
 * A face-diamond slot named by WHERE it is, not by what is printed on it.
 *
 * An on-screen controller has no plastic, so it has no printed legend to inherit
 * — it has four positions and a layout preference that decides what to draw in
 * them. Naming the positions is what stops a renderer from assuming "A is always
 * the bottom one", which is false across controller families.
 *
 * [positional] is that same position expressed in the enum the rest of the
 * bridge already uses, so a software press enters [ControllerLayoutResolver] by
 * a single documented route and cannot acquire a second mapping table of its own.
 */
enum class FaceButtonPosition(val positional: ControllerButton) {
    South(ControllerButton.A),
    East(ControllerButton.B),
    West(ControllerButton.X),
    North(ControllerButton.Y),
}

data class ResolvedControllerLayout(
    val layout: ControllerFaceLayout,
    val reason: String,
)

object ControllerLayoutResolver {
    fun resolve(
        requested: ControllerFaceLayout,
        source: ControllerSourceIdentity?,
    ): ResolvedControllerLayout {
        if (requested != ControllerFaceLayout.Auto) {
            return ResolvedControllerLayout(requested, "Selected manually")
        }
        if (source == null) {
            return ResolvedControllerLayout(ControllerFaceLayout.Xbox, "No input source selected; using positional order")
        }

        // No portable platform property exposes the printed legend. These are the
        // bounded, hardware-audited built-in controller identities; they select labels
        // only and never gate whether the device is usable. The manual override remains
        // authoritative.
        //
        // AYN's handhelds carry a button-layout toggle that changes the DEVICE
        // IDENTITY, and with it which key code each physical button sends. Both
        // modes were read off a live Odin 2 Mini on 2026-08-24:
        //
        //   0x2020/0x0111  "Odin Controller"          reports the PRINTED legend
        //   0x2020/0x0112  "Xbox Wireless Controller" reports POSITIONALLY
        //
        // So the two PIDs must NOT resolve alike, however similar the hardware is:
        // Xbox mode is an Xbox-style source that happens to live behind
        // Nintendo-printed plastic, and treating it as Nintendo inverts every face
        // button. That is a real field failure, not a hypothetical — it is what an
        // Odin left in Xbox mode did on 2026-08-24.
        val reportsPrintedLegend =
            source.vendorId == 0x2020 && source.productId == 0x0111 ||
                source.vendorId == 0x2022 && source.productId == 0x3001 ||
                source.name.contains("Odin Controller", ignoreCase = true) ||
                source.name.contains("Retroid Pocket Controller", ignoreCase = true)
        return if (reportsPrintedLegend) {
            ResolvedControllerLayout(ControllerFaceLayout.Nintendo, "Known Nintendo-labeled handheld controller")
        } else {
            ResolvedControllerLayout(ControllerFaceLayout.Xbox, "Platforms expose positions, not printed labels")
        }
    }

    /**
     * The letter a face POSITION should be drawn with under [resolved].
     *
     * Derived from [mapTouchFacePosition] rather than from a second table on
     * purpose: a drawn label that disagrees with the bit that gets sent is the
     * exact failure a renderer's own `when(layout)` block produces, and it is
     * invisible until someone presses the button on a console.
     */
    fun faceLabel(position: FaceButtonPosition, resolved: ControllerFaceLayout): String =
        mapTouchFacePosition(position.positional, resolved).name

    /**
     * ON-SCREEN face position -> logical bridge button.
     *
     * The on-screen pad draws the letter its presentation calls for and sends
     * exactly that letter: a Nintendo presentation (south drawn `B`) swaps the
     * positional slot into its printed letter, an Xbox presentation (south drawn
     * `A`) leaves it alone. [faceLabel] is derived from this, which is what keeps
     * the legend and the wire bit from drifting apart.
     *
     * Do NOT route physical key codes through here — see [mapPhysicalFaceKey].
     */
    fun mapTouchFacePosition(position: ControllerButton, resolved: ControllerFaceLayout): ControllerButton =
        if (resolved == ControllerFaceLayout.Nintendo) swapFaces(position) else position

    /**
     * PHYSICAL face key, as the source device reported it -> logical bridge button.
     *
     * A Nintendo-labelled handheld already reports its printed letters, so it
     * passes through untouched. Every other source reports positionally, where
     * the bottom button is `A` while the console's bottom button is B — so those
     * are swapped into logical order here, once, at the only boundary that knows
     * which kind of device is attached.
     *
     * Non-face buttons are returned unchanged; shoulders, Start/Select and the
     * stick clicks mean the same thing under either layout.
     *
     * Do NOT route on-screen positions through here — see [mapTouchFacePosition].
     */
    fun mapPhysicalFaceKey(reported: ControllerButton, resolved: ControllerFaceLayout): ControllerButton =
        if (resolved == ControllerFaceLayout.Nintendo) reported else swapFaces(reported)

    /** A↔B and X↔Y; everything else is identity. */
    private fun swapFaces(button: ControllerButton): ControllerButton = when (button) {
        ControllerButton.A -> ControllerButton.B
        ControllerButton.B -> ControllerButton.A
        ControllerButton.X -> ControllerButton.Y
        ControllerButton.Y -> ControllerButton.X
        else -> button
    }
}

/**
 * Per-source layout persistence, implemented by the platform (preferences file,
 * registry, dotfile). The bridge only needs a stable string keyed by descriptor.
 */
interface ControllerLayoutStore {
    fun load(descriptor: String?): ControllerFaceLayout
    fun save(descriptor: String, layout: ControllerFaceLayout)

    /** For hosts with no persistence, and for tests. */
    object None : ControllerLayoutStore {
        override fun load(descriptor: String?) = ControllerFaceLayout.Auto
        override fun save(descriptor: String, layout: ControllerFaceLayout) = Unit
    }
}
