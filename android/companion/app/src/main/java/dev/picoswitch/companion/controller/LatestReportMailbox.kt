package dev.picoswitch.companion.controller

import kotlinx.coroutines.channels.Channel

/** Capacity-one queue: a pending controller snapshot is always replaced by newer truth. */
internal class LatestReportMailbox<T> {
    private val channel = Channel<T>(Channel.CONFLATED)

    fun offer(value: T) = channel.trySend(value)
    suspend fun receive(): T = channel.receive()

    /**
     * Non-blocking take, or null when nothing newer has arrived. Used by the
     * time-driven send loop while motion is streaming: the report must go out on
     * the interval whether or not a button/axis event happened, so the sender
     * cannot suspend waiting for input.
     */
    fun poll(): T? = channel.tryReceive().getOrNull()
    fun drain() {
        while (channel.tryReceive().isSuccess) Unit
    }
}
