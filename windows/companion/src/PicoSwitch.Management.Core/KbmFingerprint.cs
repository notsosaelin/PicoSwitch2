namespace PicoSwitch.Management;

/// <summary>
/// The canonical content digest, computed exactly as the firmware computes it.
/// </summary>
/// <remarks>
/// WHAT THIS IS FOR. Windows and Android keep SEPARATE local profile libraries
/// with separate ids; the copy resident on the adapter is the only bridge
/// between them. So "is the adapter's copy of this profile still the one I
/// have?" cannot be answered by comparing identities — only by comparing
/// content. This digest is that comparison, and it is why a local library can
/// truthfully report "adapter copy out of date".
///
/// It must agree with <c>ns2_kbm_content_fingerprint()</c> in src/ns2_kbm.c byte
/// for byte. That is not a hope: tools/test_ns2_kbm_commands.c emits vectors
/// from the firmware's own function into
/// tools/fixtures/management/kbm-wire-corpus.json, and the conformance test
/// replays them through this code.
///
/// FNV-1a, 32 bits, over the CANONICAL form: overrides sorted by (kind, code)
/// with redundant ones already dropped, then the profile-owned mouse values in
/// little-endian order. Profile-owned tuning is part of the mapping's behaviour,
/// so a sensitivity change moves the digest exactly as a rebind does — otherwise
/// "out of date" would miss it.
/// </remarks>
public static class KbmFingerprint
{
    private const uint Offset = 2166136261u;
    private const uint Prime = 16777619u;

    private static uint Mix(uint hash, byte value) => (hash ^ value) * Prime;

    /// <summary>
    /// The FIRMWARE's numeric value for a destination.
    /// </summary>
    /// <remarks>
    /// The C# enum's ordering is presentational and does NOT match
    /// <c>NS2_DST_*</c> in include/ns2_remap.h — the wire carries names, so the
    /// ordinals never had to agree and quietly do not. Hashing the managed
    /// ordinal produced a digest the adapter would never reproduce, which the
    /// firmware-generated vectors caught immediately.
    ///
    /// Stated as an explicit table rather than reordering the enum: the enum's
    /// order is what the destination picker shows, and the firmware's order is a
    /// wire fact. Neither should be bent to the other.
    /// </remarks>
    public static byte FirmwareCode(KbmDestination destination) => destination switch
    {
        KbmDestination.None => 0,
        KbmDestination.B => 1,
        KbmDestination.A => 2,
        KbmDestination.Y => 3,
        KbmDestination.X => 4,
        KbmDestination.L => 5,
        KbmDestination.R => 6,
        KbmDestination.Zl => 7,
        KbmDestination.Zr => 8,
        KbmDestination.L3 => 9,
        KbmDestination.R3 => 10,
        KbmDestination.Minus => 11,
        KbmDestination.Plus => 12,
        KbmDestination.Home => 13,
        KbmDestination.Capture => 14,
        KbmDestination.DUp => 15,
        KbmDestination.DDown => 16,
        KbmDestination.DLeft => 17,
        KbmDestination.DRight => 18,
        KbmDestination.Gl => 19,
        KbmDestination.Gr => 20,
        KbmDestination.C => 21,
        KbmDestination.LStickUp => 22,
        KbmDestination.LStickDown => 23,
        KbmDestination.LStickLeft => 24,
        KbmDestination.LStickRight => 25,
        KbmDestination.RStickUp => 26,
        KbmDestination.RStickDown => 27,
        KbmDestination.RStickLeft => 28,
        KbmDestination.RStickRight => 29,
        _ => 0,
    };

    /// <summary>
    /// The firmware's numeric value for a source kind (<c>NS2_KBM_SRC_*</c>),
    /// which is 1-based and so does not match the managed enum either.
    /// </summary>
    public static byte FirmwareCode(KbmSourceKind kind) => kind switch
    {
        KbmSourceKind.Key => 1,
        KbmSourceKind.MouseButton => 2,
        _ => 0,
    };

    /// <param name="overrides">
    /// The canonical override set: sorted by (kind, code), with any entry that
    /// merely restates the layout's default already removed. Passing a raw
    /// binding list would produce a digest the adapter never agrees with.
    /// </param>
    public static long Compute(KbmLayout layout,
                               IReadOnlyList<KbmBinding> overrides,
                               KbmMouseConfig mouse)
    {
        var hash = Offset;
        hash = Mix(hash, (byte)layout);
        hash = Mix(hash, (byte)overrides.Count);
        foreach (var entry in overrides)
        {
            hash = Mix(hash, FirmwareCode(entry.Source.Kind));
            hash = Mix(hash, (byte)entry.Source.Code);
            hash = Mix(hash, FirmwareCode(entry.Destination));
        }

        // Little-endian, stated explicitly so the three languages cannot guess
        // differently.
        hash = Mix(hash, (byte)(mouse.SensitivityX & 0xFF));
        hash = Mix(hash, (byte)((mouse.SensitivityX >> 8) & 0xFF));
        hash = Mix(hash, (byte)(mouse.SensitivityY & 0xFF));
        hash = Mix(hash, (byte)((mouse.SensitivityY >> 8) & 0xFF));
        hash = Mix(hash, (byte)(mouse.VelocityWindowMs & 0xFF));
        hash = Mix(hash, (byte)((mouse.VelocityWindowMs >> 8) & 0xFF));
        hash = Mix(hash, (byte)(mouse.InvertX ? 1 : 0));
        hash = Mix(hash, (byte)(mouse.InvertY ? 1 : 0));
        hash = Mix(hash, (byte)mouse.AntiDeadzone);
        return hash;
    }

    /// <summary>
    /// Canonicalize a binding list into the override set the digest expects.
    /// </summary>
    /// <param name="bindings">
    /// What the editor holds: every input the adapter reported, each flagged
    /// with whether the user changed it.
    /// </param>
    /// <remarks>
    /// Only user overrides contribute. A binding equal to the layout's own
    /// default is not part of the profile's content — the firmware stores
    /// content SPARSELY against a canonical table, and two profiles that behave
    /// identically must fingerprint identically however they were built.
    ///
    /// An explicit "not mapped" IS an override and is kept: it says the key does
    /// nothing, which differs from the default the adapter would otherwise apply.
    /// </remarks>
    public static IReadOnlyList<KbmBinding> Canonical(
        IReadOnlyList<KbmBinding> bindings) =>
        [.. bindings.Where(binding => binding.Custom)
                    .OrderBy(binding => FirmwareCode(binding.Source.Kind))
                    .ThenBy(binding => binding.Source.Code)];
}
