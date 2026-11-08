package dev.picoswitch.companion.ui

import dev.picoswitch.companion.ui.BackKeyGesture.Result
import org.junit.Assert.assertEquals
import org.junit.Test

class BackKeyGestureTest {
    @Test
    fun `a short press is a tap`() {
        val back = BackKeyGesture(holdMs = 800)
        back.down(1_000, 0)
        assertEquals(Result.None, back.tick(1_300, holdAllowed = true))
        assertEquals(Result.Tap, back.up())
    }

    @Test
    fun `holding while streaming releases once, and the release that follows does nothing`() {
        val back = BackKeyGesture(holdMs = 800)
        back.down(1_000, 0)
        assertEquals(Result.Hold, back.tick(1_800, holdAllowed = true))
        // Key repeats keep arriving while the key stays down.
        back.down(1_850, 3)
        assertEquals(Result.None, back.tick(1_900, holdAllowed = true))
        assertEquals(Result.None, back.up())
    }

    @Test
    fun `holding while not streaming is just a tap`() {
        val back = BackKeyGesture(holdMs = 800)
        back.down(1_000, 0)
        assertEquals(Result.None, back.tick(2_500, holdAllowed = false))
        assertEquals(Result.Tap, back.up())
    }

    @Test
    fun `key repeats do not restart the hold`() {
        val back = BackKeyGesture(holdMs = 800)
        back.down(1_000, 0)
        back.down(1_500, 1)
        assertEquals(Result.Hold, back.tick(1_800, holdAllowed = true))
    }

    @Test
    fun `a cancelled press and a stray release do nothing`() {
        val back = BackKeyGesture(holdMs = 800)
        back.down(1_000, 0)
        back.cancel()
        assertEquals(Result.None, back.up())
        assertEquals(Result.None, back.tick(5_000, holdAllowed = true))
    }
}
