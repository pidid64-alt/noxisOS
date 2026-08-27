#ifndef FB_H
#define FB_H

#include <stdint.h>

/* Linear-framebuffer text console (UEFI GOP / BIOS VBE via Multiboot v1).
 * fb_init parses the Multiboot info GRUB passes in EBX and, when it carries
 * pixel framebuffer info, switches kprint output from the legacy VGA text
 * buffer to pixel drawing. Call it before the first kprint. */

/* Returns 1 when a usable pixel framebuffer (24/32 bpp RGB) was found and
 * the console is now drawing into it; 0 keeps the legacy VGA text path. */
int fb_init(uint32_t magic, uint32_t mbi_addr);

int fb_active(void);

/* Same roles as their screen.c counterparts; screen.c dispatches to these. */
void fb_clear(void);
void fb_putchar(char c);
void fb_backspace(void);

/* Continue printing at the given 80x25-style cell coordinates (kprint_at). */
void fb_cursor_at(int col, int row);

#endif
