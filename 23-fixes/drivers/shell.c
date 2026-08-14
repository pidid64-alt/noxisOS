#include "shell.h"
#include "screen.h"
#include "rtc.h"
#include "../cpu/isr.h"
#include "../libc/string.h"
#include "../libc/mem.h"
#include "../libc/function.h"
#include "../kernel/kernel.h"
#include <stdint.h>

/* Command handlers take the remainder of the input line after the command
 * name (e.g. for "ECHO hello world" the handler receives "hello world"). */
typedef void (*shell_cmd_t)(char *args);

typedef struct {
    const char *name;
    const char *desc;
    shell_cmd_t handler;
} shell_command_t;

/* Forward declarations of the command handlers. */
static void cmd_help(char *args);
static void cmd_clear(char *args);
static void cmd_echo(char *args);
static void cmd_version(char *args);
static void cmd_time(char *args);
static void cmd_end(char *args);
static void cmd_page(char *args);

static shell_command_t COMMANDS[] = {
    {"HELP",    "list available commands",          cmd_help},
    {"CLEAR",   "clear the screen",                 cmd_clear},
    {"ECHO",    "print its arguments back",         cmd_echo},
    {"VERSION", "show the OS version banner",        cmd_version},
    {"TIME",    "show current date and time",       cmd_time},
    {"END",     "halt the CPU",                     cmd_end},
    {"PAGE",    "test kmalloc() and print an address", cmd_page},
};

#define NUM_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static void cmd_help(char *args) {
    UNUSED(args);
    int i;
    kprint("Available commands:\n");
    for (i = 0; i < (int)NUM_COMMANDS; i++) {
        kprint("  ");
        kprint((char *)COMMANDS[i].name);
        kprint(" - ");
        kprint((char *)COMMANDS[i].desc);
        kprint("\n");
    }
}

static void cmd_clear(char *args) {
    UNUSED(args);
    clear_screen();
}

static void cmd_echo(char *args) {
    if (args != 0 && args[0] != '\0') {
        kprint(args);
    }
    kprint("\n");
}

static void cmd_version(char *args) {
    UNUSED(args);
    kprint("noxis v1.0 -- a from-scratch 32-bit kernel (os-tutorial)\n");
}

static void cmd_time(char *args) {
    UNUSED(args);
    rtc_print_time();
}

static void cmd_end(char *args) {
    UNUSED(args);
    kprint("Stopping the CPU. Bye!\n");
    asm volatile("hlt");
}

static void cmd_page(char *args) {
    UNUSED(args);
    uint32_t phys_addr;
    uint32_t page = kmalloc(1000, 1, &phys_addr);
    char page_str[16] = "";
    hex_to_ascii(page, page_str);
    char phys_str[16] = "";
    hex_to_ascii(phys_addr, phys_str);
    kprint("Page: ");
    kprint(page_str);
    kprint(", physical address: ");
    kprint(phys_str);
    kprint("\n");
}

static void skip_leading_spaces(char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

void shell_run(char *input) {
    char *p = input;
    skip_leading_spaces(&p);

    /* Split into command name and arguments at the first space. */
    char *name = p;
    char *args = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') {
        *p = '\0';       /* terminate the name */
        args = p + 1;
        skip_leading_spaces(&args);
    }

    if (name[0] == '\0') {
        /* Empty input: nothing to do. */
        return;
    }

    int i;
    for (i = 0; i < (int)NUM_COMMANDS; i++) {
        if (strcmp(name, (char *)COMMANDS[i].name) == 0) {
            COMMANDS[i].handler(args);
            return;
        }
    }

    kprint("Unknown command: ");
    kprint(name);
    kprint("\n");
}
