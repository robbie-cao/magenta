// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <assert.h>
#include <hw/inout.h>
// TODO(cja) The next major CL should move these into some common place so that
// PciConfig and userspace code can use them.
#define PCI_CONFIG_ADDRESS (0xCF8)
#define PCI_CONFIG_DATA    (0xCFC)
#define PCI_BDF_ADDR(bus, dev, func, off) \
    ((1 << 31) | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) | ((func & 0x7) << 8) | (off & 0xFC))

static Mutex lock;
#if defined(ARCH_X86_64)
void pci_pio_cfg_read(uint8_t bus, uint8_t dev, uint8_t func,
                                uint8_t offset, uint32_t* val, size_t width) {
    AutoLock(&lock);
    size_t shift = (offset & 0x3) * 8;

    if (shift + width > 32) {
        printf("ACPI: error, pio cfg read width %zu not aligned to reg %#2x\n", width, offset);
        return;
    }

    uint32_t addr = PCI_BDF_ADDR(bus, dev, func, offset);
    outpd(PCI_CONFIG_ADDRESS, addr);
    uint32_t tmp_val = inpd(PCI_CONFIG_DATA);
    uint32_t width_mask = (1 << width) - 1;

    // Align the read to the correct offset, then mask based on byte width
    *val = (tmp_val >> shift) & width_mask;
}

void pci_pio_cfg_write(uint16_t bus, uint16_t dev, uint16_t func,
                              uint8_t offset, uint32_t val, size_t width) {
    AutoLock(&lock);
    size_t shift = (offset & 0x3) * 8;
    uint32_t width_mask = (1 << width) - 1;
    uint32_t write_mask = width_mask << shift;

    if (shift + width > 32) {
        printf("ACPI: error, pio cfg write width %zu not aligned to reg %#2x\n", width, offset);
    }

    uint32_t addr = PCI_BDF_ADDR(bus, dev, func, offset);
    outpd(PCI_CONFIG_ADDRESS, addr);
    uint32_t tmp_val = inpd(PCI_CONFIG_DATA);

    val &= width_mask;
    tmp_val &= ~write_mask;
    tmp_val |= (val << shift);
    outpd(PCI_CONFIG_DATA, tmp_val);
}
#else
void pci_pio_cfg_read(uint8_t bus, uint8_t dev, uint8_t func,
                      uint8_t offset, uint32_t* val, size_t width) {
    PANIC_UNIMPLEMENTED;
}

void pci_pio_cfg_write(uint16_t bus, uint16_t dev, uint16_t func,
                              uint8_t offset, uint32_t val, size_t width) {
    PANIC_UNIMPLEMENTED;
}
