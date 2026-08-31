package dev.picoswitch.management

/**
 * The canonical content digest, computed exactly as the firmware computes it.
 *
 * WHAT THIS IS FOR. Windows and Android keep SEPARATE local profile libraries
 * with separate ids; the copy resident on the adapter is the only bridge between
 * them. So "is the adapter's copy of this profile still the one I have?" cannot
 * be answered by comparing identities — only by comparing content. This digest
 * is that comparison, and it is why a local library can truthfully report
 * "adapter copy out of date".
 *
 * It must agree with `ns2_kbm_content_fingerprint()` in src/ns2_kbm.c byte for
 * byte, and with the C# implementation. That is not a hope:
 * tools/test_ns2_kbm_commands.c emits vectors from the firmware's own function
 * into tools/fixtures/management/kbm-wire-corpus.json, and KbmFingerprintTest
 * replays them through this code.
 *
 * FNV-1a, 32 bits, over the CANONICAL form: overrides sorted by (kind, code)
 * with redundant ones already dropped, then the profile-owned mouse values in
 * little-endian order. Profile-owned tuning is part of the mapping's behaviour,
 * so a sensitivity change moves the digest exactly as a rebind does — otherwise
 * "out of date" would miss it.
 */
object KbmFingerprint {
    private const val OFFSET = 2166136261u
    private const val PRIME = 16777619u

    private fun mix(hash: UInt, value: Int): UInt = (hash xor value.toUInt()) * PRIME

    /**
     * The FIRMWARE's numeric value for a destination.
     *
     * The Kotlin enum's ordering is presentational and does NOT match `NS2_DST_*`
     * in include/ns2_remap.h — the wire carries names, so the ordinals never had
     * to agree and quietly do not. Hashing the ordinal produces a digest the
     * adapter can never reproduce, which would make "adapter copy out of date"
     * permanently and wrongly true. (The C# side hit exactly that.)
     */
    fun firmwareCode(destination: KbmDestination): Int = when (destination) {
        KbmDestination.None -> 0
        KbmDestination.B -> 1
        KbmDestination.A -> 2
        KbmDestination.Y -> 3
        KbmDestination.X -> 4
        KbmDestination.L -> 5
        KbmDestination.R -> 6
        KbmDestination.Zl -> 7
        KbmDestination.Zr -> 8
        KbmDestination.L3 -> 9
        KbmDestination.R3 -> 10
        KbmDestination.Minus -> 11
        KbmDestination.Plus -> 12
        KbmDestination.Home -> 13
        KbmDestination.Capture -> 14
        KbmDestination.DUp -> 15
        KbmDestination.DDown -> 16
        KbmDestination.DLeft -> 17
        KbmDestination.DRight -> 18
        KbmDestination.Gl -> 19
        KbmDestination.Gr -> 20
        KbmDestination.C -> 21
        KbmDestination.LStickUp -> 22
        KbmDestination.LStickDown -> 23
        KbmDestination.LStickLeft -> 24
        KbmDestination.LStickRight -> 25
        KbmDestination.RStickUp -> 26
        KbmDestination.RStickDown -> 27
        KbmDestination.RStickLeft -> 28
        KbmDestination.RStickRight -> 29
    }

    /** `NS2_KBM_SRC_*`, which is 1-based and so does not match the enum either. */
    fun firmwareCode(kind: KbmSourceKind): Int = when (kind) {
        KbmSourceKind.Key -> 1
        KbmSourceKind.MouseButton -> 2
    }

    /**
     * @param overrides the CANONICAL override set: sorted by (kind, code), with
     * any entry that merely restates the layout's default already removed.
     * Passing a raw binding list produces a digest the adapter never agrees with.
     */
    fun compute(
        layout: KbmProfile,
        overrides: List<KbmBinding>,
        mouse: KbmMouseConfig,
    ): Long {
        var hash = OFFSET
        hash = mix(hash, layout.ordinal)
        hash = mix(hash, overrides.size)
        for (entry in overrides) {
            hash = mix(hash, firmwareCode(entry.source.kind))
            hash = mix(hash, entry.source.code)
            hash = mix(hash, firmwareCode(entry.destination))
        }

        // Little-endian, stated explicitly so the three languages cannot guess
        // differently.
        hash = mix(hash, mouse.sensitivityX and 0xFF)
        hash = mix(hash, (mouse.sensitivityX shr 8) and 0xFF)
        hash = mix(hash, mouse.sensitivityY and 0xFF)
        hash = mix(hash, (mouse.sensitivityY shr 8) and 0xFF)
        hash = mix(hash, mouse.velocityWindowMs and 0xFF)
        hash = mix(hash, (mouse.velocityWindowMs shr 8) and 0xFF)
        hash = mix(hash, if (mouse.invertX) 1 else 0)
        hash = mix(hash, if (mouse.invertY) 1 else 0)
        hash = mix(hash, mouse.antiDeadzone)
        return hash.toLong()
    }

    /**
     * Canonicalize a binding list into the override set the digest expects.
     *
     * Only user overrides contribute. A binding equal to the layout's own default
     * is not part of the profile's content — the firmware stores content SPARSELY
     * against a canonical table, and two profiles that behave identically must
     * fingerprint identically however they were built.
     *
     * An explicit "not mapped" IS an override and is kept: it says the key does
     * nothing, which differs from the default the adapter would otherwise apply.
     */
    fun canonical(bindings: List<KbmBinding>): List<KbmBinding> =
        bindings.filter { it.custom }
            .sortedWith(
                compareBy({ firmwareCode(it.source.kind) }, { it.source.code }),
            )
}
