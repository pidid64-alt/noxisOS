# noxis Multitasking Core — Design Spec

> Part 1 of making multitasking a **global subsystem** of the kernel.
> Scope: (a) polish + commit the already-written scheduler, and
> (b) add a **weighted round-robin** scheduler policy with per-task weights.
> Out of scope for this spec: a full-screen task monitor and user-mode (ring-3)
> tasks. See "Roadmap" at the end for the FAT12 filesystem + minimal desktop
> that follow after this lands.

## Context

`noxis` already has a preemptive round-robin scheduler for *kernel threads*
(ring 0 only), driven by the PIT at 50 Hz. The implementation is present in
the working tree but **not committed**:

- `23-fixes/cpu/task.c`, `task.h` — scheduler + `task_init`, `task_create`,
  `task_kill`, `scheduler_tick`, `task_yield`, `task_sleep`.
- `23-fixes/drivers/tasks_demo.c`, `tasks_demo.h` — five demo threads
  (`blink`, `count`, `clock`, `ping`, `spin`).
- `23-fixes/drivers/shell.c` — commands `TASKS`/`PS`/`RUN`/`KILL`/`YIELD`
  already wired; `kernel.c` already calls `task_init()`.

However `README.md` still says **"Multitasking (coming soon)"** and
`docs/scheduler-design.md` lists the policy as round-robin only. This spec
closes that gap and adds **weighted** scheduling on top of the existing
infrastructure. The kernel task (slot 0) is the interactive shell and is
treated as an *idle* task: it runs whenever no weighted task is runnable.

### Existing scheduler facts (must stay true)

- `scheduler_tick(registers_t *r)` is called from the timer ISR (`cpu/timer.c`)
  and from the voluntary-yield software interrupt `0x81` (`cpu/task.c`).
- A task's context is a `registers_t` frame, identical to the ISR-pushed frame;
  saving/restoring is a straight struct copy. First schedule builds an
  `iret`-shaped frame at the top of a `kmalloc(... 1 ...)` page-aligned 4 KB
  stack.
- `TASK_SLEEPING` tasks wake when `tick >= wake_tick`.
- `MAX_TASKS = 32`, `TASK_STACK_SIZE = 4096`.

## Design

### 1. Weighted round-robin (policy A: kernel outside the weighted model)

Each weighted task gets a static `weight` (1..255). A task with weight `w`
receives up to `w` consecutive 20 ms time slices before the scheduler rotates
to the next runnable weighted task. Long-term CPU is divided proportionally to
weights. The kernel/idle task (slot 0) is **not** part of the weighted pool;
it is the fallback and only runs when no weighted task is READY.

#### PCB changes (`task.h`)
```c
typedef struct {
    /* ... existing fields ... */
    uint8_t weight;        /* static weight, 1..255 (default 1) */
    uint8_t quantum_left;  /* consecutive slices left in the current run */
    uint32_t stack_base;   /* for stack recycling on reuse (see §3) */
} task_t;
```

#### `scheduler_tick` logic (replaces current round-robin body)
1. Save `*r` into `tasks[current_task].regs`.
2. If `current_task` is a weighted task (`id != 0`) and `quantum_left > 0`:
   - give it one more slice: `quantum_left--`, `ticks++`, return (`*r` unchanged).
3. Else round-robin search among slots `1..MAX_TASKS-1` for the next READY task
   (wake any SLEEPING task whose `tick >= wake_tick`, exactly as today).
4. If found: mark RUNNING, `ticks++`, `quantum_left = weight - 1`,
   `current_task = idx`, `*r = tasks[idx].regs`.
5. If none found: keep the kernel/idle task (slot 0) RUNNING.

#### API
```c
/* New: spawn a task with an explicit weight. Returns id, or -1 if full. */
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);

/* Existing: kept as a thin wrapper, weight = 1 (no API break). */
int task_create(const char *name, void (*entry)(void));
```
`weight` is clamped to `1..255` inside `task_create_prio` (0 → 1).

### 2. Shell interface

- `RUN <name> [weight]` — parse an optional integer weight after the name;
  clamp to `1..255`, default `1`. Pass to `task_create_prio`.
  Example: `RUN spin 3`.
- `TASKS` / `PS` gain a `W` (weight) column; the existing `T` (ticks) column
  stays. Output uses fixed columns so the 80x25 VGA display stays stable.
- `DEMO_TASKS[]` gains a default `weight` per demo so the effect is visible
  immediately: `spin = 3`, all others `= 1`. Running `RUN spin` therefore
  visibly outpaces other tasks.

### 3. Polish: stop the stack leak on KILL + RUN

Today `task_create` `kmalloc`s a fresh 4 KB stack every spawn; after
`KILL <id>` + `RUN <name>` the bump heap grows without bound (no `free` exists).
Fix: store `stack_base` in the PCB. When `task_create_prio` finds a DEAD slot,
**reuse** its existing `stack_base` instead of allocating a new one. Net effect:
the stack pool is bounded at `MAX_TASKS * 4096` bytes and heap usage from
task stacks stabilizes. (Full `free` of stacks is still impossible given the
bump allocator — this is documented, not fixed.)

### 4. Global kernel state (`kernel_state_t`)

Introduce a single global `kernel_state_t` (declared in `kernel/kernel.h`,
defined in `kernel/kernel.c`) so OS-wide state has one home:
```c
typedef enum { SCHED_RR = 0, SCHED_WEIGHTED } sched_policy_t;
typedef struct {
    const char *version;
    uint8_t booted;          /* set to 1 at end of kernel_main */
    sched_policy_t sched;    /* currently SCHED_WEIGHTED */
    task_t *tasks;           /* points at the scheduler's table */
} kernel_state_t;
extern kernel_state_t g_kernel;
```
`kernel_main` initializes `g_kernel` after `task_init()`; the scheduler API or
shell can read `g_kernel.sched` to report the active policy. This is the
"global" layer the request asked for.

### 5. Robustness

- `print_tasks` must render correctly with only the kernel task present (idle
  line shows `W=1`/kernel, no crash on empty weighted list).
- `cmd_run`/`cmd_kill` already guard bad ids; keep that. `task_kill` keeps
  refusing slot 0.

### 6. Documentation

- `docs/scheduler-design.md`: replace "round-robin only" with the weighted
  model, document `weight`/`quantum_left`, `task_create_prio`, kernel-as-idle,
  and remove the "no priorities" limitation (keep the other real limitations:
  ring 0 only, bump-heap stack pool, fixed `MAX_TASKS`).
- `README.md`: drop the "Multitasking (coming soon)" note; document `RUN <name>
  [weight]`, the `W` column, and the weighted policy.

## Files to modify (all under `23-fixes/`, visible via symlinks from `24-el-capitan/`)

| File | Change |
|------|--------|
| `cpu/task.h` | `weight`, `quantum_left`, `stack_base` in `task_t`; `task_create_prio` (no `kernel_state_t` here — it lives in `kernel/kernel.h`) |
| `cpu/task.c` | Weighted `scheduler_tick`; `task_create_prio` + stack reuse; seed `quantum_left` |
| `kernel/kernel.h` / `kernel.c` | Declare/define `g_kernel` `kernel_state_t`; init in `kernel_main` |
| `drivers/shell.c` | `cmd_run` parse optional weight; `print_tasks` add `W` column |
| `drivers/tasks_demo.h` / `.c` | `weight` field in `demo_task_t`; defaults (`spin=3`) |
| `docs/scheduler-design.md`, `README.md` | Update |

## Verification (end-to-end)

Build and run (`23-fixes/` or `24-el-capitan/`):
```bash
make
make run          # QEMU i386
```
Manual checks in the OS:
1. `HELP` lists `TASKS`, `PS`, `RUN`, `KILL`, `YIELD`.
2. `RUN blink` then `RUN spin` → both run; the `spin` counter advances faster
   than one would expect under pure RR because its weight (3) grants more slices.
3. `RUN spin 1` vs `RUN spin 3` → weight 3 visibly updates more often.
4. `TASKS` → new `W` column shows the weights.
5. `KILL <id>` removes the task; repeat `RUN spin` / `KILL <id>` several times
   and watch `MEM` — stack allocations stabilize (no unbounded growth), proving
   the stack-reuse fix in §3.
6. `END` halts the CPU as before.

## Roadmap (separate specs, after this lands)

- **FAT12 filesystem** + runtime disk read/write (a design outline already
  exists at `docs/superpowers/specs/2026-08-14-fat12-disk-design.md`).
- **Minimal desktop / windowing shell** on top of the filesystem and scheduler.
  These are intentionally *not* part of this spec.
