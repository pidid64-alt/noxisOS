#!/usr/bin/env python3
"""Boot fresh noxisOS desktop images and check the visible VGA output.

Uses QMP over a private Unix socket and captures the screen with `screendump`;
no GUI/VNC server or third-party Python packages are needed.  The desktop is
800x600x8 VBE and has two windows: Files on the left and TTY on the right.
`-snapshot` keeps the IDE image unchanged.

The test covers the welcome screen, both desktop windows, mouse and keyboard
input (including TTY command-history navigation), return to the text shell, a
second desktop session, and the 800x600 TASK_GFX colour-bar demo. Every wait
checks an observable result rather than assuming a fixed boot or rendering
delay.
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

# Keep these in sync with include/sys/const.h and kernel/console.c.
TEXT_VIDEO_BASE = 0xB8000
TEXT_VIDEO_SIZE = 0x8000
TEXT_COLUMNS = 80
TEXT_CONSOLES = 3
# init_screen() advances each console's `orig` by the unrounded size_per_con;
# only con_size is rounded down to a complete number of text rows.
WORDS_PER_CONSOLE = TEXT_VIDEO_SIZE // 2 // TEXT_CONSOLES
TEXT_CONSOLE_READ_BYTES = 4000  # 80 columns x 25 rows x 2 bytes/cell
TEXT_SCREEN_SIZE = (720, 400)

GFX_SIZE = (800, 600)
DESKTOP_BLUE = (0, 0, 170)
TITLEBAR_ACTIVE = (85, 85, 255)
TITLEBAR_INACTIVE = (170, 170, 170)
WINDOW_BACKGROUND = (170, 170, 170)
TERMINAL_BACKGROUND = (0, 0, 0)
TERMINAL_TEXT = (255, 255, 255)

# Coordinates come from include/wm.h: Files at (8, 8), 288x584; TTY at
# (328, 8), 456x408. Keep the rendering constants here in sync with wm.h.
FILES_WINDOW = (8, 8, 288, 584)
TTY_WINDOW = (328, 8, 456, 408)
WM_BORDER_WIDTH = 2
WM_TITLE_HEIGHT = 18
WM_TERM_PAD = 2
WM_FONT_WIDTH = 8
WM_FONT_HEIGHT = 16
FILES_X, FILES_Y, FILES_W, FILES_H = FILES_WINDOW
TTY_X, TTY_Y, TTY_W, TTY_H = TTY_WINDOW
FILES_TITLE_TEXT = (
    FILES_X + WM_BORDER_WIDTH + 2,
    FILES_Y + WM_BORDER_WIDTH,
    FILES_X + WM_BORDER_WIDTH + 2 + len("Files: /") * WM_FONT_WIDTH,
    FILES_Y + WM_BORDER_WIDTH + WM_FONT_HEIGHT,
)
TTY_TITLE_TEXT = (
    TTY_X + WM_BORDER_WIDTH + 2,
    TTY_Y + WM_BORDER_WIDTH,
    TTY_X + WM_BORDER_WIDTH + 2 + len("TTY") * WM_FONT_WIDTH,
    TTY_Y + WM_BORDER_WIDTH + WM_FONT_HEIGHT,
)
TTY_TEXT_RECT = (
    TTY_X + WM_BORDER_WIDTH + WM_TERM_PAD,
    TTY_Y + WM_TITLE_HEIGHT + WM_TERM_PAD,
    TTY_X + TTY_W - WM_BORDER_WIDTH - WM_TERM_PAD,
    TTY_Y + TTY_H - WM_BORDER_WIDTH - WM_TERM_PAD,
)  # half-open rectangle; excludes the cursor gutter

# Points outside both windows, their shadows, and the initial mouse cursor.
DESKTOP_BACKGROUND_POINTS = (
    (4, 4), (320, 100), (4, 596), (796, 4), (796, 596),
)


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.reader = self.socket.makefile("rb")
        json.loads(self.reader.readline())  # QMP greeting
        self.command("qmp_capabilities")

    def close(self):
        self.reader.close()
        self.socket.close()

    def command(self, name, **arguments):
        request = {
            "execute": name,
            "arguments": {key.replace("_", "-"): value
                          for key, value in arguments.items()},
        }
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
        self.command("send-key", keys=[{"type": "qcode", "data": code}
                                       for code in codes], hold_time=50)
        time.sleep(0.10)  # let QEMU release the key before the next one

    def type(self, text):
        for char in text:
            self.key({" ": "spc", "\n": "ret"}.get(char, char))

    def tty(self, number):
        """Read the visible text page for one of the three BIOS consoles."""
        if not 0 <= number < TEXT_CONSOLES:
            raise ValueError("console number out of range")
        start = TEXT_VIDEO_BASE + 2 * WORDS_PER_CONSOLE * number
        result = self.command(
            "human-monitor-command",
            command_line=f"xp /{TEXT_CONSOLE_READ_BYTES}bx {start:#x}",
        )
        values = [int(byte, 16)
                  for line in result.splitlines() if ":" in line
                  for byte in re.findall(r"0x([0-9a-fA-F]{2})\b",
                                         line.split(":", 1)[1])]
        chars = "".join(chr(value) if 32 <= value < 127 else " "
                        for value in values[::2])
        return "\n".join(chars[i:i + TEXT_COLUMNS].rstrip()
                          for i in range(0, len(chars), TEXT_COLUMNS))

    def frame(self, path):
        self.command("screendump", filename=str(path))
        try:
            magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
        except ValueError as exc:
            raise RuntimeError("invalid QEMU PPM screen dump") from exc
        width, height = map(int, dimensions.split())
        if (magic != b"P6" or maximum != b"255" or
                len(pixels) != width * height * 3):
            raise RuntimeError("invalid QEMU PPM screen dump")
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
    if not (0 <= x < width and 0 <= y < height):
        raise ValueError(f"pixel ({x}, {y}) outside {width}x{height} frame")
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset + 3])


def color_matches(actual, expected):
    # VGA DAC channels are six-bit; QEMU versions can round the expansion to
    # eight-bit RGB slightly differently (e.g. 168 vs 170).
    return all(abs(a - b) <= 3 for a, b in zip(actual, expected))


def count_color(frame, rect, color):
    """Count pixels matching a palette colour inside a half-open rectangle."""
    width, height, pixels = frame
    x0, y0, x1, y1 = rect
    x0, x1 = max(0, x0), min(width, x1)
    y0, y1 = max(0, y0), min(height, y1)
    red, green, blue = color
    red_min, red_max = max(0, red - 3), min(255, red + 3)
    green_min, green_max = max(0, green - 3), min(255, green + 3)
    blue_min, blue_max = max(0, blue - 3), min(255, blue + 3)
    count = 0
    for y in range(y0, y1):
        offset = (y * width + x0) * 3
        for _ in range(x0, x1):
            if (red_min <= pixels[offset] <= red_max and
                    green_min <= pixels[offset + 1] <= green_max and
                    blue_min <= pixels[offset + 2] <= blue_max):
                count += 1
            offset += 3
    return count


def background_is_blue(frame):
    return (frame[:2] == GFX_SIZE and
            all(color_matches(pixel(frame, x, y), DESKTOP_BLUE)
                for x, y in DESKTOP_BACKGROUND_POINTS))


def welcome_visible(frame):
    """The welcome splash is white text centered on a flat blue desktop."""
    if not background_is_blue(frame):
        return False
    # "Welcome to noxisOS" is centered near (328, 284) in the 8x16 font.
    return count_color(frame, (300, 270, 500, 320), TERMINAL_TEXT) > 100


def terminal_text_pixels(frame):
    return count_color(frame, TTY_TEXT_RECT, TERMINAL_TEXT)


def terminal_row_image(frame, row):
    """Return one TTY text row, including the cursor, for navigation checks."""
    width, height, pixels = frame
    x0, _, x1, _ = TTY_TEXT_RECT
    y0 = TTY_TEXT_RECT[1] + row * WM_FONT_HEIGHT
    y1 = y0 + WM_FONT_HEIGHT
    if row < 0 or y1 > height:
        raise ValueError(f"TTY row {row} outside {width}x{height} frame")
    return b"".join(pixels[(y * width + x0) * 3:
                            (y * width + x1) * 3]
                   for y in range(y0, y1))


def desktop_visible(frame):
    """Check that both current windows and the flat desktop are on screen."""
    if not background_is_blue(frame):
        return False

    # The Files window is inactive, while TTY receives keyboard input. Sample
    # their title bars away from the title glyphs, plus unobstructed client
    # pixels near the right edge of each window.
    if not color_matches(pixel(frame, FILES_X + 3, FILES_Y + 3),
                         TITLEBAR_INACTIVE):
        return False
    if not color_matches(pixel(frame, TTY_X + 3, TTY_Y + 3),
                         TITLEBAR_ACTIVE):
        return False
    if not color_matches(pixel(frame, FILES_X + FILES_W - 4,
                                FILES_Y + FILES_H // 2), WINDOW_BACKGROUND):
        return False
    if not color_matches(pixel(frame, TTY_X + TTY_W - 4,
                                TTY_Y + TTY_H - 16), TERMINAL_BACKGROUND):
        return False

    files_title = count_color(frame, FILES_TITLE_TEXT, TERMINAL_TEXT)
    tty_title = count_color(frame, TTY_TITLE_TEXT, TERMINAL_TEXT)
    return files_title > 20 and tty_title > 8 and terminal_text_pixels(frame) > 100


def exercise_tty_history(mon, capture):
    """Exercise recent/older history navigation and draft restoration."""
    def row_frame(row, expected=None, different_from=None):
        frame = mon.frame(capture)
        if not desktop_visible(frame):
            return None
        image = terminal_row_image(frame, row)
        if expected is not None and image != expected:
            return None
        if different_from is not None and image == different_from:
            return None
        return frame

    # The preceding `ver` command leaves the next prompt on row 4. Two lines
    # later is still blank; the command below will put its next prompt on row 6.
    frame = mon.frame(capture)
    assert desktop_visible(frame), "desktop disappeared before history test"
    blank_row_6 = terminal_row_image(frame, 6)
    mon.type("echo history_probe\n")
    prompt_frame = wait_for(
        lambda: row_frame(6, different_from=blank_row_6),
        "TTY prompt after history probe",
    )
    prompt_image = terminal_row_image(prompt_frame, 6)

    mon.key("up")
    latest_frame = wait_for(
        lambda: row_frame(6, different_from=prompt_image),
        "Up to recall the latest command",
    )
    latest_image = terminal_row_image(latest_frame, 6)
    mon.key("down")
    wait_for(lambda: row_frame(6, expected=prompt_image),
             "Down to restore an empty draft")

    # Submit the recalled command once more. Its adjacent duplicate is not
    # needed in history; Up again below should therefore reach the older `ver`.
    blank_row_8 = terminal_row_image(prompt_frame, 8)
    mon.key("up")
    wait_for(lambda: row_frame(6, different_from=prompt_image),
             "Up to recall a command for re-execution")
    mon.key("ret")
    row8_prompt = wait_for(
        lambda: row_frame(8, different_from=blank_row_8),
        "TTY prompt after re-executing history",
    )
    row8_prompt_image = terminal_row_image(row8_prompt, 8)

    mon.type("draft")
    draft_frame = wait_for(
        lambda: row_frame(8, different_from=row8_prompt_image),
        "typed draft in the TTY",
    )
    draft_image = terminal_row_image(draft_frame, 8)

    mon.key("up")
    latest_frame = wait_for(
        lambda: row_frame(8, different_from=draft_image),
        "Up to recall the newest command",
    )
    latest_image = terminal_row_image(latest_frame, 8)
    mon.key("up")
    older_frame = wait_for(
        lambda: row_frame(8, different_from=latest_image),
        "Up to recall an older command",
    )
    older_image = terminal_row_image(older_frame, 8)
    assert older_image != latest_image, "history entries should be distinct"

    mon.key("down")
    wait_for(lambda: row_frame(8, expected=latest_image),
             "Down to move forward in command history")
    mon.key("down")
    wait_for(lambda: row_frame(8, expected=draft_image),
             "Down past newest command to restore the draft")
    print("PASS: TTY Up/Down history and draft restoration", flush=True)


def demo_pattern_visible(frame):
    """Recognize several clean colour bars below the demo's upper shapes."""
    if frame[:2] != GFX_SIZE:
        return False
    samples = (
        ((10, 175), (0, 0, 0)),
        ((30, 175), (0, 0, 170)),
        ((50, 175), (0, 170, 0)),
        ((70, 175), (0, 170, 170)),
        ((90, 175), (170, 0, 0)),
    )
    return all(color_matches(pixel(frame, x, y), expected)
               for (x, y), expected in samples)


def exercise(mon, folder):
    capture = folder / "screen.ppm"
    wait_for(lambda: "$" in mon.tty(1), "interactive text shell")

    # Text mode remains 720x400. Wait for a real screen image, not just a
    # string in video memory, so restoration checks have a stable baseline.
    def visible_shell():
        frame = mon.frame(capture)
        if (frame[:2] == TEXT_SCREEN_SIZE and
                sum(bool(byte) for byte in frame[2][:720 * 48 * 3]) > 1000):
            return frame
        return None

    baseline = wait_for(visible_shell, "visible text shell")
    header_bytes = baseline[0] * 48 * 3
    header = baseline[2][:header_bytes]
    assert any(header), "default console must show the shell, not an empty screen"

    def graphics():
        frame = mon.frame(capture)
        return frame if desktop_visible(frame) else None

    def text_restored():
        frame = mon.frame(capture)
        return (frame[:2] == baseline[:2] and
                frame[2][:header_bytes] == header)

    def welcome():
        frame = mon.frame(capture)
        return frame if welcome_visible(frame) else None

    for session in range(2):
        # Discard any ESC left in the TTY queue before launching a fresh GUI.
        mon.key("esc")
        mon.type("desktop\n")
        wait_for(welcome, "blue welcome screen")
        print(f"PASS: desktop session {session + 1} shows the welcome screen",
              flush=True)

        frame = wait_for(graphics, "Files and TTY windows")
        print(f"PASS: desktop session {session + 1} opens Files and TTY",
              flush=True)
        time.sleep(0.25)
        assert graphics(), "desktop closed immediately after launch"

        if session == 0:
            # The initial mouse cursor is visible; a relative mouse event must
            # move it without changing the desktop layout.
            mon.command("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": 25}},
                {"type": "rel", "data": {"axis": "y", "value": 15}},
            ])
            def mouse_moved():
                updated = mon.frame(capture)
                if (updated[:2] == frame[:2] and updated[2] != frame[2]
                        and desktop_visible(updated)):
                    return updated
                return None

            moved = wait_for(mouse_moved, "mouse cursor movement")
            print("PASS: mouse input moves the cursor", flush=True)

            # A console-switch shortcut must not take the screen away from the
            # graphics task while the desktop owns the display.
            mon.key("alt", "f1")
            assert graphics(), "console shortcut corrupted the desktop display"
            print("PASS: graphics console isolation", flush=True)

            # Verify that keystrokes reach the TTY and its `ver` command draws
            # additional text in the TTY client, rather than behind the GUI.
            before = terminal_text_pixels(moved)
            mon.type("ver\n")

            def terminal_command_visible():
                updated = mon.frame(capture)
                if (desktop_visible(updated) and
                        terminal_text_pixels(updated) >= before + 50):
                    return updated
                return None

            wait_for(terminal_command_visible, "TTY command output")
            print("PASS: TTY accepts commands and renders their output",
                  flush=True)

            exercise_tty_history(mon, capture)

        mon.key("esc")
        wait_for(lambda: mon.tty(1).count("[desktop finished]") >= session + 1,
                 "desktop command completion")
        wait_for(text_restored, "text-mode screen, font and palette restoration")
        print("PASS: ESC restores the text shell and font", flush=True)

    mon.type("echo ready\n")
    wait_for(lambda: "\nready\n$" in mon.tty(1), "shell input after desktop exit")
    print("PASS: shell accepts commands after desktop exit", flush=True)

    # TASK_GFX uses the same VBE mode-switch driver as TASK_DESKTOP. Its static
    # phase has clean colour bars below y=150 before the bouncing-ball phase.
    mon.type("demo\n")

    def demo_visible():
        frame = mon.frame(capture)
        return frame if demo_pattern_visible(frame) else None

    wait_for(demo_visible, "800x600 demo colour bars", timeout=120)
    mon.key("esc")
    wait_for(lambda: "[demo finished]" in mon.tty(1), "demo completion")
    wait_for(text_restored, "text-mode restoration after demo")
    print("PASS: demo renders 800x600 colour bars and restores text mode",
          flush=True)


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
        command = [
            args.qemu, "-m", "32", "-vga", "std", "-snapshot", "-display", "none",
            "-serial", "none", "-no-reboot", "-no-shutdown",
            "-qmp", f"unix:{monitor},server=on,wait=off",
            "-drive", f"file={images / '100m.img'},format=raw,if=ide,index=0",
        ]
        if args.boot == "floppy":
            command += ["-drive", f"file={images / 'a.img'},format=raw,if=floppy",
                        "-boot", "a"]
        elif args.boot == "iso":
            command += ["-cdrom", str(images / "osfs11.iso"), "-boot", "d"]
        else:
            command += ["-boot", "c"]

        log_path = folder / "qemu.log"
        with log_path.open("w+") as log:
            qemu = subprocess.Popen(command, stdout=log, stderr=log)
            monitor_client = None
            try:
                def started():
                    if qemu.poll() is not None:
                        log.seek(0)
                        raise RuntimeError("QEMU failed: " + log.read())
                    return monitor.exists()

                wait_for(started, "QMP socket", timeout=10)
                monitor_client = Monitor(monitor)
                exercise(monitor_client, folder)
                print(f"ALL DESKTOP QEMU CHECKS PASSED ({args.boot})", flush=True)
            except Exception:
                if monitor_client:
                    try:
                        print("--- TTY0 ---\n" + monitor_client.tty(0))
                        print("--- TTY1 ---\n" + monitor_client.tty(1))
                    except (OSError, RuntimeError):
                        pass
                raise
            finally:
                if monitor_client:
                    monitor_client.close()
                qemu.terminate()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    qemu.kill()
                    qemu.wait()


if __name__ == "__main__":
    main()
