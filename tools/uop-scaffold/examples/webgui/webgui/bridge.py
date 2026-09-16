#!/usr/bin/env python3
"""Web GUI front end for the uop-scaffold full-app demo.

This process IS the frontend UoP on the TSS fabric -- the same five
connections and wire formats as the scripted C frontend it replaces:

    STIMULUS  sub  P+0   (seq:int32, value:int32)
    RESULT    pub  P+1   (seq:int32, value:int32, mult:int32, add:int32)
    STORE_REQ pub  P+2   (req_id:int32)
    STORE_RESP sub P+3   (req_id:int32, mult:int32, add:int32)
    STORE_PUT pub  P+4   (mult:int32, add:int32)

and it additionally serves a real browser GUI over HTTP:

    GET  /            the GUI page (static/index.html)
    GET  /events      server-sent events: {"type":"log"|"state", ...}
    GET  /api/state   current {mult, add, source, loaded} as JSON
    POST /api/apply   {"mult": int, "add": int} -- the Apply button

The browser is purely view + controller: live parameters live here in
RAM, Apply-button POSTs mutate them, stimuli are transformed here, and
results publish back over TSS. No direct file I/O for persistence --
state is loaded from / offloaded to the storage UoP over TSS.

Lifecycle mirrors the C frontend:
  startup:  HTTP up immediately; load mult/add from storage via
            STORE_REQ/STORE_RESP with slow-joiner retries; print
            "webgui: ready (...)" so the feeder can gate on it.
  shutdown: on SIGTERM/SIGINT publish STORE_PUT with the live state,
            grace period, write the shutdown marker, finalize TSS.

Environment:
  BASE_PORT       TSS base port (default 41700)
  HTTP_PORT       GUI port (default 18090)
  FRONTEND_MARKER shutdown marker path (default frontend.shutdown.marker)
  TSS_ROOT        tss repo root (default: derived from this file's path)
"""

import json
import os
import queue
import signal
import struct
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
TSS_ROOT = Path(os.environ.get("TSS_ROOT", HERE.parents[3]))
sys.path.insert(0, str(TSS_ROOT / "src"))

from face_tss import Direction, FaceTss, ReturnCode, TssConfigBuilder  # noqa: E402

SEND_TIMEOUT_NS = 5_000_000_000
RECENT_MAX = 200


class Bridge:
    def __init__(self, base_port, http_port, marker_path):
        self.base_port = base_port
        self.http_port = http_port
        self.marker_path = marker_path
        self.lock = threading.Lock()
        self.mult = 3
        self.add = 7
        self.loaded = False
        self.source = "defaults"
        self.rx = 0
        self.tx = 0
        self.err = 0
        self.stop = threading.Event()
        # SSE fan-out: every connected browser gets its own queue.
        self.clients = set()
        self.clients_lock = threading.Lock()
        self.recent = deque(maxlen=RECENT_MAX)
        self.tss = None
        self.httpd = None

    # ------------------------------------------------------------------
    # browser event bus
    # ------------------------------------------------------------------
    def emit(self, kind, **fields):
        line = json.dumps({"type": kind, **fields}, separators=(",", ":"))
        with self.clients_lock:
            self.recent.append(line)
            dead = [q for q in self.clients]
        for q in dead:
            q.put(line)

    def log(self, text):
        print(f"webgui: {text}", flush=True)
        self.emit("log", text=text)

    def emit_state(self):
        with self.lock:
            self.emit("state", mult=self.mult, add=self.add,
                      source=self.source, loaded=self.loaded)

    # ------------------------------------------------------------------
    # TSS callbacks (run on TSS background threads)
    # ------------------------------------------------------------------
    def on_stimulus(self, conn_id, txn_id, guid, payload, header, qos, ctx):
        try:
            seq, value = struct.unpack("<ii", bytes(payload[:8]))
        except struct.error:
            with self.lock:
                self.err += 1
            return ReturnCode.NO_ERROR
        with self.lock:
            mult, add = self.mult, self.add
        result = mult * value + add
        try:
            self.tss.send_message(self.result_id,
                                  struct.pack("<iiii", seq, result, mult, add),
                                  SEND_TIMEOUT_NS)
            with self.lock:
                self.tx += 1
                self.rx += 1
        except Exception:
            with self.lock:
                self.err += 1
                self.rx += 1
        self.emit("log", text=f"STIMULUS seq={seq} value={value}")
        self.emit("log", text=f"RESULT seq={seq} value={result} "
                              f"(mult={mult} add={add})")
        return ReturnCode.NO_ERROR

    def on_store_resp(self, conn_id, txn_id, guid, payload, header, qos, ctx):
        # STORE_RESP wire type is store_state: <ii (mult, add), 8 bytes.
        # (STORE_REQ carries the req_id; the response echoes state only.)
        try:
            mult, add = struct.unpack("<ii", bytes(payload[:8]))
        except struct.error:
            return ReturnCode.NO_ERROR
        with self.lock:
            self.mult, self.add = mult, add
            self.loaded = True
            self.source = "storage"
        self.log(f"STORE_RESP -> mult={mult} add={add} (loaded from storage)")
        self.emit_state()
        return ReturnCode.NO_ERROR

    # ------------------------------------------------------------------
    # GUI actions (HTTP handler threads)
    # ------------------------------------------------------------------
    def apply_params(self, mult, add):
        with self.lock:
            self.mult, self.add = mult, add
            self.source = "gui"
        self.log(f"GUI Apply -> mult={mult} add={add}")
        self.emit_state()
        return {"ok": True, "mult": mult, "add": add}

    def get_state(self):
        with self.lock:
            return {"mult": self.mult, "add": self.add,
                    "source": self.source, "loaded": self.loaded}

    # ------------------------------------------------------------------
    # lifecycle
    # ------------------------------------------------------------------
    def start_tss(self):
        addrs = [f"tcp://127.0.0.1:{self.base_port + i}" for i in range(5)]
        cfg = (TssConfigBuilder().instance("webgui")
               .add("STIMULUS", direction=Direction.DESTINATION,
                    transport="pubsub", role="subscriber", address=addrs[0])
               .add("RESULT", direction=Direction.SOURCE,
                    transport="pubsub", role="publisher", address=addrs[1])
               .add("STORE_REQ", direction=Direction.SOURCE,
                    transport="pubsub", role="publisher", address=addrs[2])
               .add("STORE_RESP", direction=Direction.DESTINATION,
                    transport="pubsub", role="subscriber", address=addrs[3])
               .add("STORE_PUT", direction=Direction.SOURCE,
                    transport="pubsub", role="publisher", address=addrs[4])
               .build())
        self.tss = FaceTss("webgui")
        rc = self.tss.initialize(cfg)
        assert rc == ReturnCode.NO_ERROR, f"initialize: {rc}"
        self.stimulus_id, _ = self.tss.create_connection("STIMULUS")
        self.result_id, _ = self.tss.create_connection("RESULT")
        self.store_req_id, _ = self.tss.create_connection("STORE_REQ")
        self.store_resp_id, _ = self.tss.create_connection("STORE_RESP")
        self.store_put_id, _ = self.tss.create_connection("STORE_PUT")
        assert self.tss.register_callback(
            self.stimulus_id, self.on_stimulus) == ReturnCode.NO_ERROR
        assert self.tss.register_callback(
            self.store_resp_id, self.on_store_resp) == ReturnCode.NO_ERROR

    def start_http(self):
        bridge = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass  # demo logs stay focused on the TSS story

            def _send_json(self, obj, status=200):
                body = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/":
                    page = (HERE / "static" / "index.html").read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(page)))
                    self.end_headers()
                    self.wfile.write(page)
                elif self.path == "/api/state":
                    self._send_json(bridge.get_state())
                elif self.path == "/events":
                    self._serve_sse()
                elif self.path == "/favicon.ico":
                    self.send_response(204)
                    self.end_headers()
                else:
                    self._send_json({"ok": False, "error": "not found"},
                                    status=404)

            def do_POST(self):
                if self.path != "/api/apply":
                    self._send_json({"ok": False, "error": "not found"},
                                    status=404)
                    return
                try:
                    length = int(self.headers.get("Content-Length", 0))
                    body = json.loads(self.rfile.read(length) or b"{}")
                    mult = int(body["mult"])
                    add = int(body["add"])
                except (ValueError, KeyError, TypeError):
                    self._send_json({"ok": False,
                                     "error": "expected JSON "
                                              '{"mult": int, "add": int}'},
                                    status=400)
                    return
                self._send_json(bridge.apply_params(mult, add))

            def _serve_sse(self):
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                q = queue.Queue()
                with bridge.clients_lock:
                    replay = list(bridge.recent)
                    bridge.clients.add(q)
                try:
                    for line in replay:
                        self.wfile.write(f"data: {line}\n\n".encode())
                    self.wfile.flush()
                    while True:
                        line = q.get()
                        self.wfile.write(f"data: {line}\n\n".encode())
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    with bridge.clients_lock:
                        bridge.clients.discard(q)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.http_port),
                                         Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever,
                         name="http", daemon=True).start()
        self.log(f"GUI at http://127.0.0.1:{self.http_port}/")

    def load_from_storage(self):
        """STORE_REQ/STORE_RESP with slow-joiner retries (like the C frontend)."""
        time.sleep(2)
        for attempt in range(1, 5):
            if self.loaded:
                break
            try:
                self.tss.send_message(self.store_req_id,
                                      struct.pack("<i", attempt),
                                      SEND_TIMEOUT_NS)
            except Exception as e:
                self.log(f"STORE_REQ publish failed (attempt {attempt}): {e}")
            for _ in range(16):
                if self.loaded:
                    break
                time.sleep(0.25)

    def run(self):
        self.start_tss()
        self.start_http()
        self.load_from_storage()
        with self.lock:
            mult, add, loaded = self.mult, self.add, self.loaded
        self.emit_state()
        if loaded:
            print(f"webgui: ready (mult={mult} add={add}, loaded from storage)",
                  flush=True)
        else:
            print(f"webgui: ready (mult={mult} add={add}, defaults)",
                  flush=True)
        # Main loop: everything happens in callback / HTTP threads.
        self.stop.wait()

    def shutdown(self):
        with self.lock:
            mult, add = self.mult, self.add
            rx, tx, err = self.rx, self.tx, self.err
        try:
            self.tss.send_message(self.store_put_id,
                                  struct.pack("<ii", mult, add),
                                  SEND_TIMEOUT_NS)
            self.log(f"offloaded state mult={mult} add={add} to storage")
        except Exception as e:
            self.log(f"STORE_PUT publish failed: {e}")
        time.sleep(1)  # grace period so the PUT flushes before teardown
        try:
            with open(self.marker_path, "w") as f:
                f.write(f"webgui shutdown: rx={rx} tx={tx} err={err}\n")
        except OSError as e:
            self.log(f"cannot write marker '{self.marker_path}': {e}")
        self.tss.finalize()
        if self.httpd:
            self.httpd.shutdown()
        print("webgui: shutdown complete", flush=True)


def main():
    base_port = int(os.environ.get("BASE_PORT", "41700"))
    http_port = int(os.environ.get("HTTP_PORT", "18090"))
    marker = os.environ.get("FRONTEND_MARKER", "frontend.shutdown.marker")
    bridge = Bridge(base_port, http_port, marker)
    signal.signal(signal.SIGTERM, lambda *a: bridge.stop.set())
    signal.signal(signal.SIGINT, lambda *a: bridge.stop.set())
    bridge.run()
    bridge.shutdown()


if __name__ == "__main__":
    main()
