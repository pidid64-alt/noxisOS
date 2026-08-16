#include "shell.h"
#include "screen.h"
#include "rtc.h"
#include "../cpu/timer.h"
#include "../cpu/isr.h"
#include "../cpu/task.h"
#include "tasks_demo.h"
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
static void cmd_uptime(char *args);
static void cmd_mem(char *args);
static void cmd_tasks(char *args);
static void cmd_ps(char *args);
static void cmd_run(char *args);
static void cmd_kill(char *args);
static void cmd_yield(char *args);

static shell_command_t COMMANDS[] = {
    {"HELP",    "list available commands",          cmd_help},
    {"CLEAR",   "clear the screen",                 cmd_clear},
    {"ECHO",    "print its arguments back",         cmd_echo},
    {"VERSION", "show the OS version banner",        cmd_version},
    {"TIME",    "show current date and time",       cmd_time},
    {"END",     "halt the CPU",                     cmd_end},
    {"PAGE",    "test kmalloc() and print an address", cmd_page},
    {"UPTIME",  "show time elapsed since boot",     cmd_uptime},
    {"MEM",     "show heap allocation statistics",   cmd_mem},
    {"TASKS",   "list running tasks (alias PS)",    cmd_tasks},
    {"PS",      "list running tasks",               cmd_ps},
    {"RUN",     "spawn a demo task: RUN <name> [weight]", cmd_run},
    {"KILL",    "ask a task to exit: KILL <id>",    cmd_kill},
    {"YIELD",   "voluntarily reschedule the CPU",   cmd_yield},
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

/* The PIT runs at 50 Hz (set in init_timer via irq_install), so each tick is
 * 1/50 s. Report elapsed time as whole seconds and centiseconds. */
static void cmd_uptime(char *args) {
    UNUSED(args);
    /* Timer frequency is fixed at 50 Hz; mirror it here rather than pulling
     * the value out of timer.c. */
    const uint32_t FREQ = 50;
    uint32_t secs  = tick / FREQ;
    uint32_t centis = (tick % FREQ) * (100 / FREQ);

    char buf[12];
    int_to_ascii((int)secs, buf);
    kprint("Uptime: ");
    kprint(buf);
    kprint(".");

    /* Zero-pad the centiseconds to two digits (e.g. 7 -> "07"). */
    if (centis < 10) kprint("0");
    int_to_ascii((int)centis, buf);
    kprint(buf);
    kprint("s");
    kprint("\n");
}

/* Report how much of the fixed-size kernel heap has been consumed by kmalloc
 * so far. The bump allocator (libc/mem.c) never frees, so everything between
 * HEAP_START and the current free_mem_addr is "allocated" and the rest is free.
 * Returns nothing; output is via kprint like the other commands. */
static void cmd_mem(char *args) {
    UNUSED(args);
    uint32_t allocated = free_mem_addr - HEAP_START;
    uint32_t free = HEAP_SIZE - allocated;

    char buf[12];
    int_to_ascii((int)allocated, buf);
    kprint("MEM: allocated ");
    kprint(buf);
    kprint(" bytes, free ");
    int_to_ascii((int)free, buf);
    kprint(buf);
    kprint(" bytes\n");
}

static void skip_leading_spaces(char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* Human-readable name for a task_state_t value. */
static const char *state_name(task_state_t s) {
    switch (s) {
        case TASK_DEAD:     return "DEAD";
        case TASK_READY:    return "READY";
        case TASK_RUNNING:  return "RUN";
        case TASK_SLEEPING: return "SLEEP";
        default:            return "?";
    }
}

/* Shared body for TASKS / PS: dump the task table with id, name, state, ticks, weight. */
static void print_tasks(char *args) {
    UNUSED(args);
    kprint("ID  NAME            STATE   TICKS  W\n");
    int i;
    for (i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t == 0 || t->state == TASK_DEAD) continue;
        char idbuf[8], tbuf[8], wbuf[8];
        int_to_ascii(t->id, idbuf);
        int_to_ascii((int)t->ticks, tbuf);
        int_to_ascii((int)t->weight, wbuf);
        kprint(idbuf);
        kprint("   ");
        kprint(t->name);
        /* pad name to 16 chars */
        int nlen = (int)strlen(t->name);
        for (; nlen < 15; nlen++) kprint(" ");
        kprint((char *)state_name(t->state));
        kprint("   ");
        kprint(tbuf);
        kprint("    ");
        kprint(wbuf);
        kprint("\n");
    }
    char cbuf[8];
    int_to_ascii(task_count(), cbuf);
    kprint("tasks: ");
    kprint(cbuf);
    kprint("\n");
}

static void cmd_tasks(char *args) { print_tasks(args); }
static void cmd_ps(char *args)    { print_tasks(args); }

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
        if (strcasecmp((char *)name, (char *)DEMO_TASKS[i].name) == 0) {
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

/* KILL <id>: cooperatively ask a task to exit. */
static void cmd_kill(char *args) {
    if (args == 0 || args[0] == '\0') {
        kprint("usage: KILL <id>\n");
        return;
    }
    int id = 0, sign = 1, i = 0;
    if (args[0] == '-') { sign = -1; i = 1; }
    for (; args[i] != '\0'; i++) id = id * 10 + (args[i] - '0');
    id *= sign;
    if (task_kill(id) != 0) {
        char ibuf[8];
        int_to_ascii(id, ibuf);
        kprint("cannot kill task ");
        kprint(ibuf);
        kprint(" (bad id or kernel task)\n");
    } else {
        char ibuf[8];
        int_to_ascii(id, ibuf);
        kprint("signalled task ");
        kprint(ibuf);
        kprint(" to exit\n");
    }
}

/* YIELD: let the scheduler pick the next runnable task right now. */
static void cmd_yield(char *args) {
    UNUSED(args);
    task_yield();
    kprint("yielded\n");
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
        if (strcasecmp(name, (char *)COMMANDS[i].name) == 0) {
            COMMANDS[i].handler(args);
            return;
        }
    }

    kprint("Unknown command: ");
    kprint(name);
    kprint("\n");
}
