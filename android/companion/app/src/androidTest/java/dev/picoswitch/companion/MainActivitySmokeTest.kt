package dev.picoswitch.companion

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import org.junit.Rule
import org.junit.Test

class MainActivitySmokeTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()

    @Test fun allTopLevelDestinationsRenderOffline() {
        rule.onNodeWithText("PicoSwitch2").assertIsDisplayed()
        rule.onNodeWithText("Amiibo").performClick()
        rule.onNodeWithText("0 saved").assertIsDisplayed()
        rule.onNodeWithText("Input").performClick()
        rule.onNodeWithText("Keep this screen open while playing").assertIsDisplayed()
        rule.onNodeWithText("Adapter").performClick()
        rule.onNodeWithText("Output personality").assertIsDisplayed()
        rule.onNodeWithText("Settings").performClick()
        rule.onNodeWithText("Appearance").assertIsDisplayed()
        rule.onNodeWithText("Developer").performScrollTo().assertIsDisplayed()
    }
}
