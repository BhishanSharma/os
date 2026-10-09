// sysinfo.h - OS identity, boot facts, the status bar and the `sysinfo` command
#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdint.h>

#define OS_NAME     "Terminal OS"
#define OS_VERSION  "0.7"
#define OS_HOSTNAME "terminal-os"

/* Facts only kernel_main knows, recorded during boot for `sysinfo`. */
typedef struct {
    int uefi;                          /* booted through UEFI firmware (else BIOS) */
    uint32_t fb_width, fb_height;      /* 0 in VGA text mode */
    uint32_t fb_bpp;
} boot_info_t;
extern boot_info_t boot_info;

/* The "TERMINAL OS" banner, centred, in the theme's accent colour. */
void sysinfo_print_logo(void);

/* CPU model from CPUID (e.g. "AMD Ryzen 3 3250U with Radeon Graphics"). */
const char *sysinfo_cpu_name(void);

/* Redraw the status bar (clock, IP, memory, disk). Cheap to call often: it
 * only redraws when the second changes, unless `force`. */
void statusbar_update(int force);

/* `sysinfo`: neofetch-style summary of the machine. */
void sysinfo_print(void);

#endif
