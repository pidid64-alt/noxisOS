#include "../cpu/isr.h"
#include "../drivers/screen.h"
#include "../drivers/shell.h"
#include "kernel.h"
#include <stdint.h>

void kernel_main() {
    isr_install();
    irq_install();

    asm("int $2");
    asm("int $3");

    kprint("Welcome to noxis\n"
        "Type HELP for a list of commands\n> ");
}

void user_input(char *input) {
    shell_run(input);
    kprint("\n> ");
}
