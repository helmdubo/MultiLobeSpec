# -*- coding: utf-8 -*-
"""Minimal JSON-RPC-over-WebSocket client for the UE_MCP_Bridge plugin (ws://127.0.0.1:9877). No third-party deps.
Usage:
  python uemcp.py METHOD '{"json":"params"}'        # one call, prints result JSON
  python uemcp.py cmd  "stat unit"                   # shorthand for execute_command
  python uemcp.py py   "print(1)"                    # shorthand for execute_python (code string)
  python uemcp.py pyfile path.py                     # execute_python with file contents
"""
import sys, os, json, socket, struct, base64, time

HOST, PORT = "127.0.0.1", 9877

def ws_connect(timeout=120.0):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n") % (HOST, PORT, key)
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("handshake closed")
        resp += chunk
    if b" 101 " not in resp.split(b"\r\n", 1)[0]:
        raise RuntimeError("handshake failed: %r" % resp[:200])
    return s

def ws_send_text(s, text):
    payload = text.encode("utf-8")
    hdr = bytearray([0x81])
    n = len(payload)
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126); hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127); hdr += struct.pack(">Q", n)
    mask = os.urandom(4)
    hdr += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(bytes(hdr) + masked)

def _recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
    return buf

def ws_recv_text(s):
    msg = b""
    while True:
        b1, b2 = _recv_exact(s, 2)
        fin, opcode = b1 & 0x80, b1 & 0x0F
        masked, n = b2 & 0x80, b2 & 0x7F
        if n == 126:
            n = struct.unpack(">H", _recv_exact(s, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", _recv_exact(s, 8))[0]
        mask = _recv_exact(s, 4) if masked else None
        data = _recv_exact(s, n)
        if mask:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if opcode == 0x8:
            raise RuntimeError("server closed")
        if opcode == 0x9:  # ping -> pong
            s.sendall(bytes([0x8A, 0x80]) + os.urandom(4)); continue
        if opcode in (0x1, 0x0):
            msg += data
            if fin:
                return msg.decode("utf-8", "replace")

def call(method, params=None, timeout=120.0):
    s = ws_connect(timeout)
    try:
        req = {"jsonrpc": "2.0", "id": int(time.time() * 1000) % 100000, "method": method, "params": params or {}}
        ws_send_text(s, json.dumps(req))
        while True:
            resp = json.loads(ws_recv_text(s))
            if resp.get("id") == req["id"] or "result" in resp or "error" in resp:
                return resp
    finally:
        s.close()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    m = sys.argv[1]
    if m == "cmd":
        r = call("execute_command", {"command": sys.argv[2]})
    elif m == "py":
        r = call("execute_python", {"code": sys.argv[2]})
    elif m == "pyfile":
        r = call("execute_python", {"code": open(sys.argv[2], encoding="utf-8").read()})
    else:
        r = call(m, json.loads(sys.argv[2]) if len(sys.argv) > 2 else {})
    print(json.dumps(r, ensure_ascii=False, indent=1))
