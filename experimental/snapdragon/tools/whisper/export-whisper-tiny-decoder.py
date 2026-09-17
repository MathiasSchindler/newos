#!/usr/bin/env python3
"""Compatibility entry point for the generalized Whisper decoder exporter."""

import runpy
from pathlib import Path


runpy.run_path(
    str(Path(__file__).with_name("export-whisper-decoder.py")),
    run_name="__main__",
)
