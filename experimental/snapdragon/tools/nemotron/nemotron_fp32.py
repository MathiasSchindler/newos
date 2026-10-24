"""Export project-owned FP32 HTP operators without changing M0 semantics."""

import argparse
import ctypes
import struct
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, write_json
from nemotron_m1 import F32, I32, STATIC, Graph, encoder, fixture_write, prediction, joiner, subsampling
from nemotron_trace import load_model


OPS = dict(zip((
    "MatMul", "ElementWiseAdd", "ElementWiseMultiply", "Reshape", "Transpose", "StridedSlice",
    "Concat", "LayerNorm", "Sigmoid", "Tanh", "Relu", "Softmax", "Pad", "Conv2d", "DepthWiseConv2d",
    "Gather", "FullyConnected", "ElementWiseSubtract", "ElementWiseRsqrt", "ReduceMean", "ElementWiseSelect",
), range(1, 22)))
PARAMS = dict(zip((
    "axis", "perm", "ranges", "epsilon", "stride", "dilation", "pad_amount", "scheme",
    "pad_constant_value", "transpose_in0", "transpose_in1", "beta", "keep_dims", "axes", "weight_tiles",
), range(1, 16)))
PACKAGE = "newos.nemotron.fp32"
META_MAGIC = 0x4E4D3332
META_BASE = 34


def metadata(graph, node):
    import numpy as np

    inputs = node["inputs"]
    output = graph.tensors[node["output"]]
    if not 1 <= len(inputs) <= 4 or output["dtype"] != F32:
        raise ValueError("FP32 custom operator requires 1..4 value inputs and one float32 output")
    result = [0] * META_BASE
    result[:5] = [META_MAGIC, 1, OPS[node["operation"]], len(inputs), len(output["shape"])]
    result[5:5 + len(output["shape"])] = output["shape"]
    for index, tensor_index in enumerate(inputs):
        item = graph.tensors[tensor_index]
        if item["dtype"] != F32 and not (node["operation"] == "Gather" and index == 1 and item["dtype"] == I32):
            raise ValueError("Custom kernel value-input dtype mismatch")
        record = 9 + index * 6
        result[record:record + 2] = [item["dtype"], len(item["shape"])]
        result[record + 2:record + 2 + len(item["shape"])] = item["shape"]
    result[33] = len(node["parameters"])
    for parameter in node["parameters"]:
        code = PARAMS[parameter["name"]]
        if code == 15:
            result[1] = 2
        if parameter["type"] == 0:
            values = [parameter["value"]]
        else:
            value = graph.tensors[parameter["value"]]["value"]
            if value is None:
                raise ValueError("Custom operator parameters must be immutable")
            values = [int(item) & 0xFFFFFFFF for item in value.reshape(-1)]
        if not 1 <= len(values) <= 16:
            raise ValueError("Invalid custom parameter element count")
        result.extend([code, len(values), *values])
    return np.asarray(result, dtype="<u4").view("<i4")


def convert(graph, pack_weights=False):
    import numpy as np

    nodes = list(graph.nodes)
    for index, node in enumerate(nodes):
        if pack_weights and node["operation"] == "MatMul":
            weight = graph.tensors[node["inputs"][1]]
            shape = weight["shape"]
            if weight["type"] == STATIC and shape[-1] % 32 == 0:
                if weight.get("layout") == "column-tiles-32":
                    raise ValueError("MatMul weights already packed")
                array = weight["value"]
                shape_tiles = list(array.shape[:-2]) + [shape[-2], shape[-1] // 32, 32]
                permutation = list(range(array.ndim - 2)) + [array.ndim - 1, array.ndim - 2, array.ndim]
                weight["value"] = np.transpose(array.reshape(shape_tiles), permutation).copy().reshape(shape)
                weight["layout"] = "column-tiles-32"
                node["parameters"].append(graph.scalar("weight_tiles", 32))
        data = metadata(graph, node)
        attributes = graph.tensor(f"fp32-meta-{index:04d}", list(data.shape), STATIC, data, I32)
        node["inputs"] = [attributes, *node["inputs"]]
        node["operation"] = "NmGather" if node["operation"] == "Gather" else "NmExecute"
        node["parameters"] = []
    return graph


class CReference:
    """Development-only execution of the same C kernels, not an accelerator fallback."""

    def __init__(self, library):
        self.library = ctypes.CDLL(str(Path(library).resolve()))
        self.function = self.library.NmFp32Reference
        self.function.argtypes = [
            ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_size_t), ctypes.c_size_t, ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t, ctypes.c_uint, ctypes.c_uint,
        ]
        self.function.restype = ctypes.c_int

    def evaluate(self, graph, inputs):
        import numpy as np
        import torch
        from nemotron_m1 import READ, WRITE

        values = {index: item["value"] for index, item in enumerate(graph.tensors) if item["type"] == STATIC}
        for index, item in enumerate(graph.tensors):
            if item["type"] == WRITE:
                value = inputs[item["name"]]
                if hasattr(value, "detach"):
                    value = value.detach().cpu().numpy()
                values[index] = np.ascontiguousarray(value, dtype="<i4" if item["dtype"] == I32 else "<f4")
        for node in graph.nodes:
            descriptor = metadata(graph, node).view("<u4")
            arguments = [values[index] for index in node["inputs"]]
            pointers = (ctypes.c_void_p * len(arguments))(*(item.ctypes.data for item in arguments))
            counts = (ctypes.c_size_t * len(arguments))(*(item.size for item in arguments))
            output = np.empty(graph.tensors[node["output"]]["shape"], dtype="<f4")
            status = self.function(
                descriptor.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)), descriptor.size,
                pointers, counts, len(arguments), output.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                output.size, 0, 1,
            )
            if status:
                raise RuntimeError(f"CPU diagnostic operator {node['name']} failed: {status}")
            values[node["output"]] = output
        return {item["name"]: torch.from_numpy(values[index])
                for index, item in enumerate(graph.tensors) if item["type"] == READ}


def cpu_check(data_dir, library, output, steps, full=False):
    import torch
    from nemotron_m0 import load_json
    from nemotron_m1 import read_tensor, packed_cache

    model = load_model(data_dir)
    reference = CReference(library)
    directory = data_dir / "reference-1" / "long"
    manifest = load_json(directory / "manifest.json")
    records = {item["name"]: item for item in manifest["tensors"]}
    graph = encoder(model, 32, 24 if full else 1, full)
    first, steady = subsampling(model, True, 32), subsampling(model, False, 32)
    front_state, core_state, previous = {}, {}, None
    results = []
    with torch.inference_mode():
        for source in manifest["trace"]["steps"][:steps]:
            index = source["index"]
            prefix = f"chunk-{index:04d}"
            features = read_tensor(directory, records, prefix + "-features")
            front = reference.evaluate(first if index == 0 else steady, {"features": features[0], **front_state})
            for cache in range(3):
                expected = read_tensor(directory, records, prefix + f"-output-convolution-subsampling-{cache}")
                torch.testing.assert_close(front[f"sub-{cache}-out"], expected.permute(0, 2, 3, 1),
                                           atol=0.005, rtol=0.005)
                front_state[f"sub-{cache}-in"] = front[f"sub-{cache}-out"]
            inputs = {"hidden": front["hidden"], "mask": torch.zeros((8, 4, 60))}
            inputs["mask"][:, :, :56 - min(56, index * 4)] = -10000
            for item in graph.tensors:
                if item["type"] == 0 and item["name"] not in inputs:
                    inputs[item["name"]] = core_state.get(item["name"], torch.zeros(item["shape"]))
            actual = reference.evaluate(graph, inputs)
            if full:
                expected = read_tensor(directory, records, prefix + "-encoder-raw")[0]
                for name, reference_name in (("conditioned", "conditioned"), ("projected", "joiner-encoder-projection")):
                    torch.testing.assert_close(actual[name], read_tensor(directory, records, prefix + "-" + reference_name)[0],
                                               atol=0.005, rtol=0.005)
            else:
                captured = []
                hook = model.encoder.layers[0].register_forward_hook(lambda module, args, value: captured.append(value))
                try:
                    previous = model.get_audio_features(
                        features, prompt_ids=torch.tensor([9]), num_lookahead_tokens=3,
                        past_key_values=previous.past_key_values if previous else None,
                        padding_cache=previous.padding_cache if previous else None, use_cache=True,
                    )
                finally:
                    hook.remove()
                expected = captured[0][0]
            torch.testing.assert_close(actual["raw"], expected, atol=0.005, rtol=0.005)
            for layer in range(24 if full else 1):
                for label, reference_suffix in (("key", "keys"), ("value", "values"), ("conv", None)):
                    original = f"{prefix}-output-attention-{layer}-{reference_suffix}" if reference_suffix else \
                        f"{prefix}-output-convolution-conv-{layer}"
                    expected_cache = packed_cache(read_tensor(directory, records, original), reference_suffix is not None)
                    name = f"layer-{layer:02d}-" + label + "-out"
                    torch.testing.assert_close(actual[name], expected_cache, atol=0.005, rtol=0.005)
                    core_state[f"layer-{layer:02d}-" + label + "-in"] = actual[name]
            results.append({"step": index, "max_abs_error": float((actual["raw"] - expected).abs().max())})
            print(f"Portable FP32 C diagnostic {index + 1}/{steps} passed", flush=True)
    write_json(output, {"status": "cpu_kernel_diagnostic_passed", "accelerator_acceptance": False,
                        "reference_library_sha256": digest(library), "carried_state": True,
                        "full_encoder": full, "steps": results})


def smoke(output):
    import numpy as np

    if output.exists():
        raise ValueError("Smoke output already exists")
    output.mkdir(parents=True)
    artifacts = []
    for name in ("add", "matmul", "matmul-hvx"):
        graph = Graph("nm_fp32_" + name, 32)
        shape = [4] if name == "add" else [1, 2]
        left = graph.input("left", shape)
        right = graph.input("right", shape if name == "add" else [2, 32 if name == "matmul-hvx" else 1])
        if name == "add":
            value = graph.binary("precise-add", "ElementWiseAdd", left, right)
            a = np.asarray([10000.0, 9999.0, 10000.125, 123.0001], dtype="<f4")
            b = np.asarray([-9999.0, -10000.0, -10000.0, -123.0], dtype="<f4")
            expected = a + b
        else:
            value = graph.matmul("precise-matmul", left, right)
            a = np.asarray([[1.0001, -1]], dtype="<f4")
            b = np.full((2, 32 if name == "matmul-hvx" else 1), 10000.0, dtype="<f4")
            expected = (a[0, 0] * b[0] + a[0, 1] * b[1])[None, :].astype("<f4")
        graph.output(value, "output")
        convert(graph)
        record = graph.write(output / (name + ".nmg"))
        case = fixture_write(output / (name + ".nmf"), [record], [{
            "flags": 3, "cells": {(0, "left"): a, (0, "right"): b, (0, "output"): expected},
        }], "precise-" + name, 32)
        artifacts.append({"graph": record, "fixture": case})
    write_json(output / "manifest.json", {
        "package": PACKAGE, "smoke": artifacts,
        "precision_proof": "FP16 input conversion loses cancellation residuals of1.0 and0.125, violating unchangedM0tolerances",
    })


def micro(output, packed=False):
    import numpy as np

    if output.exists():
        raise ValueError("Microbenchmark output already exists")
    output.mkdir(parents=True)
    random = np.random.default_rng(73)
    graph = Graph("nm_fp32_dense_micro", 32)
    left = graph.input("input", [4, 1024])
    a = random.normal(size=(4, 1024)).astype("<f4")
    b = (random.normal(size=(1024, 4096)) * 0.03).astype("<f4")
    weight = graph.constant("weight", b)
    graph.output(graph.matmul("dense", left, weight), "output")
    expected = a @ b
    record = convert(graph, packed).write(output / "dense.nmg")
    steps = [{"flags": (1 if index == 0 else 0) | (2 if index == 7 else 0),
              "cells": {(0, "input"): a, (0, "output"): expected}} for index in range(8)]
    fixture = fixture_write(output / "dense.nmf", [record], steps, "dense", 32)
    write_json(output / "manifest.json", {
        "graph": record, "fixture": fixture, "macs_per_step": 4 * 1024 * 4096,
        "purpose": "Isolate dense FP32 hardware math from multi-gigabyte model transport",
    })


def export(data_dir, output, selected, packed=False):
    import torch

    if output.exists():
        raise ValueError("FP32 export output already exists")
    model = load_model(data_dir)
    output.mkdir(parents=True)
    builders = {
        "subsampling-first": lambda: subsampling(model, True, 32),
        "subsampling-steady": lambda: subsampling(model, False, 32),
        "encoder-risk": lambda: encoder(model, 32, 1, False),
        "encoder": lambda: encoder(model, 32),
        "prediction": lambda: prediction(model, 32),
        "joiner": lambda: joiner(model, 32),
    }
    graphs = []
    with torch.inference_mode():
        for name in selected.split(","):
            print("Exporting project FP32 HTP graph:", name, flush=True)
            graphs.append(convert(builders[name](), packed).write(output / (name + ".nmg")))
    write_json(output / "graphs.json", {
        "schema_version": 1, "package": PACKAGE, "graphs": graphs,
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "precision": "Project-owned FP32 HTP kernels; no CPU inference fallback and no changed reference",
        "static_weight_layout": "column-tiles-32" if packed else "row-major",
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    small = commands.add_parser("smoke")
    small.add_argument("--output", type=Path, required=True)
    full = commands.add_parser("export")
    full.add_argument("--output", type=Path, required=True)
    full.add_argument("--graphs", default="subsampling-first,subsampling-steady,encoder")
    full.add_argument("--pack-weights", action="store_true")
    check = commands.add_parser("cpu-check")
    check.add_argument("--library", type=Path, required=True)
    check.add_argument("--output", type=Path, required=True)
    check.add_argument("--steps", type=int, choices=range(1, 66), default=2)
    check.add_argument("--full", action="store_true")
    small_timing = commands.add_parser("micro")
    small_timing.add_argument("--output", type=Path, required=True)
    small_timing.add_argument("--pack-weights", action="store_true")
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "smoke":
        smoke(args.output)
    elif args.command == "micro":
        micro(args.output, args.pack_weights)
    elif args.command == "export":
        export(args.data_dir, args.output, args.graphs, args.pack_weights)
    else:
        cpu_check(args.data_dir, args.library, args.output, args.steps, args.full)


if __name__ == "__main__":
    main()
