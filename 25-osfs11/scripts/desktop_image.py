#!/usr/bin/env python3
"""Build matching desktop boot/IDE images without mounts or root privileges.

Only files in --output are written. Existing files on the output IDE image
are preserved, except for the kernel/loader and the bundled command archive.
The checked-in images are historical inputs, not build outputs.
"""
import argparse
from contextlib import ExitStack
import fcntl
import gzip
import io
from pathlib import Path
import re
import shutil
import struct
import tarfile
import tempfile

OS_DIR = Path(__file__).resolve().parents[1]
SECTOR = 512


def constant(path, name):
    match = re.search(r"^\s*(?:#define\s+)?" + name +
                      r"\s+(?:equ\s+)?(0x[0-9a-fA-F]+|[0-9]+)\b",
                      path.read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f"cannot read {name} from {path}")
    return int(match[1], 0)


def floppy_image(boot, files):
    """The boot sector's BPB describes the standard 1.44 MB FAT12 layout."""
    if len(boot) != SECTOR or boot[510:] != b"\x55\xaa":
        raise ValueError("invalid floppy boot sector")
    bps, spc, reserved, fats, entries, sectors = struct.unpack_from(
        "<HBHBHH", boot, 11)
    sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
    if (bps, spc, reserved, fats, entries, sectors, sectors_per_fat) != (
            512, 1, 1, 2, 224, 2880, 9):
        raise ValueError("expected a 1.44 MB FAT12 boot sector")
    disk = bytearray(sectors * bps)
    disk[:SECTOR] = boot
    fat = bytearray(sectors_per_fat * bps)
    fat[:3] = bytes((boot[21], 0xFF, 0xFF))
    root_start = (reserved + fats * sectors_per_fat) * bps
    data_start = root_start + entries * 32
    cluster = 2
    for index, (name, data) in enumerate(files):
        count = (len(data) + bps - 1) // bps
        if not count or data_start + (cluster - 2 + count) * bps > len(disk):
            raise ValueError("file does not fit the floppy")
        entry = root_start + index * 32
        disk[entry:entry + 11] = name
        disk[entry + 11] = 0x20
        struct.pack_into("<HI", disk, entry + 26, cluster, len(data))
        start = data_start + (cluster - 2) * bps
        disk[start:start + len(data)] = data
        for current in range(cluster, cluster + count):
            next_cluster = current + 1 if current + 1 < cluster + count else 0xFFF
            offset = current * 3 // 2
            pair = struct.unpack_from("<H", fat, offset)[0]
            if current & 1:
                pair = (pair & 0x000F) | (next_cluster << 4)
            else:
                pair = (pair & 0xF000) | next_cluster
            struct.pack_into("<H", fat, offset, pair)
        cluster += count
    for index in range(fats):
        start = (reserved + index * sectors_per_fat) * bps
        disk[start:start + len(fat)] = fat
    return disk


def replace_boot_files(disk, root_sector, files):
    """Update existing Orange'S FS inodes so HD boot also uses the new kernel."""
    base = root_sector * SECTOR
    disk.seek(base + SECTOR)
    sb = struct.unpack("<14I", disk.read(56))
    if sb[0] != 0x111 or sb[8] != 32 or sb[11:] != (16, 0, 4):
        raise ValueError("unsupported Orange'S filesystem in the IDE image")
    inode_start = base + (2 + sb[3] + sb[4]) * SECTOR
    disk.seek(inode_start + (sb[7] - 1) * sb[8])
    _, root_size, root_data, _ = struct.unpack("<4I", disk.read(16))
    disk.seek(base + root_data * SECTOR)
    directory = disk.read(root_size)
    found = set()
    for offset in range(0, len(directory), sb[11]):
        inode = struct.unpack_from("<I", directory, offset)[0]
        raw_name = directory[offset + 4:offset + 16].split(b"\0", 1)[0]
        if not inode or raw_name not in {name.encode("ascii") for name in files}:
            continue
        name = raw_name.decode("ascii")
        inode_offset = inode_start + (inode - 1) * sb[8]
        disk.seek(inode_offset)
        _, _, data_sector, capacity = struct.unpack("<4I", disk.read(16))
        data = files[name]
        if len(data) > capacity * SECTOR:
            raise ValueError(f"{name} exceeds its reserved disk space")
        disk.seek(base + data_sector * SECTOR)
        disk.write(data)
        disk.seek(inode_offset + sb[9])
        disk.write(struct.pack("<I", len(data)))
        found.add(name)
    if found != files.keys():
        raise ValueError(f"IDE template is missing boot files: {files.keys() - found}")


def check_kernel(kernel):
    if len(kernel) < 52 or kernel[:6] != b"\x7fELF\x01\x01":
        raise ValueError("kernel.bin must be a little-endian ELF32 executable")
    phoff = struct.unpack_from("<I", kernel, 28)[0]
    phsize, phnum = struct.unpack_from("<HH", kernel, 42)
    if phsize != 32 or not phnum or phoff + phsize * phnum > len(kernel):
        raise ValueError("invalid ELF program headers")
    for index in range(phnum):
        kind, offset, addr, _, filesz, memsz, _, _ = struct.unpack_from(
            "<8I", kernel, phoff + index * phsize)
        if kind == 1 and (offset + filesz > len(kernel) or filesz > memsz or
                          addr + memsz > 0x70000):
            raise ValueError("kernel segments overlap the loader's ELF at 0x70000")


def build(output):
    if output in (OS_DIR, OS_DIR / "isocontent"):
        raise ValueError("use a separate output directory, not the historical images")
    load_inc = OS_DIR / "boot/include/load.inc"
    config = OS_DIR / "include/sys/config.h"
    root_sector = constant(load_inc, "ROOT_BASE")
    install_sector = constant(config, "INSTALL_START_SECT")
    install_size = constant(config, "INSTALL_NR_SECTS") * SECTOR
    kernel = (OS_DIR / "kernel.bin").read_bytes()
    check_kernel(kernel)
    # The loader stages the ELF at 0x70000, below itself at 0x90000.
    if len(kernel) > 0x20000:
        raise ValueError("kernel.bin exceeds the loader's 128 KB staging area")
    files = {"kernel.bin": kernel}
    for name in ("echo", "pwd", "demo", "desktop"):
        files[name] = (OS_DIR / "command" / name).read_bytes()
    files["hdldr.bin"] = (OS_DIR / "boot/hdldr.bin").read_bytes()
    # A plain text file so the desktop file explorer / cat have content.
    files["readme.txt"] = (OS_DIR / "command" / "readme.txt").read_bytes()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(data))
    archive = archive.getvalue()
    if len(archive) > install_size:
        raise ValueError("commands exceed the reserved install area")
    floppy = floppy_image((OS_DIR / "boot/boot.bin").read_bytes(), [
        (b"LOADER  BIN", (OS_DIR / "boot/loader.bin").read_bytes()),
        (b"KERNEL  BIN", kernel),
    ])

    output.mkdir(parents=True, exist_ok=True)
    hd_path = output / "100m.img"
    # Stage a complete image before replacing the output; never modify the
    # historical input images or leave a half-installed kernel on failure.
    with ExitStack() as stack:
        temporary = stack.enter_context(tempfile.TemporaryDirectory(prefix=".desktop-", dir=output))
        staged = Path(temporary) / "100m.img"
        if hd_path.exists():
            source = stack.enter_context(hd_path.open("r+b"))
            # QEMU uses POSIX/OFD byte-range locks too. Refuse an in-use image
            # and keep the lock until the replacement has been installed.
            fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with staged.open("wb") as target:
                shutil.copyfileobj(source, target)
        else:
            with gzip.open(OS_DIR / "100m.img.gz", "rb") as source:
                with staged.open("wb") as target:
                    shutil.copyfileobj(source, target)
        with staged.open("r+b") as disk:
            replace_boot_files(disk, root_sector,
                               {name: files[name] for name in ("kernel.bin", "hdldr.bin")})
            disk.seek(0, io.SEEK_END)
            if (root_sector + install_sector) * SECTOR + install_size > disk.tell():
                raise ValueError("IDE image is too small for the command archive")
            boot = (OS_DIR / "boot/hdboot.bin").read_bytes()
            disk.seek(0)
            disk.write(boot[:446])  # keep the template's partition table
            disk.seek(510)
            disk.write(boot[510:512])
            disk.seek((root_sector + install_sector) * SECTOR)
            disk.write(archive)    # reset the consumed cmd.tar header for Init()
        staged_floppy = Path(temporary) / "a.img"
        staged_floppy.write_bytes(floppy)
        staged.replace(hd_path)
        staged_floppy.replace(output / "a.img")
    print(f"Built {output / 'a.img'} and {hd_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OS_DIR / "build/desktop")
    args = parser.parse_args()
    try:
        build(args.output.resolve())
    except (OSError, ValueError) as error:
        parser.exit(1, f"desktop image: {error}\n")
