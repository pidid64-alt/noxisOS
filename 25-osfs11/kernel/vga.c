/* Shared VGA mode switching for TASK_GFX and TASK_DESKTOP.
 *
 * VGA register writes alone are not enough: AC bit 5 must re-enable the
 * display, CRTC timings must be unlocked, and graphics writes overwrite both
 * text characters and the font in plane 2. Keep all of that state together.
 */
#include "type.h"
#include "stdio.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "console.h"
#include "global.h"
#include "proto.h"
#include "vga.h"

#define VGA_N_SEQ  5
#define VGA_N_CRTC 25
#define VGA_N_GC   9
#define VGA_N_AC   21
#define VGA_SEQ_OFF  1
#define VGA_CRTC_OFF (VGA_SEQ_OFF + VGA_N_SEQ)
#define VGA_GC_OFF   (VGA_CRTC_OFF + VGA_N_CRTC)
#define VGA_AC_OFF   (VGA_GC_OFF + VGA_N_GC)
#define VGA_REG_BYTES (VGA_AC_OFF + VGA_N_AC)
#define VGA_FONT_BYTES (256 * 32) /* BIOS text font, bank 0 */
#define VGA_PALETTE_BYTES (256 * 3)

/* Sharing the back buffer also leaves room below the boot loader's kernel
 * file at 0x70000 for the saved console and font. */
PUBLIC u8 vga_framebuffer[GFX_FB_BYTES];
PRIVATE int graphics_active;
PRIVATE u8 saved_regs[VGA_REG_BYTES];
PRIVATE u8 saved_text[V_MEM_SIZE];
PRIVATE u8 saved_font[VGA_FONT_BYTES];
PRIVATE u8 saved_palette[VGA_PALETTE_BYTES];
PRIVATE u8 saved_dac_mask;

PRIVATE const u8 mode13h[VGA_REG_BYTES] = {
	0x63,
	0x03, 0x01, 0x0F, 0x00, 0x0E,
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28,
	0x40, 0x96, 0xB9, 0xA3, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Mode 13h indexes the DAC directly, unlike the BIOS text-mode AC palette. */
PRIVATE const u8 colors16[16 * 3] = {
	 0,  0,  0,   0,  0, 42,   0, 42,  0,   0, 42, 42,
	42,  0,  0,  42,  0, 42,  42, 21,  0,  42, 42, 42,
	21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
	63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63
};

PRIVATE void vga_save_regs(u8 *s)
{
	int i;
	s[0] = in_byte(VGA_MISC_R);
	for (i = 0; i < VGA_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		s[VGA_SEQ_OFF + i] = in_byte(VGA_SEQ_DATA);
	}
	for (i = 0; i < VGA_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		s[VGA_CRTC_OFF + i] = in_byte(VGA_CRTC_DATA);
	}
	for (i = 0; i < VGA_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		s[VGA_GC_OFF + i] = in_byte(VGA_GC_DATA);
	}
	for (i = 0; i < VGA_N_AC; i++) {
		/* Reading 0x3C1 does NOT reset the address/data flip-flop. */
		in_byte(VGA_AC_RDY);
		out_byte(VGA_AC_ADDR, i);
		s[VGA_AC_OFF + i] = in_byte(0x3C1);
	}
}

PRIVATE void vga_write_regs(const u8 *s)
{
	int i;
	out_byte(VGA_MISC_W, s[0]);
	for (i = 0; i < VGA_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		out_byte(VGA_SEQ_DATA, s[VGA_SEQ_OFF + i]);
	}

	/* BIOS text mode protects CRTC registers 0..7. Unlock BEFORE writing
	 * them, and restore the protection register LAST. */
	out_byte(VGA_CRTC_ADDR, 0x03);
	out_byte(VGA_CRTC_DATA, in_byte(VGA_CRTC_DATA) | 0x80);
	out_byte(VGA_CRTC_ADDR, 0x11);
	out_byte(VGA_CRTC_DATA, in_byte(VGA_CRTC_DATA) & 0x7F);
	for (i = 0; i < VGA_N_CRTC; i++) {
		if (i == 0x11)
			continue;
		out_byte(VGA_CRTC_ADDR, i);
		out_byte(VGA_CRTC_DATA, s[VGA_CRTC_OFF + i]);
	}
	out_byte(VGA_CRTC_ADDR, 0x11);
	out_byte(VGA_CRTC_DATA, s[VGA_CRTC_OFF + 0x11]);

	for (i = 0; i < VGA_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		out_byte(VGA_GC_DATA, s[VGA_GC_OFF + i]);
	}
	for (i = 0; i < VGA_N_AC; i++) {
		in_byte(VGA_AC_RDY);
		out_byte(VGA_AC_ADDR, i);
		out_byte(VGA_AC_ADDR, s[VGA_AC_OFF + i]);
	}
	/* Palette Address Source = 1: without this the monitor stays black. */
	in_byte(VGA_AC_RDY);
	out_byte(VGA_AC_ADDR, 0x20);
}

PRIVATE void vga_font_access(void)
{
	out_byte(VGA_SEQ_ADDR, 2);
	out_byte(VGA_SEQ_DATA, 0x04); /* write plane 2 only */
	out_byte(VGA_SEQ_ADDR, 4);
	out_byte(VGA_SEQ_DATA, 0x06); /* sequential, not chain-4 / odd-even */
	out_byte(VGA_GC_ADDR, 4);
	out_byte(VGA_GC_DATA, 0x02);  /* read plane 2 */
	out_byte(VGA_GC_ADDR, 5);
	out_byte(VGA_GC_DATA, 0x00);
	out_byte(VGA_GC_ADDR, 6);
	out_byte(VGA_GC_DATA, 0x04);  /* A0000, 64 KB */
}

PUBLIC int vga_graphics_active(void)
{
	return graphics_active;
}

PUBLIC int vga_enter_graphics(void)
{
	int i;
	disable_int();
	if (graphics_active) {
		enable_int();
		return -1;
	}
	graphics_active = 1;

	vga_save_regs(saved_regs);
	memcpy(saved_text, (void *)V_MEM_BASE, V_MEM_SIZE);
	saved_dac_mask = in_byte(VGA_DAC_MASK);
	out_byte(VGA_DAC_READ, 0);
	for (i = 0; i < VGA_PALETTE_BYTES; i++)
		saved_palette[i] = in_byte(VGA_DAC_DATA);
	vga_font_access();
	memcpy(saved_font, (void *)GFX_FB_BASE, VGA_FONT_BYTES);

	vga_write_regs(mode13h);
	out_byte(VGA_DAC_MASK, 0xFF);
	out_byte(VGA_DAC_WRITE, 0);
	for (i = 0; i < 16 * 3; i++)
		out_byte(VGA_DAC_DATA, colors16[i]);
	enable_int();
	return 0;
}

PUBLIC void vga_leave_graphics(void)
{
	int i;
	disable_int();
	if (graphics_active) {
		/* Leave graphics addressing (including CRTC double-word mode)
		 * before restoring the planar text font. */
		vga_write_regs(saved_regs);
		vga_font_access();
		memcpy((void *)GFX_FB_BASE, saved_font, VGA_FONT_BYTES);
		vga_write_regs(saved_regs);
		memcpy((void *)V_MEM_BASE, saved_text, V_MEM_SIZE);
		out_byte(VGA_DAC_WRITE, 0);
		for (i = 0; i < VGA_PALETTE_BYTES; i++)
			out_byte(VGA_DAC_DATA, saved_palette[i]);
		out_byte(VGA_DAC_MASK, saved_dac_mask);
		graphics_active = 0;
	}
	enable_int();
}
