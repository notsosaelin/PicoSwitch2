package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.KbmDestination
import dev.picoswitch.companion.model.KbmMode
import dev.picoswitch.companion.model.KbmProfile
import dev.picoswitch.companion.model.KbmSource
import dev.picoswitch.companion.model.KbmSourceKind
import dev.picoswitch.companion.model.description
import dev.picoswitch.companion.model.kbmDestinationGroups
import dev.picoswitch.companion.model.title
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Presentation aliases remain thin while wire semantics live in management-core. */
class KbmProtocolTest {
    @Test fun `presentation names decorate core wire identifiers`() {
        assertEquals("kbmouse", KbmMode.KeyboardMouse.wire)
        assertEquals("Keyboard + Mouse", KbmMode.KeyboardMouse.title)
        assertTrue(KbmMode.KeyboardMouse.description.contains("Mouse"))
        assertEquals("kbm", KbmProfile.KeyboardMouse.wire)
    }

    @Test fun `source and destination identifiers are the core values`() {
        assertEquals("key:1A", KbmSource(KbmSourceKind.Key, 0x1A).wire)
        assertEquals(KbmDestination.LStickUp, KbmDestination.fromWire("lstick_up"))
        assertTrue(kbmDestinationGroups.flatMap { it.second }.contains(KbmDestination.C))
    }
}
