# ADR-001: Simulation stack and frame conventions

**Date:** 2026-08-10
**Status:** Accepted

## Context

Evaluating feed-level reconstruction needs a ground truth the hardware cannot provide. A
simulator has to represent silo and feed geometry, cast one ray per sensor zone, and score the
reconstruction — which requires fixing a coordinate convention, a geometry representation and a
3D visualisation stack before any of it can be written.

Three constraints shaped the choice. The team has no prior Python 3D experience, so breadth of
available reference material outweighs raw capability. The measurement of interest is
*algorithmic* error, so ground-truth geometry must not itself introduce discretisation error.
And the driver already ships a reconstruction — `zCorrection` in
`python-poc/driver/tmf8829/tmf8829_application_common.py` — whose behaviour must be reproducible
for comparison.

## Decision

**Stack:** numpy for the ray maths, trimesh for imported meshes, PyVista (VTK) for interactive
3D, matplotlib for the 2D zone map. VTK is the long-standing standard for scientific 3D in
Python and PyVista is its documented wrapper; both have Python 3.13 wheels. Open3D was rejected
because it has no cp313 wheels and would have pinned the project to 3.12.

**Geometry:** analytic primitives with closed-form ray intersection (plane, disc/annulus,
cylinder, frustum/cone) and feed surfaces as height fields `z = f(x, y)`. Height fields whose
shape has a closed form use it; arbitrary callables fall back to marching plus bisection.
Triangle meshes are supported through trimesh but are not the default, so the reference geometry
carries no tessellation error.

**Frames:** world `+Z` up with the origin at the centre of the silo floor, all lengths in
millimetres to match the device's native reporting. Camera frame follows OpenCV — `+Z` along the
optical axis, `+X` right, `+Y` down. A pose is a single 4x4 `world_T_cam`.

**Vendor model:** `zCorrection` normalises pixel offsets by `spanX = X*3/4` and `spanY = Y`,
which is algebraically a pinhole with `fx = 0.75*width`, `fy = height`. It is therefore
represented as a second set of `Intrinsics` rather than a separate code path, and its implied
field of view is a resolution-independent 67.38 x 53.13 degrees.

**Sensor model:** one central ray per zone. The reported value is the radial focus-point range,
quantised to the device's 0.25 mm LSB.

## Consequences

Ground truth is exact for the shapes that matter, so a flat scene reconstructs to the
quantisation floor (residual RMS 0.066 mm, matching LSB/sqrt(12)) and any larger error is
attributable to the reconstruction. Because the vendor model is just another `Intrinsics`, the
error it contributes is zero whenever the real optics happen to match 67.38 x 53.13 degrees —
the true field of view is still an open input, and until it is supplied from the datasheet no
absolute error figure is trustworthy.

The single-ray zone model cannot reproduce mixed-pixel or edge-straddling effects, which are
real on a coarse grid over a silo. Adding a supersampled footprint later changes only ray
generation inside `IdealTofSensor`, as the rest of the pipeline consumes a distance grid.

PyVista requires an OpenGL context. Under WSL2 this works through WSLg with D3D12 GPU
passthrough, but *not* from a conda-based interpreter, whose bundled `libstdc++` is older than
Mesa's `libLLVM` requires; the project therefore uses a uv-managed interpreter.
