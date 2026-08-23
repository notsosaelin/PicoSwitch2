package dev.picoswitch.bridge.touch

/**
 * Keeps a platform's contact reporting honest before it reaches the engine.
 *
 * A platform adapter converts one native event into the contacts that event
 * describes and hands the batch here. Two things then happen that every adapter
 * would otherwise have to reimplement:
 *
 * 1. contacts are forwarded in order, so Down / Move / Up stay deterministic;
 * 2. a contact that the platform simply STOPS mentioning, without ever ending
 *    it, is cancelled.
 *
 * The second one is the important one. A dropped contact is not a theoretical
 * failure — it is what a window losing its gesture, an interrupted event stream
 * or a host bug looks like from in here, and the consequence is a control held
 * down forever with no contact left to release it. Noticing it costs one set
 * difference per event.
 *
 * CONTRACT: each batch must describe EVERY contact the platform currently knows
 * about, not only the one that changed. An adapter whose platform reports a
 * single changed contact per event must accumulate before calling this, or the
 * reconciliation below will cancel the contacts it did not mention.
 */
class TouchContactTracker(private val engine: TouchControlEngine) {

    private val active = mutableSetOf<Long>()

    /** Contacts currently believed to be down. Diagnostics only. */
    val activeCount: Int get() = active.size

    fun dispatch(batch: List<TouchContact>) {
        batch.forEach { contact ->
            when (contact.phase) {
                TouchPhase.Down, TouchPhase.Move -> active += contact.id
                TouchPhase.Up, TouchPhase.Cancel -> active -= contact.id
            }
            engine.onContact(contact)
        }

        if (active.isEmpty()) return
        val present = batch.mapTo(mutableSetOf()) { it.id }
        val vanished = active.filterNot { it in present }
        if (vanished.isEmpty()) return
        active.removeAll(vanished.toSet())
        vanished.forEach { id ->
            engine.onContact(TouchContact(id = id, phase = TouchPhase.Cancel, x = 0f, y = 0f))
        }
    }

    /**
     * Forget every contact and release the engine.
     *
     * The adapter's boundary call: disposal, host inactivity, a caught fault. The
     * reason is carried through so the diagnostic says which boundary fired.
     */
    fun releaseAll(reason: TouchReleaseReason) {
        active.clear()
        engine.releaseAll(reason)
    }
}
