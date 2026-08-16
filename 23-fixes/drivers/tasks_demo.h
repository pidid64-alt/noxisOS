#ifndef TASKS_DEMO_H
#define TASKS_DEMO_H
#include "../cpu/task.h"
/* One-line descriptor for the shell's RUN command, so the user can do
   RUN blink / RUN count / etc. Keep names lowercase, <= 12 chars. */
typedef struct { const char *name; void (*entry)(void); const char *desc; uint8_t weight; } demo_task_t;
extern demo_task_t DEMO_TASKS[];
extern int DEMO_TASK_COUNT;
#endif
