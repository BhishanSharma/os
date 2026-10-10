// wifi.c - finding Wi-Fi adapters on the PCI bus
#include "drivers/wifi.h"
#include "drivers/pci.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdarg.h>

#define WIFI4  "Wi-Fi 4 (802.11n)"
#define WIFI5  "Wi-Fi 5 (802.11ac)"
#define WIFI6  "Wi-Fi 6 (802.11ax)"
#define WIFI6E "Wi-Fi 6E (802.11ax, 6 GHz)"
#define WIFI7  "Wi-Fi 7 (802.11be)"

typedef struct {
    uint16_t vendor, device;
    const char *model, *standard, *family;
    uint8_t firmware;
} chip_t;

/* Common laptop Wi-Fi chips. `family` is the Linux driver that runs them. */
static const chip_t chips[] = {
    // Intel: all need firmware (iwlwifi-*.ucode). CNVi ones are part of the chipset.
    {0x8086, 0x4232, "WiFi Link 5100",               WIFI4,  "iwlwifi", 1},
    {0x8086, 0x4235, "Ultimate N WiFi Link 5300",    WIFI4,  "iwlwifi", 1},
    {0x8086, 0x4236, "Ultimate N WiFi Link 5300",    WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0082, "Centrino Advanced-N 6205",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0085, "Centrino Advanced-N 6205",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0887, "Centrino Wireless-N 2230",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0888, "Centrino Wireless-N 2230",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x088E, "Centrino Advanced-N 6235",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x088F, "Centrino Advanced-N 6235",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0890, "Centrino Wireless-N 2200",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x0891, "Centrino Wireless-N 2200",     WIFI4,  "iwlwifi", 1},
    {0x8086, 0x08AE, "Centrino Wireless-N 100",      WIFI4,  "iwlwifi", 1},
    {0x8086, 0x08AF, "Centrino Wireless-N 100",      WIFI4,  "iwlwifi", 1},
    {0x8086, 0x08B1, "Wireless 7260",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x08B2, "Wireless 7260",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x08B3, "Wireless 3160",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x08B4, "Wireless 3160",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x095A, "Wireless 7265",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x095B, "Wireless 7265",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x3165, "Wireless 3165",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x3166, "Wireless 3165",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x24F3, "Wireless 8260",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x24F4, "Wireless 8260",                WIFI5,  "iwlwifi", 1},
    {0x8086, 0x24FB, "Wireless-AC 3168",             WIFI5,  "iwlwifi", 1},
    {0x8086, 0x24FD, "Wireless 8265 / 8275",         WIFI5,  "iwlwifi", 1},
    {0x8086, 0x2526, "Wireless-AC 9260",             WIFI5,  "iwlwifi", 1},
    {0x8086, 0x9DF0, "Wireless-AC 9560 (CNVi)",      WIFI5,  "iwlwifi", 1},
    {0x8086, 0xA370, "Wireless-AC 9560 (CNVi)",      WIFI5,  "iwlwifi", 1},
    {0x8086, 0x30DC, "Wireless-AC 9560 (CNVi)",      WIFI5,  "iwlwifi", 1},
    {0x8086, 0x31DC, "Wireless-AC 9560/9462/9461 (CNVi)", WIFI5, "iwlwifi", 1},
    {0x8086, 0x2723, "Wi-Fi 6 AX200",                WIFI6,  "iwlwifi", 1},
    {0x8086, 0x2729, "Wi-Fi 6 AX203",                WIFI6,  "iwlwifi", 1},
    {0x8086, 0x02F0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0x06F0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0x34F0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0x43F0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0x4DF0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0xA0F0, "Wi-Fi 6 AX201 (CNVi)",         WIFI6,  "iwlwifi", 1},
    {0x8086, 0x2725, "Wi-Fi 6E AX210",               WIFI6E, "iwlwifi", 1},
    {0x8086, 0x51F0, "Wi-Fi 6E AX211 (CNVi)",        WIFI6E, "iwlwifi", 1},
    {0x8086, 0x51F1, "Wi-Fi 6E AX211 (CNVi)",        WIFI6E, "iwlwifi", 1},
    {0x8086, 0x54F0, "Wi-Fi 6 AX201/AX211 (CNVi)",   WIFI6,  "iwlwifi", 1},
    {0x8086, 0x7A70, "Wi-Fi 6E AX211 (CNVi)",        WIFI6E, "iwlwifi", 1},
    {0x8086, 0x7AF0, "Wi-Fi 6E AX211 (CNVi)",        WIFI6E, "iwlwifi", 1},
    {0x8086, 0x7E40, "Wi-Fi 6E AX211 (CNVi)",        WIFI6E, "iwlwifi", 1},
    {0x8086, 0x272B, "Wi-Fi 7 BE200",                WIFI7,  "iwlwifi", 1},

    // Qualcomm Atheros: ath9k chips need no firmware; ath10k/ath11k/ath12k do.
    {0x168C, 0x002A, "AR928X",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x002B, "AR9285",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x002E, "AR9287",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x0030, "AR93xx",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x0032, "AR9485",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x0034, "AR9462",                       WIFI4,  "ath9k",   0},
    {0x168C, 0x0036, "QCA9565 / AR9565",             WIFI4,  "ath9k",   0},
    {0x168C, 0x003C, "QCA986x/988x",                 WIFI5,  "ath10k",  1},
    {0x168C, 0x003E, "QCA6174",                      WIFI5,  "ath10k",  1},
    {0x168C, 0x0041, "QCA6164",                      WIFI5,  "ath10k",  1},
    {0x168C, 0x0042, "QCA9377",                      WIFI5,  "ath10k",  1},
    {0x168C, 0x0046, "QCA9984",                      WIFI5,  "ath10k",  1},
    {0x168C, 0x0050, "QCA9887",                      WIFI5,  "ath10k",  1},
    {0x168C, 0x0056, "QCA9888",                      WIFI5,  "ath10k",  1},
    {0x17CB, 0x1101, "QCA6390",                      WIFI6,  "ath11k",  1},
    {0x17CB, 0x1103, "WCN6855 / QCNFA765",           WIFI6E, "ath11k",  1},
    {0x17CB, 0x1104, "QCN9074",                      WIFI6,  "ath11k",  1},
    {0x17CB, 0x1107, "WCN7850 / FastConnect 7800",   WIFI7,  "ath12k",  1},

    // Realtek: all need firmware.
    {0x10EC, 0x8176, "RTL8188CE",                    WIFI4,  "rtl8192ce", 1},
    {0x10EC, 0x8178, "RTL8192CE",                    WIFI4,  "rtl8192ce", 1},
    {0x10EC, 0x8179, "RTL8188EE",                    WIFI4,  "rtl8188ee", 1},
    {0x10EC, 0x818B, "RTL8192EE",                    WIFI4,  "rtl8192ee", 1},
    {0x10EC, 0x8723, "RTL8723AE",                    WIFI4,  "rtl8723ae", 1},
    {0x10EC, 0xB723, "RTL8723BE",                    WIFI4,  "rtl8723be", 1},
    {0x10EC, 0xD723, "RTL8723DE",                    WIFI4,  "rtw88",   1},
    {0x10EC, 0x8821, "RTL8821AE",                    WIFI5,  "rtl8821ae", 1},
    {0x10EC, 0xB822, "RTL8822BE",                    WIFI5,  "rtw88",   1},
    {0x10EC, 0xC821, "RTL8821CE",                    WIFI5,  "rtw88",   1},
    {0x10EC, 0xC822, "RTL8822CE",                    WIFI5,  "rtw88",   1},
    {0x10EC, 0x8852, "RTL8852AE",                    WIFI6,  "rtw89",   1},
    {0x10EC, 0xB852, "RTL8852BE",                    WIFI6,  "rtw89",   1},
    {0x10EC, 0xC852, "RTL8852CE",                    WIFI6E, "rtw89",   1},

    // MediaTek: all need firmware.
    {0x14C3, 0x7630, "MT7630E",                      WIFI4,  "mt76x0e", 1},
    {0x14C3, 0x7610, "MT7610E",                      WIFI5,  "mt76x0e", 1},
    {0x14C3, 0x7662, "MT7612E",                      WIFI5,  "mt76x2e", 1},
    {0x14C3, 0x7961, "MT7921",                       WIFI6,  "mt7921e", 1},
    {0x14C3, 0x0608, "MT7921K (RZ608)",              WIFI6E, "mt7921e", 1},
    {0x14C3, 0x0616, "MT7922 (RZ616)",               WIFI6E, "mt7921e", 1},
    {0x14C3, 0x7922, "MT7922",                       WIFI6E, "mt7921e", 1},
    {0x14C3, 0x7925, "MT7925",                       WIFI7,  "mt7925e", 1},

    // Broadcom
    {0x14E4, 0x4727, "BCM4313",                      WIFI4,  "brcmsmac", 0},
    {0x14E4, 0x4353, "BCM43224",                     WIFI4,  "brcmsmac", 0},
    {0x14E4, 0x4357, "BCM43225",                     WIFI4,  "brcmsmac", 0},
    {0x14E4, 0x4359, "BCM43228",                     WIFI4,  "bcma/wl", 1},
    {0x14E4, 0x4331, "BCM4331",                      WIFI4,  "b43",     1},
    {0x14E4, 0x4365, "BCM43142",                     WIFI4,  "wl (closed)", 1},
    {0x14E4, 0x43B1, "BCM4352",                      WIFI5,  "wl (closed)", 1},
    {0x14E4, 0x43A0, "BCM4360",                      WIFI5,  "wl (closed)", 1},
    {0x14E4, 0x43A3, "BCM4350",                      WIFI5,  "brcmfmac", 1},
    {0x14E4, 0x43BA, "BCM43602",                     WIFI5,  "brcmfmac", 1},
    {0x14E4, 0x43EC, "BCM4356",                      WIFI5,  "brcmfmac", 1},
};

static wifi_adapter_t adapters[WIFI_MAX_ADAPTERS];
static int adapter_count;
static int scanned;
static int usb_controllers, xhci_controllers, virtual_machine;

static const chip_t *find_chip(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < sizeof(chips) / sizeof(chips[0]); i++)
        if (chips[i].vendor == vendor && chips[i].device == device) return &chips[i];
    return 0;
}

int wifi_detect(void) {
    static pci_device_t devs[64];
    int n = pci_scan(devs, 64);
    adapter_count = usb_controllers = xhci_controllers = virtual_machine = 0;
    for (int i = 0; i < n; i++) {
        pci_device_t *d = &devs[i];
        if (d->class_code == 0x0C && d->subclass == 0x03) {
            usb_controllers++;
            if (d->prog_if == 0x30) xhci_controllers++;
        }
        // QEMU, virtio, VMware, VirtualBox devices: a virtual machine.
        if (d->vendor == 0x1234 || d->vendor == 0x1AF4 || d->vendor == 0x1B36 ||
            d->vendor == 0x15AD || d->vendor == 0x80EE)
            virtual_machine = 1;

        const chip_t *chip = find_chip(d->vendor, d->device);
        int wireless_class = (d->class_code == 0x02 && d->subclass == 0x80) ||
                             (d->class_code == 0x0D && d->subclass != 0x11);   // not Bluetooth
        if (!chip && !wireless_class) continue;
        if (adapter_count >= WIFI_MAX_ADAPTERS) continue;

        wifi_adapter_t *a = &adapters[adapter_count++];
        memset(a, 0, sizeof(*a));
        a->pci = *d;
        a->vendor = pci_vendor_name(d->vendor);
        if (chip) {
            a->model = chip->model;
            a->standard = chip->standard;
            a->family = chip->family;
            a->needs_firmware = chip->firmware;
        }
        kprintf("Wi-Fi: %04x:%04x at %02x:%02x.%x, class %02x%02x\n", d->vendor, d->device,
                d->bus, d->slot, d->func, d->class_code, d->subclass);
    }
    scanned = 1;
    return adapter_count;
}

int wifi_count(void) {
    return adapter_count;
}

const wifi_adapter_t *wifi_get(int index) {
    return index >= 0 && index < adapter_count ? &adapters[index] : 0;
}

void wifi_describe(const wifi_adapter_t *a, char *out, uint32_t size) {
    if (a->model)
        k_snprintf(out, size, "%s %s", a->vendor ? a->vendor : "", a->model);
    else if (a->vendor)
        k_snprintf(out, size, "%s wireless chip %04x:%04x", a->vendor, a->pci.vendor, a->pci.device);
    else
        k_snprintf(out, size, "wireless chip %04x:%04x", a->pci.vendor, a->pci.device);
}

static void row(const char *label, const char *fmt, ...) {
    char padded[16];
    k_snprintf(padded, sizeof(padded), "  %-11s", label);
    print_accent(padded);
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
    print_str("\n");
}

void wifi_print_status(void) {
    if (!scanned) wifi_detect();
    if (adapter_count == 0) {
        print_str("No Wi-Fi adapter found on the PCI bus.\n");
        if (virtual_machine)
            print_str("This is a virtual machine: they do not emulate Wi-Fi cards. The host's Wi-Fi\n"
                      "reaches this OS through the emulated Ethernet card (see `ifconfig`).\n");
        else
            print_str("If the laptop has Wi-Fi, check that it is enabled in the firmware (BIOS) setup.\n");
        if (usb_controllers)
            kprintf("A USB Wi-Fi stick would not show up yet: there is no USB driver (%d USB controller%s found%s).\n",
                    usb_controllers, usb_controllers == 1 ? "" : "s", xhci_controllers ? ", xHCI" : "");
        return;
    }
    for (int i = 0; i < adapter_count; i++) {
        const wifi_adapter_t *a = &adapters[i];
        char name[80];
        wifi_describe(a, name, sizeof(name));
        kprintf("Wi-Fi adapter %d: ", i + 1);
        print_highlight(name);
        print_str("\n");
        if (a->standard) row("Standard", "%s", a->standard);
        row("PCI", "%02x:%02x.%x, ID %04x:%04x, board %04x:%04x, rev %02x", a->pci.bus, a->pci.slot,
            a->pci.func, a->pci.vendor, a->pci.device, a->pci.sub_vendor, a->pci.sub_device, a->pci.revision);
        if (a->pci.bar0) row("Registers", "0x%lx", a->pci.bar0);
        if (a->family)
            row("Driver", "none yet (on Linux: %s%s)", a->family,
                a->needs_firmware ? ", which loads a firmware file into the chip" : ", no firmware needed");
        else
            row("Driver", "none yet (chip not in this OS's table)");
        row("Status", "detected; scanning and connecting need a driver for this chip");
    }
}
