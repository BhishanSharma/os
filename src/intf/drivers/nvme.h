// nvme.h - NVMe solid-state drives (PCI class 01/08/02)
//
// The controller is driven through queues in memory: an admin queue pair for
// setup (identify, create I/O queues) and one I/O queue pair for reads and
// writes. Polled: a command is done when its completion entry's phase bit
// flips. Only namespace 1 is used (what every laptop drive has).
#ifndef NVME_H
#define NVME_H

#include <stdint.h>

/* Find and start the first NVMe controller. 0 on success; `how` describes it. */
int nvme_init(char *how, int size);
int nvme_present(void);

uint64_t nvme_sectors(void);          /* namespace size in 512-byte sectors */

/* 512-byte sectors (also on drives with 4 KiB blocks: read-modify-write is
 * not done, so writes there must be 4 KiB aligned). 0 on success. */
int nvme_read(uint64_t lba, uint32_t count, void *buf);
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

/* nvme_read with the signature part_scan() wants. */
int nvme_partition_reader(uint64_t lba, uint32_t count, void *buf);

/* `nvme`: model, size, partitions. */
void nvme_print_info(void);

#endif
