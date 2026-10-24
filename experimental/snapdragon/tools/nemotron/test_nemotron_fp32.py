"""Development-only CPU tests of the exact C kernels; never an HTP acceptance gate."""

import argparse
import ctypes
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_fp32 import metadata
from nemotron_m1 import F32, I32, Graph


LIBRARY = None


def execute(graph, inputs, slices=1):
    import numpy as np

    if len(graph.nodes) != 1:
        raise ValueError("Unit runner executes one unfused operator")
    node = graph.nodes[0]
    descriptor = metadata(graph, node).view("<u4")
    values = []
    for tensor_index in node["inputs"]:
        tensor = graph.tensors[tensor_index]
        source = inputs.get(tensor["name"], tensor["value"])
        dtype = "<i4" if tensor["dtype"] == I32 else "<f4"
        values.append(np.ascontiguousarray(source, dtype=dtype).reshape(tensor["shape"]))
    pointers = (ctypes.c_void_p * len(values))(*(value.ctypes.data for value in values))
    counts = (ctypes.c_size_t * len(values))(*(value.size for value in values))
    out = np.full(graph.tensors[node["output"]]["shape"], np.nan, dtype=np.float32)
    for slice_ in range(slices):
        result = LIBRARY(descriptor.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                         descriptor.size, pointers, counts, len(values),
                         out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), out.size, slice_, slices)
        if result:
            raise ValueError(f"FP32 reference kernel returned {result}")
    return out


class KernelTests(unittest.TestCase):
    def test_precision_sensitive_add_and_matmul(self):
        import numpy as np

        graph = Graph("add", 32)
        left = graph.input("left", [4])
        right = graph.input("right", [4])
        graph.binary("add", "ElementWiseAdd", left, right)
        a = np.asarray([10000, 9999, 10000.125, 123.0001], dtype=np.float32)
        b = np.asarray([-9999, -10000, -10000, -123], dtype=np.float32)
        np.testing.assert_array_equal(execute(graph, {"left": a, "right": b}, 3), a + b)
        graph = Graph("matmul", 32)
        left = graph.input("left", [1, 2])
        right = graph.input("right", [2, 1])
        graph.matmul("mul", left, right)
        result = execute(graph, {"left": [[1.0001, -1]], "right": [[10000], [10000]]}, 2)
        np.testing.assert_array_equal(result, np.asarray([[1]], dtype=np.float32))

    def test_matrix_batch_and_full_width(self):
        import numpy as np

        random = np.random.default_rng(17)
        for a_shape, b_shape in (([4, 1024], [1024, 32]), ([8, 4, 128], [8, 128, 60])):
            graph = Graph("matrix", 32)
            left, right = graph.input("left", a_shape), graph.input("right", b_shape)
            graph.matmul("mul", left, right)
            a = random.normal(size=a_shape).astype(np.float32)
            b = random.normal(size=b_shape).astype(np.float32)
            actual = execute(graph, {"left": a, "right": b}, 4)
            np.testing.assert_allclose(actual, a @ b, atol=0.0001, rtol=0.0001)

    def test_packed_weights_preserve_numeric_values(self):
        import numpy as np

        random = np.random.default_rng(1032)
        a = random.normal(size=(4, 128)).astype(np.float32)
        b = random.normal(size=(128, 64)).astype(np.float32)
        graph = Graph("packed", 32)
        x = graph.input("input", [4, 128])
        packed = b.reshape(128, 2, 32).transpose(1, 0, 2).copy().reshape(128, 64)
        w = graph.constant("weight", packed)
        graph.op("packed", "MatMul", [x, w], [4, 64], [graph.scalar("weight_tiles", 32)])
        np.testing.assert_allclose(execute(graph, {"input": a}, 3), a @ b, atol=1e-5, rtol=1e-5)

    def test_matrix_partial_columns_and_reduction_tails(self):
        import numpy as np

        random = np.random.default_rng(6032)
        for rows, inner, columns in ((1, 1, 33), (3, 2, 60), (4, 3, 63),
                                     (4, 4, 60), (5, 5, 33), (4, 127, 60), (4, 128, 60)):
            graph = Graph("partial-matrix", 32)
            left = graph.input("left", [2, rows, inner])
            right = graph.input("right", [2, inner, columns])
            graph.matmul("mul", left, right)
            a = random.normal(size=(2, rows, inner)).astype(np.float32)
            b = random.normal(size=(2, inner, columns)).astype(np.float32)
            np.testing.assert_allclose(execute(graph, {"left": a, "right": b}, 7), a @ b,
                                       atol=1e-4, rtol=1e-4)

    def test_broadcast_arithmetic(self):
        import numpy as np

        for operation in ("ElementWiseAdd", "ElementWiseSubtract", "ElementWiseMultiply"):
            graph = Graph("arithmetic", 32)
            left, right = graph.input("left", [8, 4, 3]), graph.input("right", [8, 1, 3])
            graph.binary("op", operation, left, right)
            a = np.arange(96, dtype=np.float32).reshape(8, 4, 3)
            b = np.arange(24, dtype=np.float32).reshape(8, 1, 3) * np.float32(0.25)
            expected = a + b if operation == "ElementWiseAdd" else a - b if operation == "ElementWiseSubtract" else a * b
            np.testing.assert_array_equal(execute(graph, {"left": a, "right": b}, 7), expected)

    def test_sigmoid_tanh_relu_and_rsqrt(self):
        import numpy as np

        values = np.linspace(-50, 50, 10001, dtype=np.float32)
        for operation in ("Sigmoid", "Tanh", "Relu"):
            graph = Graph("unary", 32)
            source = graph.input("input", list(values.shape))
            graph.unary("op", operation, source)
            expected = 1 / (1 + np.exp(-values.astype(np.float64))) if operation == "Sigmoid" else \
                np.tanh(values.astype(np.float64)) if operation == "Tanh" else np.maximum(values, 0)
            np.testing.assert_allclose(execute(graph, {"input": values}, 3), expected, atol=2e-7, rtol=2e-6)
        graph = Graph("rsqrt", 32)
        source = graph.input("input", [1000])
        graph.unary("op", "ElementWiseRsqrt", source)
        values = np.geomspace(1e-40, 1e38, 1000).astype(np.float32)
        np.testing.assert_allclose(execute(graph, {"input": values}, 4), 1 / np.sqrt(values.astype(np.float64)),
                                   atol=1e-6, rtol=5e-7)

    def test_layer_norm_and_softmax(self):
        import numpy as np

        random = np.random.default_rng(91)
        a = (random.normal(size=(4, 1024)) * 1000).astype(np.float32)
        gain = random.uniform(0.01, 3, size=1024).astype(np.float32)
        bias = random.normal(size=1024).astype(np.float32)
        graph = Graph("norm", 32)
        x = graph.input("input", [4, 1024])
        g = graph.constant("gain", gain)
        b = graph.constant("bias", bias)
        graph.op("norm", "LayerNorm", [x, g, b], [4, 1024], [
            graph.scalar("epsilon", 1e-5, F32), graph.parameter("axes", [1]),
        ])
        mean = a.astype(np.float64).mean(axis=1, keepdims=True)
        variance = ((a.astype(np.float64) - mean) ** 2).mean(axis=1, keepdims=True)
        expected = (a - mean) / np.sqrt(variance + 1e-5) * gain + bias
        np.testing.assert_allclose(execute(graph, {"input": a}, 3), expected, atol=1e-5, rtol=1e-5)
        graph = Graph("softmax", 32)
        x = graph.input("input", [8, 4, 60])
        graph.op("softmax", "Softmax", [x], [8, 4, 60], [
            graph.scalar("axis", 2), graph.scalar("beta", 1, F32),
        ])
        a = random.normal(size=(8, 4, 60)).astype(np.float32) * 20
        a[:, :, :20] = -10000
        expected = np.exp(a.astype(np.float64) - a.max(axis=2, keepdims=True))
        expected /= expected.sum(axis=2, keepdims=True)
        actual = execute(graph, {"input": a}, 3)
        np.testing.assert_allclose(actual, expected, atol=2e-7, rtol=1e-5)

    def test_sigmoid_vector_boundaries_and_extremes(self):
        import numpy as np

        boundary = np.asarray([
            -104, -103.97208, -90, -88, -87, -50, -8, -1, -0.0, 0.0,
            1, 8, 50, 87, 88, 90, 103.97208, 104,
            np.finfo(np.float32).max, -np.finfo(np.float32).max, 1e-40, -1e-40,
            *np.linspace(-1, 1, 10),
        ], dtype=np.float32)
        for source_values in (boundary[:1], boundary[:31], boundary, np.append(boundary, np.float32(0.1)),
                              np.linspace(-104, 104, 10001, dtype=np.float32)):
            values = source_values.copy()
            graph = Graph("sigmoid-boundary", 32)
            source = graph.input("input", [values.size])
            graph.unary("sigmoid", "Sigmoid", source)
            ratio = np.exp(-np.abs(values.astype(np.float64)))
            expected = np.where(values >= 0, 1 / (1 + ratio), ratio / (1 + ratio))
            actual = execute(graph, {"input": values}, 7)
            np.testing.assert_allclose(actual, expected, atol=2e-7, rtol=0)
            values[-1] = np.nan
            with self.assertRaises(ValueError):
                execute(graph, {"input": values}, 7)

    def test_transpose_slice_concat_and_pad(self):
        import numpy as np

        graph = Graph("transpose", 32)
        source = graph.input("input", [4, 8, 3])
        graph.transpose("out", source, [1, 0, 2])
        data = np.arange(96, dtype=np.float32).reshape(4, 8, 3)
        np.testing.assert_array_equal(execute(graph, {"input": data}, 3), data.transpose(1, 0, 2))
        graph = Graph("slice", 32)
        source = graph.input("input", [8, 60, 3])
        graph.slice("out", source, 1, 4, 60)
        data = np.arange(1440, dtype=np.float32).reshape(8, 60, 3)
        np.testing.assert_array_equal(execute(graph, {"input": data}, 5), data[:, 4:])
        graph = Graph("concat", 32)
        left, right = graph.input("left", [8, 56, 3]), graph.input("right", [8, 4, 3])
        graph.concat("out", [left, right], 1)
        np.testing.assert_array_equal(execute(graph, {"left": data[:, :56], "right": data[:, 56:]}, 3), data)
        graph = Graph("pad", 32)
        source = graph.input("input", [1, 25, 128, 1])
        pads = [[0, 0], [2, 0], [2, 1], [0, 0]]
        graph.pad("out", source, pads)
        data = np.arange(3200, dtype=np.float32).reshape(1, 25, 128, 1)
        np.testing.assert_array_equal(execute(graph, {"input": data}, 4), np.pad(data, pads))
        graph = Graph("key-transpose", 32)
        source = graph.input("input", [8, 60, 128])
        graph.transpose("out", source, [0, 2, 1])
        data = np.arange(61440, dtype=np.float32).reshape(8, 60, 128)
        np.testing.assert_array_equal(execute(graph, {"input": data}, 4), data.transpose(0, 2, 1))

    def test_pad_row_spans_and_constant_bits(self):
        import numpy as np

        cases = (([37], [[35, 17]]),
                 ([3, 5], [[2, 1], [4, 3]]),
                 ([2, 3, 33], [[1, 2], [0, 4], [5, 7]]),
                 ([1, 4, 7, 32], [[0, 0], [2, 1], [3, 2], [0, 0]]),
                 ([2, 64], [[0, 0], [0, 0]]))
        for shape, pads in cases:
            for value in (0.0, -0.0, 1.25):
                graph = Graph("pad-spans", 32)
                source = graph.input("input", shape)
                target_shape = [size + sum(pad) for size, pad in zip(shape, pads)]
                graph.op("pad", "Pad", [source], target_shape, [
                    graph.scalar("scheme", 0), graph.parameter("pad_amount", pads),
                    graph.scalar("pad_constant_value", value, F32),
                ])
                data = np.arange(np.prod(shape), dtype=np.float32).reshape(shape)
                data.flat[0] = -0.0
                expected = np.pad(data, pads, constant_values=value)
                actual = execute(graph, {"input": data}, 7)
                np.testing.assert_array_equal(actual.view(np.uint32), expected.view(np.uint32))
                data.flat[-1] = np.inf
                with self.assertRaises(ValueError):
                    execute(graph, {"input": data}, 7)

    def test_regular_and_depthwise_convolution(self):
        import numpy as np
        random = np.random.default_rng(712)
        for depthwise in (False, True):
            channels, filters = 3, 3 if depthwise else 4
            source = random.normal(size=(1, 5, 6, channels)).astype(np.float32)
            weight = random.normal(size=(3, 2, 1 if depthwise else channels, filters)).astype(np.float32)
            bias = random.normal(size=filters).astype(np.float32)
            graph = Graph("conv", 32)
            x = graph.input("input", list(source.shape))
            w, b = graph.constant("weight", weight), graph.constant("bias", bias)
            graph.op("conv", "DepthWiseConv2d" if depthwise else "Conv2d", [x, w, b], [1, 2, 5, filters], [
                graph.parameter("stride", [2, 1]), graph.parameter("dilation", [1, 1]),
                graph.parameter("pad_amount", [[0, 0], [0, 0]]),
            ])
            expected = np.empty((1, 2, 5, filters), dtype=np.float32)
            for y in range(2):
                for x_ in range(5):
                    for c in range(filters):
                        if depthwise:
                            value = np.sum(source[0, y * 2:y * 2 + 3, x_:x_ + 2, c].astype(np.float64) *
                                           weight[:, :, 0, c]) + bias[c]
                        else:
                            value = np.sum(source[0, y * 2:y * 2 + 3, x_:x_ + 2].astype(np.float64) *
                                           weight[:, :, :, c]) + bias[c]
                        expected[0, y, x_, c] = value
            np.testing.assert_allclose(execute(graph, {"input": source}, 4), expected, atol=1e-5, rtol=1e-5)

    def test_pointwise_convolution_matrix_path(self):
        import numpy as np

        random = np.random.default_rng(1101)
        source = random.normal(size=(1, 7, 33, 64)).astype(np.float32)
        weight = random.normal(size=(1, 1, 64, 32)).astype(np.float32)
        bias = random.normal(size=32).astype(np.float32)
        graph = Graph("pointwise", 32)
        x = graph.input("input", list(source.shape))
        w, b = graph.constant("weight", weight), graph.constant("bias", bias)
        graph.op("pointwise", "Conv2d", [x, w, b], [1, 7, 33, 32], [
            graph.parameter("stride", [1, 1]), graph.parameter("dilation", [1, 1]),
            graph.parameter("pad_amount", [[0, 0], [0, 0]]),
        ])
        expected = source.reshape(-1, 64) @ weight.reshape(64, 32) + bias
        np.testing.assert_allclose(execute(graph, {"input": source}, 4).reshape(-1, 32), expected,
                                   atol=1e-5, rtol=1e-5)

    def test_gather_bounds_and_nonfinite_rejection(self):
        import numpy as np

        graph = Graph("gather", 32)
        source, tokens = graph.input("input", [3, 4]), graph.input("tokens", [2], I32)
        graph.op("gather", "Gather", [source, tokens], [2, 4], [graph.scalar("axis", 0, I32)])
        source = np.arange(12, dtype=np.float32).reshape(3, 4)
        np.testing.assert_array_equal(execute(graph, {"input": source, "tokens": [2, 0]}, 3), source[[2, 0]])
        with self.assertRaises(ValueError):
            execute(graph, {"input": source, "tokens": [3, 0]})
        graph = Graph("nonfinite", 32)
        value = graph.input("input", [1])
        graph.unary("sigmoid", "Sigmoid", value)
        with self.assertRaises(ValueError):
            execute(graph, {"input": [np.nan]})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--deps-dir", type=Path)
    args, remaining = parser.parse_known_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    dll = ctypes.CDLL(str(args.library.resolve()))
    LIBRARY = dll.NmFp32Reference
    LIBRARY.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p),
                       ctypes.POINTER(ctypes.c_size_t), ctypes.c_size_t, ctypes.POINTER(ctypes.c_float),
                       ctypes.c_size_t, ctypes.c_uint, ctypes.c_uint]
    LIBRARY.restype = ctypes.c_int
    unittest.main(argv=[sys.argv[0], *remaining])
