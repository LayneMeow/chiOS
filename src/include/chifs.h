#ifndef _CHIFS_H
#define _CHIFS_H

#include <block.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHIFS_BLOCK_SIZE 4096
#define CHIFS_NAME_MAX   63
#define CHIFS_DIRECT     11
#define CHIFS_MAX_FILE   ((uint64_t)(CHIFS_DIRECT + CHIFS_BLOCK_SIZE / 4) * CHIFS_BLOCK_SIZE)

struct chifs_dirent {
    char name[CHIFS_NAME_MAX + 1];
    uint64_t size;
    uint32_t blocks;
};

struct chifs_info {
    char label[32];
    char device[12];
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t total_inodes;
    uint32_t used_inodes;
};

bool chifs_format(const struct block_device *dev, const char *label);

bool chifs_mount(const struct block_device *dev);
void chifs_unmount(void);
bool chifs_mounted(void);

bool chifs_stat(struct chifs_info *out);

int chifs_list(struct chifs_dirent *out, int max);

bool chifs_exists(const char *name);

bool chifs_create(const char *name);

bool chifs_remove(const char *name);

int64_t chifs_read(const char *name, uint64_t offset, void *buf, size_t len);

int64_t chifs_write(const char *name, uint64_t offset, const void *buf, size_t len);

int64_t chifs_read_file(const char *name, void *buf, size_t len);
int64_t chifs_write_file(const char *name, const void *buf, size_t len);

int64_t chifs_size(const char *name);

#endif
