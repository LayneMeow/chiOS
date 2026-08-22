#ifndef _AHCI_H
#define _AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AHCI_SECTOR_SIZE 512
#define AHCI_MAX_PORTS   32

struct ahci_drive {
    bool present;
    uint64_t sectors;
    char model[41];
};

bool ahci_init(void);

bool ahci_drive_present(int port);

const char *ahci_status(void);

const struct ahci_drive *ahci_drive_info(int port);

bool ahci_read_sectors(int port, uint64_t lba, size_t count, void *buf);
bool ahci_write_sectors(int port, uint64_t lba, size_t count, const void *buf);

#endif
