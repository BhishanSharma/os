#ifndef RTC_H
#define RTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;   /* e.g. 2026 */
    uint8_t month;   /* 1-12 */
    uint8_t day;     /* 1-31 */
    uint8_t hour;    /* 0-23 */
    uint8_t minute;  /* 0-59 */
    uint8_t second;  /* 0-59 */
} rtc_time_t;

/* Read the CMOS real-time clock (assumed to hold UTC, as QEMU's does by default).
 * Returns 0 on success, -1 if the clock holds an impossible value. */
int rtc_read(rtc_time_t *out);

#ifdef __cplusplus
}
#endif

#endif
