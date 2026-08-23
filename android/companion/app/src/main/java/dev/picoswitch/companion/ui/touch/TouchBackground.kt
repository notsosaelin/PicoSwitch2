package dev.picoswitch.companion.ui.touch

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
// The framework's own ExifInterface reads JPEG only. The system photo picker
// returns HEIC on most modern phones, so the framework class would silently
// report "no orientation" and a sideways picture would be stored sideways.
import androidx.exifinterface.media.ExifInterface
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import java.io.File
import android.graphics.Matrix

/**
 * The user's background picture, kept as an app-private copy.
 *
 * COPIED rather than referenced, deliberately. A picker grant can lapse, the
 * picture can be deleted, and a stored reference to somebody's photo library is
 * a thing that stops working later for reasons the app cannot see. One
 * downsampled private copy is a few hundred kilobytes and simply keeps working —
 * no storage permission, no persistable-grant negotiation, nothing to revoke.
 *
 * Nothing leaves the device. The copy lives in the app's own files directory and
 * is deleted when the user removes the background.
 */
object TouchBackgroundStore {

    /**
     * Ceiling for the stored copy.
     *
     * Sized for a large display rather than for the source picture: a 30
     * megapixel original held in memory behind a controller is pure cost, and
     * the image is a backdrop that is then dimmed.
     */
    private const val MAX_WIDTH = 2560
    private const val MAX_HEIGHT = 1440
    private const val QUALITY = 88
    private const val FILE_NAME = "touch-background.jpg"

    fun file(context: Context): File = File(context.filesDir, FILE_NAME)

    /**
     * Decode, downsample, orient and store the picked image.
     *
     * Returns the stored path, or null when the picture could not be read at all
     * — a revoked grant, an unsupported format, a file that vanished between the
     * pick and the read.
     */
    fun adopt(context: Context, uri: Uri): String? = runCatching {
        val resolver = context.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null

        val options = BitmapFactory.Options().apply {
            inSampleSize = sampleSize(bounds.outWidth, bounds.outHeight)
        }
        val decoded = resolver.openInputStream(uri)?.use {
            BitmapFactory.decodeStream(it, null, options)
        } ?: return null

        // Orientation metadata, applied once here rather than at every draw.
        val rotation = resolver.openInputStream(uri)?.use { stream ->
            runCatching { ExifInterface(stream).rotationDegrees }.getOrDefault(0)
        } ?: 0
        val oriented = if (rotation == 0) decoded else {
            Bitmap.createBitmap(
                decoded, 0, 0, decoded.width, decoded.height,
                Matrix().apply { postRotate(rotation.toFloat()) }, true,
            )
        }

        val target = file(context)
        target.outputStream().use { oriented.compress(Bitmap.CompressFormat.JPEG, QUALITY, it) }
        if (oriented !== decoded) oriented.recycle()
        decoded.recycle()
        target.absolutePath
    }.getOrNull()

    fun remove(context: Context) {
        runCatching { file(context).delete() }
    }

    /** Load a stored copy, or null when it is gone. */
    fun load(path: String?): ImageBitmap? {
        val file = path?.let(::File) ?: return null
        if (!file.isFile) return null
        return runCatching { BitmapFactory.decodeFile(file.absolutePath)?.asImageBitmap() }.getOrNull()
    }

    /** Power-of-two subsample that first brings the image under the ceiling. */
    private fun sampleSize(width: Int, height: Int): Int {
        var sample = 1
        while (width / sample > MAX_WIDTH || height / sample > MAX_HEIGHT) sample *= 2
        return sample
    }
}
