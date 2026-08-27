# Touch analog trigger: why a downward pull cost the whole screen

**Date:** 2026-08-26
**Status:** Resolved
**Confidence:** Confirmed (reproduced and re-measured on hardware)

## Question

Feel testing reported that a predominantly vertical analog-trigger stroke needed
"almost an entire screen-height swipe" to reach 100%, while horizontal and diagonal strokes felt
right. The geometry in the build under test was supposed to make a vertical pull cost a quarter of
the usable height. Either the runtime was not doing what the unit tests said, or the intended
geometry was itself wrong.

The first hypothesis was a coordinate-space defect — a rotation, density, inset or
normalized-coordinate transform sitting between Compose pointer positions and the resolved layout,
which would plausibly show up only on a landscape tablet.

## Method

`TouchAnalogTriggerState.onDown`/`onMove` were temporarily instrumented to print the region, the
control centre, the frozen axis, both travel budgets, `fullTravelPx`, the pointer coordinates and
the resulting value. The debug layout lab was launched straight into the on-screen controller with
the GameCube personality and driven with injected pointer events:

```
adb shell am start -n dev.picoswitch.companion.debug/dev.picoswitch.companion.lab.LayoutLabActivity \
    --ez touch true --es personality gc
adb shell input swipe 384 163 384 700 900       # straight DOWN from the shipped L
```

`System.out` reaches logcat with `setprop log.redirect-stdio true`.

## Environment

| | |
|---|---|
| Device | AYN Odin 2 Mini (`kalama`), landscape |
| Panel | 1080x1920 physical, 1920x1080 rotated, app area 1920x1025 |
| Density | 369 dpi, `density.density` = 2.30625 |
| Build | `app-debug.apk` 2026-08-26 12:14 (the build under test), then the fix |
| Layout | shipped GameCube template, `trigger-l` unmoved |

The 12:14 APK was confirmed to contain the code under test by scanning its dex for
`verticalTravelRatio`, `armLatchSelection` and `latchSelecting` — a stale build was ruled out
before any measurement was interpreted.

## Results

Captured at pointer-down on the shipped `L`:

```
region=[0,55,1920,1080]  w=1920  h=1025  unitScale=2.30625
centre=(384.0, 162.625)  contact=(384.0, 163.0)
axis=(0.8181, 0.5751)    hRef=512.5  vRef=256.25  fullTravelPx=566.64
```

**The coordinate pipeline is clean.** The injected point `(384, 163)` arrived as
`contact=(384.0, 163.0)`: pointer positions are in exactly the pixel space the region is built in.
No rotation, density, inset or normalized-coordinate term sits between them. The region matches the
window's app area minus its insets. The original hypothesis is disproven.

**The intended geometry was the defect.** With the weighted-blend rule then shipping —
`fullTravel = |axis.x| * Rx + |axis.y| * Ry` — a stroke straight DOWN needs

```
dy = fullTravel / |axis.y| = Rx * tan(tilt from vertical) + Ry
   = 512.5 * tan(54.9 deg) + 256.25
   = 729 + 256 = 985 px   of a 1025 px usable height   (96.1%)
```

which reproduces the reported symptom numerically. Two terms compound:

1. `fullTravel` is inflated by the **horizontal** budget in proportion to how much of the AXIS lies
   along X — but the finger never moved in X, so it paid 729 px for width it did not spend;
2. projection recovers only `|axis.y|` = 0.575 of a downward stroke.

**The axis was drifting with the handset.** It is derived from the pixel vector to the region
centre, so a wider window puts the centre further right and tilts the pull:

| window | axis for the shipped `L` | tilt from vertical |
|---|---|---|
| 1920x1025 handheld | (0.818, 0.575) | 55 deg |
| 2560x1600 tablet | (0.772, 0.635) | 51 deg |
| 2048x1536 tablet | (0.712, 0.703) | 45 deg |

The same authored control produced a gesture ten degrees apart between devices. A genuinely
pure-vertical placement was already correct at 25% of the height throughout — the placements that
hurt are the diagonal ones, which includes the shipped default.

## Interpretation

Two independent defects, both in the intended geometry rather than in the runtime:

- the travel reference charged a budget for motion that did not occur;
- the pull direction was a property of the window rather than of the layout, and on wide windows it
  pointed well away from where a thumb pulls a top-placed trigger.

## Change

- `inwardAxis` takes the direction in the layout's **normalized** space (`dx/width`, `dy/height`).
  The layout is authored in normalized anchors, so "toward the middle of the playable rectangle" is
  a layout statement; the result is identical on every window shape and leans down for top-placed
  controls.
- `fullTravelPx` becomes `min(Rx / |axis.x|, Ry / |axis.y|)` — two budgets, the pull ends when it
  spends either. A full pull can now never displace the finger more than `Ry` vertically or `Rx`
  horizontally, whatever the axis. Pure horizontal stays exactly `Rx` and pure vertical exactly
  `Ry`.

## Re-measurement

Same device, same placement, same injected downward stroke, after the change:

```
axis=(0.6048, 0.7964)   fullTravelPx=321.78
value reaches 1.0 at dy = 404 px   of 1025   (39.4%)
```

| | before | after |
|---|---|---|
| downward stroke to full travel | 985 px (96.1% of height) | 404 px (39.4%) |
| vertical component of an along-axis pull | 326 px (31.8%) | 256 px (25.0%, = `Ry`) |
| pure horizontal placement | 512.5 px | 512.5 px (unchanged) |
| pure vertical placement | 256.25 px | 256.25 px (unchanged) |

The visible fill follows the same axis, so the shipped `L` now fills downward instead of across,
which is the direction the gesture actually wants; confirmed by screenshot at a half pull.

## Negative knowledge

Two travel rules are disproven and should not be reintroduced:

- **One shared distance for every direction** (`min(width, height) * travelFraction`). Attractive
  because it is isotropic and reachable from any placement. Rejected by feel testing: the same
  pixels are a quarter of a landscape screen's width but half of its height.
- **A weighted blend of two references** (`|axis.x| * Rx + |axis.y| * Ry`). Attractive because it
  is continuous, preserves both pure cases exactly and keeps diagonals within 12% of the shared
  distance. Rejected by the measurement above: weighting by the AXIS charges a budget the STROKE
  may never spend, and the vertical cost grows without bound as the axis tilts.

Also disproven: that a coordinate-space transform (rotation, density, insets, normalized layout
coordinates) was involved. It was not, on this device, and the capture above is the evidence.

## Remaining unknowns

- Not yet re-measured on the reporter's own landscape tablet; the change is modelled there
  (2560x1600 and 2048x1536 both land on 39.4% for a downward stroke, and the regression test pins a
  quarter of the usable height for a near-vertical placement) but only the handheld was driven.
- Whether `verticalTravelRatio = 0.50` is the right feel now that the vertical cost is bounded is a
  gameplay judgement, not a geometric one. It is one named constant.
