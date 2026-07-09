#!/usr/bin/env python3
import hashlib
import http.server
import os
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

DATE = "2026-07-09"
WIKI = "testwiki"


def dump_name(index):
    return f"{WIKI}-{DATE}-p{index}.xml.bz2"


SCENARIOS = {
    "ok": [b"slow-first", b"fast-second", b"fast-third", b"slot-reused-fourth"],
    "resume": [b"resume-payload"],
    "fallback": [b"fallback-payload"],
    "file-redirect": [b"redirected-payload"],
    "bad-range": [b"range-payload"],
    "unknown-range-total": [b"unknown-total-payload"],
    "checksum-retry": [b"checksum-payload"],
    "bad-checksum": [b"expected-payload"],
    "stall": [b"stall-payload"],
    "truncated": [b"unused"],
    "many": [f"many-{index}".encode() for index in range(70)],
    "no-size": [b"already-complete"],
}


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.counts = {}
        self.slow_done = None

    def record(self, scenario, leaf, range_header):
        with self.lock:
            now = time.monotonic()
            self.requests.append((scenario, leaf, range_header, now))
            key = (scenario, leaf)
            self.counts[key] = self.counts.get(key, 0) + 1
            return self.counts[key]


STATE = State()


def listing_for(scenario):
    lines = []
    for index, data in enumerate(SCENARIOS[scenario], 1):
        name = dump_name(index)
        suffix = "" if scenario == "no-size" else f" {len(data)}"
        lines.append(f'<a href="{name}">{name}</a>{suffix}\n')
    return "".join(lines).encode()


def manifest_for(scenario):
    lines = []
    for index, data in enumerate(SCENARIOS[scenario], 1):
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {dump_name(index)}\n")
    return "".join(lines).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send_bytes(self, body, status=200, headers=None):
        self.send_response(status)
        for name, value in headers or []:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def send_chunked(self, body):
        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        midpoint = max(1, len(body) // 2)
        for chunk in (body[:midpoint], body[midpoint:]):
            if chunk:
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")

    def do_GET(self):
        parts = self.path.split("?", 1)[0].strip("/").split("/")
        if len(parts) == 3 and parts[0] == "object" and parts[1] in SCENARIOS:
            scenario = parts[1]
            names = [dump_name(index) for index in range(1, len(SCENARIOS[scenario]) + 1)]
            if parts[2] not in names:
                self.send_error(404)
                return
            self.send_bytes(SCENARIOS[scenario][names.index(parts[2])])
            return
        if len(parts) == 2 and parts[0] == "manifest":
            scenario = parts[1]
            self.send_bytes(manifest_for(scenario))
            return
        if len(parts) < 2 or parts[0] not in SCENARIOS:
            self.send_error(404)
            return
        scenario = parts[0]
        leaf = parts[-1] if parts else ""
        range_header = self.headers.get("Range")
        attempt = STATE.record(scenario, leaf, range_header)

        if leaf == "bzip2":
            body = listing_for(scenario)
            if scenario == "truncated":
                self.send_response(200)
                self.send_header("Content-Length", str(len(body) + 10))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True
                return
            if scenario == "ok":
                self.send_chunked(body)
            else:
                self.send_bytes(body)
            return
        if leaf == "SHA256SUMS":
            if scenario == "ok":
                self.send_response(302)
                self.send_header("Location", "/manifest/ok")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
            else:
                self.send_bytes(manifest_for(scenario))
            return

        names = [dump_name(index) for index in range(1, len(SCENARIOS[scenario]) + 1)]
        if leaf not in names:
            self.send_error(404)
            return
        index = names.index(leaf)
        body = SCENARIOS[scenario][index]
        if scenario == "ok" and index == 0:
            time.sleep(0.8)
        if scenario == "checksum-retry" and attempt == 1:
            body = b"X" * len(body)
        if scenario == "bad-checksum":
            body = b"X" * len(body)
        if scenario == "file-redirect":
            self.send_response(302)
            self.send_header("Location", f"/object/{scenario}/{leaf}")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if scenario == "stall":
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body[:1])
            self.wfile.flush()
            time.sleep(0.5)
            return

        if scenario == "ok" and index == 3:
            self.send_chunked(body)
            return

        if range_header:
            try:
                offset = int(range_header.removeprefix("bytes=").removesuffix("-"))
            except ValueError:
                self.send_error(400)
                return
            if scenario == "fallback":
                self.send_bytes(body)
                return
            start = offset + 1 if scenario == "bad-range" else offset
            ranged = body[offset:]
            total = "*" if scenario == "unknown-range-total" else str(len(body))
            self.send_bytes(
                ranged,
                status=206,
                headers=[("Content-Range", f"bytes {start}-{len(body) - 1}/{total}")],
            )
            return

        self.send_bytes(body)
        if scenario == "ok" and index == 0:
            with STATE.lock:
                STATE.slow_done = time.monotonic()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def run_tool(binary, port, scenario, output_dir, *extra):
    command = [
        str(binary),
        "--base-url", f"http://127.0.0.1:{port}/{scenario}",
        "--date", DATE,
        "--output-dir", str(output_dir),
        "--quiet",
        *extra,
        "test",
    ]
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)


def require(condition, message, result=None):
    if condition:
        return
    if result is not None:
        sys.stderr.write(result.stderr.decode(errors="replace"))
    raise AssertionError(message)


def main():
    root = pathlib.Path(__file__).resolve().parent
    binary = root / "build" / "wp-download"
    require(binary.is_file(), f"missing {binary}")
    server = Server(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]

    try:
        with tempfile.TemporaryDirectory(prefix="newos-wp-download-") as temp:
            temp = pathlib.Path(temp)

            out = temp / "ok"
            out.mkdir()
            result = run_tool(binary, port, "ok", out, "--jobs", "3", "--retries", "0")
            require(result.returncode == 0, "chunked/redirect/parallel download failed", result)
            for index, data in enumerate(SCENARIOS["ok"], 1):
                require((out / dump_name(index)).read_bytes() == data, "downloaded data mismatch")
                require(not (out / (dump_name(index) + ".part")).exists(), "verified partial was not promoted")
            with STATE.lock:
                fourth_times = [entry[3] for entry in STATE.requests if entry[0] == "ok" and entry[1] == dump_name(4)]
                slow_done = STATE.slow_done
            require(fourth_times and slow_done is not None and fourth_times[0] < slow_done, "parallel slot was not reused in completion order")

            out = temp / "resume"
            out.mkdir()
            (out / (dump_name(1) + ".part")).write_bytes(SCENARIOS["resume"][0][:4])
            result = run_tool(binary, port, "resume", out, "--retries", "0")
            require(result.returncode == 0, "valid range resume failed", result)
            require((out / dump_name(1)).read_bytes() == SCENARIOS["resume"][0], "resumed data mismatch")

            out = temp / "fallback"
            out.mkdir()
            (out / (dump_name(1) + ".part")).write_bytes(b"old")
            result = run_tool(binary, port, "fallback", out, "--retries", "0")
            require(result.returncode == 0 and (out / dump_name(1)).read_bytes() == SCENARIOS["fallback"][0], "200 range fallback failed", result)

            out = temp / "file-redirect"
            out.mkdir()
            result = run_tool(binary, port, "file-redirect", out, "--retries", "0")
            require(result.returncode == 0 and (out / dump_name(1)).read_bytes() == SCENARIOS["file-redirect"][0], "file redirect failed", result)

            out = temp / "bad-range"
            out.mkdir()
            partial = out / (dump_name(1) + ".part")
            partial.write_bytes(SCENARIOS["bad-range"][0][:3])
            result = run_tool(binary, port, "bad-range", out, "--retries", "0")
            require(result.returncode != 0 and not (out / dump_name(1)).exists(), "malformed Content-Range was accepted", result)

            out = temp / "unknown-range-total"
            out.mkdir()
            (out / (dump_name(1) + ".part")).write_bytes(SCENARIOS["unknown-range-total"][0][:5])
            result = run_tool(binary, port, "unknown-range-total", out, "--retries", "0")
            require(result.returncode == 0 and (out / dump_name(1)).read_bytes() == SCENARIOS["unknown-range-total"][0], "unknown range total was rejected", result)

            out = temp / "checksum-retry"
            out.mkdir()
            result = run_tool(binary, port, "checksum-retry", out, "--retries", "1")
            require(result.returncode == 0 and (out / dump_name(1)).read_bytes() == SCENARIOS["checksum-retry"][0], "checksum failure was not retried", result)

            out = temp / "bad-checksum"
            out.mkdir()
            result = run_tool(binary, port, "bad-checksum", out, "--retries", "0")
            require(result.returncode != 0 and not (out / dump_name(1)).exists(), "bad checksum reached final filename", result)

            out = temp / "truncated"
            out.mkdir()
            result = run_tool(binary, port, "truncated", out, "--retries", "0")
            require(result.returncode != 0, "truncated metadata was accepted", result)

            out = temp / "stall"
            out.mkdir()
            result = run_tool(binary, port, "stall", out, "--timeout", "100ms", "--retries", "0")
            require(result.returncode != 0 and not (out / dump_name(1)).exists(), "stalled body ignored timeout or reached final filename", result)

            out = temp / "many"
            out.mkdir()
            result = run_tool(binary, port, "many", out, "--jobs", "3", "--retries", "0")
            require(result.returncode == 0, "dynamic dump list failed above 64 files", result)
            require(len(list(out.glob("*.xml.bz2"))) == 70, "dynamic dump list omitted files")

            out = temp / "no-size"
            out.mkdir()
            (out / dump_name(1)).write_bytes(SCENARIOS["no-size"][0])
            result = run_tool(binary, port, "no-size", out, "--retries", "0")
            require(result.returncode == 0, "digest-only existing-file verification failed", result)
            with STATE.lock:
                file_requests = [entry for entry in STATE.requests if entry[0] == "no-size" and entry[1] == dump_name(1)]
            require(not file_requests, "verified no-size file was downloaded again")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    print("WP_DOWNLOAD_TEST_OK")


if __name__ == "__main__":
    main()
