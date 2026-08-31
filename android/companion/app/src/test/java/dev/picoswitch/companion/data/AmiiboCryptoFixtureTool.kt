package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCryptoState
import dev.picoswitch.companion.model.AmiiboDetails
import dev.picoswitch.companion.model.AmiiboIdentity
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.security.MessageDigest

/**
 * Generates the language-neutral amiibo crypto fixture, and surveys the corpus.
 *
 * WHY THIS EXISTS. `AmiiboCrypto.kt` is amiitool-compatible and mirrors the
 * block in `web/index.html`, so there are already TWO implementations of
 * security-adjacent code whose failures are silent — a wrong HMAC produces a tag
 * the console simply rejects, with nothing to read back. A third implementation
 * is about to be written in C#. One artifact all three verify against is the
 * smallest thing that removes that risk, and it is a stated precondition of the
 * Windows Amiibo phase (WINDOWS_PASS.md §16.7, §31).
 *
 * The fixture is driven by THIS implementation — the one that has been exercised
 * against real hardware — rather than written by hand.
 *
 * ## Never keys, and never someone's Mii name
 *
 * Two disclosure rules, both enforced by construction rather than by care:
 *
 *  - The fixture names a **key-set fingerprint**, never key material. A verifier
 *    proves it holds the same keys without a key ever entering the repository.
 *  - Decrypted names are recorded as a **digest**, never as text. A digest is a
 *    complete equality check — a wrong UTF-16 decode fails it — while the Mii
 *    name on a genuine tag stays out of the repository. Every tag in the corpus
 *    is set up and carries one.
 *
 * The `initialized` half of each vector needs neither rule: `initialize` wipes
 * the owner and nickname and refuses to return an image that still carries them,
 * so those bytes cannot contain personal data.
 *
 * ## Running it
 *
 * Neither method runs in an ordinary build: both need `key_retail.bin`, which is
 * never in the repository and must never be committed.
 *
 * ```
 * gradlew :app:testDebugUnitTest --tests '*AmiiboCryptoFixtureTool*' \
 *   -Damiibo.retailKey=<path to key_retail.bin> -Damiibo.regenerate=true
 * ```
 *
 * [AmiiboCryptoFixtureTest] is the one that runs everywhere, and it checks the
 * keyless half of the fixture with no key present at all.
 */
class AmiiboCryptoFixtureTool {

    private val repoRoot = File("../../..").canonicalFile

    private val keyFile: File? =
        System.getProperty("amiibo.retailKey")?.let(::File)?.takeIf { it.isFile }

    private val keys: AmiiboCrypto.RetailKeys? by lazy {
        keyFile?.let { runCatching { AmiiboCrypto.parseRetailKeys(it.readBytes()) }.getOrNull() }
    }

    /**
     * Report what the real implementation makes of every tracked dump.
     *
     * Diagnostic, not an assertion: which dumps are usable as vectors is decided
     * from what the implementation actually produces, not from their filenames.
     */
    @Test fun `survey the tracked amiibo dumps`() {
        assumeTrue("needs -Damiibo.retailKey", keys != null)
        val retail = keys!!

        candidates().forEach { file ->
            val bytes = file.readBytes()
            val identity = runCatching { AmiiboCrypto.identity(bytes) }.getOrNull()
            if (identity == null) {
                println("${file.name}: not a tag image (${bytes.size} bytes)")
                return@forEach
            }
            val details = AmiiboCrypto.readDetails(bytes, retail)
            val initialized = runCatching { AmiiboCrypto.initialize(bytes, retail) }
            println(
                "${file.name}: ${bytes.size} B  ${identity.tagType}  id=${identity.figureId}  " +
                    "crypto=${details.crypto}  setUp=${details.setUp}  " +
                    "appData=${details.hasAppData}  writes=${details.writeCounter}  " +
                    "named=${details.nickname.isNotEmpty() || details.owner.isNotEmpty()}  " +
                    "initialize=${initialized.map { "ok" }.getOrElse { it.message?.take(60) }}",
            )
        }
    }

    /** Regenerate `tools/fixtures/amiibo/crypto-vectors.json`. */
    @Test fun `regenerate the crypto fixture`() {
        assumeTrue("needs -Damiibo.retailKey", keys != null)
        assumeTrue(
            "needs -Damiibo.regenerate=true",
            System.getProperty("amiibo.regenerate") == "true",
        )
        val retail = keys!!

        val accepted = mutableListOf<JsonObject>()
        val rejected = mutableListOf<JsonObject>()

        candidates().forEach { file ->
            val bytes = file.readBytes()
            val relative = file.relativeTo(repoRoot).invariantSeparatorsPath

            val identity = runCatching { AmiiboCrypto.identity(bytes) }.getOrNull()
            if (identity == null) {
                // A NEGATIVE vector. All three implementations must agree about
                // refusing a file that is not a tag image, and this corpus has
                // two of them — 668-byte captures that carry the v3 marker but
                // are not a supported size.
                rejected += buildJsonObject {
                    put("source", relative)
                    put("sourceSha256", amiiboSha256(bytes))
                    put("size", bytes.size)
                    put("reason", "not a supported tag image size (540, 572 or 2048)")
                }
                return@forEach
            }

            val details = AmiiboCrypto.readDetails(bytes, retail)
            check(details.crypto == AmiiboCryptoState.Valid) {
                "$relative did not decrypt; it is not fit to be a vector"
            }
            val initialized = AmiiboCrypto.initialize(bytes, retail)
            val initializedDetails = AmiiboCrypto.readDetails(initialized, retail)
            check(initializedDetails.owner.isEmpty() && initializedDetails.nickname.isEmpty()) {
                "$relative initialized to an image that still carries a name"
            }

            accepted += vector(file, relative, bytes, identity, details,
                               initialized, initializedDetails)
        }

        val document = buildJsonObject {
            put(
                "note",
                "Amiibo crypto vectors, generated from the Kotlin implementation in " +
                    "android/companion/app/src/main/java/dev/picoswitch/companion/data/" +
                    "AmiiboCrypto.kt by AmiiboCryptoFixtureTool. One artifact for all " +
                    "implementations (Kotlin, web/index.html, C#), because a wrong HMAC " +
                    "produces a tag the console silently rejects. NO KEY MATERIAL: the key " +
                    "set is named by fingerprint only. NO PERSONAL DATA: decrypted Mii " +
                    "names are recorded as a digest, never as text.",
            )
            put(
                "generator",
                "android/companion/app/src/test/java/dev/picoswitch/companion/data/" +
                    "AmiiboCryptoFixtureTool.kt",
            )
            put(
                "regenerate",
                "gradlew :app:testDebugUnitTest --tests '*AmiiboCryptoFixtureTool*' " +
                    "-Damiibo.retailKey=<path> -Damiibo.regenerate=true",
            )
            put(
                "nameDigest",
                "sha256(nickname + owner), UTF-8. A complete equality check on the decoded " +
                    "names that discloses neither. Both empty digests as sha256 of the empty " +
                    "string, so producing a name where there is none fails just as loudly.",
            )
            put("coverage", coverage(accepted))
            put(
                "keySet",
                buildJsonObject {
                    put("fingerprint", "sha256:${amiiboSha256(keyFile!!.readBytes())}")
                    put(
                        "note",
                        "SHA-256 of key_retail.bin, so a verifier can prove it holds the " +
                            "same key set. key_retail.bin is never committed.",
                    )
                },
            )
            put("vectors", JsonArray(accepted))
            put("rejected", JsonArray(rejected))
        }

        val output = File(repoRoot, FIXTURE)
        output.parentFile.mkdirs()
        output.writeText(Json { prettyPrint = true }.encodeToString(document) + "\n")
        println("wrote ${output.relativeTo(repoRoot).invariantSeparatorsPath}")
    }

    /**
     * What these vectors actually cover, derived from the vectors themselves.
     *
     * DERIVED, never asserted by hand. The first version of this fixture
     * hardcoded its own coverage claims, which meant that adding a tag would
     * have left the fixture stating a gap it no longer had — and, worse, that
     * removing one would have left it claiming coverage it had lost. A reader
     * deciding whether their implementation is verified has to be able to trust
     * this block.
     */
    private fun coverage(vectors: List<JsonObject>): JsonObject {
        val tagTypes = vectors
            .map { it["identity"]!!.jsonObject["tagType"]!!.jsonPrimitive.content }
            .toSet()
        val setUpStates = vectors
            .map { it["register"]!!.jsonObject["setUp"]!!.jsonPrimitive.boolean }
            .toSet()
        val appDataStates = vectors
            .map { it["register"]!!.jsonObject["hasAppData"]!!.jsonPrimitive.boolean }
            .toSet()

        // Tag type CROSSED with register state, not each independently. Having a
        // set-up tag and having a v3 tag does not mean the set-up v3 decode is
        // covered, and it is the combination an implementation gets wrong: the
        // two sizes place the encrypted blocks 0x40 bytes apart.
        val combinations = vectors
            .map {
                it["identity"]!!.jsonObject["tagType"]!!.jsonPrimitive.content to
                    it["register"]!!.jsonObject["setUp"]!!.jsonPrimitive.boolean
            }
            .toSet()

        val missing = buildList {
            if ("Ntag215" !in tagTypes) {
                add("no 540- or 572-byte NTAG215 image: that whole tag path is unverified")
            }
            if ("FigureV3" !in tagTypes) {
                add("no 2048-byte Figure v3 image: that whole tag path is unverified")
            }
            if (false !in setUpStates) {
                add("no un-set-up tag: the empty-register decode is unverified")
            }
            if (true !in setUpStates) {
                add("no set-up tag: owner, nickname and date decoding are unverified")
            }
            if (true !in appDataStates) {
                add("no tag carrying game app data: title and app id decoding are unverified")
            }
            for (type in tagTypes.sorted()) {
                if (true !in combinations.filter { it.first == type }.map { it.second }) {
                    add("no SET-UP $type tag: its register decode is unverified")
                }
                if (false !in combinations.filter { it.first == type }.map { it.second }) {
                    add(
                        "no UN-SET-UP $type tag: the state a freshly imported one is in " +
                            "is unverified",
                    )
                }
            }
        }

        return buildJsonObject {
            put("ntag215", "Ntag215" in tagTypes)
            put("figureV3", "FigureV3" in tagTypes)
            put("setUpTag", true in setUpStates)
            put("unsetTag", false in setUpStates)
            put("appDataTag", true in appDataStates)
            put(
                "tagTypeAndRegisterState",
                JsonArray(
                    combinations.sortedWith(compareBy({ it.first }, { it.second })).map {
                        JsonPrimitive("${it.first}/${if (it.second) "set-up" else "un-set-up"}")
                    },
                ),
            )
            put(
                "gap",
                if (missing.isEmpty()) {
                    "None known. Both tag sizes, both register states and the app-data " +
                        "path are represented. This says nothing about firmware or console " +
                        "behaviour, which no software fixture can cover."
                } else {
                    "An implementation verified only against these vectors is unverified " +
                        "for: " + missing.joinToString("; ") + ". Adding a matching dump to " +
                        "dumps/amiibo/ and regenerating closes it."
                },
            )
        }
    }

    private fun vector(
        file: File,
        relative: String,
        bytes: ByteArray,
        identity: AmiiboIdentity,
        details: AmiiboDetails,
        initialized: ByteArray,
        initializedDetails: AmiiboDetails,
    ): JsonObject = buildJsonObject {
        put("label", file.nameWithoutExtension)
        put("source", relative)
        put("sourceSha256", amiiboSha256(bytes))
        put("size", bytes.size)
        put(
            "identity",
            buildJsonObject {
                put("uid", identity.uid)
                put("figureId", identity.figureId)
                put("tagType", identity.tagType.name)
                put("characterGameCode", identity.characterGameCode)
                put("characterVariant", identity.characterVariant)
                put("typeName", identity.typeName)
                put("modelNumber", identity.modelNumber)
                put("seriesCode", identity.seriesCode)
                put("formatVersion", identity.formatVersion)
                put("extendedVariant", identity.extendedVariant)
            },
        )
        put("crypto", details.crypto.name)
        put("register", register(details))
        put(
            "initialized",
            buildJsonObject {
                // Deterministic: initialize zeroes fixed ranges and re-signs, with
                // no timestamp and no randomness, so the digest is a complete
                // equality check on the re-packed image.
                put("sha256", amiiboSha256(initialized))
                put("crypto", initializedDetails.crypto.name)
                put("setUp", initializedDetails.setUp)
                put("hasAppData", initializedDetails.hasAppData)
                put("writeCounter", initializedDetails.writeCounter)
                put(
                    "nameDigest",
                    amiiboNameDigest(initializedDetails.nickname, initializedDetails.owner),
                )
            },
        )
    }

    private fun register(details: AmiiboDetails): JsonObject = buildJsonObject {
        put("setUp", details.setUp)
        put("hasAppData", details.hasAppData)
        put("writeCounter", details.writeCounter)
        // Written explicitly rather than as `x?.let { put(k, it) } ?: put(k, JsonNull)`.
        // `put` returns the PREVIOUS value, which is null on a first insertion, so
        // that idiom takes the elvis branch every time and silently wrote every
        // date as null -- which the verifier then dutifully "confirmed".
        put("setupDate", details.setupDate?.let(::JsonPrimitive) ?: JsonNull)
        put("lastWriteDate", details.lastWriteDate?.let(::JsonPrimitive) ?: JsonNull)
        put("titleId", details.titleId)
        put("appId", details.appId)
        put("appDataLabel", details.appDataLabel)
        put("nameDigest", amiiboNameDigest(details.nickname, details.owner))
    }

    private fun candidates(): List<File> =
        File(repoRoot, "dumps/amiibo").listFiles().orEmpty()
            .filter { it.isFile && it.extension == "bin" }
            .sortedBy { it.name }

    private companion object {
        const val FIXTURE = "tools/fixtures/amiibo/crypto-vectors.json"
    }
}

/**
 * A complete equality check on the decoded names, disclosing neither.
 *
 * Shared by the generator and the verifier deliberately. When each had its own
 * copy they drifted on the separator alone -- one used a NUL, the other a space
 * -- and every name comparison in the fixture was vacuous until the mismatch
 * happened to surface. One definition cannot drift, which is the same argument
 * this whole fixture makes about the three crypto implementations.
 *
 * The empty case still digests, so an implementation that produced a name where
 * there is none fails just as loudly as one that produced the wrong name.
 */
internal fun amiiboNameDigest(nickname: String, owner: String): String =
    amiiboSha256("$nickname$owner".toByteArray(Charsets.UTF_8))

internal fun amiiboSha256(bytes: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it) }
