#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* Read the current date/time from the CMOS real-time clock.
 * All returned values are in binary (already converted from BCD).
 * Year is the full 4-digit year (e.g. 2026). */
void rtc_get_time(uint8_t *yr, uint8_t *mo, uint8_t *day,
                  uint8_t *h, uint8_t *m, uint8_t *s);

/* Print the current time as "YYYY-MM-DD HH:MM:SS" followed by a newline. */
void rtc_print_time(void);

#endif
