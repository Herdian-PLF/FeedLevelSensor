# ADR-003: Sensor placement parametrization

**Date:** 2026-08-10
**Status:** Accepted; reference orientation superseded by [ADR-004](004-world-aligned-euler-reference.md)

## Context

The simulator originally placed the sensor with a position and a point to aim at, which was
enough while every scene had it on the silo axis pointing straight down. Real installations are
neither: the sensor goes wherever the roof hatch is, off the axis, and tilted.

A placement is six degrees of freedom, but there is more than one way to write them down, and
the choice is an interface decision rather than a mathematical one. Aiming at a target point
suits "study what happens if it looks here"; mounting angles suit an installation, where the
measurable quantities are position and tilt, not an aim point. Neither is derivable from the
other without work on the reader's part.

Euler angles also need a stated reference orientation. "Aligned with the world frame" is
underdefined for a camera, whose optical axis points down while world +Z points up. Worse, if
the reference is chosen so that a vertical install sits at a pole of the angle convention, the
nominal case is exactly the degenerate one.

## Decision

`sensor.placement` selects the form, in the same spirit as `feed.type` and `silo.type`:

- `euler` (default) - `position_mm` plus `yaw_deg`, `pitch_deg`, `roll_deg`.
- `look_at` - `position_mm` plus `target_mm`, and `roll_deg`.

Both accept roll and both produce a 4x4 `world_T_cam`, so nothing downstream distinguishes them.
Height is always an absolute world Z; no separate datum is introduced.

The reference orientation is **nadir**: at yaw = pitch = roll = 0 the optical axis points
straight down and the zone grid's +X runs along world +X. The rotation is
`Rz(yaw) . Ry(-pitch) . NADIR . Rz(roll)`, which puts the optical axis at

    (sin(pitch)cos(yaw), sin(pitch)sin(yaw), -cos(pitch))

so pitch is the angle away from vertical and yaw is the direction of lean, CCW from +X. Taking
nadir as the reference means a vertical installation is at yaw = pitch = roll = 0 rather than at
a singularity, and small misalignments read as small angles.

## Consequences

The parametrization matches what is measurable at an installation, and the closed form for the
optical axis makes the convention directly testable rather than a matter of convention folklore.

Near vertical, yaw and roll turn about nearly the same axis: at pitch = 0 only their difference
is observable, so many (yaw, roll) pairs describe one orientation. Every triple still maps to
exactly one pose, but angles recovered from a pose are not guaranteed to be the ones it was
built from. This was accepted knowingly; a tilt-magnitude-plus-azimuth form would avoid it at
the cost of being less familiar.

Placement now measurably dominates other error sources. On the reference hopper scene, moving
the sensor 700 mm off the axis while keeping it vertical takes volume error from -0.08 % to
+1.53 %, and tilting it 12 degrees back towards the centre from that same mount recovers it to
+0.40 % - so tilt can compensate offset, and the simulator is what says by how much.

Roll has little effect on a rotationally symmetric fill, as expected; it changes which part of
the silo the non-square field of view covers, which matters for asymmetric surfaces and edge
coverage rather than for the symmetric test scenes.
