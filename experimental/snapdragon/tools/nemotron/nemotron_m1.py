"""Direct QNN graph export and fixed-cache M1 fixture preparation (development only)."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m0 import DEFAULT_DATA, digest, load_json, read_catalog, verify_file, write_json


F16, F32, I32, U32, BOOL = 0x0216, 0x0232, 0x0032, 0x0132, 0x0508
S16, S32, U16 = 0x0316, 0x0332, 0x0416
WRITE, READ, NATIVE, STATIC = 0, 1, 3, 4
HEADER, TENSOR, NODE, CELL = 256, 160, 672, 32
MAGIC = b"NM1PKG01"
RUNTIME_SHA = "99ff3a16167451fc5ebe03781260b2b64c37e4042414999ebd4805d6a1683a23"
CONTRACT_SHA = "5da29efe47e398dc06edafb467527bb80612fdbff89e3f64615cab73e7583bd6"
DATA, CARRY, ZERO = 0, 1, 2


def align(value):
    return (value + 15) & ~15


def name_bytes(name, capacity=64):
    value = name.encode("ascii")
    if not value or len(value) >= capacity or b"\0" in value:
        raise ValueError(f"Invalid package name: {name}")
    return value + bytes(capacity - len(value))


def header(kind, precision, tensors, nodes, steps, size, sha, version=1):
    catalog = read_catalog()
    model = catalog["model"]
    result = bytearray(HEADER)
    result[:8] = MAGIC
    struct.pack_into("<14I", result, 8, version, kind, HEADER, 2, 39, 2, 18, 73, 9, 320,
                     precision, tensors, nodes, steps)
    struct.pack_into("<Q", result, 64, size)
    result[72:104] = hashlib.sha256((model["repository"] + "@" + model["revision"]).encode()).digest()
    result[104:136] = bytes.fromhex(CONTRACT_SHA)
    result[136:168] = sha
    result[168:200] = bytes.fromhex(RUNTIME_SHA)
    return result


def tensor_record(tensor, offset=0, size=0, graph=0, scale_offset=0):
    result = bytearray(TENSOR)
    result[:64] = name_bytes(tensor["name"])
    shape = tensor["shape"]
    if not 1 <= len(shape) <= 4 or any(type(x) is not int or x <= 0 for x in shape):
        raise ValueError("Invalid tensor dimensions")
    struct.pack_into("<4I", result, 64, tensor["type"], tensor["dtype"], len(shape), tensor["id"])
    struct.pack_into("<4I", result, 80, *(shape + [0] * (4 - len(shape))))
    struct.pack_into("<QQI", result, 96, offset, size, graph)
    if "quantization" in tensor:
        quant = tensor["quantization"]
        if "scales" in quant:
            axis, scales = quant["axis"], quant["scales"]
            if tensor["dtype"] not in (S16, S32) or tensor["type"] != STATIC or \
                    not 0 <= axis < len(shape) or len(scales) != shape[axis] or \
                    any(not math.isfinite(value) or value <= 0 for value in scales):
                raise ValueError("Invalid per-axis signed quantized tensor")
            struct.pack_into("<IIIQ", result, 116, 2, axis, len(scales), scale_offset)
        else:
            if tensor["dtype"] not in (U16, S16, S32) or not math.isfinite(quant["scale"]) or quant["scale"] <= 0:
                raise ValueError("Invalid quantized tensor")
            struct.pack_into("<Ifi", result, 116, 1, quant["scale"], quant["offset"])
    return result


def finish_package(stream, kind, precision, tensors, nodes, steps, version=1):
    end = stream.tell()
    stream.seek(HEADER)
    value = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        value.update(block)
    stream.seek(0)
    stream.write(header(kind, precision, tensors, nodes, steps, end - HEADER, value.digest(), version))


class Graph:
    def __init__(self, name, precision=32, fully_connected=False, centered_norm=False):
        self.name, self.precision = name, precision
        self.fully_connected = fully_connected
        self.centered_norm = centered_norm
        self.tensors, self.nodes, self.names = [], [], {}

    def tensor(self, name, shape, kind=NATIVE, value=None, dtype=None):
        import numpy as np

        if name in self.names:
            raise ValueError(f"Duplicate tensor: {name}")
        name_bytes(name)
        dtype = dtype if dtype is not None else F16 if self.precision == 16 else F32
        if value is not None:
            if hasattr(value, "detach"):
                value = value.detach().cpu().numpy()
            types = {F16: "<f2", F32: "<f4", I32: "<i4", U32: "<u4", BOOL: "u1"}
            value = np.asarray(value).astype(types[dtype]).reshape(shape).copy()
            if value.dtype.kind == "f" and not np.isfinite(value).all():
                raise ValueError(f"Nonfinite or overflowing constant: {name}")
        index = len(self.tensors)
        entry = {"name": name, "shape": list(shape), "type": kind, "dtype": dtype,
                 "id": index + 1, "value": value}
        tensor_record(entry)
        self.names[name] = index
        self.tensors.append(entry)
        return index

    def input(self, name, shape, dtype=None):
        return self.tensor(name, shape, WRITE, dtype=dtype)

    def constant(self, name, value, dtype=None):
        return self.tensor(name, list(value.shape), STATIC, value, dtype)

    def output(self, index, name):
        if name in self.names and self.names[name] != index:
            raise ValueError("Output name collision")
        entry = self.tensors[index]
        self.names.pop(entry["name"])
        entry["name"], entry["type"] = name, READ
        self.names[name] = index
        return index

    def scalar(self, name, value, dtype=U32):
        bits = struct.unpack("<I", struct.pack("<f", value))[0] if dtype == F32 else int(value) & 0xFFFFFFFF
        return {"name": name, "type": 0, "dtype": dtype, "value": bits}

    def parameter(self, name, value, dtype=U32):
        import numpy as np

        index = self.constant(f"param-{len(self.tensors)}", np.asarray(value), dtype)
        return {"name": name, "type": 1, "dtype": dtype, "value": index}

    def op(self, name, operation, inputs, shape, parameters=()):
        index = self.tensor(name, shape)
        self.nodes.append({"name": name, "operation": operation, "inputs": list(inputs),
                           "output": index, "parameters": list(parameters)})
        return index

    def unary(self, name, operation, source):
        return self.op(name, operation, [source], self.tensors[source]["shape"])

    def reshape(self, name, source, shape):
        if math.prod(shape) != math.prod(self.tensors[source]["shape"]):
            raise ValueError("Reshape changes element count")
        return self.op(name, "Reshape", [source], shape)

    def transpose(self, name, source, permutation):
        shape = self.tensors[source]["shape"]
        return self.op(name, "Transpose", [source], [shape[i] for i in permutation],
                       [self.parameter("perm", permutation)])

    def slice(self, name, source, axis, start, end):
        shape = list(self.tensors[source]["shape"])
        ranges = [[0, dimension, 1] for dimension in shape]
        if not 0 <= start < end <= shape[axis]:
            raise ValueError("Invalid slice")
        ranges[axis] = [start, end, 1]
        shape[axis] = end - start
        return self.op(name, "StridedSlice", [source], shape, [self.parameter("ranges", ranges, I32)])

    def concat(self, name, inputs, axis):
        shape = list(self.tensors[inputs[0]]["shape"])
        shape[axis] = sum(self.tensors[index]["shape"][axis] for index in inputs)
        return self.op(name, "Concat", inputs, shape, [self.scalar("axis", axis)])

    def binary(self, name, operation, left, right):
        a, b = self.tensors[left]["shape"], self.tensors[right]["shape"]
        shape = []
        for x, y in zip([1] * (max(len(a), len(b)) - len(a)) + a,
                        [1] * (max(len(a), len(b)) - len(b)) + b):
            if x != y and x != 1 and y != 1:
                raise ValueError("Incompatible broadcast")
            shape.append(max(x, y))
        return self.op(name, operation, [left, right], shape)

    def matmul(self, name, left, right):
        a, b = self.tensors[left]["shape"], self.tensors[right]["shape"]
        if a[-1] != b[-2] or a[:-2] != b[:-2]:
            raise ValueError("Invalid fixed MatMul")
        return self.op(name, "MatMul", [left, right], a[:-2] + [a[-2], b[-1]],
                       [self.scalar("transpose_in0", False, BOOL), self.scalar("transpose_in1", False, BOOL)])

    def linear(self, name, source, module, weight=None, bias=None):
        weight = module.weight if weight is None else weight
        bias = module.bias if bias is None else bias
        if weight.ndim == 3:
            weight = weight[:, :, 0]
        if self.fully_connected:
            inputs = [source, self.constant(name + "-weight", weight)]
            if bias is not None:
                inputs.append(self.constant(name + "-bias", bias))
            shape = self.tensors[source]["shape"][:-1] + [weight.shape[0]]
            return self.op(name + "-fully-connected", "FullyConnected", inputs, shape)
        matrix = self.constant(name + "-weight", weight.T)
        result = self.matmul(name + "-matmul", source, matrix)
        if bias is not None:
            result = self.binary(name + "-bias-add", "ElementWiseAdd", result,
                                 self.constant(name + "-bias", bias))
        return result

    def norm(self, name, source, module):
        import numpy as np

        if self.centered_norm:
            shape = self.tensors[source]["shape"]
            reduced = shape[:-1] + [1]
            parameters = [self.parameter("axes", [len(shape) - 1]), self.scalar("keep_dims", True, BOOL)]
            mean = self.op(name + "-mean", "ReduceMean", [source], reduced, parameters)
            centered = self.binary(name + "-centered", "ElementWiseSubtract", source, mean)
            square = self.binary(name + "-square", "ElementWiseMultiply", centered, centered)
            parameters = [self.parameter("axes", [len(shape) - 1]), self.scalar("keep_dims", True, BOOL)]
            variance = self.op(name + "-variance", "ReduceMean", [square], reduced, parameters)
            epsilon = self.constant(name + "-epsilon", np.asarray([module.eps], dtype=np.float32))
            variance = self.binary(name + "-with-epsilon", "ElementWiseAdd", variance, epsilon)
            inverse = self.unary(name + "-rsqrt", "ElementWiseRsqrt", variance)
            value = self.binary(name + "-normalized", "ElementWiseMultiply", centered, inverse)
            value = self.binary(name + "-scaled", "ElementWiseMultiply", value,
                                self.constant(name + "-scale", module.weight))
            return self.binary(name + "-shifted", "ElementWiseAdd", value,
                               self.constant(name + "-bias", module.bias))
        inputs = [source, self.constant(name + "-scale", module.weight),
                  self.constant(name + "-bias", module.bias)]
        axis = len(self.tensors[source]["shape"]) - 1
        return self.op(name, "LayerNorm", inputs, self.tensors[source]["shape"],
                       [self.scalar("epsilon", module.eps, F32), self.parameter("axes", [axis])])

    def silu(self, name, source):
        return self.binary(name, "ElementWiseMultiply", source, self.unary(name + "-sigmoid", "Sigmoid", source))

    def scaled(self, name, source, scale):
        import numpy as np
        return self.binary(name, "ElementWiseMultiply", source,
                           self.constant(name + "-factor", np.asarray([scale], dtype=np.float32)))

    def pad(self, name, source, pads):
        shape = [size + sum(pad) for size, pad in zip(self.tensors[source]["shape"], pads)]
        return self.op(name, "Pad", [source], shape,
                       [self.scalar("scheme", 0), self.parameter("pad_amount", pads),
                        self.scalar("pad_constant_value", 0.0, F32)])

    def conv(self, name, source, module, depthwise=False):
        weight = module.weight
        if weight.ndim == 3:
            weight = weight.unsqueeze(-1)
        weight = weight.permute(2, 3, 1, 0)
        inputs = [source, self.constant(name + "-weight", weight)]
        if module.bias is not None:
            inputs.append(self.constant(name + "-bias", module.bias))
        shape = self.tensors[source]["shape"]
        stride = list(module.stride)
        dilation = list(module.dilation)
        if len(stride) == 1:
            stride.append(1)
            dilation.append(1)
        kernel = list(weight.shape[:2])
        out = [shape[0], (shape[1] - dilation[0] * (kernel[0] - 1) - 1) // stride[0] + 1,
               (shape[2] - dilation[1] * (kernel[1] - 1) - 1) // stride[1] + 1,
               module.out_channels]
        return self.op(name, "DepthWiseConv2d" if depthwise else "Conv2d", inputs, out,
                       [self.parameter("stride", stride), self.parameter("dilation", dilation),
                        self.parameter("pad_amount", [[0, 0], [0, 0]])])

    def write(self, path):
        path = Path(path)
        if path.exists():
            raise ValueError("Graph artifact already exists")
        path.parent.mkdir(parents=True, exist_ok=True)
        metadata_size = align(64 + len(self.tensors) * TENSOR + len(self.nodes) * NODE)
        cursor = metadata_size
        records, scales = [], []
        for entry in self.tensors:
            quant = entry.get("quantization", {})
            table = b"".join(struct.pack("<fi", value, 0) for value in quant.get("scales", []))
            scales.append(table)
            scale_offset = cursor if table else 0
            cursor = align(cursor + len(table))
            value = entry["value"]
            records.append(tensor_record(entry, cursor if value is not None else 0,
                                         value.nbytes if value is not None else 0, scale_offset=scale_offset))
            if value is not None:
                cursor = align(cursor + value.nbytes)
        with path.open("x+b") as stream:
            stream.write(bytes(HEADER))
            stream.write(name_bytes(self.name))
            for record in records:
                stream.write(record)
            for node in self.nodes:
                record = bytearray(NODE)
                record[:64], record[64:128] = name_bytes(node["name"]), name_bytes(node["operation"])
                struct.pack_into("<4I", record, 128, len(node["inputs"]), 1, len(node["parameters"]), 0)
                struct.pack_into("<16I", record, 144, *(node["inputs"] + [0] * (16 - len(node["inputs"]))))
                struct.pack_into("<16I", record, 208, node["output"], *([0] * 15))
                for index, parameter in enumerate(node["parameters"]):
                    offset = 272 + index * 64
                    record[offset:offset + 32] = name_bytes(parameter["name"], 32)
                    struct.pack_into("<3I", record, offset + 32, parameter["type"],
                                     parameter["dtype"], parameter["value"])
                stream.write(record)
            stream.write(bytes(metadata_size - (stream.tell() - HEADER)))
            for entry, table in zip(self.tensors, scales):
                if table:
                    stream.write(table)
                    stream.write(bytes(align(stream.tell() - HEADER) - (stream.tell() - HEADER)))
                if entry["value"] is not None:
                    stream.write(entry["value"].tobytes())
                    stream.write(bytes(align(stream.tell() - HEADER) - (stream.tell() - HEADER)))
            finish_package(stream, 1, self.precision, len(self.tensors), len(self.nodes), 0,
                           2 if any("quantization" in item for item in self.tensors) else 1)
        io = [{key: value for key, value in item.items() if key != "value"}
              for item in self.tensors if item["type"] in (WRITE, READ)]
        return {"name": self.name, "file": path.name, "sha256": digest(path), "size": path.stat().st_size,
                "precision": self.precision, "tensors": len(self.tensors), "nodes": len(self.nodes), "io": io,
                "operators": sorted({node["operation"] for node in self.nodes})}

    def evaluate(self, inputs, capture=False, overrides=None):
        import numpy as np
        import torch
        from torch.nn import functional as functional

        values, computed = {}, {}
        def stored(value, item):
            if "quantization" not in item:
                return value.to(torch.float16).to(torch.float32) if item["dtype"] == F16 else value
            quant = item["quantization"]
            lower, upper = {U16: (0, 65535), S16: (-32768, 32767), S32: (-2147483648, 2147483647)}[item["dtype"]]
            raw = torch.round(value / quant["scale"] - quant["offset"])
            if not torch.isfinite(raw).all() or bool(((raw < lower) | (raw > upper)).any()):
                raise ValueError(f"Interpreter exceeds calibrated range: {item['name']}")
            return (raw + quant["offset"]) * quant["scale"]
        for index, item in enumerate(self.tensors):
            if item["type"] == STATIC:
                array = item["value"]
                values[index] = torch.from_numpy(array.astype(np.float32, copy=False) if array.dtype.kind == "f"
                                                 else array.astype(np.int64))
                if "quantization" in item:
                    quant = item["quantization"]
                    if "scales" in quant:
                        shape = [1] * len(item["shape"])
                        shape[quant["axis"]] = len(quant["scales"])
                        scales = torch.tensor(quant["scales"], dtype=torch.float32).reshape(shape)
                        values[index] = values[index].to(torch.float32) * scales
                    else:
                        values[index] = (values[index].to(torch.float32) + quant["offset"]) * quant["scale"]
            elif item["type"] == WRITE:
                value = inputs[item["name"]]
                values[index] = stored(value.to(torch.float32), item)
        for node in self.nodes:
            args = [values[index] for index in node["inputs"]]
            parameters = {}
            for item in node["parameters"]:
                if item["type"] == 1:
                    parameters[item["name"]] = values[item["value"]].tolist()
                elif item["dtype"] == F32:
                    parameters[item["name"]] = struct.unpack("<f", struct.pack("<I", item["value"]))[0]
                else:
                    parameters[item["name"]] = item["value"]
            operation = node["operation"]
            if operation == "MatMul":
                result = args[0] @ args[1]
            elif operation == "FullyConnected":
                result = functional.linear(args[0], args[1], args[2] if len(args) == 3 else None)
            elif operation == "ElementWiseAdd":
                result = args[0] + args[1]
            elif operation == "ElementWiseSubtract":
                result = args[0] - args[1]
            elif operation == "ElementWiseMultiply":
                result = args[0] * args[1]
            elif operation == "ElementWiseSelect":
                result = torch.where(args[0].bool(), args[1], args[2])
            elif operation in ("Quantize", "Dequantize"):
                result = args[0]
            elif operation == "Reshape":
                result = args[0].reshape(self.tensors[node["output"]]["shape"])
            elif operation == "Transpose":
                result = args[0].permute(parameters["perm"])
            elif operation == "StridedSlice":
                result = args[0][tuple(slice(*interval) for interval in parameters["ranges"])]
            elif operation == "Concat":
                result = torch.cat(args, dim=parameters["axis"])
            elif operation == "LayerNorm":
                result = functional.layer_norm(args[0], (args[0].shape[-1],), args[1], args[2],
                                               parameters["epsilon"])
            elif operation == "ReduceMean":
                result = args[0].mean(dim=tuple(parameters["axes"]), keepdim=bool(parameters["keep_dims"]))
            elif operation == "ElementWiseRsqrt":
                result = args[0].rsqrt()
            elif operation == "Sigmoid":
                result = args[0].sigmoid()
            elif operation == "Tanh":
                result = args[0].tanh()
            elif operation == "Relu":
                result = args[0].relu()
            elif operation == "Softmax":
                result = args[0].softmax(dim=parameters["axis"])
            elif operation == "Pad":
                pads = [number for pair in reversed(parameters["pad_amount"]) for number in pair]
                result = functional.pad(args[0], pads)
            elif operation in ("Conv2d", "DepthWiseConv2d"):
                weight = args[1]
                groups = args[0].shape[-1] if operation == "DepthWiseConv2d" else 1
                weight = weight.permute(3, 2, 0, 1)
                result = functional.conv2d(args[0].permute(0, 3, 1, 2), weight,
                                           args[2] if len(args) == 3 else None,
                                           stride=parameters["stride"], dilation=parameters["dilation"],
                                           groups=groups).permute(0, 2, 3, 1)
            elif operation == "Gather":
                result = args[0][args[1].to(torch.long)]
            else:
                raise ValueError(f"Unsupported graph operation: {operation}")
            item = self.tensors[node["output"]]
            result = stored(result, item)
            computed[node["output"]] = result
            values[node["output"]] = overrides[item["name"]].to(torch.float32) \
                if overrides is not None and item["name"] in overrides else result
        return {item["name"]: computed[index] for index, item in enumerate(self.tensors)
                if item["type"] == READ or (capture and item["type"] == NATIVE)}


def subsampling(model, first, precision, fully_connected=False):
    graph = Graph("nemotron_subsampling_first" if first else "nemotron_subsampling_steady",
                  precision, fully_connected)
    x = graph.input("features", [25 if first else 32, 128])
    x = graph.reshape("features-nhwc", x, [1, 25 if first else 32, 128, 1])
    module = model.encoder.subsampling
    for index in range(3):
        conv = module.conv_in if index == 0 else module.layers[index - 1].depthwise_conv
        x = graph.pad(f"sub-{index}-frequency-pad", x, [[0, 0], [0, 0], [2, 1], [0, 0]])
        shape = graph.tensors[x]["shape"]
        cache_shape = [1, 1, shape[2], shape[3]]
        graph.output(graph.slice(f"sub-{index}-cache-out", x, 1, shape[1] - 1, shape[1]), f"sub-{index}-out")
        if first:
            x = graph.pad(f"sub-{index}-initial-time-pad", x, [[0, 0], [2, 0], [0, 0], [0, 0]])
        else:
            cache = graph.input(f"sub-{index}-in", cache_shape)
            x = graph.concat(f"sub-{index}-time-cache", [cache, x], 1)
        x = graph.conv(f"sub-{index}-convolution", x, conv, index != 0)
        if index != 0:
            x = graph.conv(f"sub-{index}-pointwise", x, module.layers[index - 1].pointwise_conv)
        x = graph.unary(f"sub-{index}-relu", "Relu", x)
    x = graph.transpose("sub-flatten-permutation", x, [0, 1, 3, 2])
    x = graph.reshape("sub-flatten", x, [4, module.linear.in_features])
    x = graph.linear("sub-linear", x, module.linear)
    graph.output(x, "hidden")
    return graph


def encoder(model, precision, layers=24, conditioning=True, fully_connected=False, centered_norm=False):
    import torch

    graph = Graph("nemotron_encoder" if layers == 24 else "nemotron_encoder_risk",
                  precision, fully_connected, centered_norm)
    hidden = graph.input("hidden", [4, 1024])
    mask = graph.input("mask", [8, 4, 60])
    positions = model.encoder.encode_positions(torch.zeros((1, 4, 1024)), cached_frames=56)[:, :63]
    for index, block in enumerate(model.encoder.layers[:layers]):
        prefix = f"layer-{index:02d}"
        def feed_forward(source, module, label):
            source = graph.linear(label + "-linear1", source, module.linear1)
            source = graph.silu(label + "-silu", source)
            source = graph.linear(label + "-linear2", source, module.linear2)
            return graph.scaled(label + "-half", source, 0.5)
        ff = feed_forward(graph.norm(prefix + "-norm-ff1", hidden, block.norm_feed_forward1),
                          block.feed_forward1, prefix + "-ff1")
        hidden = graph.binary(prefix + "-residual-ff1", "ElementWiseAdd", hidden, ff)
        normalized = graph.norm(prefix + "-norm-att", hidden, block.norm_self_att)
        projections = []
        for label, module in (("q", block.self_attn.q_proj), ("k", block.self_attn.k_proj),
                              ("v", block.self_attn.v_proj)):
            value = graph.linear(prefix + "-" + label, normalized, module)
            value = graph.reshape(prefix + "-" + label + "-heads", value, [4, 8, 128])
            projections.append(graph.transpose(prefix + "-" + label + "-transpose", value, [1, 0, 2]))
        query, key, value = projections
        states = []
        for label, current in (("key", key), ("value", value)):
            past = graph.input(prefix + "-" + label + "-in", [8, 56, 128])
            joined = graph.concat(prefix + "-" + label + "-joined", [past, current], 1)
            graph.output(graph.slice(prefix + "-" + label + "-tail", joined, 1, 4, 60),
                         prefix + "-" + label + "-out")
            states.append(joined)
        key, value = states
        biased = []
        for label, bias in (("u", block.self_attn.bias_u), ("v", block.self_attn.bias_v)):
            constant = graph.constant(prefix + "-bias-" + label, bias[:, None, :])
            biased.append(graph.binary(prefix + "-query-" + label, "ElementWiseAdd", query, constant))
        key_t = graph.transpose(prefix + "-key-transpose", key, [0, 2, 1])
        content = graph.matmul(prefix + "-content", biased[0], key_t)
        content = graph.scaled(prefix + "-content-scale", content, 128 ** -0.5)
        relative = block.self_attn.relative_k_proj(positions).reshape(63, 8, 128).permute(1, 2, 0)
        relative = graph.constant(prefix + "-folded-relative", relative)
        scores = graph.matmul(prefix + "-relative", biased[1], relative)
        rows = []
        for query_index in range(4):
            row = graph.slice(prefix + f"-relative-row-{query_index}", scores, 1, query_index, query_index + 1)
            row = graph.slice(prefix + f"-relative-shift-{query_index}", row, 2, 3 - query_index, 63 - query_index)
            rows.append(row)
        scores = graph.concat(prefix + "-relative-shifted", rows, 1)
        scores = graph.scaled(prefix + "-relative-scale", scores, 128 ** -0.5)
        scores = graph.binary(prefix + "-scores", "ElementWiseAdd", content, scores)
        scores = graph.binary(prefix + "-masked-scores", "ElementWiseAdd", scores, mask)
        scores = graph.op(prefix + "-softmax", "Softmax", [scores], [8, 4, 60],
                          [graph.scalar("axis", 2), graph.scalar("beta", 1.0, F32)])
        attention = graph.matmul(prefix + "-weighted-values", scores, value)
        attention = graph.transpose(prefix + "-attention-transpose", attention, [1, 0, 2])
        attention = graph.reshape(prefix + "-attention-flat", attention, [4, 1024])
        attention = graph.linear(prefix + "-attention-project", attention, block.self_attn.o_proj)
        hidden = graph.binary(prefix + "-residual-attention", "ElementWiseAdd", hidden, attention)
        conv = graph.norm(prefix + "-norm-conv", hidden, block.norm_conv)
        conv = graph.linear(prefix + "-conv-pointwise1", conv, block.conv.pointwise_conv1)
        left = graph.slice(prefix + "-glu-value", conv, 1, 0, 1024)
        right = graph.slice(prefix + "-glu-gate", conv, 1, 1024, 2048)
        right = graph.unary(prefix + "-glu-sigmoid", "Sigmoid", right)
        conv = graph.binary(prefix + "-glu", "ElementWiseMultiply", left, right)
        past = graph.input(prefix + "-conv-in", [8, 1024])
        conv = graph.concat(prefix + "-conv-cache", [past, conv], 0)
        graph.output(graph.slice(prefix + "-conv-tail", conv, 0, 4, 12), prefix + "-conv-out")
        conv = graph.reshape(prefix + "-conv-nhwc", conv, [1, 12, 1, 1024])
        conv = graph.conv(prefix + "-depthwise", conv, block.conv.depthwise_conv, True)
        conv = graph.reshape(prefix + "-conv-flat", conv, [4, 1024])
        conv = graph.norm(prefix + "-conv-layer-norm", conv, block.conv.norm)
        conv = graph.silu(prefix + "-conv-silu", conv)
        conv = graph.linear(prefix + "-conv-pointwise2", conv, block.conv.pointwise_conv2)
        hidden = graph.binary(prefix + "-residual-conv", "ElementWiseAdd", hidden, conv)
        ff = feed_forward(graph.norm(prefix + "-norm-ff2", hidden, block.norm_feed_forward2),
                          block.feed_forward2, prefix + "-ff2")
        hidden = graph.binary(prefix + "-residual-ff2", "ElementWiseAdd", hidden, ff)
        hidden = graph.norm(prefix + "-norm-out", hidden, block.norm_out)
    graph.output(hidden, "raw")
    if conditioning:
        module = model.prompt_projector
        bias = module.linear_1.bias + module.linear_1.weight[:, 1024 + 9]
        hidden = graph.linear("prompt-linear1", hidden, module.linear_1,
                              module.linear_1.weight[:, :1024], bias)
        hidden = graph.unary("prompt-relu", "Relu", hidden)
        hidden = graph.linear("prompt-linear2", hidden, module.linear_2)
        graph.output(hidden, "conditioned")
        hidden = graph.linear("encoder-projector", hidden, model.encoder_projector)
        graph.output(hidden, "projected")
    return graph


def joiner(model, precision):
    graph = Graph("nemotron_joiner", precision)
    enc = graph.input("encoder", [1, 640])
    pred = graph.input("prediction", [1, 640])
    value = graph.binary("joint-add", "ElementWiseAdd", enc, pred)
    value = graph.unary("joint-relu", "Relu", value)
    graph.output(graph.linear("joint-head", value, model.joint.head), "logits")
    return graph


def prediction(model, precision, fused=False):
    import torch

    graph = Graph("nemotron_prediction", precision)
    token = graph.input("token", [1], I32)
    embedding = graph.constant("embedding", model.decoder.embedding.weight)
    value = graph.op("token-embedding", "Gather", [embedding, token], [1, 640], [graph.scalar("axis", 0, I32)])
    module = model.decoder.lstm
    for index in range(2):
        prefix = f"lstm-{index}"
        h, c = graph.input(prefix + "-h-in", [1, 640]), graph.input(prefix + "-c-in", [1, 640])
        if fused:
            inputs = graph.concat(prefix + "-joined-input", [value, h], 1)
            weight = torch.cat((getattr(module, f"weight_ih_l{index}").T,
                                getattr(module, f"weight_hh_l{index}").T), dim=0)
            gates = graph.matmul(prefix + "-fused-projection", inputs, graph.constant(prefix + "-fused-weight", weight))
            bias = getattr(module, f"bias_ih_l{index}") + getattr(module, f"bias_hh_l{index}")
            gates = graph.binary(prefix + "-fused-bias", "ElementWiseAdd", gates, graph.constant(prefix + "-bias", bias))
        else:
            wi = graph.constant(prefix + "-weight-ih", getattr(module, f"weight_ih_l{index}").T)
            wh = graph.constant(prefix + "-weight-hh", getattr(module, f"weight_hh_l{index}").T)
            a, b = graph.matmul(prefix + "-ih", value, wi), graph.matmul(prefix + "-hh", h, wh)
            a = graph.binary(prefix + "-ih-bias", "ElementWiseAdd", a,
                             graph.constant(prefix + "-bias-ih", getattr(module, f"bias_ih_l{index}")))
            b = graph.binary(prefix + "-hh-bias", "ElementWiseAdd", b,
                             graph.constant(prefix + "-bias-hh", getattr(module, f"bias_hh_l{index}")))
            gates = graph.binary(prefix + "-gates", "ElementWiseAdd", a, b)
        gate_ids = []
        for gate_index, operation in enumerate(("Sigmoid", "Sigmoid", "Tanh", "Sigmoid")):
            gate = graph.slice(prefix + f"-gate-{gate_index}", gates, 1, gate_index * 640, (gate_index + 1) * 640)
            gate_ids.append(graph.unary(prefix + f"-activation-{gate_index}", operation, gate))
        i, f, g, o = gate_ids
        c = graph.binary(prefix + "-cell", "ElementWiseAdd",
                         graph.binary(prefix + "-forget", "ElementWiseMultiply", f, c),
                         graph.binary(prefix + "-candidate", "ElementWiseMultiply", i, g))
        value = graph.binary(prefix + "-hidden", "ElementWiseMultiply", o, graph.unary(prefix + "-cell-tanh", "Tanh", c))
        graph.output(c, prefix + "-c-out")
        graph.output(value, prefix + "-h-out")
    graph.output(graph.linear("prediction-projector", value, model.decoder.decoder_projector), "prediction")
    return graph


def read_tensor(directory, records, name):
    import numpy as np
    import torch

    record = records[name]
    path = directory / record["file"]
    verify_file(path, record)
    return torch.from_numpy(np.fromfile(path, dtype=record["dtype"]).reshape(record["shape"]).copy())


def fixture_write(path, graphs, steps, name, precision):
    import numpy as np

    flat = []
    for graph_index, graph in enumerate(graphs):
        flat.extend([{**tensor, "graph": graph_index} for tensor in graph["io"]])
    lookup = {(item["graph"], item["name"]): index for index, item in enumerate(flat)}
    metadata_size = align(64 + len(flat) * TENSOR + len(steps) * (16 + len(flat) * CELL))
    cursor = metadata_size
    records, arrays = [], []
    for step_index, step in enumerate(steps):
        records.append(struct.pack("<4I", step["flags"], step.get("audio_new_samples", 0),
                                   step.get("valid_features", 0), 0))
        for item in flat:
            key = (item["graph"], item["name"])
            cell = step["cells"].get(key)
            if cell is None:
                records.append(struct.pack("<IIQQII", ZERO, 0, 0, 0, 0, 0))
            elif isinstance(cell, tuple):
                source_graph, source_name, age = cell
                source = lookup[(source_graph, source_name)]
                if flat[source]["type"] != READ or flat[source]["shape"] != item["shape"] or \
                        flat[source]["dtype"] != item["dtype"] or \
                        flat[source].get("quantization") != item.get("quantization") or \
                        item["type"] != WRITE or age not in (0, 1):
                    raise ValueError("Invalid carried fixture state")
                records.append(struct.pack("<IIQQII", CARRY, source, 0, 0, age, 0))
            elif isinstance(cell, str) and cell == "zero":
                records.append(struct.pack("<IIQQII", ZERO, 0, 0, 0, 0, 0))
            else:
                if hasattr(cell, "detach"):
                    cell = cell.detach().cpu().numpy()
                dtype = "<f4" if item["type"] == READ and item["dtype"] in (F16, F32, U16, S16, S32) else \
                    {F16: "<f2", F32: "<f4", I32: "<i4", U32: "<u4", BOOL: "u1",
                     U16: "<u2", S16: "<i2", S32: "<i4"}[item["dtype"]]
                if "quantization" in item and item["type"] == WRITE:
                    quant = item["quantization"]
                    types = {U16: "<u2", S16: "<i2", S32: "<i4"}
                    limits = {U16: (0, 65535), S16: (-32768, 32767), S32: (-2147483648, 2147483647)}
                    raw = np.rint(np.asarray(cell) / quant["scale"] - quant["offset"])
                    lower, upper = limits[item["dtype"]]
                    if not np.isfinite(raw).all() or (raw < lower).any() or (raw > upper).any():
                        raise ValueError(f"Fixture input exceeds calibrated range: {item['name']}")
                    array = raw.astype(types[item["dtype"]]).reshape(item["shape"]).copy()
                else:
                    array = np.asarray(cell).astype(dtype).reshape(item["shape"]).copy()
                if array.dtype.kind == "f" and not np.isfinite(array).all():
                    raise ValueError("Nonfinite fixture tensor")
                records.append(struct.pack("<IIQQII", DATA, 0, cursor, array.nbytes, 0, 0))
                arrays.append(array)
                cursor = align(cursor + array.nbytes)
    path = Path(path)
    if path.exists():
        raise ValueError("Fixture package already exists")
    with path.open("x+b") as stream:
        stream.write(bytes(HEADER))
        stream.write(name_bytes(name))
        for item in flat:
            stream.write(tensor_record(item, graph=item["graph"]))
        for record in records:
            stream.write(record)
        stream.write(bytes(metadata_size - (stream.tell() - HEADER)))
        for array in arrays:
            stream.write(array.tobytes())
            stream.write(bytes(align(stream.tell() - HEADER) - (stream.tell() - HEADER)))
        finish_package(stream, 3, precision, len(flat), 0, len(steps),
                       2 if any("quantization" in item for item in flat) else 1)
    return {"file": path.name, "size": path.stat().st_size, "sha256": digest(path), "steps": len(steps)}


def packed_cache(value, attention=False):
    import torch

    if attention:
        result = torch.zeros((8, 56, 128), dtype=torch.float32)
        result[:, -value.shape[2]:] = value[0]
        return result
    return value[0].T.contiguous()


def cases(data_dir, graph_dir, output, selected, maximum_steps, risk=False):
    import torch
    from transformers import Nemotron3_5AsrForRNNT
    from transformers.models.nemotron_asr_streaming.modeling_nemotron_asr_streaming import (
        NemotronAsrStreamingEncoderCausalConvPaddingCache,
    )
    from nemotron_reference import environment

    environment(read_catalog())
    graph_manifest = load_json(graph_dir / "graphs.json")
    chosen_graphs = [next(item for item in graph_manifest["graphs"] if item["name"] == name) for name in
                     (("nemotron_encoder_risk",) if risk else
                      ("nemotron_subsampling_first", "nemotron_subsampling_steady", "nemotron_encoder"))]
    if output.exists():
        raise ValueError("Fixture output directory exists")
    precision = chosen_graphs[0]["precision"]
    model = Nemotron3_5AsrForRNNT.from_pretrained(
        data_dir / "checkpoint", local_files_only=True, dtype=torch.float32, attn_implementation="sdpa"
    ).eval()
    torch.set_num_threads(4)
    output.mkdir(parents=True)
    artifacts = []
    with torch.inference_mode():
        for fixture_name in selected.split(","):
            directory = data_dir / "reference-1" / fixture_name
            manifest = load_json(directory / "manifest.json")
            records = {item["name"]: item for item in manifest["tensors"]}
            source_steps = manifest["trace"]["steps"]
            if maximum_steps:
                source_steps = source_steps[:maximum_steps]
            padding = NemotronAsrStreamingEncoderCausalConvPaddingCache()
            previous = None
            steps = []
            for step_index, source in enumerate(source_steps):
                prefix = f"chunk-{step_index:04d}"
                features = read_tensor(directory, records, prefix + "-features")
                hidden = model.encoder.subsampling(features, padding_cache=padding)[0]
                active_front = 0 if step_index == 0 else 1
                core = 0 if risk else 2
                cells = {(core, "hidden"): hidden if risk else (active_front, "hidden", 0)}
                mask = torch.zeros((8, 4, 60))
                mask[:, :, :56 - min(56, step_index * 4)] = -10000
                mask_descriptor = next(item for item in chosen_graphs[core]["io"] if item["name"] == "mask")
                if mask_descriptor["dtype"] == BOOL:
                    mask = mask == 0
                cells[(core, "mask")] = mask
                if not risk:
                    cells[(active_front, "features")] = features[0]
                    cells[(active_front, "hidden")] = hidden
                    for cache_index in range(3):
                        actual = padding.layers[f"subsampling.{cache_index}"].cache.permute(0, 2, 3, 1)
                        cells[(active_front, f"sub-{cache_index}-out")] = actual.clone()
                        if step_index:
                            cells[(1, f"sub-{cache_index}-in")] = (
                                0 if step_index == 1 else 1, f"sub-{cache_index}-out", 1
                            )
                layer_count = 1 if risk else 24
                for layer in range(layer_count):
                    layer_prefix = f"layer-{layer:02d}"
                    for label, reference_name in (("key", "keys"), ("value", "values")):
                        input_name, output_name = f"{layer_prefix}-{label}-in", f"{layer_prefix}-{label}-out"
                        cells[(core, input_name)] = "zero" if step_index == 0 else (core, output_name, 1)
                        value = read_tensor(directory, records, f"{prefix}-output-attention-{layer}-{reference_name}")
                        cells[(core, output_name)] = packed_cache(value, True)
                    cells[(core, layer_prefix + "-conv-in")] = "zero" if step_index == 0 else (
                        core, layer_prefix + "-conv-out", 1
                    )
                    cells[(core, layer_prefix + "-conv-out")] = packed_cache(
                        read_tensor(directory, records, f"{prefix}-output-convolution-conv-{layer}")
                    )
                if risk:
                    captured = []
                    def capture(module, args, value):
                        captured.append(value.detach())
                    hook = model.encoder.layers[0].register_forward_hook(capture)
                    try:
                        encoded = model.get_audio_features(
                            features, prompt_ids=torch.tensor([9]), num_lookahead_tokens=3,
                            past_key_values=previous.past_key_values if previous is not None else None,
                            padding_cache=previous.padding_cache if previous is not None else None,
                            use_cache=True,
                        )
                        previous = encoded
                        cells[(0, "raw")] = captured[0][0]
                    finally:
                        hook.remove()
                else:
                    for label, reference_name in (("raw", "encoder-raw"), ("conditioned", "conditioned"),
                                                  ("projected", "joiner-encoder-projection")):
                        cells[(2, label)] = read_tensor(directory, records, prefix + "-" + reference_name)[0]
                flags = (1 if step_index == 0 else 0) | (2 if step_index == len(source_steps) - 1 else 0)
                consumed = min(manifest["clip"]["samples"], 4096 + step_index * 5120)
                previous_consumed = min(manifest["clip"]["samples"], 4096 + (step_index - 1) * 5120) if step_index else 0
                steps.append({"flags": flags, "valid_features": source["valid_features"],
                              "audio_new_samples": consumed - previous_consumed, "cells": cells})
                print(f"Prepared {fixture_name} {step_index + 1}/{len(source_steps)}", flush=True)
            path = output / (fixture_name + ".nmf")
            artifacts.append(fixture_write(path, chosen_graphs, steps, fixture_name, precision))
    write_json(output / "fixtures.json", {"schema_version": 1, "graphs_sha256": digest(graph_dir / "graphs.json"),
                                         "fixtures": artifacts, "teacher_forced_caches": False,
                                         "pipeline": not risk})


def decoder_cases(data_dir, graph_dir, output, fixture_name, maximum_steps):
    import torch
    from nemotron_reference import environment

    environment(read_catalog())
    manifest = load_json(graph_dir / "graphs.json")
    directory = data_dir / "reference-1" / fixture_name
    reference = load_json(directory / "manifest.json")
    records = {item["name"]: item for item in reference["tensors"]}
    if output.exists():
        raise ValueError("Decoder fixture directory exists")
    output.mkdir(parents=True)
    artifacts = []
    for graph_name in ("nemotron_prediction", "nemotron_joiner"):
        graph = next(item for item in manifest["graphs"] if item["name"] == graph_name)
        steps = []
        if graph_name == "nemotron_prediction":
            initial = {"token": torch.tensor([13087], dtype=torch.int32)}
            for layer in range(2):
                for state in ("h", "c"):
                    initial[f"lstm-{layer}-{state}-in"] = "zero"
                    initial[f"lstm-{layer}-{state}-out"] = read_tensor(
                        directory, records, "prediction-initial-" + ("hidden_state" if state == "h" else "cell_state")
                    )[layer]
            initial["prediction"] = read_tensor(directory, records, "prediction-initial-cache").reshape(1, 640)
            steps.append({"flags": 1, "cells": {(0, name): value for name, value in initial.items()}})
        for chunk in reference["trace"]["steps"]:
            for decision in chunk["decisions"]:
                prefix = f"chunk-{chunk['index']:04d}-frame-{decision['frame'] - chunk['index'] * 4:02d}-symbol-{decision['symbol']:02d}"
                if graph_name == "nemotron_prediction":
                    if decision["token"] == 13087:
                        continue
                    cells = {(0, "token"): torch.tensor([decision["token"]], dtype=torch.int32)}
                    for layer in range(2):
                        for state in ("h", "c"):
                            cells[(0, f"lstm-{layer}-{state}-in")] = (0, f"lstm-{layer}-{state}-out", 1)
                            cells[(0, f"lstm-{layer}-{state}-out")] = read_tensor(
                                directory, records, prefix + "-committed-" +
                                ("hidden_state" if state == "h" else "cell_state")
                            )[layer]
                    cells[(0, "prediction")] = read_tensor(directory, records, prefix + "-committed-cache").reshape(1, 640)
                else:
                    encoder_values = read_tensor(directory, records, f"chunk-{chunk['index']:04d}-joiner-encoder-projection")
                    cells = {(0, "encoder"): encoder_values[:, decision["frame"] - chunk["index"] * 4],
                             (0, "prediction"): read_tensor(directory, records, prefix + "-prediction").reshape(1, 640),
                             (0, "logits"): read_tensor(directory, records, prefix + "-logits").reshape(1, 13088)}
                steps.append({"flags": 1 if not steps else 0, "cells": cells})
                if len(steps) >= maximum_steps:
                    break
            if len(steps) >= maximum_steps:
                break
        steps[-1]["flags"] |= 2
        name = "prediction" if graph_name == "nemotron_prediction" else "joiner"
        artifacts.append(fixture_write(output / (name + ".nmf"), [graph], steps, name, graph["precision"]))
    write_json(output / "fixtures.json", {"schema_version": 1, "source": fixture_name, "fixtures": artifacts})


def cpu_check(data_dir, output, fixture_name, count, full):
    import torch
    from transformers import Nemotron3_5AsrForRNNT
    from nemotron_reference import environment

    environment(read_catalog())
    torch.set_num_threads(4)
    model = Nemotron3_5AsrForRNNT.from_pretrained(
        data_dir / "checkpoint", local_files_only=True, dtype=torch.float32, attn_implementation="sdpa"
    ).eval()
    directory = data_dir / "reference-1" / fixture_name
    manifest = load_json(directory / "manifest.json")
    records = {item["name"]: item for item in manifest["tensors"]}
    with torch.inference_mode():
        first, steady = subsampling(model, True, 32), subsampling(model, False, 32)
        core = encoder(model, 32, 24 if full else 1, full)
        front_state, core_state = {}, {}
        results = []
        previous = None
        for step in manifest["trace"]["steps"][:count]:
            index = step["index"]
            prefix = f"chunk-{index:04d}"
            features = read_tensor(directory, records, prefix + "-features")
            front_inputs = {"features": features[0], **front_state}
            front = (first if index == 0 else steady).evaluate(front_inputs)
            for cache_index in range(3):
                expected = read_tensor(directory, records, prefix + f"-output-convolution-subsampling-{cache_index}")
                torch.testing.assert_close(front[f"sub-{cache_index}-out"], expected.permute(0, 2, 3, 1),
                                           atol=0.0001, rtol=0.0001)
                front_state[f"sub-{cache_index}-in"] = front[f"sub-{cache_index}-out"]
            mask = torch.zeros((8, 4, 60))
            mask[:, :, :56 - min(56, index * 4)] = -10000
            core_inputs = {"hidden": front["hidden"], "mask": mask}
            for item in core.tensors:
                if item["type"] == WRITE and item["name"] not in core_inputs:
                    core_inputs[item["name"]] = core_state.get(item["name"], torch.zeros(item["shape"]))
            actual = core.evaluate(core_inputs)
            if full:
                expected = read_tensor(directory, records, prefix + "-encoder-raw")[0]
                torch.testing.assert_close(actual["raw"], expected, atol=0.005, rtol=0.005)
                for label, source_name in (("conditioned", "conditioned"), ("projected", "joiner-encoder-projection")):
                    torch.testing.assert_close(actual[label], read_tensor(directory, records, prefix + "-" + source_name)[0],
                                               atol=0.005, rtol=0.005)
            else:
                captured = []
                hook = model.encoder.layers[0].register_forward_hook(lambda module, args, value: captured.append(value))
                try:
                    previous = model.get_audio_features(
                        features, prompt_ids=torch.tensor([9]), num_lookahead_tokens=3,
                        past_key_values=previous.past_key_values if previous is not None else None,
                        padding_cache=previous.padding_cache if previous is not None else None, use_cache=True,
                    )
                finally:
                    hook.remove()
                expected = captured[0][0]
                torch.testing.assert_close(actual["raw"], expected, atol=0.005, rtol=0.005)
            for layer in range(24 if full else 1):
                label = f"layer-{layer:02d}"
                for suffix, source_suffix in (("key", "keys"), ("value", "values"), ("conv", None)):
                    name = label + "-" + suffix + "-out"
                    source_name = f"{prefix}-output-attention-{layer}-{source_suffix}" if source_suffix else \
                        f"{prefix}-output-convolution-conv-{layer}"
                    expected_cache = packed_cache(read_tensor(directory, records, source_name), source_suffix is not None)
                    torch.testing.assert_close(actual[name], expected_cache, atol=0.005, rtol=0.005)
                    core_state[label + "-" + suffix + "-in"] = actual[name]
            maximum = float((actual["raw"] - expected).abs().max())
            results.append({"step": index, "raw_max_abs_error": maximum})
            print(f"CPU fixed-cache parity {index + 1}/{count}: max_abs={maximum:.8g}", flush=True)
    write_json(output, {"schema_version": 1, "status": "cpu_adapter_passed", "fixture": fixture_name,
                        "full_encoder": full, "carried_caches": True, "steps": results,
                        "npu_status": "not_tested"})


def export(data_dir, output, precision, selected, fused_prediction=False, fully_connected=False, centered_norm=False):
    import torch
    from transformers import Nemotron3_5AsrForRNNT
    from nemotron_reference import environment

    accepted = load_json(data_dir / "m0-acceptance.json")
    if accepted["status"] != "m0_reference_accepted" or accepted["contract_sha256"] != CONTRACT_SHA:
        raise ValueError("M1 requires the accepted pinned M0 reference")
    reference = data_dir / "reference-1"
    if digest(reference / "manifest.json") != accepted["runs"][0]["manifest_sha256"]:
        raise ValueError("Accepted reference manifest changed")
    catalog = read_catalog()
    environment(catalog)
    if output.exists():
        raise ValueError("Export directory already exists")
    torch.set_num_threads(4)
    torch.set_num_interop_threads(1)
    model = Nemotron3_5AsrForRNNT.from_pretrained(
        data_dir / "checkpoint", local_files_only=True, dtype=torch.float32, attn_implementation="sdpa"
    ).eval()
    output.mkdir(parents=True)
    builders = {
        "subsampling-first": lambda: subsampling(model, True, precision, fully_connected),
        "subsampling-steady": lambda: subsampling(model, False, precision, fully_connected),
        "encoder-risk": lambda: encoder(model, precision, 1, False, fully_connected, centered_norm),
        "encoder": lambda: encoder(model, precision, fully_connected=fully_connected, centered_norm=centered_norm),
        "joiner": lambda: joiner(model, precision),
        "prediction": lambda: prediction(model, precision, fused_prediction),
    }
    graphs = []
    with torch.inference_mode():
        for name in selected.split(","):
            print(f"Exporting {name}, FP{precision}", flush=True)
            graph = builders[name]()
            graphs.append(graph.write(output / (name + ".nmg")))
    write_json(output / "graphs.json", {
        "schema_version": 1, "model": catalog["model"], "contract_sha256": CONTRACT_SHA,
        "m0_acceptance_sha256": digest(data_dir / "m0-acceptance.json"),
        "graphs": graphs,
        "constant_folding": "Fixed-distance relative-position projections and de-DE one-hot contribution folded offline; no runtime CPU model operations",
        "cache_contract": "Fixed 56-frame left-padded attention; separate first/steady subsampling; distinct carried input/output buffers",
        "fused_prediction": fused_prediction,
        "fully_connected": fully_connected,
        "centered_norm": centered_norm,
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--deps-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    graph_export = commands.add_parser("export")
    graph_export.add_argument("--output", type=Path, required=True)
    graph_export.add_argument("--precision", type=int, choices=(16, 32), default=16)
    graph_export.add_argument("--graphs", default="subsampling-first,subsampling-steady,encoder-risk,joiner,prediction")
    graph_export.add_argument("--fused-prediction", action="store_true")
    graph_export.add_argument("--fully-connected", action="store_true")
    graph_export.add_argument("--centered-norm", action="store_true")
    fixture_export = commands.add_parser("cases")
    fixture_export.add_argument("--graphs", type=Path, required=True)
    fixture_export.add_argument("--output", type=Path, required=True)
    fixture_export.add_argument("--fixtures", default="long,tail,silence")
    fixture_export.add_argument("--max-steps", type=int, default=0)
    fixture_export.add_argument("--risk", action="store_true")
    decoder_export = commands.add_parser("decoder-cases")
    decoder_export.add_argument("--graphs", type=Path, required=True)
    decoder_export.add_argument("--output", type=Path, required=True)
    decoder_export.add_argument("--fixture", default="de_de-1586-pcm16")
    decoder_export.add_argument("--max-steps", type=int, default=16)
    cpu = commands.add_parser("cpu-check")
    cpu.add_argument("--output", type=Path, required=True)
    cpu.add_argument("--fixture", default="long")
    cpu.add_argument("--steps", type=int, default=2)
    cpu.add_argument("--full", action="store_true")
    args = parser.parse_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    if args.command == "export":
        export(args.data_dir, args.output, args.precision, args.graphs,
               args.fused_prediction, args.fully_connected, args.centered_norm)
    elif args.command == "cases":
        cases(args.data_dir, args.graphs, args.output, args.fixtures, args.max_steps, args.risk)
    elif args.command == "decoder-cases":
        decoder_cases(args.data_dir, args.graphs, args.output, args.fixture, args.max_steps)
    else:
        cpu_check(args.data_dir, args.output, args.fixture, args.steps, args.full)


if __name__ == "__main__":
    main()
