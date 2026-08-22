#ifndef _UFS_H
#define _UFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UFS_MAX_LUNS 8

struct ufs_lun {
    bool present;
    uint64_t sectors;
    uint32_t sector_size;
    char model[41];
};

bool ufs_init(void);

const char *ufs_status(void);

bool ufs_lun_present(int lun);

const struct ufs_lun *ufs_lun_info(int lun);

bool ufs_read_sectors(int lun, uint64_t lba, size_t count, void *buf);
bool ufs_write_sectors(int lun, uint64_t lba, size_t count, const void *buf);

#endif
