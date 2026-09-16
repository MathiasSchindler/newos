"""Bounded CLI checks for the freestanding translator; Python is test-only."""

import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "build/translate.exe")
    parser.add_argument("--hardware", action="store_true")
    options = parser.parse_args()
    binary = options.binary.resolve()
    results = []

    def run(name, arguments, expected_code, expected_text=None):
        started = time.perf_counter()
        result = subprocess.run([str(binary), *arguments], cwd=binary.parent.parent,
                                capture_output=True, timeout=120, check=False)
        output = result.stdout.decode("utf-8", errors="strict")
        diagnostics = result.stderr.decode("utf-8", errors="strict")
        record = dict(name=name, arguments=arguments, exit_code=result.returncode,
                      stdout=output, stderr=diagnostics, wall_seconds=time.perf_counter() - started)
        results.append(record)
        print(json.dumps(record, ensure_ascii=True), flush=True)
        assert result.returncode == expected_code, name
        if expected_text is not None:
            assert output == expected_text, (name, output)
        assert "DSP_INFO" not in output and "Gemma Stage" not in output, (name, output)
        if expected_code == 1:
            assert "prompt restore:" not in diagnostics, name
        if "prompt restore:" in diagnostics:
            assert "cleanup errors: 0" in diagnostics, name
        return record

    try:
        run("help", ["--help"], 0, "")
        run("missing", [], 1, "")
        run("invalid-language", ["--from", "invalid", "--to", "en", "Hello"], 1, "")
        run("missing-value", ["--from"], 1, "")
        run("invalid-limit", ["--from", "de", "--to", "en", "--max-tokens", "513", "Hallo"], 1, "")
        run("over-budget", ["--from", "de", "--to", "en", "Hallo " * 700], 1, "")
        run("duplicate-text", ["--from", "de", "--to", "en", "Hallo", "Welt"], 1, "")
        if options.hardware:
            run("greeting", ["--from", "de", "--to", "en", "--max-tokens", "16", "Guten Tag."], 0, "Good day.\n")
            run("user-sentence", ["--from", "de", "--to", "en",
                "Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts."], 0)
            run("utf8-quotes", ["--from", "en", "--to", "de", "--max-tokens", "32",
                'He said "Good morning."'], 0)
            unicode_result = run("utf8-output", ["--from", "en", "--to", "de", "--max-tokens", "16",
                "The door is open."], 0)
            assert "\u00fc" in unicode_result["stdout"], unicode_result
            run("limit", ["--from", "de", "--to", "en", "--max-tokens", "1", "Guten Tag."], 2, "Good\n")
        print("PASS translator CLI" + (" and NPU integration" if options.hardware else ""), flush=True)
    finally:
        report = binary.parent / "translate-test-results.json"
        report.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()