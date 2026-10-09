"""Minimal LiteRT inference on the UNO Q CPU inside the Arduino App Lab container.

Loads models/super_resolution.tflite, runs it on a fixed synthetic input, checks the output is
finite and prints tensor details and invoke timings. Takes no parameters.
"""
import sys
import time
import traceback
from pathlib import Path

import numpy as np
# The classic Interpreter is the only working API on this board: CompiledModel (and any
# Environment.create()) loads libLiteRtWebGpuAccelerator.so, built for ARMv8.2, and the UNO Q's
# ARMv8.0 cores die on it with SIGILL even when only the CPU is requested.
from ai_edge_litert.interpreter import Interpreter

PATH_MODEL = Path(__file__).resolve().parents[1] / "models" / "super_resolution.tflite"
TIMED_RUNS = 100


def run_test() -> bool:
    try:
        interpreter = Interpreter(model_path=str(PATH_MODEL))
        interpreter.allocate_tensors()
        (detail_input,) = interpreter.get_input_details()
        (detail_output,) = interpreter.get_output_details()
        print(f"input  {detail_input['shape'].tolist()} {np.dtype(detail_input['dtype']).name}")
        print(f"output {detail_output['shape'].tolist()} {np.dtype(detail_output['dtype']).name}")

        # A fixed ramp over the normalised depth range keeps the output reproducible on the PC.
        size = int(np.prod(detail_input["shape"]))
        tensor_input = np.linspace(0.0, 1.0, size, dtype=np.float32).reshape(detail_input["shape"])
        interpreter.set_tensor(detail_input["index"], tensor_input)

        start = time.perf_counter()
        interpreter.invoke()
        first_ms = (time.perf_counter() - start) * 1e3
        output = interpreter.get_tensor(detail_output["index"])
        # The first invoke also pays one-off kernel setup, so the steady state is timed apart.
        start = time.perf_counter()
        for _ in range(TIMED_RUNS):
            interpreter.invoke()
        mean_ms = (time.perf_counter() - start) * 1e3 / TIMED_RUNS

        if not np.isfinite(output).all():
            raise ValueError("output holds NaN or Inf values")
    except Exception:
        traceback.print_exc(file=sys.stdout)
        return False

    print(f"output min {output.min():.6f} max {output.max():.6f} mean {output.mean():.6f}")
    print(f"first invoke {first_ms:.3f} ms, mean invoke {mean_ms:.3f} ms over {TIMED_RUNS} runs")
    return True


if __name__ == "__main__":
    print(f"RESULT: {'PASSED' if run_test() else 'FAILED'}", flush=True)
    # App Lab reports an app whose Python process exits on its own as failed, so the process stays
    # up after reporting until the app is stopped.
    while True:
        time.sleep(3600)
