# Task 1: Extend the PCB and add `task_create_prio` declaration

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context (for the implementer)
You are working in the `noxis` 32-bit freestanding OS repo. This is Task 1 of 7
that adds a weighted round-robin scheduler on top of an already-written
preemptive scheduler. Your task ONLY extends the PCB struct and adds a function
prototype in `23-fixes/cpu/task.h`. The real implementation of `task_create_prio`
and `scheduler_tick` happens in Task 2 (another agent). Do not implement the
bodies — just declare.

## Files
- Modify: `23-fixes/cpu/task.h`

## Interfaces
- Consumes: `task_t` (existing), `task_state_t` (existing), `registers_t` (from `isr.h`).
- Produces: new `task_t` fields `weight` (`uint8_t`), `quantum_left` (`uint8_t`),
  `stack_base` (`uint32_t`); new prototype
  `int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);`.

## Steps

### Step 1: Add the three fields to `task_t`
In `23-fixes/cpu/task.h`, inside the `task_t` struct add after `uint32_t stack_top;`:
```c
    uint8_t  weight;        /* static weight 1..255 (default 1) */
    uint8_t  quantum_left;  /* consecutive 20ms slices left in current run */
    uint32_t stack_base;    /* kmalloc'd stack base; reused on slot reclaim */
```

### Step 2: Add the weighted-spawn prototype
After the existing `int task_create(const char *name, void (*entry)(void));` declaration add:
```c
/* Spawn a task with an explicit static weight (clamped to 1..255; 0 -> 1).
 * Returns the task id, or -1 if the table is full. */
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);
```

### Step 3: Commit
```bash
cd 23-fixes && git add cpu/task.h && git commit -m "feat(sched): add weight/quantum_left/stack_base to task_t and task_create_prio prototype"
```

## Global Constraints (verbatim from plan)
- Build with `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra
  -fno-exceptions -fno-pie -fno-stack-protector -fcommon -m32` (verbatim from
  `23-fixes/Makefile`).
- All edits are under `23-fixes/`; `24-el-capitan/` reaches them via symlinks.
- `MAX_TASKS = 32`, `TASK_STACK_SIZE = 4096` (do not change).
- `weight` is clamped to `1..255`; `0` means `1`.
- Kernel task is always slot 0 and is never weighted, never slept, never killed.

## Report contract
Write your full report to `.superpowers/sdd-mt/task-1-report.md` and return ONLY:
status (DONE / DONE_WITH_CONCERNS / NEEDS_CONTEXT / BLOCKED), the commit hash, a
one-line build summary, and any concerns.
