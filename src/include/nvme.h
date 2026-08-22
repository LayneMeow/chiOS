#ifndef _NVME_H
#define _NVME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nvme_drive {
    bool present;
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t nsid;
    char model[41];
};

bool nvme_init(void);

const char *nvme_status(void);

bool nvme_drive_present(void);

const struct nvme_drive *nvme_drive_info(void);

bool nvme_read_sectors(uint64_t lba, size_t count, void *buf);
bool nvme_write_sectors(uint64_t lba, size_t count, const void *buf);

#endif
