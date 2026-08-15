# noxis

> `noxis` — a tiny 32-bit operating system built from scratch, boot sector to
> interactive shell. Born from the [os-tutorial](https://github.com/cfenollosa/os-tutorial)
> lessons and extended with a real command shell, a CMOS clock, and a fresh coat of paint.

```
        _ __   _____   __
       | '_ \ / _ \ \ / /
       | | | | (_) \ V /
       |_| |_|\___/ \_/    32-bit · freestanding · no GRUB
```

`noxis` boots off a raw floppy image, switches the CPU from 16-bit real mode
into 32-bit protected mode, installs an interrupt table, talks to the VGA
text buffer and the keyboard controller, and drops you into a small command
shell. It is **not** a production OS — it's a learning artifact and a playground
for bare-metal programming on x86.

---

## ✨ What's inside

| Subsystem | Built in | What it does |
|-----------|----------|--------------|
| Boot sector | `00`–`07` | 16-bit real-mode bootstrap that loads the kernel off the floppy. |
| Protected mode | `08`–`10` | GDT + far jump into 32-bit mode. |
| VGA driver | `15`–`17` | Color text output, scrolling, cursor control. |
| Interrupts | `18`–`20` | IDT, ISRs, IRQs, PIC remap, PIT timer. |
| Keyboard | `21` | Scancode → ASCII, line buffering, backspace. |
| Heap | `22` | `kmalloc` bump allocator. |
| **Shell** | `24` | Command table + dispatch: `HELP`, `CLEAR`, `ECHO`, `VERSION`, `TIME`, `UPTIME`, `MEM`, `END`, `PAGE`. |
| **RTC** | `24` | CMOS real-time clock driver for the `TIME` command. |

### Shell commands

```
HELP    list available commands
CLEAR   clear the screen
ECHO    print its arguments back        e.g.  ECHO hello noxis
VERSION show the noxis version banner
TIME    show current date and time       e.g.  2026-08-14 13:07:09
UPTIME  show time since boot (PIT ticks)
MEM     show kernel heap usage (allocated/free bytes)
END     halt the CPU
PAGE    test kmalloc() and print an address
```

---

## 🚀 Quick start

You need: `gcc` (or the `i686-elf` cross-compiler), `nasm`, `make`, and `qemu-system-i386`.

```bash
# Build the floppy image (os-image.bin)
make

# Run it in QEMU
make run
```

Type `HELP` at the prompt and explore. `make debug` launches QEMU with a GDB
stub (`localhost:1234`) if you want to step through the kernel with symbols.

### Building with the cross-compiler

If your system `gcc` defaults to 64-bit, you can build a dedicated
`i686-elf` toolchain with the helper script:

```bash
./build-i686-elf.sh        # builds binutils + gcc into ~/opt/cross
export PATH="$HOME/opt/cross/bin:$PATH"
make CC=i686-elf-gcc LD=i686-elf-ld
```

The included `Makefile` is preconfigured to build **freestanding 32-bit**
code straight from a host `gcc` (tested on Arch/CachyOS with `lib32-glibc`),
so the cross-compiler is optional.

---

## 🧭 Project layout

The tree follows the tutorial's numbered lessons. The live kernel code lives
under `23-fixes/` (symlinked into `24-el-capitan/`), and the interactive shell
+ RTC that `noxis` adds live in:

```
23-fixes/
├── boot/        boot sector + kernel entry stub
├── cpu/         GDT, IDT, ISRs, IRQs, PIT timer, port I/O
├── drivers/     screen.c  keyboard.c  shell.c  rtc.c   <- noxis additions
├── kernel/      kernel.c (entry + shell wiring)
└── libc/        string.c  mem.c  (int_to_ascii, kmalloc, ...)
```

The new files for `noxis` are `drivers/shell.c`, `drivers/shell.h`,
`drivers/rtc.c`, and `drivers/rtc.h`.

---

## ⚠️ A note on the source material

The original os-tutorial is an **old, abandoned project** with known technical
and design issues. It's a fantastic *starting point* for understanding how
computers boot, but if you want to seriously study OS design, pair it with
modern resources like the [OSDev wiki](https://wiki.osdev.org/) and
[The Little Book About OS Development](https://littleosbook.github.io/).

`noxis` inherits that spirit: minimal, heavily commented, and meant to be
broken and rebuilt.

---

## 🛠️ Ideas to extend noxis

- Command history with the arrow keys (keyboard driver work)
- A real `free()` for the heap
- A simple filesystem on the floppy
- User mode + a second privilege ring

Have fun — and remember, the only way to be sure you understood it is to break it.
