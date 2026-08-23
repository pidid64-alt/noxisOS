#!/usr/bin/env python3
"""Headless acceptance tests for the noxis scheduler (spec 2026-08-23).

Boots 23-fixes/os-image.bin in QEMU without a display, drives the shell
through the QEMU monitor (`sendkey`), and asserts on the VGA text buffer
read via `xp /2000xb 0xb8000`.
"""
import re
import socket
import subprocess
import sys
import time

IMAGE = "23-fixes/os-image.bin"
MON = "/tmp/noxis-qmon"


class Noxis:
    def __init__(self):
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-m", "32",
             "-drive", f"format=raw,file={IMAGE}",
             "-display", "none", "-no-reboot", "-no-shutdown",
             "-monitor", f"unix:{MON},server,nowait"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 5
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(MON)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("qemu monitor socket never appeared")
                time.sleep(0.1)
        self.s.settimeout(0.05)
        self.drain()
        if not self.expect("Welcome to noxis", 15):
            raise RuntimeError("kernel banner never appeared")

    def drain(self):
        try:
            while True:
                if not self.s.recv(65536):
                    break
        except socket.timeout:
            pass

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        self.drain()

    _KEYMAP = {" ": "spc", "\n": "ret"}

    def type_line(self, text):
        """Type a command line and press Enter. Lowercase is fine: the
        shell matches commands case-insensitively."""
        for ch in text:
            self.cmd("sendkey " + self._KEYMAP.get(ch, ch))
        self.cmd("sendkey ret")

    def screen(self):
        self.cmd("xp /2000xb 0xb8000")
        # Re-query with a proper blocking read: drain() ate the reply above,
        # so ask again and collect until quiet period.
        self.s.settimeout(0.5)
        self.s.sendall(b"xp /2000xb 0xb8000\n")
        buf = b""
        try:
            while len(buf) < 100000:
                d = self.s.recv(65536)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        vals = []
        for line in buf.decode("latin1").splitlines():
            if ":" not in line:
                continue
            vals += re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])
        chars = []
        for i in range(0, len(vals) - 1, 2):
            c = int(vals[i], 16)
            chars.append(chr(c) if 32 <= c < 127 else " ")
        rows = ["".join(chars[r * 80:(r + 1) * 80]).rstrip() for r in range(25)]
        self.s.settimeout(0.05)
        return rows

    def expect(self, substr, seconds=5):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if any(substr in row for row in self.screen()):
                return True
            time.sleep(0.3)
        return False

    def read_task_ticks(self, name):
        """Parse the TASKS table: find the row for `name`, return its ticks."""
        for row in self.screen():
            m = re.match(r"^(\d+)\s+%s\s+\S+\s+(\d+)\s" % re.escape(name), row)
            if m:
                return int(m.group(2))
        return None

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.s.close()
        self.qemu.wait(timeout=5)


def t1_regress_interactive():
    v = Noxis()
    try:
        v.type_line("help")
        assert v.expect("Available commands"), "HELP printed nothing"
        v.type_line("echo hi noxis")
        assert v.expect("hi noxis"), "ECHO printed nothing"
        v.type_line("time")
        assert v.expect("-20"), "TIME printed no date"
        return True, ""
    finally:
        v.close()


def t2_run_blink_shell_alive():
    v = Noxis()
    try:
        v.type_line("run blink")
        assert v.expect("[blink]", 5), "[blink] never appeared"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead after RUN"
        return True, ""
    finally:
        v.close()


def t3_parallel_tasks():
    v = Noxis()
    try:
        v.type_line("run count")
        v.type_line("run clock")
        assert v.expect("[clock]", 6), "[clock] missing"
        assert v.expect("[count]", 2), "[count] missing"
        v.type_line("tasks")
        assert v.expect("count", 3) and v.expect("clock", 3), "TASKS incomplete"
        return True, ""
    finally:
        v.close()


def t4_cooperative_kill():
    v = Noxis()
    try:
        v.type_line("run blink")
        assert v.expect("[blink]", 5), "[blink] never appeared"
        v.type_line("kill 1")
        time.sleep(2)
        v.type_line("tasks")
        time.sleep(0.5)
        rows = v.screen()
        gone = all(not row.startswith("1 ") or "blink" not in row for row in rows)
        assert gone, "killed task still listed"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead after KILL"
        return True, ""
    finally:
        v.close()


def t5_weights_3to1():
    v = Noxis()
    try:
        v.type_line("run spin")     # default weight 3
        v.type_line("run blink")    # default weight 1
        time.sleep(12)              # let ticks accumulate
        s1, b1 = v.read_task_ticks("spin"), v.read_task_ticks("blink")
        time.sleep(5)
        s2, b2 = v.read_task_ticks("spin"), v.read_task_ticks("blink")
        ds, db = s2 - s1, b2 - b1
        ratio = ds / max(db, 1)
        assert 2.0 <= ratio <= 4.5, f"weight ratio {ratio:.2f} (spin={ds}, blink={db})"
        return True, ""
    finally:
        v.close()


def t6_yield_alive():
    v = Noxis()
    try:
        v.type_line("yield")
        assert v.expect("yielded", 5), "YIELD produced no reply"
        v.type_line("uptime")
        assert v.expect("Uptime:", 5), "system dead after YIELD"
        return True, ""
    finally:
        v.close()


TESTS = [t1_regress_interactive, t2_run_blink_shell_alive,
         t3_parallel_tasks, t4_cooperative_kill,
         t5_weights_3to1, t6_yield_alive]

if __name__ == "__main__":
    wanted = sys.argv[1:]
    failed = []
    for t in TESTS:
        if wanted and t.__name__ not in wanted and t.__name__[2:] not in wanted:
            continue
        try:
            ok, msg = t()
            status = "PASS" if ok else f"FAIL {msg}"
        except Exception as e:
            status = f"FAIL {type(e).__name__}: {e}"
            ok = False
        print(f"[{status}] {t.__name__}")
        if not ok:
            failed.append(t.__name__)
    print(f"\n{len(TESTS)-len(failed)}/{len(TESTS)} passed")
    sys.exit(1 if failed else 0)
