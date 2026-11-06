package dev.picoswitch.companion.bridge

import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.management.Personality

/** Confirmed management personality -> exactly matching touch profile. */
object TouchProfileSelector {
    val gameplayPersonalities = setOf(
        Personality.Pro2,
        Personality.GameCube,
        Personality.JoyConLeft,
        Personality.JoyConRight,
    )

    fun select(personality: Personality): TouchProfileId? = when (personality) {
        Personality.Pro2 -> TouchProfileId.Pro2
        Personality.GameCube -> TouchProfileId.GameCube
        Personality.JoyConLeft -> TouchProfileId.JoyConLeft
        Personality.JoyConRight -> TouchProfileId.JoyConRight
        Personality.Config, Personality.Unknown -> null
    }
}
