import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nemotron_m5 import analyze


class LiveEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.stats = {
            "schema": "newos.nemotron.live.stats.v1", "passed": True, "duration_s": 1800,
            "capture_ms": 1800000, "accepted_samples": 28800000, "consumed_samples": 28800000,
            "queue_last": 0, "queue_capacity": 81920, "queue_peak": 320,
            "capture_thread": 1, "inference_thread": 2, "failure": 0, "canceled": 0,
            "model_samples": 16000, "endpoint_trimmed_samples": 9600, "idle_samples": 28774400,
            "raw_audio_recorded": False, "input_rate": 48000, "input_channels": 2, "input_format": 3,
            "measurements": 174, "steady_working_set": 5000000000, "end_working_set": 5000000000,
            "utterances": 1, "capture_mmcss": 1, "source_gap_frames": 0, "source_packets": 180000}
        self.log = "\n".join(
            f"LIVE_STATS elapsed_s={sec} queue_samples=160 queue_peak=320 working_set=5000000000 utterances=1"
            for sec in range(60, 1800, 10))
        self.event = {"schema": "newos.nemotron.live.activity.v1", "event": "final", "utterance": 0,
                      "audio_samples": 16000, "capture_samples": 25600, "endpoint_trimmed_samples": 9600,
                      "token_count": 10, "transcript_retained": False}

    def audit(self, stats=None, log=None, event=None):
        return analyze(stats or self.stats, self.log if log is None else log,
                       json.dumps(event or self.event), 1800, True)["passed"]

    def test_complete(self):
        self.assertTrue(self.audit())

    def test_failure_and_cancel(self):
        for field, value in (("passed", False), ("failure", 1), ("canceled", 1),
                             ("capture_ms", 807820), ("queue_capacity", 163840),
                             ("accepted_samples", 28800001), ("model_samples", 16001),
                             ("queue_peak", 81920), ("raw_audio_recorded", True)):
            with self.subTest(field=field):
                stats = copy.copy(self.stats)
                stats[field] = value
                self.assertFalse(self.audit(stats))

    def test_incomplete_periodic_evidence(self):
        self.assertFalse(self.audit(log=""))

    def test_queue_growth(self):
        lines = self.log.splitlines()
        lines[-44:] = [line.replace("queue_samples=160", "queue_samples=16000") for line in lines[-44:]]
        self.assertFalse(self.audit(log="\n".join(lines)))

    def test_memory_growth(self):
        lines = self.log.splitlines()
        lines[-44:] = [line.replace("working_set=5000000000", "working_set=5003000000") for line in lines[-44:]]
        self.assertFalse(self.audit(log="\n".join(lines)))

    def test_content_is_never_evidence(self):
        event = dict(self.event, text="disallowed")
        self.assertFalse(self.audit(event=event))

    def test_runtime_error_is_reported_not_crashed(self):
        self.assertFalse(self.audit(event={"schema": "newos.nemotron.asr.v1", "event": "error",
                                          "message": "native ASR failed; see diagnostics"}))

    def test_wrong_positions_or_lifecycle(self):
        for field, value in (("utterance", 1), ("capture_samples", 15000), ("audio_samples", 30000)):
            with self.subTest(field=field):
                self.assertFalse(self.audit(event=dict(self.event, **{field: value})))


if __name__ == "__main__":
    unittest.main()
