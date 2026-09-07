/* Shared VGA mode switching for TASK_GFX and TASK_DESKTOP.
 *
 * The desktop/demo run in 800x600x8 with a linear framebuffer, entered
 * through the Bochs/QEMU VBE (DISPI) interface: no BIOS call is needed in
 * protected mode -- just two 16-bit ports (0x1CE/0x1CF). The text-mode
 * state (indexed registers, three console pages, the BIOS font and the DAC)
 * is saved on entry and restored on exit so the text consoles keep working
 * after the GUI closes.
 *
 * The linear framebuffer lives at GFX_FB_LFB (0xE0000000 on the emulated
 * "std" VGA card). RAM is identity-mapped by the loader, so vga_map_lfb()
 * adds one 4 MB page-table entry for that window at boot (ring 0 only).
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

/* The double buffer is placed in free RAM (see GFX_FB_RAM in const.h): an
 * 800x600 buffer no longer fits inside the kernel image, which must stay
 * below the loader's staging area at 0x70000. */
PUBLIC u8 *vga_framebuffer = (u8 *)GFX_FB_RAM;
PRIVATE int graphics_active;
PRIVATE u8 saved_regs[VGA_REG_BYTES];
PRIVATE u8 saved_text[V_MEM_SIZE];
PRIVATE u8 saved_font[VGA_FONT_BYTES];
PRIVATE u8 saved_palette[VGA_PALETTE_BYTES];
PRIVATE u8 saved_dac_mask;

/* The 8-bit VBE mode indexes the DAC directly (like mode 13h did), so the
 * first sixteen entries are set to the standard VGA palette the window
 * manager draws with. */
PRIVATE const u8 colors16[16 * 3] = {
	 0,  0,  0,   0,  0, 42,   0, 42,  0,   0, 42, 42,
	42,  0,  0,  42,  0, 42,  42, 21,  0,  42, 42, 42,
	21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
	63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63
};

/* Bochs/QEMU VBE (DISPI) interface -------------------------------------
 * The emulated "std" VGA card implements the Bochs VBE extension: write an
 * index word to 0x1CE, then the value word to 0x1CF. Mode changes are done
 * by the card itself, so no CRTC/sequencer tables are needed. */
#define VBE_DISPI_IOPORT_INDEX   0x1CE
#define VBE_DISPI_IOPORT_DATA    0x1CF
#define VBE_DISPI_INDEX_ID       0x0
#define VBE_DISPI_INDEX_XRES     0x1
#define VBE_DISPI_INDEX_YRES     0x2
#define VBE_DISPI_INDEX_BPP      0x3
#define VBE_DISPI_INDEX_ENABLE   0x4
#define VBE_DISPI_DISABLED       0x00
#define VBE_DISPI_ENABLED        0x01
#define VBE_DISPI_LFB_ENABLED    0x40
#define VBE_DISPI_BPP_8          8

PRIVATE void vbe_write(u16 index, u16 value)
{
	out_word(VBE_DISPI_IOPORT_INDEX, index);
	out_word(VBE_DISPI_IOPORT_DATA, value);
}

/* Switch the card to GFX_FB_W x GFX_FB_H, 8 bpp, linear framebuffer. */
PRIVATE void vbe_enter_mode(void)
{
	vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	vbe_write(VBE_DISPI_INDEX_XRES, GFX_FB_W);
	vbe_write(VBE_DISPI_INDEX_YRES, GFX_FB_H);
	vbe_write(VBE_DISPI_INDEX_BPP, VBE_DISPI_BPP_8);
	vbe_write(VBE_DISPI_INDEX_ENABLE,
	          VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
}

/* Leave the VBE mode: the card falls back to the standard VGA mode whose
 * registers were saved on entry (BIOS text mode). */
PRIVATE void vbe_leave_mode(void)
{
	vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

/* The loader identity-maps RAM but not the VBE linear framebuffer window at
 * GFX_FB_LFB (0xE0000000). Add one 4 MB big-page entry to the page
 * directory -- the loader keeps it at 0x100000 -- and enable CR4.PSE so the
 * PS bit is honoured. Ring 0 only; call once at boot (kernel_main). */
#define VBE_LFB_PDE_ADDR (0x100000 + ((GFX_FB_LFB >> 22) * 4))

PUBLIC void vga_map_lfb(void)
{
#if defined(__i386__)
	disable_int();
	__asm__ __volatile__("movl %%cr4, %%eax\n\t"
	                     "orl $0x10, %%eax\n\t"  /* CR4.PSE */
	                     "movl %%eax, %%cr4"
	                     ::: "eax", "memory");
	/* P | RW | US | PS: present, writable, user, 4 MB page. */
	*(u32 *)VBE_LFB_PDE_ADDR = GFX_FB_LFB | 0x87;
	/* Flush the TLB so the new PDE takes effect. */
	__asm__ __volatile__("movl %%cr3, %%eax\n\t"
	                     "movl %%eax, %%cr3"
	                     ::: "eax", "memory");
	enable_int();
#else
	/* Host build (unit tests): no CR4/CR3 access on the test machine.
	 * The real mapping is ring-0-only and happens once in kernel_main. */
	(void)VBE_LFB_PDE_ADDR;
#endif
}

PUBLIC void vga_blit(void)
{
	memcpy((void *)GFX_FB_LFB, vga_framebuffer, GFX_FB_BYTES);
}

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

	/* 800x600x8 with a linear framebuffer (Bochs/QEMU VBE). */
	vbe_enter_mode();
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
		/* Drop the VBE mode first (the card reverts to the standard
		 * VGA mode whose registers were saved on entry), then restore
		 * the planar text font and the console pages. */
		vbe_leave_mode();
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
