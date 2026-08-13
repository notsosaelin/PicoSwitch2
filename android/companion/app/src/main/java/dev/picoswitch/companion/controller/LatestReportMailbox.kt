package dev.picoswitch.companion.controller

import kotlinx.coroutines.channels.Channel

/** Capacity-one queue: a pending controller snapshot is always replaced by newer truth. */
internal class LatestReportMailbox<T> {
    private val channel = Channel<T>(Channel.CONFLATED)

    fun offer(value: T) = channel.trySend(value)
    suspend fun receive(): T = channel.receive()
    fun drain() {
        while (channel.tryReceive().isSuccess) Unit
    }
}
