# Task 6: Documentation — `scheduler-design.md` + `README.md`

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
Task 6 of 7. Updates docs to reflect the committed weighted scheduler and removes
the "Multitasking (coming soon)" note. Behavior implemented in Tasks 1–5.

## Files
- Modify: `docs/scheduler-design.md`
- Modify: `README.md` (repo root)

## Interfaces
- Consumes: implemented behavior (Tasks 1–5).
- Produces: accurate docs; removes "coming soon".

## Steps

### Step 1: Update `docs/scheduler-design.md`
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
- Keep the other real limitations (ring 0 only; bump-heap stack pool with reuse;
  fixed `MAX_TASKS`).

### Step 2: Update root `README.md`
- Remove the blockquote:
  ```
  > **Multitasking (coming soon):** the kernel already ships a preemptive
  > round-robin scheduler (`cpu/task.h`), but the shell commands to drive it
  > (`TASKS`/`PS`, `RUN <name>`, `KILL <id>`, `YIELD`) are not wired into
  > `drivers/shell.c` yet... See [`docs/scheduler-design.md`](docs/scheduler-design.md) for the full design.
  ```
  (Remove the entire blockquote paragraph that starts with "Multitasking (coming soon)".)
- In the shell-commands list (or the "Shell commands" fenced block), update the
  `RUN` description to `RUN <name> [weight] - spawn a demo task with an optional
  weight` and note that `TASKS`/`PS` show a `W` (weight) column.
- In "What's inside", change the Scheduler row note from
  `Preemptive round-robin scheduler (50 Hz timer) + kernel threads.` to
  `Preemptive weighted round-robin scheduler (50 Hz timer) + kernel threads; kernel task is idle.`

### Step 3: Commit
```bash
git add docs/scheduler-design.md README.md && git commit -m "docs: document weighted RR, remove 'coming soon' multitasking note"
```

## Global Constraints (verbatim)
- `weight` clamped 1..255; 0 -> 1.
- Kernel task always slot 0; never weighted.

## Report contract
Write full report to `.superpowers/sdd-mt/task-6-report.md`; return status/commit/summary only.
