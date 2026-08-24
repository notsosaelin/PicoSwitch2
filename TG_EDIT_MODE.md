# TG_EDIT_MODE - Touch Gamepad Editing Mode & Profile System Design

## Autonomous Implementation Task

Read this document and implement the Touch Gamepad editing overhaul
autonomously.

The current controller personality layouts are considered complete and
correct:

-   Pro Controller 2
-   GameCube
-   Joy-Con 2 Left
-   Joy-Con 2 Right

Do not redesign default layouts during this task.

This pass is specifically for improving customization UX, editing
workflow, persistence, and profiles.

------------------------------------------------------------------------

# Important Existing Architecture

The existing `TouchLayoutOverride` sparse override architecture is the
intended foundation.

Do NOT create a separate competing layout system.

The expected architecture:

    Immutable Controller Template
            |
            v
    Named Profile
            |
            v
    Sparse Overrides
            |
            v
    Composed Runtime Layout

Factory/default layouts remain immutable.

Profiles should only store user changes.

A profile should never duplicate the entire controller template unless
required for export/import.

------------------------------------------------------------------------

# Current Problem

The current editor uses a large modal panel that covers most of the
touch gamepad.

Problems:

-   The user cannot see the layout while editing.
-   Adjusting spacing and alignment is difficult.
-   The editor interrupts the workflow.
-   The design does not scale for advanced customization.

The editor should become an in-place overlay editor.

------------------------------------------------------------------------

# Reference Projects

## Dolphin Emulator Android

https://github.com/dolphin-emu/dolphin

Use this as the primary reference.

Important concepts:

-   Dedicated edit layout mode.
-   Controls remain visible while editing.
-   Direct manipulation of controls.
-   Separate rendering/control definitions from editing behavior.
-   User-adjustable overlay controls.

------------------------------------------------------------------------

## WatermelonDS Android

https://github.com/SapphireRhodonite/WatermelonDS

Reference:

-   Touch-first editing philosophy.
-   Simple mobile customization.
-   Avoiding large blocking configuration screens.

------------------------------------------------------------------------

# New Editing Mode

## Entering Edit Mode

Normal mode:

-   Touch controls work as controller input.

Edit mode:

-   Touch controls become editable.
-   Layout remains visible.
-   Controller input is temporarily disabled.
-   User manipulates the actual controls.

Do not use a full-screen editor dialog.

------------------------------------------------------------------------

# Floating Editor Toolbar

Replace the current large editor panel.

Use a compact floating toolbar.

Default:

-   Bottom center.

Allow toolbar relocation:

-   Bottom
-   Top
-   Left
-   Right

Example:

    Profile
    Add
    Group
    Grid
    Snap
    Reset
    Done

The toolbar should provide tools without hiding the layout.

------------------------------------------------------------------------

# Control Editing

Selecting a control should show contextual options.

Example:

    Selected:
    A Button

    Position
    Size
    Visibility
    Group
    Reset

The edited control must remain visible.

------------------------------------------------------------------------

# Direct Manipulation

Support:

## Drag

Move controls directly.

## Pinch

Resize controls.

## Group Editing

Allow editing:

Individual controls:

-   A
-   B
-   X
-   Y

or groups:

    FaceDiamond
     A
     B
     X
     Y

    DPad
     Up
     Down
     Left
     Right

    Stick
     Analog
     Click

------------------------------------------------------------------------

# Alignment Tools

Add optional editing assistance:

-   Grid overlay.
-   Snap-to-grid.
-   Center horizontal guide.
-   Center vertical guide.
-   Safe edge boundaries.

These are visual aids, not restrictions.

Users should still be able to freely position controls.

------------------------------------------------------------------------

# Profile System

Profiles must be personality-specific.

Example:

    GameCube
     ├ Default
     ├ Smash
     └ Custom

    Pro Controller 2
     ├ Default
     └ FPS

Each personality has independent profile storage.

------------------------------------------------------------------------

# Profile Rules

Default profiles:

-   Always preserved.
-   Never overwritten.
-   Always restorable.

User actions:

-   Reset to Default.
-   Duplicate Default.
-   Create Custom Profile.
-   Rename profile.

------------------------------------------------------------------------

# Future Game/Application Profiles

Design profiles so future expansion is possible:

Example:

    GameCube
     ├ Default
     ├ Smash Ultimate
     └ Mario Kart

Do not implement automatic game detection yet.

------------------------------------------------------------------------

# Data Model Direction

Suggested:

    TouchProfile
    - id
    - name
    - personality
    - templateRevision
    - overrides

Override:

    ControlOverride
    - position
    - scale
    - visibility
    - group

Future compatible:

-   rotation
-   themes
-   import/export

------------------------------------------------------------------------

# Implementation Phases

## Phase 1 - Editor Foundation

Implement:

-   Overlay edit mode.
-   Floating toolbar.
-   Control selection.
-   Dragging.
-   Resize gestures.

## Phase 2 - Editing Quality

Implement:

-   Grid.
-   Snap.
-   Groups.
-   Multi-selection.
-   Alignment helpers.

## Phase 3 - Profiles

Implement:

-   Save/load profiles.
-   Duplicate profiles.
-   Reset to defaults.
-   Import/export foundation.

------------------------------------------------------------------------

# Success Criteria

The final system should feel like a modern emulator touch-control
editor:

-   The gamepad remains visible while editing.
-   Users directly manipulate controls.
-   Default layouts cannot be destroyed.
-   Profiles allow experimentation.
-   Each personality has separate customization.
-   Future per-game layouts are possible.
-   The existing template/override architecture remains the source of
    truth.

Proceed autonomously after reading this document.
