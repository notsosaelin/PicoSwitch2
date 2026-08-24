package dev.picoswitch.bridge.touch

import kotlin.math.cos
import kotlin.math.sin

/**
 * Platform-neutral GameCube face-button contours.
 *
 * These are twenty equal-arc samples of Dolphin's `gcpad_x.png` and
 * `gcpad_y.png` alpha >= 128 boundaries, normalized to each non-transparent
 * silhouette. Android turns them into cubic Canvas paths; shared input routing
 * uses the same geometry so interlocking artwork never disagrees with touch.
 */
object TouchGameCubeGeometry {
    fun contour(role: TouchVisualRole): List<TouchVector> = when (role) {
        TouchVisualRole.GameCubeBeanX -> X_CONTOUR
        TouchVisualRole.GameCubeBeanY -> Y_CONTOUR
        else -> error("$role is not a GameCube contour")
    }

    /** Points relative to the control centre after clockwise screen-space rotation. */
    fun orientedContour(
        role: TouchVisualRole,
        width: Float,
        height: Float,
        rotationDegrees: Float,
    ): List<TouchVector> {
        val radians = Math.toRadians(rotationDegrees.toDouble())
        val cosine = cos(radians).toFloat()
        val sine = sin(radians).toFloat()
        return contour(role).map { point ->
            val x = (point.x - 0.5f) * width
            val y = (point.y - 0.5f) * height
            TouchVector(x * cosine - y * sine, x * sine + y * cosine)
        }
    }

    /** Contour hit test with an optional rounded expansion for touch comfort. */
    fun contains(
        role: TouchVisualRole,
        x: Float,
        y: Float,
        width: Float,
        height: Float,
        rotationDegrees: Float,
        margin: Float,
    ): Boolean {
        val radians = Math.toRadians((-rotationDegrees).toDouble())
        val cosine = cos(radians).toFloat()
        val sine = sin(radians).toFloat()
        val localX = x * cosine - y * sine
        val localY = x * sine + y * cosine
        val points = contour(role).map { point ->
            TouchVector((point.x - 0.5f) * width, (point.y - 0.5f) * height)
        }
        if (insidePolygon(localX, localY, points)) return true
        if (margin <= 0f) return false
        val marginSquared = margin * margin
        return points.indices.any { index ->
            distanceSquaredToSegment(
                localX,
                localY,
                points[index],
                points[(index + 1) % points.size],
            ) <= marginSquared
        }
    }

    private fun insidePolygon(x: Float, y: Float, points: List<TouchVector>): Boolean {
        var inside = false
        var previous = points.last()
        points.forEach { current ->
            val crosses = (current.y > y) != (previous.y > y) &&
                x < (previous.x - current.x) * (y - current.y) /
                (previous.y - current.y) + current.x
            if (crosses) inside = !inside
            previous = current
        }
        return inside
    }

    private fun distanceSquaredToSegment(
        x: Float,
        y: Float,
        start: TouchVector,
        end: TouchVector,
    ): Float {
        val dx = end.x - start.x
        val dy = end.y - start.y
        val lengthSquared = dx * dx + dy * dy
        val t = if (lengthSquared > 0f) {
            (((x - start.x) * dx + (y - start.y) * dy) / lengthSquared).coerceIn(0f, 1f)
        } else {
            0f
        }
        val nearestX = start.x + t * dx
        val nearestY = start.y + t * dy
        val offsetX = x - nearestX
        val offsetY = y - nearestY
        return offsetX * offsetX + offsetY * offsetY
    }

    private val X_CONTOUR = listOf(
        TouchVector(0.2994f, 0.0000f), TouchVector(0.1126f, 0.0523f),
        TouchVector(0.0127f, 0.1634f), TouchVector(0.0301f, 0.2891f),
        TouchVector(0.1274f, 0.4010f), TouchVector(0.2038f, 0.5181f),
        TouchVector(0.2357f, 0.6467f), TouchVector(0.2484f, 0.7801f),
        TouchVector(0.2924f, 0.9055f), TouchVector(0.4533f, 0.9850f),
        TouchVector(0.6606f, 0.9932f), TouchVector(0.8418f, 0.9327f),
        TouchVector(0.9447f, 0.8223f), TouchVector(0.9936f, 0.6981f),
        TouchVector(1.0000f, 0.5631f), TouchVector(0.9682f, 0.4346f),
        TouchVector(0.9050f, 0.3140f), TouchVector(0.8111f, 0.2013f),
        TouchVector(0.6767f, 0.0990f), TouchVector(0.5048f, 0.0245f),
    )

    private val Y_CONTOUR = listOf(
        TouchVector(0.5922f, 0.0000f), TouchVector(0.4625f, 0.0309f),
        TouchVector(0.3407f, 0.0915f), TouchVector(0.2261f, 0.1789f),
        TouchVector(0.1217f, 0.3046f), TouchVector(0.0409f, 0.4670f),
        TouchVector(0.0039f, 0.6576f), TouchVector(0.0314f, 0.8493f),
        TouchVector(0.1310f, 0.9781f), TouchVector(0.2614f, 0.9939f),
        TouchVector(0.3743f, 0.8998f), TouchVector(0.4867f, 0.8042f),
        TouchVector(0.6101f, 0.7500f), TouchVector(0.7399f, 0.7195f),
        TouchVector(0.8711f, 0.6943f), TouchVector(0.9672f, 0.5632f),
        TouchVector(0.9991f, 0.3705f), TouchVector(0.9543f, 0.1850f),
        TouchVector(0.8517f, 0.0610f), TouchVector(0.7268f, 0.0122f),
    )
}
