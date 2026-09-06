/* Exercise the real VGA driver against an indexed-register model, not a
 * flat port array: AC flip-flops and CRTC protection caused the black screen.
 * Build with -iquote tests/stubs, without the freestanding libc headers. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#define PUBLIC
#define PRIVATE static
#define GFX_FB_W 320
#define GFX_FB_H 200
#define GFX_FB_BYTES (GFX_FB_W * GFX_FB_H)
#define V_MEM_SIZE 0x8000
#define VGA_MISC_W 0x3C2
#define VGA_MISC_R 0x3CC
#define VGA_SEQ_ADDR 0x3C4
#define VGA_SEQ_DATA 0x3C5
#define VGA_DAC_MASK 0x3C6
#define VGA_DAC_READ 0x3C7
#define VGA_DAC_WRITE 0x3C8
#define VGA_DAC_DATA 0x3C9
#define VGA_GC_ADDR 0x3CE
#define VGA_GC_DATA 0x3CF
#define VGA_CRTC_ADDR 0x3D4
#define VGA_CRTC_DATA 0x3D5
#define VGA_AC_ADDR 0x3C0
#define VGA_AC_RDY 0x3DA

static u8 text_memory[V_MEM_SIZE], font_memory[256 * 32];
#define V_MEM_BASE ((uintptr_t)text_memory)
#define GFX_FB_BASE ((uintptr_t)font_memory)
static u8 seq[5], crtc[25], gc[9], ac[21], palette[768], misc, dac_mask;
static int seq_index, crtc_index, gc_index, ac_index, ac_data_phase;
static int dac_read, dac_write, display_enabled, interrupts = 1;
static int protected_writes, bad_font_access;

static void disable_int(void) { interrupts = 0; }
static void enable_int(void) { interrupts = 1; }
static u8 in_byte(u16 port)
{
	switch (port) {
	case VGA_MISC_R: return misc;
	case VGA_SEQ_DATA: return seq[seq_index];
	case VGA_CRTC_DATA: return crtc[crtc_index];
	case VGA_GC_DATA: return gc[gc_index];
	case 0x3C1: return ac[ac_index]; /* does not reset the flip-flop! */
	case VGA_AC_RDY: ac_data_phase = 0; return 0;
	case VGA_DAC_MASK: return dac_mask;
	case VGA_DAC_DATA: return palette[dac_read++ % 768];
	default: return 0;
	}
}
static void out_byte(u16 port, u8 value)
{
	switch (port) {
	case VGA_MISC_W: misc = value; break;
	case VGA_SEQ_ADDR: seq_index = value; break;
	case VGA_SEQ_DATA: seq[seq_index] = value; break;
	case VGA_CRTC_ADDR: crtc_index = value; break;
	case VGA_CRTC_DATA:
		if (crtc_index < 8 && (crtc[0x11] & 0x80)) {
			/* Register 3's unlock bit can still be set. */
			if (crtc_index == 3 && value == (crtc[3] | 0x80))
				crtc[3] = value;
			else
				protected_writes++;
		} else {
			crtc[crtc_index] = value;
		}
		break;
	case VGA_GC_ADDR: gc_index = value; break;
	case VGA_GC_DATA: gc[gc_index] = value; break;
	case VGA_AC_ADDR:
		if (!ac_data_phase) {
			ac_index = value & 0x1F;
			display_enabled = !!(value & 0x20);
		} else if (ac_index < 21) {
			ac[ac_index] = value;
		}
		ac_data_phase ^= 1;
		break;
	case VGA_DAC_MASK: dac_mask = value; break;
	case VGA_DAC_READ: dac_read = value * 3; break;
	case VGA_DAC_WRITE: dac_write = value * 3; break;
	case VGA_DAC_DATA: palette[dac_write++ % 768] = value; break;
	}
}
static void *checked_memcpy(void *dest, const void *src, size_t count)
{
	if (dest == font_memory || src == font_memory) {
		if (seq[2] != 4 || seq[4] != 6 || gc[4] != 2 || gc[5] != 0 ||
		    gc[6] != 4 || (crtc[0x14] & 0x40) || interrupts)
			bad_font_access++;
	}
	return memcpy(dest, src, count);
}
#define memcpy checked_memcpy
#include "../kernel/vga.c"
#undef memcpy

static int checks, failures;
#define CHECK(c, m) do { checks++; if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
	u8 original[VGA_REG_BYTES], original_text[V_MEM_SIZE];
	u8 original_font[VGA_FONT_BYTES], original_palette[VGA_PALETTE_BYTES];
	int i, cycle;
	misc = 0x67;
	for (i = 0; i < 5; i++) seq[i] = i + 1;
	seq[4] = 2;
	for (i = 0; i < 25; i++) crtc[i] = i + 0x20;
	crtc[3] |= 0x80;
	crtc[0x11] = 0x8E;
	crtc[0x14] = 0x1F;
	for (i = 0; i < 9; i++) gc[i] = i + 0x10;
	for (i = 0; i < 21; i++) ac[i] = i + 0x20;
	for (i = 0; i < 768; i++) palette[i] = (i * 7) % 64;
	dac_mask = 0x7F;
	for (i = 0; i < V_MEM_SIZE; i++) text_memory[i] = i * 3;
	for (i = 0; i < VGA_FONT_BYTES; i++) font_memory[i] = i * 7;
	original[0] = misc;
	memcpy(original + VGA_SEQ_OFF, seq, sizeof(seq));
	memcpy(original + VGA_CRTC_OFF, crtc, sizeof(crtc));
	memcpy(original + VGA_GC_OFF, gc, sizeof(gc));
	memcpy(original + VGA_AC_OFF, ac, sizeof(ac));
	memcpy(original_text, text_memory, sizeof(original_text));
	memcpy(original_font, font_memory, sizeof(original_font));
	memcpy(original_palette, palette, sizeof(original_palette));

	for (cycle = 0; cycle < 3; cycle++) {
		CHECK(!vga_graphics_active(), "text mode has no graphics owner");
		CHECK(vga_enter_graphics() == 0, "enter graphics succeeds");
		CHECK(interrupts, "interrupts enabled after entry");
		CHECK(vga_graphics_active(), "VGA is owned during graphics");
		CHECK(display_enabled, "AC bit 5 enables the display");
		CHECK(!protected_writes, "CRTC timings written only after unlocking");
		CHECK(!memcmp(saved_regs, original, sizeof(original)), "save all indexed registers without corrupting AC");
		CHECK(!memcmp(crtc, mode13h + VGA_CRTC_OFF, sizeof(crtc)), "all graphics CRTC timings applied");
		CHECK(!memcmp(ac, mode13h + VGA_AC_OFF, sizeof(ac)), "all graphics AC registers applied");
		CHECK(!memcmp(palette, colors16, sizeof(colors16)), "direct DAC colors match desktop indexes");
		CHECK(dac_mask == 0xFF, "graphics DAC mask enables all colors");
		CHECK(vga_enter_graphics() == -1, "a second task cannot steal VGA");
		CHECK(vga_graphics_active() && interrupts, "failed acquisition leaves owner and interrupts intact");
		memset(text_memory, 0xCC, sizeof(text_memory));
		memset(font_memory, 0xDD, sizeof(font_memory));
		vga_leave_graphics();
		CHECK(display_enabled && !vga_graphics_active(), "text display re-enabled on exit");
		CHECK(interrupts, "interrupts enabled after exit");
		CHECK(misc == original[0], "text misc register restored");
		CHECK(!memcmp(seq, original + VGA_SEQ_OFF, sizeof(seq)), "text sequencer restored");
		CHECK(!memcmp(crtc, original + VGA_CRTC_OFF, sizeof(crtc)), "text timings and cursor restored");
		CHECK(!memcmp(gc, original + VGA_GC_OFF, sizeof(gc)), "text memory mapping restored");
		CHECK(!memcmp(ac, original + VGA_AC_OFF, sizeof(ac)), "text attribute palette restored");
		CHECK(!memcmp(text_memory, original_text, sizeof(text_memory)), "all three text consoles restored");
		CHECK(!memcmp(font_memory, original_font, sizeof(font_memory)), "BIOS font restored");
		CHECK(!bad_font_access, "font copied only with text/plane-2 addressing and IRQs masked");
		CHECK(!memcmp(palette, original_palette, sizeof(palette)), "entire text DAC palette restored");
		CHECK(dac_mask == 0x7F, "original DAC mask restored");
		vga_leave_graphics();
		CHECK(!vga_graphics_active() && interrupts, "extra leave is harmless");
	}
	printf("%s %d VGA UNIT CHECKS\n", failures ? "FAILED" : "PASSED", checks);
	return failures ? 1 : 0;
}
