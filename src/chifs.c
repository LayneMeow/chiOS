#include <chifs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIFS_MAGIC "ChiFS-01"

struct chifs_super {
    char     magic[8];
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t inode_count;
    uint32_t data_start;
    char     label[32];
};

#define INODE_FREE 0u
#define INODE_FILE 1u

struct chifs_inode {
    uint32_t flags;
    uint32_t block_count;
    uint64_t size;
    uint32_t direct[CHIFS_DIRECT];
    uint32_t indirect;
    char     name[CHIFS_NAME_MAX + 1];
};

_Static_assert(sizeof(struct chifs_inode) == 128, "inode must divide a block evenly");
_Static_assert(sizeof(struct chifs_super) <= CHIFS_BLOCK_SIZE, "superblock must fit a block");

#define INODES_PER_BLOCK (CHIFS_BLOCK_SIZE / (int)sizeof(struct chifs_inode))
#define POINTERS_PER_BLOCK (CHIFS_BLOCK_SIZE / 4)
#define BITS_PER_BLOCK (CHIFS_BLOCK_SIZE * 8)

static const struct block_device *device;
static struct chifs_super super;
static bool mounted;

static uint8_t *data_buf;
static uint8_t *inode_buf;
static uint8_t *bitmap_buf;
static uint8_t *indirect_buf;

#define NO_BLOCK 0xFFFFFFFFu
static uint32_t inode_cached = NO_BLOCK;
static uint32_t bitmap_cached = NO_BLOCK;

static uint32_t alloc_hint;

static void forget_caches(void) {
    inode_cached = NO_BLOCK;
    bitmap_cached = NO_BLOCK;
    alloc_hint = 0;
}

static bool buffers_ready(void) {
    if (data_buf != NULL) {
        return true;
    }

    data_buf = malloc(CHIFS_BLOCK_SIZE);
    inode_buf = malloc(CHIFS_BLOCK_SIZE);
    bitmap_buf = malloc(CHIFS_BLOCK_SIZE);
    indirect_buf = malloc(CHIFS_BLOCK_SIZE);

    return data_buf != NULL && inode_buf != NULL
        && bitmap_buf != NULL && indirect_buf != NULL;
}

static uint32_t sectors_per_block(const struct block_device *dev) {
    return CHIFS_BLOCK_SIZE / dev->sector_size;
}

static bool block_read_one(const struct block_device *dev, uint64_t block, void *buf) {
    uint32_t span = sectors_per_block(dev);

    return block_read(dev, block * span, span, buf);
}

static bool block_write_one(const struct block_device *dev, uint64_t block, const void *buf) {
    uint32_t span = sectors_per_block(dev);

    return block_write(dev, block * span, span, buf);
}

static bool read_block(uint64_t block, void *buf) {
    return mounted && block < super.total_blocks && block_read_one(device, block, buf);
}

static bool write_block(uint64_t block, const void *buf) {
    return mounted && block < super.total_blocks && block_write_one(device, block, buf);
}

static bool bitmap_load(uint32_t index) {
    if (index >= super.bitmap_blocks) {
        return false;
    }
    if (index == bitmap_cached) {
        return true;
    }
    if (!read_block(super.bitmap_start + index, bitmap_buf)) {
        bitmap_cached = NO_BLOCK;
        return false;
    }

    bitmap_cached = index;

    return true;
}

static bool bitmap_test(uint32_t block, bool *used) {
    if (!bitmap_load(block / BITS_PER_BLOCK)) {
        return false;
    }

    uint32_t bit = block % BITS_PER_BLOCK;

    *used = (bitmap_buf[bit / 8] & (1u << (bit % 8))) != 0;

    return true;
}

static bool bitmap_set(uint32_t block, bool used) {
    uint32_t index = block / BITS_PER_BLOCK;

    if (!bitmap_load(index)) {
        return false;
    }

    uint32_t bit = block % BITS_PER_BLOCK;
    uint8_t mask = (uint8_t)(1u << (bit % 8));

    if (used) {
        bitmap_buf[bit / 8] |= mask;
    } else {
        bitmap_buf[bit / 8] &= (uint8_t)~mask;
    }

    return write_block(super.bitmap_start + index, bitmap_buf);
}

static uint32_t block_alloc(void) {
    uint64_t span = super.total_blocks - super.data_start;

    if (alloc_hint >= span) {
        alloc_hint = 0;
    }

    for (uint64_t step = 0; step < span; step++) {
        uint64_t offset = (alloc_hint + step) % span;
        uint64_t block = super.data_start + offset;
        bool used;

        if (!bitmap_test((uint32_t)block, &used)) {
            return 0;
        }
        if (used) {
            continue;
        }
        if (!bitmap_set((uint32_t)block, true)) {
            return 0;
        }

        memset(data_buf, 0, CHIFS_BLOCK_SIZE);

        if (!write_block(block, data_buf)) {
            bitmap_set((uint32_t)block, false);
            return 0;
        }

        alloc_hint = (uint32_t)(offset + 1);

        return (uint32_t)block;
    }

    return 0;
}

static void block_free(uint32_t block) {
    if (block >= super.data_start && block < super.total_blocks) {
        bitmap_set(block, false);
        alloc_hint = block - super.data_start;
    }
}

static uint64_t count_free_blocks(void) {
    uint64_t free_blocks = 0;

    for (uint32_t index = 0; index < super.bitmap_blocks; index++) {
        if (!bitmap_load(index)) {
            return free_blocks;
        }

        for (int byte = 0; byte < CHIFS_BLOCK_SIZE; byte++) {
            uint8_t bits = bitmap_buf[byte];

            if (bits == 0xFF) {
                continue;
            }

            for (int bit = 0; bit < 8; bit++) {
                if ((bits & (1u << bit)) == 0) {
                    free_blocks++;
                }
            }
        }
    }

    return free_blocks;
}

static bool inode_load(uint32_t index) {
    uint32_t which = index / INODES_PER_BLOCK;

    if (index >= super.inode_count) {
        return false;
    }
    if (which == inode_cached) {
        return true;
    }
    if (!read_block(super.inode_start + which, inode_buf)) {
        inode_cached = NO_BLOCK;
        return false;
    }

    inode_cached = which;

    return true;
}

static bool inode_read(uint32_t index, struct chifs_inode *out) {
    if (!inode_load(index)) {
        return false;
    }

    memcpy(out, inode_buf + (index % INODES_PER_BLOCK) * sizeof *out, sizeof *out);

    return true;
}

static bool inode_write(uint32_t index, const struct chifs_inode *in) {
    if (!inode_load(index)) {
        return false;
    }

    memcpy(inode_buf + (index % INODES_PER_BLOCK) * sizeof *in, in, sizeof *in);

    return write_block(super.inode_start + index / INODES_PER_BLOCK, inode_buf);
}

static bool name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    size_t len = strlen(name);

    if (len > CHIFS_NAME_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
            return false;
        }
    }

    return true;
}

static bool inode_find(const char *name, uint32_t *index_out, struct chifs_inode *out) {
    for (uint32_t i = 0; i < super.inode_count; i++) {
        struct chifs_inode inode;

        if (!inode_read(i, &inode)) {
            return false;
        }
        if (inode.flags != INODE_FILE) {
            continue;
        }

        inode.name[CHIFS_NAME_MAX] = '\0';

        if (strcmp(inode.name, name) == 0) {
            if (index_out != NULL) {
                *index_out = i;
            }
            if (out != NULL) {
                *out = inode;
            }

            return true;
        }
    }

    return false;
}

static bool inode_find_free(uint32_t *index_out) {
    for (uint32_t i = 0; i < super.inode_count; i++) {
        struct chifs_inode inode;

        if (!inode_read(i, &inode)) {
            return false;
        }
        if (inode.flags == INODE_FREE) {
            *index_out = i;
            return true;
        }
    }

    return false;
}

static bool file_block_lookup(const struct chifs_inode *inode, uint32_t n, uint32_t *out) {
    if (n < CHIFS_DIRECT) {
        *out = inode->direct[n];
        return true;
    }

    n -= CHIFS_DIRECT;

    if (n >= POINTERS_PER_BLOCK || inode->indirect == 0) {
        *out = 0;
        return n < POINTERS_PER_BLOCK;
    }

    if (!read_block(inode->indirect, indirect_buf)) {
        return false;
    }

    memcpy(out, indirect_buf + n * 4, 4);

    return true;
}

static bool file_block_assign(struct chifs_inode *inode, uint32_t n, uint32_t block) {
    if (n < CHIFS_DIRECT) {
        inode->direct[n] = block;
        return true;
    }

    n -= CHIFS_DIRECT;

    if (n >= POINTERS_PER_BLOCK) {
        return false;
    }

    if (inode->indirect == 0) {
        inode->indirect = block_alloc();

        if (inode->indirect == 0) {
            return false;
        }

        inode->block_count++;
    }

    if (!read_block(inode->indirect, indirect_buf)) {
        return false;
    }

    memcpy(indirect_buf + n * 4, &block, 4);

    return write_block(inode->indirect, indirect_buf);
}

static uint32_t file_block(struct chifs_inode *inode, uint32_t n, bool grow) {
    uint32_t block = 0;

    if (!file_block_lookup(inode, n, &block)) {
        return 0;
    }
    if (block != 0 || !grow) {
        return block;
    }

    block = block_alloc();

    if (block == 0) {
        return 0;
    }

    if (!file_block_assign(inode, n, block)) {
        block_free(block);
        return 0;
    }

    inode->block_count++;

    return block;
}

static void file_release(struct chifs_inode *inode) {
    uint32_t total = CHIFS_DIRECT + POINTERS_PER_BLOCK;

    for (uint32_t n = 0; n < total; n++) {
        uint32_t block = 0;

        if (!file_block_lookup(inode, n, &block)) {
            break;
        }
        if (block != 0) {
            block_free(block);
        }
    }

    if (inode->indirect != 0) {
        block_free(inode->indirect);
    }

    memset(inode->direct, 0, sizeof inode->direct);
    inode->indirect = 0;
    inode->block_count = 0;
    inode->size = 0;
}

static uint32_t divide_up(uint64_t value, uint64_t by) {
    return (uint32_t)((value + by - 1) / by);
}

bool chifs_format(const struct block_device *dev, const char *label) {
    if (dev == NULL || dev->sector_size == 0 || CHIFS_BLOCK_SIZE % dev->sector_size != 0) {
        return false;
    }
    if (!buffers_ready()) {
        return false;
    }

    uint64_t total = dev->sectors / sectors_per_block(dev);

    if (total < 16) {
        return false;
    }
    if (total > 0xFFFFFFFFu) {
        total = 0xFFFFFFFFu;
    }

    uint32_t inodes = (uint32_t)(total / 16);

    if (inodes < 32) {
        inodes = 32;
    }
    if (inodes > 8192) {
        inodes = 8192;
    }

    struct chifs_super sb;

    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, CHIFS_MAGIC, 8);
    sb.version = 1;
    sb.block_size = CHIFS_BLOCK_SIZE;
    sb.total_blocks = total;
    sb.bitmap_start = 1;
    sb.bitmap_blocks = divide_up(total, BITS_PER_BLOCK);
    sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_blocks = divide_up(inodes, INODES_PER_BLOCK);
    sb.inode_count = sb.inode_blocks * INODES_PER_BLOCK;
    sb.data_start = sb.inode_start + sb.inode_blocks;

    if (sb.data_start >= total) {
        return false;
    }

    snprintf(sb.label, sizeof sb.label, "%s", label != NULL ? label : "chifs");

    memset(data_buf, 0, CHIFS_BLOCK_SIZE);

    for (uint32_t i = 0; i < sb.inode_blocks; i++) {
        if (!block_write_one(dev, sb.inode_start + i, data_buf)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        memset(data_buf, 0, CHIFS_BLOCK_SIZE);

        for (uint32_t bit = 0; bit < BITS_PER_BLOCK; bit++) {
            uint64_t block = (uint64_t)i * BITS_PER_BLOCK + bit;

            if (block < sb.data_start || block >= total) {
                data_buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
            }
        }

        if (!block_write_one(dev, sb.bitmap_start + i, data_buf)) {
            return false;
        }
    }

    memset(data_buf, 0, CHIFS_BLOCK_SIZE);
    memcpy(data_buf, &sb, sizeof sb);

    forget_caches();

    return block_write_one(dev, 0, data_buf);
}

bool chifs_mount(const struct block_device *dev) {
    if (dev == NULL || dev->sector_size == 0 || CHIFS_BLOCK_SIZE % dev->sector_size != 0) {
        return false;
    }
    if (!buffers_ready() || !block_read_one(dev, 0, data_buf)) {
        return false;
    }

    struct chifs_super sb;

    memcpy(&sb, data_buf, sizeof sb);

    if (memcmp(sb.magic, CHIFS_MAGIC, 8) != 0 || sb.version != 1
     || sb.block_size != CHIFS_BLOCK_SIZE) {
        return false;
    }

    if (sb.total_blocks > dev->sectors / sectors_per_block(dev)
     || sb.data_start >= sb.total_blocks
     || sb.inode_start < sb.bitmap_start
     || sb.data_start < sb.inode_start + sb.inode_blocks
     || sb.inode_count != sb.inode_blocks * INODES_PER_BLOCK) {
        return false;
    }

    super = sb;
    device = dev;
    mounted = true;
    forget_caches();

    return true;
}

void chifs_unmount(void) {
    mounted = false;
    device = NULL;
    forget_caches();
}

bool chifs_mounted(void) {
    return mounted;
}

bool chifs_stat(struct chifs_info *out) {
    if (!mounted || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof *out);
    memcpy(out->label, super.label, sizeof out->label - 1);
    memcpy(out->device, device->name, sizeof out->device - 1);
    out->total_blocks = super.total_blocks;
    out->free_blocks = count_free_blocks();
    out->total_inodes = super.inode_count;

    for (uint32_t i = 0; i < super.inode_count; i++) {
        struct chifs_inode inode;

        if (inode_read(i, &inode) && inode.flags == INODE_FILE) {
            out->used_inodes++;
        }
    }

    return true;
}

int chifs_list(struct chifs_dirent *out, int max) {
    if (!mounted) {
        return -1;
    }

    int found = 0;

    for (uint32_t i = 0; i < super.inode_count && found < max; i++) {
        struct chifs_inode inode;

        if (!inode_read(i, &inode) || inode.flags != INODE_FILE) {
            continue;
        }

        memcpy(out[found].name, inode.name, CHIFS_NAME_MAX);
        out[found].name[CHIFS_NAME_MAX] = '\0';
        out[found].size = inode.size;
        out[found].blocks = inode.block_count;
        found++;
    }

    return found;
}

bool chifs_exists(const char *name) {
    return mounted && name_valid(name) && inode_find(name, NULL, NULL);
}

int64_t chifs_size(const char *name) {
    struct chifs_inode inode;

    if (!mounted || !name_valid(name) || !inode_find(name, NULL, &inode)) {
        return -1;
    }

    return (int64_t)inode.size;
}

bool chifs_create(const char *name) {
    uint32_t index;

    if (!mounted || !name_valid(name) || inode_find(name, NULL, NULL)) {
        return false;
    }
    if (!inode_find_free(&index)) {
        return false;
    }

    struct chifs_inode inode;

    memset(&inode, 0, sizeof inode);
    inode.flags = INODE_FILE;
    snprintf(inode.name, sizeof inode.name, "%s", name);

    return inode_write(index, &inode);
}

bool chifs_remove(const char *name) {
    uint32_t index;
    struct chifs_inode inode;

    if (!mounted || !name_valid(name) || !inode_find(name, &index, &inode)) {
        return false;
    }

    file_release(&inode);
    memset(&inode, 0, sizeof inode);

    return inode_write(index, &inode);
}

int64_t chifs_read(const char *name, uint64_t offset, void *buf, size_t len) {
    uint32_t index;
    struct chifs_inode inode;

    if (!mounted || buf == NULL || !name_valid(name)
     || !inode_find(name, &index, &inode)) {
        return -1;
    }

    if (offset >= inode.size) {
        return 0;
    }

    uint64_t remaining = inode.size - offset;

    if (len > remaining) {
        len = (size_t)remaining;
    }

    uint8_t *cursor = buf;
    size_t done = 0;

    while (done < len) {
        uint32_t n = (uint32_t)((offset + done) / CHIFS_BLOCK_SIZE);
        uint32_t within = (uint32_t)((offset + done) % CHIFS_BLOCK_SIZE);
        size_t piece = CHIFS_BLOCK_SIZE - within;

        if (piece > len - done) {
            piece = len - done;
        }

        uint32_t block = file_block(&inode, n, false);

        if (block == 0) {

            memset(cursor + done, 0, piece);
        } else {
            if (!read_block(block, data_buf)) {
                return -1;
            }

            memcpy(cursor + done, data_buf + within, piece);
        }

        done += piece;
    }

    return (int64_t)done;
}

int64_t chifs_write(const char *name, uint64_t offset, const void *buf, size_t len) {
    uint32_t index;
    struct chifs_inode inode;

    if (!mounted || buf == NULL || !name_valid(name)) {
        return -1;
    }
    if (offset + len > CHIFS_MAX_FILE || offset + len < offset) {
        return -1;
    }

    if (!inode_find(name, &index, &inode)) {
        if (!chifs_create(name) || !inode_find(name, &index, &inode)) {
            return -1;
        }
    }

    const uint8_t *cursor = buf;
    size_t done = 0;

    while (done < len) {
        uint32_t n = (uint32_t)((offset + done) / CHIFS_BLOCK_SIZE);
        uint32_t within = (uint32_t)((offset + done) % CHIFS_BLOCK_SIZE);
        size_t piece = CHIFS_BLOCK_SIZE - within;

        if (piece > len - done) {
            piece = len - done;
        }

        uint32_t block = file_block(&inode, n, true);

        if (block == 0) {
            break;
        }

        if (piece < CHIFS_BLOCK_SIZE) {
            if (!read_block(block, data_buf)) {
                return -1;
            }
        }

        memcpy(data_buf + within, cursor + done, piece);

        if (!write_block(block, data_buf)) {
            return -1;
        }

        done += piece;
    }

    if (offset + done > inode.size) {
        inode.size = offset + done;
    }

    if (!inode_write(index, &inode)) {
        return -1;
    }

    return (int64_t)done;
}

int64_t chifs_read_file(const char *name, void *buf, size_t len) {
    return chifs_read(name, 0, buf, len);
}

int64_t chifs_write_file(const char *name, const void *buf, size_t len) {

    if (mounted && name_valid(name) && chifs_exists(name)) {
        chifs_remove(name);
    }

    return chifs_write(name, 0, buf, len);
}
