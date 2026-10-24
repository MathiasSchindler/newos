"""Calibrate true 16-bit fixed-point HTP graphs against the immutable M0 traces."""

import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, write_json
from nemotron_m1 import BOOL, F16, F32, READ, STATIC, WRITE, S16, S32, U16
from nemotron_m1 import encoder, packed_cache, read_tensor, subsampling
from nemotron_trace import load_model


def limits(values):
    import numpy as np
    if hasattr(values, "detach"):
        values = values.detach().cpu().numpy()
    values = np.asarray(values)
    if not np.isfinite(values).all():
        raise ValueError("Nonfinite calibration tensor")
    return min(0.0, float(values.min())), max(0.0, float(values.max()))


def add_range(ranges, name, value):
    lower, upper = limits(value)
    old = ranges.get(name, (0.0, 0.0))
    ranges[name] = (min(lower, old[0]), max(upper, old[1]))


def quantize(graphs, statistics, margin=1.1, per_channel=False):
    import numpy as np

    identities = [(g, t) for g, graph in enumerate(graphs) for t, item in enumerate(graph.tensors)
                  if item["dtype"] in (F16, F32)]
    parents = {key: key for key in identities}
    def find(key):
        while parents[key] != key:
            parents[key] = parents[parents[key]]
            key = parents[key]
        return key
    def union(a, b):
        if a in parents and b in parents:
            parents[find(a)] = find(b)
    for g, graph in enumerate(graphs):
        for node in graph.nodes:
            if node["operation"] in ("Reshape", "Transpose", "StridedSlice", "Concat", "Gather", "Relu"):
                for source in node["inputs"][:1] if node["operation"] == "Gather" else node["inputs"]:
                    union((g, source), (g, node["output"]))
        for item in graph.tensors:
            if item["name"].endswith("-in") and item["name"][:-3] + "-out" in graph.names:
                union((g, graph.names[item["name"]]), (g, graph.names[item["name"][:-3] + "-out"]))
    if len(graphs) == 3:
        for cache in range(3):
            a, b = f"sub-{cache}-out", f"sub-{cache}-in"
            union((0, graphs[0].names[a]), (1, graphs[1].names[b]))
        for g in (0, 1):
            union((g, graphs[g].names["hidden"]), (2, graphs[2].names["hidden"]))
    ranges = {}
    for g, index in identities:
        item = graphs[g].tensors[index]
        bounds = limits(item["value"]) if item["type"] == STATIC else statistics[g][item["name"]]
        root = find((g, index))
        previous = ranges.get(root, (0.0, 0.0))
        ranges[root] = (min(previous[0], bounds[0]), max(previous[1], bounds[1]))
    encodings = {}
    for root, (minimum, maximum) in ranges.items():
        minimum, maximum = minimum * margin, maximum * margin
        scale = np.float32(max((maximum - minimum) / 65533, 1e-10))
        offset = int(np.floor(minimum / float(scale))) if minimum < 0 else 0
        if offset < -65535:
            raise ValueError("Invalid unsigned calibration offset")
        encodings[root] = {"scale": float(scale), "offset": offset}
    for g, index in identities:
        item = graphs[g].tensors[index]
        item["dtype"] = U16
        item["quantization"] = encodings[find((g, index))]
    for graph in graphs:
        for node in graph.nodes:
            if node["operation"] in ("MatMul", "FullyConnected", "Conv2d", "DepthWiseConv2d"):
                weight = graph.tensors[node["inputs"][1]]
                if weight["type"] == STATIC:
                    if per_channel:
                        axis = 0 if node["operation"] == "FullyConnected" else len(weight["shape"]) - 1
                        maximum = np.max(np.abs(weight["value"]), axis=tuple(i for i in range(len(weight["shape"])) if i != axis))
                        scale = np.maximum(maximum / 32767, 1e-10).astype(np.float32)
                        if len(node["inputs"]) == 3 and node["operation"] in ("Conv2d", "DepthWiseConv2d", "FullyConnected"):
                            bias = graph.tensors[node["inputs"][2]]["value"]
                            input_scale = graph.tensors[node["inputs"][0]]["quantization"]["scale"]
                            minimum = np.abs(bias) * 1.001 / (input_scale * 2147483647)
                            scale = np.maximum(scale, minimum).astype(np.float32)
                    else:
                        maximum = max(abs(value) for value in limits(weight["value"]))
                        scale = float(np.float32(max(maximum / 32767, 1e-10)))
                    weight["dtype"] = S16
                    weight["quantization"] = {"axis": axis, "scales": scale.tolist()} if per_channel else \
                        {"scale": scale, "offset": 0}
                    if node["operation"] in ("Conv2d", "DepthWiseConv2d", "FullyConnected") and len(node["inputs"]) == 3:
                        bias = graph.tensors[node["inputs"][2]]
                        scale = np.float32(graph.tensors[node["inputs"][0]]["quantization"]["scale"] * scale)
                        bias["dtype"] = S32
                        bias["quantization"] = {"axis": 0, "scales": scale.tolist()} if per_channel else \
                            {"scale": float(scale), "offset": 0}
    for graph in graphs:
        for item in graph.tensors:
            if item["value"] is None or "quantization" not in item:
                continue
            quant = item["quantization"]
            if "scales" in quant:
                shape = [1] * len(item["shape"])
                shape[quant["axis"]] = len(quant["scales"])
                raw = np.rint(item["value"] / np.asarray(quant["scales"], dtype=np.float32).reshape(shape))
            else:
                raw = np.rint(item["value"] / quant["scale"] - quant["offset"])
            dtype = {U16: "<u2", S16: "<i2", S32: "<i4"}[item["dtype"]]
            info = np.iinfo(dtype)
            if (raw < info.min).any() or (raw > info.max).any():
                raise ValueError(f"Static quantization overflow: {item['name']}")
            item["value"] = raw.astype(dtype)


def boolean_mask(graphs, statistics):
    import numpy as np

    for index, graph in enumerate(graphs):
        if "mask" not in graph.names:
            continue
        mask = graph.names["mask"]
        graph.tensors[mask]["dtype"] = BOOL
        for node in graph.nodes:
            if node["name"].endswith("-masked-scores"):
                scores = node["inputs"][0]
                bounds = statistics[index][graph.tensors[scores]["name"]]
                floor = np.float32(bounds[0] * 1.1 - 32)
                constant = graph.constant(node["name"] + "-floor", np.asarray([floor], dtype=np.float32))
                node["operation"] = "ElementWiseSelect"
                node["inputs"] = [mask, scores, constant]
                statistics[index][graph.tensors[node["output"]]["name"]] = (float(floor), bounds[1])


def float_norms(graph):
    import numpy as np

    nodes, replacements = [], {}
    for node in graph.nodes:
        node["inputs"] = [replacements.get(index, index) for index in node["inputs"]]
        if node["operation"] != "LayerNorm":
            nodes.append(node)
            continue
        original = graph.tensors[node["output"]]
        encoding = original.pop("quantization")
        original["dtype"] = F32
        source = node["inputs"][0]
        dequantized = graph.op(node["name"] + "-input-float", "Dequantize", [source],
                              graph.tensors[source]["shape"])
        graph.tensors[dequantized]["dtype"] = F32
        nodes.append(graph.nodes.pop())
        node["inputs"][0] = dequantized
        for index in node["inputs"][1:]:
            item = graph.tensors[index]
            quant = item.pop("quantization")
            item["value"] = ((item["value"].astype(np.float32) + quant["offset"]) * quant["scale"]).astype("<f4")
            item["dtype"] = F32
        nodes.append(node)
        if any(node["output"] in consumer["inputs"] for consumer in graph.nodes):
            quantized = graph.op(node["name"] + "-output-fixed", "Quantize", [node["output"]], original["shape"])
            graph.tensors[quantized]["dtype"] = U16
            graph.tensors[quantized]["quantization"] = encoding
            nodes.append(graph.nodes.pop())
            replacements[node["output"]] = quantized
    graph.nodes = nodes


def hybrid(source, output):
    from nemotron_trace import load_graph

    if output.exists():
        raise ValueError("Hybrid output already exists")
    manifest = load_json(source / "graphs.json")
    output.mkdir(parents=True)
    records = []
    for descriptor in manifest["graphs"]:
        graph = load_graph(source / descriptor["file"])
        float_norms(graph)
        records.append(graph.write(output / descriptor["file"]))
    write_json(output / "graphs.json", {
        **manifest, "graphs": records, "source_graph_manifest_sha256": digest(source / "graphs.json"),
        "normalization": "Explicit HTP Dequantize -> float LayerNorm -> Quantize; matrix/convolution/attention remain fixed-point",
    })


def export(data_dir, output, fixtures, risk=False, per_channel=False, mixed_norm=False):
    import torch
    from transformers.models.nemotron_asr_streaming.modeling_nemotron_asr_streaming import (
        NemotronAsrStreamingEncoderCausalConvPaddingCache,
    )

    if output.exists():
        raise ValueError("Quantized graph output already exists")
    model = load_model(data_dir)
    graphs = [encoder(model, 32, 1, False)] if risk else \
        [subsampling(model, True, 32), subsampling(model, False, 32), encoder(model, 32)]
    statistics = [{} for graph in graphs]
    processed = []
    with torch.inference_mode():
        for fixture in fixtures.split(","):
            directory = data_dir / "reference-1" / fixture
            manifest = load_json(directory / "manifest.json")
            records = {item["name"]: item for item in manifest["tensors"]}
            padding = NemotronAsrStreamingEncoderCausalConvPaddingCache()
            for step in manifest["trace"]["steps"]:
                index = step["index"]
                prefix = f"chunk-{index:04d}"
                features = read_tensor(directory, records, prefix + "-features")
                hidden = model.encoder.subsampling(features, padding_cache=padding)[0]
                if not risk:
                    front_index = 0 if index == 0 else 1
                    inputs = {"features": features[0]}
                    for cache in range(3):
                        if index:
                            cached = read_tensor(directory, records, f"chunk-{index - 1:04d}-output-convolution-subsampling-{cache}")
                            inputs[f"sub-{cache}-in"] = cached.permute(0, 2, 3, 1)
                    values = graphs[front_index].evaluate(inputs, capture=True)
                    for name, value in {**inputs, **values}.items():
                        add_range(statistics[front_index], name, value)
                core_index = 0 if risk else 2
                inputs = {"hidden": hidden, "mask": torch.zeros((8, 4, 60))}
                inputs["mask"][:, :, :56 - min(56, index * 4)] = -10000
                for layer in range(1 if risk else 24):
                    label = f"layer-{layer:02d}"
                    for suffix, original in (("key", "keys"), ("value", "values"), ("conv", None)):
                        if index:
                            source = f"chunk-{index - 1:04d}-output-attention-{layer}-{original}" if original else \
                                f"chunk-{index - 1:04d}-output-convolution-conv-{layer}"
                            value = packed_cache(read_tensor(directory, records, source), original is not None)
                        else:
                            value = torch.zeros((8, 56, 128) if original else (8, 1024))
                        inputs[label + "-" + suffix + "-in"] = value
                values = graphs[core_index].evaluate(inputs, capture=True)
                for name, value in {**inputs, **values}.items():
                    add_range(statistics[core_index], name, value)
                if not risk:
                    expected = read_tensor(directory, records, prefix + "-encoder-raw")[0]
                    torch.testing.assert_close(values["raw"], expected, atol=0.005, rtol=0.005)
                print(f"Calibrated {fixture} {index + 1}/{len(manifest['trace']['steps'])}", flush=True)
            processed.append({"fixture": fixture, "manifest_sha256": digest(directory / "manifest.json"),
                              "steps": len(manifest["trace"]["steps"])})
    boolean_mask(graphs, statistics)
    quantize(graphs, statistics, per_channel=per_channel)
    if mixed_norm:
        for graph in graphs:
            float_norms(graph)
    output.mkdir(parents=True)
    metadata = []
    with torch.inference_mode():
        for graph, file in zip(graphs, ("encoder-risk.nmg",) if risk else
                               ("subsampling-first.nmg", "subsampling-steady.nmg", "encoder.nmg")):
            metadata.append(graph.write(output / file))
    write_json(output / "graphs.json", {
        "schema_version": 1, "graphs": metadata, "quantization": "U16 activations, S16 weights, S32 convolution biases",
        "calibration": processed, "margin": 1.1, "tolerances_unchanged": True,
        "per_channel_weights": per_channel, "mixed_norm": mixed_norm,
        "attention_mask": "Boolean select; masked floor below calibrated minimum by32 to avoid sacrificing score resolution to-10000",
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
    })
    write_json(output / "calibration.json", {"statistics": statistics, "inputs": processed})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixtures", default="long,tail,silence")
    parser.add_argument("--risk", action="store_true")
    parser.add_argument("--hybrid-norm-source", type=Path)
    parser.add_argument("--per-channel", action="store_true")
    parser.add_argument("--mixed-norm", action="store_true")
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.hybrid_norm_source:
        hybrid(args.hybrid_norm_source, args.output)
    else:
        export(args.data_dir, args.output, args.fixtures, args.risk, args.per_channel, args.mixed_norm)


if __name__ == "__main__":
    main()
