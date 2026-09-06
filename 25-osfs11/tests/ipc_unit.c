/* Deterministic regression for the boot/TTY IRQ race in REAL proc.c.
 * The IRQ arrives just after RECEIVING has been published. It must wait until
 * the whole sendrec transition (including block/schedule) has completed.
 * -no-pie keeps static messages below 4 GB for va2la's real 32-bit addresses.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#define PUBLIC
#define PRIVATE static
#define NR_TASKS 1
#define NR_PROCS 2
#define NR_NATIVE_PROCS 2
#define SEND 1
#define RECEIVE 2
#define SENDING 2
#define RECEIVING 4
#define ANY 13
#define NO_TASK 23
#define INTERRUPT -10
#define HARD_INT 1
#define INDEX_LDT_RW 1
#define STR_DEFAULT_LEN 256
#define GREEN 2
#define RED 4
#define MAKE_COLOR(x,y) (((x) << 4) | (y))
#define CRTC_ADDR_REG 0x3D4
#define CRTC_DATA_REG 0x3D5
#define START_ADDR_H 12
#define START_ADDR_L 13

typedef struct {
	int source, type;
	union { struct {
		int m3i1, m3i2, m3i3, m3i4;
		void *m3p1, *m3p2;
	} m3; } u;
} MESSAGE;
struct descriptor { u16 base_low; u8 base_mid, base_high; };
struct proc {
	struct descriptor ldts[2];
	int ldt_sel, ticks, priority;
	char name[16];
	int p_flags;
	MESSAGE *p_msg;
	int p_recvfrom, p_sendto, has_int_msg;
	struct proc *q_sending, *next_sending;
};
static struct proc proc_table[NR_TASKS + NR_PROCS], *p_proc_ready;
static int k_reenter;
#define FIRST_PROC proc_table[0]
#define LAST_PROC proc_table[NR_TASKS + NR_PROCS - 1]
#define proc2pid(p) ((int)((p) - proc_table))
#define phys_copy memcpy

void *va2la(int pid, void *va);
void inform_int(int task);
static int failures, checks, irq_enabled = 1, irq_pending, inject_irq;
static void disable_int(void) { irq_enabled = 0; }
static void enable_int(void)
{
	irq_enabled = 1;
	if (irq_pending) {
		irq_pending = 0;
		inform_int(0);
	}
}
static void kernel_assert(int ok, const char *expression)
{
	if (inject_irq && !strcmp(expression, "p_who_wanna_recv->p_flags == RECEIVING")) {
		inject_irq = 0;
		if (irq_enabled)
			inform_int(0);
		else
			irq_pending = 1;
	}
	if (!ok) {
		printf("FAIL: kernel assert(%s)\n", expression);
		failures++;
	}
}
#define assert(exp) kernel_assert(!!(exp), #exp)
static int printl(const char *fmt, ...) { (void)fmt; return 0; }
static void panic(const char *fmt, ...) { (void)fmt; failures++; }
static void out_byte(u16 port, u8 value) { (void)port; (void)value; }
static void disp_color_str(const char *s, int color) { (void)s; (void)color; }
static int diagnostic_format(char *s, const char *fmt, ...) { (void)s; (void)fmt; return 0; }
#define sprintf diagnostic_format
#include "../kernel/proc.c"
#undef sprintf

static MESSAGE messages[2];
#define CHECK(c, m) do { checks++; if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)
static void init_processes(void)
{
	int i;
	memset(proc_table, 0, sizeof(proc_table));
	memset(messages, 0, sizeof(messages));
	for (i = 0; i < NR_TASKS + NR_PROCS; i++) {
		proc_table[i].ticks = proc_table[i].priority = 15;
		proc_table[i].p_recvfrom = proc_table[i].p_sendto = NO_TASK;
	}
	p_proc_ready = proc_table;
	irq_enabled = 1;
	irq_pending = inject_irq = 0;
}
int main(void)
{
	init_processes();
	inject_irq = 1;
	CHECK(sys_sendrec(RECEIVE, ANY, &messages[0], &proc_table[0]) == 0, "receive succeeds with an IRQ at publication");
	CHECK(!inject_irq, "IRQ injection point reached");
	CHECK(irq_enabled && !irq_pending, "pending IRQ delivered after unlocking IPC");
	CHECK(messages[0].source == INTERRUPT && messages[0].type == HARD_INT, "IRQ message not lost");
	CHECK(!proc_table[0].p_flags && !proc_table[0].p_msg, "IRQ unblocks a fully initialized receiver");

	init_processes();
	inform_int(0);
	CHECK(sys_sendrec(RECEIVE, INTERRUPT, &messages[0], &proc_table[0]) == 0, "receive an already pending IRQ");
	CHECK(irq_enabled && !proc_table[0].has_int_msg, "early receive path restores IRQs");
	CHECK(messages[0].type == HARD_INT, "queued IRQ delivered");

	init_processes();
	sys_sendrec(RECEIVE, ANY, &messages[1], &proc_table[1]);
	messages[0].type = 42;
	CHECK(sys_sendrec(SEND, 1, &messages[0], &proc_table[0]) == 0, "direct send succeeds");
	CHECK(irq_enabled && !proc_table[1].p_flags, "direct send unblocks receiver and restores IRQs");
	CHECK(messages[1].source == 0 && messages[1].type == 42, "direct message copied");

	init_processes();
	messages[0].type = 99;
	sys_sendrec(SEND, 1, &messages[0], &proc_table[0]);
	CHECK(irq_enabled && proc_table[0].p_flags == SENDING, "queued sender blocks without disabling future IRQs");
	CHECK(sys_sendrec(RECEIVE, ANY, &messages[1], &proc_table[1]) == 0, "receive queued sender");
	CHECK(irq_enabled && !proc_table[0].p_flags, "queued sender unblocked");
	CHECK(messages[1].source == 0 && messages[1].type == 99, "queued message copied");
	printf("%s %d IPC UNIT CHECKS\n", failures ? "FAILED" : "PASSED", checks);
	return failures ? 1 : 0;
}
