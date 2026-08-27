#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "isr.h"   /* registers_t */

/* How many tasks may exist at once, including the kernel/idle task in slot 0. */
#define MAX_TASKS 32

/* Per-task kernel stack size. kmalloc(..., 1, ...) page-aligns it, which keeps
 * the initial ESP aligned and avoids issues with the FPU/SSEx save areas later. */
#define TASK_STACK_SIZE 4096

typedef enum {
    TASK_DEAD = 0,   /* slot is free / task has exited */
    TASK_READY,      /* runnable, waiting for the scheduler to pick it */
    TASK_RUNNING,    /* currently executing on the CPU */
    TASK_SLEEPING    /* blocked until tick >= wake_tick */
} task_state_t;

/* Process Control Block. A task is just a saved CPU context plus bookkeeping. */
typedef struct {
    int id;              /* slot index; 0 is always the kernel/idle task */
    char name[16];
    task_state_t state;
    uint32_t ticks;      /* how many timeslices this task has been scheduled */
    uint8_t kill_req;    /* set by task_kill(); the task should exit at its leisure */
    uint32_t wake_tick;  /* when SLEEPING, become READY again once tick >= wake_tick */
    void (*entry)(void); /* demo entry point, run by the shared trampoline */
    uint32_t stack_top;  /* top of the kmalloc'd stack (initial ESP) */
    uint8_t  weight;        /* static weight 1..255 (default 1) */
    uint8_t  quantum_left;  /* consecutive 20ms slices left in current run */
    uint32_t stack_base;    /* kmalloc'd stack base; reused on slot reclaim */
    registers_t regs;    /* saved CPU context (matches interrupt.asm frame) */
} task_t;

/* One-time scheduler setup. Call once after the IDT/IRQ are installed. */
void task_init(void);

/* Spawn a new kernel task that runs `entry`. Returns the task id, or -1 if the
 * table is full. The task becomes READY and is picked up by the preemptive
 * scheduler on the next timer tick. Safe to call from any context. */
int task_create(const char *name, void (*entry)(void));

/* Spawn a task with an explicit static weight (clamped to 1..255; 0 -> 1).
 * Returns the task id, or -1 if the table is full. */
int task_create_prio(const char *name, void (*entry)(void), uint8_t weight);

/* Ask a task to exit. The task polls task_should_exit() in its loop and returns;
 * its trampoline then marks it DEAD. Returns 0 on success, -1 for a bad id or
 * for attempting to kill the kernel task (slot 0). */
int task_kill(int id);

/* Preemptive scheduler entry. Called from the timer ISR (or the yield
 * interrupt) with the interrupted CPU frame. It saves the current task's
 * context into its PCB and, by mutating *r, makes the ISR stub resume the next
 * runnable task instead. */
void scheduler_tick(registers_t *r);

/* Voluntarily hand the CPU to the next task. Used by tasks that would otherwise
 * busy-wait; triggers a reschedule through a dedicated software interrupt. */
void task_yield(void);

/* Block the calling task for `ms` milliseconds, then become READY again. */
void task_sleep(uint32_t ms);

/* True if the current task has been asked to exit. Tasks poll this in loops. */
int task_should_exit(void);

/* Id of the task currently on the CPU (-1 before the first schedule). */
int task_current_id(void);

/* Count of tasks that are READY or RUNNING (DEAD slots excluded). */
int task_count(void);

/* Direct index into the global task table (0 .. MAX_TASKS-1). */
task_t *task_get(int id);

/* Timer ticks since boot, for callers that want to schedule against time. */
uint32_t ticks_now(void);

#endif
