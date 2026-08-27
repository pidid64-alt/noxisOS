# Task 2: Weighted `scheduler_tick` + stack-reuse in `task.c`

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
This is Task 2 of 7. Task 1 (already done) added three fields to `task_t` in
`cpu/task.h`: `weight` (`uint8_t`), `quantum_left` (`uint8_t`), `stack_base`
(`uint32_t`), plus the prototype `int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);`.
Your task implements the real logic in `23-fixes/cpu/task.c`: replace the old
`task_create` with `task_create_prio` (stack-reuse + weight), keep `task_create`
as a weight-1 wrapper, and rewrite `scheduler_tick` to be weighted.

The kernel task is slot 0 (the shell). It is the idle fallback and is NEVER
weighted. Round-robin rotation is over slots 1..MAX_TASKS-1 only.

## Files
- Modify: `23-fixes/cpu/task.c`

## Interfaces
- Consumes: `task_t.weight`, `task_t.quantum_left`, `task_t.stack_base`;
  `kmalloc`, `memory_set`, `tick` (extern, from timer.h), `KERNEL_CS`, `isr81`,
  `MAX_TASKS` (32), `TASK_STACK_SIZE` (4096).
- Produces: `task_create_prio(name, entry, weight)` (real body); `task_create`
  becomes a wrapper returning `task_create_prio(name, entry, 1)`. Weighted
  `scheduler_tick` behavior.

## Steps

### Step 1: Replace `task_create` with `task_create_prio` + wrapper
Delete the existing `int task_create(const char *name, void (*entry)(void))` body
and replace with (verbatim from plan):
```c
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight) {
    int slot = -1, i;
    for (i = 1; i < MAX_TASKS; i++) {        /* never reuse slot 0 */
        if (tasks[i].state == TASK_DEAD) { slot = i; break; }
    }
    if (slot < 0) return -1;

    if (weight == 0) weight = 1;             /* clamp */

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

### Step 2: Make `scheduler_tick` weighted
Replace the existing `void scheduler_tick(registers_t *r)` body with (verbatim):
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

### Step 3: Build to verify it compiles
```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: `os-image.bin` produced, no errors.

### Step 4: Commit
```bash
cd 23-fixes && git add cpu/task.c && git commit -m "feat(sched): weighted RR tick + stack-reuse on slot reclaim"
```

## Global Constraints (verbatim)
- `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions
  -fno-pie -fno-stack-protector -fcommon -m32`.
- `MAX_TASKS = 32`, `TASK_STACK_SIZE = 4096` (do not change).
- `weight` clamped 1..255; 0 -> 1.
- Kernel task always slot 0; never weighted/slept/killed.
- `registers_t`/`task_t` frame-copy semantics intact.
- No `free()` exists (bump heap); stack reuse, not freeing, is the leak fix.

## Report contract
Write full report to `.superpowers/sdd-mt/task-2-report.md`; return status/commit/summary only.
