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
| **Scheduler** | `24` | Preemptive weighted round-robin scheduler (50 Hz timer) + kernel threads; kernel task is idle. |

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
TASKS   list running tasks (alias PS); shows an id, name, state, ticks and weight (W)
PS      list running tasks
RUN     spawn a demo task with an optional weight: RUN <name> [weight]
KILL    ask a task to exit: KILL <id>
YIELD   voluntarily reschedule the CPU
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

## 🧵 Multitasking

`noxis` now has a **preemptive, weighted round-robin scheduler** (`cpu/task.h` /
`cpu/task.c` + the timer ISR). It runs entirely in ring 0 — tasks are "kernel
threads", not separate user programs — but it is genuinely preemptive: the PIT
timer fires at **50 Hz** and every tick calls `scheduler_tick()`, which saves
the current task's CPU context (a `registers_t` frame) into its Process Control
Block and resumes the next `READY` task. No task can hog the CPU for more than
one 20 ms timeslice, and a task with weight `w` gets up to `w` consecutive
slices before the scheduler rotates, so long-term CPU is divided proportionally
to weight. The kernel/idle task (slot 0) is never weighted.

Key facts for newcomers:

- **Task 0 is the kernel / idle task.** It runs the interactive shell and soaks
  up the CPU whenever nothing else is runnable.
- **Up to `MAX_TASKS` (32) tasks**, including slot 0. Each new task gets its own
  4 KB page-aligned kernel stack from `kmalloc`.
- **Cooperative exit.** `task_kill()` only sets a flag; the task must poll
  `task_should_exit()` and return on its own. There is no forced teardown.
- **`task_yield()`** hands the CPU over voluntarily (via software interrupt
  `0x81`), and **`task_sleep(ms)`** blocks a task for ~ms by setting a wake tick.

To try it: type `TASKS` (or `PS`) to see what's running (with weights in the
`W` column), `RUN clock` to spawn a demo task that prints the time, `RUN spin 5`
to spawn the busy task with a high weight, and `KILL <id>` to ask a task to exit.
If you prefer the C API, drive it through `task.h`: `task_init()`,
`task_create_prio(name, entry, weight)`, `task_kill(id)`, `scheduler_tick(regs)`,
`task_yield()`, `task_sleep(ms)`, `task_should_exit()`, `task_current_id()`,
`task_count()`, and `task_get(id)`.

---

## 🛠️ Ideas to extend noxis

- Command history with the arrow keys (keyboard driver work)
- A real `free()` for the heap (the bump allocator never reclaims)
- A simple filesystem on the floppy
- User mode + a second privilege ring
- User-mode tasks + privilege rings
- Per-task CPU accounting / `TOP`-style view over `TASKS`

Have fun — and remember, the only way to be sure you understood it is to break it.
