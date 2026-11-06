package dev.picoswitch.bridge.core

import org.junit.Assert.*
import org.junit.Test

/**
 * The exclusion rule must hide non-actionable virtual/mapping inputs without ever
 * hiding legitimate unusual hardware. The audited handhelds are the reference
 * cases: their built-in controllers are backed by virtual kernel nodes and report
 * `isExternal = true`, yet are entirely real.
 */
class ControllerCandidateTest {
    private fun real(
        name: String,
        vid: Int,
        pid: Int,
        axes: Boolean = true,
        buttons: Boolean = true,
        virtual: Boolean = false,
        gamepadSource: Boolean = true,
        descriptor: String = name,
    ) = ControllerCandidate(
        id = name.hashCode(), descriptor = descriptor, name = name,
        vendorId = vid, productId = pid,
        hasMotionAxes = axes, hasGamepadButtons = buttons, isVirtual = virtual,
        hasGamepadSource = gamepadSource,
    )

    private val odin = real("Odin Controller", 0x2020, 0x0111)
    private val retroid = real("Retroid Pocket Controller", 0x2022, 0x3001)

    /**
     * Android's virtual keyboard as it actually appears: id -1, name "Virtual",
     * VID/PID 0000:0000, no axes or gamepad buttons, and reaching the list only
     * via SOURCE_DPAD. Confirmed present on a phone with no controller at all.
     */
    private val virtualMapper = real(
        "Virtual", 0, 0, axes = false, buttons = false,
        virtual = true, gamepadSource = false,
    )

    @Test fun `audited handheld controllers are always usable`() {
        assertTrue(odin.isUsable)
        assertTrue(retroid.isUsable)
        assertNull(odin.exclusionReason)
    }

    @Test fun `the android virtual keyboard entry is excluded with a stated reason`() {
        assertFalse(virtualMapper.isUsable)
        assertNotNull(virtualMapper.exclusionReason)
    }

    @Test fun `a dpad-only source is not a controller even without the virtual flag`() {
        // Independent of isVirtual: an OEM may not set it, but SOURCE_DPAD alone
        // still describes a keyboard-like device, never a gamepad.
        val dpadOnly = real("Virtual", 0, 0, axes = false, buttons = false, gamepadSource = false)
        assertFalse(dpadOnly.isUsable)
        assertTrue(dpadOnly.exclusionReason!!.contains("D-pad"))
    }

    @Test fun `anonymous capability-less device is excluded with a stated reason`() {
        val anonymous = real("Unknown", 0, 0, axes = false, buttons = false)
        assertFalse(anonymous.isUsable)
        assertTrue(anonymous.exclusionReason!!.contains("VID/PID"))
    }

    @Test fun `a real VID or PID is never hidden however odd its capabilities`() {
        // Legitimate unusual hardware: identity but no axes/buttons reported yet.
        val odd = real("Odd Pad", 0x1234, 0x0000, axes = false, buttons = false)
        assertTrue(odd.isUsable)
        val pidOnly = real("Pid Only", 0x0000, 0x5678, axes = false, buttons = false)
        assertTrue(pidOnly.isUsable)
    }

    @Test fun `an anonymous device that really reports controls is kept`() {
        // Some built-in controllers expose no VID/PID but are genuinely usable.
        assertTrue(real("Builtin", 0, 0, axes = true, buttons = false).isUsable)
        assertTrue(real("Builtin", 0, 0, axes = false, buttons = true).isUsable)
    }

    @Test fun `android virtual devices are excluded even with identity`() {
        val v = real("Virtual Keyboard", 0x1111, 0x2222, virtual = true)
        assertFalse(v.isUsable)
        assertTrue(v.exclusionReason!!.contains("virtual"))
    }

    @Test fun `the only real controller is selected without asking`() {
        val list = listOf(virtualMapper, odin)
        assertEquals(odin, ControllerCandidates.autoSelect(list))
        assertFalse(ControllerCandidates.needsUserChoice(list))
        assertEquals(listOf(odin), ControllerCandidates.usable(list))
        assertEquals(listOf(virtualMapper), ControllerCandidates.excluded(list))
    }

    @Test fun `two real controllers require an explicit choice`() {
        val list = listOf(odin, retroid)
        assertNull(ControllerCandidates.autoSelect(list))
        assertTrue(ControllerCandidates.needsUserChoice(list))
    }

    @Test fun `nothing usable selects nothing and asks nothing`() {
        val list = listOf(virtualMapper)
        assertNull(ControllerCandidates.autoSelect(list))
        assertFalse(ControllerCandidates.needsUserChoice(list))
    }

    @Test fun `an existing valid choice survives a refresh`() {
        val list = listOf(odin, retroid)
        assertEquals(retroid, ControllerCandidates.resolveSelection(list, retroid.descriptor))
    }

    @Test fun `a choice that disappeared falls back to the only remaining controller`() {
        // Controller-mode switch: the same physical controls re-enumerate under a
        // different name/identity, so the saved descriptor no longer exists.
        val afterModeSwitch = listOf(virtualMapper, real("Xbox Gamepad", 0x045E, 0x02E0))
        val resolved = ControllerCandidates.resolveSelection(afterModeSwitch, odin.descriptor)
        assertEquals("Xbox Gamepad", resolved?.name)
    }

    @Test fun `a disappeared choice with several alternatives does not guess`() {
        assertNull(ControllerCandidates.resolveSelection(listOf(odin, retroid), "gone"))
    }

    @Test fun `an excluded descriptor is never resolved back into use`() {
        assertNull(
            ControllerCandidates.resolveSelection(listOf(virtualMapper), virtualMapper.descriptor),
        )
    }
}
