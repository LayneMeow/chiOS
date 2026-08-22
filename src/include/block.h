#ifndef _BLOCK_H
#define _BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_MAX_DEVICES 16

enum block_kind {
    BLOCK_ATA,
    BLOCK_AHCI,
    BLOCK_NVME,
    BLOCK_UFS
};

struct block_device {
    enum block_kind kind;
    int unit;
    uint64_t sectors;
    uint32_t sector_size;
    char name[12];
    char model[41];
};

void block_scan(void);

int block_device_count(void);

const struct block_device *block_device_get(int index);

const struct block_device *block_device_by_name(const char *name);

bool block_read(const struct block_device *dev, uint64_t lba, size_t count, void *buf);
bool block_write(const struct block_device *dev, uint64_t lba, size_t count, const void *buf);

uint64_t block_capacity(const struct block_device *dev);

#endif
