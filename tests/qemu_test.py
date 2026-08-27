#!/usr/bin/env python3
"""Headless acceptance tests for the noxis scheduler (spec 2026-08-23).

Boots 23-fixes/os-image.bin in QEMU without a display, drives the shell
through the QEMU monitor (`sendkey`), and asserts on the VGA text buffer
read via `xp /2000xb 0xb8000`.
"""
import os
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
        self.s = None
        try:
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
        except BaseException:
            # Never orphan the QEMU process: tear down exactly like close().
            self.close()
            raise

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
            m = re.match(r"^(\d+)\s+%s\s+\S+\s+(\d+)\b" % re.escape(name), row)
            if m:
                return int(m.group(2))
        return None

    def close(self):
        """Idempotent teardown: polite quit via monitor, then hard kill."""
        s = getattr(self, "s", None)
        if s is not None:
            try:
                self.cmd("quit")
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass
            self.s = None
        qemu = getattr(self, "qemu", None)
        if qemu is not None:
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass


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


ISO = "23-fixes/noxis.iso"


def t7_grub_iso_boots():
    """GRUB/El Torito path (plan 2026-08-23 Task 3): GRUB must load the
    kernel with an intact Multiboot handoff — EAX=0x2BADB002 must SURVIVE
    _start's segment reloads (writing AX keeps EAX's upper half, so the
    magic used to arrive as 0x2BAD0010 and fb_init fell back to the hidden
    VGA-text console on a black screen). Asserts on the debugcon trace,
    a screendump (framebuffer banner is NOT in 0xb8000), and a clean
    -d int exception log."""
    dbg_log = "/tmp/noxis-t7-debugcon.log"
    int_log = "/tmp/noxis-t7-int.log"
    shot = "/tmp/noxis-t7-screen.ppm"
    mon = "/tmp/noxis-t7-qmon"
    for f in (dbg_log, int_log, shot):
        if os.path.exists(f):
            os.unlink(f)

    qemu = subprocess.Popen(
        ["qemu-system-i386", "-m", "32",
         "-cdrom", ISO, "-boot", "d",
         "-display", "none", "-no-reboot", "-no-shutdown",
         "-debugcon", f"file:{dbg_log}",
         "-d", "int", "-D", int_log,
         "-monitor", f"unix:{mon},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = None
    try:
        deadline = time.time() + 5
        while True:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(mon)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("qemu monitor socket never appeared")
                time.sleep(0.1)
        time.sleep(8)  # GRUB (timeout=0) + kernel init
        s.sendall(f"screendump {shot}\n".encode())
        time.sleep(1)

        dbg = open(dbg_log, errors="replace").read()
        assert "FB: enter magic=0x2badb002" in dbg, \
            f"kernel saw wrong/absent Multiboot magic: {dbg!r}"
        assert "FB: ACTIVE" in dbg, "framebuffer never activated"

        # Screendump must not be an all-black frame: the banner has to be
        # visibly painted in the framebuffer.
        with open(shot, "rb") as f:
            data = f.read().split(b"\n", 3)
        w, h = map(int, data[1].split())
        pix = data[3]
        bright = sum(1 for i in range(0, len(pix), 3)
                     if pix[i] + pix[i + 1] + pix[i + 2] > 150)
        assert bright > 200, f"screen looks black ({bright} bright px of {w}x{h})"

        ints = open(int_log, errors="replace").read()
        assert not re.search(r"new 0x[0-9a-f]+", ints), "CPU exceptions during ISO boot"
        return True, ""
    finally:
        if s is not None:
            try:
                s.sendall(b"quit\n")
                s.close()
            except Exception:
                pass
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait(timeout=5)


TESTS = [t1_regress_interactive, t2_run_blink_shell_alive,
         t3_parallel_tasks, t4_cooperative_kill,
         t5_weights_3to1, t6_yield_alive, t7_grub_iso_boots]

if __name__ == "__main__":
    wanted = sys.argv[1:]
    selected = [t for t in TESTS
                if not wanted or t.__name__ in wanted
                or t.__name__.lstrip("_") in wanted]
    if wanted and not selected:
        valid = ", ".join(t.__name__.lstrip("_") for t in TESTS)
        print(f"error: no test matches {wanted}; valid names: {valid}",
              file=sys.stderr)
        sys.exit(2)

    ran = 0
    failed = []
    interrupted = False
    try:
        for t in selected:
            try:
                ok, msg = t()
                status = "PASS" if ok else f"FAIL {msg}"
            except Exception as e:
                status = f"FAIL {type(e).__name__}: {e}"
                ok = False
            ran += 1
            print(f"[{status}] {t.__name__}")
            if not ok:
                failed.append(t.__name__)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        print(f"\n{ran - len(failed)}/{len(selected)} passed")
        if interrupted:
            print("INTERRUPTED: remaining tests skipped", file=sys.stderr)
        # Interrupt counts as failure even if every finished test passed.
        sys.exit(130 if interrupted else (1 if failed else 0))
