"""导出模型的三个检测头，供Icraft转换使用。"""
import argparse
import copy
import hashlib
import json
import types
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
from ultralytics import YOLO


def raw_heads(self, features):
    outputs = []
    for index in range(self.nl):
        outputs.extend((self.cv3[index](features[index]), self.cv2[index](features[index])))
    return tuple(outputs)


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--weights", type=Path, default=repo / "pc_runs/m2_rgb_step5_baseline_b32/weights/best.pt"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo / "pc_runs/m5_drone_yolo11_339/2_compile/fmodel/drone_yolo11n_raw_heads.onnx",
    )
    parser.add_argument(
        "--report", type=Path, default=repo / "reports/drone_raw_heads_export_20261001.json"
    )
    args = parser.parse_args()
    weights, output = args.weights.resolve(), args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    detector = YOLO(str(weights))
    model = copy.deepcopy(detector.model).cpu().float().eval()
    model.fuse(verbose=False)
    head = model.model[-1]
    if head.nc != 1 or head.nl != 3 or head.reg_max != 16 or getattr(head, "end2end", False):
        raise ValueError(
            "This deployment adapter expects one class, three heads, reg_max=16, non-end2end Detect"
        )
    head.forward = types.MethodType(raw_heads, head)
    torch.manual_seed(42)
    example = torch.rand(1, 3, 640, 640)
    names = ["cls_s8", "box_s8", "cls_s16", "box_s16", "cls_s32", "box_s32"]
    with torch.no_grad():
        expected = tuple(value.cpu().numpy() for value in model(example))
        torch.onnx.export(
            model,
            example,
            str(output),
            opset_version=17,
            dynamo=False,
            input_names=["images"],
            output_names=names,
            dynamic_axes=None,
        )
    graph = onnx.load(str(output))
    onnx.checker.check_model(graph)
    session = ort.InferenceSession(str(output), providers=["CPUExecutionProvider"])
    actual = session.run(None, {"images": example.numpy()})
    comparisons = []
    for name, reference, candidate in zip(names, expected, actual):
        delta = float(np.max(np.abs(reference - candidate)))
        np.testing.assert_allclose(candidate, reference, atol=0.001, rtol=0.001)
        comparisons.append(
            {"name": name, "shape": list(reference.shape), "maximum_absolute_difference": delta}
        )
    producers = {name: node.op_type for node in graph.graph.node for name in node.output}
    if any(producers[value.name] != "Conv" for value in graph.graph.output):
        raise ValueError(
            "DetPost export endpoints must be the raw classification/regression convolutions"
        )
    report = {
        "weights": str(weights),
        "weights_sha256": hashlib.sha256(weights.read_bytes()).hexdigest(),
        "output": str(output),
        "onnx_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "torch_version": torch.__version__,
        "opset": 17,
        "classes": detector.names,
        "outputs": comparisons,
        "output_producers": {value.name: producers[value.name] for value in graph.graph.output},
        "all_passed": True,
        "scope": "Six-head numerical export check on one seeded input; full detection alignment is tested separately.",
    }
    args.report.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.report.resolve().write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Exported: {output}")
    for item in comparisons:
        print(item)


if __name__ == "__main__":
    main()
