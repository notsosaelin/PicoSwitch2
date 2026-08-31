using System.Security.Cryptography;
using System.Text;

namespace PicoSwitch.Management;

public enum AmiiboTagType
{
    /// <summary>A 540- or 572-byte NTAG215 dump: the ordinary amiibo figure.</summary>
    Ntag215,

    /// <summary>A 2048-byte Switch 2 "figure v3" image.</summary>
    FigureV3,
}

public enum AmiiboCryptoState
{
    NotAttempted,

    /// <summary>No retail key set has been supplied, so nothing was decrypted.</summary>
    KeyUnavailable,

    /// <summary>Both HMACs verified.</summary>
    Valid,

    /// <summary>The image decrypted to something whose HMACs do not check out.</summary>
    Invalid,
}

/// <summary>Plaintext identity, readable with no keys at all.</summary>
public sealed record AmiiboIdentity
{
    public required string Uid { get; init; }

    public required string FigureId { get; init; }

    public required AmiiboTagType TagType { get; init; }

    public required string CharacterGameCode { get; init; }

    public required int CharacterVariant { get; init; }

    public required string TypeName { get; init; }

    public required string ModelNumber { get; init; }

    public required int SeriesCode { get; init; }

    public required int FormatVersion { get; init; }

    public required string ExtendedVariant { get; init; }
}

/// <summary>Identity plus the register metadata that only decrypts with keys.</summary>
public sealed record AmiiboDetails
{
    public required AmiiboIdentity Identity { get; init; }

    public required int Size { get; init; }

    public required string Crc32 { get; init; }

    public AmiiboCryptoState Crypto { get; init; } = AmiiboCryptoState.NotAttempted;

    public string Owner { get; init; } = "";

    public string Nickname { get; init; } = "";

    public bool SetUp { get; init; }

    public string? SetupDate { get; init; }

    public string? LastWriteDate { get; init; }

    public int WriteCounter { get; init; }

    public bool HasAppData { get; init; }

    public string TitleId { get; init; } = "";

    public string AppId { get; init; } = "";

    public string AppDataLabel { get; init; } = "";
}

/// <summary>
/// What a tag image must look like before anything else touches it.
/// </summary>
public static class AmiiboFiles
{
    /// <summary>
    /// 540 is an NTAG215 dump; 572 adds the reader's trailing password block;
    /// 2048 is a Switch 2 figure v3 image.
    /// </summary>
    public static readonly IReadOnlySet<int> SupportedSizes = new HashSet<int> { 540, 572, 2048 };

    /// <summary>
    /// Accept a file for import, repairing only what dump exporters routinely
    /// leave as placeholders.
    /// </summary>
    /// <remarks>
    /// The two UID check bytes are DERIVED from the UID, so a wrong one says
    /// nothing about the tag and rejecting on it would refuse otherwise perfect
    /// dumps from common tools. Nothing else is repaired: every other byte is
    /// either the user's data or covered by an HMAC.
    /// </remarks>
    public static byte[] NormalizeImport(byte[] raw)
    {
        if (!SupportedSizes.Contains(raw.Length))
        {
            throw new ArgumentException(
                "Amiibo backups must be exactly 540, 572, or 2048 bytes");
        }

        var data = (byte[])raw.Clone();
        if (data.Length != 2048)
        {
            data[3] = (byte)(0x88 ^ data[0] ^ data[1] ^ data[2]);
            data[8] = (byte)(data[4] ^ data[5] ^ data[6] ^ data[7]);
        }

        Validate(data);
        return data;
    }

    public static void Validate(byte[] data)
    {
        if (!SupportedSizes.Contains(data.Length))
        {
            throw new ArgumentException("Amiibo backups must be 540, 572, or 2048 bytes");
        }

        if (data.Length <= 0x5B)
        {
            throw new ArgumentException("Amiibo backup is missing its identity block");
        }

        if (data[0] != 0x04)
        {
            throw new ArgumentException(
                "Amiibo UID must start with Nintendo/NXP manufacturer byte 04");
        }

        if (data.Length == 2048)
        {
            if (data[7] != 0x00 || data[8] != 0x44)
            {
                throw new ArgumentException("Figure v3 image is missing its 00/44 internal marker");
            }
        }
        else
        {
            var bcc0 = (byte)(0x88 ^ data[0] ^ data[1] ^ data[2]);
            var bcc1 = (byte)(data[4] ^ data[5] ^ data[6] ^ data[7]);
            if (data[3] != bcc0 || data[8] != bcc1)
            {
                throw new ArgumentException("Amiibo UID checksum is invalid");
            }
        }
    }

    public static string Crc32(byte[] data) =>
        Management.Crc32.Compute(data).ToString("X8");
}

/// <summary>
/// amiitool-compatible amiibo tag crypto.
/// </summary>
/// <remarks>
/// THE THIRD IMPLEMENTATION, AND WHY THAT IS ACCEPTABLE HERE.
///
/// This algorithm already exists twice in the repository — in the Android app's
/// <c>AmiiboCrypto.kt</c> and in <c>web/index.html</c> — and a third copy of
/// security-adjacent code is normally exactly the duplication to refuse. It is
/// tolerable only because all three now verify against ONE artifact:
/// <c>tools/fixtures/amiibo/crypto-vectors.json</c>, generated from the Kotlin
/// implementation over real tags. That fixture was a stated precondition of this
/// work (WINDOWS_PASS.md §16.7) precisely because the failure mode here is
/// silent: a wrong HMAC produces a tag the console simply rejects, with no error
/// to read.
///
/// Written from the documented algorithm and the shared vectors, not transcribed
/// from the Kotlin. Where the two are structurally similar it is because the
/// algorithm is fixed, not because one was translated.
///
/// ## The layout, once
///
/// A tag is stored in a scattered order; the crypto operates on a contiguous
/// 0x208-byte "internal" form. <see cref="TagToInternal"/> and
/// <see cref="InternalToTag"/> are exact inverses, and both offset tables are
/// written out rather than derived, because a clever derivation here would be a
/// second thing to get wrong.
///
/// ## Nothing here logs
///
/// No key, no derived key, no plaintext register block and no Mii name is ever
/// written to a log, an exception message or a support bundle from this file.
/// Exceptions carry only what a user can act on.
/// </remarks>
public static class AmiiboCrypto
{
    private const int MasterBytes = 80;
    private const int RetailKeyBytes = 160;
    private const int InternalBytes = 0x208;

    public sealed record MasterKey(
        byte[] HmacKey,
        byte[] TypeString,
        int MagicSize,
        byte[] MagicBytes,
        byte[] XorPad);

    public sealed record RetailKeys(MasterKey Data, MasterKey Tag);

    private readonly record struct Decrypted(byte[] Internal, bool Valid);

    private readonly record struct DerivedKeys(byte[] AesKey, byte[] AesIv, byte[] HmacKey);

    /// <summary>
    /// Validate the portal-compatible 160-byte <c>key_retail.bin</c> format.
    /// </summary>
    /// <remarks>
    /// The two masters are accepted in EITHER order and sorted here. Both
    /// orderings are in circulation, and a file that is merely the other way
    /// round would otherwise decrypt to garbage and be reported as a bad key —
    /// sending the user to look for a different file they do not need.
    /// </remarks>
    public static RetailKeys ParseRetailKeys(byte[] raw)
    {
        if (raw.Length != RetailKeyBytes)
        {
            throw new ArgumentException($"key_retail.bin must be 160 bytes, got {raw.Length}");
        }

        var first = ParseMaster(raw[..MasterBytes]);
        var second = ParseMaster(raw[MasterBytes..RetailKeyBytes]);

        var (data, tag) =
            Label(first).StartsWith("locked", StringComparison.Ordinal) &&
            Label(second).StartsWith("unfixed", StringComparison.Ordinal)
                ? (second, first)
                : (first, second);

        if (!Label(data).StartsWith("unfixed", StringComparison.Ordinal) ||
            !Label(tag).StartsWith("locked", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Unrecognized key file: expected unfixed-info and locked-secret masters");
        }

        return new RetailKeys(data, tag);
    }

    /// <summary>
    /// The plaintext identity block. Needs no keys.
    /// </summary>
    /// <remarks>
    /// Deliberately separate from <see cref="ReadDetails"/>: a user with no key
    /// set can still see which figure a backup is, which is most of what a
    /// library list shows.
    /// </remarks>
    public static AmiiboIdentity Identity(byte[] bytes)
    {
        AmiiboFiles.Validate(bytes);
        var v3 = bytes.Length == 2048;

        // The v3 UID is contiguous; the NTAG215 UID skips its check byte at 3.
        var uidOffsets = v3
            ? new[] { 0, 1, 2, 3, 4, 5, 6 }
            : new[] { 0, 1, 2, 4, 5, 6, 7 };

        var typeCode = bytes[0x57];
        return new AmiiboIdentity
        {
            Uid = string.Concat(uidOffsets.Select(i => bytes[i].ToString("X2"))),
            FigureId = Hex(bytes, 0x54, 0x5C),
            TagType = v3 ? AmiiboTagType.FigureV3 : AmiiboTagType.Ntag215,
            CharacterGameCode = Hex(bytes, 0x54, 0x56),
            CharacterVariant = bytes[0x56],
            TypeName = typeCode switch
            {
                0x00 => "Figure",
                0x01 => "Card",
                0x02 => "Yarn",
                0x03 => "Power-Up Band",
                0x04 => "Wood Block",
                _ => $"Type 0x{typeCode:X2}",
            },
            ModelNumber = Hex(bytes, 0x58, 0x5A),
            SeriesCode = bytes[0x5A],
            FormatVersion = bytes[0x5B],
            ExtendedVariant = Hex(bytes, 0x5C, 0x60),
        };
    }

    /// <summary>
    /// Identity, plus the register metadata when a key set is available.
    /// </summary>
    /// <remarks>
    /// Degrades in three distinct ways rather than one, because they mean
    /// different things to a user: no keys supplied
    /// (<see cref="AmiiboCryptoState.KeyUnavailable"/> — supply them), keys that
    /// did not verify (<see cref="AmiiboCryptoState.Invalid"/> — wrong keys, or
    /// a damaged dump), and success. Collapsing the first two would send someone
    /// hunting for a corrupt file when they simply have not imported a key.
    /// </remarks>
    public static AmiiboDetails ReadDetails(byte[] bytes, RetailKeys? keys = null)
    {
        var identity = Identity(bytes);
        var basis = new AmiiboDetails
        {
            Identity = identity,
            Size = bytes.Length,
            Crc32 = AmiiboFiles.Crc32(bytes),
            Crypto = keys is null
                ? AmiiboCryptoState.KeyUnavailable
                : AmiiboCryptoState.NotAttempted,
        };

        if (keys is null)
        {
            return basis;
        }

        Decrypted decoded;
        try
        {
            decoded = DecryptInternal(keys, bytes, identity.TagType == AmiiboTagType.FigureV3);
        }
        catch (Exception)
        {
            // A dump that cannot even be unpacked is reported the same as one
            // whose HMACs fail: from the user's side both mean "this file and
            // these keys do not go together".
            return basis with { Crypto = AmiiboCryptoState.Invalid };
        }

        if (!decoded.Valid)
        {
            return basis with { Crypto = AmiiboCryptoState.Invalid };
        }

        var register = ReadRegisterInfo(decoded.Internal);
        return basis with
        {
            Crypto = AmiiboCryptoState.Valid,
            Owner = register.Owner,
            Nickname = register.Nickname,
            SetUp = register.SetUp,
            SetupDate = register.SetupDate,
            LastWriteDate = register.LastWriteDate,
            WriteCounter = register.WriteCounter,
            HasAppData = register.HasAppData,
            TitleId = register.TitleId,
            AppId = register.AppId,
            AppDataLabel = register.AppDataLabel,
        };
    }

    /// <summary>
    /// Wipe the user-owned settings and game regions, and re-sign the image.
    /// </summary>
    /// <remarks>
    /// Deterministic: fixed ranges are zeroed and the result re-signed, with no
    /// timestamp and no randomness, so initializing the same image twice gives
    /// the same bytes. The shared fixture pins that by digest.
    ///
    /// It refuses in two places rather than trusting itself. An image whose HMAC
    /// does not verify is never re-signed — that would mint a valid signature
    /// over content of unknown provenance — and the result is decrypted again and
    /// checked to actually be empty before it is returned. A wipe that silently
    /// left the previous owner's name on the tag is the failure worth spending
    /// two extra checks on.
    /// </remarks>
    public static byte[] Initialize(byte[] bytes, RetailKeys keys)
    {
        var identity = Identity(bytes);
        var v3 = identity.TagType == AmiiboTagType.FigureV3;

        var decoded = DecryptInternal(keys, bytes, v3);
        if (!decoded.Valid)
        {
            throw new InvalidOperationException(
                "This dump failed its HMAC check; refusing to re-sign it");
        }

        var plain = (byte[])decoded.Internal.Clone();
        if (v3)
        {
            Array.Clear(plain, 0x02A, 0x02C - 0x02A);
        }

        Array.Clear(plain, 0x02C, 0x1B4 - 0x02C);
        var output = PackInternal(keys, plain, bytes, v3);

        if (v3)
        {
            // Air Riders keeps writable game state OUTSIDE the encrypted amiibo
            // body, so re-signing alone does not clear it. Only the ranges
            // captured as allocation-independent are cleared; chip configuration
            // and machine/SRAM identity survive deliberately.
            Array.Clear(output, 0x248, 0x388 - 0x248);
            Array.Clear(output, 0x400, 0x800 - 0x400);
        }

        var verify = DecryptInternal(keys, output, v3);
        if (!verify.Valid)
        {
            throw new InvalidOperationException(
                "Re-signed image failed verification; nothing was changed");
        }

        var info = ReadRegisterInfo(verify.Internal);
        if (info.SetUp || info.Owner.Length > 0 || info.Nickname.Length > 0 ||
            info.LastWriteDate is not null || info.WriteCounter != 0 || info.HasAppData)
        {
            throw new InvalidOperationException(
                "Initialized image still has user data; nothing was changed");
        }

        return output;
    }

    /// <summary>
    /// The amiibo date format: 7 bits of year-since-2000, 4 of month, 5 of day.
    /// </summary>
    /// <remarks>
    /// An unset field is 0 or 0xFFFF, and an out-of-range month or day means the
    /// field is not a date at all — an un-set-up tag has arbitrary bytes here.
    /// Returning null rather than a clamped date keeps "no date" distinguishable
    /// from "the first of January".
    /// </remarks>
    public static string? DecodeDate(int hi, int lo)
    {
        var value = (hi << 8) | lo;
        if (value == 0 || value == 0xFFFF)
        {
            return null;
        }

        var year = 2000 + (value >> 9);
        var month = (value >> 5) & 0x0F;
        var day = value & 0x1F;
        if (month is < 1 or > 12 || day is < 1 or > 31)
        {
            return null;
        }

        return $"{year:D4}-{month:D2}-{day:D2}";
    }

    // ---------------------------------------------------------------- crypto

    private static Decrypted DecryptInternal(RetailKeys keys, byte[] encryptedTag, bool v3)
    {
        var buffer = TagToInternal(encryptedTag, v3);
        var tagKeys = DeriveKeys(keys.Tag, buffer);
        var dataKeys = DeriveKeys(keys.Data, buffer);

        var plain = AesCtr(dataKeys.AesKey, dataKeys.AesIv, buffer[0x2C..0x1B4]);
        plain.CopyTo(buffer, 0x2C);

        var tagHmac = HmacSha256(tagKeys.HmacKey, buffer[0x1D4..InternalBytes]);
        var dataHmac = HmacSha256(dataKeys.HmacKey, buffer[0x029..InternalBytes]);

        // Fixed-time comparison: these are authentication tags, and a
        // content-dependent early exit is the one thing not to do with them.
        return new Decrypted(
            buffer,
            CryptographicOperations.FixedTimeEquals(tagHmac, buffer[0x1B4..0x1D4]) &&
            CryptographicOperations.FixedTimeEquals(dataHmac, buffer[0x008..0x028]));
    }

    private static byte[] PackInternal(RetailKeys keys, byte[] plainInternal,
                                       byte[] originalTag, bool v3)
    {
        var plain = (byte[])plainInternal.Clone();
        var tagKeys = DeriveKeys(keys.Tag, plain);
        var dataKeys = DeriveKeys(keys.Data, plain);

        // ORDER IS LOAD-BEARING: the data HMAC covers the region the tag HMAC
        // was just written into, so the tag HMAC must be written first. Swapping
        // these produces an image that verifies here and is rejected by the
        // console — the exact silent failure this file's fixture exists to catch.
        HmacSha256(tagKeys.HmacKey, plain[0x1D4..InternalBytes]).CopyTo(plain, 0x1B4);
        HmacSha256(dataKeys.HmacKey, plain[0x029..InternalBytes]).CopyTo(plain, 0x008);

        AesCtr(dataKeys.AesKey, dataKeys.AesIv, plain[0x02C..0x1B4], encrypt: true)
            .CopyTo(plain, 0x02C);

        return InternalToTag(plain, originalTag, v3);
    }

    /// <summary>
    /// Derive the AES key, IV and HMAC key for one master from the tag itself.
    /// </summary>
    /// <remarks>
    /// The seed is assembled from four disjoint tag regions, and the duplicated
    /// 0x1D4..0x1DC copy at both 0x10 and 0x18 is not a mistake — the original
    /// implementation writes that field twice and the derivation depends on it.
    /// </remarks>
    private static DerivedKeys DeriveKeys(MasterKey master, byte[] internalBuffer)
    {
        var seed = new byte[64];
        internalBuffer[0x029..0x02B].CopyTo(seed, 0);
        internalBuffer[0x1D4..0x1DC].CopyTo(seed, 0x10);
        internalBuffer[0x1D4..0x1DC].CopyTo(seed, 0x18);
        internalBuffer[0x1E8..0x208].CopyTo(seed, 0x20);

        var derived = Drbg(master.HmacKey, PrepareSeed(master, seed), 48);
        return new DerivedKeys(derived[..16], derived[16..32], derived[32..48]);
    }

    /// <summary>HMAC-SHA256 in counter mode, as amiitool defines it.</summary>
    private static byte[] Drbg(byte[] hmacKey, byte[] seed, int outputLength)
    {
        var result = new byte[outputLength];
        var input = new byte[2 + seed.Length];
        seed.CopyTo(input, 2);

        var written = 0;
        for (var iteration = 0; written < outputLength; iteration++)
        {
            input[0] = (byte)(iteration >> 8);
            input[1] = (byte)iteration;

            var block = HmacSha256(hmacKey, input);
            var take = Math.Min(block.Length, outputLength - written);
            Array.Copy(block, 0, result, written, take);
            written += take;
        }

        return result;
    }

    private static byte[] PrepareSeed(MasterKey master, byte[] baseSeed)
    {
        var nul = Array.IndexOf(master.TypeString, (byte)0);
        if (nul < 0)
        {
            throw new ArgumentException("master key typeString not NUL-terminated");
        }

        var leading = 16 - master.MagicSize;
        if (leading < 0)
        {
            throw new ArgumentException("master key magic size is invalid");
        }

        var output = new byte[nul + 1 + leading + master.MagicSize + 16 + 32];
        var offset = 0;

        Array.Copy(master.TypeString, 0, output, offset, nul + 1);
        offset += nul + 1;

        Array.Copy(baseSeed, 0, output, offset, leading);
        offset += leading;

        Array.Copy(master.MagicBytes, 0, output, offset, master.MagicSize);
        offset += master.MagicSize;

        Array.Copy(baseSeed, 0x10, output, offset, 0x10);
        offset += 0x10;

        for (var index = 0; index < 0x20; index++)
        {
            output[offset + index] = (byte)(baseSeed[0x20 + index] ^ master.XorPad[index]);
        }

        return output;
    }

    // ---------------------------------------------------------------- layout

    /// <summary>
    /// Gather the scattered tag layout into the contiguous form the crypto uses.
    /// </summary>
    /// <remarks>
    /// The v3 image puts the encrypted blocks 0x40 bytes later than an NTAG215
    /// one; everything else sits at the same place in both. Exact inverse of
    /// <see cref="InternalToTag"/>.
    /// </remarks>
    private static byte[] TagToInternal(byte[] tag, bool v3)
    {
        var expected = v3 ? tag.Length == 2048 : tag.Length is 540 or 572;
        if (!expected)
        {
            throw new ArgumentException("Unexpected Amiibo image size for crypto");
        }

        var buffer = new byte[InternalBytes];
        var encryptedBase = v3 ? 0x0C0 : 0x080;
        var dataBase = v3 ? 0x0E0 : 0x0A0;

        tag[0x008..0x010].CopyTo(buffer, 0x000);
        tag[encryptedBase..(encryptedBase + 0x20)].CopyTo(buffer, 0x008);
        tag[0x010..0x034].CopyTo(buffer, 0x028);
        tag[dataBase..(dataBase + 0x168)].CopyTo(buffer, 0x04C);
        tag[0x034..0x054].CopyTo(buffer, 0x1B4);
        tag[0x000..0x008].CopyTo(buffer, 0x1D4);
        tag[0x054..0x080].CopyTo(buffer, 0x1DC);
        return buffer;
    }

    /// <summary>
    /// Scatter the internal form back over a COPY of the original tag.
    /// </summary>
    /// <remarks>
    /// Starting from the original rather than from zeroes is deliberate: every
    /// byte the internal form does not describe — the v3 image's out-of-body
    /// game state, the reader's trailing password block on a 572-byte dump — is
    /// preserved untouched instead of being silently blanked.
    /// </remarks>
    private static byte[] InternalToTag(byte[] internalBuffer, byte[] originalTag, bool v3)
    {
        var tag = (byte[])originalTag.Clone();
        var encryptedBase = v3 ? 0x0C0 : 0x080;

        internalBuffer[0x000..0x008].CopyTo(tag, 0x008);
        internalBuffer[0x008..0x028].CopyTo(tag, encryptedBase);
        internalBuffer[0x028..0x04C].CopyTo(tag, 0x010);
        internalBuffer[0x04C..0x1B4].CopyTo(tag, encryptedBase + 0x20);
        internalBuffer[0x1B4..0x1D4].CopyTo(tag, 0x034);
        internalBuffer[0x1D4..0x1DC].CopyTo(tag, 0x000);
        internalBuffer[0x1DC..0x208].CopyTo(tag, 0x054);
        return tag;
    }

    // -------------------------------------------------------------- register

    private readonly record struct RegisterInfo(
        string Owner,
        string Nickname,
        bool SetUp,
        string? SetupDate,
        string? LastWriteDate,
        int WriteCounter,
        bool HasAppData,
        string TitleId,
        string AppId,
        string AppDataLabel);

    private static RegisterInfo ReadRegisterInfo(byte[] internalBuffer)
    {
        var flags = internalBuffer[0x2C];
        var setUp = (flags & 0x10) != 0;
        var hasAppData = (flags & 0x20) != 0;

        // The two names use OPPOSITE byte orders, which is a property of the
        // format rather than a bug worth "fixing": the nickname is big-endian
        // UTF-16 and the owner's Mii name is little-endian.
        var nickname = setUp
            ? Plausible(DecodeUtf16(internalBuffer, 0x38, 0x4C, littleEndian: false))
            : "";
        var owner = setUp
            ? Plausible(DecodeUtf16(internalBuffer, 0x4C + 0x1A, 0x4C + 0x2E, littleEndian: true))
            : "";

        // Dates are only meaningful once something has written the tag. On a
        // blank tag these bytes are arbitrary and would decode to a plausible
        // looking date that never happened.
        var datesValid = setUp || hasAppData;

        var titleId = hasAppData && !AllZero(internalBuffer, 0xAC, 0xB4)
            ? Hex(internalBuffer, 0xAC, 0xB4)
            : "";
        var appId = hasAppData ? Hex(internalBuffer, 0xB6, 0xBA) : "";

        return new RegisterInfo(
            Owner: owner,
            Nickname: nickname,
            SetUp: setUp,
            SetupDate: datesValid
                ? DecodeDate(internalBuffer[0x30], internalBuffer[0x31])
                : null,
            LastWriteDate: datesValid
                ? DecodeDate(internalBuffer[0x32], internalBuffer[0x33])
                : null,
            WriteCounter: (internalBuffer[0xB4] << 8) | internalBuffer[0xB5],
            HasAppData: hasAppData,
            TitleId: titleId,
            AppId: appId,
            AppDataLabel: !hasAppData
                ? "None"
                : AppIds.TryGetValue(appId, out var known)
                    ? known
                    : titleId.Length > 0
                        ? $"Unrecognised game (title {titleId})"
                        : appId.Length > 0 && appId != "00000000"
                            ? $"Unrecognised game (AppID {appId})"
                            : "Present");
    }

    // --------------------------------------------------------------- helpers

    private static byte[] AesCtr(byte[] key, byte[] iv, byte[] data, bool encrypt = false)
    {
        // CTR is symmetric, so one path serves both directions; the flag exists
        // only to make call sites read correctly.
        _ = encrypt;

        using var aes = Aes.Create();
        aes.Mode = CipherMode.ECB;
        aes.Padding = PaddingMode.None;
        aes.Key = key;

        using var encryptor = aes.CreateEncryptor();
        var output = new byte[data.Length];
        var counter = (byte[])iv.Clone();
        var keyStream = new byte[16];

        for (var offset = 0; offset < data.Length; offset += 16)
        {
            encryptor.TransformBlock(counter, 0, 16, keyStream, 0);
            var take = Math.Min(16, data.Length - offset);
            for (var i = 0; i < take; i++)
            {
                output[offset + i] = (byte)(data[offset + i] ^ keyStream[i]);
            }

            IncrementCounter(counter);
        }

        return output;
    }

    /// <summary>Big-endian increment across the whole 16-byte block.</summary>
    private static void IncrementCounter(byte[] counter)
    {
        for (var i = counter.Length - 1; i >= 0; i--)
        {
            if (++counter[i] != 0)
            {
                return;
            }
        }
    }

    private static byte[] HmacSha256(byte[] key, byte[] data) =>
        HMACSHA256.HashData(key, data);

    private static MasterKey ParseMaster(byte[] raw)
    {
        if (raw.Length != MasterBytes)
        {
            throw new ArgumentException("master key must be 80 bytes");
        }

        int magicSize = raw[31];
        if (magicSize > 16)
        {
            throw new ArgumentException("master key magicBytesSize > 16");
        }

        if (Array.IndexOf(raw, (byte)0, 16, 14) < 0)
        {
            throw new ArgumentException("master key typeString not NUL-terminated");
        }

        return new MasterKey(
            HmacKey: raw[0..16],
            TypeString: raw[16..30],
            MagicSize: magicSize,
            MagicBytes: raw[32..48],
            XorPad: raw[48..80]);
    }

    private static string Label(MasterKey master)
    {
        var nul = Array.IndexOf(master.TypeString, (byte)0);
        var length = nul < 0 ? master.TypeString.Length : nul;
        return Encoding.ASCII.GetString(master.TypeString, 0, length).ToLowerInvariant();
    }

    private static string DecodeUtf16(byte[] bytes, int from, int to, bool littleEndian)
    {
        var builder = new StringBuilder();
        for (var offset = from; offset + 1 < to; offset += 2)
        {
            var code = littleEndian
                ? bytes[offset] | (bytes[offset + 1] << 8)
                : (bytes[offset] << 8) | bytes[offset + 1];
            if (code == 0)
            {
                break;
            }

            builder.Append((char)code);
        }

        return builder.ToString().Trim();
    }

    /// <summary>
    /// Keep a decoded name only if it reads as text.
    /// </summary>
    /// <remarks>
    /// The register bytes are arbitrary on a tag that was never set up, and
    /// showing the mojibake they decode to as somebody's name is worse than
    /// showing nothing.
    /// </remarks>
    private static string Plausible(string value)
    {
        if (value.Length == 0)
        {
            return "";
        }

        foreach (var c in value)
        {
            if (c == '�' || c < 0x20 || (c >= 0x7F && c <= 0x9F) ||
                c == '﻿' || char.IsSurrogate(c))
            {
                return "";
            }
        }

        return value;
    }

    private static bool AllZero(byte[] bytes, int from, int to)
    {
        for (var i = from; i < to; i++)
        {
            if (bytes[i] != 0)
            {
                return false;
            }
        }

        return true;
    }

    private static string Hex(byte[] bytes, int from, int to)
    {
        var builder = new StringBuilder((to - from) * 2);
        for (var i = from; i < to; i++)
        {
            builder.Append(bytes[i].ToString("X2"));
        }

        return builder.ToString();
    }

    private static readonly Dictionary<string, string> AppIds = new(StringComparer.Ordinal)
    {
        ["10110E00"] = "Super Smash Bros.",
        ["0014F000"] = "Animal Crossing: Happy Home Designer",
        ["00152600"] = "Chibi-Robo!: Zip Lash",
        ["00132600"] = "Mario & Luigi: Paper Jam",
        ["1019C800"] = "The Legend of Zelda: Twilight Princess HD",
    };
}
