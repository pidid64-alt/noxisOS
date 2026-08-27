# Task 3: Global `kernel_state_t` in `kernel.h` / `kernel.c`

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
Task 3 of 7. Introduces a single global OS-state struct so OS-wide state has
one home ("global" layer the request asked for). `kernel_main` already calls
`task_init()`; you add the global struct and initialize it. The scheduler's
`task_t` and `SCHED_WEIGHTED` policy are referenced.

## Files
- Modify: `23-fixes/kernel/kernel.h`
- Modify: `23-fixes/kernel/kernel.c`

## Interfaces
- Consumes: `task_t` (fwd via `task.h`), scheduler policy concept.
- Produces: `kernel_state_t`, `extern kernel_state_t g_kernel`, initialized in
  `kernel_main` after `task_init()`.

## Steps

### Step 1: Declare `kernel_state_t` in `kernel.h`
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

### Step 2: Define and initialize `g_kernel` in `kernel.c`
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

### Step 3: Build
```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success, `os-image.bin` produced.

### Step 4: Commit
```bash
cd 23-fixes && git add kernel/kernel.h kernel/kernel.c && git commit -m "feat(kernel): add global kernel_state_t (g_kernel) with weighted policy"
```

## Global Constraints (verbatim)
- `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions
  -fno-pie -fno-stack-protector -fcommon -m32`.
- Kernel task always slot 0.
- No `free()` exists.

## Report contract
Write full report to `.superpowers/sdd-mt/task-3-report.md`; return status/commit/summary only.
