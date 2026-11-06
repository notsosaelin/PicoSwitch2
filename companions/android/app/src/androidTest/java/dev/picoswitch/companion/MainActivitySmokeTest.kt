package dev.picoswitch.companion

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import org.junit.Rule
import org.junit.Test

/**
 * Every top-level destination renders with no adapter present.
 *
 * Offline is the state a first launch is actually in, and it is the state in
 * which a screen that assumes connected data crashes. Each destination is
 * asserted through a string that only that destination shows.
 *
 * Navigation items are matched by content description rather than by label
 * text: the bottom bar drops to icon-only on narrow displays and at raised font
 * scales, so a label-based click is device-dependent while the description is
 * always present.
 *
 * Those finders read the UNMERGED tree. A navigation item merges its children's
 * semantics, and the merged node exposes the item's label rather than its icon's
 * content description — so the description is present but not visible to a
 * merged-tree finder. Matching the unmerged node keeps the intent (find the icon,
 * not the label, because the label disappears on a narrow bar) while addressing
 * the node that actually carries it.
 */
class MainActivitySmokeTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()

    @Test fun allTopLevelDestinationsRenderOffline() {
        // Adapter is the launch destination. Anchors below the first screenful
        // are scrolled to rather than asserted in place: this runs on whatever
        // window the device has, and a short landscape one legitimately shows
        // fewer cards.
        rule.onAllNodesWithText("Adapter").onFirst().assertIsDisplayed()
        rule.onNodeWithText("Controller mode").performScrollTo().assertIsDisplayed()

        rule.onNodeWithContentDescription("Keyboard", useUnmergedTree = true).performClick()
        rule.onNodeWithText("Keyboard & Mouse").assertIsDisplayed()
        rule.onNodeWithText("Adapter not connected").assertIsDisplayed()

        rule.onNodeWithContentDescription("Amiibo", useUnmergedTree = true).performClick()
        rule.onNodeWithText("0 saved").assertIsDisplayed()

        rule.onNodeWithContentDescription("Gamepad", useUnmergedTree = true).performClick()
        rule.onNodeWithText("This handheld").performScrollTo().assertIsDisplayed()

        rule.onNodeWithContentDescription("Settings", useUnmergedTree = true).performClick()
        rule.onNodeWithText("Appearance").assertIsDisplayed()
        rule.onNodeWithText("Diagnostics").performScrollTo().assertIsDisplayed()
    }

    /** Diagnostics is an overlay over Settings, and must open and close again. */
    @Test fun diagnosticsOverlayOpensAndCloses() {
        rule.onNodeWithContentDescription("Settings", useUnmergedTree = true).performClick()
        rule.onNodeWithText("Diagnostics").performScrollTo().performClick()
        // "Diagnostics" is now both the page heading and the row behind it, so
        // the assertion uses a string only the overlay has.
        rule.onNodeWithText("Identity").assertIsDisplayed()
        rule.onNodeWithContentDescription("Close diagnostics").performClick()
        // Settings keeps the scroll position it had when the overlay opened, so
        // Appearance is legitimately above the fold on a short window.
        rule.onNodeWithText("Appearance").performScrollTo().assertIsDisplayed()
    }
}
