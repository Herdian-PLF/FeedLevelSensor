# ADR-002: Silo body model

**Date:** 2026-08-10
**Status:** Accepted

## Context

The simulator initially modelled the silo as a plain cylinder with a flat floor. A real silo is
three sections stacked bottom to top: an inverted cone trunk (the hopper) opening out from the
discharge outlet, a cylindrical shell, and a cone trunk narrowing to the roof opening. The lid is
out of scope for now.

That difference is not cosmetic. Cross-sectional area varies with height, so feed volume is not
`pi r^2 h`, and the fraction of the silo that a low fill occupies is much smaller than a
cylinder of the same footprint would suggest. It also breaks the rule used to tell feed returns
from wall returns: with a constant radius, "inside the body radius" identifies feed, but in a
hopper a genuine feed return can sit far inside that radius while the sloping wall is what the
ray actually hit.

## Decision

A single `SiloProfile` describes the body as a solid of revolution, given the three section
heights and the outlet, body and roof radii. Everything needing the silo's shape goes through
it: surface construction, volume integration, feed/wall classification and mesh clipping. A
cylinder is the degenerate case (`SiloProfile.cylindrical`), where the hopper and roof sections
have zero height and are omitted.

The profile exposes radius as a function of height, and its inverse as the interval of heights
enclosed at a given radius. Volume is integrated as vertical columns clamped to that interval,
which is exact for any surface and reduces to the closed-form frustum and cylinder volumes for a
horizontal one. Feed/wall classification tests a point against the wall radius *at that point's
own height*.

Ray casting needs no clipping: a feed surface extending past the wall is always occluded by the
wall, because any ray reaching beyond it must cross the shell first, and the scene keeps the
nearest hit. Meshes are clipped, since an unclipped one would visibly poke through the silo.

## Consequences

Volume and fill percentage are now correct for a real silo, and validated: the numeric column
integration matches the analytic section volumes at fill levels spanning all three sections. Fill
percentage against true capacity became available and is reported.

Low fills behave very differently from the cylindrical case. With feed at 800 mm in a 5.2 m silo,
only 8 of 64 zones land on feed and the rest hit the hopper wall — so classification, not surface
fitting, is what determines whether a near-empty silo reads correctly. A constant-radius
threshold accepts those wall returns as feed.

The profile assumes a vertical axis of revolution and straight-sided sections. A silo with a
non-circular cross-section, an off-axis hopper or curved walls needs the triangle-mesh path
instead, which the metrics do not currently support — they require a height field for the feed
and a profile for the body.
