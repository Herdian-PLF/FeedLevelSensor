# notebooks

Generates the synthetic labelled depth dataset used to train and evaluate feed-level
reconstruction: an analytic feed surface inside a silo, observed by a simulated TMF8829 from a
randomised pose, written as depth images with ground-truth metadata.

## Setup

```bash
cd synthetic-dataset
uv sync
```


## Notebooks

| Notebook | Purpose |
|---|---|
| `gerar_superficies_yaml.ipynb` | Renders every surface in the catalogue to PNG for visual review. Read-only on the catalogue. |
| `gerar_10_cenas_sensor.ipynb` | 10 fully annotated scenes with geometry plots. Used to inspect placement and surface behaviour. |
| `gerar_dataset_5000_cenas_com_silos.ipynb` | Produces the 5000-scene training dataset. |

Full generation takes roughly 2 hours; `raycast_surface` is the bottleneck, not the
downsampling. To run it headless:

```bash
uv run --group dev python -m nbconvert --to notebook --execute --inplace gerar_dataset_5000_cenas_com_silos.ipynb
```

## Configs

| File | Contents |
|---|---|
| `../configs/catalogo_superficies.yaml` | 45 feed surfaces as `id`, `tipo`, `parametros`, spanning 9 analytic types (flat, discharge crater, rathole, avalanche front, truncated cone, eccentric crater, displaced pile, tilted plane, lateral pile with crater). |
| `../configs/config_sensor.yaml` | TMF8829 model (80° diagonal FoV, 0.01–11 m range, 8×8 and 32×32 zone grids, 10 mm noise sigma, 3% dropout) and the pose sampling ranges. |
| `../configs/config_silos.yaml` | 6 silo geometries. Header documents the coordinate convention; `y = 0` is the hopper/cylinder interface. Each silo is validated on load: `roof_top_y − bottom_y` must equal `total_height_m`. |

## Output

Written to `../out/synthetic_depth_dataset/`, which is gitignored.

```
out/synthetic_depth_dataset/
├── config/                     copies of the three input YAMLs
└── scenes/scene_NNNNNN/
    ├── depth_8x8.tif
    ├── depth_32x32.tif
    └── metadata.yaml
```

Depth images are **uint16 TIFF, uncompressed, millimetres, `0` = no return**. Zero is a sentinel,
not a distance — any consumer must mask it before averaging or scaling. `depth_32x32.tif` is the
noise-free raycast; `depth_8x8.tif` is the sensor's real output resolution and is the only one
carrying noise and dropout.

## Method

**Surface.** Each type is an analytic `y = f(x, z)` evaluated on a 160×160 grid over the silo
radius, plus a sinusoidal roughness term. Points outside the radius are `NaN`.

**Placement.** Pose is sampled from the ranges in `config_sensor.yaml` and rejected unless the
sensor clears the surface by `min_range_m` and stays `0.05 m` below the roof. Silos are assigned
round-robin by scene index; the surface is drawn at random.

**Raycast.** One ray per 32×32 zone, marched in 500 steps from `min_range_m` to `max_range_m`.
The surface height under each sample point is bilinearly interpolated. When a ray crosses from
above to below the surface, the distance recorded is the **last sample still above it**, not the
interpolated intersection. Rays leaving the silo radius terminate as no-return.

**Downsampling (32×32 → 8×8).** `cv2.resize` with `INTER_AREA`. At an integer factor of 4,
`INTER_AREA` is exactly the mean of each 4×4 block — the other interpolation flags sample a
neighbourhood around a point and miss most of the 16 source pixels, deviating by tens of
millimetres. It is applied twice, to `value × mask` and to `mask` alone, because `INTER_AREA` has
no concept of an invalid pixel: `(sum/16) ÷ (valid/16) = sum/valid`. A single unmasked resize
averages the zero sentinels into the result and has been measured to err by up to 2879 mm.

Input is cast to `float32` deliberately. Passing `uint16` straight to `cv2.resize` makes OpenCV
accumulate in fixed point and return an integer, which does not reproduce the block mean. The
cast is exact: a 4×4 block sums to at most `16 × 65535`, well inside the `2^24` integer range of
`float32`.

**Degradation.** Applied to the 8×8 map only: Gaussian noise at `noise_sigma_mm` on valid
pixels, then dropout at `dropout_fraction`. The result is rounded with `np.rint`, not truncated
by `astype` — truncation biased every valid pixel by −0.5 mm.


