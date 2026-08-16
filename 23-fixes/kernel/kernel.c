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

    kprint("Welcome to noxis\n"
        "Type HELP for a list of commands\n> ");
}

void user_input(char *input) {
    shell_run(input);
    kprint("\n> ");
}
