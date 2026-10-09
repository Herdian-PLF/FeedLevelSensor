# Arduino UNO Q verification

Minimal Arduino App Lab apps for the UNO Q, each recording the least code needed to use one
hardware or runtime capability of the board. Every subfolder is a complete app that can be copied
onto the board as-is. `litert_cpu_inference` runs a `.tflite` model on the CPU with `ai-edge-litert`;
`litert_tiff_inference` runs the same model on the dataset's 8x8 depth TIFFs and scores the 32x32
predictions against the ground truth.

```
embedded/arduino_unoq_verification/
├── litert_cpu_inference/
│   ├── app.yaml
│   ├── .gitignore
│   ├── models/                       # gitignored, created by hand
│   │   └── super_resolution.tflite
│   └── python/
│       ├── main.py
│       └── requirements.txt
└── litert_tiff_inference/
    ├── app.yaml
    ├── .gitignore
    ├── models/                       # gitignored, created by hand
    │   └── super_resolution.tflite
    ├── data/                         # gitignored, created by hand
    │   └── scene_NNNNNN/
    │       ├── depth_8x8.tif
    │       └── depth_32x32.tif       # optional, enables scoring
    ├── output/                       # gitignored, written by the app
    │   └── scene_NNNNNN_depth_32x32.tif
    └── python/
        ├── main.py
        └── requirements.txt
```

## Setup

The model and the data are not versioned. Generate them with the dataset notebooks, the training
notebook and the conversion script (see [synthetic-dataset](../../synthetic-dataset/README.md)),
then copy the model into each app:

```bash
for app in litert_cpu_inference litert_tiff_inference; do
  mkdir -p $app/models
  cp ../../synthetic-dataset/train_model/runs/QuickSRNetSmall/best_model.tflite $app/models/super_resolution.tflite
done
```

and the scenes to run into `litert_tiff_inference/data/`, for example the test split:

```bash
mkdir -p litert_tiff_inference/data
grep ',test$' ../../synthetic-dataset/train_model/runs/QuickSRNetSmall/dataset_split.csv | cut -d, -f1 |
  xargs -I{} cp -r ../../synthetic-dataset/out/synthetic_depth_dataset/scenes/{} litert_tiff_inference/data/
```

App Lab runs one app at a time, so stop whatever is running first (normally the gateway,
`user:gateway_arduinounoq`) and start it again afterwards.

```bash
arduino-app-cli app list
arduino-app-cli app stop user:gateway_arduinounoq
```

Copy the app to `/home/arduino/ArduinoApps/` on the board, then:

```bash
arduino-app-cli app start user:<app folder>
arduino-app-cli app logs user:<app folder>
```

The log ends with `RESULT: PASSED` or `RESULT: FAILED`. The app then idles until it is stopped.
