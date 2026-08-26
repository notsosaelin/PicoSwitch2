package dev.picoswitch.companion.ui.touch

import android.os.SystemClock
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.pointerInput
import dev.picoswitch.bridge.touch.TouchContact
import dev.picoswitch.bridge.touch.TouchContactTracker
import dev.picoswitch.bridge.touch.TouchPhase
import dev.picoswitch.bridge.touch.TouchReleaseReason
import kotlinx.coroutines.CancellationException

/**
 * Compose pointer events -> portable touch contacts.
 *
 * This is the whole Android half of the input path, and it is deliberately
 * small: identify the contacts, convert their coordinates, forward them. Nothing
 * about sticks, sectors, ownership or neutralization is decided here.
 *
 * ## Why the low-level API rather than gesture detectors
 *
 * A controller surface needs five contacts working independently, immediately.
 * The high-level helpers are built for the opposite case: `detectDragGestures`
 * waits for touch slop before it reports anything, which is exactly wrong for a
 * stick that should answer the first pixel; several of them compete for gesture
 * ownership, so a second stick can fail to start while the first is dragging;
 * and click semantics have no notion of a five-finger chord. Routing once, at the
 * root, makes overlap, priority and cancellation explicit instead of emergent.
 *
 * ## Why the identifier and never the index
 *
 * `PointerId` is stable for the lifetime of a contact. Its position inside
 * `changes` is not, and reorders freely between events. Every ownership decision
 * downstream is keyed on `PointerId.value`, and this adapter never passes an
 * index anywhere.
 *
 * ## Coordinates
 *
 * Positions arrive relative to this modifier's own node, which is the same box
 * the layout was resolved against, so no translation happens here at all. That
 * is the point: display density, window origin, rotation and insets are resolved
 * before the layout is built, and by the time a contact reaches the portable
 * engine it is just a point in the same plane as the control geometry.
 */
fun Modifier.touchGamepadContacts(
    /**
     * Restarts the handler when it changes. Pass whatever invalidates the
     * geometry or the session; the block releases on the way out, so a restart is
     * a clean boundary rather than a state leak.
     */
    key: Any?,
    tracker: TouchContactTracker,
    /**
     * Called once per platform event, after the whole batch has been applied.
     *
     * This is where the renderer refreshes its picture. Deliberately event-driven
     * rather than frame-driven: a `withFrameNanos` loop would keep the frame
     * clock running at display rate for as long as the controller is on screen,
     * which is a measurable battery cost for a surface that is idle whenever no
     * thumb is moving.
     */
    afterBatch: () -> Unit = {},
): Modifier = pointerInput(key) {
    try {
        awaitPointerEventScope {
            val batch = ArrayList<TouchContact>(MAX_EXPECTED_CONTACTS)
            while (true) {
                // Initial pass: this surface is the gameplay controller and owns
                // its area outright. Waiting for the Main pass would let any
                // helper composed above it take a contact first.
                val event = awaitPointerEvent(PointerEventPass.Initial)
                batch.clear()
                event.changes.forEach { change ->
                    batch += change.toContact()
                    // Consume, so nothing behind this surface reacts to gameplay
                    // contacts. The menu and status affordances are drawn ABOVE
                    // it and are unaffected.
                    change.consume()
                }
                tracker.dispatch(batch)
                afterBatch()
            }
        }
    } catch (cancellation: CancellationException) {
        // The handler is being torn down mid-gesture: a key change, a
        // recomposition that removed this node, or the surface leaving. Whatever
        // was held has no contact left that could release it.
        tracker.releaseAll(TouchReleaseReason.Disposed)
        afterBatch()
        throw cancellation
    }
}

/**
 * One Compose change, classified.
 *
 * `pressed` and `previousPressed` are the only two facts needed: their
 * transition is the phase. A move is reported even when the position is
 * unchanged; the engine drops it because the resulting contribution is equal,
 * which is a cheaper place to decide that than here.
 */
private fun PointerInputChange.toContact(): TouchContact {
    val phase = when {
        pressed && !previousPressed -> TouchPhase.Down
        !pressed && previousPressed -> TouchPhase.Up
        pressed -> TouchPhase.Move
        // Not pressed and was not pressed: a hover or a stray. Treated as an end
        // so it can never be mistaken for a held contact.
        else -> TouchPhase.Cancel
    }
    return TouchContact(
        id = id.value,
        phase = phase,
        x = position.x,
        y = position.y,
        timeNanos = uptimeMillis * NANOS_PER_MILLI,
    )
}

/** Shared with the surface, which converts the platform's gesture timeouts too. */
internal const val NANOS_PER_MILLI = 1_000_000L

/**
 * The one definition of "now" in the same clock a contact is stamped with.
 *
 * Compose reports `uptimeMillis` on every pointer change, and the engine's
 * gesture deadlines are absolute values in that clock. Anything driving those
 * deadlines has to read the SAME clock — `System.nanoTime` and
 * `currentTimeMillis` are both different timelines — so both the stamp above and
 * the surface's tick driver come from here.
 */
internal fun touchClockNanos(): Long = SystemClock.uptimeMillis() * NANOS_PER_MILLI

/** Sized for a comfortable chord; the list grows if a device reports more. */
private const val MAX_EXPECTED_CONTACTS = 8
