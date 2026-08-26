#!/usr/bin/env python3
"""Boot osfs11.img in QEMU (no display), read the VGA text buffer via the
monitor, print it. Used to confirm the prebuilt image boots and to give a
behavioural baseline before/after build fixes."""
import os, re, socket, subprocess, sys, time

MODE = sys.argv[1] if len(sys.argv) > 1 else "floppy"   # floppy | cdrom
IMG = sys.argv[2] if len(sys.argv) > 2 else "osfs11.iso"
HD = sys.argv[3] if len(sys.argv) > 3 else None
MON = "/tmp/osfs11-qmon"
if os.path.exists(MON):
    os.unlink(MON)

if MODE == "cdrom":
    cmd = ["qemu-system-i386", "-m", "32", "-cdrom", IMG, "-boot", "d",
           "-display", "none", "-no-reboot", "-no-shutdown",
           "-monitor", f"unix:{MON},server,nowait"]
else:
    cmd = ["qemu-system-i386", "-m", "32", "-fda", IMG,
           "-display", "none", "-no-reboot", "-no-shutdown",
           "-monitor", f"unix:{MON},server,nowait"]
if HD:
    cmd += ["-drive", f"if=ide,format=raw,file={HD},index=0,media=disk"]

qemu = subprocess.Popen(
    cmd,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    s = None
    deadline = time.time() + 5
    while True:
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(MON); break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() > deadline:
                raise RuntimeError("monitor socket never appeared")
            time.sleep(0.1)
    s.settimeout(0.5)

    def screen():
        s.sendall(b"xp /2000xb 0xb8000\n")
        buf = b""
        try:
            while len(buf) < 100000:
                d = s.recv(65536)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        vals = []
        for line in buf.decode("latin1").splitlines():
            if ":" in line:
                vals += re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])
        chars = [chr(int(v, 16)) if 32 <= int(v, 16) < 127 else " "
                 for v in vals[::2][:2000]]
        return ["".join(chars[r*80:(r+1)*80]).rstrip() for r in range(25)]

    for t in (3, 8, 15):
        time.sleep(t if t == 3 else (t - prev_t))
        prev_t = t
        rows = screen()
        print(f"--- t={t}s ---")
        for r in rows:
            if r.strip():
                print(r)
        sys.stdout.flush()
    s.close()
finally:
    qemu.kill()
    try:
        qemu.wait(timeout=5)
    except Exception:
        pass
