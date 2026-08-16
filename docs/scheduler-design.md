# noxis Scheduler — Design Spec

## Overview

`noxis` has a **preemptive, weighted round-robin scheduler** for *kernel
threads* (ring-0 only). The kernel/idle task is slot 0 and is never weighted. It is driven by the PIT timer at **50 Hz**. Every timer tick the
ISR calls `scheduler_tick(&regs)`, which swaps the running task's saved CPU
context for the next runnable one. There is no user mode yet, so "tasks" are
lightweight kernel coroutines, not separate address spaces.

```
        PIT @ 50 Hz
            │  IRQ
            ▼
   timer ISR ── captures registers_t *r
            │
            ▼
   scheduler_tick(r)
     • save *r into cur->regs   (PCB)
     • pick next READY task
     • copy next->regs into *r
            │
            ▼
   ISR stub: popa / iret   ── resumes next task
```

## Data structures

`task_t` (the PCB) and `registers_t` (the interrupt frame) are reused directly:

- `id` — slot index (0 = kernel/idle task, always present).
- `name[16]` — label shown by `TASKS`/`PS`.
- `state` — `TASK_DEAD | TASK_READY | TASK_RUNNING | TASK_SLEEPING`.
- `ticks` — timeslices this task has consumed (for `TASKS`).
- `kill_req` — set by `task_kill()`; the task polls `task_should_exit()`.
- `wake_tick` — when `SLEEPING`, becomes READY again once `tick >= wake_tick`.
- `entry` — demo entry point run by the shared trampoline.
- `stack_top` — top of the `kmalloc`'d 4 KB page-aligned stack (initial ESP).
- `regs` — a `registers_t` frame; **this is the same struct the ISR pushes**,
  so saving/restoring it is a straight copy.

`MAX_TASKS = 32`, `TASK_STACK_SIZE = 4096` (page-aligned via `kmalloc(...,1,...)`).

## Context switch flow

`scheduler_tick(registers_t *r)` receives the *interrupted* frame:

1. `cur = task_current(); cur->regs = *r;` — save the live frame into the PCB.
2. Walk the table (round-robin from the last slot) for the next `READY` task;
   if none, fall back to task 0 (idle/shell).
3. Mark the new task `RUNNING` and `*r = next->regs;` — overwrite the frame the
   ISR will `popa`/`iret`. Because the ISR stub pops that frame on return, the
   CPU transparently resumes `next`. The swap is O(1) per tick.

## Initial-task stack frame

A freshly `task_create()`'d task is started by building an `iret`-shaped frame
at the **top of its new stack**:

```
   high addr ┌───────────┐
             │ ss        │
             │ esp       │
             │ eflags    │
             │ cs        │
             │ eip ──────┼──► trampoline / entry
   low addr  └───────────┘
```

On the first schedule, `next->regs` points this frame at the trampoline; the
`iret` semantics pop `ss:esp/eflags:cs:eip` and begin executing in ring 0.

## Timer integration

The PIT is configured to 50 Hz (`init_timer`). The timer ISR captures the
current `registers_t *r` and calls `scheduler_tick(r)`. With 50 ticks/s, each
task gets a 20 ms timeslice whether or not it cooperates.

## Weights

`task_create_prio(name, entry, weight)` (and `task_create(name, entry)` as a
weight-1 wrapper) set a task's static `weight` (clamped 1..255). The scheduler
tracks `quantum_left` per task; while it is > 0 the same task is resumed on
each timer tick, granting `weight` consecutive slices. `RUN <name> [weight]`
in the shell passes an explicit weight; demo tasks carry defaults (`spin = 3`).

## `task_yield()`

A busy task that would otherwise spin calls `task_yield()`, which fires software
interrupt `0x81`; its handler routes to `scheduler_tick()` for a voluntary
reschedule.

## `task_sleep(ms)`

The caller sets `state = SLEEPING` and `wake_tick = tick + ms/20`. The scheduler
skips `SLEEPING` tasks until `tick >= wake_tick`, then flips them back to
`READY`.

## Cooperative kill

`task_kill(id)` sets `kill_req = 1` (and refuses slot 0). The task must poll
`task_should_exit()` in its loop and `return`; the trampoline then marks the
slot `TASK_DEAD`. There is no forced teardown.

## Known limitations

- **No user mode** — tasks run in ring 0; no isolation or separate address spaces.
- **No stack recycling** — `task_kill()` frees the PCB slot but never `free()`s
  the 4 KB stack (the bump heap has no `free()` anyway). Stacks leak.
- **Fixed resources** — `MAX_TASKS` and `TASK_STACK_SIZE` are compile-time
  constants; stacks are pre-sized, not growable.
- **Weighted round-robin** — a task with weight `w` receives up to `w`
  consecutive 20 ms slices before the scheduler rotates; long-term CPU is
  divided proportionally to weights. The kernel/idle task (slot 0) is not
  weighted and only runs when no weighted task is runnable.
