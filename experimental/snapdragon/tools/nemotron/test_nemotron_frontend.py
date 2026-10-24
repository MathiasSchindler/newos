import argparse
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_reference import FeatureStream, TensorWriter, chunk_features, features_for_audio, greedy_trace


class FrontendTests(unittest.TestCase):
    def test_partition_and_offline_parity(self):
        import numpy as np
        import torch
        from transformers import NemotronAsrStreamingFeatureExtractor

        torch.set_num_threads(1)
        extractor = NemotronAsrStreamingFeatureExtractor(feature_size=128)
        for length in (160, 173, 4000, 5120, 10240, 10413):
            samples = (np.sin(np.arange(length, dtype=np.float32) * np.float32(0.03)) *
                       np.float32(0.2)).tolist()
            with self.subTest(length=length):
                first, _ = features_for_audio(samples, extractor, torch, [5120])
                second, peak = features_for_audio(samples, extractor, torch, [1, 159, 256, 4096])
                offline = extractor(np.asarray(samples), sampling_rate=16000, return_tensors="pt")
                torch.testing.assert_close(first, offline.input_features[:, :length // 160],
                                           atol=2e-5, rtol=2e-5)
                torch.testing.assert_close(first, second, atol=2e-5, rtol=2e-5)
                self.assertEqual(first.shape[1], length // 160)
                self.assertLessEqual(peak, 4096 + 1024)
                chunks = chunk_features(first, torch)
                self.assertEqual(sum(valid for _, valid in chunks), length // 160)
                self.assertEqual(chunks[0][0].shape, (1, 25, 128))
                for chunk, valid in chunks:
                    self.assertTrue(torch.equal(chunk[:, valid:], torch.zeros_like(chunk[:, valid:])))

    def test_silence_reset_and_eof(self):
        import torch
        from transformers import NemotronAsrStreamingFeatureExtractor

        extractor = NemotronAsrStreamingFeatureExtractor(feature_size=128)
        stream = FeatureStream(extractor, torch)
        first = stream.feed([0] * 4000, eof=True)
        other = FeatureStream(extractor, torch).feed([0] * 4000, eof=True)
        torch.testing.assert_close(first, other, atol=0, rtol=0)
        self.assertTrue(torch.isfinite(first).all())
        with self.assertRaises(ValueError):
            stream.feed([0])
        with self.assertRaises(ValueError):
            FeatureStream(extractor, torch).feed([float("nan")])

    def fake_decoder_model(self, choices):
        import torch

        calls = []
        tokens = iter(choices)
        config = SimpleNamespace(blank_token_id=2, num_prompts=128, max_symbols_per_step=10,
                                 decoder_hidden_size=2, num_decoder_layers=1)

        def prediction(inputs, cache):
            calls.append(int(inputs[0, 0]))
            value = torch.ones((1, 1, 2)) * len(calls)
            if not cache.is_initialized:
                cache.lazy_initialization(value)
            cache.update(value, torch.ones((1, 1, 2)) * len(calls),
                         torch.ones((1, 1, 2)) * len(calls))
            return cache.cache

        def encode(*args, **kwargs):
            return SimpleNamespace(
                last_hidden_state=torch.zeros((1, 2, 2)), pooler_output=torch.zeros((1, 2, 2)),
                past_key_values=None, padding_cache=None,
            )

        def joint(**kwargs):
            result = torch.zeros((1, 1, 1, 3))
            result[0, 0, 0, next(tokens)] = 1
            return result

        model = SimpleNamespace(config=config, decoder=prediction, get_audio_features=encode,
                                prompt_projector=lambda value: value[:, :, :2],
                                joint=joint, dtype=torch.float32)
        processor = SimpleNamespace(decode=lambda values, **kwargs: " ".join(map(str, values)))
        return model, processor, calls

    def test_greedy_blank_reuse_and_nonblank_commit(self):
        import torch

        model, processor, calls = self.fake_decoder_model([0, 2, 2])
        with tempfile.TemporaryDirectory() as directory:
            writer = TensorWriter(directory)
            result = greedy_trace(model, processor, [(torch.zeros((1, 25, 128)), 25)], writer, torch)
            self.assertEqual(result["tokens"], [0])
            self.assertEqual(calls, [2, 0])
            self.assertEqual([item["frame"] for item in result["steps"][0]["decisions"]], [0, 0, 1])

    def test_greedy_symbol_limit_is_error(self):
        import torch

        model, processor, calls = self.fake_decoder_model([0] * 10)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "symbol limit"):
                greedy_trace(model, processor, [(torch.zeros((1, 25, 128)), 25)],
                             TensorWriter(directory), torch)

    def test_greedy_nonfinite_is_error(self):
        import torch

        model, processor, _ = self.fake_decoder_model([])
        model.joint = lambda **kwargs: torch.full((1, 1, 1, 3), float("nan"))
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Nonfinite joiner"):
                greedy_trace(model, processor, [(torch.zeros((1, 25, 128)), 25)],
                             TensorWriter(directory), torch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--deps-dir", type=Path)
    args, remaining = parser.parse_known_args()
    if args.deps_dir:
        sys.path.insert(0, str(args.deps_dir.resolve()))
    unittest.main(argv=[sys.argv[0], *remaining])
