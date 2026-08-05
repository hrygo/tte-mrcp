#!/usr/bin/env python3
"""Loopback WebSocket doubles for the production TTS and ASR plugins.

The fixture deliberately uses only the Python standard library.  It is a
protocol double, not a media-quality oracle: the MRCP client and the mock
exchange real WebSocket frames and deterministic audio, while the runner
asserts the MRCP/RTP result separately.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
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
from typing import Any

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
# Keep the parser limit below the service's practical message size so malformed
# length fields cannot make the fixture allocate unbounded memory.
MAX_FRAME_SIZE = 2 * 1024 * 1024
# 701 bytes deliberately crosses the plugin's receive and codec boundaries;
# the final frame is shorter, exercising carry handling at the end of a stream.
AUDIO_FRAME_BYTES = 701
TTS_SAMPLE_RATE = 24000


def websocket_accept(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()


def encode_frame(opcode: int, payload: bytes = b"") -> bytes:
    size = len(payload)
    if size < 126:
        header = bytes((0x80 | opcode, size))
    elif size <= 0xFFFF:
        header = bytes((0x80 | opcode, 126)) + struct.pack("!H", size)
    else:
        header = bytes((0x80 | opcode, 127)) + struct.pack("!Q", size)
    return header + payload


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("peer closed")
        data.extend(chunk)
    return bytes(data)


def read_frame(sock: socket.socket) -> tuple[int, bytes]:
    first, second = recv_exact(sock, 2)
    if first & 0x70:
        raise ValueError("reserved WebSocket bits are set")
    opcode = first & 0x0F
    if opcode not in {0x0, 0x1, 0x2, 0x8, 0x9, 0xA}:
        raise ValueError(f"unsupported opcode 0x{opcode:x}")
    if opcode >= 0x8 and not (first & 0x80):
        raise ValueError("control frame is fragmented")
    size = second & 0x7F
    if size == 126:
        size = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", recv_exact(sock, 8))[0]
    if opcode >= 0x8 and size > 125:
        raise ValueError("control frame payload exceeds 125 bytes")
    if size > MAX_FRAME_SIZE:
        raise ValueError(f"frame too large (>{MAX_FRAME_SIZE} bytes)")
    masked = bool(second & 0x80)
    if not masked:
        raise ValueError("client frame is not masked")
    mask = recv_exact(sock, 4) if masked else b""
    payload = bytearray(recv_exact(sock, size))
    if masked:
        for index in range(size):
            payload[index] ^= mask[index % 4]
    return opcode, bytes(payload)


def split_send(sock: socket.socket, data: bytes, split: bool) -> None:
    if not split:
        sock.sendall(data)
        return
    for width in (1, 2, 3, 7, 13):
        if not data:
            break
        piece, data = data[:width], data[width:]
        sock.sendall(piece)
        time.sleep(0.002)
    if data:
        sock.sendall(data)


def pcm24k(duration_ms: int = 240) -> bytes:
    samples = int(TTS_SAMPLE_RATE * duration_ms / 1000)
    return b"".join(
        struct.pack("<h", int(9000 * math.sin(2 * math.pi * 440 * i / TTS_SAMPLE_RATE)))
        for i in range(samples)
    )


def emit_config(source: Path, target: Path, host: str, tts_port: int, asr_port: int) -> None:
    if target.exists():
        raise ValueError(f"refusing to overwrite {target}")
    tree = ET.parse(source)
    root = tree.getroot()
    engines = {node.get("id"): node for node in root.iter("engine")}
    for engine_id, values in {
        "TTS-WebSocket-1": {"tts-host": host, "tts-port": str(tts_port)},
        "ASR-WebSocket-1": {"funasr-host": host, "funasr-port": str(asr_port), "funasr-path": "/ws/asr"},
    }.items():
        engine = engines.get(engine_id)
        if engine is None:
            raise ValueError(f"missing {engine_id}")
        for param, value in values.items():
            node = next((p for p in engine if p.get("name") == param), None)
            if node is None:
                node = ET.SubElement(engine, "param", {"name": param})
            node.set("value", value)
    target.parent.mkdir(parents=True, exist_ok=True)
    tree.write(target, encoding="utf-8", xml_declaration=True)


class FixtureState:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.lock = threading.Lock()
        self.next_id = 1
        self.completed = 0
        self.fatal_error: OSError | None = None

    def record(self, value: dict[str, Any]) -> None:
        with self.lock:
            line = json.dumps(value, sort_keys=True)
            if self.args.report:
                try:
                    with open(self.args.report, "a", encoding="utf-8") as stream:
                        stream.write(line + "\n")
                except OSError as error:
                    self.fatal_error = error
                    print(f"fixture report write failed: {error}", file=sys.stderr, flush=True)
                    threading.Thread(target=self.args.server.shutdown, daemon=True).start()
                    return
            print(line, flush=True)
            self.completed += 1
            if self.args.once and self.completed >= self.args.once:
                threading.Thread(target=self.args.server.shutdown, daemon=True).start()

    def id(self) -> int:
        with self.lock:
            value = self.next_id
            self.next_id += 1
            return value


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        state: FixtureState = self.server.state  # type: ignore[attr-defined]
        connection_id = state.id()
        record: dict[str, Any] = {
            "schema": "websocket-e2e-fixture-v1", "service": state.args.service,
            "connection_id": connection_id, "text_messages": [], "audio_frames": 0,
            "audio_bytes": 0, "ping_sent": False, "pong_received": False,
            "audio_sample_rate": None,
            "outcome": "pending",
        }
        mode = state.args.mode
        try:
            self.request.settimeout(state.args.timeout)
            header = bytearray()
            while b"\r\n\r\n" not in header:
                header.extend(self.request.recv(1024))
                if len(header) > 16384:
                    raise ValueError("HTTP header too large")
            lines = bytes(header).split(b"\r\n")
            request = lines[0].decode("ascii").split()
            if len(request) != 3:
                raise ValueError("invalid HTTP request")
            if state.args.service == "asr" and not request[1].startswith("/ws/asr"):
                raise ValueError("unexpected ASR path")
            key_lines = [line for line in lines if line.lower().startswith(b"sec-websocket-key:")]
            if len(key_lines) != 1:
                raise ValueError("missing or duplicate Sec-WebSocket-Key")
            key = key_lines[0].split(b":", 1)[1].strip().decode("ascii")
            response = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Accept: " + websocket_accept(key) + "\r\n\r\n").encode()
            split_send(self.request, response, mode in {"split", "slow"})

            if state.args.service == "tts":
                self._tts(state, record, mode)
            else:
                self._asr(state, record, mode)
        except (EOFError, socket.timeout, ConnectionResetError, BrokenPipeError, ValueError) as error:
            record["outcome"] = f"error:{type(error).__name__}:{error}"
        finally:
            state.record(record)

    def _recv_until_done(self, record: dict[str, Any]) -> None:
        while True:
            opcode, payload = read_frame(self.request)
            if opcode == 0xA:
                record["pong_received"] = True
            elif opcode == 0x8:
                raise EOFError("client close")
            elif opcode == 0x1:
                value = json.loads(payload.decode())
                record["text_messages"].append(value)
                if value.get("type") == "audio.start":
                    record["audio_sample_rate"] = value.get("sample_rate")
                if value.get("type") in {"input.done", "session.done"}:
                    return
            elif opcode == 0x2:
                record["audio_frames"] += 1
                record["audio_bytes"] += len(payload)

    def _tts(self, state: FixtureState, record: dict[str, Any], mode: str) -> None:
        self._recv_until_done(record)
        if mode == "close":
            self.request.sendall(encode_frame(0x8, b"fixture fault"))
            record["outcome"] = "injected-close"
            return
        self.request.sendall(encode_frame(0x9, b"fixture-ping"))
        record["ping_sent"] = True
        messages = (f'{{"type":"audio.start","sample_rate":{TTS_SAMPLE_RATE}}}'.encode(),
                    b'{"type":"audio.done"}', b'{"type":"session.done"}')
        self.request.sendall(encode_frame(0x1, messages[0]))
        audio = pcm24k()
        for offset in range(0, len(audio), AUDIO_FRAME_BYTES):
            split_send(
                self.request,
                encode_frame(0x2, audio[offset:offset + AUDIO_FRAME_BYTES]),
                mode in {"split", "slow"},
            )
        self.request.sendall(encode_frame(0x1, messages[1]))
        self.request.sendall(encode_frame(0x1, messages[2]))
        record["outcome"] = "final-sent"

    def _asr(self, state: FixtureState, record: dict[str, Any], mode: str) -> None:
        while True:
            opcode, payload = read_frame(self.request)
            if opcode == 0x9:
                self.request.sendall(encode_frame(0xA, payload))
                continue
            if opcode == 0xA:
                record["pong_received"] = True
                continue
            if opcode == 0x8:
                record["outcome"] = "client-close"
                return
            if opcode != 0x2:
                continue
            record["audio_frames"] += 1
            record["audio_bytes"] += len(payload)
            if record["audio_frames"] == 1 and mode in {"split", "slow"}:
                self.request.sendall(encode_frame(0x9, b"fixture-ping"))
                record["ping_sent"] = True
            if record["audio_bytes"] >= 320:
                if mode == "close":
                    self.request.sendall(encode_frame(0x8, b"fixture fault"))
                    record["outcome"] = "injected-close"
                    return
                payload = json.dumps({"code": 0, "text": "fixture-asr"}, separators=(",", ":")).encode()
                split_send(self.request, encode_frame(0x1, payload), mode in {"split", "slow"})
                record["outcome"] = "final-sent"
                return


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class SelfTests(unittest.TestCase):
    def test_accept_and_frames(self) -> None:
        self.assertEqual(websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
        self.assertEqual(encode_frame(0x9, b"x"), b"\x89\x01x")

    def test_pcm_is_deterministic_and_audible(self) -> None:
        data = pcm24k()
        self.assertEqual(len(data) % 2, 0)
        self.assertGreater(max(abs(value[0]) for value in struct.iter_unpack("<h", data)), 1000)

    def test_oversized_frame_is_rejected(self) -> None:
        left, right = socket.socketpair()
        try:
            right.sendall(bytes((0x82, 127)) + struct.pack("!Q", MAX_FRAME_SIZE + 1))
            with self.assertRaisesRegex(ValueError, "frame too large"):
                read_frame(left)
        finally:
            left.close()
            right.close()

    def test_control_frame_with_reserved_bits_is_rejected(self) -> None:
        left, right = socket.socketpair()
        try:
            right.sendall(bytes((0xC9, 0)))
            with self.assertRaisesRegex(ValueError, "reserved"):
                read_frame(left)
        finally:
            left.close()
            right.close()

    def test_control_frame_payload_limit_is_rejected(self) -> None:
        left, right = socket.socketpair()
        try:
            right.sendall(bytes((0x89, 126)) + struct.pack("!H", 126))
            with self.assertRaisesRegex(ValueError, "control frame payload"):
                read_frame(left)
        finally:
            left.close()
            right.close()

    def test_unmasked_client_frame_is_rejected(self) -> None:
        left, right = socket.socketpair()
        try:
            right.sendall(bytes((0x81, 1)) + b"x")
            with self.assertRaisesRegex(ValueError, "not masked"):
                read_frame(left)
        finally:
            left.close()
            right.close()

    def test_audio_fixture_contract_is_explicit(self) -> None:
        data = pcm24k()
        self.assertEqual(len(data), TTS_SAMPLE_RATE * 240 // 1000 * 2)
        self.assertEqual(AUDIO_FRAME_BYTES, 701)

    def test_timeout_is_a_distinct_fixture_failure(self) -> None:
        left, right = socket.socketpair()
        try:
            left.settimeout(0.01)
            with self.assertRaises(socket.timeout):
                recv_exact(left, 1)
        finally:
            left.close()
            right.close()

    def test_config_rewrites_both_endpoints(self) -> None:
        source = Path(tempfile.mktemp(suffix=".xml"))
        target = Path(tempfile.mktemp(suffix=".xml"))
        source.write_text("<root><engine id='TTS-WebSocket-1'><param name='tts-host' value='x'/><param name='tts-port' value='1'/></engine><engine id='ASR-WebSocket-1'><param name='funasr-host' value='x'/><param name='funasr-port' value='2'/><param name='funasr-path' value='/x'/></engine></root>")
        try:
            emit_config(source, target, "127.0.0.1", 8091, 8022)
            values = {p.get("name"): p.get("value") for e in ET.parse(target).getroot().iter("engine") for p in e}
            self.assertEqual(values["tts-port"], "8091")
            self.assertEqual(values["funasr-port"], "8022")
        finally:
            source.unlink(missing_ok=True)
            target.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", choices=("tts", "asr"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--mode", choices=("normal", "split", "slow", "close"), default="split")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--once", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--emit-config", nargs=2, metavar=("SOURCE", "TARGET"))
    parser.add_argument("--tts-port", type=int, default=8091)
    parser.add_argument("--asr-port", type=int, default=8022)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return 0 if unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(SelfTests)).wasSuccessful() else 1
    if args.emit_config:
        emit_config(Path(args.emit_config[0]), Path(args.emit_config[1]), args.host, args.tts_port, args.asr_port)
        return 0
    if not args.service:
        parser.error("--service is required unless --emit-config or --self-test is used")
    with Server((args.host, args.port), Handler) as server:
        args.server = server
        server.state = FixtureState(args)  # type: ignore[attr-defined]
        print(json.dumps({"event": "listening", "service": args.service, "host": args.host, "port": server.server_address[1]}), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    if server.state.fatal_error is not None:  # type: ignore[attr-defined]
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
