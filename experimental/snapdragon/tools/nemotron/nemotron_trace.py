"""Diagnostic HTP intermediate capture; teacher-forced inputs never imply M1 acceptance."""

import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, write_json
from nemotron_m1 import DATA, HEADER, READ, STATIC, TENSOR, WRITE, Graph, encoder, fixture_write
from nemotron_m1 import packed_cache, read_tensor, subsampling, prediction


def load_model(data_dir):
    import torch
    from transformers import Nemotron3_5AsrForRNNT
    from nemotron_reference import environment
    from nemotron_m0 import read_catalog, verify_checkpoint
    from nemotron_m1 import CONTRACT_SHA

    catalog, _ = verify_checkpoint(data_dir / "checkpoint", ("transformers_weights",))
    accepted = load_json(data_dir / "m0-acceptance.json")
    if accepted["status"] != "m0_reference_accepted" or accepted["contract_sha256"] != CONTRACT_SHA or \
            accepted["model"] != catalog["model"] or \
            accepted["runs"][0]["manifest_sha256"] != digest(data_dir / "reference-1" / "manifest.json"):
        raise ValueError("Trace/calibration requires the unchanged accepted M0 provenance")
    environment(read_catalog())
    torch.set_num_threads(4)
    return Nemotron3_5AsrForRNNT.from_pretrained(
        data_dir / "checkpoint", local_files_only=True, dtype=torch.float32,
        attn_implementation="sdpa",
    ).eval()


def load_graph(path):
    import numpy as np
    from nemotron_m1 import NODE

    with Path(path).open("rb") as stream:
        header = stream.read(HEADER)
        tensors, nodes = struct.unpack_from("<2I", header, 52)
        precision = struct.unpack_from("<I", header, 48)[0]
        name = stream.read(64).split(b"\0")[0].decode("ascii")
        graph = Graph(name, precision)
        table = stream.read(tensors * TENSOR)
        node_table = stream.read(nodes * NODE)
        for index in range(tensors):
            record = table[index * TENSOR:(index + 1) * TENSOR]
            name = record[:64].split(b"\0")[0].decode("ascii")
            kind, dtype, rank, identity = struct.unpack_from("<4I", record, 64)
            shape = list(struct.unpack_from("<4I", record, 80)[:rank])
            offset, size = struct.unpack_from("<QQ", record, 96)
            item = {"name": name, "shape": shape, "type": kind, "dtype": dtype, "id": identity, "value": None}
            encoding = struct.unpack_from("<I", record, 116)[0]
            if encoding == 1:
                scale, zero_offset = struct.unpack_from("<fi", record, 120)
                item["quantization"] = {"scale": scale, "offset": zero_offset}
            elif encoding == 2:
                axis, count = struct.unpack_from("<2I", record, 120)
                scale_offset = struct.unpack_from("<Q", record, 128)[0]
                stream.seek(HEADER + scale_offset)
                scales = [struct.unpack("<fi", stream.read(8)) for _ in range(count)]
                if any(offset != 0 for _, offset in scales):
                    raise ValueError("Signed per-axis offsets must be zero")
                item["quantization"] = {"axis": axis, "scales": [scale for scale, _ in scales]}
            if kind == STATIC:
                types = {0x0216: "<f2", 0x0232: "<f4", 0x0416: "<u2", 0x0316: "<i2", 0x0332: "<i4",
                         0x0032: "<i4", 0x0132: "<u4", 0x0508: "u1"}
                stream.seek(HEADER + offset)
                item["value"] = np.frombuffer(stream.read(size), dtype=types[dtype]).copy().reshape(shape)
            graph.names[name] = index
            graph.tensors.append(item)
        for index in range(nodes):
            record = node_table[index * NODE:(index + 1) * NODE]
            name = record[:64].split(b"\0")[0].decode("ascii")
            operation = record[64:128].split(b"\0")[0].decode("ascii")
            ni, no, np_ = struct.unpack_from("<3I", record, 128)
            if no != 1:
                raise ValueError("Diagnostic interpreter requires single-output nodes")
            parameters = []
            for param in range(np_):
                start = 272 + param * 64
                label = record[start:start + 32].split(b"\0")[0].decode("ascii")
                kind, dtype, value = struct.unpack_from("<3I", record, start + 32)
                parameters.append({"name": label, "type": kind, "dtype": dtype, "value": value})
            graph.nodes.append({"name": name, "operation": operation,
                                "inputs": list(struct.unpack_from("<16I", record, 144)[:ni]),
                                "output": struct.unpack_from("<I", record, 208)[0], "parameters": parameters})
        return graph


def export(data_dir, output, component, precision, count, quant_graph=None):
    import torch

    if output.exists():
        raise ValueError("Trace output already exists")
    model = load_model(data_dir)
    directory = data_dir / "reference-1" / "long"
    manifest = load_json(directory / "manifest.json")
    records = {item["name"]: item for item in manifest["tensors"]}
    with torch.inference_mode():
        builder = (lambda p: encoder(model, p, 1, False)) if component == "encoder" else \
            (lambda p: subsampling(model, True, p))
        reference = builder(32)
        graph = load_graph(quant_graph) if quant_graph else builder(precision)
        names = [item["name"] for item in graph.tensors if item["type"] in (3, READ)]
        for name in names:
            graph.output(graph.names[name], name)
        output.mkdir(parents=True)
        metadata = graph.write(output / "trace.nmg")
        steps = []
        for index in range(count):
            prefix = f"chunk-{index:04d}"
            features = read_tensor(directory, records, prefix + "-features")
            if component == "encoder":
                hidden_capture = []
                hook = model.encoder.subsampling.register_forward_hook(lambda m, a, o: hidden_capture.append(o))
                try:
                    model.get_audio_features(features, prompt_ids=torch.tensor([9]),
                                             num_lookahead_tokens=3, use_cache=True)
                finally:
                    hook.remove()
                inputs = {"hidden": hidden_capture[0][0], "mask": torch.zeros((8, 4, 60))}
                inputs["mask"][:, :, :56] = -10000
                if index:
                    raise ValueError("Encoder trace currently accepts the first fresh chunk only")
                for item in graph.tensors:
                    if item["type"] == WRITE and item["name"] not in inputs:
                        inputs[item["name"]] = torch.zeros(item["shape"])
            else:
                inputs = {"features": features[0]}
                if index:
                    raise ValueError("First subsampling trace accepts only one chunk")
            expected = reference.evaluate(inputs, capture=True)
            if quant_graph:
                if "mask" in inputs and graph.tensors[graph.names["mask"]]["dtype"] == 0x0508:
                    inputs["mask"] = inputs["mask"] == 0
                ideal_quant = graph.evaluate(inputs, capture=True)
                expected = {**ideal_quant, **expected}
            cells = {(0, name): value for name, value in {**inputs, **expected}.items()}
            steps.append({"flags": 3, "cells": cells})
        fixture = fixture_write(output / "trace.nmf", [metadata], steps, component, precision)
        if quant_graph:
            steps[0]["cells"] = {(0, name): value for name, value in {**inputs, **ideal_quant}.items()}
            fixture_write(output / "ideal-quant.nmf", [metadata], steps, component, precision)
    write_json(output / "manifest.json", {
        "graph": metadata, "fixture": fixture, "component": component,
        "expected_backend": "FP32 direct-graph interpreter checked separately against M0",
        "teacher_forced": True, "acceptance": False,
    })


def read_outputs(path):
    import numpy as np

    with Path(path).open("rb") as stream:
        header = stream.read(HEADER)
        kind = struct.unpack_from("<I", header, 12)[0]
        count, _, steps = struct.unpack_from("<3I", header, 52)
        table = stream.read(64 + count * TENSOR)
        stride = (64 if kind == 4 else 16) + count * 32
        data = {}
        for step in range(steps):
            stream.seek(HEADER + 64 + count * TENSOR + step * stride + (64 if kind == 4 else 16))
            cells = stream.read(count * 32)
            for index in range(count):
                record = table[64 + index * TENSOR:64 + (index + 1) * TENSOR]
                name = record[:64].split(b"\0")[0].decode("ascii")
                mode, _, offset, size = struct.unpack_from("<IIQQ", cells, index * 32)
                if mode != DATA or struct.unpack_from("<I", record, 64)[0] != READ:
                    continue
                dtype = struct.unpack_from("<I", record, 68)[0]
                rank = struct.unpack_from("<I", record, 72)[0]
                shape = struct.unpack_from("<4I", record, 80)[:rank]
                kind_dtype = "<f4" if kind == 3 or dtype == 0x0232 else {
                    0x0216: "<f2", 0x0416: "<u2", 0x0316: "<i2", 0x0332: "<i4",
                }[dtype]
                stream.seek(HEADER + offset)
                value = np.frombuffer(stream.read(size), dtype=kind_dtype).astype("float64").reshape(shape)
                if kind == 4 and struct.unpack_from("<I", record, 116)[0] == 1:
                    scale, offset_value = struct.unpack_from("<fi", record, 120)
                    value = (value + offset_value) * scale
                data[(step, name)] = value
        return data


def compare(expected, actual, output):
    import numpy as np

    a, b = read_outputs(expected), read_outputs(actual)
    if set(a) != set(b):
        raise ValueError("Trace output names do not match")
    results = []
    for key in a:
        error = np.abs(a[key] - b[key])
        violations = (~np.isfinite(b[key])) | (error > 0.005 + 0.005 * np.abs(a[key]))
        item = {"step": key[0], "name": key[1], "max_abs": float(error.max()),
                "mean_abs": float(error.mean()), "violations": int(violations.sum()),
                "elements": int(error.size)}
        results.append(item)
        print(f"{item['name']}: max={item['max_abs']:.8g} mean={item['mean_abs']:.8g} violations={item['violations']}/{item['elements']}")
    write_json(output, {"teacher_forced_diagnostic": True, "tensors": results})


def shadow(data_dir, graph_path, result_path, output):
    import torch
    import numpy as np

    graph = load_graph(graph_path)
    native = read_outputs(result_path)
    overrides = {name: torch.from_numpy(value.astype(np.float32)) for (step, name), value in native.items()}
    model = load_model(data_dir)
    directory = data_dir / "reference-1" / "long"
    records = {item["name"]: item for item in load_json(directory / "manifest.json")["tensors"]}
    with torch.inference_mode():
        features = read_tensor(directory, records, "chunk-0000-features")
        capture = []
        hook = model.encoder.subsampling.register_forward_hook(lambda m, a, o: capture.append(o))
        try:
            model.get_audio_features(features, prompt_ids=torch.tensor([9]), num_lookahead_tokens=3, use_cache=True)
        finally:
            hook.remove()
        inputs = {"hidden": capture[0][0], "mask": torch.zeros((8, 4, 60))}
        inputs["mask"][:, :, :56] = -10000
        if graph.tensors[graph.names["mask"]]["dtype"] == 0x0508:
            inputs["mask"] = inputs["mask"] == 0
        for item in graph.tensors:
            if item["type"] == WRITE and item["name"] not in inputs:
                inputs[item["name"]] = torch.zeros(item["shape"])
        expected = graph.evaluate(inputs, capture=True, overrides=overrides)
    results = []
    for name, value in expected.items():
        reference = value.numpy().astype(np.float64)
        actual = native[(0, name)]
        error = np.abs(reference - actual)
        violations = error > 0.005 + 0.005 * np.abs(reference)
        item = {"name": name, "max_abs": float(error.max()), "violations": int(violations.sum())}
        results.append(item)
        print(f"{name}: local_operator_max={item['max_abs']:.8g}, violations={item['violations']}")
    write_json(output, {"teacher_forced_operator_inputs": True, "tensors": results})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("export")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--component", choices=("encoder", "subsampling"), default="encoder")
    create.add_argument("--precision", type=int, choices=(16, 32), default=32)
    create.add_argument("--quant-graph", type=Path)
    comparison = commands.add_parser("compare")
    comparison.add_argument("expected", type=Path)
    comparison.add_argument("actual", type=Path)
    comparison.add_argument("--output", type=Path, required=True)
    local = commands.add_parser("shadow")
    local.add_argument("--graph", type=Path, required=True)
    local.add_argument("--result", type=Path, required=True)
    local.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "export":
        export(args.data_dir, args.output, args.component, args.precision, 1, args.quant_graph)
    elif args.command == "compare":
        compare(args.expected, args.actual, args.output)
    else:
        shadow(args.data_dir, args.graph, args.result, args.output)


if __name__ == "__main__":
    main()
