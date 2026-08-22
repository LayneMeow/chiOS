#ifndef _PCI_H
#define _PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                        uint16_t value);

#define PCI_PROG_IF_ANY (-1)

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     struct pci_device *out);

bool pci_next(const struct pci_device *after, struct pci_device *out);

bool pci_find_class_after(uint8_t class_code, uint8_t subclass, int prog_if,
                          const struct pci_device *after, struct pci_device *out);

uint32_t pci_bar(const struct pci_device *dev, int index);

uint64_t pci_bar_address(const struct pci_device *dev, int index);

void pci_disable_intx(const struct pci_device *dev);

void pci_enable_bus_mastering(const struct pci_device *dev);

#endif
