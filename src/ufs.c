#include <io.h>
#include <memory.h>
#include <pci.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ufs.h>

struct ufs_regs {
    uint32_t cap;
    uint32_t rsvd0;
    uint32_t ver;
    uint32_t rsvd1;
    uint32_t hcpid;
    uint32_t hcmid;
    uint32_t ahit;
    uint32_t rsvd2;
    uint32_t is;
    uint32_t ie;
    uint32_t rsvd3[2];
    uint32_t hcs;
    uint32_t hce;
    uint32_t uecpa;
    uint32_t uecdl;
    uint32_t uecn;
    uint32_t uect;
    uint32_t uecdme;
    uint32_t utriacr;
    uint32_t utrlba;
    uint32_t utrlbau;
    uint32_t utrldbr;
    uint32_t utrlclr;
    uint32_t utrlrsr;
    uint32_t utrlcnr;
    uint32_t rsvd4[2];
    uint32_t utmrlba;
    uint32_t utmrlbau;
    uint32_t utmrldbr;
    uint32_t utmrlclr;
    uint32_t utmrlrsr;
    uint32_t rsvd5[3];
    uint32_t uiccmd;
    uint32_t uiccmdarg1;
    uint32_t uiccmdarg2;
    uint32_t uiccmdarg3;
};

_Static_assert(offsetof(struct ufs_regs, hcs) == 0x30, "HCS register");
_Static_assert(offsetof(struct ufs_regs, utrlba) == 0x50, "UTRLBA register");
_Static_assert(offsetof(struct ufs_regs, utmrlba) == 0x70, "UTMRLBA register");
_Static_assert(offsetof(struct ufs_regs, uiccmd) == 0x90, "UICCMD register");

#define CAP_NUTRS(c)  (((c) & 0x1Fu) + 1)
#define CAP_NUTMRS(c) ((((c) >> 16) & 0x7u) + 1)
#define CAP_64AS      (1u << 24)

#define HCE_ENABLE (1u << 0)

#define HCS_DP       (1u << 0)
#define HCS_UTRLRDY  (1u << 1)
#define HCS_UTMRLRDY (1u << 2)
#define HCS_UCRDY    (1u << 3)

#define IS_UTRCS (1u << 0)
#define IS_UE    (1u << 2)
#define IS_ULSS  (1u << 8)
#define IS_UCCS  (1u << 10)
#define IS_DFES  (1u << 11)
#define IS_HCFES (1u << 16)
#define IS_SBFES (1u << 17)

#define UIC_DME_LINKSTARTUP 0x16

struct utp_transfer_req_desc {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;
    uint32_t ucdba;
    uint32_t ucdbau;
    uint16_t resp_len;
    uint16_t resp_off;
    uint16_t prdt_len;
    uint16_t prdt_off;
};

_Static_assert(sizeof(struct utp_transfer_req_desc) == 32, "transfer request descriptor");

#define UTRD_CT_UFS       (1u << 28)
#define UTRD_DD_NONE      0u
#define UTRD_DD_TO_DEVICE (1u << 25)
#define UTRD_DD_TO_HOST   (1u << 26)

#define OCS_SUCCESS 0x0
#define OCS_INVALID 0xF

struct ufs_prdt_entry {
    uint32_t base;
    uint32_t base_upper;
    uint32_t rsvd;
    uint32_t size;
};

#define UPIU_ROOM 128

struct ufs_cmd_desc {
    uint8_t request[UPIU_ROOM];
    uint8_t response[UPIU_ROOM];
    struct ufs_prdt_entry prdt[1];
    uint8_t pad[128 - sizeof(struct ufs_prdt_entry)];
};

_Static_assert(sizeof(struct ufs_cmd_desc) % 128 == 0, "command descriptor alignment");

#define UPIU_NOP_OUT   0x00
#define UPIU_COMMAND   0x01
#define UPIU_QUERY_REQ 0x16
#define UPIU_NOP_IN    0x20
#define UPIU_RESPONSE  0x21
#define UPIU_QUERY_RSP 0x36

#define UPIU_FLAG_READ  0x40
#define UPIU_FLAG_WRITE 0x20

#define QUERY_READ_REQUEST  0x01
#define QUERY_WRITE_REQUEST 0x81

#define QUERY_OP_READ_FLAG 0x05
#define QUERY_OP_SET_FLAG  0x06

#define FLAG_IDN_DEVICE_INIT 0x01

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A
#define SCSI_SYNCHRONIZE_CACHE 0x35

#define SCSI_STATUS_GOOD 0x00

#define MS_REG     1000
#define MS_LINK    1000
#define MS_COMMAND 10000

#define MAX_TRANSFER (128 * 1024)

static volatile struct ufs_regs *regs;
static bool needs_low_dma;
static struct utp_transfer_req_desc *utrd;
static struct ufs_cmd_desc *ucd;
static uint64_t ucd_phys;
static uint8_t *scratch;
static uint64_t scratch_phys;
static struct ufs_lun luns[UFS_MAX_LUNS];

static char status_text[80] = "no UFS controller on the PCI bus";

static void note(const struct pci_device *dev, const char *what) {
    snprintf(status_text, sizeof status_text, "ufs %02x:%02x.%u: %s",
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

static bool hba_enable(void) {
    regs->hce = 0;

    if (!wait_bits(&regs->hce, HCE_ENABLE, 0, MS_REG)) {
        return false;
    }

    regs->hce = HCE_ENABLE;
    delay_ms(2);

    if (!wait_bits(&regs->hce, HCE_ENABLE, HCE_ENABLE, MS_REG)) {
        return false;
    }

    return wait_bits(&regs->hcs, HCS_UCRDY, HCS_UCRDY, MS_REG);
}

static bool uic_command(uint8_t opcode, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (!wait_bits(&regs->hcs, HCS_UCRDY, HCS_UCRDY, MS_REG)) {
        return false;
    }

    regs->is = IS_UCCS;

    regs->uiccmdarg1 = arg1;
    regs->uiccmdarg2 = arg2;
    regs->uiccmdarg3 = arg3;
    regs->uiccmd = opcode;

    if (!wait_bits(&regs->is, IS_UCCS, IS_UCCS, MS_LINK)) {
        return false;
    }

    uint32_t result = regs->uiccmdarg2 & 0xFF;

    regs->is = IS_UCCS;

    return result == 0;
}

static bool submit(uint32_t direction, uint64_t data_phys, size_t data_len) {
    memset(utrd, 0, sizeof *utrd);

    utrd->dword0 = UTRD_CT_UFS | direction;
    utrd->dword2 = OCS_INVALID;
    utrd->ucdba = (uint32_t)ucd_phys;
    utrd->ucdbau = (uint32_t)(ucd_phys >> 32);
    utrd->resp_off = (uint16_t)(offsetof(struct ufs_cmd_desc, response) / 4);
    utrd->resp_len = (uint16_t)(UPIU_ROOM / 4);

    if (data_len > 0) {
        utrd->prdt_off = (uint16_t)(offsetof(struct ufs_cmd_desc, prdt) / 4);
        utrd->prdt_len = 1;

        ucd->prdt[0].base = (uint32_t)data_phys;
        ucd->prdt[0].base_upper = (uint32_t)(data_phys >> 32);
        ucd->prdt[0].rsvd = 0;
        ucd->prdt[0].size = (uint32_t)(data_len - 1);
    }

    memset(ucd->response, 0, sizeof ucd->response);

    asm volatile ("" ::: "memory");

    regs->utrldbr = 1u;

    if (!wait_bits(&regs->utrldbr, 1u, 0, MS_COMMAND)) {
        return false;
    }

    uint32_t status = regs->is;

    regs->is = status;

    if (status & (IS_DFES | IS_HCFES | IS_SBFES)) {
        return false;
    }

    return (utrd->dword2 & 0xFFu) == OCS_SUCCESS;
}

static bool nop_out(void) {
    memset(ucd->request, 0, sizeof ucd->request);
    ucd->request[0] = UPIU_NOP_OUT;
    ucd->request[3] = 0;

    if (!submit(UTRD_DD_NONE, 0, 0)) {
        return false;
    }

    return ucd->response[0] == UPIU_NOP_IN;
}

static bool query_flag(uint8_t opcode, uint8_t idn, bool *value_out) {
    bool reading = opcode == QUERY_OP_READ_FLAG;

    memset(ucd->request, 0, sizeof ucd->request);
    ucd->request[0] = UPIU_QUERY_REQ;
    ucd->request[5] = reading ? QUERY_READ_REQUEST : QUERY_WRITE_REQUEST;
    ucd->request[12] = opcode;
    ucd->request[13] = idn;

    if (!submit(UTRD_DD_NONE, 0, 0)) {
        return false;
    }

    if (ucd->response[0] != UPIU_QUERY_RSP || ucd->response[6] != 0) {
        return false;
    }

    if (value_out != NULL) {
        *value_out = (get_be32(&ucd->response[20]) & 1u) != 0;
    }

    return true;
}

static bool scsi_command(uint8_t lun, const uint8_t *cdb, size_t cdb_len,
                          void *data, size_t data_len, bool write) {
    uint32_t direction = UTRD_DD_NONE;
    uint8_t flags = 0;
    uint64_t data_phys = 0;

    if (data_len > 0) {
        direction = write ? UTRD_DD_TO_DEVICE : UTRD_DD_TO_HOST;
        flags = write ? UPIU_FLAG_WRITE : UPIU_FLAG_READ;
        data_phys = (uint64_t)(uintptr_t)data - hhdm_offset();
    }

    memset(ucd->request, 0, sizeof ucd->request);
    ucd->request[0] = UPIU_COMMAND;
    ucd->request[1] = flags;
    ucd->request[2] = lun;
    ucd->request[3] = 0;
    ucd->request[4] = 0;
    put_be16(&ucd->request[10], 0);
    put_be32(&ucd->request[12], (uint32_t)data_len);
    memcpy(&ucd->request[16], cdb, cdb_len);

    if (!submit(direction, data_phys, data_len)) {
        return false;
    }

    return ucd->response[0] == UPIU_RESPONSE && ucd->response[7] == SCSI_STATUS_GOOD;
}

static void trim_string(char *out, const uint8_t *src, int count) {
    int len = 0;

    while (len < count) {
        out[len] = src[len] >= 0x20 && src[len] < 0x7F ? (char)src[len] : ' ';
        len++;
    }

    while (len > 0 && out[len - 1] == ' ') {
        len--;
    }

    out[len] = '\0';
}

static bool probe_lun(int lun) {
    uint8_t cdb[16];

    memset(cdb, 0, sizeof cdb);
    cdb[0] = SCSI_TEST_UNIT_READY;

    for (int i = 0; i < 3; i++) {
        if (scsi_command((uint8_t)lun, cdb, 6, NULL, 0, false)) {
            break;
        }
        delay_ms(10);
    }

    memset(cdb, 0, sizeof cdb);
    cdb[0] = SCSI_READ_CAPACITY10;

    memset(scratch, 0, 8);

    if (!scsi_command((uint8_t)lun, cdb, 10, scratch, 8, false)) {
        return false;
    }

    uint32_t last_lba = get_be32(&scratch[0]);
    uint32_t block_size = get_be32(&scratch[4]);

    if (block_size < 512 || block_size > 4096 || (block_size & (block_size - 1)) != 0) {
        return false;
    }

    luns[lun].sectors = (uint64_t)last_lba + 1;
    luns[lun].sector_size = block_size;

    memset(cdb, 0, sizeof cdb);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;

    memset(scratch, 0, 36);

    if (scsi_command((uint8_t)lun, cdb, 6, scratch, 36, false)) {

        trim_string(luns[lun].model, &scratch[8], 24);
    } else {
        strcpy(luns[lun].model, "UFS logical unit");
    }

    luns[lun].present = true;

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

    regs = mmio_map(bar, 0x1000);

    if (regs == NULL) {
        note(dev, "registers could not be mapped");
        return false;
    }

    if (regs->ver == 0 || regs->ver == 0xFFFFFFFFu) {
        note(dev, "not answering as a UFS controller");
        return false;
    }

    regs->ie = 0;

    if (!hba_enable()) {
        note(dev, "would not come out of reset");
        return false;
    }

    bool linked = false;

    for (int attempt = 0; attempt < 3 && !linked; attempt++) {
        if (attempt > 0 && !hba_enable()) {
            break;
        }

        regs->is = IS_ULSS | IS_UE;

        linked = uic_command(UIC_DME_LINKSTARTUP, 0, 0, 0) && (regs->hcs & HCS_DP) != 0;
    }

    if (!linked) {
        note(dev, "no device on the link");
        return false;
    }

    regs->is = regs->is;

    uint32_t cap = regs->cap;

    needs_low_dma = (cap & CAP_64AS) == 0;
    uint64_t utrd_phys = 0, utmrd_phys = 0;

    utrd = dma_alloc(CAP_NUTRS(cap) * sizeof(struct utp_transfer_req_desc), 1024,
                     needs_low_dma, &utrd_phys);

    void *utmrd = dma_alloc(CAP_NUTMRS(cap) * 80, 1024, needs_low_dma, &utmrd_phys);

    ucd = dma_alloc(sizeof(struct ufs_cmd_desc), 128, needs_low_dma, &ucd_phys);
    scratch = dma_alloc(512, 512, needs_low_dma, &scratch_phys);

    if (utrd == NULL || utmrd == NULL || ucd == NULL || scratch == NULL) {
        note(dev, "out of memory for its queues");
        return false;
    }

    regs->utrlba = (uint32_t)utrd_phys;
    regs->utrlbau = (uint32_t)(utrd_phys >> 32);
    regs->utmrlba = (uint32_t)utmrd_phys;
    regs->utmrlbau = (uint32_t)(utmrd_phys >> 32);

    regs->utrlrsr = 1;
    regs->utmrlrsr = 1;

    if (!wait_bits(&regs->hcs, HCS_UTRLRDY, HCS_UTRLRDY, MS_REG)) {
        note(dev, "its request list never came ready");
        return false;
    }

    bool awake = false;

    for (int i = 0; i < 20 && !awake; i++) {
        awake = nop_out();

        if (!awake) {
            delay_ms(10);
        }
    }

    if (!awake) {
        note(dev, "device did not answer a NOP");
        return false;
    }

    if (!query_flag(QUERY_OP_SET_FLAG, FLAG_IDN_DEVICE_INIT, NULL)) {
        note(dev, "would not accept fDeviceInit");
        return false;
    }

    for (int i = 0; i < 100; i++) {
        bool still_initialising = true;

        if (query_flag(QUERY_OP_READ_FLAG, FLAG_IDN_DEVICE_INIT, &still_initialising)
         && !still_initialising) {
            break;
        }

        delay_ms(10);
    }

    int found = 0;

    for (int lun = 0; lun < UFS_MAX_LUNS; lun++) {
        if (probe_lun(lun)) {
            found++;
        }
    }

    if (found == 0) {
        note(dev, "no usable logical unit");
        return false;
    }

    return true;
}

bool ufs_init(void) {
    struct pci_device dev, previous;
    struct pci_device *cursor = NULL;
    bool any_controller = false;

    while (pci_find_class_after(0x01, 0x09, PCI_PROG_IF_ANY, cursor, &dev)) {
        previous = dev;
        cursor = &previous;
        any_controller = true;

        if (controller_init(&dev)) {
            return true;
        }

        memset(luns, 0, sizeof luns);
    }

    if (!any_controller) {
        strcpy(status_text, "no UFS controller on the PCI bus");
    }

    regs = NULL;

    return false;
}

const char *ufs_status(void) {
    return status_text;
}

bool ufs_lun_present(int lun) {
    if (lun < 0 || lun >= UFS_MAX_LUNS) {
        return false;
    }

    return luns[lun].present;
}

const struct ufs_lun *ufs_lun_info(int lun) {
    return ufs_lun_present(lun) ? &luns[lun] : NULL;
}

static bool transfer(int lun, uint64_t lba, size_t count, void *buf, bool write) {
    if (!ufs_lun_present(lun) || count == 0) {
        return false;
    }

    struct ufs_lun *unit = &luns[lun];

    if (lba + count > unit->sectors || lba + count < lba) {
        return false;
    }

    if (!dma_reachable(buf, count * unit->sector_size, needs_low_dma)) {
        return false;
    }

    uint8_t *cursor = buf;
    size_t per_command = MAX_TRANSFER / unit->sector_size;

    while (count > 0) {
        size_t chunk = count < per_command ? count : per_command;
        uint8_t cdb[16];

        memset(cdb, 0, sizeof cdb);
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        if (!scsi_command((uint8_t)lun, cdb, 10, cursor,
                          chunk * unit->sector_size, write)) {
            return false;
        }

        lba += chunk;
        count -= chunk;
        cursor += chunk * unit->sector_size;
    }

    if (write) {
        uint8_t cdb[16];

        memset(cdb, 0, sizeof cdb);
        cdb[0] = SCSI_SYNCHRONIZE_CACHE;

        if (!scsi_command((uint8_t)lun, cdb, 10, NULL, 0, false)) {
            return false;
        }
    }

    return true;
}

bool ufs_read_sectors(int lun, uint64_t lba, size_t count, void *buf) {
    return transfer(lun, lba, count, buf, false);
}

bool ufs_write_sectors(int lun, uint64_t lba, size_t count, const void *buf) {
    return transfer(lun, lba, count, (void *)(uintptr_t)buf, true);
}
