#include "rtc.h"
#include "../cpu/ports.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include <stdint.h>

#define CMOS_COMMAND 0x70
#define CMOS_DATA    0x71

/* Read a single BCD register from the CMOS RTC at the given index. */
static uint8_t cmos_read(uint8_t reg) {
    port_byte_out(CMOS_COMMAND, reg);
    return port_byte_in(CMOS_DATA);
}

/* Convert a packed-BCD byte to binary: each nibble is a decimal digit. */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0xF));
}

void rtc_get_time(uint8_t *yr, uint8_t *mo, uint8_t *day,
                  uint8_t *h, uint8_t *m, uint8_t *s) {
    uint8_t sec1, sec2;

    /* Guard against reading across a tick boundary: sample seconds twice
     * and only proceed once two consecutive reads agree. */
    do {
        sec1 = bcd_to_bin(cmos_read(0x00));
        sec2 = bcd_to_bin(cmos_read(0x00));
    } while (sec1 != sec2);

    *s   = sec1;
    *m   = bcd_to_bin(cmos_read(0x02));
    *h   = bcd_to_bin(cmos_read(0x04));
    *day = bcd_to_bin(cmos_read(0x07));
    *mo  = bcd_to_bin(cmos_read(0x08));
    /* CMOS year register holds the low two digits; assume 2000+ epoch. */
    *yr  = (uint8_t)(2000 + bcd_to_bin(cmos_read(0x09)));
}

/* Print a value that should always occupy two digits, zero-padded
 * (e.g. 9 -> "09"). Used for month/day/hour/minute/second. */
static void kprint_2digit(uint8_t v) {
    char buf[3];
    if (v < 10) {
        buf[0] = '0';
        int_to_ascii(v, buf + 1);
        buf[2] = '\0';
    } else {
        int_to_ascii(v, buf);
    }
    kprint(buf);
}

void rtc_print_time(void) {
    uint8_t yr, mo, day, h, m, s;
    rtc_get_time(&yr, &mo, &day, &h, &m, &s);

    char buf[6];

    /* Year */
    int_to_ascii(yr, buf);
    kprint(buf);
    kprint("-");

    /* Month */
    kprint_2digit(mo);
    kprint("-");

    /* Day */
    kprint_2digit(day);
    kprint(" ");

    /* Hour */
    kprint_2digit(h);
    kprint(":");

    /* Minute */
    kprint_2digit(m);
    kprint(":");

    /* Second */
    kprint_2digit(s);
    kprint("\n");
}
