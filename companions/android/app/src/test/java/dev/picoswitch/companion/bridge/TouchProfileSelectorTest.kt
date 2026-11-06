package dev.picoswitch.companion.bridge

import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.management.Personality
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TouchProfileSelectorTest {
    @Test
    fun `every gameplay personality selects its exact profile`() {
        assertEquals(TouchProfileId.Pro2, TouchProfileSelector.select(Personality.Pro2))
        assertEquals(TouchProfileId.GameCube, TouchProfileSelector.select(Personality.GameCube))
        assertEquals(TouchProfileId.JoyConLeft, TouchProfileSelector.select(Personality.JoyConLeft))
        assertEquals(TouchProfileId.JoyConRight, TouchProfileSelector.select(Personality.JoyConRight))
        assertEquals(
            setOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight),
            TouchProfileSelector.gameplayPersonalities,
        )
    }

    @Test
    fun `non-gameplay personalities cannot retain a touch profile`() {
        assertNull(TouchProfileSelector.select(Personality.Config))
        assertNull(TouchProfileSelector.select(Personality.Unknown))
    }
}
