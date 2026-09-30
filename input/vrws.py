#!/usr/bin/python3
"""vrws: vrserver's local web socket, with nothing but the standard library.

vrserver (SteamVR) serves its controller binding page on 127.0.0.1:27062, and that page
follows the controllers' raw input through a web socket there. Reading it takes nothing
from anyone: whatever the buttons are bound to still happens, and it works whatever app
has focus, in games too. frame-voice reads its push-to-talk button the same way. The host's
Python has no `websockets` module, so this is a small RFC 6455 client of its own.

  getstate()          the devices SteamVR has now, from /input/getstate.json: each with its
                      root path (/user/hand/left, /devices/cv/<serial> while our pointer holds
                      that hand, /user/head), controller type, side, and input components
  VrSocket()          a connection: open(mailbox), subscribe(device), recv() -> dict or None

Run as a program, it prints component changes as they come, for the gaze-first tests
(docs/gaze-first.md, test 4):

  input/vrws.py [--all] [--seconds N] [component ...]

By default it follows the buttons (…/click), the thumbsticks' axes, the trigger's value,
and the headset's proximity; --all shows every component. At the end it prints how often
each one updated.
"""
import base64
import json
import os
import select
import socket
import struct
import sys
import time
import urllib.request

HOST, PORT = "127.0.0.1", 27062
ORIGIN = f"http://{HOST}:{PORT}"
HEADERS = {"Referer": f"{ORIGIN}/dashboard/controllerbinding.html"}


def getstate(timeout=3):
    """The devices SteamVR has now (only the ones with a root path)."""
    req = urllib.request.Request(f"{ORIGIN}/input/getstate.json", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return [d for d in json.load(resp).get("devices", []) if d.get("root_path")]


class VrSocket:
    def __init__(self, timeout=3):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET / HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                           f"Origin: {ORIGIN}\r\nReferer: {HEADERS['Referer']}\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("vrserver closed the connection during the handshake")
            head += chunk
        head, self.buf = head.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status + b" ":
            raise OSError(f"vrserver refused the web socket: {status.decode(errors='replace')}")
        self.sock.settimeout(None)
        self.parts = b""

    def fileno(self):
        return self.sock.fileno()

    def close(self):
        try:
            self._frame(0x8, b"")
        except OSError:
            pass
        self.sock.close()

    def _frame(self, opcode, payload):
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send(self, text):
        self._frame(0x1, text.encode())

    def open(self, mailbox):
        self.send(f"mailbox_open {mailbox}")
        self.mailbox = mailbox

    def subscribe(self, device, on=True):
        kind = "request_input_state_updates" if on else "cancel_input_state_updates"
        self.send("mailbox_send input_server " +
                  json.dumps({"type": kind, "device_path": device, "returnAddress": self.mailbox}))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("vrserver closed the web socket")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout=None):
        """The next text message as parsed JSON (None for one that isn't JSON), or None at
        the timeout. Raises OSError when the connection ends."""
        if not self.buf and timeout is not None:
            if not select.select([self.sock], [], [], timeout)[0]:
                return None
        while True:
            b0, b1 = self._read(2)
            opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if opcode == 0x8:
                raise OSError("vrserver closed the web socket")
            if opcode == 0x9:
                self._frame(0xA, data)
                continue
            if opcode in (0x1, 0x2, 0x0):
                self.parts += data
                if not b0 & 0x80:
                    continue
                text, self.parts = self.parts, b""
                try:
                    return json.loads(text)
                except ValueError:
                    return None


def main():
    args = [a for a in sys.argv[1:]]
    show_all = "--all" in args
    seconds = 0.0
    if "--seconds" in args:
        i = args.index("--seconds")
        seconds = float(args[i + 1])
        del args[i:i + 2]
    wanted = [a for a in args if not a.startswith("--")]

    def followed(name):
        if show_all:
            return True
        if wanted:
            return name in wanted
        return (name.endswith("/click") or name in ("/input/thumbstick/x", "/input/thumbstick/y",
                                                   "/input/trigger/value", "/proximity"))

    devices = getstate()
    names = {}
    for d in devices:
        names[d["root_path"]] = f"{d['root_path']} ({d.get('controller_type', '?')}{', ' + d['side'] if d.get('side') else ''})"
        print("device", names[d["root_path"]])
    ws = VrSocket()
    ws.open(f"frametop_vrws_{os.getpid()}")
    for d in devices:
        ws.subscribe(d["root_path"])
    start = time.monotonic()
    last, counts = {}, {}
    try:
        while not seconds or time.monotonic() - start < seconds:
            msg = ws.recv(timeout=0.5)
            if not isinstance(msg, dict) or msg.get("type") != "update_component_states":
                continue
            dev = msg.get("device")
            for name, value in (msg.get("components") or {}).items():
                if not followed(name):
                    continue
                key = (dev, name)
                counts[key] = counts.get(key, 0) + 1
                if last.get(key) != value:
                    last[key] = value
                    print(f"{time.monotonic() - start:9.3f}  {names.get(dev, dev)}  {name} = {value}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ws.close()
    took = max(time.monotonic() - start, 1e-6)
    for (dev, name), n in sorted(counts.items()):
        print(f"updates: {names.get(dev, dev)} {name}: {n} ({n / took:.1f}/s)")


if __name__ == "__main__":
    main()
