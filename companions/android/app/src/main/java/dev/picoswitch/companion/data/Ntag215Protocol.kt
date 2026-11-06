package dev.picoswitch.companion.data

/**
 * Small, Android-independent NTAG215 reader protocol.
 *
 * The transceiver is deliberately only a command/reply boundary.  It has no
 * NDEF, authentication, write, sector-select, or adapter operations, which
 * keeps a phone scan limited to the ordinary raw NTAG215 backup path.
 */
fun interface Ntag215Transceiver {
    fun transceive(command: ByteArray): ByteArray
}

enum class Ntag215Rejection(val description: String) {
    TRANSPORT_ERROR("The tag did not answer a required NFC command"),
    GET_VERSION_RESPONSE_INVALID("The tag returned an invalid GET_VERSION response"),
    UNSUPPORTED_TAG("Only NTAG215 tags are supported (NTAG213/216 were rejected)"),
    FIGURE_V3_UNSUPPORTED("Figure-v3/NTAG I2C 2K tags are not supported by phone backup"),
    READ_RESPONSE_INVALID("The tag returned an invalid 16-byte page read"),
    FAST_READ_RESPONSE_INVALID("The tag returned an invalid 12-byte final read"),
    MANUFACTURER_MISMATCH("The tag is not an NXP/Nintendo tag (manufacturer byte is not 04)"),
    BCC_INVALID("The tag UID check bytes (BCC) are invalid"),
}

sealed class Ntag215ReadResult {
    data class Success(
        val bytes: ByteArray,
        val signatureIncluded: Boolean,
    ) : Ntag215ReadResult()

    data class Rejected(val reason: Ntag215Rejection) : Ntag215ReadResult()
}

/** Exact ordinary NTAG215 command sequence used by the phone NFC backup. */
object Ntag215Protocol {
    const val RAW_BYTES = 540
    const val SIGNATURE_BYTES = 32
    const val EXTENDED_BYTES = RAW_BYTES + SIGNATURE_BYTES

    /** ISO/IEC 14443-3A GET_VERSION command. */
    val GET_VERSION_COMMAND: ByteArray = byteArrayOf(0x60)

    /** NTAG215's exact GET_VERSION reply; other storage sizes are rejected. */
    val NTAG215_GET_VERSION: ByteArray = byteArrayOf(
        0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03,
    )

    /** NTAG Type 2 READ command; each reply carries four 4-byte pages. */
    const val READ_COMMAND = 0x30

    /** FAST_READ command used for the final three pages (0x84..0x86). */
    val FAST_READ_COMMAND: ByteArray = byteArrayOf(0x3A, 0x84.toByte(), 0x86.toByte())

    /** Optional 32-byte originality signature command. */
    val READ_SIG_COMMAND: ByteArray = byteArrayOf(0x3C, 0x00)

    /**
     * Read one ordinary NTAG215.  A signature failure is intentionally a
     * successful, explicit 540-byte result; it is not replaced with zeroes.
     */
    fun read(transceiver: Ntag215Transceiver, includeSignature: Boolean = true): Ntag215ReadResult {
        val version = try {
            transceiver.transceive(GET_VERSION_COMMAND.copyOf())
        } catch (_: Exception) {
            return Ntag215ReadResult.Rejected(Ntag215Rejection.TRANSPORT_ERROR)
        }
        val versionRejection = validateVersion(version)
        if (versionRejection != null) return Ntag215ReadResult.Rejected(versionRejection)

        val image = ByteArray(RAW_BYTES)
        var offset = 0
        // READ returns four pages / 16 bytes.  Pages 00..83 are 33 commands;
        // FAST_READ then supplies the final pages 84..86 (12 bytes).
        for (page in 0..0x80 step 4) {
            val response = try {
                transceiver.transceive(byteArrayOf(READ_COMMAND.toByte(), page.toByte()))
            } catch (_: Exception) {
                return Ntag215ReadResult.Rejected(Ntag215Rejection.TRANSPORT_ERROR)
            }
            if (response.size != 16) {
                return Ntag215ReadResult.Rejected(Ntag215Rejection.READ_RESPONSE_INVALID)
            }
            response.copyInto(image, offset)
            offset += response.size
        }

        val finalRead = try {
            transceiver.transceive(FAST_READ_COMMAND.copyOf())
        } catch (_: Exception) {
            return Ntag215ReadResult.Rejected(Ntag215Rejection.TRANSPORT_ERROR)
        }
        if (finalRead.size != 12) {
            return Ntag215ReadResult.Rejected(Ntag215Rejection.FAST_READ_RESPONSE_INVALID)
        }
        finalRead.copyInto(image, offset)

        val imageRejection = validateImage(image)
        if (imageRejection != null) return Ntag215ReadResult.Rejected(imageRejection)
        if (!includeSignature) return Ntag215ReadResult.Success(image, signatureIncluded = false)

        // READ_SIG is optional on a physical tag.  Any transport error or
        // non-exact reply length falls back to the explicit raw 540-byte image.
        val signature = try {
            transceiver.transceive(READ_SIG_COMMAND.copyOf())
        } catch (_: Exception) {
            null
        }
        if (signature?.size != SIGNATURE_BYTES) {
            return Ntag215ReadResult.Success(image, signatureIncluded = false)
        }
        return Ntag215ReadResult.Success(image + signature, signatureIncluded = true)
    }

    /** Validate a complete raw phone-backup image without repairing bytes. */
    fun validateImage(image: ByteArray): Ntag215Rejection? {
        if (image.size == 2048) return Ntag215Rejection.FIGURE_V3_UNSUPPORTED
        if (image.size != RAW_BYTES && image.size != EXTENDED_BYTES) {
            return Ntag215Rejection.READ_RESPONSE_INVALID
        }
        if (image[0].u8() != 0x04) return Ntag215Rejection.MANUFACTURER_MISMATCH
        val bcc0 = 0x88 xor image[0].u8() xor image[1].u8() xor image[2].u8()
        val bcc1 = image[4].u8() xor image[5].u8() xor image[6].u8() xor image[7].u8()
        if (image[3].u8() != bcc0 || image[8].u8() != bcc1) return Ntag215Rejection.BCC_INVALID
        return null
    }

    private fun validateVersion(version: ByteArray): Ntag215Rejection? {
        if (version.size != NTAG215_GET_VERSION.size) {
            return Ntag215Rejection.GET_VERSION_RESPONSE_INVALID
        }
        if (version.contentEquals(NTAG215_GET_VERSION)) return null

        // Keep the common Type 2 version shape distinguishable in diagnostics.
        // NTAG213/216 differ in the storage-size byte [6].
        val commonType2Header = version[0].u8() == 0x00 &&
            version[1].u8() == 0x04 && version[2].u8() == 0x04 &&
            version[3].u8() == 0x02 && version[4].u8() == 0x01 &&
            version[5].u8() == 0x00 && version[7].u8() == 0x03
        if (commonType2Header && version[6].u8() != 0x11) return Ntag215Rejection.UNSUPPORTED_TAG

        // The v3 figure uses the NTAG I2C Plus 2K version reply and is a
        // separate image/command surface, not a truncated NTAG215 backup.
        if (version.contentEquals(byteArrayOf(0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03))) {
            return Ntag215Rejection.FIGURE_V3_UNSUPPORTED
        }
        return Ntag215Rejection.GET_VERSION_RESPONSE_INVALID
    }

    private fun Byte.u8(): Int = toInt() and 0xFF
}
