# train_model

Trains the super-resolution model that reconstructs each scene's `depth_32x32.tif` from its
`depth_8x8.tif`, using the dataset in `../out/synthetic_depth_dataset/`, and saves the run's
checkpoint, metrics and charts.

## Setup

```bash
cd synthetic-dataset
uv sync
```

Generate the dataset first (see `../README.md`), then run
`train_quicksrnet_depth_superresolution.ipynb` with the `synthetic-dataset/.venv` kernel. To export
the run's `best_model.pt` to `.tflite`, run `../scripts/convert_super_resolution_pt_to_tflite.py`
(see `../README.md`).

## Output

Each run is written to `runs/<model class>/` (gitignored); training the same model again overwrites
it, except `best_model.onnx` and `best_model.tflite`, which only the conversion script rewrites.

```
runs/<model class>/
├── config.json                       hyperparameters of the run
├── dataset_split.csv                 train / val / test split of every scene, with its silo and surface
├── history.csv                       one row per epoch
├── best_model.pt                     checkpoint of the epoch with the lowest validation MAE, used for every test analysis
├── best_model.onnx                   best_model.pt exported by the conversion script
├── best_model.tflite                 best_model.onnx compiled by Qualcomm AI Hub
├── test_metrics.json                 global test metrics
├── metrics_by_scene.csv              test metrics per scene
├── metrics_by_depth_range.csv        test metrics per 500 mm range of reference depth
├── training_curves.png               MAE and validation within_tolerance_pct per epoch
├── test_metrics.png                  within_tolerance_pct vs. tolerance, δ thresholds and AbsRel, MAE / RMSE / bias, absolute error percentiles
├── error_distribution.png            signed error histogram; predicted vs. reference depth
├── performance_by_group.png          within_tolerance_pct per silo and per surface type
├── metrics_by_depth_range.png        MAE, within_tolerance_pct and bias per depth range
└── examples.png                      input, prediction, reference and absolute error of 4 test scenes
```

The model, `best_model.onnx` and `best_model.tflite` take float32 `[1, 1, 8, 8]` and return
`[1, 1, 32, 32]`, both as depth in mm ÷ `depth_max_mm` (`config.json`, the sensor's max range)
clipped to `[0, 1]`. `0` in the input means no return; the output predicts a depth for every pixel.

## Metrics

Errors are `prediction − reference` depth in millimetres, computed only on pixels whose reference
is not `0` (no return). Metrics of a scene, depth range, silo or surface type pool all valid pixels
of that group; they are not averages of per-scene values.

| Key | Meaning |
|---|---|
| `within_tolerance_pct` | % of valid pixels with absolute error ≤ `ACCURACY_TOLERANCE_MM` (`tolerance_mm` in `config.json`, 50 mm) |
| `mae_mm` | mean absolute error |
| `rmse_mm` | root mean squared error; weighs large errors more than `mae_mm` |
| `bias_mm` | mean signed error; positive means the surface is predicted farther from the sensor |
| `median_absolute_error_mm` | half of the pixels have a smaller absolute error |
| `p90_absolute_error_mm`, `p95_absolute_error_mm` | 90% / 95% of the pixels have a smaller absolute error |
| `psnr_db` | `20·log10(max range / rmse_mm)`, max range being `sensor.max_range_m` from `config_sensor.yaml` |
| `abs_rel_pct` | mean of absolute error divided by the reference depth, in % (AbsRel of depth estimation benchmarks) |
| `delta_1_pct`, `delta_2_pct`, `delta_3_pct` | % of valid pixels with `max(prediction / reference, reference / prediction)` below 1.25, 1.25², 1.25³ |
| `valid_pixels` | number of valid pixels the row was computed on |

| File | Columns |
|---|---|
| `test_metrics.json` | every key above except `valid_pixels` |
| `metrics_by_scene.csv` | `scene`, `silo`, `surface_type`, `valid_pixels`, `mae_mm`, `rmse_mm`, `bias_mm`, `within_tolerance_pct` |
| `metrics_by_depth_range.csv` | `depth_range_mm`, `valid_pixels`, `mae_mm`, `rmse_mm`, `bias_mm`, `within_tolerance_pct` |

`history.csv`:

| Column | Meaning |
|---|---|
| `epoch` | epoch number; training stops after `EARLY_STOPPING_PATIENCE` (20) epochs without a lower `val_mae_mm`, or at `epochs` (`config.json`) |
| `train_mae_mm` | absolute error summed over every valid training pixel of the epoch, divided by their count; each batch is measured before its weight update, so it can exceed `val_mae_mm` in the first epochs |
| `val_mae_mm` | `mae_mm` on the validation split at the end of the epoch |
| `val_within_tolerance_pct` | `within_tolerance_pct` on the validation split at the end of the epoch |
| `learning_rate` | learning rate of the epoch; halved after 8 epochs without improvement |
