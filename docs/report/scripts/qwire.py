"""qwire — the Quicx wire protocol and daemon control, shared by the report's scripts."""
import os
import platform
import signal
import socket
import struct
import subprocess
import time

SUBMIT, OK, ERROR, READY, DONE, FAILED, TASK, WAIT, HEARTBEAT, PONG, STATS, STATS_RESPONSE = range(1, 13)
ERR_QUEUE_FULL, ERR_INVALID_MSG, ERR_PAYLOAD_TOO_BIG, ERR_UNKNOWN_TYPE, ERR_VERSION_MISMATCH = range(1, 6)


def frame(t, body=b"", version=1):
    return struct.pack(">BBL", version, t, len(body)) + body


def submit_body(typ, payload):
    return bytes([len(typ)]) + typ + payload


class Conn:
    def __init__(self, port, timeout=5.0):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, t, body=b""):
        self.s.sendall(frame(t, body))

    def send_raw(self, data):
        self.s.sendall(data)

    def recv(self, timeout=5.0):
        """next frame as (type, body); raises ConnectionError when the daemon closes"""
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 6:
                _, t, n = struct.unpack(">BBL", self.buf[:6])
                if len(self.buf) >= 6 + n:
                    body, self.buf = self.buf[6:6 + n], self.buf[6 + n:]
                    return t, body
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed by the daemon")
            self.buf += chunk

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
        return True
    except OSError:
        return False


class Daemon:
    """starts `binary start --port P` in the foreground and kills exactly that process"""

    def __init__(self, binary, port, log=None):
        if port_open(port):
            raise RuntimeError(f"port {port} is already in use")
        self.port = port
        out = open(log, "ab") if log else subprocess.DEVNULL
        self.p = subprocess.Popen([binary, "start", "--port", str(port)], stdout=out, stderr=out)
        for _ in range(100):
            if self.p.poll() is not None:
                raise RuntimeError(f"daemon exited with {self.p.returncode}")
            if port_open(port):
                return
            time.sleep(0.05)
        raise RuntimeError("daemon never accepted connections")

    @property
    def pid(self):
        return self.p.pid

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(timeout=3)
        for _ in range(50):
            if not port_open(self.port):
                break
            time.sleep(0.05)


def physical_kb(pid):
    """macOS: phys_footprint from vmmap. Linux: VmRSS. Returns (kb, name)."""
    if platform.system() == "Darwin":
        out = subprocess.run(["vmmap", "-summary", str(pid)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.startswith("Physical footprint:"):
                v = line.split(":")[1].strip()
                num, unit = float(v[:-1]), v[-1]
                return int(num * {"K": 1, "M": 1024, "G": 1024 * 1024}.get(unit, 1)), "phys_footprint"
        return -1, "phys_footprint"
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]), "rss"
    except OSError:
        pass
    return -1, "rss"


def rss_kb(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else -1
