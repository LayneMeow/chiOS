#include <ahci.h>
#include <ata.h>
#include <block.h>
#include <nvme.h>
#include <stdio.h>
#include <string.h>
#include <ufs.h>

static struct block_device devices[BLOCK_MAX_DEVICES];
static int device_count;

static struct block_device *add(enum block_kind kind, int unit, const char *prefix,
                                 uint64_t sectors, uint32_t sector_size,
                                 const char *model) {
    if (device_count >= BLOCK_MAX_DEVICES || sectors == 0) {
        return NULL;
    }

    struct block_device *dev = &devices[device_count];

    memset(dev, 0, sizeof *dev);
    dev->kind = kind;
    dev->unit = unit;
    dev->sectors = sectors;
    dev->sector_size = sector_size;

    int seen = 0;

    for (int i = 0; i < device_count; i++) {
        if (devices[i].kind == kind) {
            seen++;
        }
    }

    snprintf(dev->name, sizeof dev->name, "%s%d", prefix, seen);
    snprintf(dev->model, sizeof dev->model, "%s", model);

    device_count++;

    return dev;
}

void block_scan(void) {
    device_count = 0;

    for (int bus = 0; bus < ATA_BUS_COUNT; bus++) {
        for (int drive = 0; drive < ATA_DRIVE_COUNT; drive++) {
            const struct ata_drive *info = ata_drive_info(bus, drive);

            if (info != NULL) {
                add(BLOCK_ATA, bus * ATA_DRIVE_COUNT + drive, "ata",
                    info->sectors, ATA_SECTOR_SIZE, info->model);
            }
        }
    }

    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        const struct ahci_drive *info = ahci_drive_info(port);

        if (info != NULL) {
            add(BLOCK_AHCI, port, "ahci", info->sectors, AHCI_SECTOR_SIZE, info->model);
        }
    }

    const struct nvme_drive *nvme = nvme_drive_info();

    if (nvme != NULL) {
        add(BLOCK_NVME, 0, "nvme", nvme->sectors, nvme->sector_size, nvme->model);
    }

    for (int lun = 0; lun < UFS_MAX_LUNS; lun++) {
        const struct ufs_lun *info = ufs_lun_info(lun);

        if (info != NULL) {
            add(BLOCK_UFS, lun, "ufs", info->sectors, info->sector_size, info->model);
        }
    }
}

int block_device_count(void) {
    return device_count;
}

const struct block_device *block_device_get(int index) {
    if (index < 0 || index >= device_count) {
        return NULL;
    }

    return &devices[index];
}

const struct block_device *block_device_by_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }

    return NULL;
}

uint64_t block_capacity(const struct block_device *dev) {
    return dev == NULL ? 0 : dev->sectors * dev->sector_size;
}

static bool in_range(const struct block_device *dev, uint64_t lba, size_t count) {
    return dev != NULL && count != 0
        && lba + count >= lba && lba + count <= dev->sectors;
}

bool block_read(const struct block_device *dev, uint64_t lba, size_t count, void *buf) {
    if (!in_range(dev, lba, count) || buf == NULL) {
        return false;
    }

    switch (dev->kind) {
    case BLOCK_ATA:
        return ata_read_sectors(dev->unit / ATA_DRIVE_COUNT, dev->unit % ATA_DRIVE_COUNT,
                                (uint32_t)lba, count, buf);
    case BLOCK_AHCI:
        return ahci_read_sectors(dev->unit, lba, count, buf);
    case BLOCK_NVME:
        return nvme_read_sectors(lba, count, buf);
    case BLOCK_UFS:
        return ufs_read_sectors(dev->unit, lba, count, buf);
    }

    return false;
}

bool block_write(const struct block_device *dev, uint64_t lba, size_t count, const void *buf) {
    if (!in_range(dev, lba, count) || buf == NULL) {
        return false;
    }

    switch (dev->kind) {
    case BLOCK_ATA:
        return ata_write_sectors(dev->unit / ATA_DRIVE_COUNT, dev->unit % ATA_DRIVE_COUNT,
                                 (uint32_t)lba, count, buf);
    case BLOCK_AHCI:
        return ahci_write_sectors(dev->unit, lba, count, buf);
    case BLOCK_NVME:
        return nvme_write_sectors(lba, count, buf);
    case BLOCK_UFS:
        return ufs_write_sectors(dev->unit, lba, count, buf);
    }

    return false;
}
