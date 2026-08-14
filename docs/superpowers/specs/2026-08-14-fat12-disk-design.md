# noxis — Runtime FAT12 Disk Read/Write (Design)

**Date:** 2026-08-14
**Status:** Approved (design)

## Goal

Give the noxis 32-bit kernel the ability to read and write files on a FAT12
floppy disk **at runtime**, from the interactive shell — extending the OS
beyond "boot once and talk to the screen." This reuses the existing FAT12
volume the stage-2 bootloader already reads the kernel from, but performs all
disk I/O from protected mode via a BIOS real-mode thunk.

## Non-goals (YAGNI)

- No journaling / crash-safety. A mid-write power loss may corrupt the volume;
  acceptable for a learning OS.
- No long filenames (only 8.3), no subdirectories, no FAT16/32.
- No protected-mode FDC driver (we use the BIOS `int 13h` via a mode switch).
- No filesystem unification with the boot floppy; the data volume is a separate
  floppy image mounted as the second drive (`-fdb`).

## Architecture & data flow

```
shell.c (LS / CAT <name> / WRITE <name> <text...>)
        │  (C, 32-bit protected mode)
        ▼
drivers/disk.h  ->  drivers/fs_fat12.c   (FAT12 layout + helpers)
        │
        ▼
cpu/realmode_disk.c  ->  calls the asm thunk
        │
        ▼
cpu/realmode_disk.asm  (saves PMODE regs, switches to 16-bit RM,
                         sets int 13h registers, calls int 0x13,
                         switches back to PMODE, restores regs)
        ▼
BIOS int 13h  (reads/writes raw floppy sectors)
```

- **Read:** `fs_open(name)` locates a root-directory entry → walk the FAT12
  cluster chain → `disk_read` each data cluster → copy into a PMODE buffer.
- **Write:** `fs_write(name, data, len)` finds free clusters (scan FAT), builds
  a cluster chain, writes a root-directory entry (8.3 name, size, first
  cluster), writes the data clusters, then flushes the modified FAT sectors
  and root-directory sector back to the disk.

## New files / components

| File | Responsibility |
|------|----------------|
| `cpu/realmode_disk.h` | C prototypes: `disk_read(u32 lba, u8 count, void *pm_buf)`, `disk_write(u32 lba, u8 count, void *pm_buf)`, status codes. |
| `cpu/realmode_disk.asm` | The PMODE↔RM switch; sets `int 13h` register frame, copies data through a low-memory scratch buffer, returns status. |
| `drivers/disk.h` | Thin public API used by the shell (sector-level read/write + FS entry points). |
| `drivers/fs_fat12.h` | FAT12 on-disk structures, `fs_` API, status codes. |
| `drivers/fs_fat12.c` | `fs_init()`, `fs_open`, `fs_read`, `fs_list_root`, `fs_write`, 8.3 name conversion. |

**Bootloader change:** `boot/disk.asm` (stage-2 loader) must be told to NOT
overwrite the reserved low-memory region where the thunk + scratch buffer live
(see Memory Layout). No change to the boot-sector detection logic itself.

## Memory layout constraint (the key gotcha)

- Kernel loads at `0x1000`. Low memory below it holds the boot sector,
  stage-2 loader, and IDT.
- Reserve a fixed **thunk + scratch region at `0x9000`**: 1 KB for the thunk
  code and **15 KB** for the data scratch buffer (enough for ~30 clusters of
  512 B). Total region `0x9000`–`0xB000`.
- `kernel.bin` and the stage-2 loader load path skip `0x9000`–`0xB000`.
- VGA text buffer is `0xB8000`, so there is no conflict.

## FAT12 details

- **Geometry:** standard 1.44 MB floppy — 512 B/sector, 2880 sectors, 2
  heads, 18 sectors/track. FAT12 boot sector fields (`bpb_*` in the existing
  boot code) give the root dir start LBA and size; reuse those values in
  `fs_init()` rather than hard-coding.
- **Read:** root directory is a fixed sector span; scan entries for the 8.3
  name match; follow the 12-bit FAT cluster chain until `0xFFF` (end-of-chain).
- **Write:**
  1. Convert the requested name to uppercase 8.3 (`hello.txt` → `HELLO.TXT`).
  2. If the file exists, reuse its cluster chain; otherwise allocate free
     clusters by scanning the FAT for `0x000` entries (contiguity not required
     — the chain is followed).
  3. Write the data clusters via `disk_write`.
  4. Write/update the root-directory entry (name, size, first cluster).
  5. Write back the modified FAT sectors and the root-directory sector.
- **8.3 limit** is enforced; the shell maps user input to 8.3 before calling
  the FS layer.

## Shell commands

- `LS` — list root-directory entries: `8.3 NAME` and size.
- `CAT <name>` — read and print the file's contents (treated as text).
- `WRITE <name> <text...>` — create or overwrite a file containing the given
  text, e.g. `WRITE NOTES.TXT hello noxis` writes `hello noxis`.
- Existing commands (`HELP`, `CLEAR`, `ECHO`, `VERSION`, `TIME`, `END`,
  `PAGE`, `UPTIME`) are unchanged.

## Build step (Makefile)

Add a reproducible `disk.img` target:

1. `dd` a blank 1.44 MB image, format as FAT12 (`mkfs.fat -F 12` if present,
   otherwise a hand-rolled FAT12 header).
2. Copy sample text files (`README.TXT`, `HELLO.TXT`) onto it via `mcopy`
   (preferred, no root) or `mount -o loop` as a fallback.
3. Keep `os-image.bin` = `bootsect.bin + kernel.bin` as the **boot floppy**.
4. Run qemu with **two** floppies: `-fda os-image.bin -fdb floppy.img`. The
   data volume is the second drive; `LS`/`CAT`/`WRITE` operate on it.

This is fully reproducible from the repo — no hand-supplied `.img`.

## Error handling

- Disk ops return status: `DISK_OK`, `DISK_ERR`, `FS_NOT_FOUND`,
  `FS_NO_SPACE`. The shell prints a clear message instead of hanging.
- Buffer overruns are guarded by comparing file size against the 15 KB scratch
  buffer; `WRITE` of >15 KB is refused with `FS_NO_SPACE`.

## Verification

- `make` builds clean with `-Wall -Wextra` (no warnings).
- Boot under qemu with both floppies:
  - `LS` shows the sample files (`README.TXT`, `HELLO.TXT`).
  - `CAT HELLO.TXT` prints its contents.
  - `WRITE NOTES.TXT hi` then `LS` shows `NOTES.TXT` with the correct size.
  - `CAT NOTES.TXT` returns `hi`.
  - Re-run qemu (without rebuilding `floppy.img`) and `CAT NOTES.TXT` still
    returns `hi` — confirming the write **persisted** to the data volume.
- Because the VGA text buffer (`0xB8000`) is MMIO and cannot be `pmemsave`d,
  verification drives the keyboard via the qemu monitor and reads results back
  through the screen, or via a debug/serial path added only if needed. The
  approach used for the earlier `UPTIME` check (monitor `sendkey` + screen
  inspection) applies here.

## Open risks

- The PMODE↔RM switch must reload a 16-bit code segment and a real-mode data
  segment added to the GDT; an incorrect segment setup will triple-fault. The
  thunk is the highest-risk module and should be tested first with a single
  `disk_read` of the boot sector before layering FAT12 on top.
- `mkfs.fat` / `mcopy` availability varies by host; the Makefile must detect
  them and fall back to a hand-rolled FAT12 image if missing.
