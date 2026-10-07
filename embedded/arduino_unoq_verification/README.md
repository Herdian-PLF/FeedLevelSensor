# Arduino UNO Q verification

Minimal Arduino App Lab apps for the UNO Q, each recording the least code needed to use one
hardware or runtime capability of the board. Every subfolder is a complete app that can be copied
onto the board as-is. `litert_cpu_inference` runs a `.tflite` model on the CPU with `ai-edge-litert`.

```
embedded/arduino_unoq_verification/
└── litert_cpu_inference/
    ├── app.yaml
    ├── .gitignore
    ├── models/                       # gitignored, created by hand
    │   └── super_resolution.tflite
    └── python/
        ├── main.py
        └── requirements.txt
```

## Setup

The model is not versioned. Generate it with the training notebook and the conversion script
(see [synthetic-dataset](../../synthetic-dataset/README.md)), then copy it into the app:

```bash
mkdir -p litert_cpu_inference/models
cp ../../synthetic-dataset/train_model/runs/QuickSRNetSmall/best_model.tflite \
   litert_cpu_inference/models/super_resolution.tflite
```

App Lab runs one app at a time, so stop whatever is running first (normally the gateway,
`user:gateway_arduinounoq`) and start it again afterwards.

```bash
arduino-app-cli app list
arduino-app-cli app stop user:gateway_arduinounoq
```

Copy the app to `/home/arduino/ArduinoApps/` on the board, then:

```bash
arduino-app-cli app start user:litert_cpu_inference
arduino-app-cli app logs user:litert_cpu_inference
```

The log ends with `RESULT: PASSED` or `RESULT: FAILED`. The app then idles until it is stopped.
