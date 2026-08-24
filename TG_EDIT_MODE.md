# TG_EDIT_MODE.md

# Touch Gamepad Editor Architecture & Profile System High-Level Design Specification

## Document Purpose

This document defines the architectural direction for the next Touch
Gamepad customization pass.

The objective is not a minor UI adjustment. This is a redesign of the
editing workflow into a production-quality touch layout editor.

The existing controller personalities and default layouts are considered
complete:

-   Pro Controller 2
-   GameCube
-   Joy-Con 2 Left
-   Joy-Con 2 Right

This project phase focuses exclusively on:

-   Editing workflow
-   User customization
-   Profile architecture
-   Persistence
-   UX consistency
-   Future extensibility

------------------------------------------------------------------------

# Architectural Constraint

The existing layout architecture is the foundation.

The implementation MUST continue using:

Template -\> Profile -\> Sparse Overrides -\> Runtime Composition

The system MUST NOT create a second independent layout representation.

## Required behavior

Default templates:

-   Are immutable.
-   Represent official/default controller layouts.
-   Can always be restored.

Profiles:

-   Store user customization only.
-   Reference a template revision.
-   Contain sparse overrides.
-   Never replace the source template.

Runtime:

    Controller Personality
            |
            v
    Default Template
            |
            v
    Selected Profile Overrides
            |
            v
    Resolved Touch Layout

The existing TouchLayoutOverride approach should be extended, not
replaced.

------------------------------------------------------------------------

# UX Research Basis

This design follows established patterns from mature emulator
touch-control systems.

Dolphin Emulator's Android overlay system is the primary reference
because it treats controller overlays as editable objects rather than a
separate settings screen. Relevant concepts include:

-   Dedicated edit mode.
-   Visible controls during editing.
-   Direct manipulation.
-   Separation between drawable controls and editing behavior.

WatermelonDS and related Android emulator projects similarly prioritize
keeping the touch surface visible while allowing adjustment.

Community feedback around Android emulator overlays frequently
identifies grid/snap alignment and direct manipulation as important
usability improvements. citeturn0search5

------------------------------------------------------------------------

# Current UX Problems

The current editor implementation is not suitable for a complex
controller layout system.

Problems:

## Screen obstruction

The editor panel covers the majority of the touch surface.

Result:

-   Users cannot evaluate spacing.
-   Users cannot compare alignment.
-   Users cannot see final composition.

## Poor spatial editing

Layout editing is a spatial task.

A modal settings panel forces users to mentally map:

Editor controls -\> Hidden layout

A proper editor should allow:

User sees control -\> User moves control -\> User immediately
understands result

------------------------------------------------------------------------

# Proposed Editing Mode

## Mode switching

Normal mode:

-   Touch controls behave as controller input.

Edit mode:

-   Touch controls become editable objects.
-   Input forwarding is paused.
-   Layout remains rendered.
-   User directly manipulates controls.

------------------------------------------------------------------------

# Editor Interface

## Floating Editor Toolbar

Replace the large modal editor.

The toolbar should be a small floating component.

Default placement:

-   Bottom center

User configurable:

-   Bottom
-   Top
-   Left
-   Right

Reasoning:

Users have different device orientations, aspect ratios, and hand
positions.

------------------------------------------------------------------------

## Toolbar Functions

Primary actions:

    Profile
    Add Control
    Group
    Grid
    Snap
    Reset
    Save
    Exit

The toolbar provides tools without becoming the primary visual element.

------------------------------------------------------------------------

# Direct Manipulation System

## Selection

Tap a control:

-   Select object.
-   Show bounding indicator.
-   Open contextual actions.

## Movement

Drag:

-   Move selected control.

## Scaling

Pinch:

-   Resize selected control.

## Multi-selection

Support editing:

Individual:

-   A
-   B
-   X
-   Y

Groups:

-   Face diamond
-   D-pad
-   Analog stick assembly

------------------------------------------------------------------------

# Group Architecture

Groups are logical editing units.

Examples:

    FaceDiamond
     ├ A
     ├ B
     ├ X
     └ Y

    DPad
     ├ Up
     ├ Down
     ├ Left
     └ Right

    LeftStick
     ├ Stick
     └ Click

A group does not change input behavior.

It only affects editing operations.

------------------------------------------------------------------------

# Alignment Assistance

The editor should provide optional visual assistance.

Features:

## Grid

Optional grid overlay.

## Snap

Optional snapping.

## Guides

Display:

-   Horizontal center line.
-   Vertical center line.
-   Equal spacing guides.
-   Safe edge zones.

Important:

Guides assist users.

They do not restrict placement.

------------------------------------------------------------------------

# Profile System

Profiles must be personality-specific.

Example:

    GameCube

    Default
    Smash
    Custom

    Pro Controller 2

    Default
    FPS
    RPG

------------------------------------------------------------------------

# Default Profile Protection

Factory profiles:

-   Cannot be overwritten.
-   Cannot be deleted.
-   Always available.

User actions:

-   Reset to default.
-   Duplicate default.
-   Create custom profile.
-   Rename custom profile.

------------------------------------------------------------------------

# Future Per-Game Profiles

The architecture should support future expansion:

Example:

    GameCube

    Default
    Smash Ultimate
    Mario Kart

Automatic game detection is outside this implementation phase.

------------------------------------------------------------------------

# Persistence Model

Recommended structure:

    TouchProfile

    id
    name
    personality
    templateId
    templateRevision
    overrideDocument
    metadata

Override:

    TouchControlOverride

    position
    scale
    visibility
    group

Future compatible fields:

-   Rotation
-   Theme
-   Opacity
-   Animation behavior

------------------------------------------------------------------------

# Implementation Plan

## Phase 1: Editor Foundation

Implement:

-   Edit mode state.
-   Floating toolbar.
-   Selection.
-   Dragging.
-   Scaling.
-   Save/cancel flow.

## Phase 2: Editing Quality

Implement:

-   Grid.
-   Snap.
-   Groups.
-   Multi-select.
-   Alignment helpers.

## Phase 3: Profiles

Implement:

-   Profile creation.
-   Profile switching.
-   Profile duplication.
-   Reset.
-   Export/import foundation.

------------------------------------------------------------------------

# Validation Requirements

Before completion:

Verify:

-   Existing default layouts are unchanged.
-   Profile switching does not mutate templates.
-   Multiple personalities maintain independent profiles.
-   Editing remains usable on:
    -   16:9
    -   16:10
    -   4:3
    -   1:1 displays
-   Large layouts remain editable without toolbar obstruction.

------------------------------------------------------------------------

# Final Goal

The Touch Gamepad editor should feel comparable to a modern emulator
overlay editor:

-   Visible while editing.
-   Direct manipulation.
-   Safe defaults.
-   Powerful customization.
-   Future-proof profile architecture.

Implement this as an extension of the existing layout composition
system.
