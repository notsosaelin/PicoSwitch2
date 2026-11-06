package dev.picoswitch.companion.transport

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which commands may be sent twice.
 *
 * THE RISK THIS GUARDS. The adapter can accept a command and never answer it —
 * its bridge drops anything arriving while its single slot is busy — so a
 * timeout does NOT mean the command was not executed. Anything retried must
 * therefore be safe to execute twice, not merely safe to *send* twice.
 *
 * Getting that wrong is silent: re-sending `amiibo begin` after it was in fact
 * received would reset the staging buffer, the upload would carry on from the
 * wrong place, and the result would be a corrupt tag that still passed its own
 * CRC check. These tests exist so that stays impossible by construction.
 */
class ManagementRetryPolicyTest {

    @Test fun `chunk writes are repeatable because they carry an absolute offset`() {
        assertTrue(ManagementRetryPolicy.isRepeatable("amiibo chunk 0 00112233"))
        assertTrue(ManagementRetryPolicy.isRepeatable("amiibo chunk 288 aabbccdd"))
    }

    @Test fun `reads and status queries are repeatable because they change nothing`() {
        assertTrue(ManagementRetryPolicy.isRepeatable("amiibo read 64 32"))
        assertTrue(ManagementRetryPolicy.isRepeatable("amiibo status"))
    }

    @Test fun `staged draft writes are repeatable because they are absolute sets`() {
        // `bind` sets the entry keyed by its source; `mouse` sets one named field
        // to one value. Both act on a draft held in RAM, so a repeat leaves it
        // byte-identical, and neither touches stored or realized state.
        assertTrue(ManagementRetryPolicy.isRepeatable("kbm draft bind key:1D a"))
        assertTrue(ManagementRetryPolicy.isRepeatable("kbm draft bind mouse:1 rstick_right"))
        assertTrue(ManagementRetryPolicy.isRepeatable("kbm draft mouse sensitivityx 512"))
        assertTrue(ManagementRetryPolicy.isRepeatable("kbm draft mouse inverty 1"))
    }

    @Test fun `the draft transaction boundaries are never repeated`() {
        assertFalse(ManagementRetryPolicy.isRepeatable("kbm draft begin kb pos:1 0 Work"))
        assertFalse(ManagementRetryPolicy.isRepeatable("kbm draft commit"))
        assertFalse(ManagementRetryPolicy.isRepeatable("kbm draft abort"))
    }

    /**
     * The dangerous ones. Each of these has an effect that a second execution
     * would change, so a lost reply must end the session rather than be papered
     * over.
     */
    @Test fun `staging and publishing commands are never repeated`() {
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo begin 540 b136cf6f"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo commit"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo commit save2"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo persist"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo downloaded"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo cancel"))
    }

    /**
     * An allowlist, not a denylist: a command nobody has reasoned about is
     * unsafe until somebody does.
     */
    @Test fun `anything unrecognised is treated as unsafe`() {
        assertFalse(ManagementRetryPolicy.isRepeatable("reenumerate"))
        assertFalse(ManagementRetryPolicy.isRepeatable("save"))
        assertFalse(ManagementRetryPolicy.isRepeatable("kbm mode keyboard"))
        assertFalse(ManagementRetryPolicy.isRepeatable(""))
    }

    /**
     * "amiibo chunk" must not match by being a prefix of something else, and the
     * trailing space is what stops a future "amiibo chunkmode" from inheriting
     * an unexamined retry.
     */
    @Test fun `matching is on a whole command word`() {
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo chunkmode on"))
        assertFalse(ManagementRetryPolicy.isRepeatable("amiibo readback"))
    }

    @Test fun `matching tolerates case and surrounding space`() {
        assertTrue(ManagementRetryPolicy.isRepeatable("  AMIIBO CHUNK 0 00  "))
    }

    /** Bounded: a genuinely unreachable adapter must still surface as a failure. */
    @Test fun `retries are bounded`() {
        assertTrue(ManagementRetryPolicy.MAX_RETRIES in 1..3)
    }
}
