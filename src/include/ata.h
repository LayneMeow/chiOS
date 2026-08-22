#ifndef _ATA_H
#define _ATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATA_SECTOR_SIZE 512

#define ATA_BUS_PRIMARY   0
#define ATA_BUS_SECONDARY 1
#define ATA_BUS_COUNT     2

#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1
#define ATA_DRIVE_COUNT  2

struct ata_drive {
    bool present;
    bool lba48;
    uint32_t sectors;
    char model[41];
};

bool ata_init(void);

bool ata_drive_present(int bus, int drive);

const struct ata_drive *ata_drive_info(int bus, int drive);

bool ata_read_sectors(int bus, int drive, uint32_t lba, size_t count, void *buf);
bool ata_write_sectors(int bus, int drive, uint32_t lba, size_t count, const void *buf);

#endif
