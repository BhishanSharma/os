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

/* Read the CMOS real-time clock, as UTC. Returns 0 on success, -1 if the
 * clock holds an impossible value. */
int rtc_read(rtc_time_t *out);

/* Does the hardware clock hold local time (as Windows keeps it) rather than
 * UTC (Linux, VMs)? Decided at first use: local on real PCs, UTC in a VM;
 * `clock local` / `clock utc` change it. */
int rtc_is_local(void);
void rtc_set_local(int local);

/* Local time zone used for display. The RTC itself stays in UTC (TLS needs it). */
#define RTC_LOCAL_OFFSET_MIN 330   /* UTC+05:30 */
#define RTC_LOCAL_TZ_NAME    "IST"

/* Shift a time by a number of minutes (may be negative), rolling over days,
 * months and years. */
void rtc_add_minutes(rtc_time_t *t, int minutes);

#ifdef __cplusplus
}
#endif

#endif
