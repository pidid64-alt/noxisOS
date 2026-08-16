#include "task.h"
#include "timer.h"     /* tick */
#include "idt.h"       /* set_idt_gate, KERNEL_CS */
#include "../libc/string.h"
#include "../libc/mem.h"      /* kmalloc, memory_set */
#include "../libc/function.h"
#include <stdint.h>

/* The global task table. Slot 0 is always the kernel/idle task; it runs the
 * interactive shell and absorbs the CPU whenever no other task is runnable. */
static task_t tasks[MAX_TASKS];

/* Index of the task currently on the CPU. -1 until the first schedule. */
static int current_task = -1;

/* Software interrupt used by task_yield() to voluntarily reschedule. */
#define SCHED_INT 0x81
extern void isr81(void);

/* Copy at most 15 chars of `n` into a task name, NUL-terminated. */
static void set_name(task_t *t, const char *n) {
    int i;
    for (i = 0; i < 15 && n[i] != '\0'; i++) t->name[i] = n[i];
    t->name[i] = '\0';
}

/* Shared entry point for every spawned task. Runs on the task's own kernel
 * stack. When the demo entry returns, the task is marked DEAD so the scheduler
 * stops selecting it. The trailing hlt keeps a dead task from falling off the
 * end of the world. */
static void task_trampoline(void) {
    int me = current_task;
    if (me >= 0 && me < MAX_TASKS && tasks[me].entry) {
        tasks[me].entry();
    }
    if (me >= 0 && me < MAX_TASKS) {
        tasks[me].state = TASK_DEAD;
        tasks[me].kill_req = 0;
    }
    for (;;) asm volatile("hlt");
}

void task_init(void) {
    int i;
    for (i = 0; i < MAX_TASKS; i++) tasks[i].state = TASK_DEAD;

    /* Kernel/idle task in slot 0. Its real context is captured live on the
     * first timer tick; until then we just mark it running. */
    tasks[0].id = 0;
    set_name(&tasks[0], "kernel");
    tasks[0].state = TASK_RUNNING;
    tasks[0].ticks = 0;
    tasks[0].kill_req = 0;
    tasks[0].wake_tick = 0;
    tasks[0].entry = 0;
    tasks[0].stack_top = 0; /* uses the shared kernel stack */
    memory_set((uint8_t *)&tasks[0].regs, 0, sizeof(registers_t));

    current_task = 0;

    /* Install the voluntary-yield software interrupt (int $0x81) and register
     * the scheduler to run when it fires, so task_yield() actually reschedules.
     * The IDT is already loaded; writing the in-memory gate + handler table is enough. */
    set_idt_gate(SCHED_INT, (uint32_t)isr81);
    register_interrupt_handler(SCHED_INT, scheduler_tick);
}

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

    /* Build a synthetic interrupt frame at the top of the new stack so that
     * the scheduler's iret resumes task_trampoline. iret pops, low address
     * first: eip, cs, eflags, esp, ss. We push in the reverse order so ss ends
     * up at the highest address. */
    uint32_t *sp = (uint32_t *)stack_top;
    *--sp = 0x10;                  /* ss  (kernel data) */
    *--sp = stack_top - 20;        /* esp: post-iret stack pointer (just below frame) */
    *--sp = 0x202;                 /* eflags: interrupts enabled */
    *--sp = KERNEL_CS;             /* cs  (kernel code) */
    *--sp = (uint32_t)task_trampoline; /* eip */

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
    t->regs.esp = (uint32_t)sp;    /* popa loads this; iret then pops eip..ss */
    t->regs.eip = (uint32_t)task_trampoline; /* informational; iret uses the stack */

    return slot;
}

/* Backwards-compatible wrapper: weight 1. */
int task_create(const char *name, void (*entry)(void)) {
    return task_create_prio(name, entry, 1);
}

int task_kill(int id) {
    if (id <= 0 || id >= MAX_TASKS) return -1;
    if (tasks[id].state == TASK_DEAD) return -1;
    tasks[id].kill_req = 1; /* cooperative: the task exits on its next poll */
    return 0;
}

/* The heart of the scheduler. Called from the timer IRQ (and the yield
 * interrupt) with the frame of the task that was just interrupted. We save it
 * into the PCB, pick the next runnable task, and copy its context into *r so the
 * ISR stub resumes it instead of the interrupted task.
 *
 * Scheduling policy is weighted round-robin: a task with weight w is resumed for
 * up to w consecutive 20 ms timeslices (tracked by quantum_left) before the
 * scheduler rotates to the next runnable task. The kernel/idle task (slot 0) is
 * never weighted — it only runs when no weighted task is runnable. */
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
     *    Wake SLEEPING tasks whose wake_tick has arrived. Skip the kernel slot. */
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
    *r = tasks[next].regs; /* the ISR stub's popa/iret will now resume `next` */
}

void task_yield(void) {
    asm volatile("int $0x81");
}

void task_sleep(uint32_t ms) {
    int me = current_task;
    if (me <= 0 || me >= MAX_TASKS) return; /* kernel/idle never sleeps */
    if (tasks[me].state == TASK_DEAD) return;

    /* PIT runs at 50 Hz => one tick every 20 ms. Round up. */
    uint32_t ticks_needed = (ms + 19) / 20;
    if (ticks_needed == 0) ticks_needed = 1;
    tasks[me].wake_tick = tick + ticks_needed;
    tasks[me].state = TASK_SLEEPING;
    task_yield(); /* immediately give the CPU to the next runnable task */
}

int task_should_exit(void) {
    if (current_task < 0 || current_task >= MAX_TASKS) return 0;
    return tasks[current_task].kill_req;
}

int task_current_id(void) { return current_task; }

int task_count(void) {
    int n = 0, i;
    for (i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING) n++;
    return n;
}

task_t *task_get(int id) {
    if (id < 0 || id >= MAX_TASKS) return 0;
    return &tasks[id];
}

uint32_t ticks_now(void) { return tick; }
