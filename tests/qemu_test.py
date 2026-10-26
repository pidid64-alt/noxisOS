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
    def __init__(self, extra_qemu_args=None):
        cmd = ["qemu-system-i386", "-m", "32",
               "-drive", f"format=raw,file={IMAGE}",
               "-display", "none", "-no-reboot", "-no-shutdown",
               "-monitor", f"unix:{MON},server,nowait"]
        if extra_qemu_args:
            cmd += extra_qemu_args
        self.qemu = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.s = None
        try:
            deadline = time.time() + 20
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

    _KEYMAP = {" ": "spc", "\n": "ret", ".": "dot", "-": "minus",
               "=": "equal", "/": "slash", ",": "comma"}

    def type_line(self, text):
        """Type a command line and press Enter. Lowercase is fine: the
        shell matches commands case-insensitively."""
        for ch in text:
            self.cmd("sendkey " + self._KEYMAP.get(ch, ch))
        self.cmd("sendkey ret")

    def screen(self):
        # Read the 25 text rows one xp request each. A single xp /2000xb
        # (250 output lines) stalls partway through -- the HMP monitor
        # paginates long output and waits for a keypress on a socket
        # monitor -- which made everything below the first screenful look
        # empty. 160-byte requests (20 lines) stay under the pager limit.
        chars = []
        for row in range(25):
            vals = self.xp_bytes(0xB8000 + row * 160, 160)
            for i in range(0, len(vals) - 1, 2):
                c = vals[i]
                chars.append(chr(c) if 32 <= c < 127 else " ")
        rows = ["".join(chars[r * 80:(r + 1) * 80]).rstrip() for r in range(25)]
        return rows

    def xp_bytes(self, addr, n):
        """Read n bytes of guest memory via the monitor and return them as
        a list of ints. Collects until the full reply has arrived."""
        self.s.settimeout(1.5)
        self.s.sendall(("xp /%dxb 0x%x\n" % (n, addr)).encode())
        buf = b""
        deadline = time.time() + 3
        while time.time() < deadline:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            buf += d
            pairs = 0
            for line in buf.decode("latin1").splitlines():
                if re.match(r"^0+", line) and ":" in line:
                    pairs += len(re.findall(r"0x[0-9a-f]{2}\b",
                                            line.split(":", 1)[1]))
            if pairs >= n:
                break
        vals = []
        for line in buf.decode("latin1").splitlines():
            if re.match(r"^0+", line) and ":" in line:
                vals += re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])
        return [int(x, 16) for x in vals[:n]]

    def expect(self, substr, seconds=5):
        # Case-insensitive on purpose: the kernel's sc_ascii table only
        # produces uppercase letters, so typed input and echoed args come
        # back capitalized regardless of what the test typed.
        needle = substr.lower()
        deadline = time.time() + seconds
        while time.time() < deadline:
            if any(needle in row.lower() for row in self.screen()):
                return True
            time.sleep(0.3)
        return False

    def expect_re(self, pattern, seconds=5):
        """Like expect(), but matches a regex against each screen row. Used
        where the expected output is not a fixed string (e.g. the wall
        clock), so the assertion cannot depend on today's date."""
        rx = re.compile(pattern, re.IGNORECASE)
        deadline = time.time() + seconds
        while time.time() < deadline:
            if any(rx.search(row) for row in self.screen()):
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
        # A full YYYY-MM-DD HH:MM:SS stamp. The old assertion was
        # expect("-20"), which only matched when the day of the month was
        # 20..29 -- the test passed or failed depending on the date.
        assert v.expect_re(r"\b\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\b"), \
            "TIME printed no YYYY-MM-DD HH:MM:SS stamp"
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
        v.type_line("tasks")        # print the table the parser reads
        time.sleep(1)
        s1, b1 = v.read_task_ticks("spin"), v.read_task_ticks("blink")
        assert s1 is not None and b1 is not None, "TASKS rows not found"
        time.sleep(5)
        v.type_line("tasks")
        time.sleep(1)
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

NET_ARGS = ["-netdev", "user,id=net0",
            "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9"]


def t8_net_ne2000():
    """Networking: with a ne2k_isa on the default slirp LAN, IFCONFIG must
    show the static 10.0.2.15/24 setup, and PING 10.0.2.2 (the slirp
    gateway) must resolve ARP, exchange ICMP echoes and fill the ARP table.
    Exercises the NE2000 driver IRQ path, ARP and IPv4/ICMP end to end."""
    v = Noxis(NET_ARGS)
    try:
        v.type_line("ifconfig")
        assert v.expect("eth0: NE2000 at 0x300 irq 9"), "IFCONFIG silent"
        assert v.expect("inet 10.0.2.15"), "no guest address"
        assert v.expect_re(r"ether [0-9a-f]{2}(:[0-9a-f]{2}){5}"), "no MAC"

        v.type_line("ping 10.0.2.2")
        assert v.expect_re(r"16 bytes from 10\.0\.2\.2", 20), \
            "no ICMP echo replies from the slirp gateway"
        assert v.expect_re(r"\d+ received"), "no ping summary"

        v.type_line("arp")
        assert v.expect("10.0.2.2"), "gateway missing from ARP table"

        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead after PING"
        return True, ""
    finally:
        v.close()


def t9_net_absent_graceful():
    """Without -netdev the shell must stay alive and report the missing
    card instead of touching the empty port range."""
    v = Noxis()
    try:
        v.type_line("ifconfig")
        assert v.expect("no NE2000"), "missing card not reported"
        v.type_line("ping 10.0.2.2")
        assert v.expect("no network card", 5), "PING did not notice"
        v.type_line("help")
        assert v.expect("Available commands", 5), "shell dead"
        return True, ""
    finally:
        v.close()


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
         t5_weights_3to1, t6_yield_alive, t7_grub_iso_boots,
         t8_net_ne2000, t9_net_absent_graceful]

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
