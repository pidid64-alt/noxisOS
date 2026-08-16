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
