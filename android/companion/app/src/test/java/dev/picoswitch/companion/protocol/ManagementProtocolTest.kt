package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.Personality
import org.junit.Assert.*
import org.junit.Test

class ManagementProtocolTest {
    @Test fun `frames commands and splits at minimum ATT payload`() {
        val chunks = ManagementProtocol.chunks("amiibo status", 20)
        assertEquals(1, chunks.size)
        assertArrayEquals("amiibo status\n".encodeToByteArray(), chunks.single())
    }

    @Test fun `rejects multiline and overlong commands`() {
        assertThrows(IllegalArgumentException::class.java) { ManagementProtocol.frame("ping\nget") }
        assertEquals(128, ManagementProtocol.frame("x".repeat(127)).size)
        assertThrows(IllegalArgumentException::class.java) { ManagementProtocol.frame("x".repeat(128)) }
    }

    @Test fun `accepts exact wireless reply limit and rejects the next byte`() {
        ManagementProtocol.requireReplyWithinLimit(511)
        assertThrows(ManagementReplyTooLargeException::class.java) {
            ManagementProtocol.requireReplyWithinLimit(512)
        }
    }

    @Test fun `parses versioned complete bond page`() {
        val page = ManagementProtocol.bondsPage(
            ManagementProtocol.objectOrThrow(
                "bonds list",
                """{"v":2,"total":2,"bonds":[{"i":0,"type":1,"addr":"010203040506"},{"i":4,"type":0,"addr":"AABBCCDDEEFF"}],"next":null}""",
            ),
        )
        assertEquals(2, page.total)
        assertEquals(2, page.entries.size)
        assertNull(page.next)
        assertEquals(4, page.entries[1].index)
    }

    @Test fun `rejects unversioned or inconsistent bond pages`() {
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.bondsPage(
                ManagementProtocol.objectOrThrow("bonds list", """{"bonds":[]}"""),
            )
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.bondsPage(
                ManagementProtocol.objectOrThrow(
                    "bonds list v2",
                    """{"v":2,"total":0,"bonds":[{"i":1,"addr":"00"}],"next":null}""",
                ),
            )
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.bondsPage(
                ManagementProtocol.objectOrThrow(
                    "bonds list v2",
                    """{"v":1,"total":0,"bonds":[],"next":null}""",
                ),
            )
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.bondsPage(
                ManagementProtocol.objectOrThrow(
                    "bonds list v2",
                    """{"v":2,"total":0,"bonds":[],"next":"nope"}""",
                ),
            )
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.bondsPage(
                ManagementProtocol.objectOrThrow(
                    "bonds list v2",
                    """{"v":2,"total":1,"bonds":[],"next":4}""",
                ),
            )
        }
    }

    @Test fun `parses current config and compatibility aliases independently`() {
        val json = ManagementProtocol.objectOrThrow("get", """{"body_color":[1,2,3],"joycon2_left_accent":[4,5,6],"joycon2_right_accent":[7,8,9],"lightbar":[[99,99,99]]}""")
        val value = ManagementProtocol.config(json)
        assertEquals(1, value.bodyColor.red)
        assertEquals(5, value.leftAccent.green)
        assertEquals(9, value.rightAccent.blue)
    }

    @Test fun `parses full amiibo status including v3 and figure identity`() {
        val json = ManagementProtocol.objectOrThrow("amiibo status", """{"loaded":false,"dirty":true,"presented":true,"v3loaded":true,"persisted":false,"persistPending":true,"size":2048,"signature":true,"hasSave2":false,"usingSave2":false,"generation":42,"payloadCrc":"A1B2C3D4","uid":"04112233445566","figureId":"0000000000000003","upload":{"active":false,"received":0,"size":0}}""")
        val value = ManagementProtocol.amiibo(json)
        assertTrue(value.v3Loaded)
        assertTrue(value.dirty)
        assertEquals(2048, value.size)
        assertEquals(42, value.generation)
        assertEquals("0000000000000003", value.figureId)
    }

    @Test fun `turns adapter errors into typed command errors`() {
        val error = assertThrows(AdapterCommandException::class.java) {
            ManagementProtocol.objectOrThrow("amiibo commit", """{"error":"dirty","code":8}""")
        }
        assertEquals(8, error.code)
        assertEquals("amiibo commit", error.command)
    }

    @Test fun `parses personality set and read shape`() {
        val value = ManagementProtocol.personality(ManagementProtocol.objectOrThrow("personality", """{"current":"pro2","available":["pro2","gc","jcl","jcr"]}"""))
        assertEquals(Personality.Pro2, value.current)
        assertEquals(4, value.available.size)
    }

    @Test fun `rejects incomplete success shapes and invalid read hex`() {
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.firmware(ManagementProtocol.objectOrThrow("info", "{}"))
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.requireOk("wake", ManagementProtocol.objectOrThrow("wake", "{}"))
        }
        assertThrows(ManagementException::class.java) {
            ManagementProtocol.readData(ManagementProtocol.objectOrThrow("amiibo read", """{"data":"GG"}"""))
        }
    }

    @Test fun `empty firmware device identity is detached`() {
        val value = ManagementProtocol.controller(
            ManagementProtocol.objectOrThrow(
                "device",
                """{"name":"","vid":0,"pid":0,"batteryValid":0,"battery":0,"charging":0}""",
            ),
        )
        assertEquals("No controller", value.name)
        assertFalse(value.attached)
    }
}
