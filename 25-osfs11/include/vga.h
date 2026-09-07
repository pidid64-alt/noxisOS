#ifndef _NOXIS_VGA_H_
#define _NOXIS_VGA_H_

/* Include type.h and const.h first, like the other kernel driver headers. */

/* The graphics double buffer. It lives in free RAM (see GFX_FB_RAM in
 * const.h) instead of the kernel image, which must stay below the loader's
 * staging area at 0x70000. The same buffer is shared by the demo task and
 * the desktop task; only one of them may own the screen at a time. */
extern u8 *vga_framebuffer;

PUBLIC int vga_enter_graphics(void); /* 0 on success, -1 if already owned */
PUBLIC void vga_leave_graphics(void);
PUBLIC int vga_graphics_active(void);

/* Copy the double buffer through the banked VBE aperture (one full frame). */
PUBLIC void vga_blit(void);

#endif /* _NOXIS_VGA_H_ */
