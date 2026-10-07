"""Super-resolution of 8x8 depth TIFFs on the UNO Q CPU inside the Arduino App Lab container.

For every data/<scene>/depth_8x8.tif (uint16 mm, 0 = no return) it writes the 32x32 prediction to
output/<scene>_depth_32x32.tif in the same encoding and, when data/<scene>/depth_32x32.tif exists,
prints the scene's MAE and the share of pixels within 50 mm of it. Takes no parameters.
"""
import sys
import time
import traceback
from pathlib import Path

import numpy as np
import tifffile
# The classic Interpreter is the only working API on this board: CompiledModel (and any
# Environment.create()) loads libLiteRtWebGpuAccelerator.so, built for ARMv8.2, and the UNO Q's
# ARMv8.0 cores die on it with SIGILL even when only the CPU is requested.
from ai_edge_litert.interpreter import Interpreter

DIR_APP = Path(__file__).resolve().parents[1]
PATH_MODEL = DIR_APP / "models" / "super_resolution.tflite"
DIR_DATA = DIR_APP / "data"
DIR_OUTPUT = DIR_APP / "output"
# depth_max_mm of the training run's config.json: the model reads and writes depth / 11 000 mm.
DEPTH_MAX_MM = 11000.0
TOLERANCE_MM = 50.0


def run_test() -> bool:
    try:
        interpreter = Interpreter(model_path=str(PATH_MODEL))
        interpreter.allocate_tensors()
        (detail_input,) = interpreter.get_input_details()
        (detail_output,) = interpreter.get_output_details()

        scenes = sorted(path.parent for path in DIR_DATA.glob("*/depth_8x8.tif"))
        if not scenes:
            raise FileNotFoundError(f"no <scene>/depth_8x8.tif under {DIR_DATA}")
        DIR_OUTPUT.mkdir(exist_ok=True)

        errors = []
        for scene in scenes:
            depth_low = tifffile.imread(scene / "depth_8x8.tif")
            # Same normalisation as the training notebook: the no-return zeros go in unmasked.
            x = np.clip(depth_low.astype(np.float32) / DEPTH_MAX_MM, 0.0, 1.0)
            interpreter.set_tensor(detail_input["index"], x.reshape(detail_input["shape"]))
            start = time.perf_counter()
            interpreter.invoke()
            elapsed_ms = (time.perf_counter() - start) * 1e3
            pred_mm = interpreter.get_tensor(detail_output["index"]).squeeze() * DEPTH_MAX_MM
            tifffile.imwrite(DIR_OUTPUT / f"{scene.name}_depth_32x32.tif",
                             np.clip(np.rint(pred_mm), 0, 65535).astype(np.uint16), compression=None)

            line = f"{scene.name}: {elapsed_ms:.3f} ms"
            path_true = scene / "depth_32x32.tif"
            if path_true.is_file():
                true_mm = tifffile.imread(path_true).astype(np.float64)
                # Ground-truth zeros are no-return, not a distance, so they are left out of the score.
                valid = true_mm > 0
                error = np.abs(pred_mm[valid] - true_mm[valid])
                errors.append(error)
                line += (f", MAE {error.mean():.1f} mm, "
                         f"{np.mean(error <= TOLERANCE_MM) * 100:.1f}% within {TOLERANCE_MM:.0f} mm")
            print(line)
    except Exception:
        traceback.print_exc(file=sys.stdout)
        return False

    print(f"\n{len(scenes)} scenes written to {DIR_OUTPUT}")
    if errors:
        pooled = np.concatenate(errors)
        print(f"all scored pixels: MAE {pooled.mean():.2f} mm, "
              f"{np.mean(pooled <= TOLERANCE_MM) * 100:.2f}% within {TOLERANCE_MM:.0f} mm")
    return True


if __name__ == "__main__":
    print(f"RESULT: {'PASSED' if run_test() else 'FAILED'}", flush=True)
    # App Lab reports an app whose Python process exits on its own as failed, so the process stays
    # up after reporting until the app is stopped.
    while True:
        time.sleep(3600)
