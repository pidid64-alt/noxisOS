# noxis Multitasking Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commit the already-written preemptive scheduler and add a weighted
round-robin policy (per-task weight, kernel task = idle) plus stack-reuse and
a global `kernel_state`, then document it — closing the "Multitasking (coming
soon)" gap.

**Architecture:** Weighted RR sits on the existing timer-driven `scheduler_tick`.
A `task_t` gains `weight` + `quantum_left`; the scheduler grants a weighted
task up to `weight` consecutive 20 ms slices before rotating. The kernel task
(slot 0, the shell) is the idle fallback and is never subject to weights.
`task_create_prio(name, entry, weight)` is the new entry point; `task_create`
stays as a weight-1 wrapper. Killed tasks' 4 KB stacks are recycled into the
same PCB slot to stop the bump-heap leak. A single `g_kernel` `kernel_state_t`
holds OS-wide state.

**Tech Stack:** freestanding 32-bit C (gcc `-m32 -ffreestanding`), NASM,
GNU ld, QEMU i386. No standard library, no test framework — "tests" are a
passing `make` plus manual QEMU verification by the user.

## Global Constraints

- Build with `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra
  -fno-exceptions -fno-pie -fno-stack-protector -fcommon -m32` (verbatim from
  `23-fixes/Makefile`).
- All edits are under `23-fixes/`; `24-el-capitan/` reaches them via symlinks
  (`cpu`, `drivers`, `kernel`, `libc`, `boot`). Build/run from `23-fixes/`.
- `MAX_TASKS = 32`, `TASK_STACK_SIZE = 4096` (do not change).
- `weight` is clamped to `1..255`; `0` means `1`.
- Kernel task is always slot 0 and is never weighted, never slept, never killed.
- `registers_t`/`task_t` frame-copy semantics must stay intact (existing design).
- No `free()` exists (bump heap); stack reuse, not freeing, is the leak fix.
- Commit frequently; every task ends with its own commit.

---

### Task 1: Extend the PCB and add `task_create_prio` declaration

**Files:**
- Modify: `23-fixes/cpu/task.h`

**Interfaces:**
- Consumes: `task_t` (existing), `task_state_t` (existing), `registers_t` (from `isr.h`).
- Produces: new `task_t` fields `weight` (`uint8_t`), `quantum_left` (`uint8_t`),
  `stack_base` (`uint32_t`); new prototype `int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);`.

- [ ] **Step 1: Add the three fields to `task_t`**

In `23-fixes/cpu/task.h`, inside the `task_t` struct add after `uint32_t stack_top;`:
```c
    uint8_t  weight;        /* static weight 1..255 (default 1) */
    uint8_t  quantum_left;  /* consecutive 20ms slices left in current run */
    uint32_t stack_base;    /* kmalloc'd stack base; reused on slot reclaim */
```

- [ ] **Step 2: Add the weighted-spawn prototype**

After the existing `int task_create(const char *name, void (*entry)(void));` declaration add:
```c
/* Spawn a task with an explicit static weight (clamped to 1..255; 0 -> 1).
 * Returns the task id, or -1 if the table is full. */
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);
```

- [ ] **Step 3: Commit**

```bash
cd 23-fixes && git add cpu/task.h && git commit -m "feat(sched): add weight/quantum_left/stack_base to task_t and task_create_prio prototype"
```

---

### Task 2: Weighted `scheduler_tick` + stack-reuse in `task.c`

**Files:**
- Modify: `23-fixes/cpu/task.c`

**Interfaces:**
- Consumes: `task_t.weight`, `task_t.quantum_left`, `task_t.stack_base` (Task 1),
  `kmalloc`, `memory_set`, `tick`, `KERNEL_CS`, `isr81`, `scheduler_tick` self.
- Produces: weighted rotation behavior; `task_create_prio` (real); `task_create`
  now a weight-1 wrapper.

- [ ] **Step 1: Replace `task_create` with `task_create_prio` + wrapper**

Delete the existing `int task_create(const char *name, void (*entry)(void)) { ... }`
body and replace with:
```c
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight) {
    int slot = -1, i;
    for (i = 1; i < MAX_TASKS; i++) {        /* never reuse slot 0 */
        if (tasks[i].state == TASK_DEAD) { slot = i; break; }
    }
    if (slot < 0) return -1;

    if (weight == 0) weight = 1;             /* clamp */
    /* 255 is fine; higher weights just mean more consecutive slices. */

    uint32_t stack;
    if (tasks[slot].stack_base != 0) {
        /* Recycle the previously allocated stack for this slot (leak fix). */
        stack = tasks[slot].stack_base;
    } else {
        uint32_t phys;
        stack = kmalloc(TASK_STACK_SIZE, 1, &phys); /* page-aligned */
        if (stack == 0) return -1;
        tasks[slot].stack_base = stack;       /* remember for reuse */
    }
    uint32_t stack_top = stack + TASK_STACK_SIZE;

    /* Synthetic iret frame at the top of the new stack (see existing design). */
    uint32_t *sp = (uint32_t *)stack_top;
    *--sp = 0x10;                       /* ss  (kernel data) */
    *--sp = stack_top - 20;             /* esp: post-iret stack pointer */
    *--sp = 0x202;                      /* eflags: interrupts enabled */
    *--sp = KERNEL_CS;                  /* cs  (kernel code) */
    *--sp = (uint32_t)task_trampoline;  /* eip */

    task_t *t = &tasks[slot];
    t->id = slot;
    set_name(t, name);
    t->state = TASK_READY;
    t->ticks = 0;
    t->kill_req = 0;
    t->wake_tick = 0;
    t->entry = entry;
    t->stack_top = stack_top;
    t->weight = weight;
    t->quantum_left = weight;           /* fresh full quantum on spawn */
    memory_set((uint8_t *)&t->regs, 0, sizeof(registers_t));
    t->regs.esp = (uint32_t)sp;
    t->regs.eip = (uint32_t)task_trampoline;

    return slot;
}

/* Backwards-compatible wrapper: weight 1. */
int task_create(const char *name, void (*entry)(void)) {
    return task_create_prio(name, entry, 1);
}
```

- [ ] **Step 2: Make `scheduler_tick` weighted**

Replace the existing `void scheduler_tick(registers_t *r) { ... }` body with:
```c
void scheduler_tick(registers_t *r) {
    /* 1. Save the interrupted task's context. */
    if (current_task >= 0 && current_task < MAX_TASKS) {
        tasks[current_task].regs = *r;
        if (tasks[current_task].state == TASK_RUNNING)
            tasks[current_task].state = TASK_READY;
    }

    /* 2. Weighted preemption: if the running weighted task still has slices
     *    left in its quantum, give it one more consecutive slice. The kernel
     *    task (slot 0) is NEVER weighted — it falls through to idle. */
    if (current_task > 0 && current_task < MAX_TASKS) {
        task_t *cur = &tasks[current_task];
        if (cur->state == TASK_READY && cur->quantum_left > 0) {
            cur->quantum_left--;
            cur->ticks++;
            cur->state = TASK_RUNNING;
            return; /* *r unchanged -> resume same task */
        }
    }

    /* 3. Round-robin search for the next READY weighted task (slots 1..N).
     *    Wake SLEEPING tasks whose wake_tick has arrived. */
    int next = -1, i;
    for (i = 1; i <= MAX_TASKS; i++) {
        int idx = (current_task + i) % MAX_TASKS;
        if (idx == 0) idx = (idx + 1) % MAX_TASKS; /* skip kernel slot */
        task_t *t = &tasks[idx];
        if (t->state == TASK_DEAD) continue;
        if (t->state == TASK_SLEEPING) {
            if (tick >= t->wake_tick) {
                t->state = TASK_READY;
            } else continue;
        }
        if (t->state == TASK_READY) { next = idx; break; }
    }

    if (next < 0) {
        /* Nothing else runnable: keep the kernel/idle task (slot 0) on CPU. */
        if (current_task == 0 && current_task < MAX_TASKS)
            tasks[0].state = TASK_RUNNING;
        return;
    }

    /* 4. Grant the next task a fresh quantum of `weight` slices. */
    tasks[next].state = TASK_RUNNING;
    tasks[next].ticks++;
    tasks[next].quantum_left = tasks[next].weight - 1;
    current_task = next;
    *r = tasks[next].regs;
}
```

- [ ] **Step 3: Build to verify it compiles**

```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: `os-image.bin` is produced with no errors (warnings about unused
`stack_base` before Task 4 are acceptable; there should be none).

- [ ] **Step 4: Commit**

```bash
cd 23-fixes && git add cpu/task.c && git commit -m "feat(sched): weighted RR tick + stack-reuse on slot reclaim"
```

---

### Task 3: Global `kernel_state_t` in `kernel.h` / `kernel.c`

**Files:**
- Modify: `23-fixes/kernel/kernel.h`
- Modify: `23-fixes/kernel/kernel.c`

**Interfaces:**
- Consumes: `task_t` (fwd decl), scheduler policy concept.
- Produces: `kernel_state_t`, `extern kernel_state_t g_kernel`, initialized in
  `kernel_main` after `task_init()`.

- [ ] **Step 1: Declare `kernel_state_t` in `kernel.h`**

Replace the whole content of `23-fixes/kernel/kernel.h` with:
```c
#ifndef KERNEL_H
#define KERNEL_H

#include "../cpu/task.h"

typedef enum { SCHED_RR = 0, SCHED_WEIGHTED } sched_policy_t;

typedef struct {
    const char    *version;     /* OS version string */
    uint8_t        booted;      /* 1 once kernel_main finishes setup */
    sched_policy_t sched;       /* active scheduler policy */
    task_t        *tasks;       /* pointer to the scheduler's task table */
} kernel_state_t;

extern kernel_state_t g_kernel;

void user_input(char *input);

#endif
```

- [ ] **Step 2: Define and initialize `g_kernel` in `kernel.c`**

Replace the whole content of `23-fixes/kernel/kernel.c` with:
```c
#include "../cpu/isr.h"
#include "../cpu/task.h"
#include "../drivers/screen.h"
#include "../drivers/shell.h"
#include "kernel.h"
#include <stdint.h>

kernel_state_t g_kernel;

void kernel_main() {
    isr_install();
    irq_install();
    task_init();   /* bring up the preemptive scheduler (kernel task = slot 0) */

    /* Publish global OS state. */
    g_kernel.version = "noxis 24-el-capitan";
    g_kernel.booted  = 1;
    g_kernel.sched   = SCHED_WEIGHTED;
    g_kernel.tasks   = 0; /* reserved; see scheduler registry if added later */

    asm("int $2");
    asm("int $3");

    kprint("Welcome to noxis\n"
        "Type HELP for a list of commands\n> ");
}

void user_input(char *input) {
    shell_run(input);
    kprint("\n> ");
}
```

- [ ] **Step 3: Build**

```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success, `os-image.bin` produced.

- [ ] **Step 4: Commit**

```bash
cd 23-fixes && git add kernel/kernel.h kernel/kernel.c && git commit -m "feat(kernel): add global kernel_state_t (g_kernel) with weighted policy"
```

---

### Task 4: Per-demo default weights (`tasks_demo`)

**Files:**
- Modify: `23-fixes/drivers/tasks_demo.h`
- Modify: `23-fixes/drivers/tasks_demo.c`

**Interfaces:**
- Consumes: `task_create_prio` (Task 2) — but demo table only carries metadata;
  `cmd_run` (Task 5) will consume `weight`.
- Produces: `demo_task_t.weight` (uint8_t), `DEMO_TASKS[]` with `spin = 3`,
  others `= 1`.

- [ ] **Step 1: Add `weight` to `demo_task_t`**

In `23-fixes/drivers/tasks_demo.h` change the struct to:
```c
typedef struct {
    const char *name;
    void (*entry)(void);
    const char *desc;
    uint8_t     weight;   /* default weight when spawned via RUN (1..255) */
} demo_task_t;
```

- [ ] **Step 2: Set default weights in the table**

In `23-fixes/drivers/tasks_demo.c` change the `DEMO_TASKS[]` initializer to use
designated initializers (keep order: blink, count, clock, ping, spin):
```c
demo_task_t DEMO_TASKS[] = {
    {"blink", demo_blink, "toggle a cell every 0.5s", 1},
    {"count", demo_count, "increment a counter every 1s", 1},
    {"clock", demo_clock, "print RTC time every 1s", 1},
    {"ping",  demo_ping,  "alive heartbeat every 1.5s", 1},
    {"spin",  demo_spin,  "busy task that yields to the scheduler", 3},
};
```

- [ ] **Step 3: Build**

```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success (warning-free; `weight` is used by Task 5 next).

- [ ] **Step 4: Commit**

```bash
cd 23-fixes && git add drivers/tasks_demo.h drivers/tasks_demo.c && git commit -m "feat(demo): give demo tasks default weights (spin=3)"
```

---

### Task 5: Shell `RUN [weight]` + `W` column in `TASKS`/`PS`

**Files:**
- Modify: `23-fixes/drivers/shell.c`

**Interfaces:**
- Consumes: `task_create_prio` (Task 2), `demo_task_t.weight` (Task 4),
  `g_kernel` (Task 3) for policy reporting.
- Produces: `RUN <name> [weight]` parsing; `print_tasks` shows weight column.

- [ ] **Step 1: Parse optional weight in `cmd_run`**

Replace the existing `static void cmd_run(char *args) { ... }` (the body that
loops over `DEMO_TASK_COUNT` and calls `task_create`) so it parses an optional
trailing weight and calls `task_create_prio`:
```c
/* RUN <name> [weight]: spawn a demo task, optionally with a weight. */
static void cmd_run(char *args) {
    if (args == 0 || args[0] == '\0') {
        kprint("usage: RUN <name> [weight]  (try: ");
        int i;
        for (i = 0; i < DEMO_TASK_COUNT; i++) {
            kprint((char *)DEMO_TASKS[i].name);
            if (i + 1 < DEMO_TASK_COUNT) kprint(" ");
        }
        kprint(")\n");
        return;
    }

    /* Split name and optional weight at the first space. */
    char *name = args;
    char *wptr = 0;
    int i = 0;
    for (; args[i] != '\0'; i++) {
        if (args[i] == ' ' || args[i] == '\t') {
            args[i] = '\0';
            wptr = args + i + 1;
            while (*wptr == ' ' || *wptr == '\t') wptr++;
            break;
        }
    }

    uint8_t weight = 0;
    if (wptr != 0 && wptr[0] != '\0') {
        int v = 0, j = 0;
        for (; wptr[j] >= '0' && wptr[j] <= '9'; j++) v = v * 10 + (wptr[j] - '0');
        if (v > 255) v = 255;
        if (v < 1)   v = 1;
        weight = (uint8_t)v;
    }

    for (i = 0; i < DEMO_TASK_COUNT; i++) {
        if (strcmp((char *)name, (char *)DEMO_TASKS[i].name) == 0) {
            /* If no explicit weight given, use the demo's default. */
            uint8_t w = (weight == 0) ? DEMO_TASKS[i].weight : weight;
            int id = task_create_prio(DEMO_TASKS[i].name, DEMO_TASKS[i].entry, w);
            if (id < 0) {
                kprint("failed: task table full\n");
            } else {
                char ibuf[8], wbuf[8];
                int_to_ascii(id, ibuf);
                int_to_ascii((int)w, wbuf);
                kprint("spawned ");
                kprint((char *)DEMO_TASKS[i].name);
                kprint(" as task ");
                kprint(ibuf);
                kprint(" (weight ");
                kprint(wbuf);
                kprint(")\n");
            }
            return;
        }
    }
    kprint("unknown task: ");
    kprint(name);
    kprint("\n");
}
```

- [ ] **Step 2: Add the `W` column to `print_tasks`**

In `print_tasks`, change the header line and the per-row output to include
weight. Replace:
```c
    kprint("ID  NAME            STATE   TICKS\n");
```
with:
```c
    kprint("ID  NAME            STATE   TICKS  W\n");
```
and after the `kprint(tbuf); kprint("\n");` (inside the loop, before the closing
`}`), insert:
```c
        char wbuf[8];
        int_to_ascii((int)t->weight, wbuf);
        kprint("    ");
        kprint(wbuf);
```
Keep the existing `tasks: N` summary line unchanged.

- [ ] **Step 3: Build**

```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success, `os-image.bin` produced, no warnings.

- [ ] **Step 4: Commit**

```bash
cd 23-fixes && git add drivers/shell.c && git commit -m "feat(shell): RUN <name> [weight] and W column in TASKS/PS"
```

---

### Task 6: Documentation — `scheduler-design.md` + `README.md`

**Files:**
- Modify: `docs/scheduler-design.md`
- Modify: `README.md` (repo root)

**Interfaces:**
- Consumes: the implemented behavior (Tasks 1–5).
- Produces: accurate docs; removes "Multitasking (coming soon)".

- [ ] **Step 1: Update `docs/scheduler-design.md`**

- In the Overview, change "round-robin scheduler" mention to "weighted
  round-robin scheduler (kernel task = idle)".
- In "Known limitations", replace the bullet
  `"Round-robin only — every task gets an equal 20 ms slice; no priorities."`
  with:
  ```
  - **Weighted round-robin** — a task with weight `w` receives up to `w`
    consecutive 20 ms slices before the scheduler rotates; long-term CPU is
    divided proportionally to weights. The kernel/idle task (slot 0) is not
    weighted and only runs when no weighted task is runnable.
  ```
- Add a short "Weights" subsection near "Timer integration":
  ```
  ## Weights

  `task_create_prio(name, entry, weight)` (and `task_create(name, entry)` as a
  weight-1 wrapper) set a task's static `weight` (clamped 1..255). The scheduler
  tracks `quantum_left` per task; while it is > 0 the same task is resumed on
  each timer tick, granting `weight` consecutive slices. `RUN <name> [weight]`
  in the shell passes an explicit weight; demo tasks carry defaults (`spin = 3`).
  ```
- Keep the other real limitations (ring 0 only; bump-heap stack pool with
  reuse; fixed `MAX_TASKS`).

- [ ] **Step 2: Update root `README.md`**

- Remove the blockquote:
  ```
  > **Multitasking (coming soon):** the kernel already ships a preemptive
  > round-robin scheduler (`cpu/task.h`), but the shell commands to drive it
  > (`TASKS`/`PS`, `RUN <name>`, `KILL <id>`, `YIELD`) are not wired into
  > `drivers/shell.c` yet... See [`docs/scheduler-design.md`](docs/scheduler-design.md) for the full design.
  ```
- In the shell-commands list, update the `RUN` description to
  `RUN <name> [weight] - spawn a demo task with an optional weight` and add a
  note that `TASKS`/`PS` show a `W` (weight) column.
- In "What's inside", change the Scheduler row note from
  `Preemptive round-robin scheduler (50 Hz timer) + kernel threads.` to
  `Preemptive weighted round-robin scheduler (50 Hz timer) + kernel threads; kernel task is idle.`

- [ ] **Step 3: Commit**

```bash
git add docs/scheduler-design.md README.md && git commit -m "docs: document weighted RR, remove 'coming soon' multitasking note"
```

---

### Task 7: End-to-end build + handoff verification

**Files:** (none changed; verification only)

- [ ] **Step 1: Clean build from scratch**

```bash
cd 23-fixes && make clean && make 2>&1 | tail -25
```
Expected: `os-image.bin` and `kernel.bin` built with no errors.

- [ ] **Step 2: Hand off QEMU verification to the user**

Report to the user that the build is green and list the manual checks
(also in the spec's Verification section):
1. `make run` (QEMU) → `HELP` shows `TASKS PS RUN KILL YIELD`.
2. `RUN blink` then `RUN spin` → `spin` advances faster (weight 3).
3. `RUN spin 1` vs `RUN spin 3` → weight 3 updates more often.
4. `TASKS` shows the `W` column with weights.
5. `KILL <id>` then repeat `RUN spin`/`KILL <id>` several times; `MEM` shows
   stack allocations stabilizing (leak fix).
6. `END` halts as before.

- [ ] **Step 3: Final commit (if any stray files remain)**

```bash
git status --short
```
If only build artifacts (already gitignored or untracked binaries) remain, no
commit needed. If any tracked source file is modified-and-uncommitted from the
above tasks, `git add` and commit it with a descriptive message.

---

## Self-Review Notes (author checklist — already applied)

- **Spec coverage:** weighted RR (Tasks 1,2,5), kernel-as-idle (Task 2 step 2),
  `task_create_prio` + wrapper (Task 2), `RUN [weight]` + `W` column (Task 5),
  demo defaults spin=3 (Task 4), stack-reuse leak fix (Task 2 step 1),
  `kernel_state_t`/`g_kernel` (Task 3), docs (Task 6). All spec items mapped.
- **Placeholders:** none — every code step shows full code.
- **Type consistency:** `task_create_prio(const char*, void(*)(void), uint8_t)`
  matches across Tasks 1 (proto), 2 (impl), 4 (demo Carry), 5 (call). `weight`
  is `uint8_t` in `task_t`, `demo_task_t`, and the `RUN` parser. `kernel_state_t`
  defined once in `kernel.h` (Task 3), consumed nowhere else yet except init.
