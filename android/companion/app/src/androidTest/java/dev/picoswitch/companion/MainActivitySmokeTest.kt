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
        rule.onNodeWithText("Hardware at a glance").assertIsDisplayed()
        rule.onNodeWithText("Amiibo").performClick()
        rule.onNodeWithText("Amiibo library").assertIsDisplayed()
        rule.onNodeWithText("Input").performClick()
        rule.onNodeWithText("Android controller").assertIsDisplayed()
        rule.onNodeWithText("Adapter").performClick()
        rule.onNodeWithText("Adapter & modes").assertIsDisplayed()
        rule.onNodeWithText("More").performClick()
        rule.onNodeWithText("Settings & information").assertIsDisplayed()
        rule.onNodeWithText("Developer / diagnostics").performScrollTo().assertIsDisplayed()
    }
}
