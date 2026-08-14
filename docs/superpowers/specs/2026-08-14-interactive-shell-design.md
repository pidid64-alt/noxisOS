# Interactive Shell for the os-tutorial Kernel

**Date:** 2026-08-14
**Stage:** 24-el-capitan (final build-lesson folder; code lives in `23-fixes`)

## Goal

Replace the two-magic-word handler in `kernel.c` with a real interactive
shell driven by a command table, and add a small CMOS real-time-clock (RTC)
driver so the `TIME` command can report the live date/time. Keep existing
behavior (`END` halts, `PAGE` tests `kmalloc`) working.

## Current state (context)

The kernel is a 32-bit freestanding binary (entry at `0x1000`) with:

- VGA text driver (`drivers/screen.c`, `kprint*`, `clear_screen`)
- Keyboard driver (`drivers/keyboard.c`) that buffers scancode ASCII and, on
  `ENTER`, calls `user_input(key_buffer)` then resets the buffer
- ISR/IRQ infra (`cpu/isr.c`, `cpu/idt.c`, `cpu/interrupt.asm`) with PIC remap
- Timer (`cpu/timer.c`)
- Bump-pointer allocator (`libc/mem.c`, `kmalloc`)
- libc helpers: `int_to_ascii`, `hex_to_ascii`, `strlen`, `strcmp`, `append`,
  `backspace` (`libc/string.c`)

`kernel.c::user_input` currently only recognizes `END` and `PAGE`.

The build (`24-el-capitan/Makefile`) globs `drivers/*.c`, `kernel/*.c`, etc.,
so new `.c`/`.h` files are picked up automatically — **no Makefile change**.

## Approach

Approach A (chosen): a **command table** in a new `drivers/shell.c`, with the
RTC as a separate independent driver module. `user_input` becomes a thin
wrapper delegating to the shell. Matches existing table-driven conventions in
the codebase (`sc_ascii[]`, `interrupt_handlers[256]`).

## 1. Files

| File | Role |
|------|------|
| `drivers/shell.c` + `shell.h` | Command table + dispatch. Exposes `void shell_run(char *input)`. |
| `drivers/rtc.c` + `rtc.h` | CMOS RTC driver for the `TIME` command. |
| `kernel/kernel.c` | `user_input()` calls `shell_run(input)` then prints the `> ` prompt. |

`Makefile` is **unchanged** (glob already covers `drivers/*.c`).

## 2. Shell design (`drivers/shell.c`)

Static table drives dispatch:

```c
typedef void (*shell_cmd_t)(char *args);
typedef struct { const char *name; shell_cmd_t handler; } shell_command_t;

static shell_command_t COMMANDS[] = {
  {"HELP",    cmd_help},
  {"CLEAR",   cmd_clear},
  {"ECHO",    cmd_echo},
  {"VERSION", cmd_version},
  {"TIME",    cmd_time},
  {"END",     cmd_end},    // halt CPU (unchanged behavior)
  {"PAGE",    cmd_page},   // kmalloc test (unchanged behavior)
};
```

`shell_run(char *input)`:
- Trim leading whitespace, then split on the first space into `name` + `args`
  (the remainder is passed to handlers that need it, e.g. `ECHO`).
- Linear scan over `COMMANDS`; on a name match, call `handler(args)` and return.
- If no match, print `"Unknown command: <name>"`.

Handlers (each small/isolated):
- `cmd_help` — prints the command list with one-line descriptions.
- `cmd_clear` — calls existing `clear_screen()` from `screen.h`.
- `cmd_echo` — prints `args` verbatim (empty → just newline).
- `cmd_version` — prints a banner string, e.g.
  `"el-capitan OS v1.0 (cfenollosa tutorial)\n"`.
- `cmd_end` — port the existing `END` behavior (print message, `hlt`).
- `cmd_page` — port the existing `PAGE` behavior (call `kmalloc(1000,1,...)` and
  print virtual + physical address via `hex_to_ascii`).

Matching is **case-sensitive uppercase**, consistent with the existing
`strcmp` style and the keyboard's `sc_ascii` table (which only emits uppercase
letters). No uppercase-conversion needed.

`kernel.c::user_input` reduces to:
```c
void user_input(char *input) {
    shell_run(input);
    kprint("\n> ");
}
```
(The newline + `> ` prompt replace the old per-branch `kprint("\n> ")`.)

## 3. RTC driver (`drivers/rtc.c`)

CMOS RTC via standard ports (reusing `port_byte_in/out` from `cpu/ports.h`):
- Address port `0x70`, data port `0x71`.
- Read BCD registers: seconds `0x00`, minutes `0x02`, hours `0x04`,
  day `0x07`, month `0x08`, year `0x09`.
- Update-race guard: read `seconds` twice in a loop until two consecutive
  reads agree before sampling the other fields (standard CMOS "wait for update"
  pattern), preventing torn reads across a tick boundary.
- BCD→binary conversion per byte: `(val >> 4) * 10 + (val & 0xF)`.
- Assume 24-hour mode (QEMU / typical BIOS default); no 12-hour handling.

`rtc.h` API:
```c
void rtc_get_time(uint8_t *yr, uint8_t *mo, uint8_t *day,
                  uint8_t *h, uint8_t *m, uint8_t *s);
void rtc_print_time(void);   // formats YYYY-MM-DD HH:MM:SS via int_to_ascii
```
`cmd_time` calls `rtc_print_time()`. The raw getter is kept separate for
clarity/testability.

Behavioral note: the reported date/time is the VM's RTC (host clock), not a
hard-coded constant — correct expected behavior.

## 4. Error handling / edge cases

- Unknown command → informative `Unknown command:` message, no halt.
- `ECHO` with no args → prints a blank line.
- `HELP` lists every command including `END` and `PAGE` so the user learns the
  full surface.
- Empty input (user presses Enter on a blank line) → `shell_run` with empty
  string: no match → `Unknown command: ` (acceptable; could later be a no-op).

## 5. Testing / verification

- Build: `make` inside `24-el-capitan` produces `os-image.bin` with no warnings
  (existing `CFLAGS` use `-Wall -Wextra`; keep clean).
- Run under QEMU: `make run` (qemu-system-i386 -fda os-image.bin) and exercise:
  - type `HELP` → command list
  - type `CLEAR` → screen clears, prompt returns
  - type `ECHO hello` → `hello`
  - type `VERSION` → banner
  - type `TIME` → `YYYY-MM-DD HH:MM:SS`
  - type `PAGE` → malloc address line (unchanged)
  - type `END` → halts (unchanged)
  - type `FOO` → `Unknown command: FOO`
- Optionally `make debug` to load symbols in gdb if something misbehaves.

## Out of scope (YAGNI)

- No arrow keys / command history (separate keyboard-driver work).
- No real heap `free` (bump allocator unchanged).
- No persistence, filesystem, or process model.
