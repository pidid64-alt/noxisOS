#!/usr/bin/env python3
"""Headless QEMU test for the TASK_GFX demo (25-osfs11).

Usage: gfx_qemu_test.py [hd|iso]   (default: hd)

Boots the OS, waits for the shell (screen quiescence -- boot takes 20-70 s
under TCG, so a fixed sleep cannot work), types `demo`, captures the
rendered VGA surface via the QEMU monitor `screendump`, and verifies:
  (a) snapshot 1 matches the deterministic vertical colour-bar pattern;
  (b) snapshot 2 differs (the bouncing ball has moved);
  (c) after ESC the VGA is back in text mode (no colour-bar frame present).

  hd  : -hda 100m.img -boot c        (kernel from the HD boot chain)
  iso : -cdrom osfs11.iso -boot d -hda 100m.img
        (kernel from the floppy image inside the ISO; the HD must still be
         attached -- cmd.tar with the user commands is untarred from it)

The VGA framebuffer (0xA0000) is a device mapping, not guest RAM, so it is
captured with `screendump` against a VNC display backend rather than
`pmemsave`. The captured image is a PPM (P6) scaled by QEMU; we scale the
expected 320x200 pattern up to the captured size.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OS_DIR = os.path.abspath(os.path.join(HERE, ".."))
HD_IMG = os.path.join(OS_DIR, "100m.img")
MON_SOCK = "/tmp/gfx_qemu_mon.sock"
SD1 = "/tmp/gfx_sd1.ppm"
SD2 = "/tmp/gfx_sd2.ppm"
SD3 = "/tmp/gfx_sd3.ppm"


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


class QemuMonitor:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self._read_prompt()

    def _read_prompt(self):
        self.s.settimeout(6)
        buf = b""
        while True:
            if buf.rstrip().endswith(b"(qemu)"):
                break
            chunk = self.s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf

    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        return self._read_prompt()

    def sendkey(self, key):
        self.cmd("sendkey " + key)

    def type_text(self, text):
        for ch in text:
            self.sendkey(ch)

    def screendump(self, path):
        self.cmd("screendump %s" % path)


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    assert d[:2] == b"P6", "unexpected magic %r" % d[:2]
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(d) and d[idx] in b" \t\n":
            idx += 1
        end = idx
        while end < len(d) and d[end] not in b" \t\n":
            end += 1
        fields.append(int(d[idx:end]))
        idx = end
    w, h, maxval = fields
    while d[idx] != ord("\n"):
        idx += 1
    idx += 1
    px = d[idx:]
    assert len(px) == w * h * 3, "pixel len %d != %d" % (len(px), w * h * 3)
    return w, h, px


def pattern_color(x320):
    return (x320 // 20) % 16


def get_linear_pixel(px, w, h, lx, ly):
    sx = int((lx + 0.5) * w / 320)
    sy = int((ly + 0.5) * h / 200)
    sx = min(max(sx, 0), w - 1)
    sy = min(max(sy, 0), h - 1)
    o = (sy * w + sx) * 3
    return px[o], px[o + 1], px[o + 2]


def _frame_diff(px1, px2):
    """Count pixels that changed meaningfully between two frames."""
    if len(px1) != len(px2):
        return 1 << 30
    diff = 0
    for i in range(0, len(px1), 3):
        if (abs(px1[i] - px2[i]) > 40 or
                abs(px1[i + 1] - px2[i + 1]) > 40 or
                abs(px1[i + 2] - px2[i + 2]) > 40):
            diff += 1
    return diff


def _screen_idle(mon, frames=4):
    """True when `frames` consecutive dumps (2 s apart) differ only by the
    blinking cursor cell (~150 px)."""
    prev = None
    for _ in range(frames):
        time.sleep(2)
        mon.screendump("/tmp/gfx_poll.ppm")
        _, _, px = read_ppm("/tmp/gfx_poll.ppm")
        if prev is not None and _frame_diff(prev, px) >= 400:
            return False
        prev = px
    return True


def _graphics_present(path):
    """True when the captured screen has saturated colour (mode 13h output).
    Text mode is pure black/white, so any colourful frame means graphics."""
    _, _, px = read_ppm(path)
    colorful = 0
    for i in range(0, len(px), 3):
        if max(px[i], px[i + 1], px[i + 2]) - min(px[i], px[i + 1], px[i + 2]) > 60:
            colorful += 1
    return colorful > len(px) // 3 // 20      # > 5 % of pixels


def wait_and_launch_demo(mon, timeout=300):
    """Boot pauses look idle (the HD loader freezes 10+ s on its RAM dump,
    and the shell draws no visible prompt to probe). So instead of
    detecting the shell, detect the OUTCOME: wait for a quiet screen, type
    `demo`, and poll for graphics. If nothing appears, that quiet window
    was a boot pause (keystrokes lost) -- wait for the next one and retry.

    Returns as soon as graphics output is visible (still inside the ~2 s
    static-pattern phase, so the caller can snapshot it).
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not _screen_idle(mon):
            continue
        mon.type_text("demo")
        mon.sendkey("ret")
        launch = time.time()
        while time.time() - launch < 12:
            time.sleep(0.3)
            mon.screendump("/tmp/gfx_poll.ppm")
            if _graphics_present("/tmp/gfx_poll.ppm"):
                return
    fail("demo never produced graphics within %ds" % timeout)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "hd"
    if mode == "iso":
        boot_args = ["-cdrom", os.path.join(OS_DIR, "osfs11.iso"),
                     "-boot", "d", "-hda", HD_IMG]
    elif mode == "hd":
        boot_args = ["-hda", HD_IMG, "-boot", "c"]
    else:
        fail("unknown mode %r (use hd or iso)" % mode)

    if not os.path.exists(HD_IMG):
        fail("HD image not found: %s" % HD_IMG)
    for f in (SD1, SD2, SD3, MON_SOCK):
        if os.path.exists(f):
            os.remove(f)

    qemu = subprocess.Popen(
        ["qemu-system-i386"] + boot_args +
        ["-m", "32",
         "-vnc", ":0",
         "-monitor", "unix:%s,server,nowait" % MON_SOCK],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        for _ in range(50):
            if os.path.exists(MON_SOCK):
                break
            time.sleep(0.2)
        else:
            fail("QEMU monitor socket never appeared")

        time.sleep(1)
        mon = QemuMonitor(MON_SOCK)
        wait_and_launch_demo(mon)

        # Snapshot 1: still inside the ~2 s static pattern phase.
        time.sleep(0.3)
        mon.screendump(SD1)
        w, h, px1 = read_ppm(SD1)

        # Snapshot 2: during the bouncing-ball phase (> 2 s).
        time.sleep(3.5)
        mon.screendump(SD2)
        _, _, px2 = read_ppm(SD2)

        # (a) vertical colour-bar pattern, sampled at several rows/columns.
        palette = {
            0: (0, 0, 0), 1: (0, 0, 170), 2: (0, 170, 0),
            3: (0, 170, 170), 4: (170, 0, 0), 5: (170, 0, 170),
            6: (170, 85, 0), 7: (170, 170, 170), 8: (85, 85, 85),
            9: (85, 85, 255), 10: (85, 255, 85), 11: (85, 255, 255),
            12: (255, 85, 85), 13: (255, 85, 255), 14: (255, 255, 85),
            15: (255, 255, 255),
        }
        bad = 0
        for ly in (20, 100, 180):
            for lx in (10, 60, 110, 160, 210, 270, 310):
                r, g, b = get_linear_pixel(px1, w, h, lx, ly)
                er, eg, eb = palette[pattern_color(lx)]
                if abs(r - er) > 30 or abs(g - eg) > 30 or abs(b - eb) > 30:
                    bad += 1
        if bad > 3:
            fail("pattern mismatch at %d sampled points" % bad)
        print("PASS: pattern phase matches vertical colour bars")

        # (b) snapshot 2 differs from snapshot 1 (animation moved).
        diff_px = 0
        total = w * h
        for i in range(0, len(px1), 3):
            if px1[i] != px2[i] or px1[i + 1] != px2[i + 1] or px1[i + 2] != px2[i + 2]:
                diff_px += 1
        if diff_px < total * 0.1:
            fail("frames too similar (%d changed) - animation not moving" % diff_px)
        print("PASS: snapshots differ (animation moving, %d changed px)" % diff_px)

        # (c) quit via ESC, verify text mode restored (no full-screen colour bar).
        mon.sendkey("esc")
        time.sleep(1.5)
        mon.screendump(SD3)
        _, _, px3 = read_ppm(SD3)
        bar_like = 0
        for ly in (20, 100, 180):
            for lx in (10, 60, 110, 160, 210, 270, 310):
                r, g, b = get_linear_pixel(px3, w, h, lx, ly)
                if max(r, g, b) - min(r, g, b) > 80 and max(r, g, b) > 120:
                    bar_like += 1
        if bar_like > 3:
            fail("after ESC colour-bar still visible (%d pts) - mode not restored" % bar_like)
        print("PASS: after ESC VGA back in text mode (no colour bars)")

    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except Exception:
            qemu.kill()

    print("\nALL CHECKS PASSED")


if __name__ == "__main__":
    main()
