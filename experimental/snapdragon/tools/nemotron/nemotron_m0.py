"""Development-only M0 input preparation; no inference dependencies."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[4]
CATALOG = Path(__file__).with_name("nemotron-model.json")
DEFAULT_DATA = Path(r"D:\newos-data\nemotron-asr")
FLEURS_REVISION = "70bb2e84b976b7e960aa89f1c648e09c59f894dd"


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=True, indent=2, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, path)


def digest(path, algorithm="sha256", prefix=b""):
    value = hashlib.new(algorithm)
    value.update(prefix)
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def safe_name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or \
            name in (".", "..") or name.endswith((".", " ")) or \
            re.match(r"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", name, re.IGNORECASE):
        raise ValueError(f"Invalid artifact name: {name!r}")
    return name


def read_catalog(path=CATALOG):
    catalog = load_json(path)
    if catalog["schema_version"] != 1 or not re.fullmatch(r"[0-9a-f]{40}", catalog["model"]["revision"]):
        raise ValueError("Unpinned or unsupported model catalog")
    names = set()
    for entry in catalog["files"]:
        name = safe_name(entry["name"])
        if name in names or type(entry["size"]) is not int or entry["size"] <= 0:
            raise ValueError("Duplicate file or invalid artifact size")
        names.add(name)
        identity = entry.get("sha256", entry.get("git_blob_sha1", ""))
        if not re.fullmatch(r"[0-9a-f]{64}|[0-9a-f]{40}", identity):
            raise ValueError(f"Missing immutable identity: {name}")
    return catalog


def verify_file(path, entry):
    path = Path(path)
    if path.stat().st_size != entry["size"]:
        raise ValueError(f"Size mismatch: {path}")
    actual = digest(path)
    if "sha256" in entry and actual != entry["sha256"]:
        raise ValueError(f"SHA-256 mismatch: {path}")
    if "git_blob_sha1" in entry:
        prefix = f"blob {entry['size']}\0".encode("ascii")
        if digest(path, "sha1", prefix) != entry["git_blob_sha1"]:
            raise ValueError(f"Git blob mismatch: {path}")
    return actual


def download(url, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "curl.exe" if os.name == "nt" else "curl",
        "--fail", "--location", "--silent", "--show-error",
        "--retry", "5", "--retry-all-errors", "--continue-at", "-",
        "--output", str(path), url,
    ], check=True)


def fetch(data_dir, weights):
    catalog = read_catalog()
    destination = Path(data_dir) / "checkpoint"
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    for entry in catalog["files"]:
        if entry["role"].endswith("_weights") and weights != "both" and \
                entry["role"] != weights + "_weights":
            continue
        path = destination / entry["name"]
        if not path.exists():
            partial = path.with_name(path.name + ".partial")
            print(f"Fetching {entry['name']}", flush=True)
            url = "https://huggingface.co/{}/resolve/{}/{}?download=true".format(
                catalog["model"]["repository"], catalog["model"]["revision"],
                urllib.parse.quote(entry["name"]),
            )
            download(url, partial)
            verify_file(partial, entry)
            os.replace(partial, path)
        sha256 = verify_file(path, entry)
        records.append({**entry, "sha256": sha256})
    write_json(destination / "source-lock.json", {
        "schema_version": 1, "model": catalog["model"], "files": records,
    })
    license_entry = catalog["license_artifact"]
    license_path = destination / safe_name(license_entry["file"])
    if not license_path.exists():
        partial = license_path.with_name(license_path.name + ".partial")
        download(license_entry["url"], partial)
        verify_file(partial, license_entry)
        os.replace(partial, license_path)
    verify_file(license_path, license_entry)
    license_lock = destination / "license-lock.json"
    record = {"url": "https://openmdw.ai/license/1-1/", "file": license_path.name,
              "sha256": digest(license_path), "size": license_path.stat().st_size}
    if license_lock.exists() and load_json(license_lock) != record:
        raise ValueError("Preserved OpenMDW license changed")
    write_json(license_lock, record)
    sources = Path(data_dir) / "nemo-source"
    revision = catalog["reference"]["nemo_revision"]
    source_records = []
    for entry in catalog["nemo_sources"]:
        name = entry["source"]
        path = sources / safe_name(entry["file"])
        if not path.exists():
            partial = path.with_name(path.name + ".partial")
            download(f"https://raw.githubusercontent.com/NVIDIA-NeMo/Speech/{revision}/{name}", partial)
            verify_file(partial, entry)
            os.replace(partial, path)
        verify_file(path, entry)
        source_records.append({"source": name, "file": path.name, "size": path.stat().st_size,
                               "sha256": digest(path)})
    source_lock = {"repository": catalog["reference"]["nemo_repository"],
                   "revision": revision, "files": source_records}
    source_lock_path = sources / "source-lock.json"
    if source_lock_path.exists() and load_json(source_lock_path) != source_lock:
        raise ValueError("Preserved NeMo sources changed")
    write_json(source_lock_path, source_lock)
    return destination


def verify_checkpoint(directory, required_roles=()):
    directory = Path(directory)
    catalog = read_catalog()
    lock = load_json(directory / "source-lock.json")
    if lock.get("schema_version") != 1 or lock["model"] != catalog["model"]:
        raise ValueError("Checkpoint identity does not match the committed catalog")
    canonical = {entry["name"]: entry for entry in catalog["files"]}
    names = set()
    roles = set()
    for entry in lock["files"]:
        name = safe_name(entry["name"])
        if name in names or name not in canonical:
            raise ValueError("Invalid source-lock file selection")
        names.add(name)
        expected = canonical[name]
        if any(entry.get(key) != value for key, value in expected.items()):
            raise ValueError(f"Source lock disagrees with catalog: {name}")
        if not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", "")):
            raise ValueError("Source lock must materialize SHA-256 for every file")
        verify_file(directory / name, entry)
        roles.add(entry["role"])
    if not set(required_roles).issubset(roles):
        raise ValueError("Checkpoint is missing required weight files")
    if not {entry["name"] for entry in catalog["files"] if not entry["role"].endswith("_weights")}.issubset(names):
        raise ValueError("Checkpoint metadata/tokenizer selection is incomplete")
    license_lock = load_json(directory / "license-lock.json")
    if license_lock != catalog["license_artifact"]:
        raise ValueError("License identity does not match the committed catalog")
    verify_file(directory / safe_name(license_lock["file"]), license_lock)
    return catalog, lock


def read_wav(path):
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("Expected RIFF/WAVE")
    end = struct.unpack_from("<I", data, 4)[0] + 8
    if end != len(data):
        raise ValueError("Truncated WAV or trailing bytes")
    cursor = 12
    fmt = None
    payload = None
    while cursor < end:
        if cursor + 8 > end:
            raise ValueError("Truncated WAV chunk header")
        tag, size = struct.unpack_from("<4sI", data, cursor)
        cursor += 8
        next_cursor = cursor + size + (size & 1)
        if next_cursor > end:
            raise ValueError("Truncated WAV chunk")
        if tag == b"fmt ":
            if fmt is not None or size < 16:
                raise ValueError("Invalid or duplicate WAV format")
            fmt = struct.unpack_from("<HHIIHH", data, cursor)
        elif tag == b"data":
            if payload is not None:
                raise ValueError("Duplicate WAV data")
            payload = data[cursor:cursor + size]
        cursor = next_cursor
    if fmt is None or payload is None:
        raise ValueError("WAV requires fmt and data")
    kind, channels, rate, byte_rate, alignment, bits = fmt
    if channels != 1 or rate != 16000 or (kind, bits) not in ((1, 16), (3, 32)) or \
            alignment != bits // 8 or byte_rate != rate * alignment or len(payload) % alignment:
        raise ValueError("Expected 16 kHz mono PCM16 or float32 WAV")
    if kind == 1:
        samples = [value[0] / 32768 for value in struct.iter_unpack("<h", payload)]
    else:
        samples = [value[0] for value in struct.iter_unpack("<f", payload)]
    if not samples or any(not math.isfinite(value) or abs(value) > 1 for value in samples):
        raise ValueError("Empty, nonfinite or out-of-range WAV")
    return samples


def write_pcm16(path, samples):
    payload = bytearray()
    for sample in samples:
        if not math.isfinite(sample) or abs(sample) > 1:
            raise ValueError("Invalid PCM sample")
        value = min(32767, max(-32768, round(sample * 32768)))
        payload.extend(struct.pack("<h", value))
    Path(path).write_bytes(
        struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(payload), b"WAVE",
                    b"fmt ", 16, 1, 1, 16000, 32000, 2, 16, b"data", len(payload)) + payload
    )


def prepare_corpus(data_dir, source_manifest):
    source_manifest = Path(source_manifest)
    source = load_json(source_manifest)
    if source.get("dataset") != "google/fleurs" or source.get("revision") != FLEURS_REVISION or \
            source.get("license") != "CC-BY-4.0":
        raise ValueError("Expected the rights-documented pinned FLEURS manifest")
    clips = [clip for clip in source["clips"] if clip["config"] == "de_de"]
    if not clips:
        raise ValueError("No German clips in corpus")
    destination = Path(data_dir) / "corpus"
    if destination.exists():
        raise ValueError("Corpus destination already exists; use a new data directory")
    destination.mkdir(parents=True)
    records = []
    for clip in clips:
        path = source_manifest.parent / safe_name(clip["file"])
        verify_file(path, clip)
        samples = read_wav(path)
        if len(samples) != clip["samples"]:
            raise ValueError("Corpus sample count mismatch")
        name = safe_name(Path(clip["file"]).stem + "-pcm16.wav")
        output = destination / name
        write_pcm16(output, samples)
        records.append({
            "id": Path(name).stem, "file": name, "samples": len(samples),
            "size": output.stat().st_size, "sha256": digest(output),
            "kind": "quality", "transcript": clip["transcript"],
            "rights": {"dataset": source["dataset"], "revision": source["revision"],
                       "license": source["license"], "source": clip["source_asset"],
                       "source_sha256": clip["sha256"], "config": "de_de",
                       "split": clip["split"], "row": clip["row"],
                       "attribution": "FLEURS, Google; see preserved dataset card",
                       "modification": "float32 to PCM16, round-to-nearest-even and saturation"},
        })
    source_samples = read_wav(destination / records[0]["file"])
    long_count = read_catalog()["profile"]["long_sequence_chunks"]
    cases = [
        ("short-speech", source_samples[:16000 * 3], "speech_crop"),
        ("pause", source_samples[:16000 * 2] + [0.0] * 16000 + source_samples[16000 * 2:16000 * 4], "pause"),
        ("silence", [0.0] * 16000, "silence"),
        ("boundary", source_samples[:5120 * 2], "chunk_boundary"),
        ("tail", source_samples[:5120 * 2 + 173], "incomplete_tail"),
        ("long", (source_samples + [0.0] * 5120) *
         ((5120 * long_count) // (len(source_samples) + 5120) + 1), "long_sequence"),
    ]
    for name, samples, kind in cases:
        if kind == "long_sequence":
            samples = samples[:5120 * long_count + 173]
        output = destination / (name + ".wav")
        write_pcm16(output, samples)
        records.append({
            "id": name, "file": output.name, "samples": len(samples),
            "size": output.stat().st_size, "sha256": digest(output), "kind": kind,
            "transcript": "" if kind == "silence" else None,
            "rights": {"license": "CC-BY-4.0" if kind != "silence" else "CC0-1.0",
                       "derived_from": records[0]["id"] if kind != "silence" else None,
                       "modification": kind, "quality_scoring": False},
        })
    card = source_manifest.parent / "FLEURS-README.md"
    if not card.is_file():
        raise ValueError("FLEURS dataset card is required for attribution")
    (destination / card.name).write_bytes(card.read_bytes())
    result = {
        "schema_version": 1, "language": "de-DE", "sample_rate": 16000,
        "source_manifest_sha256": digest(source_manifest),
        "dataset_card": {"file": card.name, "size": card.stat().st_size, "sha256": digest(card)},
        "selection": "All de_de entries in pinned local calibration manifest; derived diagnostic fixtures",
        "quality_scope": "Fixed German regression corpus; not a representative command-acceptance corpus",
        "wer_normalization": "Unicode NFC, casefold, punctuation to spaces, whitespace split; no number expansion",
        "clips": records,
    }
    write_json(destination / "manifest.json", result)
    return destination


def fetch_corpus(data_dir):
    catalog_path = Path(__file__).with_name("nemotron-corpus.json")
    catalog = load_json(catalog_path)
    if catalog["revision"] != FLEURS_REVISION or catalog["license"] != "CC-BY-4.0":
        raise ValueError("Unexpected corpus revision or license")
    destination = Path(data_dir) / "fleurs"
    destination.mkdir(parents=True, exist_ok=True)
    for clip in catalog["clips"]:
        path = destination / safe_name(clip["file"])
        if not path.exists():
            reuse = ROOT / "experimental" / "snapdragon" / "models" / "calibration" / "fleurs" / path.name
            if reuse.is_file() and digest(reuse) == clip["sha256"]:
                path.write_bytes(reuse.read_bytes())
            else:
                partial = path.with_name(path.name + ".partial")
                url = "https://datasets-server.huggingface.co/rows?dataset=google%2Ffleurs&config=de_de&split=validation&offset={}&length=1".format(clip["row"])
                with urllib.request.urlopen(url, timeout=60) as response:
                    row = json.load(response)["rows"][0]["row"]
                asset = row["audio"][0]["src"]
                if asset.split("?")[0] != clip["source_asset"] or row["id"] != clip["id"]:
                    raise ValueError("Refreshed corpus asset differs from pinned catalog")
                download(asset, partial)
                verify_file(partial, clip)
                os.replace(partial, path)
        verify_file(path, clip)
    card = destination / catalog["dataset_card"]["file"]
    if not card.exists():
        partial = card.with_name(card.name + ".partial")
        download(catalog["dataset_card"]["url"], partial)
        verify_file(partial, catalog["dataset_card"])
        os.replace(partial, card)
    verify_file(card, catalog["dataset_card"])
    write_json(destination / "manifest.json", catalog)
    return destination


def pin_corpus(data_dir):
    """One-time catalog generation; normal fetches never consult mutable rows."""
    url = "https://datasets-server.huggingface.co/rows?dataset=google%2Ffleurs&config=de_de&split=validation&offset=0&length=10"
    with urllib.request.urlopen(url, timeout=60) as response:
        rows = json.load(response)["rows"]
    if len(rows) != 10 or [record["row_idx"] for record in rows] != list(range(10)):
        raise ValueError("Unexpected FLEURS row selection")
    destination = Path(data_dir) / "fleurs"
    destination.mkdir(parents=True, exist_ok=True)
    clips = []
    for record in rows:
        row = record["row"]
        asset = row["audio"][0]["src"].split("?")[0]
        if f"/--/{FLEURS_REVISION}/--/de_de/validation/{record['row_idx']}/" not in asset:
            raise ValueError("Corpus asset is not from the pinned dataset revision")
        path = destination / safe_name(f"de_de-{row['id']}.wav")
        if not path.exists():
            reuse = ROOT / "experimental" / "snapdragon" / "models" / "calibration" / "fleurs" / path.name
            if reuse.is_file():
                path.write_bytes(reuse.read_bytes())
            else:
                partial = path.with_name(path.name + ".partial")
                download(row["audio"][0]["src"], partial)
                os.replace(partial, path)
        samples = read_wav(path)
        if len(samples) != row["num_samples"]:
            raise ValueError("Corpus asset sample count mismatch")
        clips.append({"config": "de_de", "split": "validation", "row": record["row_idx"],
                      "id": row["id"], "file": path.name, "samples": len(samples),
                      "size": path.stat().st_size, "sha256": digest(path),
                      "transcript": row["transcription"], "source_asset": asset})
    card = destination / "FLEURS-README.md"
    card_url = f"https://huggingface.co/datasets/google/fleurs/resolve/{FLEURS_REVISION}/README.md"
    if not card.exists():
        download(card_url, card)
    catalog = {"schema_version": 1, "dataset": "google/fleurs", "revision": FLEURS_REVISION,
               "license": "CC-BY-4.0", "selection": "de_de validation rows 0 through 9",
               "dataset_card": {"file": card.name, "url": card_url,
                                "size": card.stat().st_size, "sha256": digest(card)},
               "clips": clips}
    write_json(destination / "manifest.json", catalog)
    write_json(Path(__file__).with_name("nemotron-corpus.json"), catalog)
    return destination


def inspect_checkpoint(directory, output):
    import yaml

    catalog, lock = verify_checkpoint(directory, ("nemo_weights",))
    archive = Path(directory) / "nemotron-3.5-asr-streaming-0.6b.nemo"
    members = []
    config = None
    with tarfile.open(archive, "r:*") as stream:
        for member in stream:
            if member.isfile():
                members.append({"name": member.name, "size": member.size})
                if member.name.removeprefix("./") == "model_config.yaml":
                    if config is not None or member.size > 4 * 1024 * 1024:
                        raise ValueError("Invalid NeMo model config")
                    config = yaml.safe_load(stream.extractfile(member).read())
    if not isinstance(config, dict):
        raise ValueError("NeMo archive has no model_config.yaml")
    write_json(output, {
        "schema_version": 1, "model": catalog["model"],
        "source_lock_sha256": digest(Path(directory) / "source-lock.json"),
        "nemo_source_revision": catalog["reference"]["nemo_revision"],
        "nemo_parity": "not_verified", "archive_members": members,
        "model_config": config,
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    commands = parser.add_subparsers(dest="command", required=True)
    fetch_parser = commands.add_parser("fetch")
    fetch_parser.add_argument("--weights", choices=("both", "transformers", "nemo", "none"), default="both")
    commands.add_parser("verify")
    commands.add_parser("fetch-corpus")
    commands.add_parser("pin-corpus")
    corpus = commands.add_parser("corpus")
    corpus.add_argument("--source-manifest", type=Path)
    commands.add_parser("inspect")
    args = parser.parse_args()
    if args.command == "fetch":
        print(fetch(args.data_dir, args.weights))
    elif args.command == "verify":
        verify_checkpoint(args.data_dir / "checkpoint", ("transformers_weights", "nemo_weights"))
        print("PASS checkpoint hashes and license")
    elif args.command == "corpus":
        print(prepare_corpus(args.data_dir, args.source_manifest or args.data_dir / "fleurs" / "manifest.json"))
    elif args.command == "fetch-corpus":
        print(fetch_corpus(args.data_dir))
    elif args.command == "pin-corpus":
        print(pin_corpus(args.data_dir))
    else:
        inspect_checkpoint(args.data_dir / "checkpoint", args.data_dir / "nemo-config.json")


if __name__ == "__main__":
    main()
