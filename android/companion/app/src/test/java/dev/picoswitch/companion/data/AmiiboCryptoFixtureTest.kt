package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCryptoState
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.security.MessageDigest

/**
 * The shared amiibo crypto fixture, verified against this implementation.
 *
 * The Kotlin end of `tools/fixtures/amiibo/crypto-vectors.json`. The C# port and
 * the `web/` implementation get their own verifiers against the same artifact,
 * which is the whole point: three implementations of code whose failure mode is
 * a tag the console silently rejects, checked against one thing.
 *
 * ## It runs with no keys
 *
 * Everything that does not need `key_retail.bin` is asserted unconditionally —
 * the vectors' source files still exist and still hash the same, identity
 * decodes exactly, and the negative vectors are still refused. That is what
 * keeps the fixture meaningful on a machine, or in CI, that has no key material
 * and never will.
 *
 * The decrypt-dependent half runs only when `-Damiibo.retailKey=<path>` is given,
 * and is skipped rather than failed otherwise. A skipped assertion is honest; a
 * fixture that could only be checked by whoever holds the keys would rot.
 */
class AmiiboCryptoFixtureTest {

    private val repoRoot = File("../../..").canonicalFile
    private val fixtureFile = File(repoRoot, "tools/fixtures/amiibo/crypto-vectors.json")

    private val fixture: JsonObject by lazy {
        Json.parseToJsonElement(fixtureFile.readText()).jsonObject
    }

    private val vectors: List<JsonObject> by lazy {
        fixture["vectors"]!!.jsonArray.map { it.jsonObject }
    }

    private val keyFile: File? =
        System.getProperty("amiibo.retailKey")?.let(::File)?.takeIf { it.isFile }

    private val keys: AmiiboCrypto.RetailKeys? by lazy {
        keyFile?.let { runCatching { AmiiboCrypto.parseRetailKeys(it.readBytes()) }.getOrNull() }
    }

    // ------------------------------------------------- always, without keys

    @Test fun `the fixture exists and carries vectors`() {
        assertTrue(
            "missing ${fixtureFile.path}; regenerate with AmiiboCryptoFixtureTool",
            fixtureFile.isFile,
        )
        assertTrue("the fixture has no vectors", vectors.isNotEmpty())
    }

    @Test fun `the fixture contains no key material`() {
        // The key set is named by fingerprint. A 160-byte key file would be 320
        // hex characters; nothing that long has any business being in here, and
        // this fails loudly if a future change starts inlining one.
        val fingerprint = fixture["keySet"]!!.jsonObject["fingerprint"]!!.jsonPrimitive.content
        assertTrue("the key set must be a sha256 fingerprint", fingerprint.startsWith("sha256:"))
        assertEquals("a sha256 is 64 hex characters", 64, fingerprint.removePrefix("sha256:").length)

        val longHex = Regex("[0-9a-fA-F]{96,}").find(fixtureFile.readText())
        assertNull("a long hex blob in the fixture looks like key or tag material", longHex)
    }

    @Test fun `every vector's source file is present and unchanged`() {
        // The vectors describe specific bytes. If a dump is edited or replaced
        // without regenerating, every downstream expectation is silently about a
        // different tag.
        vectors.forEach { vector ->
            val source = File(repoRoot, vector.string("source"))
            assertTrue("missing ${vector.string("source")}", source.isFile)
            assertEquals(
                "${vector.string("label")} source has changed since the fixture was generated",
                vector.string("sourceSha256"),
                sha256(source.readBytes()),
            )
            assertEquals(vector["size"]!!.jsonPrimitive.int, source.readBytes().size)
        }
    }

    @Test fun `identity decodes exactly, with no keys at all`() {
        // Identity is plaintext on the tag. It is the half of the contract a
        // client can honour before the user has supplied any keys, so it is
        // asserted here unconditionally.
        vectors.forEach { vector ->
            val bytes = File(repoRoot, vector.string("source")).readBytes()
            val identity = AmiiboCrypto.identity(bytes)
            val expected = vector["identity"]!!.jsonObject
            val label = vector.string("label")

            assertEquals("$label uid", expected.string("uid"), identity.uid)
            assertEquals("$label figureId", expected.string("figureId"), identity.figureId)
            assertEquals("$label tagType", expected.string("tagType"), identity.tagType.name)
            assertEquals(
                "$label characterGameCode",
                expected.string("characterGameCode"),
                identity.characterGameCode,
            )
            assertEquals(
                "$label characterVariant",
                expected["characterVariant"]!!.jsonPrimitive.int,
                identity.characterVariant,
            )
            assertEquals("$label typeName", expected.string("typeName"), identity.typeName)
            assertEquals(
                "$label modelNumber",
                expected.string("modelNumber"),
                identity.modelNumber,
            )
            assertEquals(
                "$label seriesCode",
                expected["seriesCode"]!!.jsonPrimitive.int,
                identity.seriesCode,
            )
            assertEquals(
                "$label formatVersion",
                expected["formatVersion"]!!.jsonPrimitive.int,
                identity.formatVersion,
            )
            assertEquals(
                "$label extendedVariant",
                expected.string("extendedVariant"),
                identity.extendedVariant,
            )
        }
    }

    @Test fun `the rejected vectors are still refused`() {
        // Negative knowledge, kept deliberately: these two files carry the v3
        // marker and look like tags, and every implementation must refuse them
        // for their size rather than half-parse one.
        val rejected = fixture["rejected"]!!.jsonArray.map { it.jsonObject }
        assertTrue("no negative vectors", rejected.isNotEmpty())
        rejected.forEach { entry ->
            val file = File(repoRoot, entry.string("source"))
            assertTrue("missing ${entry.string("source")}", file.isFile)
            val bytes = file.readBytes()
            assertEquals(entry.string("sourceSha256"), sha256(bytes))
            assertFalse(
                "${entry.string("source")} is now accepted; the fixture says it must not be",
                runCatching { AmiiboCrypto.identity(bytes) }.isSuccess,
            )
        }
    }

    @Test fun `the stated coverage matches the vectors actually present`() {
        // The coverage block is what a reader consults to decide whether their
        // implementation is verified, so it must describe these vectors rather
        // than whatever was true when someone last edited it by hand. Checked
        // against the vectors instead of against a constant, so adding or
        // removing a dump cannot leave the claim stale.
        val coverage = fixture["coverage"]!!.jsonObject
        val tagTypes = vectors.map { it["identity"]!!.jsonObject.string("tagType") }.toSet()

        assertEquals(
            "ntag215 coverage claim",
            "Ntag215" in tagTypes,
            coverage["ntag215"]!!.jsonPrimitive.boolean,
        )
        assertEquals(
            "figureV3 coverage claim",
            "FigureV3" in tagTypes,
            coverage["figureV3"]!!.jsonPrimitive.boolean,
        )

        // Both tag paths and both register states are what make the fixture
        // worth verifying a new implementation against; an un-set-up tag is the
        // only thing that exercises the empty-register decode.
        assertTrue("both tag sizes must be represented", tagTypes.containsAll(
            setOf("Ntag215", "FigureV3"),
        ))
        assertTrue("a set-up tag must be represented", coverage["setUpTag"]!!.jsonPrimitive.boolean)
        assertTrue("an un-set-up tag must be represented", coverage["unsetTag"]!!.jsonPrimitive.boolean)
        assertTrue("an app-data tag must be represented", coverage["appDataTag"]!!.jsonPrimitive.boolean)
        assertTrue("the gap must be stated either way", coverage.string("gap").isNotBlank())
    }

    // -------------------------------------------------- only with real keys

    @Test fun `the key set matches the one the fixture was generated with`() {
        assumeTrue("needs -Damiibo.retailKey", keyFile != null)
        assertEquals(
            "a different key set: the decrypt expectations below would not apply",
            fixture["keySet"]!!.jsonObject.string("fingerprint"),
            "sha256:${sha256(keyFile!!.readBytes())}",
        )
    }

    @Test fun `decrypted register fields match`() {
        assumeTrue("needs -Damiibo.retailKey", keys != null)
        val retail = keys!!

        vectors.forEach { vector ->
            val bytes = File(repoRoot, vector.string("source")).readBytes()
            val details = AmiiboCrypto.readDetails(bytes, retail)
            val expected = vector["register"]!!.jsonObject
            val label = vector.string("label")

            assertEquals("$label crypto", vector.string("crypto"), details.crypto.name)
            assertEquals(AmiiboCryptoState.Valid, details.crypto)
            assertEquals("$label setUp", expected["setUp"]!!.jsonPrimitive.boolean, details.setUp)
            assertEquals(
                "$label hasAppData",
                expected["hasAppData"]!!.jsonPrimitive.boolean,
                details.hasAppData,
            )
            assertEquals(
                "$label writeCounter",
                expected["writeCounter"]!!.jsonPrimitive.int,
                details.writeCounter,
            )
            assertEquals("$label setupDate", expected.nullable("setupDate"), details.setupDate)
            assertEquals(
                "$label lastWriteDate",
                expected.nullable("lastWriteDate"),
                details.lastWriteDate,
            )
            assertEquals("$label titleId", expected.string("titleId"), details.titleId)
            assertEquals("$label appId", expected.string("appId"), details.appId)
            assertEquals(
                "$label appDataLabel",
                expected.string("appDataLabel"),
                details.appDataLabel,
            )
            // A digest rather than the text: a wrong UTF-16 decode fails this
            // just as loudly, and the Mii name stays out of the repository.
            assertEquals(
                "$label decoded names",
                expected.string("nameDigest"),
                nameDigest(details.nickname, details.owner),
            )
        }
    }

    @Test fun `re-signing produces the same bytes every time`() {
        assumeTrue("needs -Damiibo.retailKey", keys != null)
        val retail = keys!!

        vectors.forEach { vector ->
            val bytes = File(repoRoot, vector.string("source")).readBytes()
            val expected = vector["initialized"]!!.jsonObject
            val label = vector.string("label")

            val initialized = AmiiboCrypto.initialize(bytes, retail)
            // The strongest single check in the fixture: it covers the decrypt,
            // the wipe, the re-encrypt and both HMACs in one comparison.
            assertEquals(
                "$label re-signed image",
                expected.string("sha256"),
                sha256(initialized),
            )

            val details = AmiiboCrypto.readDetails(initialized, retail)
            assertEquals("$label re-signed crypto", AmiiboCryptoState.Valid, details.crypto)
            assertFalse("$label must not still be set up", details.setUp)
            assertEquals(
                "$label re-signed names",
                expected.string("nameDigest"),
                nameDigest(details.nickname, details.owner),
            )
        }
    }

    @Test fun `re-signing is idempotent`() {
        // Initializing an already-initialized image must be a no-op in content
        // terms. If it were not, a second Initialize in the UI would produce a
        // different tag than the first and the user could not tell.
        assumeTrue("needs -Damiibo.retailKey", keys != null)
        val retail = keys!!

        val bytes = File(repoRoot, vectors.first().string("source")).readBytes()
        val once = AmiiboCrypto.initialize(bytes, retail)
        val twice = AmiiboCrypto.initialize(once, retail)
        assertEquals(sha256(once), sha256(twice))
    }

    // ------------------------------------------------------------- helpers

    private fun JsonObject.string(key: String): String =
        this[key]!!.jsonPrimitive.content

    private fun JsonObject.nullable(key: String): String? =
        this[key]?.takeIf { it != JsonNull }?.jsonPrimitive?.content

    // Both borrowed from the generator rather than reimplemented. Two private
    // copies is exactly how the separator drifted the first time.
    private fun nameDigest(nickname: String, owner: String): String =
        amiiboNameDigest(nickname, owner)

    private fun sha256(bytes: ByteArray): String = amiiboSha256(bytes)
}
