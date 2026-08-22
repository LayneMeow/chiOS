#include <ata.h>
#include <io.h>
#include <string.h>

#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA_LOW     3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HIGH    5
#define ATA_REG_DRIVE_HEAD  6
#define ATA_REG_STATUS      7
#define ATA_REG_COMMAND     7

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_SRV 0x10
#define ATA_STATUS_DF  0x20
#define ATA_STATUS_RDY 0x40
#define ATA_STATUS_BSY 0x80

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_DRIVE_SELECT_LBA 0xE0

#define ATA_TIMEOUT 100000

static const uint16_t bus_io[ATA_BUS_COUNT]  = { 0x1F0, 0x170 };
static const uint16_t bus_ctrl[ATA_BUS_COUNT] = { 0x3F6, 0x376 };

static struct ata_drive drives[ATA_BUS_COUNT][ATA_DRIVE_COUNT];

static uint16_t reg(int bus, int r) {
    return (uint16_t)(bus_io[bus] + r);
}

static void select_delay(int bus) {
    for (int i = 0; i < 4; i++) {
        inb(bus_ctrl[bus]);
    }
}

static bool wait_not_busy(int bus) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if ((inb(reg(bus, ATA_REG_STATUS)) & ATA_STATUS_BSY) == 0) {
            return true;
        }
    }

    return false;
}

static bool wait_drq(int bus) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(reg(bus, ATA_REG_STATUS));

        if (status & ATA_STATUS_ERR) {
            return false;
        }
        if (status & ATA_STATUS_DRQ) {
            return true;
        }
    }

    return false;
}

static void select_drive(int bus, int drive, uint8_t lba_high4) {
    outb(reg(bus, ATA_REG_DRIVE_HEAD),
         (uint8_t)(ATA_DRIVE_SELECT_LBA | (drive << 4) | (lba_high4 & 0x0F)));
    select_delay(bus);
}

static void identify_string(char *out, const uint16_t *words, int count) {
    int len = 0;

    for (int i = 0; i < count; i++) {
        out[len++] = (char)(words[i] >> 8);
        out[len++] = (char)(words[i] & 0xFF);
    }

    while (len > 0 && out[len - 1] == ' ') {
        len--;
    }

    out[len] = '\0';
}

static bool identify_drive(int bus, int drive, struct ata_drive *out) {
    memset(out, 0, sizeof *out);

    select_drive(bus, drive, 0);

    outb(reg(bus, ATA_REG_SECCOUNT), 0);
    outb(reg(bus, ATA_REG_LBA_LOW), 0);
    outb(reg(bus, ATA_REG_LBA_MID), 0);
    outb(reg(bus, ATA_REG_LBA_HIGH), 0);
    outb(reg(bus, ATA_REG_COMMAND), ATA_CMD_IDENTIFY);

    if (inb(reg(bus, ATA_REG_STATUS)) == 0) {
        return false;
    }

    if (!wait_not_busy(bus)) {
        return false;
    }

    if (inb(reg(bus, ATA_REG_LBA_MID)) != 0 || inb(reg(bus, ATA_REG_LBA_HIGH)) != 0) {
        return false;
    }

    if (!wait_drq(bus)) {
        return false;
    }

    uint16_t id[256];

    for (int i = 0; i < 256; i++) {
        id[i] = inw(reg(bus, ATA_REG_DATA));
    }

    out->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    out->lba48 = (id[83] & (1u << 10)) != 0;
    identify_string(out->model, &id[27], 20);

    return true;
}

bool ata_init(void) {
    for (int bus = 0; bus < ATA_BUS_COUNT; bus++) {
        for (int drive = 0; drive < ATA_DRIVE_COUNT; drive++) {
            drives[bus][drive].present = identify_drive(bus, drive, &drives[bus][drive]);
        }
    }

    return true;
}

bool ata_drive_present(int bus, int drive) {
    if (bus < 0 || bus >= ATA_BUS_COUNT || drive < 0 || drive >= ATA_DRIVE_COUNT) {
        return false;
    }

    return drives[bus][drive].present;
}

const struct ata_drive *ata_drive_info(int bus, int drive) {
    if (!ata_drive_present(bus, drive)) {
        return NULL;
    }

    return &drives[bus][drive];
}

static bool transfer_chunk(int bus, int drive, uint32_t lba, size_t count,
                            void *buf, bool write) {
    select_drive(bus, drive, (uint8_t)(lba >> 24));

    if (!wait_not_busy(bus)) {
        return false;
    }

    outb(reg(bus, ATA_REG_SECCOUNT), (uint8_t)count);
    outb(reg(bus, ATA_REG_LBA_LOW), (uint8_t)lba);
    outb(reg(bus, ATA_REG_LBA_MID), (uint8_t)(lba >> 8));
    outb(reg(bus, ATA_REG_LBA_HIGH), (uint8_t)(lba >> 16));
    outb(reg(bus, ATA_REG_COMMAND), write ? ATA_CMD_WRITE_SECTORS : ATA_CMD_READ_SECTORS);

    uint16_t *words = buf;

    for (size_t s = 0; s < count; s++) {
        if (!wait_drq(bus)) {
            return false;
        }

        for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            if (write) {
                outw(reg(bus, ATA_REG_DATA), words[i]);
            } else {
                words[i] = inw(reg(bus, ATA_REG_DATA));
            }
        }

        words += ATA_SECTOR_SIZE / 2;
    }

    if (write) {

        outb(reg(bus, ATA_REG_COMMAND), ATA_CMD_CACHE_FLUSH);
        if (!wait_not_busy(bus)) {
            return false;
        }
    }

    return true;
}

static bool transfer(int bus, int drive, uint32_t lba, size_t count, void *buf, bool write) {
    if (!ata_drive_present(bus, drive)) {
        return false;
    }

    if ((uint64_t)lba + count > drives[bus][drive].sectors) {
        return false;
    }

    uint8_t *cursor = buf;

    while (count > 0) {
        size_t chunk = count > 256 ? 256 : count;

        if (!transfer_chunk(bus, drive, lba, chunk, cursor, write)) {
            return false;
        }

        lba += (uint32_t)chunk;
        count -= chunk;
        cursor += chunk * ATA_SECTOR_SIZE;
    }

    return true;
}

bool ata_read_sectors(int bus, int drive, uint32_t lba, size_t count, void *buf) {
    return transfer(bus, drive, lba, count, buf, false);
}

bool ata_write_sectors(int bus, int drive, uint32_t lba, size_t count, const void *buf) {
    return transfer(bus, drive, lba, count, (void *)(uintptr_t)buf, true);
}
