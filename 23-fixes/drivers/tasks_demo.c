#include "tasks_demo.h"
#include "screen.h"
#include "rtc.h"
#include "../cpu/timer.h"
#include "../libc/string.h"
#include "../libc/function.h"
#include "../cpu/task.h"
#include <stdint.h>

/* These demo tasks run on their OWN kernel stack (no tty / no keyboard).
 * Each loops, polls task_should_exit(), and yields/sleeps regularly so the
 * shell (slot 0) is never starved. Output goes to fixed screen cells so the
 * 80x25 VGA display never scrolls from task spam. */

/* ------------------------------------------------------------------ blink */
void demo_blink(void) {
    const int COL = 0, ROW = 20;
    int on = 0;
    for (;;) {
        if (task_should_exit()) break;
        const char *txt = on ? "[blink] #" : "[blink] .";
        kprint_at((char *)"           ", COL, ROW);
        kprint_at((char *)txt, COL, ROW);
        on = !on;
        task_sleep(500);   /* ~2 Hz toggle */
    }
    kprint_at((char *)"           ", COL, ROW);
}

/* ----------------------------------------------------------------- count */
void demo_count(void) {
    const int COL = 0, ROW = 21;
    int n = 0;
    char buf[16];
    for (;;) {
        if (task_should_exit()) break;
        kprint_at((char *)"           ", COL, ROW);
        kprint_at((char *)"[count] ", COL, ROW);
        int_to_ascii(n, buf);
        kprint_at(buf, COL + 8, ROW);
        n++;
        task_sleep(1000);  /* 1 Hz counter */
    }
}

/* ----------------------------------------------------------------- clock */
void demo_clock(void) {
    const int COL = 0, ROW = 22;
    uint8_t yr, mo, day, h, m, s;
    char buf[8];
    for (;;) {
        if (task_should_exit()) break;
        rtc_get_time(&yr, &mo, &day, &h, &m, &s);
        kprint_at((char *)"                    ", COL, ROW);
        kprint_at((char *)"[clock] ", COL, ROW);
        int_to_ascii(h, buf); kprint_at(buf, COL + 9,  ROW);
        kprint_at((char *)":",  COL + 11, ROW);
        int_to_ascii(m, buf); kprint_at(buf, COL + 12, ROW);
        kprint_at((char *)":",  COL + 14, ROW);
        int_to_ascii(s, buf); kprint_at(buf, COL + 15, ROW);
        task_sleep(1000);  /* 1 Hz wall clock */
    }
}

/* ------------------------------------------------------------------ ping */
void demo_ping(void) {
    const int COL = 0, ROW = 23;
    char buf[16];
    for (;;) {
        if (task_should_exit()) break;
        kprint_at((char *)"                       ", COL, ROW);
        kprint_at((char *)"[ping] alive tick=", COL, ROW);
        int_to_ascii((int)ticks_now(), buf);
        kprint_at(buf, COL + 18, ROW);
        task_sleep(1500);  /* heartbeat every 1.5 s */
    }
}

/* ------------------------------------------------------------------ spin */
void demo_spin(void) {
    const int COL = 0, ROW = 24;
    volatile uint32_t acc = 0;
    int n = 0;
    char buf[16];
    for (;;) {
        if (task_should_exit()) break;
        /* Busy loop doing a tiny bit of math, yielding frequently so the
         * preemptive scheduler can still run the shell and friends. */
        int i;
        for (i = 0; i < 5000; i++) {
            acc += (uint32_t)(acc * 1103515245u + 12345u);
        }
        n++;
        if (n % 50 == 0) {
            kprint_at((char *)"               ", COL, ROW);
            kprint_at((char *)"[spin] n=", COL, ROW);
            int_to_ascii(n, buf);
            kprint_at(buf, COL + 9, ROW);
        }
        task_yield();  /* hand the CPU back after a burst of work */
    }
}

/* ------------------------------------------------------------------ table */
demo_task_t DEMO_TASKS[] = {
    {"blink", demo_blink, "toggle a cell every 0.5s", 1},
    {"count", demo_count, "increment a counter every 1s", 1},
    {"clock", demo_clock, "print RTC time every 1s", 1},
    {"ping",  demo_ping,  "alive heartbeat every 1.5s", 1},
    {"spin",  demo_spin,  "busy task that yields to the scheduler", 3},
};
int DEMO_TASK_COUNT = (int)(sizeof(DEMO_TASKS) / sizeof(DEMO_TASKS[0]));
