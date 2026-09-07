#!/usr/bin/env python3
"""Boot fresh desktop images and test the visible VGA output, not just RAM.

Uses QMP over a private Unix socket, no GUI/VNC port or third-party packages.
-snapshot keeps the IDE image unchanged. Every wait checks an actual outcome;
boot/exec latency is not mistaken for a successful launch or a frozen screen.

NOTE (2026-09-07): STALE. The desktop now enters 800x600x8 through the
Bochs/QEMU VBE (DISPI) interface with a linear framebuffer (kernel/vga.c)
instead of 320x200 mode 13h, and it opens a left-docked \"Files:\" explorer
next to the TTY window (kernel/desktop.c). The logical 320x200 coordinates,
TERM_*/BACKGROUND constants and expected capture sizes below must be
re-derived under QEMU before this harness is used again.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

OS_DIR = Path(__file__).resolve().parents[1]


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.reader = self.socket.makefile("rb")
        json.loads(self.reader.readline())  # greeting
        self.command("qmp_capabilities")

    def close(self):
        self.reader.close()
        self.socket.close()

    def command(self, name, **arguments):
        request = {"execute": name,
                   "arguments": {k.replace("_", "-"): v for k, v in arguments.items()}}
        self.socket.sendall(json.dumps(request).encode() + b"\n")
        while True:
            line = self.reader.readline()
            if not line:
                raise RuntimeError("QEMU closed the monitor")
            reply = json.loads(line)
            if "error" in reply:
                raise RuntimeError(str(reply["error"]))
            if "return" in reply:
                return reply["return"]

    def key(self, *codes):
        self.command("send-key", keys=[{"type": "qcode", "data": c} for c in codes],
                     hold_time=50)
        time.sleep(0.10)  # release this key before sending the next one

    def type(self, text):
        for char in text:
            self.key({" ": "spc", "\n": "ret"}.get(char, char))

    def tty(self, number):
        start = 0xB8000 + 2 * (0x8000 // 2 // 3) * number
        result = self.command("human-monitor-command", command_line=f"xp /4000bx {start:#x}")
        data = [int(b, 16) for line in result.splitlines() if ":" in line
                for b in re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])]
        chars = "".join(chr(b) if 32 <= b < 127 else " " for b in data[::2])
        return "\n".join(chars[i:i + 80].rstrip() for i in range(0, len(chars), 80))

    def frame(self, path):
        self.command("screendump", filename=str(path))
        magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
            raise RuntimeError("invalid QEMU screen dump")
        return width, height, pixels


def wait_for(check, label, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.10)
    raise AssertionError(f"timed out waiting for {label}")


def pixel(frame, x, y):
    width, height, pixels = frame
    offset = ((y * height // 200) * width + x * width // 320) * 3
    return tuple(pixels[offset:offset + 3])


def color_matches(actual, expected):
    # VGA DAC channels are only six bits; QEMU versions differ slightly in
    # their conversion to eight-bit RGB (e.g. 168 vs 170, 87 vs 85).
    return all(abs(a - b) <= 3 for a, b in zip(actual, expected))


DESKTOP_BLUE = (0, 0, 170)
TITLEBAR_BLUE = (85, 85, 255)
TERMINAL_BLACK = (0, 0, 0)
TERMINAL_WHITE = (255, 255, 255)

# The TTY window is centred and sized from the character grid in include/wm.h:
# 36x9 cells of 8x16 px, 2 px border, 18 px title bar, 2 px padding.
TERM_W, TERM_H = 296, 168
TERM_X, TERM_Y = (320 - TERM_W) // 2, (200 - TERM_H) // 2
# Background points, all outside the window and its 2 px shadow.
BACKGROUND = [(2, 2), (317, 2), (2, 197), (317, 197), (5, 100), (160, 5)]


def count_color(frame, x0, y0, x1, y1, color):
    """How many of the sampled logical (320x200) pixels have this colour."""
    return sum(color_matches(pixel(frame, x, y), color)
               for y in range(y0, y1) for x in range(x0, x1))


def background_is_blue(frame):
    # The desktop is one flat blue. The old gradient painted green, cyan and
    # red bands down the screen; those must not come back.
    return all(color_matches(pixel(frame, x, y), DESKTOP_BLUE)
               for x, y in BACKGROUND)


def welcome_visible(frame):
    """The startup screen: only a welcome message on the blue desktop."""
    if frame[:2] != (640, 400) or not background_is_blue(frame):
        return False
    # No window yet: the middle of the screen is blue except for the text.
    if not color_matches(pixel(frame, TERM_X + 4, TERM_Y + 4), DESKTOP_BLUE):
        return False
    return count_color(frame, 40, 84, 280, 100, TERMINAL_WHITE) > 100


def terminal_text_pixels(frame):
    """White pixels inside the TTY window's client area."""
    return count_color(frame, TERM_X + 6, TERM_Y + 22,
                       TERM_X + TERM_W - 6, TERM_Y + TERM_H - 6, TERMINAL_WHITE)


def desktop_visible(frame):
    if frame[:2] != (640, 400) or not background_is_blue(frame):
        return False
    # Focused title bar, dark terminal client area, and the banner text the
    # terminal prints when it opens.
    if not color_matches(pixel(frame, 160, TERM_Y + 8), TITLEBAR_BLUE):
        return False
    if not color_matches(pixel(frame, 160, TERM_Y + TERM_H - 20), TERMINAL_BLACK):
        return False
    return terminal_text_pixels(frame) > 200


def exercise(mon, folder):
    capture = folder / "screen.ppm"
    wait_for(lambda: "$" in mon.tty(1), "interactive shell")
    # Wait for the renderer too: it may lag behind the guest's text writes.
    def visible_shell():
        frame = mon.frame(capture)
        if frame[:2] == (720, 400) and sum(bool(b) for b in frame[2][:720 * 48 * 3]) > 1000:
            return frame
        return None
    baseline = wait_for(visible_shell, "visible shell")
    # The first three rows never change during this test. Exact pixels catch
    # font/attribute corruption that a text-memory read cannot detect.
    header_bytes = baseline[0] * 48 * 3
    header = baseline[2][:header_bytes]
    assert any(header), "default console must show the shell, not an empty screen"

    def graphics():
        f = mon.frame(capture)
        return f if desktop_visible(f) else None

    def text_restored():
        f = mon.frame(capture)
        return f[:2] == baseline[:2] and f[2][:header_bytes] == header

    def welcome():
        f = mon.frame(capture)
        return f if welcome_visible(f) else None

    for session in range(2):
        # A stale ESC in text mode must not instantly close the next session.
        mon.key("esc")
        mon.type("desktop\n")
        # Startup shows the welcome message first, and only then the TTY window.
        wait_for(welcome, "welcome message")
        print(f"PASS: desktop session {session + 1} shows the welcome message",
              flush=True)
        frame = wait_for(graphics, "TTY window")
        print(f"PASS: desktop session {session + 1} opens the TTY window",
              flush=True)
        time.sleep(0.25)
        assert graphics(), "desktop closed immediately after launch"

        if session == 0:
            mon.command("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": 25}},
                {"type": "rel", "data": {"axis": "y", "value": 15}},
            ])
            wait_for(lambda: (f := mon.frame(capture))[:2] == frame[:2] and
                     f[2] != frame[2] and desktop_visible(f), "mouse cursor movement")
            # Switching text consoles in graphics mode used to overwrite the
            # CRTC start address and make the desktop vanish/scroll.
            mon.key("alt", "f1")
            assert graphics(), "console shortcut corrupted the graphics display"
            print("PASS: mouse input and graphics console isolation", flush=True)

            # The TTY window is a real terminal: typing must echo into it and
            # a command must answer there, not in the text console behind it.
            before = terminal_text_pixels(mon.frame(capture))
            mon.type("ver\n")
            wait_for(lambda: (f := mon.frame(capture)) and desktop_visible(f) and
                     terminal_text_pixels(f) > before, "terminal echo and output")
            print("PASS: the TTY window accepts typed commands", flush=True)

        mon.key("esc")
        wait_for(lambda: mon.tty(1).count("[desktop finished]") == session + 1,
                 "desktop command completion")
        wait_for(text_restored, "readable text, font and palette restoration")
        print("PASS: ESC restores the visible shell and its font", flush=True)

    mon.type("echo ready\n")
    wait_for(lambda: "\nready\n$" in mon.tty(1), "echo after desktop exit")
    print("PASS: shell accepts commands after desktop exit", flush=True)

    # TASK_GFX shares the mode-switch driver with TASK_DESKTOP.
    mon.type("demo\n")
    wait_for(lambda: (f := mon.frame(capture))[:2] == (640, 400) and
             color_matches(pixel(f, 30, 175), (0, 0, 170)) and
             color_matches(pixel(f, 50, 175), (0, 170, 0)), "graphics demo color bars")
    mon.key("esc")
    wait_for(lambda: "[demo finished]" in mon.tty(1), "demo completion")
    wait_for(text_restored, "text restoration after demo")
    print("PASS: demo still enters/exits graphics correctly", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-i386")
    parser.add_argument("--image-dir", type=Path, default=OS_DIR / "build/desktop")
    parser.add_argument("--boot", choices=("floppy", "hd", "iso"), default="floppy")
    args = parser.parse_args()
    images = args.image_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="noxis-desktop-") as temporary:
        folder = Path(temporary)
        monitor = folder / "monitor.sock"
        command = [args.qemu, "-m", "32", "-vga", "std", "-snapshot", "-display", "none",
                   "-serial", "none", "-no-reboot", "-no-shutdown",
                   "-qmp", f"unix:{monitor},server=on,wait=off",
                   "-drive", f"file={images / '100m.img'},format=raw,if=ide,index=0"]
        if args.boot == "floppy":
            command += ["-drive", f"file={images / 'a.img'},format=raw,if=floppy", "-boot", "a"]
        elif args.boot == "iso":
            command += ["-cdrom", str(images / "osfs11.iso"), "-boot", "d"]
        else:
            command += ["-boot", "c"]
        with (folder / "qemu.log").open("w+") as log:
            qemu = subprocess.Popen(command, stdout=log, stderr=log)
            mon = None
            try:
                def started():
                    if qemu.poll() is not None:
                        log.seek(0)
                        raise RuntimeError("QEMU failed: " + log.read())
                    return monitor.exists()
                wait_for(started, "QMP socket", timeout=10)
                mon = Monitor(monitor)
                exercise(mon, folder)
                print(f"ALL DESKTOP QEMU CHECKS PASSED ({args.boot})", flush=True)
            except Exception:
                if mon:
                    try:
                        print("--- TTY0 ---\n" + mon.tty(0))
                        print("--- TTY1 ---\n" + mon.tty(1))
                    except (OSError, RuntimeError):
                        pass
                raise
            finally:
                if mon:
                    mon.close()
                qemu.terminate()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    qemu.kill()
                    qemu.wait()


if __name__ == "__main__":
    main()
