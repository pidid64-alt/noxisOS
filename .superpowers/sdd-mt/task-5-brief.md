# Task 5: Shell `RUN [weight]` + `W` column in `TASKS`/`PS`

**From plan:** docs/superpowers/plans/2026-08-15-multitasking-core.md

## Context
Task 5 of 7. Wires the weighted scheduler into the shell. `task_create_prio`
exists (Task 2); demo tasks carry default weights (Task 4). Your task: parse an
optional trailing weight in `cmd_run` and call `task_create_prio`, and add a `W`
(weight) column to `print_tasks`.

## Files
- Modify: `23-fixes/drivers/shell.c`

## Interfaces
- Consumes: `task_create_prio` (Task 2), `demo_task_t.weight` (Task 4),
  `g_kernel` (Task 3) — note: `cmd_run`/`print_tasks` may reference `g_kernel`
  but this task does NOT need to change it; only consume `task_create_prio` and
  `DEMO_TASKS[i].weight`.
- Produces: `RUN <name> [weight]` parsing; `print_tasks` shows weight column.

## Steps

### Step 1: Parse optional weight in `cmd_run`
Replace the existing `static void cmd_run(char *args) { ... }` body (the one that
loops over `DEMO_TASK_COUNT` and calls `task_create`) with:
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

### Step 2: Add the `W` column to `print_tasks`
In `print_tasks`, change the header line:
```c
    kprint("ID  NAME            STATE   TICKS\n");
```
to:
```c
    kprint("ID  NAME            STATE   TICKS  W\n");
```
and after the existing `kprint(tbuf); kprint("\n");` (inside the loop, before
the closing `}`), insert:
```c
        char wbuf[8];
        int_to_ascii((int)t->weight, wbuf);
        kprint("    ");
        kprint(wbuf);
```
Keep the existing `tasks: N` summary line unchanged.

### Step 3: Build
```bash
cd 23-fixes && make 2>&1 | tail -20
```
Expected: success, `os-image.bin` produced, no warnings.

### Step 4: Commit
```bash
cd 23-fixes && git add drivers/shell.c && git commit -m "feat(shell): RUN <name> [weight] and W column in TASKS/PS"
```

## Global Constraints (verbatim)
- `CC = gcc`, `LD = ld`, `CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions
  -fno-pie -fno-stack-protector -fcommon -m32`.
- `weight` clamped 1..255; 0 -> 1.

## Report contract
Write full report to `.superpowers/sdd-mt/task-5-report.md`; return status/commit/summary only.
