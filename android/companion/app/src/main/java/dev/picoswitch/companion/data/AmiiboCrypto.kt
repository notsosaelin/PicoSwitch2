package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCryptoState
import dev.picoswitch.companion.model.AmiiboDetails
import dev.picoswitch.companion.model.AmiiboIdentity
import dev.picoswitch.companion.model.AmiiboTagType
import java.io.File
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Amiibo crypto, register metadata reader, and local initialization primitive.
 *
 * The offsets and key schedule are a direct Kotlin port of the tested
 * amiitool-compatible block in web/index.html. This class intentionally has no
 * firmware or network dependency: retail keys are accepted only from the
 * caller and never included in a model, diagnostic, or management command.
 */
object AmiiboCrypto {
    private const val MASTER_BYTES = 80
    private const val RETAIL_KEY_BYTES = 160
    private const val INTERNAL_BYTES = 0x208

    data class MasterKey(
        val hmacKey: ByteArray,
        val typeString: ByteArray,
        val magicSize: Int,
        val magicBytes: ByteArray,
        val xorPad: ByteArray,
    )

    data class RetailKeys(val data: MasterKey, val tag: MasterKey)

    private data class Decrypted(val internal: ByteArray, val valid: Boolean)

    /** Validate the portal-compatible 160-byte key_retail.bin format. */
    fun parseRetailKeys(raw: ByteArray): RetailKeys {
        require(raw.size == RETAIL_KEY_BYTES) {
            "key_retail.bin must be 160 bytes, got ${raw.size}"
        }
        var data = parseMaster(raw.copyOfRange(0, MASTER_BYTES))
        var tag = parseMaster(raw.copyOfRange(MASTER_BYTES, RETAIL_KEY_BYTES))
        val dataLabel = label(data)
        val tagLabel = label(tag)
        if (dataLabel.startsWith("locked") && tagLabel.startsWith("unfixed")) {
            val swap = data
            data = tag
            tag = swap
        }
        require(dataLabelOf(data).lowercase().startsWith("unfixed") &&
            dataLabelOf(tag).lowercase().startsWith("locked")) {
            "Unrecognized key file: expected unfixed-info and locked-secret masters"
        }
        return RetailKeys(data, tag)
    }

    fun identity(bytes: ByteArray): AmiiboIdentity {
        AmiiboFiles.validate(bytes)
        val v3 = bytes.size == 2048
        val uidOffsets = if (v3) intArrayOf(0, 1, 2, 3, 4, 5, 6)
        else intArrayOf(0, 1, 2, 4, 5, 6, 7)
        val uid = uidOffsets.joinToString("") { "%02X".format(bytes[it].u8()) }
        val id = hex(bytes, 0x54, 0x5C)
        val typeCode = bytes[0x57].u8()
        val typeName = when (typeCode) {
            0x00 -> "Figure"
            0x01 -> "Card"
            0x02 -> "Yarn"
            0x03 -> "Power-Up Band"
            0x04 -> "Wood Block"
            else -> "Type 0x%02X".format(typeCode)
        }
        return AmiiboIdentity(
            uid = uid,
            figureId = id,
            tagType = if (v3) AmiiboTagType.FigureV3 else AmiiboTagType.Ntag215,
            characterGameCode = hex(bytes, 0x54, 0x56),
            characterVariant = bytes[0x56].u8(),
            typeName = typeName,
            modelNumber = hex(bytes, 0x58, 0x5A),
            seriesCode = bytes[0x5A].u8(),
            formatVersion = bytes[0x5B].u8(),
            extendedVariant = hex(bytes, 0x5C, 0x60),
        )
    }

    /** Decode identity even without keys; encrypted register fields stay unavailable. */
    fun readDetails(bytes: ByteArray, keys: RetailKeys? = null): AmiiboDetails {
        val id = identity(bytes)
        val base = AmiiboDetails(
            uid = id.uid,
            figureId = id.figureId,
            tagType = id.tagType,
            size = bytes.size,
            crc32 = AmiiboFiles.crc32(bytes),
            characterGameCode = id.characterGameCode,
            characterVariant = id.characterVariant,
            typeName = id.typeName,
            modelNumber = id.modelNumber,
            seriesCode = id.seriesCode,
            formatVersion = id.formatVersion,
            extendedVariant = id.extendedVariant,
            crypto = if (keys == null) AmiiboCryptoState.KeyUnavailable else AmiiboCryptoState.NotAttempted,
        )
        if (keys == null) return base
        val decoded = runCatching { decryptInternal(keys, bytes, id.tagType == AmiiboTagType.FigureV3) }.getOrNull()
            ?: return base.copy(crypto = AmiiboCryptoState.Invalid)
        if (!decoded.valid) return base.copy(crypto = AmiiboCryptoState.Invalid)
        val register = readRegisterInfo(decoded.internal)
        return base.copy(
            crypto = AmiiboCryptoState.Valid,
            owner = register.owner,
            nickname = register.nickname,
            setUp = register.setUp,
            setupDate = register.setupDate,
            lastWriteDate = register.lastWriteDate,
            writeCounter = register.writeCounter,
            hasAppData = register.hasAppData,
            titleId = register.titleId,
            appId = register.appId,
            appDataLabel = register.appDataLabel,
        )
    }

    /**
     * Wipe the user-owned settings/game regions and re-sign the same image.
     * This is intentionally local-only: the caller must already have a
     * validated user-supplied retail key and commits the returned bytes through
     * the transactional library store.
     */
    fun initialize(bytes: ByteArray, keys: RetailKeys): ByteArray {
        val identity = identity(bytes)
        val v3 = identity.tagType == AmiiboTagType.FigureV3
        val decoded = decryptInternal(keys, bytes, v3)
        require(decoded.valid) { "This dump failed its HMAC check; refusing to re-sign it" }
        val internal = decoded.internal.copyOf()
        if (v3) internal.fill(0, 0x02A, 0x02C)
        internal.fill(0, 0x02C, 0x1B4)
        var output = packInternal(keys, internal, bytes, v3)
        if (v3) {
            // Air Riders stores writable game state outside the encrypted
            // amiibo body. Clear only the captured allocation-independent
            // ranges; chip configuration and machine/SRAM identity survive.
            output = output.copyOf().also {
                it.fill(0, 0x248, 0x388)
                it.fill(0, 0x400, 0x800)
            }
        }
        val verify = decryptInternal(keys, output, v3)
        require(verify.valid) { "Re-signed image failed verification; nothing was changed" }
        val info = readRegisterInfo(verify.internal)
        require(!info.setUp && info.owner.isEmpty() && info.nickname.isEmpty() &&
            info.lastWriteDate == null && info.writeCounter == 0 && !info.hasAppData) {
            "Initialized image still has user data; nothing was changed"
        }
        return output
    }

    private data class RegisterInfo(
        val owner: String,
        val nickname: String,
        val setUp: Boolean,
        val setupDate: String?,
        val lastWriteDate: String?,
        val writeCounter: Int,
        val hasAppData: Boolean,
        val titleId: String,
        val appId: String,
        val appDataLabel: String,
    )

    private fun readRegisterInfo(internal: ByteArray): RegisterInfo {
        val flags = internal[0x2C].u8()
        val setUp = (flags and 0x10) != 0
        val hasAppData = (flags and 0x20) != 0
        val nickname = decodeUtf16(internal, 0x38, 0x4C, littleEndian = false)
            .takeIf(::plausibleName).orEmpty().takeIf { setUp }.orEmpty()
        val owner = decodeUtf16(internal, 0x4C + 0x1A, 0x4C + 0x2E, littleEndian = true)
            .takeIf(::plausibleName).orEmpty().takeIf { setUp }.orEmpty()
        val datesValid = setUp || hasAppData
        val titleId = if (hasAppData) hex(internal, 0xAC, 0xB4).takeUnless { allZero(internal, 0xAC, 0xB4) }.orEmpty() else ""
        val appId = if (hasAppData) hex(internal, 0xB6, 0xBA) else ""
        val appDataLabel = when {
            !hasAppData -> "None"
            APP_IDS[appId] != null -> APP_IDS.getValue(appId)
            titleId.isNotBlank() -> "Unrecognised game (title $titleId)"
            appId.isNotBlank() && appId != "00000000" -> "Unrecognised game (AppID $appId)"
            else -> "Present"
        }
        return RegisterInfo(
            owner = owner,
            nickname = nickname,
            setUp = setUp,
            setupDate = if (datesValid) decodeDate(internal[0x30].u8(), internal[0x31].u8()) else null,
            lastWriteDate = if (datesValid) decodeDate(internal[0x32].u8(), internal[0x33].u8()) else null,
            writeCounter = (internal[0xB4].u8() shl 8) or internal[0xB5].u8(),
            hasAppData = hasAppData,
            titleId = titleId,
            appId = appId,
            appDataLabel = appDataLabel,
        )
    }

    fun decodeDate(hi: Int, lo: Int): String? {
        val value = (hi shl 8) or lo
        if (value == 0 || value == 0xFFFF) return null
        val year = 2000 + (value shr 9)
        val month = (value shr 5) and 0x0F
        val day = value and 0x1F
        if (month !in 1..12 || day !in 1..31) return null
        return "%04d-%02d-%02d".format(year, month, day)
    }

    private fun decryptInternal(keys: RetailKeys, encryptedTag: ByteArray, v3: Boolean): Decrypted {
        val internal = tagToInternal(encryptedTag, v3)
        val tagKeys = deriveKeys(keys.tag, internal)
        val dataKeys = deriveKeys(keys.data, internal)
        val plain = aesCtr(dataKeys.aesKey, dataKeys.aesIv, internal.copyOfRange(0x2C, 0x1B4))
        plain.copyInto(internal, 0x2C)
        val tagHmac = hmacSha256(tagKeys.hmacKey, internal.copyOfRange(0x1D4, INTERNAL_BYTES))
        val dataHmac = hmacSha256(dataKeys.hmacKey, internal.copyOfRange(0x029, INTERNAL_BYTES))
        return Decrypted(
            internal,
            MessageDigest.isEqual(tagHmac, internal.copyOfRange(0x1B4, 0x1D4)) &&
                MessageDigest.isEqual(dataHmac, internal.copyOfRange(0x008, 0x028)),
        )
    }

    private fun packInternal(keys: RetailKeys, internal: ByteArray, originalTag: ByteArray, v3: Boolean): ByteArray {
        val plain = internal.copyOf()
        val tagKeys = deriveKeys(keys.tag, plain)
        val dataKeys = deriveKeys(keys.data, plain)
        // The data HMAC covers the tag HMAC, so retain this ordering.
        hmacSha256(tagKeys.hmacKey, plain.copyOfRange(0x1D4, INTERNAL_BYTES))
            .copyInto(plain, 0x1B4)
        hmacSha256(dataKeys.hmacKey, plain.copyOfRange(0x029, INTERNAL_BYTES))
            .copyInto(plain, 0x008)
        aesCtr(dataKeys.aesKey, dataKeys.aesIv, plain.copyOfRange(0x02C, 0x1B4), Cipher.ENCRYPT_MODE)
            .copyInto(plain, 0x02C)
        return internalToTag(plain, originalTag, v3)
    }

    private data class DerivedKeys(val aesKey: ByteArray, val aesIv: ByteArray, val hmacKey: ByteArray)

    private fun deriveKeys(master: MasterKey, internal: ByteArray): DerivedKeys {
        val seed = ByteArray(64)
        internal.copyOfRange(0x029, 0x02B).copyInto(seed, 0)
        internal.copyOfRange(0x1D4, 0x1DC).copyInto(seed, 0x10)
        internal.copyOfRange(0x1D4, 0x1DC).copyInto(seed, 0x18)
        internal.copyOfRange(0x1E8, 0x208).copyInto(seed, 0x20)
        val prepared = prepareSeed(master, seed)
        val derived = drbg(master.hmacKey, prepared, 48)
        return DerivedKeys(derived.copyOfRange(0, 16), derived.copyOfRange(16, 32), derived.copyOfRange(32, 48))
    }

    private fun drbg(hmacKey: ByteArray, seed: ByteArray, outputLength: Int): ByteArray {
        val result = ByteArray(outputLength)
        val input = ByteArray(2 + seed.size)
        seed.copyInto(input, 2)
        var written = 0
        var iteration = 0
        while (written < outputLength) {
            input[0] = (iteration shr 8).toByte()
            input[1] = iteration.toByte()
            val block = hmacSha256(hmacKey, input)
            val take = minOf(block.size, outputLength - written)
            block.copyInto(result, written, 0, take)
            written += take
            iteration++
        }
        return result
    }

    private fun prepareSeed(master: MasterKey, baseSeed: ByteArray): ByteArray {
        val nul = master.typeString.indexOf(0)
        require(nul >= 0) { "master key typeString not NUL-terminated" }
        val leading = 16 - master.magicSize
        require(leading >= 0) { "master key magic size is invalid" }
        val output = ByteArray(nul + 1 + leading + master.magicSize + 16 + 32)
        var offset = 0
        master.typeString.copyInto(output, offset, 0, nul + 1); offset += nul + 1
        baseSeed.copyInto(output, offset, 0, leading); offset += leading
        master.magicBytes.copyInto(output, offset, 0, master.magicSize); offset += master.magicSize
        baseSeed.copyInto(output, offset, 0x10, 0x20); offset += 0x10
        repeat(0x20) { index -> output[offset + index] = (baseSeed[0x20 + index].u8() xor master.xorPad[index].u8()).toByte() }
        return output
    }

    private fun tagToInternal(tag: ByteArray, v3: Boolean): ByteArray {
        require(tag.size == (if (v3) 2048 else 540) || (!v3 && tag.size == 572)) {
            "Unexpected Amiibo image size for crypto"
        }
        val internal = ByteArray(INTERNAL_BYTES)
        tag.copyOfRange(0x008, 0x010).copyInto(internal, 0x000)
        val encryptedBase = if (v3) 0x0C0 else 0x080
        tag.copyOfRange(encryptedBase, encryptedBase + 0x20).copyInto(internal, 0x008)
        tag.copyOfRange(0x010, 0x034).copyInto(internal, 0x028)
        val dataBase = if (v3) 0x0E0 else 0x0A0
        tag.copyOfRange(dataBase, dataBase + 0x168).copyInto(internal, 0x04C)
        tag.copyOfRange(0x034, 0x054).copyInto(internal, 0x1B4)
        tag.copyOfRange(0x000, 0x008).copyInto(internal, 0x1D4)
        tag.copyOfRange(0x054, 0x080).copyInto(internal, 0x1DC)
        return internal
    }

    private fun aesCtr(key: ByteArray, iv: ByteArray, data: ByteArray, mode: Int = Cipher.DECRYPT_MODE): ByteArray {
        val cipher = Cipher.getInstance("AES/CTR/NoPadding")
        cipher.init(mode, SecretKeySpec(key, "AES"), IvParameterSpec(iv))
        return cipher.doFinal(data)
    }

    private fun internalToTag(internal: ByteArray, originalTag: ByteArray, v3: Boolean): ByteArray {
        val tag = originalTag.copyOf()
        val encryptedBase = if (v3) 0x0C0 else 0x080
        internal.copyOfRange(0x000, 0x008).copyInto(tag, 0x008)
        internal.copyOfRange(0x008, 0x028).copyInto(tag, encryptedBase)
        internal.copyOfRange(0x028, 0x04C).copyInto(tag, 0x010)
        internal.copyOfRange(0x04C, 0x1B4).copyInto(tag, encryptedBase + 0x20)
        internal.copyOfRange(0x1B4, 0x1D4).copyInto(tag, 0x034)
        internal.copyOfRange(0x1D4, 0x1DC).copyInto(tag, 0x000)
        internal.copyOfRange(0x1DC, 0x208).copyInto(tag, 0x054)
        return tag
    }

    private fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    private fun parseMaster(raw: ByteArray): MasterKey {
        require(raw.size == MASTER_BYTES) { "master key must be 80 bytes" }
        val magicSize = raw[31].u8()
        require(magicSize <= 16) { "master key magicBytesSize > 16" }
        require(raw.copyOfRange(16, 30).indexOf(0) >= 0) { "master key typeString not NUL-terminated" }
        return MasterKey(
            hmacKey = raw.copyOfRange(0, 16),
            typeString = raw.copyOfRange(16, 30),
            magicSize = magicSize,
            magicBytes = raw.copyOfRange(32, 48),
            xorPad = raw.copyOfRange(48, 80),
        )
    }

    private fun label(master: MasterKey) = dataLabelOf(master).lowercase()

    private fun dataLabelOf(master: MasterKey) = master.typeString
        .takeWhile { it.toInt() != 0 }
        .toByteArray()
        .toString(StandardCharsets.US_ASCII)

    private fun decodeUtf16(bytes: ByteArray, from: Int, to: Int, littleEndian: Boolean): String {
        val builder = StringBuilder()
        var offset = from
        while (offset + 1 < to) {
            val code = if (littleEndian) bytes[offset].u8() or (bytes[offset + 1].u8() shl 8)
            else (bytes[offset].u8() shl 8) or bytes[offset + 1].u8()
            if (code == 0) break
            builder.append(code.toChar())
            offset += 2
        }
        return builder.toString().trim()
    }

    private fun plausibleName(value: String): Boolean {
        if (value.isEmpty()) return false
        return value.all { c ->
            c.code != 0xFFFD && c.code >= 0x20 && c.code !in 0x7F..0x9F &&
                c.code != 0xFEFF && !c.isSurrogate()
        }
    }

    private fun allZero(bytes: ByteArray, from: Int, to: Int) = bytes.copyOfRange(from, to).all { it == 0.toByte() }

    private fun hex(bytes: ByteArray, from: Int, to: Int) = bytes.copyOfRange(from, to)
        .joinToString("") { "%02X".format(it.u8()) }

    private fun Byte.u8() = toInt() and 0xFF

    private val APP_IDS = mapOf(
        "10110E00" to "Super Smash Bros.",
        "0014F000" to "Animal Crossing: Happy Home Designer",
        "00152600" to "Chibi-Robo!: Zip Lash",
        "00132600" to "Mario & Luigi: Paper Jam",
        "1019C800" to "The Legend of Zelda: Twilight Princess HD",
    )
}

/**
 * Private local key store. The app manifest disables Android backup, and this
 * file is never included in [AmiiboLibrary] exports or diagnostics.
 */
class AmiiboKeyStore(private val root: File) {
    private val file = File(root, ".amiibo-retail-key.bin")

    init { root.mkdirs() }

    fun read(): AmiiboCrypto.RetailKeys? {
        if (!file.isFile) return null
        return runCatching { AmiiboCrypto.parseRetailKeys(file.readBytes()) }.getOrNull()
    }

    fun import(raw: ByteArray): AmiiboCrypto.RetailKeys {
        val parsed = AmiiboCrypto.parseRetailKeys(raw)
        val temp = File(root, ".${file.name}.${UUID.randomUUID()}.tmp")
        try {
            temp.outputStream().use { stream -> stream.write(raw); stream.fd.sync() }
            runCatching {
                Files.move(temp.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            }.getOrElse {
                Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            temp.delete()
        }
        return parsed
    }

    fun clear() { file.delete() }
    fun exists() = file.isFile
}
