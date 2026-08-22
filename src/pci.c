#include <io.h>
#include <pci.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR      0x00
#define PCI_REG_COMMAND     0x04
#define PCI_REG_CLASS       0x08
#define PCI_REG_HEADER_TYPE 0x0E

#define PCI_COMMAND_MEMORY (1u << 1)
#define PCI_COMMAND_MASTER (1u << 2)
#define PCI_COMMAND_INTX_DISABLE (1u << 10)

#define PCI_HEADER_MULTIFUNCTION 0x80

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, offset));

    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t value = pci_config_read32(bus, slot, func, (uint8_t)(offset & 0xFC));

    return (uint16_t)(value >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                        uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

static void command_set(const struct pci_device *dev, uint16_t bits) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);

    cmd = (uint16_t)(cmd | bits);

    outl(PCI_CONFIG_ADDRESS, config_address(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND));
    outl(PCI_CONFIG_DATA, cmd);
}

static bool function_present(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_config_read16(bus, slot, func, PCI_REG_VENDOR) != 0xFFFF;
}

static uint32_t address_of(uint8_t bus, uint8_t slot, uint8_t func) {
    return ((uint32_t)bus << 8) | ((uint32_t)slot << 3) | func;
}

static void describe(uint8_t bus, uint8_t slot, uint8_t func, struct pci_device *out) {
    uint32_t class_reg = pci_config_read32(bus, slot, func, PCI_REG_CLASS);

    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = pci_config_read16(bus, slot, func, PCI_REG_VENDOR);
    out->device_id = pci_config_read16(bus, slot, func, PCI_REG_VENDOR + 2);
    out->class_code = (uint8_t)(class_reg >> 24);
    out->subclass = (uint8_t)(class_reg >> 16);
    out->prog_if = (uint8_t)(class_reg >> 8);
}

bool pci_next(const struct pci_device *after, struct pci_device *out) {
    uint32_t start = after != NULL ? address_of(after->bus, after->slot, after->func) + 1 : 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (!function_present((uint8_t)bus, slot, 0)) {
                continue;
            }

            uint8_t header_type =
                (uint8_t)pci_config_read16((uint8_t)bus, slot, 0, PCI_REG_HEADER_TYPE);
            uint8_t max_func = (header_type & PCI_HEADER_MULTIFUNCTION) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                if (address_of((uint8_t)bus, slot, func) < start) {
                    continue;
                }
                if (!function_present((uint8_t)bus, slot, func)) {
                    continue;
                }

                describe((uint8_t)bus, slot, func, out);

                return true;
            }
        }
    }

    return false;
}

bool pci_find_class_after(uint8_t class_code, uint8_t subclass, int prog_if,
                          const struct pci_device *after, struct pci_device *out) {
    struct pci_device dev;
    const struct pci_device *cursor = after;

    while (pci_next(cursor, &dev)) {
        if (dev.class_code == class_code && dev.subclass == subclass
         && (prog_if == PCI_PROG_IF_ANY || dev.prog_if == (uint8_t)prog_if)) {
            *out = dev;
            return true;
        }

        *out = dev;
        cursor = out;
    }

    return false;
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     struct pci_device *out) {
    return pci_find_class_after(class_code, subclass, prog_if, NULL, out);
}

uint64_t pci_bar_address(const struct pci_device *dev, int index) {
    uint32_t low = pci_bar(dev, index);

    if (low & 1u) {
        return 0;
    }

    uint64_t addr = low & ~0xFu;

    if (((low >> 1) & 3u) == 2u && index < 5) {
        addr |= (uint64_t)pci_bar(dev, index + 1) << 32;
    }

    return addr;
}

void pci_disable_intx(const struct pci_device *dev) {
    command_set(dev, PCI_COMMAND_INTX_DISABLE);
}

uint32_t pci_bar(const struct pci_device *dev, int index) {
    return pci_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(0x10 + index * 4));
}

void pci_enable_bus_mastering(const struct pci_device *dev) {
    command_set(dev, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}
