package dev.picoswitch.companion.transport

/** Pure timing policy for the adapter's one-slot wireless request/reply carrier. */
object ManagementTurnaroundPolicy {
    const val MIN_MILLIS = 100L

    fun delayMillis(nowMillis: Long, lastReplyMillis: Long): Long {
        if (lastReplyMillis <= 0L || nowMillis < lastReplyMillis) return 0L
        return (MIN_MILLIS - (nowMillis - lastReplyMillis)).coerceAtLeast(0L)
    }
}
