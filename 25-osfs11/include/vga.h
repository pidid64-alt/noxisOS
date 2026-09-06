#ifndef _NOXIS_VGA_H_
#define _NOXIS_VGA_H_

/* Include type.h and const.h first, like the other kernel driver headers. */
extern u8 vga_framebuffer[GFX_FB_BYTES];

/* A single VGA device is shared by the demo and the desktop. */
PUBLIC int vga_enter_graphics(void); /* 0 on success, -1 if already owned */
PUBLIC void vga_leave_graphics(void);
PUBLIC int vga_graphics_active(void);

#endif
