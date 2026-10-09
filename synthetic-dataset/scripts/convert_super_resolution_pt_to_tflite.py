"""Convert the depth super-resolution checkpoint into a float32 .tflite through Qualcomm AI Hub.

Route: notebook model cell + state_dict -> ONNX -> onnxruntime check -> qai-hub compile -> .tflite
-> TFLite interpreter check. Each check runs PyTorch and the converted model on the same inputs and
exits non-zero when an output differs by more than --atol.

The .tflite keeps the notebook's tensor contract: float32 [1, 1, low_size, low_size] in and
[1, 1, high_size, high_size] out (configs/config_sensor.yaml), both depth / depth_max_mm clipped to
[0, 1], with 0 meaning no return.
"""
from __future__ import annotations

import argparse
import ast
import csv
import json
import os
import sys
from pathlib import Path

# Hidden before torch loads, so the notebook's DEVICE resolves to CPU and the reference outputs come
# from plain float32 kernels like the TFLite interpreter's: cuDNN may run float32 convolutions in
# TF32, whose error alone would exceed --atol.
os.environ["CUDA_VISIBLE_DEVICES"] = ""

import numpy as np
import onnx
import onnxruntime
import qai_hub
import tifffile
import torch
import yaml
from ai_edge_litert.interpreter import Interpreter

DIR_PROJECT = Path(__file__).resolve().parents[1]
DIR_DATASET = DIR_PROJECT / "out" / "synthetic_depth_dataset"
PATH_SENSOR_CONFIG = DIR_PROJECT / "configs" / "config_sensor.yaml"
RANDOM_SAMPLES = 64


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", type=Path,
                        default=DIR_PROJECT / "train_model" / "runs" / "QuickSRNetSmall" / "best_model.pt",
                        help="best_model.pt written by the training notebook; config.json and "
                             "dataset_split.csv are read from the same run directory")
    parser.add_argument("--output", type=Path,
                        help="destination .tflite, the checkpoint path with a .tflite suffix by "
                             "default; the intermediate .onnx is written next to it")
    parser.add_argument("--notebook", type=Path,
                        default=DIR_PROJECT / "train_model" / "train_quicksrnet_depth_superresolution.ipynb",
                        help="training notebook whose model cell defines the architecture")
    parser.add_argument("--device", default="QCS8550 (Proxy)",
                        help="qai-hub device the compile job targets")
    parser.add_argument("--atol", type=float, default=1e-4,
                        help="largest absolute output error accepted against PyTorch, in normalised "
                             "depth (1 = depth_max_mm)")
    parser.add_argument("--skip-compile", action="store_true",
                        help="stop after the onnxruntime check, without submitting a qai-hub job")
    return parser.parse_args(argv)


def _assigns_model(source: str) -> bool:
    return any(
        isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "model" for target in node.targets)
        for node in ast.parse(source).body
    )


def build_model_from_notebook(path_notebook: Path) -> torch.nn.Module:
    notebook = json.loads(path_notebook.read_text(encoding="utf-8"))
    sources = ["".join(cell["source"]) for cell in notebook["cells"] if cell["cell_type"] == "code"]
    index_model = next((i for i, source in enumerate(sources) if _assigns_model(source)), None)
    if index_model is None:
        raise SystemExit(f"{path_notebook}: no code cell assigns `model`")

    # The notebook's model cell may be swapped for any nn.Module, so the architecture comes from
    # running it instead of from a copy here that would drift from what was trained. Only the setup
    # cells before it run too: data loading and training come after it in the notebook.
    namespace = {}
    for source in sources[:index_model + 1]:
        exec(compile(source, str(path_notebook), "exec"), namespace)
    return namespace["model"]


def load_trained_model(path_checkpoint: Path, path_notebook: Path) -> tuple[torch.nn.Module, dict]:
    config_run = json.loads((path_checkpoint.parent / "config.json").read_text(encoding="utf-8"))
    model = build_model_from_notebook(path_notebook)
    if type(model).__name__ != config_run["model"]:
        raise SystemExit(f"{path_notebook} builds {type(model).__name__}, but {path_checkpoint} "
                         f"was trained as {config_run['model']}")

    checkpoint = torch.load(path_checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict(checkpoint["model_state_dict"])
    print(f"loaded {path_checkpoint} (epoch {checkpoint['epoch']})")
    return model.eval(), config_run


def validation_inputs(size: int, path_split: Path, depth_max_mm: float) -> dict:
    rng = np.random.default_rng(0)
    inputs = {
        "zeros": np.zeros((1, 1, size, size), np.float32),
        "ones": np.ones((1, 1, size, size), np.float32),
        "random": rng.uniform(0.0, 1.0, (RANDOM_SAMPLES, 1, size, size)).astype(np.float32),
    }

    scenes = []
    if path_split.is_file():
        with path_split.open(encoding="utf-8", newline="") as file:
            scenes = [row["scene"] for row in csv.DictReader(file) if row["split"] == "test"]
    paths = [DIR_DATASET / "scenes" / scene / f"depth_{size}x{size}.tif" for scene in scenes]
    if not paths or not all(path.is_file() for path in paths):
        print(f"test split skipped: {path_split} or its scenes under {DIR_DATASET} are missing")
        return inputs

    depth_mm = np.stack([tifffile.imread(path) for path in paths]).astype(np.float32)
    # Must match load_depth in the notebook, which feeds the no-return zeros to the model unmasked.
    inputs["test split"] = np.clip(depth_mm / depth_max_mm, 0.0, 1.0)[:, None]
    return inputs


def export_onnx(model: torch.nn.Module, shape_input: tuple, path_onnx: Path) -> None:
    torch.onnx.export(
        model,
        (torch.zeros(shape_input),),
        path_onnx,
        input_names=["depth_low"],
        output_names=["depth_high"],
        # 18 is the lowest opset the torch.export-based exporter implements; a lower one is only
        # reached through a best-effort down-conversion.
        opset_version=18,
        # Otherwise the weights go to a sidecar .data file, which the qai-hub upload of this single
        # path would leave behind.
        external_data=False,
        verbose=False,
    )
    ops = sorted({node.op_type for node in onnx.load(path_onnx).graph.node})
    print(f"wrote {path_onnx} ({path_onnx.stat().st_size / 1e3:.1f} kB), ops: {', '.join(ops)}")


def onnxruntime_runner(path_onnx: Path):
    session = onnxruntime.InferenceSession(str(path_onnx), providers=["CPUExecutionProvider"])
    name_input = session.get_inputs()[0].name
    return lambda batch: session.run(None, {name_input: batch})[0]


def compile_tflite(path_onnx: Path, path_tflite: Path, name_device: str) -> None:
    job = qai_hub.submit_compile_job(
        model=str(path_onnx),
        device=qai_hub.Device(name_device),
        options="--target_runtime tflite",
    )
    print(f"compile job {job.job_id}: {job.url}")
    status = job.wait()
    if not status.success:
        raise SystemExit(f"compile job {job.job_id} failed: {status.message}")

    job.download_target_model(str(path_tflite))
    print(f"wrote {path_tflite} ({path_tflite.stat().st_size / 1e3:.1f} kB)")


def tflite_runner(path_tflite: Path, shape_input: tuple, shape_output: tuple):
    interpreter = Interpreter(model_path=str(path_tflite))
    interpreter.allocate_tensors()
    (detail_input,) = interpreter.get_input_details()
    (detail_output,) = interpreter.get_output_details()
    for kind, detail, shape in (("input", detail_input, shape_input),
                                ("output", detail_output, shape_output)):
        shape_tflite = tuple(detail["shape"].tolist())
        print(f"tflite {kind} {detail['name']}: {shape_tflite} {np.dtype(detail['dtype'])}")
        if shape_tflite != shape or detail["dtype"] != np.float32:
            raise SystemExit(f"tflite {kind} is not float32 {shape}")

    def run(batch: np.ndarray) -> np.ndarray:
        interpreter.set_tensor(detail_input["index"], batch)
        interpreter.invoke()
        return interpreter.get_tensor(detail_output["index"])

    return run


def check_against_pytorch(backend: str, run, model: torch.nn.Module, inputs: dict,
                          depth_max_mm: float, atol: float) -> bool:
    print(f"\n{backend} vs PyTorch, one sample per inference")
    print(f"  {'inputs':<10} {'samples':>7}  {'output shape':<14}  {'PyTorch output':>21}"
          f"  {'max |error|':>21}  {'mean |error|':>21}")
    passed = True
    for name, batch in inputs.items():
        with torch.no_grad():
            expected = model(torch.from_numpy(batch)).numpy()
        actual = np.concatenate([run(batch[i:i + 1]) for i in range(len(batch))])
        if actual.shape != expected.shape:
            print(f"  {name:<10} output {actual.shape[1:]} per sample, PyTorch {expected.shape[1:]}")
            passed = False
            continue

        error = np.abs(actual - expected)
        passed &= bool(error.max() <= atol)
        span = f"{expected.min() * depth_max_mm:.1f} .. {expected.max() * depth_max_mm:.1f} mm"
        error_max = f"{error.max():.2e} ({error.max() * depth_max_mm:.4f} mm)"
        error_mean = f"{error.mean():.2e} ({error.mean() * depth_max_mm:.4f} mm)"
        print(f"  {name:<10} {len(batch):>7}  {str((1, *actual.shape[1:])):<14}  {span:>21}"
              f"  {error_max:>21}  {error_mean:>21}")
    print(f"  {'PASS' if passed else 'FAIL'} at atol {atol:g} ({atol * depth_max_mm:g} mm), "
          f"errors in normalised depth, 1 = {depth_max_mm:g} mm")
    return passed


def main(argv=None) -> int:
    args = parse_args(argv)
    path_checkpoint = args.checkpoint.expanduser().resolve()
    path_tflite = (args.output or path_checkpoint.with_suffix(".tflite")).expanduser().resolve()
    if path_tflite.suffix != ".tflite":
        raise SystemExit(f"--output must end in .tflite: {path_tflite}")
    path_onnx = path_tflite.with_suffix(".onnx")
    path_tflite.parent.mkdir(parents=True, exist_ok=True)

    model, config_run = load_trained_model(path_checkpoint, args.notebook.expanduser().resolve())
    sensor = yaml.safe_load(PATH_SENSOR_CONFIG.read_text(encoding="utf-8"))["sensor"]
    shape_input = (1, 1, sensor["low_size"], sensor["low_size"])
    shape_output = (1, 1, sensor["high_size"], sensor["high_size"])
    with torch.no_grad():
        shape_model = tuple(model(torch.zeros(shape_input)).shape)
    if shape_model != shape_output:
        raise SystemExit(f"{config_run['model']} maps {shape_input} to {shape_model}, "
                         f"{PATH_SENSOR_CONFIG} expects {shape_output}")

    depth_max_mm = config_run["depth_max_mm"]
    inputs = validation_inputs(sensor["low_size"], path_checkpoint.parent / "dataset_split.csv",
                               depth_max_mm)

    export_onnx(model, shape_input, path_onnx)
    if not check_against_pytorch("onnxruntime", onnxruntime_runner(path_onnx), model, inputs,
                                 depth_max_mm, args.atol):
        return 1
    if args.skip_compile:
        print("--skip-compile set, stopping before the qai-hub submission")
        return 0

    compile_tflite(path_onnx, path_tflite, args.device)
    run_tflite = tflite_runner(path_tflite, shape_input, shape_output)
    return 0 if check_against_pytorch("TFLite", run_tflite, model, inputs, depth_max_mm,
                                      args.atol) else 1


if __name__ == "__main__":
    sys.exit(main())
