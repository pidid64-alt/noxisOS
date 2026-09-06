#!/usr/bin/env python3
"""Host checks for the rootless image builder; no actual disk images modified."""
import io
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from desktop_image import check_kernel, floppy_image, replace_boot_files


def boot_sector():
    boot = bytearray(512)
    struct.pack_into("<HBHBHH", boot, 11, 512, 1, 1, 2, 224, 2880)
    boot[21] = 0xF0
    struct.pack_into("<H", boot, 22, 9)
    boot[510:] = b"\x55\xaa"
    return boot


def filesystem():
    data = bytearray(40 * 512)
    # Root device begins at sector 1, inode table at absolute sector 5.
    struct.pack_into("<14I", data, 2 * 512,
                     0x111, 8, 39, 1, 1, 6, 1, 1, 32, 4, 8, 16, 0, 4)
    struct.pack_into("<4I", data, 5 * 512, 0x4000, 64, 10, 1)
    for index, (name, sector, payload) in enumerate([
            ("kernel.bin", 12, b"old kernel"),
            ("hdldr.bin", 14, b"old loader"),
            ("notes", 16, b"user notes must survive")], start=2):
        struct.pack_into("<4I", data, 5 * 512 + (index - 1) * 32,
                         0x8000, len(payload), sector, 2)
        data[(1 + sector) * 512:(1 + sector) * 512 + len(payload)] = payload
        struct.pack_into("<I12s", data, 11 * 512 + (index - 1) * 16,
                         index, name.encode())
    struct.pack_into("<I12s", data, 11 * 512, 1, b".")
    return io.BytesIO(data)


class ImageTests(unittest.TestCase):
    def test_fat12_chains_and_both_tables(self):
        files = [(b"LOADER  BIN", bytes(range(256)) * 5),
                 (b"KERNEL  BIN", b"kernel bytes" * 101)]
        image = floppy_image(boot_sector(), files)
        self.assertEqual(len(image), 1474560)
        fat = image[512:512 * 10]
        self.assertEqual(fat, image[512 * 10:512 * 19])
        self.assertEqual(fat[:3], b"\xf0\xff\xff")
        for index, (name, payload) in enumerate(files):
            entry = 19 * 512 + index * 32
            self.assertEqual(image[entry:entry + 11], name)
            cluster, size = struct.unpack_from("<HI", image, entry + 26)
            result = bytearray()
            visited = set()
            while cluster < 0xFF8:
                self.assertNotIn(cluster, visited)
                visited.add(cluster)
                offset = (33 + cluster - 2) * 512
                result.extend(image[offset:offset + 512])
                pair = struct.unpack_from("<H", fat, cluster * 3 // 2)[0]
                cluster = (pair >> 4) if cluster & 1 else (pair & 0xFFF)
            self.assertEqual(size, len(payload))
            self.assertEqual(result[:size], payload)

    def test_rejects_bad_boot_sector_and_overflow(self):
        with self.assertRaises(ValueError):
            floppy_image(bytes(512), [])
        with self.assertRaises(ValueError):
            floppy_image(boot_sector(), [(b"KERNEL  BIN", bytes(1474560))])

    def test_boot_files_updated_without_touching_user_files(self):
        disk = filesystem()
        original = disk.getvalue()
        files = {"kernel.bin": b"new kernel contents", "hdldr.bin": b"new loader"}
        replace_boot_files(disk, 1, files)
        for name, inode, sector in [("kernel.bin", 2, 12), ("hdldr.bin", 3, 14)]:
            disk.seek(5 * 512 + (inode - 1) * 32)
            _, size, start, capacity = struct.unpack("<4I", disk.read(16))
            self.assertEqual((size, start, capacity), (len(files[name]), sector, 2))
            disk.seek((1 + sector) * 512)
            self.assertEqual(disk.read(size), files[name])
        self.assertEqual(disk.getvalue()[17 * 512:19 * 512], original[17 * 512:19 * 512])
        self.assertEqual(disk.getvalue()[11 * 512:12 * 512], original[11 * 512:12 * 512])
        self.assertEqual(disk.getvalue()[:512], original[:512])

    def test_rejects_missing_or_oversized_boot_files(self):
        with self.assertRaises(ValueError):
            replace_boot_files(filesystem(), 1, {"missing": b"data"})
        with self.assertRaises(ValueError):
            replace_boot_files(filesystem(), 1, {"kernel.bin": bytes(1025)})

    def test_elf32_and_staging_memory_boundary(self):
        elf = bytearray(84)
        elf[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<I", elf, 28, 52)
        struct.pack_into("<HH", elf, 42, 32, 1)
        struct.pack_into("<8I", elf, 52, 1, 84, 0x1000, 0x1000, 0, 0x60000, 5, 4096)
        check_kernel(elf)
        struct.pack_into("<I", elf, 52 + 20, 0x70000)
        with self.assertRaises(ValueError):
            check_kernel(elf)
        elf[4] = 2
        with self.assertRaises(ValueError):
            check_kernel(elf)


if __name__ == "__main__":
    unittest.main()
