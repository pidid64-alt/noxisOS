#include "fb.h"
#include "font8x16.h"
#include "../libc/mem.h"
#include "../libc/string.h"
#include <stdint.h>

/* Multiboot v1 boot info: GRUB enters with EAX = 0x2BADB002 and EBX = MBI.
 * When the kernel header asked for video (flags bit 2) GRUB reports the mode
 * it left active through the framebuffer fields at fixed MBI offsets
 * (present when MBI flags bit 12 is set). */
#define MB_BOOT_MAGIC 0x2BADB002
#define MBI_FLAG_FB   (1u << 12)

#define MBI_FB_ADDR_LO 88   /* u64: low dword at 88, high dword at 92 */
#define MBI_FB_PITCH   96   /* u32 */
#define MBI_FB_WIDTH   100  /* u32 */
#define MBI_FB_HEIGHT  104  /* u32 */
#define MBI_FB_BPP     108  /* u8 */
#define MBI_FB_TYPE    109  /* u8 — right AFTER bpp, before color_info; reading
                               it from 112 (post-color_info) returned garbage */

/* Multiboot v1 framebuffer_type: 0 = indexed, 1 = direct RGB, 2 = EGA text. */
#define FB_TYPE_RGB 1

/* Cell geometry mirrors the legacy console so kprint_at(col,row) keeps its
 * meaning; cells outside the real resolution are simply off-screen. */
#define CELL_COLS_MAX 80
#define CELL_ROWS_MAX 25

static uint8_t  *fb;
static uint32_t  fb_pitch, fb_width, fb_height, fb_bpp;
static int       active;
static int       cur_col, cur_row;   /* current cell (FONT_W x FONT_H) */
static int       cursor_drawn;

int fb_active(void) { return active; }

/* One pixel, assuming the near-universal BGRx byte order GRUB's GOP/VBE
 * modes use (color_info positions 16/8/0). 24 bpp has no padding byte. */
static void put_pixel(int x, int y, uint32_t rgb)
{
    uint8_t *p = fb + (uint32_t)y * fb_pitch + (uint32_t)x * (fb_bpp / 8);
    p[0] = rgb & 0xff;         /* blue  */
    p[1] = (rgb >> 8) & 0xff;  /* green */
    p[2] = (rgb >> 16) & 0xff; /* red   */
    if (fb_bpp == 32) p[3] = 0;
}

#define FG 0xffffff   /* white on black, like WHITE_ON_BLACK in screen.h */
#define BG 0x000000

static void draw_glyph(int col, int row, char c, uint32_t fg, uint32_t bg)
{
    int x0 = col * FONT_W, y0 = row * FONT_H;
    const unsigned char *g = 0;
    if (c >= FONT_FIRST && c <= FONT_LAST)
        g = font8x16[c - FONT_FIRST];

    for (int ry = 0; ry < FONT_H; ry++) {
        unsigned char bits = g ? g[ry] : 0;
        for (int rx = 0; rx < FONT_W; rx++)
            put_pixel(x0 + rx, y0 + ry, (bits & (0x80 >> rx)) ? fg : bg);
    }
}

/* XOR-invert the cell's pixels: draw to show the cursor, draw again to hide.
 * Needs no shadow copy of the screen and survives any glyph underneath. */
static void cursor_xor(void)
{
    int x0 = cur_col * FONT_W, y0 = cur_row * FONT_H;
    for (int ry = 0; ry < FONT_H; ry++)
        for (int rx = 0; rx < FONT_W; rx++) {
            uint8_t *p = fb + (uint32_t)(y0 + ry) * fb_pitch
                             + (uint32_t)(x0 + rx) * (fb_bpp / 8);
            p[0] ^= 0xff; p[1] ^= 0xff; p[2] ^= 0xff;
        }
    cursor_drawn = !cursor_drawn;
}

static void hide_cursor(void)
{
    if (cursor_drawn) cursor_xor();
}

static void show_cursor(void)
{
    if (!cursor_drawn) cursor_xor();
}

static int cell_cols(void) { return (int)(fb_width  / FONT_W); }
static int cell_rows(void) { return (int)(fb_height / FONT_H); }

static void scroll_up(void)
{
    /* Move every row up by one cell, then blank the last cell row. */
    uint32_t row_bytes = fb_width * (fb_bpp / 8);
    for (int y = FONT_H; y < cell_rows() * FONT_H; y++)
        memory_copy(fb + (uint32_t)y * fb_pitch,
                    fb + (uint32_t)(y - FONT_H) * fb_pitch,
                    (int)row_bytes);
    for (int y = (cell_rows() - 1) * FONT_H; y < cell_rows() * FONT_H; y++)
        for (int x = 0; x < (int)fb_width; x++)
            put_pixel(x, y, BG);
}

void fb_clear(void)
{
    for (uint32_t y = 0; y < fb_height; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            put_pixel((int)x, (int)y, BG);
    cur_col = cur_row = 0;
    cursor_drawn = 0;
}

void fb_cursor_at(int col, int row)
{
    hide_cursor();
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    cur_col = col;
    cur_row = row;
}

void fb_backspace(void)
{
    hide_cursor();
    if (cur_col > 0) cur_col--;
    else if (cur_row > 0) { cur_row--; cur_col = cell_cols() - 1; }
    draw_glyph(cur_col, cur_row, ' ', FG, BG);
    show_cursor();
}

/* Print one character at the cursor and advance. '\n' wraps, the screen
 * scrolls when the bottom is reached. */
void fb_putchar(char c)
{
    hide_cursor();

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == 0x08) {
        fb_backspace();          /* re-shows the cursor */
        return;
    } else {
        draw_glyph(cur_col, cur_row, c, FG, BG);
        cur_col++;
        if (cur_col >= cell_cols()) { cur_col = 0; cur_row++; }
    }

    if (cur_row >= cell_rows()) {
        cur_row = cell_rows() - 1;
        scroll_up();
    }
    show_cursor();
}

/* TEMP DEBUG: debugcon (port 0xe9) trace of fb_init under GRUB/UEFI. */
static void dbg(const char *s) {
    for (const char *p = s; *p; p++)
        asm volatile("outb %%al, %%dx" :: "a"((uint8_t)*p), "d"((uint16_t)0xe9));
}

int fb_init(uint32_t magic, uint32_t mbi_addr)
{
    active = 0;
    dbg("FB: enter magic=");
    { char b[12]; hex_to_ascii((int)magic, b); dbg(b); dbg("\n"); }
    if (magic != MB_BOOT_MAGIC || mbi_addr == 0) { dbg("FB: no MBI\n"); return 0; }

    uint32_t flags = *(uint32_t *)mbi_addr;
    dbg("FB: flags=");
    { char b[12]; hex_to_ascii((int)flags, b); dbg(b); dbg("\n"); }
    if (!(flags & MBI_FLAG_FB)) { dbg("FB: no FB flag\n"); return 0; }

    uint32_t type = *(uint8_t *)(mbi_addr + MBI_FB_TYPE);
    uint32_t bpp  = *(uint8_t *)(mbi_addr + MBI_FB_BPP);
    uint32_t lo   = *(uint32_t *)(mbi_addr + MBI_FB_ADDR_LO);
    uint32_t pitch = *(uint32_t *)(mbi_addr + MBI_FB_PITCH);
    uint32_t w    = *(uint32_t *)(mbi_addr + MBI_FB_WIDTH);
    uint32_t h    = *(uint32_t *)(mbi_addr + MBI_FB_HEIGHT);

    dbg("FB: type=");
    { char b[12]; hex_to_ascii((int)type, b); dbg(b); }
    dbg(" bpp=");
    { char b[12]; hex_to_ascii((int)bpp, b); dbg(b); }
    dbg(" addr=");
    { char b[12]; hex_to_ascii((int)lo, b); dbg(b); }
    dbg(" w=");
    { char b[12]; int_to_ascii((int)w, b); dbg(b); }
    dbg(" h=");
    { char b[12]; int_to_ascii((int)h, b); dbg(b); dbg("\n"); }

    /* Only direct-RGB 24/32 bpp modes are renderable here; EGA text (type 2)
     * and indexed modes fall back to the legacy VGA-text console. */
    if (type != FB_TYPE_RGB || (bpp != 24 && bpp != 32)) { dbg("FB: bad type/bpp\n"); return 0; }
    if (lo == 0 || pitch == 0 || w < 8 * CELL_COLS_MAX || h < FONT_H * 2) { dbg("FB: bad geom\n"); return 0; }

    fb        = (uint8_t *)(unsigned long)lo;
    fb_pitch  = pitch;
    fb_width  = w;
    fb_height = h;
    fb_bpp    = bpp;

    fb_clear();
    active = 1;
    dbg("FB: ACTIVE\n");
    return 1;
}
