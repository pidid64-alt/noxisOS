#include "../cpu/isr.h"
#include "../cpu/task.h"
#include "../drivers/fb.h"
#include "../drivers/screen.h"
#include "../drivers/shell.h"
#include "kernel.h"
#include <stdint.h>

kernel_state_t g_kernel;

/* magic/mbi_addr come from _start: GRUB passes 0x2BADB002 + the Multiboot
 * info pointer, the floppy path zeroes both (see boot/pm_relocate.asm). */
void kernel_main(uint32_t magic, uint32_t mbi_addr) {
    /* TEMP DEBUG marker: confirm control reached kernel under GRUB/UEFI. */
    asm volatile("movb $'K', %%al\n\toutb %%al, $0xe9" ::: "al");
    /* First of all: if GRUB handed us a framebuffer (UEFI GOP / VBE), start
     * drawing there, so the banner is visible even if later init explodes. */
    fb_init(magic, mbi_addr);

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
