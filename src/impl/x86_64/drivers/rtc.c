#include "drivers/rtc.h"
#include "../lib/ports.h"
#include <stdint.h>

/* CMOS real-time clock, ports 0x70 (register select) / 0x71 (data). */
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_CENTURY  0x32  /* ACPI's usual century register; QEMU and most PCs have it */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

typedef struct { uint8_t sec, min, hour, day, mon, year, cent; } raw_time_t;

static void read_raw(raw_time_t *r) {
    while (update_in_progress()) {}
    r->sec  = cmos_read(RTC_SECONDS);
    r->min  = cmos_read(RTC_MINUTES);
    r->hour = cmos_read(RTC_HOURS);
    r->day  = cmos_read(RTC_DAY);
    r->mon  = cmos_read(RTC_MONTH);
    r->year = cmos_read(RTC_YEAR);
    r->cent = cmos_read(RTC_CENTURY);
}

static int raw_equal(const raw_time_t *a, const raw_time_t *b) {
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour && a->day == b->day &&
           a->mon == b->mon && a->year == b->year && a->cent == b->cent;
}

static uint8_t days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return (uint8_t)(mdays[month - 1] + (month == 2 && leap));
}

void rtc_add_minutes(rtc_time_t *t, int minutes) {
    int total = t->hour * 60 + t->minute + minutes;
    int day_shift = 0;
    while (total < 0) { total += 24 * 60; day_shift--; }
    while (total >= 24 * 60) { total -= 24 * 60; day_shift++; }
    t->hour = (uint8_t)(total / 60);
    t->minute = (uint8_t)(total % 60);

    for (; day_shift > 0; day_shift--) {
        if (++t->day > days_in_month(t->year, t->month)) {
            t->day = 1;
            if (++t->month > 12) { t->month = 1; t->year++; }
        }
    }
    for (; day_shift < 0; day_shift++) {
        if (--t->day < 1) {
            if (--t->month < 1) { t->month = 12; t->year--; }
            t->day = days_in_month(t->year, t->month);
        }
    }
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

int rtc_read(rtc_time_t *out) {
    /* The clock can tick over mid-read; repeat until two reads agree. */
    raw_time_t a, b;
    read_raw(&a);
    for (int tries = 0; tries < 10; tries++) {
        read_raw(&b);
        if (raw_equal(&a, &b)) break;
        a = b;
    }

    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int binary = status_b & 0x04;
    int hour24 = status_b & 0x02;
    int pm = b.hour & 0x80;
    uint8_t hour = (uint8_t)(b.hour & 0x7F);

    if (!binary) {
        b.sec = from_bcd(b.sec); b.min = from_bcd(b.min); hour = from_bcd(hour);
        b.day = from_bcd(b.day); b.mon = from_bcd(b.mon); b.year = from_bcd(b.year);
        b.cent = from_bcd(b.cent);
    }
    if (!hour24) {
        if (hour == 12) hour = 0;
        if (pm) hour = (uint8_t)(hour + 12);
    }

    uint16_t century = (b.cent >= 19 && b.cent <= 30) ? b.cent : 20;
    out->year = (uint16_t)(century * 100 + b.year);
    out->month = b.mon;
    out->day = b.day;
    out->hour = hour;
    out->minute = b.min;
    out->second = b.sec;

    if (out->month < 1 || out->month > 12 || out->day < 1 || out->day > 31 ||
        out->hour > 23 || out->minute > 59 || out->second > 59)
        return -1;
    return 0;
}
