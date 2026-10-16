// logd - keeps the system log on the boot stick (like macOS's syslogd)
//
// Everything printed since power-on (the kernel's messages, programs' output,
// crash reports) is in the kernel's log ring (syslog_read). Every few seconds
// logd adds what is new to /USB/LOGS/BOOTnnnn.LOG, one file per boot; the last
// LOGS_KEPT boots are kept. scripts/usb-logs.ps1 reads them in Windows.
//
// Started by launchd (LAUNCHD.CFG). Without a boot stick at /USB it ends.
#include <stdio.h>
#include <string.h>
#include <os.h>

#define DIR "/USB/LOGS"
#define LOGS_KEPT 10
#define FILE_MAX (1024 * 1024)       /* one boot's file at most: each save rewrites the file */
#define EVERY_MS 3000

static int number_of(const char *name) {    /* "BOOT0042.LOG" -> 42, else -1 */
    if (strncmp(name, "BOOT", 4) && strncmp(name, "boot", 4)) return -1;
    int n = 0, digits = 0;
    for (const char *p = name + 4; *p >= '0' && *p <= '9'; p++, digits++) n = n * 10 + (*p - '0');
    return digits == 4 ? n : -1;
}

int main(void) {
    struct os_stat st;
    if (stat("/USB", &st) != 0) return 0;    /* not started from a stick (or no driver for it) */
    mkdir(DIR);

    /* The next number; the old files beyond the last LOGS_KEPT go. */
    static struct os_dirent list[128];
    int n = listdir(DIR, list, 128), last = 0;
    for (int i = 0; i < n; i++) {
        int k = number_of(list[i].name);
        if (k > last) last = k;
    }
    int me = last + 1 > 9999 ? 1 : last + 1;
    for (int i = 0; i < n; i++) {
        int k = number_of(list[i].name);
        if (k >= 0 && k <= me - LOGS_KEPT) {
            char old[64];
            snprintf(old, sizeof(old), DIR "/%s", list[i].name);
            unlink(old);
        }
    }
    char path[64];
    snprintf(path, sizeof(path), DIR "/BOOT%04d.LOG", me);
    printf("logd: saving the log to %s\n", path);

    static char buf[64 * 1024];
    unsigned long from = 0, written = 0;
    for (;;) {
        int got;
        while ((got = syslog_read(&from, buf, sizeof(buf))) > 0) {
            if (written + (unsigned long)got > FILE_MAX) {
                static const char full[] = "\n[logd: the log is full for this boot]\n";
                int fd = open(path, OPEN_APPEND);
                if (fd >= 0) {
                    write(fd, full, sizeof(full) - 1);
                    close(fd);
                }
                return 0;
            }
            int fd = open(path, OPEN_APPEND);
            if (fd < 0) return 1;            /* the stick went away */
            write(fd, buf, (size_t)got);
            close(fd);                       /* saved to the stick here */
            written += (unsigned long)got;
        }
        sleep_ms(EVERY_MS);
    }
}
