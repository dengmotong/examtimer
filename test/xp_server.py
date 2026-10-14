#!/usr/bin/env python3
"""
Host-side control server for the Windows XP test agent (agent.vbs).

Endpoints
  GET  /cmd                -> next queued command (plain text, empty if none)   [agent]
  POST /result             -> agent posts command output                          [agent]
  GET  /file/<name>        -> serve file from SHARE                               [agent]
  POST /upload/<name>      -> store uploaded file into SHARE                      [agent]
  POST /push               -> queue a command, returns its sequence number        [operator]
  GET  /wait?seq=N&timeout -> block until command N produced a result             [operator]
  GET  /last               -> last result text                                    [operator]
  GET  /status             -> json status                                         [operator]
"""
import os
import json
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse, parse_qs

SHARE = os.environ.get("AGENT_SHARE", "/srv/xpshare")
LOG = os.path.join(SHARE, "results.log")
PORT = int(os.environ.get("AGENT_PORT", "8899"))

_cv = threading.Condition()
_seq = 0                 # last queued command id
_done_seq = 0            # last completed command id
_queue = []              # [(seq, cmd)]
_inflight = None         # (seq, cmd)
_last_result = ""
_history = {}


def queue_command(cmd):
    global _seq
    with _cv:
        _seq += 1
        _queue.append((_seq, cmd))
        _cv.notify_all()
        return _seq


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _safe(self, name):
        p = os.path.normpath(os.path.join(SHARE, unquote(name).lstrip("/\\")))
        return p if p.startswith(SHARE) else None

    def _body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        global _inflight
        u = urlparse(self.path)
        path, qs = u.path, parse_qs(u.query)

        if path in ("/cmd", "/poll"):
            with _cv:
                if _queue:
                    _inflight = _queue.pop(0)
                    cmd = f"{_inflight[0]}|{_inflight[1]}"
                else:
                    cmd = ""
            if cmd:
                print(f"[-> xp] {cmd[:300]}", flush=True)
            self._send(200, cmd)

        elif path == "/wait":
            want = int(qs.get("seq", ["0"])[0])
            timeout = float(qs.get("timeout", ["60"])[0])
            deadline = time.time() + timeout
            with _cv:
                while _done_seq < want and time.time() < deadline:
                    _cv.wait(min(1.0, max(0.05, deadline - time.time())))
                ok = _done_seq >= want
                res = _history.get(want, _last_result if ok else "")
            self._send(200, ("OK\n" if ok else "TIMEOUT\n") + res)

        elif path.startswith("/file/"):
            p = self._safe(path[len("/file/"):])
            if p and os.path.isfile(p):
                with open(p, "rb") as f:
                    data = f.read()
                print(f"[--> xp] {p} ({len(data)}B)", flush=True)
                self._send(200, data, "application/octet-stream")
            else:
                self._send(404, "not found")

        elif path == "/last":
            self._send(200, _last_result)

        elif path == "/status":
            with _cv:
                st = dict(queue=[c for _, c in _queue], seq=_seq, done=_done_seq,
                          inflight=_inflight[1] if _inflight else None)
            self._send(200, json.dumps(st, ensure_ascii=False), "application/json")

        elif path == "/":
            self._send(200, "xp agent control server\n")

        else:
            self._send(404, "not found")

    def do_POST(self):
        global _last_result, _done_seq, _inflight
        u = urlparse(self.path)
        path = u.path
        qs = parse_qs(u.query)
        body = self._body()

        if path == "/result":
            txt = body.decode("utf-8", "replace")
            seq = qs.get("seq", [""])[0]
            with _cv:
                _last_result = txt
                if seq.isdigit():
                    n = int(seq)
                    _done_seq = max(_done_seq, n)
                    _history[n] = txt
                    if _inflight and _inflight[0] == n:
                        _inflight = None
                _cv.notify_all()
            with open(LOG, "a", encoding="utf-8") as f:
                f.write(f"\n===== {time.strftime('%H:%M:%S')} =====\n{txt}\n")
            print(f"[<- xp] {len(txt)}B :: {txt[:400].strip()}", flush=True)
            self._send(200, "ok")

        elif path.startswith("/upload/"):
            p = self._safe(path[len("/upload/"):])
            if not p:
                self._send(400, "bad path")
                return
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(body)
            print(f"[<-- xp] {p} ({len(body)}B)", flush=True)
            self._send(200, "ok")

        elif path == "/cmd":          # POST variant of the poll (uncacheable)
            with _cv:
                if _queue:
                    _inflight = _queue.pop(0)
                    cmd = f"{_inflight[0]}|{_inflight[1]}"
                else:
                    cmd = ""
            if cmd:
                print(f"[-> xp] {cmd[:300]}", flush=True)
            self._send(200, cmd)

        elif path == "/push":
            cmd = body.decode("utf-8", "replace").strip()
            seq = queue_command(cmd)
            print(f"[queue #{seq}] {cmd[:200]}", flush=True)
            self._send(200, str(seq))

        else:
            self._send(404, "not found")


def main():
    os.makedirs(SHARE, exist_ok=True)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    srv.daemon_threads = True
    print(f"agent control server on :{PORT}, share={SHARE}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
