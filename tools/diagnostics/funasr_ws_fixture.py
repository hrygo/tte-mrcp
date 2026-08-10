#!/usr/bin/env python3
"""Loopback-only FunASR WebSocket fixture and pacing report aggregator."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import re
import signal
import socket
import socketserver
import struct
import sys
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import parse_qs, urlsplit


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
LOOPBACK_HOSTS = {"127.0.0.1", "::1", "localhost"}
METRICS_RE = re.compile(
    r"(?:session_id=(?P<session_id>[^\]]+)\]\s+)?"
    r"audio transport generation=(?P<generation>\d+)"
    r" audio_rx_frames=(?P<media_frames>\d+)"
    r" audio_rx_bytes=(?P<valid_audio_bytes>\d+)"
    r" audio_rx_gap_samples=(?P<media_gap_samples>\d+)"
    r" audio_rx_gap_hist_ms=(?P<media_gap_hist_ms>\S+)"
    r" audio_rx_gap_p99_ms=(?P<media_gap_p99_ms>\d+)"
    r" audio_rx_gap_max_ms=(?P<media_gap_max_ms>\d+)"
    r" audio_tx_frames=(?P<ws_audio_frames>\d+)"
    r" audio_tx_bytes=(?P<ws_audio_bytes>\d+)"
    r" audio_tx_last_us=(?P<ws_audio_last_us>-?\d+)"
    r" audio_tx_gap_last_ms=(?P<ws_audio_gap_last_ms>-?\d+)"
    r" audio_tx_gap_max_ms=(?P<ws_audio_gap_max_ms>-?\d+)"
    r" ring_high_water=(?P<ring_high_water>\d+)"
    r" overrun_bytes=(?P<overrun_bytes>\d+)"
    r" overrun_events=(?P<overrun_events>\d+)"
    r" first_send_ms=(?P<first_send_ms>-?\d+)"
    r" write_wait_max_ms=(?P<write_wait_max_ms>\d+)"
    r" abnormal_closes=(?P<abnormal_closes>\d+)"
    r" partial_reads=(?P<partial_reads>\d+)"
    r" rx_messages=(?P<rx_messages>\d+)"
    r" completion_failure=(?P<completion_failure>\d+)"
)

LEGACY_METRICS_RE = re.compile(
    r"(?:session_id=(?P<session_id>[^\]]+)\]\s+)?"
    r"transport metrics generation=(?P<generation>\d+)"
    r" media_frames=(?P<media_frames>\d+)"
    r" media_gap_samples=(?P<media_gap_samples>\d+)"
    r" media_gap_hist_ms=(?P<media_gap_hist_ms>\S+)"
    r" media_gap_p99_ms=(?P<media_gap_p99_ms>\d+)"
    r" media_gap_max_ms=(?P<media_gap_max_ms>\d+)"
    r" valid_audio_bytes=(?P<valid_audio_bytes>\d+)"
    r" ring_high_water=(?P<ring_high_water>\d+)"
    r" overrun_bytes=(?P<overrun_bytes>\d+)"
    r" overrun_events=(?P<overrun_events>\d+)"
    r" first_send_ms=(?P<first_send_ms>-?\d+)"
    r" write_wait_max_ms=(?P<write_wait_max_ms>\d+)"
    r" abnormal_closes=(?P<abnormal_closes>\d+)"
    r" partial_reads=(?P<partial_reads>\d+)"
    r" rx_messages=(?P<rx_messages>\d+)"
    r" completion_failure=(?P<completion_failure>\d+)"
)


def websocket_accept(key: str) -> str:
    digest = hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def upgrade_response(key: str) -> bytes:
    return (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {websocket_accept(key)}\r\n"
        "\r\n"
    ).encode("ascii")


def encode_frame(opcode: int, payload: bytes = b"") -> bytes:
    if len(payload) < 126:
        header = bytes((0x80 | opcode, len(payload)))
    elif len(payload) <= 0xFFFF:
        header = bytes((0x80 | opcode, 126)) + struct.pack("!H", len(payload))
    else:
        header = bytes((0x80 | opcode, 127)) + struct.pack("!Q", len(payload))
    return header + payload


def split_bytes(data: bytes, widths: Iterable[int]) -> list[bytes]:
    pieces: list[bytes] = []
    offset = 0
    for width in widths:
        if offset >= len(data):
            break
        end = min(offset + max(1, width), len(data))
        pieces.append(data[offset:end])
        offset = end
    if offset < len(data):
        pieces.append(data[offset:])
    return pieces


def fault_applies(connection_id: int, fault_connection: int) -> bool:
    return fault_connection > 0 and connection_id == fault_connection


def percentile(values: list[float], percent: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, math.ceil(percent * len(ordered)) - 1)
    return ordered[rank]


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("peer closed")
        chunks.extend(chunk)
    return bytes(chunks)


def read_frame(sock: socket.socket) -> tuple[int, bytes, int]:
    first, second = recv_exact(sock, 2)
    wire_size = 2
    if first & 0x70:
        raise ValueError("reserved WebSocket bits are set")
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(sock, 2))[0]
        wire_size += 2
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(sock, 8))[0]
        wire_size += 8
    if length > 2 * 1024 * 1024:
        raise ValueError("frame exceeds fixture limit")
    mask = recv_exact(sock, 4) if masked else b""
    wire_size += len(mask) + length
    payload = bytearray(recv_exact(sock, length))
    if masked:
        for index in range(length):
            payload[index] ^= mask[index % 4]
    return opcode, bytes(payload), wire_size


def make_record(connection_id: int, mode: str) -> dict[str, Any]:
    return {
        "schema": "funasr-fixture-v1",
        "connection_id": connection_id,
        "mode": mode,
        "call_id": None,
        "wall_start": None,
        "wall_end": None,
        "monotonic_start_s": 0.0,
        "monotonic_end_s": 0.0,
        "bytes_read": 0,
        "bytes_written": 0,
        "audio_frames": 0,
        "frame_gaps_ms": [],
        "injected_pauses_ms": [],
        "ping_sent": False,
        "pong_received": False,
        "outcome": "pending",
    }


class FixtureState:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.lock = threading.Lock()
        self.next_id = 1
        self.completed = 0

    def allocate(self) -> int:
        with self.lock:
            value = self.next_id
            self.next_id += 1
            return value

    def emit(self, record: dict[str, Any]) -> None:
        line = json.dumps(record, ensure_ascii=False, sort_keys=True)
        with self.lock:
            if self.args.report:
                with open(self.args.report, "a", encoding="utf-8") as stream:
                    stream.write(line + "\n")
            print(line, flush=True)
            self.completed += 1
            if self.args.once and self.completed >= self.args.once:
                threading.Thread(
                    target=self.args.server.shutdown, daemon=True
                ).start()


class FixtureHandler(socketserver.BaseRequestHandler):
    def _send(self, data: bytes, record: dict[str, Any], bytewise: bool = False) -> None:
        pieces = [bytes((item,)) for item in data] if bytewise else [data]
        for piece in pieces:
            self.request.sendall(piece)
            record["bytes_written"] += len(piece)

    def handle(self) -> None:
        state: FixtureState = self.server.state  # type: ignore[attr-defined]
        connection_id = state.allocate()
        configured_mode = state.args.mode
        mode = configured_mode if fault_applies(
            connection_id, state.args.fault_connection
        ) else "normal"
        record = make_record(connection_id, mode)
        wall_start = time.time()
        mono_start = time.monotonic()
        record["wall_start"] = wall_start
        record["monotonic_start_s"] = mono_start
        self.request.settimeout(state.args.timeout)
        last_audio: float | None = None
        try:
            header = bytearray()
            while b"\r\n\r\n" not in header:
                chunk = self.request.recv(1 if mode == "slow-read" else 1024)
                if not chunk:
                    raise EOFError("closed during HTTP upgrade")
                header.extend(chunk)
                record["bytes_read"] += len(chunk)
                if len(header) > 16384:
                    raise ValueError("HTTP header exceeds fixture limit")
            request_line = bytes(header).split(b"\r\n", 1)[0]
            request_parts = request_line.decode("ascii").split()
            if len(request_parts) != 3:
                raise ValueError("invalid HTTP request line")
            request_target = urlsplit(request_parts[1])
            if request_target.path != state.args.path:
                raise ValueError("unexpected WebSocket path")
            record["call_id"] = parse_qs(request_target.query).get(
                "call_id", [None]
            )[0]
            match = re.search(
                br"(?im)^Sec-WebSocket-Key:\s*([^\r\n]+)", bytes(header)
            )
            if not match:
                raise ValueError("missing Sec-WebSocket-Key")
            response = upgrade_response(match.group(1).decode("ascii").strip())
            self._send(response, record, bytewise=mode == "bytewise-header")

            while True:
                if mode == "slow-read":
                    pause = state.args.pause_ms / 1000.0
                    record["injected_pauses_ms"].append(state.args.pause_ms)
                    time.sleep(pause)
                opcode, payload, wire_size = read_frame(self.request)
                record["bytes_read"] += wire_size
                if opcode == 0xA:
                    record["pong_received"] = True
                    continue
                if opcode == 0x8:
                    record["outcome"] = "client-close"
                    break
                if opcode != 0x2:
                    continue
                now = time.monotonic()
                if payload:
                    record["audio_frames"] += 1
                    if last_audio is not None:
                        record["frame_gaps_ms"].append(
                            round((now - last_audio) * 1000.0, 3)
                        )
                    last_audio = now
                    if mode == "ping" and not record["ping_sent"]:
                        ping = encode_frame(0x9, b"pacing")
                        self._send(ping, record)
                        record["ping_sent"] = True
                    if mode == "close":
                        self._send(encode_frame(0x8, b"fault"), record)
                        record["outcome"] = "injected-close"
                        break
                    continue

                if mode == "delayed-final":
                    record["injected_pauses_ms"].append(state.args.pause_ms)
                    time.sleep(state.args.pause_ms / 1000.0)
                final_payload = json.dumps(
                    {"code": 0, "text": f"fixture-{connection_id}"},
                    separators=(",", ":"),
                ).encode("utf-8")
                final_frame = encode_frame(0x1, final_payload)
                if mode == "split-payload":
                    for piece in split_bytes(final_frame, (1, 1, 2, 3)):
                        self._send(piece, record)
                        time.sleep(0.005)
                else:
                    self._send(final_frame, record)
                record["outcome"] = "final-sent"
                break
        except (EOFError, OSError, ValueError) as error:
            record["outcome"] = f"error:{type(error).__name__}:{error}"
        finally:
            record["wall_end"] = time.time()
            record["monotonic_end_s"] = time.monotonic()
            state.emit(record)


class ThreadedFixtureServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def emit_config(source: Path, target: Path, host: str, port: int, path: str) -> None:
    source = source.resolve()
    target = target.resolve()
    if source == target:
        raise ValueError("target must differ from source")
    if not source.is_file():
        raise ValueError(f"source does not exist: {source}")
    if target.exists():
        raise ValueError(f"refusing to overwrite target: {target}")
    tree = ET.parse(source)
    root = tree.getroot()
    plugin_factory = root.find(".//plugin-factory")
    if plugin_factory is None:
        raise ValueError("plugin-factory not found")
    matches = [
        node
        for node in list(plugin_factory)
        if node.tag == "engine"
        and (
            node.get("id") == "Demo-Recog-1"
            or node.get("name") == "demorecog"
            or node.get("id") == "ASR-WebSocket-1"
            or node.get("name") == "asr_websocket"
        )
    ]
    insert_at = list(plugin_factory).index(matches[0]) if matches else len(plugin_factory)
    for node in matches:
        plugin_factory.remove(node)
    engine = ET.Element(
        "engine",
        {"id": "ASR-WebSocket-1", "name": "asr_websocket", "enable": "true"},
    )
    for name, value in (
        ("funasr-host", host),
        ("funasr-port", str(port)),
        ("funasr-path", path),
    ):
        ET.SubElement(engine, "param", {"name": name, "value": value})
    plugin_factory.insert(insert_at, engine)

    legacy_engine_names = {"Demo-Recog-1", "demorecog"}
    mapping_matches: list[tuple[ET.Element, ET.Element]] = []
    for parent in root.iter():
        for node in list(parent):
            if (
                node.tag == "resource"
                and node.get("id") == "speechrecog"
                and node.get("engine") in legacy_engine_names | {"ASR-WebSocket-1"}
            ):
                mapping_matches.append((parent, node))
    if mapping_matches:
        mapping_parent, mapping = mapping_matches[0]
        mapping.set("engine", "ASR-WebSocket-1")
        for parent, node in mapping_matches[1:]:
            parent.remove(node)
    else:
        resource_map = root.find(".//resource-engine-map")
        if resource_map is not None:
            ET.SubElement(
                resource_map,
                "resource",
                {"id": "speechrecog", "engine": "ASR-WebSocket-1"},
            )
    target.parent.mkdir(parents=True, exist_ok=True)
    tree.write(target, encoding="utf-8", xml_declaration=True)


def load_jsonl(path: Path | None) -> list[dict[str, Any]]:
    if path is None or not path.is_file():
        return []
    records: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"JSONL line {line_number} is not an object")
            records.append(value)
    return records


def parse_gap_histogram(value: str) -> dict[int, int]:
    if value == "-":
        return {}
    result: dict[int, int] = {}
    for item in value.split(","):
        bucket, count = item.split(":", 1)
        result[int(bucket)] = result.get(int(bucket), 0) + int(count)
    return result


def parse_server_metrics(path: Path | None) -> list[dict[str, Any]]:
    if path is None or not path.is_file():
        return []
    metrics: list[dict[str, Any]] = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = METRICS_RE.search(line) or LEGACY_METRICS_RE.search(line)
            if match:
                values: dict[str, Any] = {}
                for key, value in match.groupdict().items():
                    if key == "session_id":
                        values[key] = value
                    elif key == "media_gap_hist_ms":
                        values[key] = parse_gap_histogram(value)
                    else:
                        values[key] = int(value)
                metrics.append(values)
    return metrics


def histogram_percentile(histogram: dict[int, int], percent: float) -> int | None:
    sample_count = sum(histogram.values())
    if sample_count == 0:
        return None
    threshold = max(1, math.ceil(sample_count * percent))
    accumulated = 0
    for bucket in sorted(histogram):
        accumulated += histogram[bucket]
        if accumulated >= threshold:
            return bucket
    return None


def aggregate(
    server_log: Path | None,
    fixture_report: Path | None,
    output: Path,
    warmup: int,
    fault_connection: int,
) -> dict[str, Any]:
    fixture = load_jsonl(fixture_report)
    metrics = parse_server_metrics(server_log)
    sample_fixture = fixture[warmup:]
    if server_log is not None and not metrics:
        raise ValueError(f"no transport metrics found in {server_log}")
    metrics_by_session = {
        str(metric["session_id"]): metric
        for metric in metrics
        if metric.get("session_id")
    }
    sample_metrics: list[dict[str, Any]] = []
    nonfault_metrics: list[dict[str, Any]] = []
    for index, record in enumerate(sample_fixture):
        metric = metrics_by_session.get(str(record.get("call_id")))
        if metric is None and warmup + index < len(metrics):
            metric = metrics[warmup + index]
        if metric is not None:
            sample_metrics.append(metric)
            if int(record.get("connection_id", 0)) != fault_connection:
                nonfault_metrics.append(metric)
    if not fixture:
        sample_metrics = metrics[warmup:]
        nonfault_metrics = sample_metrics
    nonfault_chunk_gaps = [
        float(gap)
        for record in sample_fixture
        if int(record.get("connection_id", 0)) != fault_connection
        for gap in record.get("frame_gaps_ms", [])
    ]
    nonfault_histogram: dict[int, int] = {}
    for metric in nonfault_metrics:
        for bucket, count in metric.get("media_gap_hist_ms", {}).items():
            nonfault_histogram[int(bucket)] = (
                nonfault_histogram.get(int(bucket), 0) + int(count)
            )
    report: dict[str, Any] = {
        "schema": "funasr-pacing-v1",
        "generated_wall_time": time.time(),
        "sources": {
            "server_log": str(server_log) if server_log else None,
            "fixture_report": str(fixture_report) if fixture_report else None,
        },
        "warmup": warmup,
        "sample_window": len(sample_fixture),
        "injection_connection_id": fault_connection,
        "requests": len(sample_fixture),
        "successes": sum(r.get("outcome") == "final-sent" for r in sample_fixture),
        "normal_completions": sum(
            r.get("outcome") == "final-sent" for r in sample_fixture
        ),
        "stop_cancellations": sum(
            r.get("outcome") == "client-close" for r in sample_fixture
        ),
        "valid_audio_bytes": sum(m.get("valid_audio_bytes", 0) for m in sample_metrics),
        "ring_high_water": max(
            (m.get("ring_high_water", 0) for m in sample_metrics), default=0
        ),
        "overrun_bytes": sum(m.get("overrun_bytes", 0) for m in sample_metrics),
        "overrun_events": sum(m.get("overrun_events", 0) for m in sample_metrics),
        "abnormal_closes": sum(m.get("abnormal_closes", 0) for m in sample_metrics),
        "first_send_latency_ms": [
            m["first_send_ms"] for m in sample_metrics if m.get("first_send_ms", -1) >= 0
        ],
        "nonfault_frame_gap_histogram_ms": {
            str(bucket): nonfault_histogram[bucket]
            for bucket in sorted(nonfault_histogram)
        },
        "nonfault_frame_gap_sample_count": sum(nonfault_histogram.values()),
        "nonfault_frame_gap_p99_ms": histogram_percentile(
            nonfault_histogram, 0.99
        ),
        "nonfault_frame_gap_max_ms": max(
            (m.get("media_gap_max_ms", 0) for m in nonfault_metrics),
            default=None,
        ),
        "nonfault_websocket_chunk_gap_samples_ms": nonfault_chunk_gaps,
        "nonfault_websocket_chunk_gap_p99_ms": percentile(
            nonfault_chunk_gaps, 0.99
        ),
        "nonfault_websocket_chunk_gap_max_ms": max(
            nonfault_chunk_gaps, default=None
        ),
        "plugin_metric_records": len(sample_metrics),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


class FixtureSelfTests(unittest.TestCase):
    def test_rfc_accept_and_upgrade(self) -> None:
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        self.assertEqual(websocket_accept(key), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
        self.assertIn(b"101 Switching Protocols", upgrade_response(key))

    def test_frame_encoding_and_split_schedule(self) -> None:
        frame = encode_frame(0x9, b"ping")
        self.assertEqual(frame, b"\x89\x04ping")
        self.assertEqual(b"".join(split_bytes(frame, (1, 1, 2))), frame)

    def test_fault_selection_and_schema(self) -> None:
        self.assertTrue(fault_applies(2, 2))
        self.assertFalse(fault_applies(1, 2))
        record = make_record(2, "ping")
        for field in (
            "schema",
            "connection_id",
            "monotonic_start_s",
            "wall_start",
            "bytes_read",
            "bytes_written",
            "outcome",
        ):
            self.assertIn(field, record)

    def test_ping_close_encoding(self) -> None:
        self.assertEqual(encode_frame(0x9, b"x"), b"\x89\x01x")
        self.assertEqual(encode_frame(0x8), b"\x88\x00")

    def test_emit_config_migrates_legacy_engine_to_canonical_name(self) -> None:
        xml = """<unimrcpserver><components><plugin-factory>
        <engine id="Demo-Recog-1" name="demorecog" enable="true"/>
        <engine id="Demo-Recog-1" name="demorecog" enable="true"/>
        </plugin-factory><resource-engine-map>
        <resource id="speechrecog" engine="Demo-Recog-1"/>
        <resource id="speechrecog" engine="demorecog"/>
        </resource-engine-map></components></unimrcpserver>"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.xml"
            target = Path(directory) / "target.xml"
            source.write_text(xml, encoding="utf-8")
            emit_config(source, target, "127.0.0.1", 8022, "/ws/audio")
            engines = ET.parse(target).getroot().findall(
                ".//engine[@id='ASR-WebSocket-1']"
            )
            self.assertEqual(len(engines), 1)
            self.assertEqual(engines[0].get("name"), "asr_websocket")
            self.assertEqual(
                ET.parse(target).getroot().findall(".//engine[@name='demorecog']"),
                [],
            )
            mappings = ET.parse(target).getroot().findall(
                ".//resource[@id='speechrecog']"
            )
            self.assertEqual(len(mappings), 1)
            self.assertEqual(mappings[0].get("engine"), "ASR-WebSocket-1")
            params = {p.get("name"): p.get("value") for p in engines[0]}
            self.assertEqual(params["funasr-path"], "/ws/audio")
            with self.assertRaises(ValueError):
                emit_config(source, target, "127.0.0.1", 8022, "/ws/audio")

    def test_aggregate_keeps_process_monotonic_values_separate(self) -> None:
        records = [make_record(1, "normal"), make_record(2, "normal")]
        records[0]["outcome"] = records[1]["outcome"] = "final-sent"
        records[0]["frame_gaps_ms"] = [200.0]
        records[1]["frame_gaps_ms"] = [210.0]
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture.jsonl"
            output = Path(directory) / "pacing.json"
            fixture.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            report = aggregate(None, fixture, output, 0, 1)
            self.assertEqual(
                report["nonfault_websocket_chunk_gap_p99_ms"], 210.0
            )
            self.assertTrue(output.is_file())

    def test_metrics_parser_and_histogram_aggregation(self) -> None:
        line = (
            "asr_websocket: [session_id=s-2] audio transport generation=2 "
            "audio_rx_frames=50 audio_rx_bytes=32000 audio_rx_gap_samples=49 "
            "audio_rx_gap_hist_ms=20:48,120:1 audio_rx_gap_p99_ms=120 "
            "audio_rx_gap_max_ms=120 audio_tx_frames=50 audio_tx_bytes=32000 "
            "audio_tx_last_us=123456 audio_tx_gap_last_ms=20 "
            "audio_tx_gap_max_ms=120 "
            "ring_high_water=6400 overrun_bytes=0 overrun_events=0 "
            "first_send_ms=201 write_wait_max_ms=2 abnormal_closes=0 "
            "partial_reads=3 rx_messages=1 completion_failure=7\n"
        )
        record = make_record(2, "normal")
        record["call_id"] = "s-2"
        record["outcome"] = "final-sent"
        with tempfile.TemporaryDirectory() as directory:
            server_log = Path(directory) / "server.log"
            fixture = Path(directory) / "fixture.jsonl"
            output = Path(directory) / "pacing.json"
            server_log.write_text(line, encoding="utf-8")
            fixture.write_text(json.dumps(record) + "\n", encoding="utf-8")
            report = aggregate(server_log, fixture, output, 0, 0)
            self.assertEqual(report["plugin_metric_records"], 1)
            self.assertEqual(report["nonfault_frame_gap_sample_count"], 49)
            self.assertEqual(report["nonfault_frame_gap_p99_ms"], 120)

    def test_requested_empty_server_log_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            server_log = Path(directory) / "server.log"
            server_log.write_text("unrelated\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                aggregate(
                    server_log,
                    None,
                    Path(directory) / "pacing.json",
                    0,
                    0,
                )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8022)
    parser.add_argument("--path", default="/ws/audio")
    parser.add_argument(
        "--mode",
        choices=(
            "normal",
            "slow-read",
            "bytewise-header",
            "split-payload",
            "ping",
            "delayed-final",
            "close",
        ),
        default="normal",
    )
    parser.add_argument("--fault-connection", type=int, default=1)
    parser.add_argument("--pause-ms", type=int, default=250)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--once", type=int, default=0)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--allow-non-loopback", action="store_true")
    parser.add_argument("--emit-config", nargs=2, metavar=("SOURCE", "TARGET"))
    parser.add_argument("--aggregate", action="store_true")
    parser.add_argument("--server-log", type=Path)
    parser.add_argument("--fixture-report", type=Path)
    parser.add_argument("--pacing-json", type=Path)
    parser.add_argument("--warmup", type=int, default=0)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if "--self-test" in argv:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(FixtureSelfTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    args = parse_args(argv)
    if args.emit_config:
        source, target = (Path(value) for value in args.emit_config)
        emit_config(source, target, args.host, args.port, args.path)
        return 0
    if args.aggregate:
        if not args.pacing_json:
            raise SystemExit("--aggregate requires --pacing-json")
        report = aggregate(
            args.server_log,
            args.fixture_report,
            args.pacing_json,
            args.warmup,
            args.fault_connection,
        )
        print(json.dumps(report, ensure_ascii=False, sort_keys=True))
        return 0
    if args.host not in LOOPBACK_HOSTS and not args.allow_non_loopback:
        raise SystemExit("non-loopback bind requires --allow-non-loopback")
    if args.report:
        args.report = args.report.resolve()
        args.report.parent.mkdir(parents=True, exist_ok=True)
    with ThreadedFixtureServer((args.host, args.port), FixtureHandler) as server:
        args.server = server
        server.state = FixtureState(args)  # type: ignore[attr-defined]
        signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown).start())
        print(
            json.dumps(
                {"event": "listening", "host": args.host, "port": server.server_address[1]},
                sort_keys=True,
            ),
            flush=True,
        )
        server.serve_forever(poll_interval=0.1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
