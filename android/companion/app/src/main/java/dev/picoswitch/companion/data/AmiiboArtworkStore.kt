package dev.picoswitch.companion.data

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Catalog artwork, fetched once and kept.
 *
 * WHAT THIS REPLACES. Every tile used to open its own HTTP connection the moment
 * it scrolled into view, decode the response at full resolution, and throw the
 * result away when it scrolled off. Browsing a thousand-figure library therefore
 * re-downloaded the same images continuously and decoded megapixel bitmaps for
 * 150dp tiles — on a phone that is the difference between a smooth grid and a
 * stuttering one, and it is a great deal of traffic for pictures that never
 * change.
 *
 * Three layers, cheapest first:
 *
 * 1. A memory cache bounded by BYTES, not entries. Amiibo artwork varies enough
 *    in size that a count-based budget would be meaningless.
 * 2. A disk cache, so the second run of the app draws the library without a
 *    network at all. Artwork for a released figure is immutable, which is what
 *    makes caching it indefinitely correct rather than merely convenient.
 * 3. The network, once per URL. Concurrent callers for the same URL wait on one
 *    request instead of starting several — a fast scroll asks for the same tile
 *    repeatedly, and without this each ask was another download.
 *
 * Decoding is downsampled to roughly the size actually being drawn. A tile does
 * not become sharper for holding a bitmap eight times larger than its own box;
 * it only becomes more expensive to keep.
 */
class AmiiboArtworkStore(
    cacheRoot: File,
    maxMemoryBytes: Int = DEFAULT_MEMORY_BYTES,
    private val maxDiskBytes: Long = DEFAULT_DISK_BYTES,
) {
    private val directory = File(cacheRoot, "amiibo-artwork")

    private val memory = object : LruCache<String, Bitmap>(maxMemoryBytes) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.byteCount
    }

    /**
     * One in-flight request per URL.
     *
     * Guarded by [locksGuard] rather than a concurrent map so the map itself
     * cannot grow without bound: entries are removed once nobody is waiting.
     */
    private val locks = mutableMapOf<String, Mutex>()
    private val locksGuard = Mutex()

    /** A cached bitmap, if one is already in memory. Never touches disk. */
    fun peek(url: String, targetPx: Int): Bitmap? = memory[key(url, targetPx)]

    /**
     * The artwork for [url], drawn at roughly [targetPx].
     *
     * Returns null for a blank URL, an unusable address, a failed request or an
     * undecodable body. A missing image is a placeholder, never an error: the
     * library is fully usable offline and artwork is decoration.
     */
    suspend fun load(url: String, targetPx: Int): Bitmap? {
        if (url.isBlank() || targetPx <= 0) return null

        val key = key(url, targetPx)
        memory[key]?.let { return it }

        val lock = locksGuard.withLock { locks.getOrPut(key) { Mutex() } }
        try {
            return lock.withLock {
                // Another caller may have finished while this one waited.
                memory[key] ?: withContext(Dispatchers.IO) {
                    val bytes = readDisk(url) ?: download(url)?.also { writeDisk(url, it) }
                    bytes?.let { decode(it, targetPx) }?.also { memory.put(key, it) }
                }
            }
        } finally {
            locksGuard.withLock { if (!lock.isLocked) locks.remove(key) }
        }
    }

    // ------------------------------------------------------------------ disk

    private fun fileFor(url: String) = File(directory, digest(url))

    private fun readDisk(url: String): ByteArray? = runCatching {
        fileFor(url).takeIf { it.isFile }?.readBytes()?.takeIf { it.isNotEmpty() }
    }.getOrNull()

    private fun writeDisk(url: String, bytes: ByteArray) {
        runCatching {
            directory.mkdirs()
            // Written aside and moved, so a kill mid-write cannot leave a
            // truncated file that would decode as a broken image forever.
            val temporary = File(directory, "${digest(url)}.part")
            temporary.writeBytes(bytes)
            if (!temporary.renameTo(fileFor(url))) temporary.delete()
            trimDisk()
        }
    }

    /**
     * Keep the cache under budget, oldest first.
     *
     * Crude by design: this runs after a write, over a directory of small files,
     * and the alternative — an index that has to stay consistent with the
     * filesystem — is a second source of truth for something that can always be
     * refetched.
     */
    private fun trimDisk() {
        val files = directory.listFiles()?.sortedBy { it.lastModified() } ?: return
        var total = files.sumOf { it.length() }
        for (file in files) {
            if (total <= maxDiskBytes) return
            total -= file.length()
            file.delete()
        }
    }

    // --------------------------------------------------------------- network

    private fun download(url: String): ByteArray? = runCatching {
        val connection = URL(url).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = CONNECT_TIMEOUT_MS
            connection.readTimeout = READ_TIMEOUT_MS
            connection.instanceFollowRedirects = true
            if (connection.responseCode !in 200..299) return null

            connection.inputStream.use { input ->
                val output = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    // A catalog thumbnail that large is not a thumbnail; refuse
                    // it rather than hold it in memory.
                    if (output.size() + count > MAX_IMAGE_BYTES) return null
                    output.write(buffer, 0, count)
                }
                output.toByteArray()
            }
        } finally {
            connection.disconnect()
        }
    }.getOrNull()

    // ---------------------------------------------------------------- decode

    /**
     * Decode at the smallest power-of-two scale that still covers [targetPx].
     *
     * inSampleSize is the only downsample the decoder does without allocating
     * the full-size bitmap first, which is the entire point: the expensive part
     * was never the download.
     */
    private fun decode(bytes: ByteArray, targetPx: Int): Bitmap? = runCatching {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)

        val longest = maxOf(bounds.outWidth, bounds.outHeight)
        var sample = 1
        while (longest / (sample * 2) >= targetPx) sample *= 2

        BitmapFactory.decodeByteArray(
            bytes, 0, bytes.size,
            BitmapFactory.Options().apply { inSampleSize = sample },
        )
    }.getOrNull()

    // ------------------------------------------------------------------ keys

    /**
     * Bucketed by size so the grid and the carousel do not evict each other.
     *
     * Rounding to a power of two keeps the number of buckets small; a cache
     * keyed on exact pixel widths would hold a separate copy per layout pass.
     */
    private fun key(url: String, targetPx: Int): String {
        var bucket = 64
        while (bucket < targetPx && bucket < 1024) bucket *= 2
        return "$url@$bucket"
    }

    private fun digest(url: String): String =
        MessageDigest.getInstance("SHA-256")
            .digest(url.toByteArray())
            .joinToString("") { "%02x".format(it) }

    companion object {
        private const val CONNECT_TIMEOUT_MS = 2_500
        private const val READ_TIMEOUT_MS = 8_000
        private const val MAX_IMAGE_BYTES = 2 * 1024 * 1024

        /**
         * A sixteenth of the heap. Generous enough to hold a screenful several
         * times over while scrolling, small enough that the cache is never the
         * reason the app is killed.
         */
        private val DEFAULT_MEMORY_BYTES: Int
            get() = (Runtime.getRuntime().maxMemory() / 16)
                .coerceIn(4L * 1024 * 1024, 48L * 1024 * 1024)
                .toInt()

        private const val DEFAULT_DISK_BYTES = 64L * 1024 * 1024
    }
}
