# Task 4: Per-demo default weights (`tasks_demo`)

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
Task 4 of 7. Adds a default `weight` (uint8_t) to each demo task so the weighted
scheduler effect is visible immediately when spawned. `task_create_prio` already
exists (Task 2). `cmd_run` (Task 5) will read `demo_task_t.weight`. Your task
only adds the field and sets defaults: `spin = 3`, others `= 1`.

## Files
- Modify: `23-fixes/drivers/tasks_demo.h`
- Modify: `23-fixes/drivers/tasks_demo.c`

## Interfaces
- Consumes: `task_create_prio` (Task 2) — but the table only carries metadata;
  `cmd_run` (Task 5) consumes `weight`.
- Produces: `demo_task_t.weight` (uint8_t); `DEMO_TASKS[]` with `spin = 3`,
  others `= 1`.

## Steps

### Step 1: Add `weight` to `demo_task_t`
In `23-fixes/drivers/tasks_demo.h` change the struct to:
```c
typedef struct {
    const char *name;
    void (*entry)(void);
    const char *desc;
    uint8_t     weight;   /* default weight when spawned via RUN (1..255) */
} demo_task_t;
```

### Step 2: Set default weights in the table
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

### Step 3: Build
```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success (warning-free; `weight` used by Task 5 next).

### Step 4: Commit
```bash
cd 23-fixes && git add drivers/tasks_demo.h drivers/tasks_demo.c && git commit -m "feat(demo): give demo tasks default weights (spin=3)"
```

## Global Constraints (verbatim)
- `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions
  -fno-pie -fno-stack-protector -fcommon -m32`.
- `weight` clamped 1..255; 0 -> 1.

## Report contract
Write full report to `.superpowers/sdd-mt/task-4-report.md`; return status/commit/summary only.
