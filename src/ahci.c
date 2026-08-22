#include <ahci.h>
#include <io.h>
#include <memory.h>
#include <pci.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct hba_port {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
};

struct hba_mem {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    struct hba_port ports[AHCI_MAX_PORTS];
};

_Static_assert(sizeof(struct hba_port) == 0x80, "port register block");
_Static_assert(offsetof(struct hba_mem, bohc) == 0x28, "BIOS/OS handoff register");
_Static_assert(offsetof(struct hba_mem, ports) == 0x100, "port register blocks");

#define CAP_NP_MASK 0x1Fu
#define CAP_S64A    (1u << 31)

#define CAP2_BOH (1u << 0)

#define GHC_AE (1u << 31)
#define GHC_IE (1u << 1)

#define BOHC_BOS (1u << 0)
#define BOHC_OOS (1u << 1)
#define BOHC_BB  (1u << 4)

#define PXCMD_ST  (1u << 0)
#define PXCMD_SUD (1u << 1)
#define PXCMD_POD (1u << 2)
#define PXCMD_FRE (1u << 4)
#define PXCMD_FR  (1u << 14)
#define PXCMD_CR  (1u << 15)
#define PXCMD_ICC_MASK   (0xFu << 28)
#define PXCMD_ICC_ACTIVE (0x1u << 28)

#define PXIS_TFES (1u << 30)

#define PXTFD_ERR 0x01
#define PXTFD_DRQ 0x08
#define PXTFD_BSY 0x80

#define PXSSTS_DET_MASK    0x0Fu
#define PXSSTS_DET_PRESENT 0x03u

#define PXSCTL_DET_MASK     0x0Fu
#define PXSCTL_DET_COMRESET 0x01u

#define SATA_SIG_ATA    0x00000101u
#define SATA_SIG_NONE   0xFFFFFFFFu

struct hba_cmd_header {
    uint8_t  cfl   : 5;
    uint8_t  a     : 1;
    uint8_t  w     : 1;
    uint8_t  p     : 1;
    uint8_t  r     : 1;
    uint8_t  b     : 1;
    uint8_t  c     : 1;
    uint8_t  rsv0  : 1;
    uint8_t  pmp   : 4;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed));

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc  : 22;
    uint32_t rsv1 : 9;
    uint32_t i    : 1;
} __attribute__((packed));

struct hba_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct hba_prdt_entry prdt[1];
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0   : 3;
    uint8_t c      : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed));

#define FIS_TYPE_REG_H2D 0x27
#define DEVICE_LBA_MODE  0x40

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_IDENTIFY 0xEC

#define MS_IDLE     500
#define MS_LINK    1000
#define MS_EMPTY    100
#define MS_READY   5000
#define MS_COMMAND 5000

#define AHCI_MAX_SECTORS_PER_CMD 4096

struct ahci_port_ctx {
    struct hba_cmd_header *cl;
    struct hba_cmd_table *ct;
    uint64_t ct_phys;
};

static volatile struct hba_mem *hba;
static struct ahci_port_ctx port_ctx[AHCI_MAX_PORTS];
static struct ahci_drive drives[AHCI_MAX_PORTS];

static bool needs_low_dma;

static uint16_t *identify_buf;

static char status_text[80] = "no AHCI controller on the PCI bus";

static void note(const struct pci_device *dev, const char *what) {
    snprintf(status_text, sizeof status_text, "ahci %02x:%02x.%u: %s",
             dev->bus, dev->slot, dev->func, what);
}

static void delay_ms(uint32_t ms) {
    while (ms-- > 0) {
        for (int i = 0; i < 1000; i++) {
            io_wait();
        }
    }
}

static bool wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (int j = 0; j < 1000; j++) {
            if ((*reg & mask) == want) {
                return true;
            }
            io_wait();
        }
    }

    return (*reg & mask) == want;
}

static uint8_t port_det(volatile struct hba_port *port) {
    return (uint8_t)(port->ssts & PXSSTS_DET_MASK);
}

static void port_clear_errors(volatile struct hba_port *port) {
    port->serr = port->serr;
    port->is = port->is;
}

static bool port_stop(volatile struct hba_port *port) {
    port->cmd &= ~(uint32_t)PXCMD_ST;

    if (!wait_bits(&port->cmd, PXCMD_CR, 0, MS_IDLE)) {
        return false;
    }

    port->cmd &= ~(uint32_t)PXCMD_FRE;

    return wait_bits(&port->cmd, PXCMD_FR, 0, MS_IDLE);
}

static void port_spin_up(volatile struct hba_port *port) {
    uint32_t cmd = port->cmd;

    port->cmd = (cmd & ~PXCMD_ICC_MASK) | PXCMD_SUD | PXCMD_POD | PXCMD_ICC_ACTIVE;
}

static bool wait_link(volatile struct hba_port *port, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        uint8_t det = port_det(port);

        if (det == PXSSTS_DET_PRESENT) {
            return true;
        }
        if (det == 0 && i >= MS_EMPTY) {
            return false;
        }

        delay_ms(1);
    }

    return port_det(port) == PXSSTS_DET_PRESENT;
}

static void port_comreset(volatile struct hba_port *port) {
    port->sctl = (port->sctl & ~PXSCTL_DET_MASK) | PXSCTL_DET_COMRESET;
    delay_ms(2);
    port->sctl &= ~PXSCTL_DET_MASK;
}

static bool port_settle(volatile struct hba_port *port) {
    return wait_bits(&port->tfd, PXTFD_BSY | PXTFD_DRQ, 0, MS_READY);
}

static bool port_alloc(int idx, volatile struct hba_port *port) {
    uint64_t cl_phys = 0, fis_phys = 0, ct_phys = 0;

    void *cl = dma_alloc(32 * sizeof(struct hba_cmd_header), 1024, needs_low_dma, &cl_phys);
    void *fis = dma_alloc(256, 256, needs_low_dma, &fis_phys);
    void *ct = dma_alloc(sizeof(struct hba_cmd_table), 128, needs_low_dma, &ct_phys);

    if (cl == NULL || fis == NULL || ct == NULL) {
        return false;
    }

    port_ctx[idx].cl = cl;
    port_ctx[idx].ct = ct;
    port_ctx[idx].ct_phys = ct_phys;

    port->clb  = (uint32_t)cl_phys;
    port->clbu = (uint32_t)(cl_phys >> 32);
    port->fb   = (uint32_t)fis_phys;
    port->fbu  = (uint32_t)(fis_phys >> 32);

    return true;
}

static bool port_bring_up(int idx, volatile struct hba_port *port) {
    port->ie = 0;

    if (!port_stop(port)) {
        return false;
    }

    port_spin_up(port);

    if (!wait_link(port, MS_LINK)) {
        port_comreset(port);

        if (!wait_link(port, MS_LINK)) {
            return false;
        }
    }

    if (!port_alloc(idx, port)) {
        return false;
    }

    struct hba_cmd_header *hdr = &port_ctx[idx].cl[0];

    memset(hdr, 0, sizeof *hdr);
    hdr->ctba = (uint32_t)port_ctx[idx].ct_phys;
    hdr->ctbau = (uint32_t)(port_ctx[idx].ct_phys >> 32);

    port_clear_errors(port);

    port->cmd |= PXCMD_FRE;

    if (!wait_bits(&port->cmd, PXCMD_FR, PXCMD_FR, MS_IDLE)) {
        return false;
    }

    if (port->sig == SATA_SIG_NONE || !port_settle(port)) {

        port_comreset(port);

        if (!wait_link(port, MS_LINK)) {
            return false;
        }

        port_clear_errors(port);

        if (!port_settle(port)) {
            return false;
        }
    }

    if (port->sig != SATA_SIG_ATA) {
        return false;
    }

    port->cmd |= PXCMD_ST;

    return true;
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

static void port_recover(volatile struct hba_port *port) {
    port->cmd &= ~(uint32_t)PXCMD_ST;
    wait_bits(&port->cmd, PXCMD_CR, 0, MS_IDLE);

    port_clear_errors(port);

    if (port_det(port) == PXSSTS_DET_PRESENT) {
        port->cmd |= PXCMD_ST;
    }
}

static bool port_command(int idx, volatile struct hba_port *port, uint8_t command,
                          uint64_t lba, uint16_t count, void *buf, bool write) {
    if (!wait_bits(&port->tfd, PXTFD_BSY | PXTFD_DRQ, 0, MS_COMMAND)) {
        return false;
    }

    port_clear_errors(port);

    struct hba_cmd_header *hdr = &port_ctx[idx].cl[0];
    struct hba_cmd_table *ct = port_ctx[idx].ct;

    memset(hdr, 0, sizeof *hdr);
    hdr->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    hdr->ctba = (uint32_t)port_ctx[idx].ct_phys;
    hdr->ctbau = (uint32_t)(port_ctx[idx].ct_phys >> 32);

    memset(ct, 0, sizeof *ct);

    if (buf != NULL) {
        uint64_t buf_phys = (uint64_t)(uintptr_t)buf - hhdm_offset();

        hdr->prdtl = 1;
        ct->prdt[0].dba = (uint32_t)buf_phys;
        ct->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
        ct->prdt[0].dbc = (uint32_t)count * AHCI_SECTOR_SIZE - 1;
        ct->prdt[0].i = 1;
    }

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)ct->cfis;

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;
    fis->device = DEVICE_LBA_MODE;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    port->ci = 1u;

    bool completed = false;

    for (uint32_t ms = 0; ms < MS_COMMAND && !completed; ms++) {
        for (int i = 0; i < 1000; i++) {
            if ((port->ci & 1u) == 0) {
                completed = true;
                break;
            }
            if (port->is & PXIS_TFES) {
                break;
            }
            io_wait();
        }

        if (port->is & PXIS_TFES) {
            break;
        }
    }

    if (!completed || (port->tfd & PXTFD_ERR) != 0) {
        port_recover(port);
        return false;
    }

    return true;
}

static bool identify(int idx, volatile struct hba_port *port) {
    uint16_t *id = identify_buf;

    if (id == NULL || !port_command(idx, port, ATA_CMD_IDENTIFY, 0, 1, id, false)) {
        return false;
    }

    drives[idx].sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                         | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);

    if (drives[idx].sectors == 0) {
        drives[idx].sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }

    identify_string(drives[idx].model, &id[27], 20);

    return drives[idx].sectors != 0;
}

static bool version_plausible(uint32_t vs) {
    return vs != 0 && vs != 0xFFFFFFFFu && (vs >> 16) <= 1;
}

static void take_ownership(volatile struct hba_mem *h) {
    if ((h->cap2 & CAP2_BOH) == 0) {
        return;
    }

    h->bohc |= BOHC_OOS;

    for (int i = 0; i < 25 && (h->bohc & BOHC_BOS); i++) {
        delay_ms(1);
    }

    for (int i = 0; i < 2000 && (h->bohc & BOHC_BB); i++) {
        delay_ms(1);
    }
}

#define PCI_VENDOR_INTEL 0x8086
#define INTEL_PCS        0x92

static void intel_pcs_quirk(const struct pci_device *dev, uint32_t pi) {
    if (dev->vendor_id != PCI_VENDOR_INTEL) {
        return;
    }

    uint16_t pcs = pci_config_read16(dev->bus, dev->slot, dev->func, INTEL_PCS);
    uint16_t want = (uint16_t)(pcs | (pi & 0xFF));

    if (want != pcs) {
        pci_config_write16(dev->bus, dev->slot, dev->func, INTEL_PCS, want);
    }
}

static bool controller_init(const struct pci_device *dev) {
    pci_enable_bus_mastering(dev);

    uint32_t bar5 = pci_bar(dev, 5);

    if ((bar5 & 1u) != 0 || (bar5 & ~0xFu) == 0) {
        note(dev, "no register window assigned");
        return false;
    }

    volatile struct hba_mem *h = mmio_map(bar5 & ~0xFu, 0x1100);

    if (h == NULL) {
        note(dev, "registers could not be mapped");
        return false;
    }

    h->ghc |= GHC_AE;

    take_ownership(h);

    h->ghc |= GHC_AE;

    if (!version_plausible(h->vs)) {
        note(dev, "not answering as an AHCI controller");
        return false;
    }

    h->ghc &= ~GHC_IE;
    h->is = h->is;

    hba = h;
    needs_low_dma = (h->cap & CAP_S64A) == 0;

    uint32_t pi = h->pi;

    if (pi == 0) {

        pi = ((uint32_t)2 << (h->cap & CAP_NP_MASK)) - 1;
    }

    intel_pcs_quirk(dev, pi);

    uint64_t identify_phys = 0;

    identify_buf = dma_alloc(AHCI_SECTOR_SIZE, AHCI_SECTOR_SIZE, needs_low_dma, &identify_phys);

    if (identify_buf == NULL) {
        note(dev, needs_low_dma ? "no memory it can reach below 4GiB"
                                : "out of memory");
        return false;
    }

    int implemented = 0, linked = 0, found = 0;

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((pi & (1u << i)) == 0) {
            continue;
        }

        implemented++;

        volatile struct hba_port *port = &h->ports[i];

        if (!port_bring_up(i, port)) {
            linked += port_det(port) == PXSSTS_DET_PRESENT;
            continue;
        }

        linked++;

        if (identify(i, port)) {
            drives[i].present = true;
            found++;
        }
    }

    if (found == 0) {
        char detail[48];

        snprintf(detail, sizeof detail, "%d port%s, %d linked, no disk",
                 implemented, implemented == 1 ? "" : "s", linked);
        note(dev, detail);

        hba = NULL;
    }

    return found > 0;
}

bool ahci_init(void) {
    struct pci_device dev, previous;
    struct pci_device *cursor = NULL;
    bool any_controller = false;

    while (pci_find_class_after(0x01, 0x06, PCI_PROG_IF_ANY, cursor, &dev)) {
        previous = dev;
        cursor = &previous;
        any_controller = true;

        if (controller_init(&dev)) {
            return true;
        }
    }

    if (!any_controller) {
        strcpy(status_text, "no AHCI controller on the PCI bus");
    }

    return false;
}

const char *ahci_status(void) {
    return status_text;
}

bool ahci_drive_present(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS) {
        return false;
    }

    return drives[port].present;
}

const struct ahci_drive *ahci_drive_info(int port) {
    if (!ahci_drive_present(port)) {
        return NULL;
    }

    return &drives[port];
}

static bool transfer(int idx, uint64_t lba, size_t count, void *buf, bool write) {
    if (!ahci_drive_present(idx) || count == 0) {
        return false;
    }

    if (lba + count > drives[idx].sectors || lba + count < lba) {
        return false;
    }

    if (!dma_reachable(buf, count * AHCI_SECTOR_SIZE, needs_low_dma)) {
        return false;
    }

    volatile struct hba_port *port = &hba->ports[idx];
    uint8_t *cursor = buf;
    uint8_t cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;

    while (count > 0) {
        size_t chunk = count > AHCI_MAX_SECTORS_PER_CMD ? AHCI_MAX_SECTORS_PER_CMD : count;

        if (!port_command(idx, port, cmd, lba, (uint16_t)chunk, cursor, write)) {
            return false;
        }

        lba += chunk;
        count -= chunk;
        cursor += chunk * AHCI_SECTOR_SIZE;
    }

    if (write) {

        if (!port_command(idx, port, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, NULL, false)) {
            return false;
        }
    }

    return true;
}

bool ahci_read_sectors(int port, uint64_t lba, size_t count, void *buf) {
    return transfer(port, lba, count, buf, false);
}

bool ahci_write_sectors(int port, uint64_t lba, size_t count, const void *buf) {
    return transfer(port, lba, count, (void *)(uintptr_t)buf, true);
}
