#!/usr/bin/env python3
"""Offline quality evaluation; never changes deployment artifacts or decoding rules."""

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unicodedata


TOOLS = Path(__file__).resolve().parent
CORPUS = TOOLS / "translategemma-quality.json"
VARIANTS = ("bf16", "w8a16", "w4a16")
RATINGS = ("meaning", "omission", "hallucination", "terminology", "unwanted_content")
PILOT_POLICY = {"cases": 24, "maximum_major_cases": 0, "maximum_minor_cases": 4,
                "require_termination": True, "allow_empty_output": False}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), TOOLS / (name + ".py"))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def split_digest(corpus, split):
    cases = [case for case in corpus["cases"] if case["split"] == split]
    encoded = json.dumps(cases, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_corpus(path=CORPUS):
    corpus = json.loads(Path(path).read_text(encoding="utf-8"))
    if corpus["schema_version"] != 1 or not corpus.get("provenance"):
        raise ValueError("unsupported or unattributed corpus")
    identifiers, texts = set(), set()
    for case in corpus["cases"]:
        key = " ".join(unicodedata.normalize("NFC", case["text"]).casefold().split())
        if case["id"] in identifiers or key in texts:
            raise ValueError("duplicate ID or source text across corpus splits")
        if case["split"] not in ("diagnostic", "heldout") or not key:
            raise ValueError("invalid split or empty source")
        if not case["references"] or not all(isinstance(text, str) and text.strip() for text in case["references"]):
            raise ValueError("missing references")
        if not case["tags"] or not case["meaning"] or not case["source"] or not case["target"]:
            raise ValueError("missing case annotations")
        identifiers.add(case["id"])
        texts.add(key)
    if Counter(case["split"] for case in corpus["cases"]) != {"diagnostic": 24, "heldout": 24}:
        raise ValueError("expected 24 diagnostic and 24 held-out examples")
    return corpus


def select_cases(corpus, split, identifiers=None):
    if split == "heldout":
        review = corpus["review"][split]
        if review["status"] != "approved" or not review.get("reviewer", "").strip() or review.get("reviewer_kind") not in ("human", "ai"):
            raise ValueError("heldout references require an identified human or AI semantic review before use")
        if review.get("cases_sha256") != split_digest(corpus, split):
            raise ValueError("heldout cases changed after review; review and freeze their hash again")
        if identifiers:
            raise ValueError("heldout evaluation must include the entire frozen split")
    cases = [case for case in corpus["cases"] if case["split"] == split]
    if identifiers:
        requested = set(identifiers)
        if not requested <= {case["id"] for case in cases}:
            raise ValueError("unknown or cross-split case selection")
        cases = [case for case in cases if case["id"] in requested]
    return cases


def output_health(tokens, translation, maximum):
    if not tokens or len(tokens) > maximum or any(token < 0 or token >= 262145 for token in tokens):
        raise ValueError("invalid generated token sequence")
    if any(token in (1, 106) for token in tokens[:-1]):
        raise ValueError("generation continued after stop")
    stopped = tokens[-1] in (1, 106)
    if not stopped and len(tokens) != maximum:
        raise ValueError("truncated diagnostic result")
    translation.encode("utf-8", errors="strict")
    counts = Counter(tuple(tokens[start:start + 4]) for start in range(max(0, len(tokens) - 3)))
    return {"terminated": stopped, "token_limit": not stopped,
            "repeated_4gram_flag": any(count >= 3 for count in counts.values()),
            "replacement_character_flag": "\ufffd" in translation,
            "empty_output": not translation.strip()}


def save(path, data):
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(data, stream, ensure_ascii=True, indent=2)
            stream.write("\n")
        except BaseException:
            stream.close()
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def quantize_grouped_w4(values, group_size, exporter=None):
    import numpy as np

    exporter = exporter or module("export-translategemma")
    values = np.asarray(values, dtype=np.float32)
    if values.ndim != 2 or group_size <= 0 or group_size % 2 or values.shape[1] == 0 or values.shape[1] % group_size:
        raise ValueError("grouped W4 requires nonempty rows divisible by an even group size")
    quantized, scales = exporter.quantize_groups(values.reshape(-1, group_size), 4)
    return exporter.pack_s4(quantized.reshape(values.shape)), scales.reshape(values.shape[0], -1)


def dequantize_grouped_w4(packed, scales, group_size):
    import numpy as np

    quantized = np.empty((packed.shape[0], packed.shape[1] * 2), dtype=np.int8)
    quantized[:, 0::2] = (packed << np.uint8(4)).view(np.int8) >> 4
    quantized[:, 1::2] = packed.view(np.int8) >> 4
    grouped = quantized.reshape(packed.shape[0], -1, group_size).astype(np.float32)
    return (grouped * scales.astype(np.float32)[..., None]).reshape(quantized.shape)


def candidate_config(base, layers=(), head=False):
    if base not in ("w4a16", "group32") or len(set(layers)) != len(layers) or any(type(layer) is not int or not 0 <= layer < 34 for layer in layers):
        raise ValueError("candidate requires w4a16/group32 and unique layer numbers 0..33")
    if type(head) is not bool:
        raise ValueError("candidate head must be boolean")
    return {"base": base, "w8_layers": sorted(layers), "w8_head": head}


def candidate_weights(reference, exporter, config):
    config = candidate_config(config["base"], config["w8_layers"], config["w8_head"])
    root = TOOLS.parent

    class CandidateWeights(reference.Weights):
        def __init__(self):
            super().__init__(exporter, root / "data/translategemma-4b", root / "models/translategemma-4b-stage3", "w4a16")
            self.quantized_rows = {}
            self.w8 = reference.Weights(exporter, self.model_dir, self.artifact_dir, "w8a16") if config["w8_layers"] or config["w8_head"] else None

        def rows(self, name, start=None, end=None):
            if name.startswith("layers.") and int(name.split(".")[1]) in config["w8_layers"]:
                return self.w8.rows(name, start, end)
            full_name = reference.PREFIX + name
            shape = self.entries[full_name]["shape"]
            if config["base"] != "group32" or len(shape) == 1:
                return super().rows(name, start, end)
            start = 0 if start is None else start
            end = shape[0] if end is None else end
            key = (name, start, end)
            if key not in self.quantized_rows:
                values = exporter.bf16_to_f32(self.source(full_name)[start:end])
                self.quantized_rows[key] = quantize_grouped_w4(values, 32, exporter)
                if end == shape[0]:
                    exporter.close_memmap(self.mappings.pop(full_name))
            packed, scales = self.quantized_rows[key]
            return dequantize_grouped_w4(packed, scales, 32)

        def linear(self, values, name, rows):
            if config["w8_head"] and name == "embed_tokens.weight":
                return self.w8.linear(values, name, rows)
            return super().linear(values, name, rows)

        def close(self):
            try:
                super().close()
            finally:
                self.quantized_rows.clear()
                if self.w8 is not None:
                    self.w8.close()

    return CandidateWeights()


def candidate_payload(config):
    manifest = json.loads((TOOLS.parent / "models/translategemma-4b-stage3/manifest.json").read_text())
    total = 0
    for entry in manifest["variants"]["w4a16"]["artifacts"]:
        shape = entry["shape"]
        if len(shape) == 1:
            total += shape[0] * 2
            continue
        name = entry["name"].removeprefix("language_model.model.")
        count = shape[0] * shape[1]
        promoted = name.startswith("layers.") and int(name.split(".")[1]) in config["w8_layers"]
        total += count + shape[0] * 2 if promoted else count // 2 + (count // 16 if config["base"] == "group32" else shape[0] * 2)
        if config["w8_head"] and name == "embed_tokens.weight":
            total += count + shape[0] * 2
    return total


def semantic_gate(report, packet, variant):
    reviews = {item["review_id"]: item for item in packet["ratings"]}
    selected = [item for item in report["results"] if item["variant"] == variant]
    major = minor = unhealthy = 0
    for result in selected:
        ratings = [reviews[result["review_id"]][dimension] for dimension in RATINGS]
        major += "major" in ratings
        minor += "minor" in ratings and "major" not in ratings
        health = output_health(result["generated_ids"], result["translation"], report["maximum_new_tokens"])
        unhealthy += not health["terminated"] or health["empty_output"] or health["replacement_character_flag"]
    return {"passed": len(selected) == PILOT_POLICY["cases"] and major <= PILOT_POLICY["maximum_major_cases"]
            and minor <= PILOT_POLICY["maximum_minor_cases"] and unhealthy == 0,
            "major_cases": major, "minor_cases": minor, "unhealthy_cases": unhealthy, "cases": len(selected)}


def validate_inventory(report, corpus, split):
    expected = select_cases(corpus, split)
    if report["split"] != split or len(report["cases"]) != len(expected):
        raise ValueError("evaluation requires the complete split")
    if [{key: case.get(key) for key in original} for case, original in zip(report["cases"], expected)] != expected:
        raise ValueError("evaluation cases differ from the frozen corpus")


def freeze_candidate(report_path, review_path, corpus_path):
    score(report_path, review_path)
    report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    packet = json.loads(Path(review_path).read_text(encoding="utf-8"))
    corpus = load_corpus(corpus_path)
    validate_inventory(report, corpus, "diagnostic")
    if "candidate" not in report["variants"] or not semantic_gate(report, packet, "candidate")["passed"]:
        raise ValueError("candidate must pass the reviewed diagnostic pilot gate before freezing")
    if report["candidate_implementation_sha256"] != digest(__file__):
        raise ValueError("candidate implementation changed since diagnostic evaluation")
    return {"schema_version": 1, "candidate": report["candidate"], "maximum_new_tokens": report["maximum_new_tokens"],
            "policy": PILOT_POLICY, "variants": [*VARIANTS, "candidate"],
            "heldout_cases_sha256": split_digest(corpus, "heldout"),
            "diagnostic_report_sha256": digest(report_path), "diagnostic_reviews_sha256": digest(review_path),
            "frozen_by": packet["reviewer"], "reviewer_kind": packet["reviewer_kind"],
            **{key: report[key] for key in ("corpus_sha256", "reference_sha256", "weight_manifest_sha256",
               "tokenizer_sha256", "tokenizer_config_sha256", "config_sha256", "candidate_implementation_sha256")}}


def validate_frozen(frozen, prepared, variants):
    if frozen.get("schema_version") != 1 or frozen["policy"] != PILOT_POLICY or variants != frozen["variants"]:
        raise ValueError("frozen policy or variant inventory changed")
    if prepared["maximum_new_tokens"] != frozen["maximum_new_tokens"] or prepared["candidate"] != frozen["candidate"]:
        raise ValueError("candidate or decoding budget differs from frozen selection")
    if digest(__file__) != frozen["candidate_implementation_sha256"]:
        raise ValueError("implementation changed after candidate freeze")
    for key in ("corpus_sha256", "reference_sha256", "weight_manifest_sha256", "tokenizer_sha256",
                "tokenizer_config_sha256", "config_sha256"):
        if prepared[key] != frozen[key]:
            raise ValueError("frozen provenance changed: " + key)
    cases = [{key: value for key, value in case.items() if key != "prompt_ids"} for case in prepared["cases"]]
    if split_digest({"cases": cases}, "heldout") != frozen["heldout_cases_sha256"]:
        raise ValueError("heldout cases differ from frozen selection")


def evaluate_heldout(report_path, review_path, frozen_path, corpus_path):
    measured = score(report_path, review_path)
    report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    packet = json.loads(Path(review_path).read_text(encoding="utf-8"))
    frozen = json.loads(Path(frozen_path).read_text(encoding="utf-8"))
    validate_inventory(report, load_corpus(corpus_path), "heldout")
    validate_frozen(frozen, report, report["variants"])
    if report.get("frozen_selection_sha256") != digest(frozen_path):
        raise ValueError("heldout run was not bound to this frozen selection")
    gates = {variant: semantic_gate(report, packet, variant) for variant in report["variants"]}
    return {**measured, "pilot_gates": gates, "pilot_quality_accepted": gates["candidate"]["passed"],
            "deployment_accepted": False, "policy": PILOT_POLICY,
            "limitation": "24-case AI-reviewed pilot; not independent validation or hardware acceptance"}


def prepare(corpus_path, split, identifiers, maximum):
    from transformers import AutoTokenizer

    corpus = load_corpus(corpus_path)
    cases = select_cases(corpus, split, identifiers)
    model = TOOLS.parent / "data/translategemma-4b"
    tokenizer = AutoTokenizer.from_pretrained(model, local_files_only=True)
    prepared = []
    for case in cases:
        prompt = tokenizer.apply_chat_template([{"role": "user", "content": [{"type": "text",
            "source_lang_code": case["source"], "target_lang_code": case["target"], "text": case["text"]}]}],
            tokenize=True, add_generation_prompt=True, return_dict=False)
        if len(prompt) + maximum > 2048 or not prompt:
            raise ValueError("case exceeds context budget: " + case["id"])
        prepared.append({**case, "prompt_ids": prompt})
    return {"schema_version": 1, "split": split, "corpus_sha256": digest(corpus_path),
            "tokenizer_sha256": digest(model / "tokenizer.json"),
            "tokenizer_config_sha256": digest(model / "tokenizer_config.json"),
            "config_sha256": digest(model / "config.json"),
            "reference_sha256": digest(TOOLS / "translategemma-reference.py"),
            "weight_manifest_sha256": digest(TOOLS.parent / "models/translategemma-4b-stage3/manifest.json"),
            "reference_review": corpus["review"][split], "maximum_new_tokens": maximum,
            "global_rope_factor": 8, "quantized_residual_divisor": 32, "cases": prepared}


def resume_results(prepared, variants, path):
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    if report["variants"] != variants or any(report.get(key) != value for key, value in prepared.items()):
        raise ValueError("resume requires identical prompts, provenance, candidates and decoding settings")
    expected = {(case["id"], variant) for case in prepared["cases"] for variant in variants}
    seen = set()
    for result in report["results"]:
        key = result["case_id"], result["variant"]
        if key not in expected or key in seen:
            raise ValueError("duplicate or foreign result in resume checkpoint")
        seen.add(key)
        health = output_health(result["generated_ids"], result["translation"], prepared["maximum_new_tokens"])
        review_id = hashlib.sha256((result["variant"] + result["case_id"] + result["translation"]).encode("utf-8")).hexdigest()
        if result["health"] != health or result["review_id"] != review_id:
            raise ValueError("inconsistent result in resume checkpoint")
    return report["results"]


def generate(prepared, variants, destination, resume_path=None):
    from transformers import AutoTokenizer

    reference = module("translategemma-reference")
    exporter = module("export-translategemma")
    root = TOOLS.parent
    tokenizer = AutoTokenizer.from_pretrained(root / "data/translategemma-4b", local_files_only=True)
    report = {**prepared, "variants": variants, "complete": False, "quality_accepted": False, "results": []}
    if resume_path:
        report["results"] = resume_results(prepared, variants, resume_path)
        report["resumed_from_sha256"] = digest(resume_path)
    completed = {(item["case_id"], item["variant"]) for item in report["results"]}
    save(destination, report)
    for variant in variants:
        if all((case["id"], variant) in completed for case in prepared["cases"]):
            continue
        weights = candidate_weights(reference, exporter, prepared["candidate"]) if variant == "candidate" else reference.Weights(
            exporter, root / "data/translategemma-4b", root / "models/translategemma-4b-stage3", variant)
        try:
            engine = reference.Reference(weights, residual_scale=1 if variant == "bf16" else 32)
            for case in prepared["cases"]:
                if (case["id"], variant) in completed:
                    continue
                tokens = engine.generate(case["prompt_ids"], prepared["maximum_new_tokens"])
                translation = tokenizer.decode(tokens, skip_special_tokens=True)
                review_id = hashlib.sha256((variant + case["id"] + translation).encode("utf-8")).hexdigest()
                report["results"].append({"case_id": case["id"], "variant": variant,
                    "review_id": review_id, "generated_ids": tokens, "translation": translation,
                    "health": output_health(tokens, translation, prepared["maximum_new_tokens"])})
                save(destination, report)
                print(f"Finished {variant} {case['id']}: {len(tokens)} tokens", flush=True)
        finally:
            weights.close()
    report["complete"] = True
    save(destination, report)
    cases = {case["id"]: case for case in prepared["cases"]}
    packet = {"report_sha256": digest(destination), "reviewer": "", "reviewer_kind": "", "ratings": []}
    for result in sorted(report["results"], key=lambda result: result["review_id"]):
        case = cases[result["case_id"]]
        packet["ratings"].append({"review_id": result["review_id"], "source": case["source"],
            "target": case["target"], "text": case["text"], "translation": result["translation"],
            "meaning_check": case["meaning"], "reference_examples": case["references"],
            **{rating: None for rating in RATINGS}, "notes": ""})
    save(str(destination) + ".reviews.json", packet)


def score(path, review_path=None):
    import sacrebleu

    if sacrebleu.__version__ != "2.5.1":
        raise ValueError("quality scoring requires sacrebleu==2.5.1")
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    if not report["complete"]:
        raise ValueError("refuse to score an incomplete run")
    cases = {case["id"]: case for case in report["cases"]}
    expected = {(case, variant) for case in cases for variant in report["variants"]}
    results = {(result["case_id"], result["variant"]): result for result in report["results"]}
    if len(results) != len(report["results"]) or set(results) != expected:
        raise ValueError("duplicate or missing results")
    reviews = {}
    reviewer_kind = None
    if review_path:
        packet = json.loads(Path(review_path).read_text(encoding="utf-8"))
        reviewer_kind = packet.get("reviewer_kind")
        if packet["report_sha256"] != digest(path) or not packet["reviewer"].strip() or reviewer_kind not in ("human", "ai"):
            raise ValueError("reviews need an identified human or AI reviewer and matching report hash")
        reviews = {item["review_id"]: item for item in packet["ratings"]}
        if len(reviews) != len(packet["ratings"]) or set(reviews) != {item["review_id"] for item in results.values()}:
            raise ValueError("reviews must cover every result exactly once")
        if any(item[rating] not in ("correct", "minor", "major") for item in reviews.values() for rating in RATINGS):
            raise ValueError("every review dimension requires correct, minor or major")
    metric = sacrebleu.metrics.CHRF(char_order=6, word_order=2, beta=2)
    summary = {"report_sha256": digest(path), "quality_accepted": False,
        "semantic_review_complete": bool(reviews), "reviewer_kind": reviewer_kind,
        "human_review_complete": bool(reviews) and reviewer_kind == "human", "reference_review": report["reference_review"],
        "interpretation": "chrF++ is descriptive; exact BF16 agreement is not semantic correctness; no acceptance threshold is inferred",
        "variants": {}}
    for variant in report["variants"]:
        selected = [results[(identifier, variant)] for identifier in cases]
        hypotheses = [result["translation"] for result in selected]
        references = [cases[result["case_id"]]["references"] for result in selected]
        reference_streams = [[texts[min(index, len(texts) - 1)] for texts in references]
                             for index in range(max(map(len, references)))]
        measured = metric.corpus_score(hypotheses, reference_streams)
        pairs = {}
        for result in selected:
            case = cases[result["case_id"]]
            pair = case["source"] + "->" + case["target"]
            pairs.setdefault(pair, []).append(metric.sentence_score(result["translation"], case["references"]).score)
        health = Counter(key for result in selected for key, value in result["health"].items() if value)
        baseline = [results.get((result["case_id"], "bf16")) for result in selected]
        summary["variants"][variant] = {"chrf_pp": measured.score,
            "pair_mean_sentence_chrf_pp": {pair: sum(values) / len(values) for pair, values in pairs.items()},
            "health_counts": dict(health), "cases": len(selected),
            "exact_bf16_sequences": sum(result["generated_ids"] == base["generated_ids"]
                for result, base in zip(selected, baseline)) if all(baseline) else None,
            "review_counts": {rating: dict(Counter(reviews[result["review_id"]][rating] for result in selected))
                              for rating in RATINGS} if reviews else None}
    summary["metric"] = {"library": "sacrebleu", "version": sacrebleu.__version__, "signature": str(metric.get_signature())}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate", "prepare", "run", "score", "freeze", "evaluate"))
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--split", choices=("diagnostic", "heldout"), default="diagnostic")
    parser.add_argument("--case", action="append", dest="identifiers")
    parser.add_argument("--variants", nargs="+", choices=(*VARIANTS, "candidate"), default=list(VARIANTS))
    parser.add_argument("--candidate-base", choices=("w4a16", "group32"), default="group32")
    parser.add_argument("--w8-layer", type=int, action="append", default=[])
    parser.add_argument("--w8-head", action="store_true")
    parser.add_argument("--maximum", type=int, default=128)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--reviews", type=Path)
    parser.add_argument("--frozen", type=Path)
    parser.add_argument("--resume", type=Path)
    args = parser.parse_args()
    if args.resume and args.action != "run":
        parser.error("--resume is only valid for run")
    if args.action == "validate":
        corpus = load_corpus(args.corpus)
        print("PASS corpus: " + str(Counter(case["split"] for case in corpus["cases"])))
        print("Heldout cases hash (record only after semantic review): " + split_digest(corpus, "heldout"))
        return
    if args.action == "score":
        if not args.report:
            parser.error("score requires --report")
        print(json.dumps(score(args.report, args.reviews), indent=2))
        return
    if args.action == "evaluate":
        if not args.report or not args.reviews or not args.frozen:
            parser.error("evaluate requires --report, --reviews and --frozen")
        print(json.dumps(evaluate_heldout(args.report, args.reviews, args.frozen, args.corpus), indent=2))
        return
    if not args.output or args.output.exists() or Path(str(args.output) + ".reviews.json").exists():
        parser.error("provide a new --output path; existing reports are never overwritten")
    if args.action == "freeze":
        if not args.report or not args.reviews:
            parser.error("freeze requires --report and --reviews")
        frozen = freeze_candidate(args.report, args.reviews, args.corpus)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        save(args.output, frozen)
        print("Frozen reviewed candidate; no heldout inference performed")
        return
    if not 1 <= args.maximum <= 512 or len(set(args.variants)) != len(args.variants):
        parser.error("maximum must be 1..512 and variants must be unique")
    if args.split == "heldout" and not set(VARIANTS) <= set(args.variants):
        parser.error("heldout evaluation requires BF16, W8 and W4 baselines together")
    if args.split == "heldout" and (not args.frozen or "candidate" not in args.variants):
        parser.error("heldout evaluation requires --frozen and the candidate alongside all baselines")
    prepared = prepare(args.corpus, args.split, args.identifiers, args.maximum)
    if "candidate" in args.variants:
        prepared["candidate"] = candidate_config(args.candidate_base, args.w8_layer, args.w8_head)
        prepared["candidate_implementation_sha256"] = digest(__file__)
        prepared["candidate_hardware_accepted"] = False
        prepared["candidate_payload_bytes"] = candidate_payload(prepared["candidate"])
    if args.frozen:
        if args.split != "heldout":
            parser.error("--frozen is reserved for heldout evaluation")
        validate_frozen(json.loads(args.frozen.read_text(encoding="utf-8")), prepared, args.variants)
        prepared["frozen_selection_sha256"] = digest(args.frozen)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.action == "prepare":
        save(args.output, prepared)
        print("Prepared " + str(len(prepared["cases"])) + " prompts; no inference performed")
    else:
        generate(prepared, args.variants, args.output, args.resume)


if __name__ == "__main__":
    main()