#include <io.h>
#include <memory.h>
#include <nvme.h>
#include <pci.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct nvme_regs {
    uint32_t cap_lo, cap_hi;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t rsvd0;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint32_t asq_lo, asq_hi;
    uint32_t acq_lo, acq_hi;
};

_Static_assert(offsetof(struct nvme_regs, cc) == 0x14, "CC register");
_Static_assert(offsetof(struct nvme_regs, aqa) == 0x24, "AQA register");
_Static_assert(offsetof(struct nvme_regs, acq_lo) == 0x30, "ACQ register");

#define CAP_MQES(lo)   ((lo) & 0xFFFFu)
#define CAP_TO(lo)     (((lo) >> 24) & 0xFFu)
#define CAP_DSTRD(hi)  ((hi) & 0xFu)
#define CAP_CSS(hi)    (((hi) >> 5) & 0xFFu)
#define CAP_MPSMIN(hi) (((hi) >> 16) & 0xFu)

#define CSS_NVM (1u << 0)

#define CC_EN     (1u << 0)
#define CC_CSS_NVM   (0u << 4)
#define CC_AMS_RR    (0u << 11)
#define CC_IOSQES(n) ((uint32_t)(n) << 16)
#define CC_IOCQES(n) ((uint32_t)(n) << 20)

#define CSTS_RDY (1u << 0)
#define CSTS_CFS (1u << 1)

#define DOORBELL_BASE 0x1000

struct nvme_command {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2, cdw3;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct nvme_completion {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
};

_Static_assert(sizeof(struct nvme_command) == 64, "submission queue entry");
_Static_assert(sizeof(struct nvme_completion) == 16, "completion queue entry");

#define ADMIN_CREATE_SQ 0x01
#define ADMIN_CREATE_CQ 0x05
#define ADMIN_IDENTIFY  0x06

#define IO_FLUSH 0x00
#define IO_WRITE 0x01
#define IO_READ  0x02

#define CNS_NAMESPACE       0x00
#define CNS_CONTROLLER      0x01
#define CNS_ACTIVE_NSID_LIST 0x02

#define PAGE_SIZE 4096
#define QUEUE_DEPTH 64
#define IO_QUEUE_ID 1

#define MAX_TRANSFER (128 * 1024)

#define MS_COMMAND 30000

struct nvme_queue {
    struct nvme_command *sq;
    volatile struct nvme_completion *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  phase;
    uint32_t id;
};

static volatile struct nvme_regs *regs;
static size_t db_stride;
static struct nvme_queue admin_queue;
static struct nvme_queue io_queue;
static uint64_t *prp_list;
static uint64_t prp_list_phys;
static uint32_t max_transfer = MAX_TRANSFER;
static struct nvme_drive drive;

static char status_text[80] = "no NVMe controller on the PCI bus";

static void note(const struct pci_device *dev, const char *what) {
    snprintf(status_text, sizeof status_text, "nvme %02x:%02x.%u: %s",
             dev->bus, dev->slot, dev->func, what);
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

static volatile uint32_t *doorbell(uint32_t queue, bool completion) {
    size_t index = (size_t)queue * 2 + (completion ? 1 : 0);

    return (volatile uint32_t *)((volatile uint8_t *)regs + DOORBELL_BASE + index * db_stride);
}

static bool queue_alloc(struct nvme_queue *q, uint32_t id, uint16_t depth) {
    memset(q, 0, sizeof *q);

    q->sq = dma_alloc((size_t)depth * sizeof(struct nvme_command), PAGE_SIZE, false, &q->sq_phys);
    q->cq = dma_alloc((size_t)depth * sizeof(struct nvme_completion), PAGE_SIZE, false, &q->cq_phys);

    if (q->sq == NULL || q->cq == NULL) {
        return false;
    }

    q->depth = depth;
    q->phase = 1;
    q->id = id;

    return true;
}

static bool queue_submit(struct nvme_queue *q, const struct nvme_command *cmd,
                          uint32_t *result_out) {
    struct nvme_command *slot = &q->sq[q->sq_tail];

    *slot = *cmd;
    slot->cid = q->sq_tail;

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);

    asm volatile ("" ::: "memory");

    *doorbell(q->id, false) = q->sq_tail;

    volatile struct nvme_completion *entry = &q->cq[q->cq_head];
    bool completed = false;

    for (uint32_t ms = 0; ms < MS_COMMAND && !completed; ms++) {
        for (int i = 0; i < 1000; i++) {
            if ((entry->status & 1u) == q->phase) {
                completed = true;
                break;
            }
            io_wait();
        }

        if (regs->csts & CSTS_CFS) {
            break;
        }
    }

    if (!completed) {
        return false;
    }

    uint16_t status = (uint16_t)(entry->status >> 1);

    if (result_out != NULL) {
        *result_out = entry->result;
    }

    q->cq_head = (uint16_t)(q->cq_head + 1);

    if (q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1;
    }

    *doorbell(q->id, true) = q->cq_head;

    return status == 0;
}

static void build_prps(uint64_t phys, size_t bytes, uint64_t *prp1, uint64_t *prp2) {
    uint64_t next = (phys & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;

    *prp1 = phys;

    if (phys + bytes <= next) {
        *prp2 = 0;
    } else if (phys + bytes <= next + PAGE_SIZE) {
        *prp2 = next;
    } else {
        size_t n = 0;

        while (next < phys + bytes) {
            prp_list[n++] = next;
            next += PAGE_SIZE;
        }

        *prp2 = prp_list_phys;
    }
}

static bool identify(uint8_t cns, uint32_t nsid, void *buf, uint64_t buf_phys) {
    struct nvme_command cmd;

    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = buf_phys;
    cmd.cdw10 = cns;

    memset(buf, 0, PAGE_SIZE);

    return queue_submit(&admin_queue, &cmd, NULL);
}

static void trim_string(char *out, const uint8_t *src, int count) {
    int len = 0;

    while (len < count) {
        out[len] = (char)src[len];
        len++;
    }

    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\0')) {
        len--;
    }

    out[len] = '\0';
}

static bool create_io_queues(void) {
    struct nvme_command cmd;

    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADMIN_CREATE_CQ;
    cmd.prp1 = io_queue.cq_phys;
    cmd.cdw10 = ((uint32_t)(io_queue.depth - 1) << 16) | IO_QUEUE_ID;
    cmd.cdw11 = 1u;

    if (!queue_submit(&admin_queue, &cmd, NULL)) {
        return false;
    }

    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = ADMIN_CREATE_SQ;
    cmd.prp1 = io_queue.sq_phys;
    cmd.cdw10 = ((uint32_t)(io_queue.depth - 1) << 16) | IO_QUEUE_ID;
    cmd.cdw11 = ((uint32_t)IO_QUEUE_ID << 16) | 1u;

    return queue_submit(&admin_queue, &cmd, NULL);
}

static bool read_namespace(const uint8_t *id, uint32_t nsid) {
    uint64_t sectors = 0;

    for (int i = 0; i < 8; i++) {
        sectors |= (uint64_t)id[i] << (i * 8);
    }

    if (sectors == 0) {
        return false;
    }

    uint8_t format = id[26] & 0x0F;
    const uint8_t *lbaf = &id[128 + format * 4];
    uint8_t lbads = lbaf[2];

    if (lbads < 9 || lbads > 12) {
        return false;
    }

    drive.sectors = sectors;
    drive.sector_size = 1u << lbads;
    drive.nsid = nsid;

    return true;
}

static bool controller_init(const struct pci_device *dev) {
    pci_enable_bus_mastering(dev);
    pci_disable_intx(dev);

    uint64_t bar = pci_bar_address(dev, 0);

    if (bar == 0) {
        note(dev, "no register window assigned");
        return false;
    }

    regs = mmio_map(bar, 0x2000);

    if (regs == NULL) {
        note(dev, "registers could not be mapped");
        return false;
    }

    uint32_t cap_lo = regs->cap_lo;
    uint32_t cap_hi = regs->cap_hi;

    if ((cap_lo == 0xFFFFFFFFu && cap_hi == 0xFFFFFFFFu) || (regs->vs >> 16) == 0
     || (regs->vs >> 16) > 2) {
        note(dev, "not answering as an NVMe controller");
        return false;
    }

    if ((CAP_CSS(cap_hi) & CSS_NVM) == 0) {
        note(dev, "does not speak the NVM command set");
        return false;
    }

    if (CAP_MPSMIN(cap_hi) != 0) {
        note(dev, "smallest page it supports is larger than 4KiB");
        return false;
    }

    db_stride = (size_t)4 << CAP_DSTRD(cap_hi);

    if (mmio_map(bar, DOORBELL_BASE + 4 * db_stride) == NULL) {
        note(dev, "doorbells could not be mapped");
        return false;
    }

    uint32_t reset_ms = CAP_TO(cap_lo) * 500;

    if (reset_ms < 500) {
        reset_ms = 500;
    }

    regs->cc &= ~CC_EN;

    if (!wait_bits(&regs->csts, CSTS_RDY, 0, reset_ms)) {
        note(dev, "would not go idle");
        return false;
    }

    regs->intms = 0xFFFFFFFFu;

    uint16_t depth = QUEUE_DEPTH;

    if (CAP_MQES(cap_lo) + 1 < depth) {
        depth = (uint16_t)(CAP_MQES(cap_lo) + 1);
    }

    if (depth < 2 || !queue_alloc(&admin_queue, 0, depth)
     || !queue_alloc(&io_queue, IO_QUEUE_ID, depth)) {
        note(dev, "out of memory for its queues");
        return false;
    }

    prp_list = dma_alloc(PAGE_SIZE, PAGE_SIZE, false, &prp_list_phys);

    uint64_t id_phys = 0;
    uint8_t *id = dma_alloc(PAGE_SIZE, PAGE_SIZE, false, &id_phys);

    if (prp_list == NULL || id == NULL) {
        note(dev, "out of memory");
        return false;
    }

    regs->aqa = ((uint32_t)(depth - 1) << 16) | (uint32_t)(depth - 1);
    regs->asq_lo = (uint32_t)admin_queue.sq_phys;
    regs->asq_hi = (uint32_t)(admin_queue.sq_phys >> 32);
    regs->acq_lo = (uint32_t)admin_queue.cq_phys;
    regs->acq_hi = (uint32_t)(admin_queue.cq_phys >> 32);

    regs->cc = CC_CSS_NVM | CC_AMS_RR | CC_IOSQES(6) | CC_IOCQES(4) | CC_EN;

    if (!wait_bits(&regs->csts, CSTS_RDY, CSTS_RDY, reset_ms)) {
        note(dev, "would not come ready");
        return false;
    }

    if (!identify(CNS_CONTROLLER, 0, id, id_phys)) {
        note(dev, "would not identify itself");
        return false;
    }

    trim_string(drive.model, &id[24], 40);

    uint8_t mdts = id[77];

    if (mdts != 0 && mdts < 20) {
        uint32_t limit = (1u << mdts) * PAGE_SIZE;

        if (limit < max_transfer) {
            max_transfer = limit;
        }
    }

    if (!create_io_queues()) {
        note(dev, "would not create an I/O queue");
        return false;
    }

    uint32_t nsid = 1;

    if (identify(CNS_ACTIVE_NSID_LIST, 0, id, id_phys)) {
        uint32_t first = (uint32_t)id[0] | ((uint32_t)id[1] << 8)
                       | ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24);

        if (first != 0) {
            nsid = first;
        }
    }

    if (!identify(CNS_NAMESPACE, nsid, id, id_phys) || !read_namespace(id, nsid)) {
        note(dev, "has no usable namespace");
        return false;
    }

    drive.present = true;

    return true;
}

bool nvme_init(void) {
    struct pci_device dev, previous;
    struct pci_device *cursor = NULL;
    bool any_controller = false;

    while (pci_find_class_after(0x01, 0x08, PCI_PROG_IF_ANY, cursor, &dev)) {
        previous = dev;
        cursor = &previous;
        any_controller = true;

        if (controller_init(&dev)) {
            return true;
        }

        memset(&drive, 0, sizeof drive);
    }

    if (!any_controller) {
        strcpy(status_text, "no NVMe controller on the PCI bus");
    }

    regs = NULL;

    return false;
}

const char *nvme_status(void) {
    return status_text;
}

bool nvme_drive_present(void) {
    return drive.present;
}

const struct nvme_drive *nvme_drive_info(void) {
    return drive.present ? &drive : NULL;
}

static bool transfer(uint64_t lba, size_t count, void *buf, bool write) {
    if (!drive.present || count == 0) {
        return false;
    }

    if (lba + count > drive.sectors || lba + count < lba) {
        return false;
    }

    if (!dma_reachable(buf, count * drive.sector_size, false)
     || ((uintptr_t)buf & 3) != 0) {
        return false;
    }

    uint8_t *cursor = buf;
    size_t per_command = max_transfer / drive.sector_size;

    while (count > 0) {
        size_t chunk = count < per_command ? count : per_command;
        size_t bytes = chunk * drive.sector_size;
        uint64_t phys = (uint64_t)(uintptr_t)cursor - hhdm_offset();

        struct nvme_command cmd;

        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = write ? IO_WRITE : IO_READ;
        cmd.nsid = drive.nsid;
        build_prps(phys, bytes, &cmd.prp1, &cmd.prp2);
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = (uint32_t)(chunk - 1);

        if (!queue_submit(&io_queue, &cmd, NULL)) {
            return false;
        }

        lba += chunk;
        count -= chunk;
        cursor += bytes;
    }

    if (write) {
        struct nvme_command cmd;

        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = IO_FLUSH;
        cmd.nsid = drive.nsid;

        if (!queue_submit(&io_queue, &cmd, NULL)) {
            return false;
        }
    }

    return true;
}

bool nvme_read_sectors(uint64_t lba, size_t count, void *buf) {
    return transfer(lba, count, buf, false);
}

bool nvme_write_sectors(uint64_t lba, size_t count, const void *buf) {
    return transfer(lba, count, (void *)(uintptr_t)buf, true);
}
