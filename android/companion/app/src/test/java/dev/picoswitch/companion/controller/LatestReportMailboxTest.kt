package dev.picoswitch.companion.controller

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Test

class LatestReportMailboxTest {
    @Test fun `newest pending state replaces stale history`() = runBlocking {
        val mailbox = LatestReportMailbox<Int>()
        mailbox.offer(1)
        mailbox.offer(2)
        mailbox.offer(3)
        assertEquals(3, mailbox.receive())
    }

    @Test fun `drain removes state from a previous link`() = runBlocking {
        val mailbox = LatestReportMailbox<Int>()
        mailbox.offer(1)
        mailbox.drain()
        mailbox.offer(2)
        assertEquals(2, mailbox.receive())
    }
}
