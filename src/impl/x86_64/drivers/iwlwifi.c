// iwlwifi.c - Intel Wi-Fi 6 "So" family (AX101 / AX201 / AX211 on CNVi)
//
// Bring-up, as OpenBSD's iwx(4) does it for the AX210 device family:
//   1. PCI: memory decoding, bus mastering; legacy interrupts off (polled).
//   2. Card: "prepare" handshake, software reset, power-management init
//      (APM) until the MAC clock is ready.
//   3. Memory the card reads: the firmware sections (copied out of the
//      firmware file), the image loader (IML), a receive queue (free and
//      used descriptors, 4 KiB buffers, a status word), the command queue,
//      and the "context info" + "peripheral scratch" blocks that point at
//      all of that.
//   4. Tell the card where the context info is, and let its boot ROM run:
//      it loads the firmware by itself, starts it, and the firmware sends
//      an ALIVE notification through the receive queue.
// Everything is polled from the CSR_INT register and the receive queue's
// status word; no interrupt is needed.
#include "drivers/iwlwifi.h"
#include "drivers/wifi.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/fat32.h"
#include "sys/vfs.h"
#include "drivers/msi.h"
#include "drivers/nic.h"
#include "net/net.h"
#include "net/wpa.h"
#include "bearssl.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define FIRMWARE_PATH "/AX101.FW"     /* iwlwifi-so-a0-hr-b0-77.ucode */

#define PAGE_PCD 0x10
#define PAGE_PWT 0x08

/* ---- Registers (CSR: control/status, at the start of BAR 0) ---------------- */

#define CSR_HW_IF_CONFIG        0x000
#define CSR_INT_COALESCING      0x004
#define CSR_INT                 0x008
#define CSR_INT_MASK            0x00C
#define CSR_FH_INT_STATUS       0x010
#define CSR_RESET               0x020
#define CSR_GP_CNTRL            0x024
#define CSR_HW_REV              0x028
#define CSR_GIO                 0x03C
#define CSR_UCODE_DRV_GP1_CLR   0x05C
#define CSR_MBOX_SET            0x088
#define CSR_HW_RF_ID            0x09C
#define CSR_MAC_SHADOW_REG_CTRL 0x0A8
#define CSR_GIO_CHICKEN         0x100
#define CSR_CTXT_INFO_ADDR      0x118
#define CSR_IML_DATA_ADDR       0x120
#define CSR_IML_SIZE            0x128
#define CSR_IML_RESP            0x12C
#define CSR_DBG_HPET_MEM        0x240
#define CSR_DBG_LINK_PWR_MGMT   0x250

#define HBUS_PRPH_WADDR         0x444    /* peripheral (internal) registers */
#define HBUS_PRPH_RADDR         0x448
#define HBUS_PRPH_WDAT          0x44C
#define HBUS_PRPH_RDAT          0x450
#define RFH_Q0_FRBDCB_WIDX_TRG  0x1C80   /* free receive buffers: write index */

#define HW_IF_NIC_READY         0x00400000
#define HW_IF_PREPARE           0x08000000
#define HW_IF_HAP_WAKE_L1A      0x00080000
#define HW_IF_AUTO_FUNC_BOOT    0x00000002   /* CSR_CTXT_INFO_BOOT_CTRL is this register */

#define GP_MAC_CLOCK_READY      0x00000001
#define GP_INIT_DONE            0x00000004
#define GP_MAC_ACCESS_REQ       0x00000008
#define GP_GOING_TO_SLEEP       0x00000010
#define GP_RFKILL_WAKE_L1A_EN   0x04000000
#define GP_HW_RF_KILL_SW        0x08000000   /* set = radio allowed */

#define RESET_SW_RESET          0x00000080

#define INT_ALIVE               (1u << 0)
#define INT_RF_KILL             (1u << 7)
#define INT_SW_RX               (1u << 3)
#define INT_SW_ERR              (1u << 25)
#define INT_FH_TX               (1u << 27)
#define INT_HW_ERR              (1u << 29)
#define INT_FH_RX               (1u << 31)
#define FH_INT_RX_MASK          ((1u << 30) | (1u << 17) | (1u << 16))

/* Peripheral registers; UMAC ones are 0x300000 further on this family. */
#define UMAC_PRPH_OFFSET        0x300000
#define UREG_CHICK              0xA05C00
#define UREG_CHICK_MSI_ENABLE   (1u << 24)
#define UREG_CPU_INIT_RUN       0xA05C44

/* ---- Memory structures the card reads (little-endian, packed) ------------- */

#define MAX_DRAM_ENTRY 64

typedef struct {
    uint64_t umac_img[MAX_DRAM_ENTRY];
    uint64_t lmac_img[MAX_DRAM_ENTRY];
    uint64_t virtual_img[MAX_DRAM_ENTRY];
} __attribute__((packed)) ctxt_dram_t;

typedef struct {
    struct { uint16_t mac_id, version, size, reserved; } __attribute__((packed)) version;
    struct { uint32_t control_flags, reserved; } __attribute__((packed)) control;
    struct { uint64_t base; uint32_t size, reserved; } __attribute__((packed)) pnvm;
    struct { uint64_t base; uint32_t size, debug_token; } __attribute__((packed)) hwm;
    struct { uint64_t free_rbd_addr; uint32_t reserved; } __attribute__((packed)) rbd;
    struct { uint64_t base; uint32_t size, reserved; } __attribute__((packed)) reduce_power;
} __attribute__((packed)) prph_ctrl_t;

typedef struct {
    prph_ctrl_t ctrl;
    uint32_t reserved[12];
    ctxt_dram_t dram;
} __attribute__((packed)) prph_scratch_t;

typedef struct {
    uint16_t version, size;
    uint32_t config;
    uint64_t prph_info_base_addr;
    uint64_t cr_head_idx_arr_base_addr;
    uint64_t tr_tail_idx_arr_base_addr;
    uint64_t cr_tail_idx_arr_base_addr;
    uint64_t tr_head_idx_arr_base_addr;
    uint16_t cr_idx_arr_size, tr_idx_arr_size;
    uint64_t mtr_base_addr;
    uint64_t mcr_base_addr;
    uint16_t mtr_size, mcr_size;
    uint16_t mtr_doorbell_vec, mcr_doorbell_vec;
    uint16_t mtr_msi_vec, mcr_msi_vec;
    uint8_t mtr_opt_header_size, mtr_opt_footer_size;
    uint8_t mcr_opt_header_size, mcr_opt_footer_size;
    uint16_t msg_rings_ctrl_flags;
    uint16_t prph_info_msi_vec;
    uint64_t prph_scratch_base_addr;
    uint32_t prph_scratch_size;
    uint32_t reserved;
} __attribute__((packed)) ctxt_info_gen3_t;

#define PRPH_SCRATCH_RB_SIZE_4K  (1u << 16)
#define PRPH_SCRATCH_MTR_MODE    (1u << 17)
#define PRPH_MTR_FORMAT_256B     0xC0000u

typedef struct { uint16_t rbid; uint16_t reserved[3]; uint64_t addr; } __attribute__((packed)) rx_free_desc_t;
typedef struct { uint32_t reserved1; uint16_t rbid; uint8_t flags; uint8_t reserved2[25]; } __attribute__((packed)) rx_used_desc_t;

typedef struct { uint16_t len; uint64_t addr; } __attribute__((packed)) tfh_tb_t;
typedef struct { uint16_t num_tbs; tfh_tb_t tbs[25]; uint32_t pad; } __attribute__((packed)) tfh_tfd_t;

#define CMD_SLOT_SIZE_EARLY 4096
#define RX_COUNT    512                  /* receive buffers (log2 = 9) */
#define RX_BUF_SIZE 4096
#define TX_COUNT    256                  /* command queue entries (log2 = 8) */

/* Firmware file */
#define TLV_MAGIC          0x0A4C5749
#define TLV_SEC_RT         19
#define TLV_FW_VERSION     36
#define TLV_IML            52
#define SEP_CPU1_CPU2      0xFFFFCCCCu
#define SEP_PAGING         0xAAAABBBBu
#define MAX_SECTIONS       64

typedef struct {
    uint32_t devoff;
    const uint8_t *data;
    uint32_t len;
} fw_section_t;

/* ---- State ----------------------------------------------------------------- */

static int quiet;                          /* 1: bring-up progress is not printed */
#define say(...) do { if (!quiet) kprintf(__VA_ARGS__); } while (0)


static volatile uint8_t *regs;
static uint8_t *fw_file;
static uint32_t fw_size;
static fw_section_t sections[MAX_SECTIONS];
static int section_count;
static const uint8_t *iml;
static uint32_t iml_len;
static uint32_t fw_ver[3];
static uint32_t fw_api[4], fw_capa[4];       /* API / capability bit sets */
static const uint8_t *fw_cmd_versions;       /* {cmd, group, cmd_ver, notif_ver} entries */
static uint32_t fw_cmd_versions_len, fw_n_scan_channels;

static ctxt_info_gen3_t *ctxt;
static prph_scratch_t *scratch;
static uint8_t *prph_info;
static rx_free_desc_t *rx_free;
static rx_used_desc_t *rx_used;
static volatile uint16_t *rx_status;
static uint8_t *rx_bufs;
static tfh_tfd_t *tx_cmd;
static uint8_t *cmd_bufs;                  /* one 4 KiB buffer per command queue slot */
static uint16_t cmd_hw;                    /* command queue write index (wraps at 65536) */
static int rx_cur;
static int nic_locks;

/* ---- Low level --------------------------------------------------------------- */

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(regs + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static void wr8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(regs + off) = v; }
static void setbits(uint32_t off, uint32_t b) { wr(off, rd(off) | b); }
static void clrbits(uint32_t off, uint32_t b) { wr(off, rd(off) & ~b); }

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t tsc_per_us;

/* Microsecond delays from the TSC, measured once against the timer. */
static void udelay(uint32_t us) {
    if (!tsc_per_us) {
        uint32_t t = get_tick();
        while (get_tick() == t) __asm__ volatile("pause");
        uint64_t a = rdtsc();
        t = get_tick();
        while ((uint32_t)(get_tick() - t) < 5) __asm__ volatile("pause");
        tsc_per_us = (rdtsc() - a) / (5 * 1000000 / TIMER_FREQ);
        if (!tsc_per_us) tsc_per_us = 1;
    }
    uint64_t end = rdtsc() + tsc_per_us * us;
    while (rdtsc() < end) __asm__ volatile("pause");
}

/* Wait until (reg & mask) == (bits & mask); timeout in microseconds. */
static int poll_bit(uint32_t reg, uint32_t bits, uint32_t mask, uint32_t timeout) {
    for (;;) {
        if ((rd(reg) & mask) == (bits & mask)) return 1;
        if (timeout < 10) return 0;
        timeout -= 10;
        udelay(10);
    }
}

/* Keep the card's MAC awake while touching internal registers. */
static int nic_lock(void) {
    if (nic_locks++) return 1;
    setbits(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    udelay(2);
    if (poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP, 150000)) return 1;
    nic_locks--;
    clrbits(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    return 0;
}

static void nic_unlock(void) {
    if (nic_locks > 0 && --nic_locks == 0) clrbits(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
}

static void write_prph(uint32_t addr, uint32_t v) {
    wr(HBUS_PRPH_WADDR, (addr & 0x00FFFFFF) | (3u << 24));
    wr(HBUS_PRPH_WDAT, v);
}

static void *dma_alloc(uint64_t size, uint64_t align) {
    uint8_t *raw = kmalloc(size + align);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + align - 1) & ~(align - 1));
    memset(p, 0, size);
    return p;
}

/* ---- Firmware file ----------------------------------------------------------- */

static int read_firmware(void) {
    if (fw_file) return 0;
    /* In the system folder (the RAM disk): "/" or "/SYS". */
    char path[64];
    k_snprintf(path, sizeof(path), "%s%s", strcmp(vfs_system_dir(), "/") ? vfs_system_dir() : "", FIRMWARE_PATH);
    fw_size = fat32_get_file_size(path);
    int got = -1;
    if (fw_size && fw_size != 0xFFFFFFFFu && (fw_file = kmalloc(fw_size)) != 0)
        got = fat32_read_file(path, fw_file, fw_size);
    if (!fw_size || fw_size == 0xFFFFFFFFu) {
        kprintf("  firmware %s not found on the disk\n", path);
        return -1;
    }
    if (got < (int)fw_size) {
        kprintf("  could not read %s (%u bytes, got %d)\n", path, fw_size, got);
        if (fw_file) kfree(fw_file);
        fw_file = 0;
        return -1;
    }
    const uint32_t *h = (const uint32_t *)fw_file;
    if (fw_size < 88 || h[0] != 0 || h[1] != TLV_MAGIC) {
        kprintf("  %s is not an Intel firmware file\n", FIRMWARE_PATH);
        return -1;
    }
    uint32_t off = 88;                                      /* zero, magic, name[64], ver, build, 8 bytes */
    while (off + 8 <= fw_size) {
        uint32_t type = *(const uint32_t *)(fw_file + off), len = *(const uint32_t *)(fw_file + off + 4);
        const uint8_t *data = fw_file + off + 8;
        if (off + 8 + len > fw_size) break;
        if (type == TLV_SEC_RT && len > 4 && section_count < MAX_SECTIONS) {
            sections[section_count].devoff = *(const uint32_t *)data;
            sections[section_count].data = data + 4;
            sections[section_count].len = len - 4;
            section_count++;
        } else if (type == TLV_IML) {
            iml = data;
            iml_len = len;
        } else if ((type == 29 || type == 30) && len == 8) {   /* API_CHANGES_SET, ENABLED_CAPABILITIES */
            uint32_t idx = *(const uint32_t *)data, bits = *(const uint32_t *)(data + 4);
            if (idx < 4) (type == 29 ? fw_api : fw_capa)[idx] |= bits;
        } else if (type == 48) {                            /* CMD_VERSIONS */
            fw_cmd_versions = data;
            fw_cmd_versions_len = len;
        } else if (type == 31 && len == 4) {                /* N_SCAN_CHANNELS */
            fw_n_scan_channels = *(const uint32_t *)data;
        } else if (type == TLV_FW_VERSION && len == 12) {
            memcpy(fw_ver, data, 12);
        }
        off += 8 + ((len + 3) & ~3u);
    }
    say("  firmware: %u bytes, version %u.%08x.%u, %d sections, image loader %u bytes\n", fw_size,
            fw_ver[0], fw_ver[1], fw_ver[2], section_count, iml_len);
    return section_count && iml ? 0 : -1;
}

static int count_until_separator(int start) {
    int n = 0;
    while (start < section_count && sections[start].devoff != SEP_CPU1_CPU2 && sections[start].devoff != SEP_PAGING) {
        start++;
        n++;
    }
    return n;
}

static void *section_dma[MAX_SECTIONS];

static void *copy_section(int i) {
    if (!section_dma[i]) section_dma[i] = dma_alloc(sections[i].len, 4096);
    if (section_dma[i]) memcpy(section_dma[i], sections[i].data, sections[i].len);
    return section_dma[i];
}

/* ---- Card bring-up ----------------------------------------------------------- */

static int set_hw_ready(void) {
    setbits(CSR_HW_IF_CONFIG, HW_IF_NIC_READY);
    int ok = poll_bit(CSR_HW_IF_CONFIG, HW_IF_NIC_READY, HW_IF_NIC_READY, 50);
    if (ok) setbits(CSR_MBOX_SET, 0x20);                    /* OS alive */
    return ok;
}

static int prepare_card(void) {
    if (set_hw_ready()) return 0;
    setbits(CSR_DBG_LINK_PWR_MGMT, 0x80000000);
    udelay(1000);
    for (int tries = 0; tries < 10; tries++) {
        setbits(CSR_HW_IF_CONFIG, HW_IF_PREPARE);
        for (int t = 0; t < 150000; t += 200) {
            if (set_hw_ready()) return 0;
            udelay(200);
        }
        udelay(25000);
    }
    return -1;
}

static int apm_init(void) {
    setbits(CSR_GIO_CHICKEN, 0x00800000);                   /* L1A, no L0S on RX */
    setbits(CSR_DBG_HPET_MEM, 0xFFFF0000);
    setbits(CSR_HW_IF_CONFIG, HW_IF_HAP_WAKE_L1A);
    setbits(CSR_GIO, 0x00000002);                           /* L0S disabled */
    setbits(CSR_GP_CNTRL, GP_INIT_DONE);
    return poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000) ? 0 : -1;
}

static void sw_reset(void) {
    setbits(CSR_RESET, RESET_SW_RESET);
    udelay(5000);
}

static int alloc_queues(void) {
    if (ctxt) return 0;
    ctxt = dma_alloc(sizeof(ctxt_info_gen3_t), 4096);
    scratch = dma_alloc(sizeof(prph_scratch_t), 4096);
    prph_info = dma_alloc(4096, 4096);
    rx_free = dma_alloc(sizeof(rx_free_desc_t) * RX_COUNT, 256);
    rx_used = dma_alloc(sizeof(rx_used_desc_t) * RX_COUNT, 256);
    rx_status = dma_alloc(16, 16);
    rx_bufs = dma_alloc((uint64_t)RX_BUF_SIZE * RX_COUNT, 4096);
    tx_cmd = dma_alloc(sizeof(tfh_tfd_t) * TX_COUNT, 256);
    cmd_bufs = dma_alloc((uint64_t)CMD_SLOT_SIZE_EARLY * TX_COUNT, 4096);
    if (!ctxt || !scratch || !prph_info || !rx_free || !rx_used || !rx_status || !rx_bufs || !tx_cmd || !cmd_bufs)
        return -1;
    for (int i = 0; i < RX_COUNT; i++) {
        rx_free[i].rbid = (uint16_t)i;
        rx_free[i].addr = (uint64_t)(rx_bufs + (uint64_t)i * RX_BUF_SIZE);
    }
    return 0;
}

/* Fill the context info and peripheral scratch, and start the boot ROM. */
static int start_firmware(void) {
    int lmac = count_until_separator(0);
    int umac = count_until_separator(lmac + 1);
    int paging = count_until_separator(lmac + umac + 2);
    say("  sections: %d LMAC, %d UMAC, %d paging\n", lmac, umac, paging);
    if (!lmac || !umac || lmac > MAX_DRAM_ENTRY || umac > MAX_DRAM_ENTRY || paging > MAX_DRAM_ENTRY) return -1;

    memset(scratch, 0, sizeof(*scratch));
    scratch->ctrl.version.mac_id = (uint16_t)rd(CSR_HW_REV);
    scratch->ctrl.version.size = (uint16_t)(sizeof(prph_scratch_t) / 4);
    scratch->ctrl.control.control_flags = PRPH_SCRATCH_RB_SIZE_4K | PRPH_SCRATCH_MTR_MODE | PRPH_MTR_FORMAT_256B;
    scratch->ctrl.rbd.free_rbd_addr = (uint64_t)rx_free;
    for (int i = 0; i < lmac; i++) {
        void *p = copy_section(i);
        if (!p) return -1;
        scratch->dram.lmac_img[i] = (uint64_t)p;
    }
    for (int i = 0; i < umac; i++) {
        void *p = copy_section(lmac + 1 + i);
        if (!p) return -1;
        scratch->dram.umac_img[i] = (uint64_t)p;
    }
    for (int i = 0; i < paging; i++) {
        void *p = copy_section(lmac + umac + 2 + i);
        if (!p) return -1;
        scratch->dram.virtual_img[i] = (uint64_t)p;
    }

    memset(ctxt, 0, sizeof(*ctxt));
    ctxt->prph_info_base_addr = (uint64_t)prph_info;
    ctxt->prph_scratch_base_addr = (uint64_t)scratch;
    ctxt->prph_scratch_size = sizeof(prph_scratch_t);
    ctxt->cr_head_idx_arr_base_addr = (uint64_t)rx_status;
    ctxt->tr_tail_idx_arr_base_addr = (uint64_t)prph_info + 2048;
    ctxt->cr_tail_idx_arr_base_addr = (uint64_t)prph_info + 3072;
    ctxt->mtr_base_addr = (uint64_t)tx_cmd;
    ctxt->mcr_base_addr = (uint64_t)rx_used;
    ctxt->mtr_size = 8 - 3;                                 /* log2(TX_COUNT) - 3 */
    ctxt->mcr_size = 9;                                     /* log2(RX_COUNT) */

    static uint8_t *iml_dma;
    if (!iml_dma) iml_dma = dma_alloc(iml_len, 4096);
    if (!iml_dma) return -1;
    memcpy(iml_dma, iml, iml_len);

    uint64_t a = (uint64_t)ctxt;
    wr(CSR_CTXT_INFO_ADDR, (uint32_t)a);
    wr(CSR_CTXT_INFO_ADDR + 4, (uint32_t)(a >> 32));
    a = (uint64_t)iml_dma;
    wr(CSR_IML_DATA_ADDR, (uint32_t)a);
    wr(CSR_IML_DATA_ADDR + 4, (uint32_t)(a >> 32));
    wr(CSR_IML_SIZE, iml_len);
    setbits(CSR_HW_IF_CONFIG, HW_IF_AUTO_FUNC_BOOT);

    if (!nic_lock()) return -1;
    write_prph(UREG_CPU_INIT_RUN + UMAC_PRPH_OFFSET, 1);    /* kick the self-load */
    nic_unlock();
    return 0;
}

/* ---- Receive queue and firmware commands ------------------------------------ */

#define HBUS_TARG_WRPTR   0x460            /* command queue write pointer: qid << 16 | index */
#define CMD_SLOT_SIZE     4096
#define LONG_GROUP        0x1
#define SYSTEM_GROUP      0x2
#define NVM_GROUP         0xC


static int alive_ok, alive_seen, packets_seen;
static uint32_t alive_lmac_major, alive_lmac_minor, alive_status, alive_sku[3];
static int init_complete, pnvm_complete;

/* The answer to the command in slot `resp_idx`, copied here when it comes. */
static int resp_idx = -1, resp_done, resp_failed;
static uint8_t resp_buf[2048];
static uint32_t resp_len;

static void handle_mpdu(const uint8_t *payload, uint32_t paylen);
static void link_reset(void);
static void handle_tx_done(uint8_t qid, const uint8_t *payload, uint32_t paylen);
static void handle_link_notif(uint8_t group, uint8_t code, const uint8_t *payload, uint32_t paylen);
static int txq_id = -1;                    /* our transmit queue, once the firmware gave one */
static volatile int scan_done;

static void handle_packet(const uint8_t *buf) {
    uint32_t len_flags = *(const uint32_t *)buf;
    uint8_t code = buf[4], group = buf[5], idx = buf[6], qid = buf[7];
    uint32_t len = len_flags & 0x3FFF;                      /* header (4) + payload */
    const uint8_t *payload = buf + 8;
    uint32_t paylen = len >= 4 ? len - 4 : 0;
    if (len_flags == 0x55550000u || (code == 0 && group == 0 && idx == 0 && qid == 0)) return;
    packets_seen++;

    if (code == 0x01 && group == 0 && paylen >= 12) {       /* ALIVE */
        alive_seen = 1;
        alive_status = *(const uint16_t *)payload;
        alive_lmac_major = *(const uint32_t *)(payload + 4);
        alive_lmac_minor = *(const uint32_t *)(payload + 8);
        if (paylen >= 128) memcpy(alive_sku, payload + 116, 12);   /* after 2 LMAC + 1 UMAC blocks */
        if (alive_status == 0xCAFE) alive_ok = 1;
    } else if (code == 0x04 && (group == 0 || group == LONG_GROUP)) {
        init_complete = 1;                                  /* INIT_COMPLETE_NOTIF */
    } else if (code == 0xFE && group == NVM_GROUP) {
        pnvm_complete = 1;                                  /* PNVM_INIT_COMPLETE */
    } else if (code == 0xC1 && (group == 0 || group == LONG_GROUP)) {
        handle_mpdu(payload, paylen);                       /* a received 802.11 frame */
    } else if ((code == 0x0F || code == 0xB5) && (group == 0 || group == LONG_GROUP) && (qid & 0x80)) {
        scan_done = 1;                                      /* SCAN_COMPLETE / ITERATION_COMPLETE */
    } else if (code == 0x1C && !(qid & 0x80) && txq_id >= 0 && qid == txq_id) {
        handle_tx_done(qid, payload, paylen);              /* a frame we sent: done */
    } else if ((code == 0xFB && group == 0x3) || (code == 0xA2 && (group == 0 || group == LONG_GROUP))) {
        handle_link_notif(group, code, payload, paylen);    /* session protection, missed beacons */
    } else if (!(qid & 0x80) && qid == 0 && idx == resp_idx) {
        resp_failed = (group & 0x40) != 0;
        resp_len = paylen < sizeof(resp_buf) ? paylen : sizeof(resp_buf);
        memcpy(resp_buf, payload, resp_len);
        resp_done = 1;                                      /* a reply to our command */
    } else if (packets_seen <= 12 && !(!(qid & 0x80) && qid == 0)) {
        say("  notification: group %02x code %02x, %u bytes\n", group, code, paylen);
    }
}

static void receive(void) {
    int hw = (*rx_status & 0xFFF) & (RX_COUNT - 1);
    if (rx_cur == hw) return;
    while (rx_cur != hw) {
        const uint8_t *b = rx_bufs + (uint64_t)rx_cur * RX_BUF_SIZE;
        for (uint32_t off = 0; off + 8 <= RX_BUF_SIZE;) {
            uint32_t lf = *(const uint32_t *)(b + off), len = 4 + (lf & 0x3FFF);
            const uint8_t *h = b + off + 4;
            if (lf == 0x55550000u || (h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0)) break;
            if (len < 8 || off + len > RX_BUF_SIZE) break;
            handle_packet(b + off);
            if (h[0] == 0xC1) break;                        /* a received frame fills its buffer */
            off += (len + 63) & ~63u;
        }
        rx_cur = (rx_cur + 1) % RX_COUNT;
    }
    int done = hw == 0 ? RX_COUNT - 1 : hw - 1;             /* hand processed buffers back */
    wr(RFH_Q0_FRBDCB_WIDX_TRG, (uint32_t)(done & ~7));
}

static uint64_t irq_off(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static volatile int fw_error;               /* the firmware reported an error: restart it */
static int fw_alive;                        /* ALIVE seen: the receive queue may be polled */

/* Acknowledge the card's interrupt causes and take what it received. Runs
 * with interrupts off: from wait_for() and from the timer hook. */
static void poll_once(void) {
    uint32_t r = rd(CSR_INT);
    if (r && r != 0xFFFFFFFF) {
        wr(CSR_INT, r);
        if (r & (INT_FH_RX | INT_SW_RX)) wr(CSR_FH_INT_STATUS, FH_INT_RX_MASK);
        if (r & (INT_SW_ERR | INT_HW_ERR)) fw_error = 1;
    }
    receive();
}

/* MSI: the card has something (interrupts are off here, as in the timer). */
static void iwl_interrupt(void) {
    if (regs && fw_alive) poll_once();
}

/* Poll the card for up to `ms` milliseconds or until *flag is set. */
static int wait_for(volatile int *flag, uint32_t ms) {
    uint32_t start = get_tick(), ticks = ms * TIMER_FREQ / 1000 + 1;
    while (!*flag && (uint32_t)(get_tick() - start) < ticks) {
        uint64_t fl = irq_off();
        poll_once();
        irq_restore(fl);
        if (fw_error) {
            kprintf("  the firmware stopped (error reported)\n");
            return -1;
        }
        udelay(100);
    }
    return *flag ? 0 : -1;
}


/* Put a command on the command queue; returns its slot. Safe from the timer
 * interrupt too (the network path installs group keys from there). */
static int queue_cmd(uint8_t group, uint8_t opcode, const void *data, uint16_t len) {
    if (len + 8 > CMD_SLOT_SIZE) return -1;
    uint64_t fl = irq_off();
    int slot = cmd_hw % TX_COUNT;
    uint8_t *cmd = cmd_bufs + (uint64_t)slot * CMD_SLOT_SIZE;
    memset(cmd, 0, 8);
    cmd[0] = opcode;
    cmd[1] = group ? group : LONG_GROUP;
    cmd[2] = (uint8_t)slot;                                 /* idx */
    cmd[3] = 0;                                             /* qid: the command queue */
    *(uint16_t *)(cmd + 4) = len;
    if (len) memcpy(cmd + 8, data, len);

    tfh_tfd_t *tfd = &tx_cmd[slot];
    memset(tfd, 0, sizeof(*tfd));
    uint32_t total = 8u + len;
    tfd->tbs[0].len = (uint16_t)(total < 20 ? total : 20);  /* the first piece is at most 20 bytes */
    tfd->tbs[0].addr = (uint64_t)cmd;
    tfd->num_tbs = 1;
    if (total > 20) {
        tfd->tbs[1].len = (uint16_t)(total - 20);
        tfd->tbs[1].addr = (uint64_t)cmd + 20;
        tfd->num_tbs = 2;
    }
    cmd_hw++;
    __asm__ volatile("mfence" ::: "memory");
    wr(HBUS_TARG_WRPTR, (0u << 16) | (uint32_t)cmd_hw);
    irq_restore(fl);
    return slot;
}

/* Send a firmware command (group, opcode) and wait for its reply, which is
 * left in resp_buf/resp_len. Old-style (group 0) commands go in the LONG
 * group, as firmware API 50+ requires. */
static int send_cmd(uint8_t group, uint8_t opcode, const void *data, uint16_t len) {
    uint64_t fl = irq_off();                                /* the reply must not slip past */
    resp_done = 0;
    resp_failed = 0;
    resp_idx = (int)(cmd_hw % TX_COUNT);
    if (queue_cmd(group, opcode, data, len) < 0) {
        resp_idx = -1;
        irq_restore(fl);
        return -1;
    }
    irq_restore(fl);
    int r = wait_for(&resp_done, 1000);
    resp_idx = -1;
    if (r != 0) kprintf("  no reply to command %02x/%02x\n", group, opcode);
    else if (resp_failed) {
        kprintf("  the firmware refused command %02x/%02x\n", group, opcode);
        r = -1;
    }
    return r;
}

/* ---- Step 2: firmware start-up handshake, NVM, MAC address ------------------- */

static uint8_t mac_addr[6];
static uint32_t nvm_tx_chains = 1, nvm_rx_chains = 1;
static int fw_ready;                       /* setup commands sent: ready to scan */

static void read_mac(void) {
    memset(mac_addr, 0, 6);
    if (!nic_lock()) return;
    for (int strap = 1; strap >= 0; strap--) {             /* the OEM-fused address first, then OTP */
        uint32_t a0 = rd(0x380 + (strap ? 0x08 : 0x00)), a1 = rd(0x380 + (strap ? 0x0C : 0x04));
        uint8_t m[6] = { (uint8_t)(a0 >> 24), (uint8_t)(a0 >> 16), (uint8_t)(a0 >> 8), (uint8_t)a0,
                         (uint8_t)(a1 >> 8), (uint8_t)a1 };
        int zero = 1, ones = 1;
        for (int i = 0; i < 6; i++) { zero &= m[i] == 0; ones &= m[i] == 0xFF; }
        if (!zero && !ones && !(m[0] & 1) && !(m[0] == 0x02 && m[1] == 0xCC && m[2] == 0xAA)) {
            memcpy(mac_addr, m, 6);
            break;
        }
    }
    nic_unlock();
}

static int firmware_init(void) {
    if (alive_sku[0] || alive_sku[1] || alive_sku[2]) {
        /* No platform NVM file for this radio: tell the firmware to go on. */
        pnvm_complete = 0;
        if (nic_lock()) {
            write_prph(0xA05C04 + UMAC_PRPH_OFFSET, 1u << 20);   /* UREG_DOORBELL_TO_ISR6: PNVM */
            nic_unlock();
        }
        if (wait_for(&pnvm_complete, 2000) != 0) say("  (no PNVM-complete notification; going on)\n");
    }

    uint32_t init_flags = 1u << 1;                          /* INIT_NVM */
    if (send_cmd(SYSTEM_GROUP, 0x03, &init_flags, 4) != 0) return -1;      /* INIT_EXTENDED_CFG */
    uint32_t zero = 0;
    init_complete = 0;
    if (send_cmd(NVM_GROUP, 0x00, &zero, 4) != 0) return -1;               /* NVM_ACCESS_COMPLETE */
    if (wait_for(&init_complete, 2000) != 0) {
        kprintf("  no INIT_COMPLETE from the firmware\n");
        return -1;
    }
    say("  firmware initialised\n");

    if (send_cmd(NVM_GROUP, 0x02, &zero, 4) != 0) return -1;               /* NVM_GET_INFO */
    if (resp_len < 28) {
        kprintf("  NVM reply too short (%u bytes)\n", resp_len);
        return -1;
    }
    uint16_t nvm_version = *(uint16_t *)(resp_buf + 4);
    uint32_t sku = *(uint32_t *)(resp_buf + 8);
    uint32_t tx_chains = *(uint32_t *)(resp_buf + 12), rx_chains = *(uint32_t *)(resp_buf + 16);
    uint32_t lar = *(uint32_t *)(resp_buf + 20), n_channels = *(uint32_t *)(resp_buf + 24);
    nvm_tx_chains = tx_chains & 0xF ? tx_chains & 0xF : 1;
    nvm_rx_chains = rx_chains & 0xF ? rx_chains & 0xF : 1;
    read_mac();
    say("  NVM version %x: %s%s%s%s%s, antennas tx %x rx %x, %u channels%s\n", nvm_version,
            sku & 1 ? "2.4 GHz " : "", sku & 2 ? "5 GHz " : "", sku & 4 ? "11n " : "", sku & 8 ? "11ac " : "",
            sku & 16 ? "11ax" : "", tx_chains, rx_chains, n_channels, lar ? ", location-aware regulatory" : "");
    say("Wi-Fi MAC address %02x:%02x:%02x:%02x:%02x:%02x. Step 2 done.\n", mac_addr[0], mac_addr[1],
            mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return 0;
}

/* ---- Step 3: runtime setup and scanning ---------------------------------------- */

static int has_api(int bit) { return (fw_api[bit / 32] >> (bit % 32)) & 1; }
static int has_capa(int bit) { return (fw_capa[bit / 32] >> (bit % 32)) & 1; }

/* Version of command (group, cmd) from the firmware's table; 0 if not listed. */
static int cmd_version(uint8_t group, uint8_t cmd) {
    for (uint32_t i = 0; i < fw_cmd_versions_len / 4; i++) {
        const uint8_t *v = fw_cmd_versions + i * 4;
        if (v[0] == cmd && v[1] == group) return v[2];
    }
    return 0;
}


/* The commands OpenBSD's iwx_init_hw() sends after the NVM is read. */
static int runtime_setup(void) {
    uint32_t v = nvm_tx_chains;
    if (send_cmd(0, 0x98, &v, 4) != 0) return -1;                         /* TX_ANT_CONFIGURATION */
    uint32_t bt[2] = { 3, 0 };                                            /* BT coex: Wi-Fi mode */
    if (send_cmd(0, 0x9B, bt, 8) != 0) return -1;
    /* SoC: integrated CNVi with a slow crystal (Linux: so_long_latency):
     * low-latency flag (needs scan API >= 2), 12000 us crystal latency. */
    uint32_t soc[2] = { cmd_version(LONG_GROUP, 0x0D) >= 2 ? 2u : 0u, 12000 };
    if (send_cmd(SYSTEM_GROUP, 0x01, soc, 8) != 0) return -1;
    if (has_capa(12)) {                                                   /* DQA: command queue 0 */
        uint32_t q = 0;
        if (send_cmd(0x5, 0x00, &q, 4) != 0) return -1;
    }
    if (has_capa(74)) {                                                   /* firmware handles CT-kill */
        uint8_t ths[4 + 16];
        memset(ths, 0, sizeof(ths));
        if (send_cmd(0x4, 0x04, ths, sizeof(ths)) != 0) return -1;
    }
    uint32_t power = 0;                                                   /* no power saving */
    if (send_cmd(0, 0x77, &power, 4) != 0) return -1;

    uint8_t mcc[28];                                                      /* country "ZZ": world */
    memset(mcc, 0, sizeof(mcc));
    *(uint16_t *)mcc = ('Z' << 8) | 'Z';
    mcc[2] = (has_api(9) || has_capa(29)) ? 0x10 : 0;                     /* source: get current */
    if (send_cmd(0, 0xC8, mcc, sizeof(mcc)) != 0) return -1;

    uint8_t scfg[12];                                                     /* SCAN_CFG (reduced) */
    memset(scfg, 0, sizeof(scfg));
    if (cmd_version(LONG_GROUP, 0x0C) < 5) scfg[2] = 0xFF;                /* broadcast station id */
    *(uint32_t *)(scfg + 4) = nvm_tx_chains;
    *(uint32_t *)(scfg + 8) = nvm_rx_chains;
    if (send_cmd(LONG_GROUP, 0x0C, scfg, sizeof(scfg)) != 0) return -1;

    uint8_t bf[60];                                                       /* beacon filter off */
    memset(bf, 0, sizeof(bf));
    if (send_cmd(0, 0xD2, bf, sizeof(bf)) != 0) return -1;
    fw_ready = 1;
    return 0;
}

/* SCAN_REQ_UMAC version 14 / 17 (same size; field layouts differ slightly). */
typedef struct {
    uint16_t flags;
    uint8_t reserved, scan_start_mac_id;
    uint8_t active_dwell[2];
    uint8_t adwell_default_2g, adwell_default_5g, adwell_default_social_chn, reserved1;
    uint16_t adwell_max_budget;
    uint32_t max_out_of_time[2];
    uint32_t suspend_time[2];
    uint32_t scan_priority;
    uint8_t passive_dwell[2];
    uint8_t num_of_fragments[2];
} __attribute__((packed)) scan_general_t;

typedef struct {
    uint32_t flags;
    uint8_t b[4];                          /* v1: channel, iter, iter_interval16; v2: channel, band, iter, interval; v5: channel, psd, iter, interval */
} __attribute__((packed)) scan_chan_t;

typedef struct {
    uint16_t offset, len;
} __attribute__((packed)) probe_seg_t;

typedef struct {
    uint32_t uid, ooc_priority;
    scan_general_t general;
    uint8_t ch_flags, ch_count, n_aps_override[2];
    scan_chan_t channels[67];
    struct { uint16_t interval; uint8_t iter_count, reserved; } __attribute__((packed)) schedule[2];
    uint16_t delay, reserved_p;
    probe_seg_t mac_header, band_data[3], common_data;
    uint8_t preq_buf[512];
    uint8_t short_ssid_num, bssid_num;
    uint16_t reserved_q;
    struct { uint8_t id, len, ssid[32]; } __attribute__((packed)) direct_scan[20];
    uint32_t short_ssid[8];
    uint8_t bssid_array[16][6];
} __attribute__((packed)) scan_req_t;

/* ---- Received frames ----------------------------------------------------------- */

/* What a beacon or probe response says about a network. */
typedef struct {
    char ssid[33];
    int ssid_len, channel, dtim_count, dtim_period;
    uint8_t rates[16];
    int n_rates;
    int rsn, wpa, psk, sae, eap, ccmp, mfpr;   /* psk: bit 0 = PSK, bit 1 = PSK-SHA256 */
    uint8_t group_cipher;
} ies_t;

static void parse_ies(const uint8_t *ie, uint32_t len, ies_t *o) {
    memset(o, 0, sizeof(*o));
    o->channel = -1;
    for (uint32_t i = 0; i + 2 <= len;) {
        uint8_t id = ie[i], l = ie[i + 1];
        if (i + 2 + l > len) break;
        const uint8_t *e = ie + i + 2;
        if (id == 0 && l <= 32) {
            memcpy(o->ssid, e, l);
            o->ssid[l] = 0;
            o->ssid_len = l;
        } else if (id == 1 || id == 50) {                  /* supported / extended rates */
            for (int k = 0; k < l && o->n_rates < 16; k++) o->rates[o->n_rates++] = e[k];
        } else if (id == 3 && l == 1) {
            o->channel = e[0];
        } else if (id == 5 && l >= 2) {                     /* TIM: DTIM count and period */
            o->dtim_count = e[0];
            o->dtim_period = e[1];
        } else if (id == 48 && l >= 8) {                    /* RSN: WPA2/WPA3 */
            o->rsn = 1;
            if (e[2] == 0x00 && e[3] == 0x0F && e[4] == 0xAC) o->group_cipher = e[5];
            uint32_t p = 6;
            uint16_t n = l >= p + 2 ? (uint16_t)(e[p] | e[p + 1] << 8) : 0;
            p += 2;
            for (int k = 0; k < n && p + 4 <= l; k++, p += 4)
                if (e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 4) o->ccmp = 1;
            n = l >= p + 2 ? (uint16_t)(e[p] | e[p + 1] << 8) : 0;
            p += 2;
            for (int k = 0; k < n && p + 4 <= l; k++, p += 4) {
                if (e[p] != 0x00 || e[p + 1] != 0x0F || e[p + 2] != 0xAC) continue;
                if (e[p + 3] == 2) o->psk |= 1;
                else if (e[p + 3] == 6) o->psk |= 2;
                else if (e[p + 3] == 8) o->sae = 1;
                else if (e[p + 3] == 1 || e[p + 3] == 5) o->eap = 1;
            }
            if (p + 2 <= l && (e[p] & 0x40)) o->mfpr = 1;  /* management frame protection required */
        } else if (id == 221 && l >= 4 && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xF2 && e[3] == 1) {
            o->wpa = 1;
        }
        i += 2 + l;
    }
}

/* What the scan found. */
#define MAX_BSS 96
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    int ssid_len, channel, signal, dtim_period;
    uint16_t capab, bintval;
    uint8_t rates[16];
    int n_rates;
    uint8_t group_cipher;
    int secured;
    const char *security;
    const char *why_not;                   /* 0 if we can join it */
} bss_t;
static bss_t bss_list[MAX_BSS];
static int bss_count, frames_seen, scanning;

static void add_bss(const uint8_t *frame, uint32_t len, int signal, int desc_channel) {
    const uint8_t *bssid = frame + 16, *body = frame + 24;
    uint16_t capab = (uint16_t)(body[10] | body[11] << 8);
    ies_t ie;
    parse_ies(body + 12, len - 36, &ie);
    frames_seen++;
    int secured = (capab & 0x10) != 0;
    const char *security = !secured ? "open" :
                           ie.rsn ? (ie.sae && ie.psk ? "WPA2/WPA3" : ie.sae ? "WPA3" : ie.psk ? "WPA2" :
                                     ie.eap ? "WPA2-Enterprise" : "WPA2?") :
                           ie.wpa ? "WPA (old)" : "WEP (old)";
    const char *why = 0;
    if (secured) {
        if (!ie.rsn) why = ie.wpa ? "it uses old WPA (TKIP), which is not supported" : "it uses WEP, which is not supported";
        else if (!(ie.psk & 1)) why = ie.sae ? "it is WPA3-only, which is not supported yet" :
                                      ie.eap ? "it is WPA2-Enterprise (user name + password), not supported" :
                                               "it uses a key management that is not supported";
        else if (!ie.ccmp || ie.group_cipher != 4) why = "it uses TKIP encryption (set the router to AES)";
        else if (ie.mfpr) why = "it requires protected management frames (802.11w), not supported yet";
    }
    for (int k = 0; k < bss_count; k++) {
        if (!memcmp(bss_list[k].bssid, bssid, 6)) {
            if (signal > bss_list[k].signal) bss_list[k].signal = signal;
            if (ie.ssid[0] && !bss_list[k].ssid[0]) {
                memcpy(bss_list[k].ssid, ie.ssid, sizeof(ie.ssid));
                bss_list[k].ssid_len = ie.ssid_len;
            }
            return;
        }
    }
    if (bss_count >= MAX_BSS) return;
    bss_t *b = &bss_list[bss_count++];
    memset(b, 0, sizeof(*b));
    memcpy(b->bssid, bssid, 6);
    memcpy(b->ssid, ie.ssid, sizeof(ie.ssid));
    b->ssid_len = ie.ssid_len;
    b->channel = ie.channel > 0 ? ie.channel : desc_channel;
    b->signal = signal;
    b->dtim_period = ie.dtim_period;
    b->capab = capab;
    b->bintval = (uint16_t)(body[8] | body[9] << 8);
    memcpy(b->rates, ie.rates, sizeof(ie.rates));
    b->n_rates = ie.n_rates;
    b->group_cipher = ie.group_cipher;
    b->secured = secured;
    b->security = security;
    b->why_not = why;
}

/* 802.11 header length (data frames: QoS, 4 addresses, HT control). */
static uint32_t frame_hdrlen(const uint8_t *f) {
    uint32_t len = 24;
    if (((f[0] >> 2) & 3) == 2) {
        if ((f[1] & 3) == 3) len += 6;
        if (f[0] & 0x80) {
            len += 2;
            if (f[1] & 0x80) len += 4;
        }
    } else if (f[1] & 0x80) {
        len += 4;
    }
    return len;
}

static void mgmt_rx(const uint8_t *f, uint32_t len, int signal, int channel, uint32_t gp2);
static void data_rx(const uint8_t *f, uint32_t len, uint32_t status, int signal);

/* REPLY_RX_MPDU_CMD: a 64-byte descriptor (AX210 family), then the frame. */
static void handle_mpdu(const uint8_t *payload, uint32_t paylen) {
    if (paylen < 64) return;
    uint32_t mpdu_len = *(const uint16_t *)payload;
    uint8_t mac_flags2 = payload[3];
    uint32_t status = *(const uint32_t *)(payload + 12);
    if (!(status & 1) || !(status & 2)) return;             /* CRC ok, no overrun */
    if (mpdu_len < 24 || mpdu_len > paylen - 64) return;
    const uint8_t *frame = payload + 64;
    static uint8_t copy[4096];
    if (mac_flags2 & 0x20) {                                /* 2 bytes of padding after the header (and IV) */
        uint32_t hl = frame_hdrlen(frame) + ((status & 0x700) == 0x200 ? 8 : 0);
        if (mpdu_len < hl + 2 || mpdu_len > sizeof(copy)) return;
        memcpy(copy, frame, hl);
        memcpy(copy + hl, frame + hl + 2, mpdu_len - hl - 2);
        frame = copy;
        mpdu_len -= 2;
    }
    int energy = payload[40];
    int signal = energy ? -energy : -100;
    int type = (frame[0] >> 2) & 3;
    if (type == 0) mgmt_rx(frame, mpdu_len, signal, payload[42], *(const uint32_t *)(payload + 44));
    else if (type == 2) data_rx(frame, mpdu_len, status, signal);
}

static const uint8_t channels_24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const uint8_t channels_5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                      132, 136, 140, 144, 149, 153, 157, 161, 165 };

static int start_scan(void) {
    int ver = cmd_version(LONG_GROUP, 0x0D);
    if (ver < 14) {
        kprintf("  this firmware's scan command (version %d) is not supported\n", ver);
        return -1;
    }
    static scan_req_t req;
    memset(&req, 0, sizeof(req));
    req.ooc_priority = 6;                                   /* SCAN_PRIORITY_EXT_6 */
    scan_general_t *g = &req.general;
    g->flags = (1u << 11) | (1u << 1) | (1u << 2) | (1u << 7);   /* passive, pass all, notify, adaptive dwell */
    g->adwell_default_social_chn = 10;
    g->adwell_default_2g = 2;
    g->adwell_default_5g = 8;
    g->adwell_max_budget = 300;
    g->scan_priority = 6;
    g->active_dwell[0] = g->active_dwell[1] = 10;
    g->passive_dwell[0] = g->passive_dwell[1] = 110;
    req.schedule[0].iter_count = 1;

    /* A probe request template (sent only in active scans): header, SSID, rates. */
    uint8_t *f = req.preq_buf;
    f[0] = 0x40;                                            /* management, probe request */
    memset(f + 4, 0xFF, 6);
    memcpy(f + 10, mac_addr, 6);
    memset(f + 16, 0xFF, 6);
    uint32_t n = 24;
    f[n++] = 0; f[n++] = 0;                                 /* SSID: filled in by the firmware */
    req.mac_header.offset = 0;
    req.mac_header.len = (uint16_t)n;
    static const uint8_t rates24[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C };
    req.band_data[0].offset = (uint16_t)n;
    memcpy(f + n, rates24, sizeof(rates24));
    n += sizeof(rates24);
    if (has_capa(9)) { f[n++] = 3; f[n++] = 1; f[n++] = 0; }   /* DS parameter set */
    req.band_data[0].len = (uint16_t)(n - req.band_data[0].offset);
    static const uint8_t rates5[] = { 1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
    req.band_data[1].offset = (uint16_t)n;
    memcpy(f + n, rates5, sizeof(rates5));
    n += sizeof(rates5);
    req.band_data[1].len = sizeof(rates5);
    req.common_data.offset = (uint16_t)n;

    /* Channels: 2.4 GHz 1-13, 5 GHz (listening only: legal everywhere). */
    req.ch_flags = 1u << 5;                                 /* ENABLE_CHAN_ORDER */
    req.n_aps_override[0] = 10;
    req.n_aps_override[1] = 2;
    int count = 0, max = fw_n_scan_channels ? (int)fw_n_scan_channels : 40;
    for (int band = 0; band < 2; band++) {
        const uint8_t *list = band ? channels_5 : channels_24;
        int nl = band ? (int)sizeof(channels_5) : (int)sizeof(channels_24);
        for (int i = 0; i < nl && count < max && count < 67; i++) {
            scan_chan_t *c = &req.channels[count++];
            uint8_t phy_band = band ? 0 : 1;                /* PHY_BAND_5 = 0, PHY_BAND_24 = 1 */
            c->b[0] = list[i];
            if (ver >= 17) {                                /* v5 channel config */
                c->b[1] = 0x80;                             /* psd_20 = -128 */
                c->b[2] = 1;
                c->flags = (uint32_t)phy_band << 30;
            } else if (has_api(58)) {                       /* v2: with band */
                c->b[1] = phy_band;
                c->b[2] = 1;
            } else {                                        /* v1 */
                c->b[1] = 1;
            }
        }
    }
    req.ch_count = (uint8_t)count;

    bss_count = frames_seen = scan_done = 0;
    scanning = 1;
    if (send_cmd(LONG_GROUP, 0x0D, &req, sizeof(req)) != 0) {
        scanning = 0;
        return -1;
    }
    say("  scanning %d channels (scan command v%d)...\n", count, ver);
    if (wait_for(&scan_done, 15000) != 0) say("  (no scan-complete notification within 15 s)\n");
    scanning = 0;
    return 0;
}

/* The numbered list `wifi scan` printed last: one entry per network name. */
static char scan_names[MAX_BSS][33];
static int scan_names_count;

const char *iwl_scan_pick(int number) {
    return number >= 1 && number <= scan_names_count ? scan_names[number - 1] : 0;
}

int iwl_scan(void) {
    if (!fw_ready) {
        kprintf("Starting the Wi-Fi card...\n");
        quiet = 1;                                          /* `wifi start` shows the details */
        int r = iwl_start() != 0 || runtime_setup() != 0;
        quiet = 0;
        if (r) {
            kprintf("The card did not start; `wifi start` shows why.\n");
            return -1;
        }
    }
    kprintf("Scanning...\n");
    quiet = 1;
    int r = start_scan();
    quiet = 0;
    if (r != 0) return -1;

    /* Strongest first. */
    for (int i = 1; i < bss_count; i++)
        for (int j = i; j > 0 && bss_list[j].signal > bss_list[j - 1].signal; j--) {
            bss_t t = bss_list[j];
            bss_list[j] = bss_list[j - 1];
            bss_list[j - 1] = t;
        }
    /* One line per name, its strongest access point (the list is sorted). */
    scan_names_count = 0;
    int hidden = 0;
    kprintf("  %3s  %-32s  %-3s  %-12s  %s\n", "#", "NAME", "CH", "SIGNAL", "SECURITY");
    for (int i = 0; i < bss_count; i++) {
        bss_t *b = &bss_list[i];
        if (!b->ssid[0]) {
            hidden++;
            continue;
        }
        int dup = 0;
        for (int k = 0; k < scan_names_count && !dup; k++) dup = strcmp(scan_names[k], b->ssid) == 0;
        if (dup) continue;
        memcpy(scan_names[scan_names_count++], b->ssid, sizeof(b->ssid));
        int bars = b->signal >= -55 ? 4 : b->signal >= -65 ? 3 : b->signal >= -75 ? 2 : 1;
        kprintf("  %3d  %-32s  %3d  %3d dBm %s  %s%s\n", scan_names_count, b->ssid, b->channel, b->signal,
                bars == 4 ? "####" : bars == 3 ? "###." : bars == 2 ? "##.." : "#...", b->security,
                b->why_not ? " (not supported)" : "");
    }
    if (!scan_names_count) kprintf("  (no networks heard)\n");
    if (hidden) kprintf("  and %d hidden network%s (no name broadcast)\n", hidden, hidden == 1 ? "" : "s");
    return 0;
}

int iwl_start(void) {
    if (wifi_count() == 0) {
        kprintf("No Wi-Fi adapter found.\n");
        return -1;
    }
    const wifi_adapter_t *a = wifi_get(0);
    const pci_device_t *d = &a->pci;
    if (d->vendor != 0x8086) {
        kprintf("Only Intel Wi-Fi cards have a driver so far.\n");
        return -1;
    }
    say("Starting %04x:%04x at %02x:%02x.%u\n", d->vendor, d->device, d->bus, d->slot, d->func);
    fw_ready = 0;

    /* PCI: wake (D0), an address if the firmware gave none, memory + bus
     * master on, legacy interrupts off. */
    uint32_t bar0 = pci_config_read_dword(d->bus, d->slot, d->func, 0x10);
    uint32_t bar1 = pci_config_read_dword(d->bus, d->slot, d->func, 0x14);
    uint32_t pm_after, pm = pci_power_on(d->bus, d->slot, d->func, &pm_after);
    if ((pci_config_read_dword(d->bus, d->slot, d->func, 0x10) & ~0xFu) != (bar0 & ~0xFu)) {
        pci_config_write_dword(d->bus, d->slot, d->func, 0x10, bar0);
        pci_config_write_dword(d->bus, d->slot, d->func, 0x14, bar1);
    }
    uint64_t base = bar0 & ~0xFull;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)bar1 << 32;
    int assigned = 0;
    if (!base) {
        base = pci_assign_bar0(d->bus, d->slot, d->func);
        assigned = 1;
    }
    say("  power state %x -> %x, BAR %lx%s\n", pm & 3, pm_after & 3, base, assigned ? " (assigned by us)" : "");
    if (!base) {
        kprintf("  no memory address for the card: %s\n", pci_assign_note());
        return -1;
    }
    uint32_t cmd = pci_config_read_dword(d->bus, d->slot, d->func, 0x04);
    pci_config_write_dword(d->bus, d->slot, d->func, 0x04, (cmd & 0xFFFF) | 0x2 | 0x4 | 0x400);
    uint32_t r40 = pci_config_read_dword(d->bus, d->slot, d->func, 0x40);
    pci_config_write_dword(d->bus, d->slot, d->func, 0x40, r40 & ~0xFF00u);   /* no retry timeout */
    static int msi_tried;
    if (!msi_tried) {                                       /* interrupts: packets handled at once */
        msi_tried = 1;
        msi_enable(d->bus, d->slot, d->func, iwl_interrupt, "Wi-Fi", 0);
    }
    static uint64_t mapped_base;
    if (mapped_base != base) {
        regs = (volatile uint8_t *)mmio_map(base, 0x4000);
        mapped_base = base;
    }
    nic_locks = 0;

    uint32_t hw_rev = rd(CSR_HW_REV), rf_id = rd(CSR_HW_RF_ID);
    say("  registers at %lx: hardware %08x, radio %08x\n", base, hw_rev, rf_id);
    if (hw_rev == 0xFFFFFFFF) {
        kprintf("  the card does not answer (asleep or not powered)\n");
        return -1;
    }
    wr(CSR_INT_MASK, 0);
    wr(CSR_INT, 0xFFFFFFFF);
    wr(CSR_FH_INT_STATUS, 0xFFFFFFFF);

    if (read_firmware() != 0) return -1;
    if (alloc_queues() != 0) {
        kprintf("  out of memory\n");
        return -1;
    }

    /* Start the hardware (iwx_start_hw). */
    if (prepare_card() != 0) {
        kprintf("  the card did not become ready (HW_IF_CONFIG %08x)\n", rd(CSR_HW_IF_CONFIG));
        return -1;
    }
    sw_reset();
    if (apm_init() != 0) {
        kprintf("  the MAC clock did not start (GP_CNTRL %08x)\n", rd(CSR_GP_CNTRL));
        return -1;
    }
    if (nic_lock()) {                                       /* MSI mode: causes appear in CSR_INT */
        write_prph(UREG_CHICK + UMAC_PRPH_OFFSET, UREG_CHICK_MSI_ENABLE);
        nic_unlock();
    }
    wr(CSR_INT_MASK, INT_RF_KILL);
    setbits(CSR_GP_CNTRL, GP_RFKILL_WAKE_L1A_EN);
    int radio_on = (rd(CSR_GP_CNTRL) & GP_HW_RF_KILL_SW) != 0;
    say("  card ready, MAC clock running; radio switch: %s\n", radio_on ? "on" : "OFF (airplane mode?)");

    /* Start the firmware (iwx_start_fw). */
    wr(CSR_INT, 0xFFFFFFFF);
    wr(CSR_INT_MASK, 0);
    wr(CSR_FH_INT_STATUS, 0xFFFFFFFF);
    wr(CSR_UCODE_DRV_GP1_CLR, 0x2);                         /* rfkill handshake bits */
    wr(CSR_UCODE_DRV_GP1_CLR, 0x4);
    wr(CSR_INT, 0xFFFFFFFF);
    apm_init();
    wr8(CSR_INT_COALESCING, 0x40);
    setbits(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFF);
    wr(CSR_INT_MASK, INT_ALIVE | INT_FH_RX);
    rx_cur = 0;
    cmd_hw = 0;
    fw_alive = 0;
    fw_error = 0;
    link_reset();
    *rx_status = 0;
    alive_ok = alive_seen = packets_seen = 0;
    memset(alive_sku, 0, sizeof(alive_sku));

    if (start_firmware() != 0) {
        kprintf("  could not set up the firmware load\n");
        return -1;
    }
    say("  firmware handed to the card; waiting for it to start...\n");

    uint32_t start = get_tick(), seen = 0;
    while ((uint32_t)(get_tick() - start) < 3 * TIMER_FREQ && !alive_seen) {
        uint32_t r = rd(CSR_INT);
        if (r == 0xFFFFFFFF) break;
        if (r) {
            wr(CSR_INT, r);
            seen |= r;
        }
        if (r & INT_ALIVE) {                                /* the firmware set up the receive DMA */
            wr(RFH_Q0_FRBDCB_WIDX_TRG, 8);
        }
        if (r & (INT_FH_RX | INT_SW_RX)) wr(CSR_FH_INT_STATUS, FH_INT_RX_MASK);
        if (r & (INT_SW_ERR | INT_HW_ERR)) break;
        receive();
        udelay(200);
    }

    say("  interrupt causes seen: %08x; image loader said %08x; %d packet%s received\n", seen,
            rd(CSR_IML_RESP), packets_seen, packets_seen == 1 ? "" : "s");
    if (alive_ok) {
        fw_alive = 1;
        /* From now on the card may interrupt for everything we handle. */
        wr(CSR_INT_MASK, INT_FH_RX | INT_SW_RX | INT_ALIVE | INT_SW_ERR | INT_HW_ERR | INT_RF_KILL);
        say("Firmware is ALIVE (status %04x, version %u.%u). Step 1 done.\n", alive_status,
                alive_lmac_major, alive_lmac_minor);
        return firmware_init();
    }
    if (alive_seen) kprintf("Firmware answered ALIVE but with status %04x (expected cafe).\n", alive_status);
    else if (seen & INT_SW_ERR) kprintf("The firmware crashed while starting (software error).\n");
    else if (seen & INT_HW_ERR) kprintf("The card reported a hardware (DMA) error.\n");
    else kprintf("No ALIVE within 3 seconds. GP_CNTRL %08x, HW_IF_CONFIG %08x, RX status %u\n",
                 rd(CSR_GP_CNTRL), rd(CSR_HW_IF_CONFIG), (unsigned)*rx_status);
    return -1;
}

/* ---- Step 4: joining a network ----------------------------------------------------
 *
 * As OpenBSD's iwx_auth() and iwx_run() do it: tell the firmware about the
 * channel (PHY context), about us (MAC context), bind the two, add the access
 * point as a station, get a transmit queue, and reserve air time ("session
 * protection"). Then authentication and association frames go out on that
 * queue, the firmware is told we are associated, and the access point starts
 * the WPA2 handshake (net/wpa.c). Its keys go to the firmware, which encrypts
 * and decrypts from then on. Data frames are turned into Ethernet frames for
 * the network stack, and back.
 */

#define MAC_CONF_GROUP   0x3
#define DATA_PATH_GROUP  0x5
#define ACTION_ADD       1
#define ACTION_MODIFY    2

#define TXF_CMD_RATE     (1u << 0)
#define TXF_ENCRYPT_DIS  (1u << 1)
#define TXF_HIGH_PRI     (1u << 2)
#define RATE_6M          ((1u << 14) | (1u << 8))      /* antenna A, legacy OFDM, 6 Mbit/s */

#define LINK_DOWN        0
#define LINK_JOINING     1    /* contexts set up, authenticating / associating */
#define LINK_HANDSHAKE   2    /* associated, WPA2 handshake running */
#define LINK_UP          3    /* data flows */

#define TXQ_SLOTS        256
#define TXQ_BUF          2048

static bss_t cur;                          /* the network we joined (or are joining) */
static int link_state, contexts_dirty, port_open, ptk_installed;
static volatile int auth_done, assoc_done, session_started, handshake_done, link_lost;
static uint16_t auth_status, assoc_status, assoc_aid, lost_reason;
static const char *lost_kind;
static const char *join_error;
static char join_error_buf[96];
static uint32_t last_beacon_tick, beacon_gp2, beacons_up;
static int fixed_rate;                     /* rate scaling refused: send data at 6 Mbit/s */
static const char *setup_note;             /* an optional setup command the firmware refused */
static uint64_t beacon_tsf;
static int beacon_dtim_count;
static uint16_t tx_seq, last_rx_seq = 0xFFFF;
static wpa_t wpa;
static uint8_t pending_eapol[512];
static int pending_eapol_len;

static tfh_tfd_t *txq_tfd;
static uint16_t *txq_bc;
static uint8_t *txq_bufs;
static uint16_t txq_hw, txq_done;
static uint32_t tx_reports, tx_last_status;

static nic_rx_cb_t rx_cb;
static nic_stats_t stats;

static void random_bytes(uint8_t *out, int n);

static void link_reset(void) {
    link_state = LINK_DOWN;
    contexts_dirty = port_open = ptk_installed = 0;
    auth_done = assoc_done = session_started = handshake_done = link_lost = 0;
    pending_eapol_len = 0;
    txq_id = -1;
}

static void set_error(const char *fmt, uint32_t a, uint32_t b) {
    k_snprintf(join_error_buf, sizeof(join_error_buf), fmt, a, b);
    join_error = join_error_buf;
}

/* ---- Transmit ---- */

/* One frame on our queue: header (`hdrlen` bytes), then two body pieces. */
static int tx_frame(const uint8_t *hdr, int hdrlen, const uint8_t *b1, int l1, const uint8_t *b2, int l2,
                    uint16_t flags, uint32_t rate) {
    int pad = (hdrlen & 3) ? 4 - (hdrlen & 3) : 0;
    int total = hdrlen + l1 + l2;
    if (txq_id < 0 || 32 + hdrlen + pad + l1 + l2 > TXQ_BUF) return -1;
    uint64_t fl = irq_off();
    if ((uint16_t)(txq_hw - txq_done) >= TXQ_SLOTS - 4) {   /* full: the card is not keeping up */
        irq_restore(fl);
        stats.tx_errors++;
        return -1;
    }
    int slot = txq_hw % TXQ_SLOTS;
    uint8_t *buf = txq_bufs + (uint64_t)slot * TXQ_BUF;
    memset(buf, 0, 32);
    buf[0] = 0x1C;                                          /* TX_CMD, short header */
    buf[2] = (uint8_t)slot;
    buf[3] = (uint8_t)txq_id;
    uint8_t *tx = buf + 4;                                  /* struct iwx_tx_cmd_gen3 (28 bytes) */
    *(uint16_t *)tx = (uint16_t)total;
    *(uint16_t *)(tx + 2) = flags;
    *(uint32_t *)(tx + 4) = ((uint32_t)(hdrlen / 2) & 0x1F) << 8 | (pad ? 1u << 13 : 0);
    *(uint32_t *)(tx + 16) = rate;
    memcpy(buf + 32, hdr, (size_t)hdrlen);
    memset(buf + 32 + hdrlen, 0, (size_t)pad);
    uint8_t *body = buf + 32 + hdrlen + pad;
    if (l1) memcpy(body, b1, (size_t)l1);
    if (l2) memcpy(body + l1, b2, (size_t)l2);

    tfh_tfd_t *d = &txq_tfd[slot];
    memset(d, 0, sizeof(*d));
    d->tbs[0].len = 20;
    d->tbs[0].addr = (uint64_t)buf;
    d->tbs[1].len = (uint16_t)(32 + hdrlen + pad - 20);
    d->tbs[1].addr = (uint64_t)buf + 20;
    d->num_tbs = 2;
    if (l1 + l2) {
        d->tbs[2].len = (uint16_t)(l1 + l2);
        d->tbs[2].addr = (uint64_t)body;
        d->num_tbs = 3;
    }
    txq_bc[slot] = (uint16_t)total;                        /* bytes; one 64-byte TFD chunk */
    txq_hw++;
    __asm__ volatile("mfence" ::: "memory");
    wr(HBUS_TARG_WRPTR, (uint32_t)txq_id << 16 | txq_hw);
    irq_restore(fl);
    stats.tx_packets++;
    stats.tx_bytes += (uint32_t)total;
    return 0;
}

static void handle_tx_done(uint8_t qid, const uint8_t *p, uint32_t len) {
    (void)qid;
    if (len < 48 || p[0] != 1) return;                      /* one frame (no aggregation) */
    tx_last_status = *(const uint16_t *)(p + 40) & 0xFF;
    uint32_t ssn = *(const uint32_t *)(p + 44);            /* frames before this ring index are done */
    if (ssn < 65536 && (uint16_t)(txq_hw - (uint16_t)ssn) <= TXQ_SLOTS) txq_done = (uint16_t)ssn;
    tx_reports++;
    if (tx_last_status != 1 && tx_last_status != 2) stats.tx_errors++;
}

static void make_header(uint8_t *h, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3) {
    memset(h, 0, 24);
    h[0] = fc0;
    h[1] = fc1;
    memcpy(h + 4, a1, 6);
    memcpy(h + 10, a2, 6);
    memcpy(h + 16, a3, 6);
    uint16_t sc = (uint16_t)((tx_seq++ & 0xFFF) << 4);
    h[22] = (uint8_t)sc;
    h[23] = (uint8_t)(sc >> 8);
}

/* Management frames: lowest rate, never encrypted. */
static int send_mgmt(uint8_t fc0, const uint8_t *body, int len) {
    uint8_t h[24];
    make_header(h, fc0, 0, cur.bssid, mac_addr, cur.bssid);
    return tx_frame(h, 24, body, len, 0, 0, TXF_CMD_RATE | TXF_ENCRYPT_DIS, RATE_6M);
}

/* A data frame to the access point (ToDS), with an LLC/SNAP header. */
static int send_data(const uint8_t *da, const uint8_t *sa, uint16_t type, const uint8_t *p, int len, int eapol) {
    uint8_t h[24], llc[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, (uint8_t)(type >> 8), (uint8_t)type };
    make_header(h, 0x08, (uint8_t)(0x01 | (ptk_installed ? 0x40 : 0)), cur.bssid, sa, da);
    uint16_t flags = ptk_installed ? 0 : TXF_ENCRYPT_DIS;
    uint32_t rate = 0;                                      /* the firmware picks (rate scaling) */
    if (eapol || fixed_rate) {
        flags |= TXF_CMD_RATE | TXF_HIGH_PRI;
        rate = RATE_6M;
    }
    return tx_frame(h, 24, llc, 8, p, len, flags, rate);
}

static void send_deauth(uint16_t reason) {
    uint8_t body[2] = { (uint8_t)reason, (uint8_t)(reason >> 8) };
    send_mgmt(0xC0, body, 2);
}

/* ---- Keys ---- */

/* SEC_KEY_CMD (data path group): the firmware encrypts/decrypts with it.
 * Queued without waiting: group keys also arrive from the timer interrupt. */
static void install_key(const uint8_t *key, int len, int id, int group) {
    uint8_t c[80];
    memset(c, 0, sizeof(c));
    *(uint32_t *)c = ACTION_ADD;
    *(uint32_t *)(c + 4) = 1;                               /* station 0 */
    *(uint32_t *)(c + 8) = (uint32_t)id;
    *(uint32_t *)(c + 12) = 2u | (group ? 0x40u : 0);       /* CCMP, multicast */
    memcpy(c + 16, key, (size_t)len);
    queue_cmd(DATA_PATH_GROUP, 0x18, c, sizeof(c));
}

static void process_eapol(const uint8_t *p, int len) {
    uint8_t reply[256];
    int ev;
    int r = wpa_input(&wpa, p, len, reply, &ev);
    if (r < 0) {
        if (wpa.error) join_error = wpa.error;
        return;
    }
    if (r > 0) send_data(cur.bssid, mac_addr, 0x888E, reply, r, 1);
    if (ev & WPA_EV_PTK) {
        install_key(wpa.tk, 16, 0, 0);
        ptk_installed = 1;
    }
    if (ev & WPA_EV_GTK) install_key(wpa.gtk, wpa.gtk_len, wpa.gtk_idx, 1);
    if (ev & WPA_EV_DONE) handshake_done = 1;
}

/* Keep message 1 until the firmware knows we are associated. */
static void eapol_rx(const uint8_t *p, uint32_t len) {
    if (link_state < LINK_HANDSHAKE) {
        if (len <= sizeof(pending_eapol)) {
            memcpy(pending_eapol, p, len);
            pending_eapol_len = (int)len;
        }
        return;
    }
    process_eapol(p, (int)len);
}

/* ---- Receive ---- */

static void handle_link_notif(uint8_t group, uint8_t code, const uint8_t *p, uint32_t len) {
    if (group == MAC_CONF_GROUP && code == 0xFB && len >= 16) {     /* session protection */
        if (*(const uint32_t *)(p + 4) && *(const uint32_t *)(p + 8)) session_started = 1;
    } else if (code == 0xA2 && len >= 12) {                         /* missed beacons */
        if (link_state == LINK_UP && *(const uint32_t *)(p + 8) >= 16) {
            lost_kind = "no beacons from the access point";
            lost_reason = 0;
            link_lost = 1;
        }
    }
}

static void mgmt_rx(const uint8_t *f, uint32_t len, int signal, int channel, uint32_t gp2) {
    uint8_t sub = f[0] >> 4;
    const uint8_t *body = f + 24;
    uint32_t blen = len - 24;
    if (sub == 8 || sub == 5) {                             /* beacon, probe response */
        if (len < 36) return;
        if (scanning) add_bss(f, len, signal, channel);
        if (link_state != LINK_DOWN && sub == 8 && !memcmp(f + 16, cur.bssid, 6)) {
            cur.signal = signal;
            last_beacon_tick = get_tick();
            if (link_state == LINK_UP) beacons_up++;
            memcpy(&beacon_tsf, body, 8);
            beacon_gp2 = gp2;
            ies_t ie;
            parse_ies(body + 12, blen - 12, &ie);
            beacon_dtim_count = ie.dtim_count;
            if (ie.dtim_period) cur.dtim_period = ie.dtim_period;
        }
        return;
    }
    if (link_state == LINK_DOWN || memcmp(f + 10, cur.bssid, 6) != 0) return;   /* only from our AP */
    if (memcmp(f + 4, mac_addr, 6) != 0 && !(f[4] & 1)) return;
    if (sub == 0xB && blen >= 6) {                          /* authentication */
        auth_status = (uint16_t)(body[4] | body[5] << 8);
        if ((body[2] | body[3] << 8) == 2) auth_done = 1;
    } else if ((sub == 1 || sub == 3) && blen >= 6) {       /* (re)association response */
        assoc_status = (uint16_t)(body[2] | body[3] << 8);
        assoc_aid = (uint16_t)((body[4] | body[5] << 8) & 0x3FFF);
        assoc_done = 1;
    } else if ((sub == 0xC || sub == 0xA) && blen >= 2) {   /* deauthentication, disassociation */
        lost_reason = (uint16_t)(body[0] | body[1] << 8);
        lost_kind = sub == 0xC ? "the access point ended the connection" : "the access point disassociated us";
        link_lost = 1;
    }
}

static uint8_t eth_frame[14 + 2304];

static void data_rx(const uint8_t *f, uint32_t len, uint32_t status, int signal) {
    if (link_state == LINK_DOWN || !assoc_done) return;
    if ((f[1] & 3) != 2 || memcmp(f + 10, cur.bssid, 6) != 0) return;   /* FromDS, from our AP */
    if (f[0] & 0x40) return;                                /* null data (no payload) */
    uint32_t hl = frame_hdrlen(f);
    if (len < hl + 8) return;
    uint16_t seq = (uint16_t)(f[22] | f[23] << 8);
    if ((f[1] & 0x08) && seq == last_rx_seq) return;        /* a retry we already have */
    last_rx_seq = seq;
    const uint8_t *b = f + hl;
    uint32_t bl = len - hl;
    int prot = (f[1] & 0x40) != 0;
    if (prot) {                                             /* the card decrypted it: check it did */
        if ((status & 0x700) != 0x200 || (status & 0x840) != 0x840 || bl < 8) {
            stats.rx_errors++;
            return;
        }
        b += 8;                                             /* CCMP header */
        bl -= 8;
    }
    const uint8_t *da = f + 4, *sa = f + 16;
    if ((f[0] & 0x80) && (f[hl - 2] & 0x80)) {              /* A-MSDU subframe: its own addresses */
        if (bl < 14) return;
        da = b;
        sa = b + 6;
        uint32_t sl = (uint32_t)(b[12] << 8 | b[13]);
        b += 14;
        bl -= 14;
        if (sl < bl) bl = sl;
    }
    if (bl < 8 || b[0] != 0xAA || b[1] != 0xAA || b[2] != 0x03 || b[3] || b[4] || b[5]) return;
    uint16_t type = (uint16_t)(b[6] << 8 | b[7]);
    b += 8;
    bl -= 8;
    if (!memcmp(sa, mac_addr, 6)) return;                   /* our own broadcast, relayed back */
    if (signal > -100) cur.signal = signal;
    if (type == 0x888E) {
        eapol_rx(b, bl);
        return;
    }
    if ((!prot && ptk_installed) || !port_open || bl > 2304) return;
    memcpy(eth_frame, da, 6);
    memcpy(eth_frame + 6, sa, 6);
    eth_frame[12] = (uint8_t)(type >> 8);
    eth_frame[13] = (uint8_t)type;
    memcpy(eth_frame + 14, b, bl);
    stats.rx_packets++;
    stats.rx_bytes += 14 + bl;
    if (rx_cb && 14 + bl <= 0xFFFF) rx_cb(eth_frame, (uint16_t)(14 + bl));
}

/* ---- Firmware commands for joining ---- */

static const uint8_t legacy_rates[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };

static int status_reply(uint32_t want) {
    return resp_len >= 4 && (*(const uint32_t *)resp_buf & 0xFF) == want ? 0 : -1;
}

static int phy_cmd(uint32_t action) {
    uint8_t c[32];                                          /* PHY_CONTEXT_CMD v4, wide channel info */
    memset(c, 0, sizeof(c));
    *(uint32_t *)(c + 4) = action;
    *(uint32_t *)(c + 8) = (uint32_t)cur.channel;
    c[12] = cur.channel <= 14 ? 1 : 0;                      /* PHY_BAND_24 / PHY_BAND_5; 20 MHz */
    return send_cmd(0, 0x08, c, sizeof(c));
}

static int rlc_cmd(void) {
    uint8_t c[32];                                          /* receive chains for PHY 0 */
    memset(c, 0, sizeof(c));
    *(uint32_t *)(c + 4) = (nvm_rx_chains << 1) | (1u << 10) | (1u << 12);
    return send_cmd(DATA_PATH_GROUP, 0x08, c, sizeof(c));
}

static int mac_cmd(uint32_t action, int assoc) {
    uint8_t c[148];                                         /* MAC_CONTEXT_CMD, station */
    memset(c, 0, sizeof(c));
    *(uint32_t *)(c + 4) = action;
    *(uint32_t *)(c + 8) = 5;                               /* FW_MAC_TYPE_BSS_STA */
    memcpy(c + 16, mac_addr, 6);
    memcpy(c + 24, cur.bssid, 6);
    *(uint32_t *)(c + 32) = cur.channel <= 14 ? 0x0F : 0;   /* ACK rates: CCK 1-11, OFDM 6/12/24 */
    *(uint32_t *)(c + 36) = 0x15;
    *(uint32_t *)(c + 44) = (cur.capab & 0x20) ? 0x20 : 0;  /* short preamble */
    *(uint32_t *)(c + 48) = (cur.capab & 0x400) ? 0x10 : 0; /* short slot */
    *(uint32_t *)(c + 52) = (1u << 2) | (1u << 6);          /* group frames, beacons */
    static const uint8_t fifo[4] = { 2, 1, 3, 4 };          /* BE, BK, VI, VO */
    static const uint8_t ecwmin[4] = { 4, 4, 3, 2 }, ecwmax[4] = { 10, 10, 4, 3 }, aifsn[4] = { 3, 7, 2, 2 };
    static const uint16_t txop[4] = { 0, 0, 94, 47 };
    for (int i = 0; i < 4; i++) {
        uint8_t *ac = c + 60 + 8 * fifo[i];
        *(uint16_t *)ac = (uint16_t)((1 << ecwmin[i]) - 1);
        *(uint16_t *)(ac + 2) = (uint16_t)((1 << ecwmax[i]) - 1);
        ac[4] = aifsn[i];
        ac[5] = (uint8_t)(1 << fifo[i]);
        *(uint16_t *)(ac + 6) = (uint16_t)(txop[i] * 32);
    }
    uint8_t *sta = c + 100;
    uint32_t bi = cur.bintval ? cur.bintval : 100, period = cur.dtim_period ? (uint32_t)cur.dtim_period : 1;
    *(uint32_t *)sta = (uint32_t)assoc;
    if (assoc) {
        uint32_t dtim_off = (uint32_t)beacon_dtim_count * bi * 1024;
        *(uint32_t *)(sta + 4) = beacon_gp2 + dtim_off;
        *(uint64_t *)(sta + 8) = beacon_tsf + dtim_off;
        *(uint32_t *)(sta + 40) = beacon_gp2;
    }
    *(uint32_t *)(sta + 16) = bi;
    *(uint32_t *)(sta + 24) = bi * period;
    *(uint32_t *)(sta + 32) = 10;                           /* listen interval */
    *(uint32_t *)(sta + 36) = assoc ? assoc_aid : 0;
    return send_cmd(0, 0x28, c, sizeof(c));
}

static int binding_cmd(void) {
    uint32_t c[7] = { 0, ACTION_ADD, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 };
    if (send_cmd(0, 0x2B, c, sizeof(c)) != 0) return -1;
    return status_reply(0);
}

static int sta_cmd(int update) {
    uint8_t c[48];                                          /* ADD_STA: the access point is station 0 */
    memset(c, 0, sizeof(c));
    c[0] = (uint8_t)update;
    if (!update) memcpy(c + 8, cur.bssid, 6);
    *(uint32_t *)(c + 24) = (3u << 26) | (3u << 28);        /* 20 MHz, no MIMO */
    if (send_cmd(0, 0x18, c, sizeof(c)) != 0) return -1;
    return status_reply(1);
}

static int enable_txq(void) {
    if (!txq_tfd) {
        txq_tfd = dma_alloc(sizeof(tfh_tfd_t) * TXQ_SLOTS, 256);
        txq_bc = dma_alloc(1024 * 2, 4096);
        txq_bufs = dma_alloc((uint64_t)TXQ_BUF * TXQ_SLOTS, 4096);
        if (!txq_tfd || !txq_bc || !txq_bufs) return -1;
    }
    memset(txq_tfd, 0, sizeof(tfh_tfd_t) * TXQ_SLOTS);
    memset(txq_bc, 0, 1024 * 2);
    uint8_t c[36];                                          /* SCD_QUEUE_CONFIG v3: add */
    memset(c, 0, sizeof(c));
    *(uint32_t *)(c + 4) = 1;                               /* station 0 */
    c[8] = 15;                                              /* the non-QoS ("management") TID */
    *(uint32_t *)(c + 16) = 5;                              /* log2(256) - 3 */
    *(uint64_t *)(c + 20) = (uint64_t)txq_bc;
    *(uint64_t *)(c + 28) = (uint64_t)txq_tfd;
    if (send_cmd(DATA_PATH_GROUP, 0x17, c, sizeof(c)) != 0 || resp_len < 8) return -1;
    txq_hw = txq_done = *(const uint16_t *)(resp_buf + 4);
    txq_id = *(const uint16_t *)resp_buf;
    return 0;
}

static int session_cmd(uint32_t duration_tu) {
    uint32_t c[6] = { 0, ACTION_ADD, 0, duration_tu, 0, 0 };     /* conf: association */
    return send_cmd(MAC_CONF_GROUP, 0x05, c, sizeof(c));
}

/* After association (iwx_run): smart FIFO, multicast, power, rate scaling.
 * Only marking us associated is essential; the rest is best effort. */
static void run_cmds(void) {
    setup_note = 0;
    uint32_t sf[23];                                        /* SF_CFG: full on */
    static const uint32_t full_on[10] = { 2016, 320, 2016, 320, 10016, 2016, 2016, 320, 2016, 320 };
    sf[0] = 1;
    sf[1] = 4096;
    sf[2] = 4096;
    for (int i = 0; i < 10; i++) sf[3 + i] = 1000000;
    memcpy(&sf[13], full_on, sizeof(full_on));
    if (send_cmd(0, 0xD1, sf, sizeof(sf)) != 0) setup_note = "smart FIFO";

    uint8_t mc[12];                                         /* multicast: let everything in */
    memset(mc, 0, sizeof(mc));
    mc[0] = 1;
    mc[3] = 1;
    memcpy(mc + 4, cur.bssid, 6);
    if (send_cmd(0, 0xD0, mc, sizeof(mc)) != 0) setup_note = "multicast filter";

    uint8_t pw[40];                                         /* MAC power: awake, keep-alive 25 s */
    memset(pw, 0, sizeof(pw));
    *(uint16_t *)(pw + 6) = 25;
    if (send_cmd(0, 0xA9, pw, sizeof(pw)) != 0) setup_note = "power settings";

    uint8_t tlc[28];                                        /* TLC_MNG_CONFIG v4: legacy rates */
    memset(tlc, 0, sizeof(tlc));
    tlc[6] = 1;                                             /* chain A */
    uint16_t rates = 0;
    for (int i = 0; i < cur.n_rates; i++)
        for (int k = 0; k < 12; k++)
            if ((cur.rates[i] & 0x7F) == legacy_rates[k]) rates |= (uint16_t)(1 << k);
    if (cur.channel > 14) rates &= 0xFF0;
    if (!rates) rates = cur.channel > 14 ? 0x0FF0 : 0x0FFF;
    *(uint16_t *)(tlc + 10) = rates;
    *(uint16_t *)(tlc + 24) = 2304;                         /* max MPDU length */
    fixed_rate = send_cmd(DATA_PATH_GROUP, 0x0F, tlc, sizeof(tlc)) != 0;
    if (fixed_rate) setup_note = "rate scaling (sending at 6 Mbit/s)";
}

/* Authentication, association and the WPA2 handshake with `cur`. */
static int join(const uint8_t *pmk) {
    link_reset();
    link_state = LINK_JOINING;
    contexts_dirty = 1;
    last_beacon_tick = get_tick();
    beacon_gp2 = 0;
    beacon_tsf = 0;
    beacon_dtim_count = 0;
    last_rx_seq = 0xFFFF;
    join_error = 0;

    if (phy_cmd(ACTION_ADD) != 0) { join_error = "the firmware refused the channel"; return -1; }
    if (cmd_version(DATA_PATH_GROUP, 0x08) == 2 && rlc_cmd() != 0) { join_error = "receive chain setup failed"; return -1; }
    if (mac_cmd(ACTION_ADD, 0) != 0) { join_error = "MAC context setup failed"; return -1; }
    if (binding_cmd() != 0) { join_error = "binding setup failed"; return -1; }
    if (sta_cmd(0) != 0) { join_error = "adding the access point as a station failed"; return -1; }
    if (enable_txq() != 0) { join_error = "no transmit queue from the firmware"; return -1; }
    if (session_cmd((cur.bintval ? cur.bintval : 100) * 9) != 0) { join_error = "air time reservation failed"; return -1; }
    wait_for(&session_started, 1000);

    for (int tries = 0; tries < 3 && !auth_done; tries++) {
        static const uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };     /* open system, sequence 1 */
        if (send_mgmt(0xB0, auth, sizeof(auth)) != 0) { join_error = "could not send"; return -1; }
        wait_for(&auth_done, 500);
        if (fw_error) { join_error = "the firmware stopped"; return -1; }
    }
    if (!auth_done) { join_error = "the access point does not answer (too far away?)"; return -1; }
    if (auth_status) { set_error("the access point refused us (authentication status %u)", auth_status, 0); return -1; }

    uint8_t body[160], ie[64];
    int n = 0, ie_len = 0;
    uint16_t capab = (uint16_t)(0x0001 | (cur.capab & (0x0010 | 0x0020 | 0x0400)));
    body[n++] = (uint8_t)capab;
    body[n++] = (uint8_t)(capab >> 8);
    body[n++] = 10;                                         /* listen interval */
    body[n++] = 0;
    body[n++] = 0;
    body[n++] = (uint8_t)cur.ssid_len;
    memcpy(body + n, cur.ssid, (size_t)cur.ssid_len);
    n += cur.ssid_len;
    int nr = cur.n_rates > 8 ? 8 : cur.n_rates;
    body[n++] = 1;
    body[n++] = (uint8_t)nr;
    memcpy(body + n, cur.rates, (size_t)nr);
    n += nr;
    if (cur.n_rates > 8) {
        body[n++] = 50;
        body[n++] = (uint8_t)(cur.n_rates - 8);
        memcpy(body + n, cur.rates + 8, (size_t)(cur.n_rates - 8));
        n += cur.n_rates - 8;
    }
    if (cur.secured) {
        ie_len = wpa_rsn_ie(ie, cur.group_cipher);
        memcpy(body + n, ie, (size_t)ie_len);
        n += ie_len;
        uint8_t nonce[32];
        random_bytes(nonce, 32);
        wpa_begin(&wpa, pmk, cur.bssid, mac_addr, ie, ie_len, nonce);
    }
    for (int tries = 0; tries < 3 && !assoc_done; tries++) {
        if (send_mgmt(0x00, body, n) != 0) { join_error = "could not send"; return -1; }
        wait_for(&assoc_done, 500);
        if (fw_error) { join_error = "the firmware stopped"; return -1; }
    }
    if (!assoc_done) { join_error = "no answer to the association request"; return -1; }
    if (assoc_status) {
        set_error(assoc_status == 17 ? "the access point is full (status %u)" :
                  "the access point refused to associate (status %u)", assoc_status, 0);
        return -1;
    }

    if (sta_cmd(1) != 0 || mac_cmd(ACTION_MODIFY, 1) != 0) { join_error = "could not mark us associated"; return -1; }
    run_cmds();
    link_state = LINK_HANDSHAKE;

    if (cur.secured) {
        if (pending_eapol_len) {
            uint64_t fl = irq_off();
            int l = pending_eapol_len;
            pending_eapol_len = 0;
            process_eapol(pending_eapol, l);
            irq_restore(fl);
        }
        wait_for(&handshake_done, 8000);
        if (!handshake_done) {
            if (!join_error)
                join_error = link_lost ? "the access point refused the password (handshake failed)"
                                       : "no WPA2 handshake from the access point (wrong password?)";
            return -1;
        }
    }
    link_lost = 0;
    port_open = 1;
    beacons_up = 0;
    link_state = LINK_UP;
    static volatile int never;
    wait_for(&never, 100);                                  /* let the key commands finish */
    return 0;
}

/* ---- Connecting, the network stack, staying connected ---------------------------- */

static void random_bytes(uint8_t *out, int n) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int have_rdrand = (c >> 30) & 1;
    br_sha256_context h;
    br_sha256_init(&h);
    for (int i = 0; i < 16; i++) {
        uint64_t t = rdtsc(), r = 0;
        if (have_rdrand) __asm__ volatile("rdrand %0" : "=r"(r) :: "cc");
        br_sha256_update(&h, &t, 8);
        br_sha256_update(&h, &r, 8);
        udelay(1);
    }
    br_sha256_update(&h, mac_addr, 6);
    uint8_t out32[32];
    br_sha256_out(&h, out32);
    memcpy(out, out32, (size_t)(n < 32 ? n : 32));
}

static char saved_ssid[33];
static uint8_t saved_pmk[32];
static int have_saved, saved_open, want_connected, attached, in_service;
static uint32_t next_retry, retry_delay, dhcp_second;

#define CONFIG_FILE "WIFI.CFG"

static void in_root(char *cwd, int size) {
    if (fat32_get_current_directory(cwd, (uint32_t)size) != 0) cwd[0] = 0;
    fat32_change_directory("/");
}

static void save_config(void) {
    char buf[140], cwd[128];
    int n = k_snprintf(buf, sizeof(buf), "%s\n", saved_ssid);
    for (int i = 0; i < 32 && !saved_open; i++) n += k_snprintf(buf + n, sizeof(buf) - n, "%02x", saved_pmk[i]);
    n += k_snprintf(buf + n, sizeof(buf) - n, "\n");
    in_root(cwd, sizeof(cwd));
    fat32_write_file(CONFIG_FILE, (const uint8_t *)buf, (uint32_t)n);
    if (cwd[0]) fat32_change_directory(cwd);
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int load_config(void) {
    char buf[160], cwd[128];
    in_root(cwd, sizeof(cwd));
    uint32_t size = fat32_get_file_size(CONFIG_FILE);
    int got = -1;
    if (size && size != 0xFFFFFFFFu && size < sizeof(buf)) got = fat32_read_file(CONFIG_FILE, (uint8_t *)buf, size);
    if (cwd[0]) fat32_change_directory(cwd);
    if (got <= 0) return -1;
    buf[got] = 0;
    char *nl = buf;
    while (*nl && *nl != '\n' && *nl != '\r') nl++;
    int len = (int)(nl - buf);
    if (len < 1 || len > 32) return -1;
    memcpy(saved_ssid, buf, (size_t)len);
    saved_ssid[len] = 0;
    while (*nl == '\n' || *nl == '\r') nl++;
    saved_open = 1;
    if (strlen(nl) >= 64) {
        for (int i = 0; i < 32; i++) {
            int hi = hexval(nl[2 * i]), lo = hexval(nl[2 * i + 1]);
            if (hi < 0 || lo < 0) return -1;
            saved_pmk[i] = (uint8_t)(hi << 4 | lo);
        }
        saved_open = 0;
    }
    have_saved = 1;
    return 0;
}

/* The network stack's view: an Ethernet card. */
static void nic_poll(void) {
    if (regs && fw_alive) poll_once();                      /* timer interrupt: already cli */
}
static void nic_mac(uint8_t m[6]) { memcpy(m, mac_addr, 6); }
static void nic_set_rx(nic_rx_cb_t cb) { rx_cb = cb; }
static void nic_stats(nic_stats_t *o) { *o = stats; }
static int nic_tx(const void *frame, uint16_t len) {
    const uint8_t *f = frame;
    if (link_state != LINK_UP || !port_open || len < 14) return -1;
    return send_data(f, f + 6, (uint16_t)(f[12] << 8 | f[13]), f + 14, len - 14, 0);
}
static nic_driver_t wifi_nic = { "Intel Wi-Fi", NIC_IRQ_NONE, nic_poll, nic_mac, nic_set_rx, nic_stats, nic_tx };

/* A freshly started firmware, ready to scan. */
static int bring_up(void) {
    if (fw_ready && !contexts_dirty && !fw_error) return 0;
    if (link_state >= LINK_HANDSHAKE && !fw_error) {
        send_deauth(3);                                     /* "leaving" */
        static volatile int never;
        wait_for(&never, 30);
    }
    if (iwl_start() != 0) return -1;
    return runtime_setup();
}

/* Find `ssid` (the strongest access point we can join) and join it. */
static int do_connect(const char *ssid, const uint8_t *pmk, int talk) {
    join_error = 0;
    if (bring_up() != 0) { join_error = "the card did not start (`wifi start` shows why)"; return -1; }
    if (start_scan() != 0) { join_error = "scanning failed"; return -1; }
    int best = -1, seen = -1;
    for (int i = 0; i < bss_count; i++) {
        if (strcmp(bss_list[i].ssid, ssid) != 0) continue;
        seen = i;
        if (bss_list[i].why_not) continue;
        if (best < 0 || bss_list[i].signal > bss_list[best].signal) best = i;
    }
    if (best < 0) {
        if (seen >= 0)
            k_snprintf(join_error_buf, sizeof(join_error_buf), "\"%s\" cannot be joined: %s", ssid, bss_list[seen].why_not);
        else k_snprintf(join_error_buf, sizeof(join_error_buf), "no network called \"%s\" is in range", ssid);
        join_error = join_error_buf;
        return -1;
    }
    cur = bss_list[best];
    if (cur.secured && !pmk) { join_error = "this network needs a password"; return -1; }
    if (talk)
        kprintf("Joining %s on channel %d (%d dBm, %s)...\n", cur.ssid, cur.channel, cur.signal, cur.security);
    return join(pmk);
}

static void attach_network(void) {
    if (!attached) {
        nic_attach(&wifi_nic);
        net_init();
        attached = 1;
    }
    net_configure();
    dhcp_second = get_seconds();
}

int iwl_connect(const char *ssid, const char *password) {
    if (wifi_count() == 0) {
        kprintf("No Wi-Fi adapter found.\n");
        return -1;
    }
    uint8_t pmk[32];
    const uint8_t *use = 0;
    if (password && password[0]) {
        int n = (int)strlen(password);
        if (n < 8 || n > 63) {
            kprintf("A WPA2 password has 8 to 63 characters.\n");
            return -1;
        }
        wpa_passphrase(password, (const uint8_t *)ssid, (int)strlen(ssid), pmk);
        use = pmk;
    } else if (have_saved && !saved_open && strcmp(saved_ssid, ssid) == 0) {
        use = saved_pmk;
    }
    want_connected = 0;
    kprintf("Looking for \"%s\"...\n", ssid);
    quiet = 1;
    int r = do_connect(ssid, use, 1);
    quiet = 0;
    if (r != 0) {
        kprintf("Could not connect: %s.\n", join_error ? join_error : "unknown error");
        return -1;
    }
    kprintf("Connected to \"%s\".\n", cur.ssid);
    kstrncpy(saved_ssid, cur.ssid, sizeof(saved_ssid));
    saved_open = !cur.secured;
    if (use) memcpy(saved_pmk, use, 32);
    have_saved = 1;
    save_config();
    want_connected = 1;
    retry_delay = 0;
    attach_network();
    return 0;
}

int iwl_has_saved(const char *ssid) {
    if (!have_saved) load_config();
    return have_saved && strcmp(saved_ssid, ssid) == 0;
}

void iwl_disconnect(void) {
    want_connected = 0;
    if (link_state == LINK_DOWN) {
        kprintf("Wi-Fi is not connected.\n");
        return;
    }
    if (link_state >= LINK_HANDSHAKE && !fw_error) {
        send_deauth(3);
        static volatile int never;
        wait_for(&never, 30);
    }
    kprintf("Disconnected from \"%s\".\n", cur.ssid);
    link_state = LINK_DOWN;
    port_open = 0;
}

void iwl_forget(void) {
    char cwd[128];
    want_connected = 0;
    have_saved = 0;
    in_root(cwd, sizeof(cwd));
    int existed = fat32_file_exists(CONFIG_FILE);
    if (existed) fat32_delete_file(CONFIG_FILE);
    if (cwd[0]) fat32_change_directory(cwd);
    kprintf(existed ? "Forgot the saved network.\n" : "No network was saved.\n");
}

void iwl_print_status(void) {
    if (link_state != LINK_UP) {
        kprintf("Wi-Fi: not connected%s%s%s\n", have_saved ? " (saved network: \"" : "", have_saved ? saved_ssid : "",
                have_saved ? "\")" : "");
        if (want_connected) kprintf("  reconnecting (last error: %s)\n", join_error ? join_error : "none");
        return;
    }
    char ip[16], gw[16];
    net_fmt_ip(ip, net_get_config()->ip);
    net_fmt_ip(gw, net_get_config()->gateway);
    kprintf("Wi-Fi: connected to \"%s\"\n", cur.ssid);
    kprintf("  access point  %02x:%02x:%02x:%02x:%02x:%02x, channel %d (%s GHz), %d dBm\n", cur.bssid[0],
            cur.bssid[1], cur.bssid[2], cur.bssid[3], cur.bssid[4], cur.bssid[5], cur.channel,
            cur.channel <= 14 ? "2.4" : "5", cur.signal);
    kprintf("  security      %s\n", cur.secured ? "WPA2-Personal (AES-CCMP, encrypted by the card)" : "none (open network)");
    kprintf("  address       %s, gateway %s\n", ip, gw);
    kprintf("  traffic       %u packets in, %u out, %u errors\n", stats.rx_packets, stats.tx_packets,
            stats.rx_errors + stats.tx_errors);
    if (setup_note) kprintf("  note          the firmware refused: %s\n", setup_note);
}

/* Called while the shell waits for a key: notice a lost connection and get it back. */
void iwl_service(void) {
    if (!want_connected || in_service) return;
    in_service = 1;
    uint32_t now = get_tick();
    if (link_state == LINK_UP) {
        const char *why = 0;
        if (fw_error) why = "the card's firmware stopped";
        else if (link_lost) why = lost_kind;
        else if (beacons_up && (uint32_t)(now - last_beacon_tick) > 10 * TIMER_FREQ) why = "no signal from the access point";
        if (!why) {
            uint32_t lease = net_lease_seconds();           /* renew the address halfway through */
            if (lease && get_seconds() - dhcp_second > lease / 2) {
                net_configure();
                dhcp_second = get_seconds();
            }
            in_service = 0;
            return;
        }
        kprintf("\n[Wi-Fi] lost \"%s\": %s", saved_ssid, why);
        if (lost_reason) kprintf(" (reason %u)", lost_reason);
        kprintf(". Reconnecting...\n");
        link_state = LINK_DOWN;
        port_open = 0;
        next_retry = now;
    }
    if ((int32_t)(now - next_retry) < 0) {
        in_service = 0;
        return;
    }
    quiet = 1;
    int r = do_connect(saved_ssid, saved_open ? 0 : saved_pmk, 0);
    quiet = 0;
    if (r == 0) {
        kprintf("[Wi-Fi] reconnected to \"%s\".\n", saved_ssid);
        retry_delay = 0;
        attach_network();
    } else {
        retry_delay = retry_delay ? (retry_delay * 2 > 60 ? 60 : retry_delay * 2) : 5;
        next_retry = get_tick() + retry_delay * TIMER_FREQ;
        kprintf("[Wi-Fi] could not reconnect (%s); next try in %u s.\n", join_error ? join_error : "?", retry_delay);
    }
    in_service = 0;
}

int iwl_autoconnect(char *how, int size) {
    if (wifi_count() == 0 || wifi_get(0)->pci.vendor != 0x8086) return 1;
    if (load_config() != 0) return 1;
    quiet = 1;
    int r = do_connect(saved_ssid, saved_open ? 0 : saved_pmk, 0);
    quiet = 0;
    if (r != 0) {
        k_snprintf(how, (size_t)size, "\"%s\": %s (will keep trying)", saved_ssid, join_error ? join_error : "failed");
        want_connected = 1;
        next_retry = get_tick() + 10 * TIMER_FREQ;
        retry_delay = 10;
        return -1;
    }
    want_connected = 1;
    attach_network();
    char ip[16];
    net_fmt_ip(ip, net_get_config()->ip);
    k_snprintf(how, (size_t)size, "connected to \"%s\" (channel %d, %d dBm), address %s", cur.ssid, cur.channel,
               cur.signal, ip);
    return 0;
}
