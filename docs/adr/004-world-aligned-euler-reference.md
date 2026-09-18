# ADR-004: World-aligned Euler reference

**Date:** 2026-08-14
**Status:** Accepted
**Supersedes:** the reference-orientation part of [ADR-003](003-sensor-placement-parametrization.md)

## Context

ADR-003 chose **nadir** as the Euler reference: at yaw = pitch = roll = 0 the optical axis pointed
straight down. The argument was ergonomic - a vertical installation is the nominal case, so it
should read as zero, and a small mounting error should read as a small angle.

In use the choice proved to be a trap rather than a convenience. `0, 0, 0` is indistinguishable
from an unset field, so a config with no `sensor` block silently produced a valid downward-looking
sensor. The 180 degree flip between world and camera axes lived inside a `NADIR` constant rather
than in the numbers, so anyone reading the angles could not see it. Worst of all, `pitch: 180` -
what the flip *looks* like it should be - pointed the sensor at the roof and still ran.

Reasoning about the convention was repeatedly harder than reasoning about the geometry.

## Decision

The reference orientation is the **world frame**: at yaw = pitch = roll = 0 the camera axes
coincide with the world axes, so the optical axis (camera +Z) points straight **up**. The rotation
is a ZYZ Euler composition

    Rz(yaw) . Ry(pitch) . Rz(roll)

which puts the optical axis at

    (sin(pitch)cos(yaw), sin(pitch)sin(yaw), cos(pitch))

These are ordinary spherical coordinates: **pitch is the polar angle from world +Z** and **yaw the
azimuth the axis leans towards**, CCW from +X. A roof-mounted sensor hangs at pitch = 180. An
inclinometer reading of tilt-from-vertical `t` becomes `pitch = 180 - t`.

`SensorConfig.pitch_deg` defaults to 180 rather than 0, so an unconfigured sensor still looks down;
`from_euler` keeps a 0 default, because as a rotation constructor its neutral element is the
identity. The `NADIR` constant is deleted - the reference is now the identity and needs no name.

Everything else in ADR-003 stands: `sensor.placement` still selects `euler` or `look_at`, both
still produce a 4x4 `world_T_cam`, and height is still an absolute world Z.

## Consequences

The datum is now visible in the numbers instead of hidden in a matrix, and `pitch: 180` means what
it looks like. `0, 0, 0` is no longer a plausible-looking accident: it aims at the roof, which
shows up immediately as an empty or nonsensical frame rather than as a subtly wrong one.

The cost is that the nominal installation is no longer zero. A 3 degree mounting error reads as
`pitch: 177` or `183` depending on which way it leans, which is exactly the ergonomic objection
ADR-003 raised. That objection was real; it was simply outweighed by the ambiguity.

Pitching 180 about +Y turns the zone grid's +X to world -X, so `yaw: 180, pitch: 180` - not
`pitch: 180` alone - reproduces the old nadir datum with the grid's +X along world +X. On a
rotationally symmetric fill this is invisible, since a 180 degree roll maps the rectangular field
of view onto itself; it matters only for asymmetric surfaces. `configs/offset_tilted.yaml` moved
from `yaw 180, pitch 12` to `yaw 180, pitch 168` and reproduces its previous pose and metrics to
floating-point noise (volume error +0.396 % before and after).

The yaw/roll degeneracy ADR-003 recorded is unchanged in substance; it now sits at pitch = 180
rather than pitch = 0.
