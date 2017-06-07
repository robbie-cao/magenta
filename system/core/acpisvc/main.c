// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <acpica/acpi.h>
#include <magenta/process.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/resource.h>

#include "ec.h"
#include "pci.h"
#include "powerbtn.h"
#include "processor.h"
#include "resource_tree.h"

#define ACPI_MAX_INIT_TABLES 32

static ACPI_STATUS set_apic_irq_mode(void);
static ACPI_STATUS init(void);
static mx_status_t find_iommus(void);

mx_handle_t root_resource_handle;

int main(int argc, char** argv) {
    root_resource_handle = mx_get_startup_handle(PA_HND(PA_USER0, 0));
    if (root_resource_handle <= 0) {
        printf("Failed to find root resource handle\n");
        return 1;
    }

    // Get handle from devmgr to serve as the ACPI root handle
    mx_handle_t acpi_root = mx_get_startup_handle(PA_HND(PA_USER1, 0));
    if (acpi_root <= 0) {
        printf("Failed to find acpi root handle\n");
        return 1;
    }

    ACPI_STATUS status = init();
    if (status != MX_OK) {
        printf("Failed to initialize ACPI\n");
        return 3;
    }
    printf("Initialized ACPI\n");

    mx_handle_t port;
    mx_status_t mx_status = mx_port_create(0, &port);
    if (mx_status != MX_OK) {
        printf("Failed to construct resource port\n");
        return 4;
    }

    // TODO(teisenbe): In the future, devmgr should create this and hand it to
    // us.
    mx_handle_t acpi_bus_resource;
    {
        mx_rrec_t records[1] = { { 0 } };
        records[0].self.type = MX_RREC_SELF;
        records[0].self.subtype = MX_RREC_SELF_GENERIC;
        records[0].self.options = 0;
        records[0].self.record_count = 1;
        strncpy(records[0].self.name, "ACPI-BUS", sizeof(records[0].self.name));
        mx_status = mx_resource_create(root_resource_handle, records, countof(records),
                                       &acpi_bus_resource);
        if (mx_status != MX_OK) {
            printf("Failed to create ACPI-BUS resource\n");
            return 6;
        }
    }

    mx_status = resource_tree_init(port, acpi_bus_resource);
    if (mx_status != MX_OK) {
        printf("Failed to initialize resource tree\n");
        return 5;
    }

    ec_init();

    mx_status = install_powerbtn_handlers();
    if (mx_status != MX_OK) {
        printf("Failed to install powerbtn handler\n");
    }

    mx_status = find_iommus();
    if (mx_status != NO_ERROR) {
        printf("Failed to publish iommus\n");
    }

    mx_status = pci_report_current_resources(root_resource_handle);
    if (mx_status != MX_OK) {
        printf("WARNING: ACPI failed to report all current resources!\n");
    }

    return begin_processing(acpi_root);
}

static ACPI_STATUS init(void) {
    // This sequence is described in section 10.1.2.1 (Full ACPICA Initialization)
    // of the ACPICA developer's reference.
    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI\n");
        return status;
    }

    status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
    if (status == AE_NOT_FOUND) {
        printf("WARNING: could not find ACPI tables\n");
        return status;
    } else if (status == AE_NO_MEMORY) {
        printf("WARNING: could not initialize ACPI tables\n");
        return status;
    } else if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI tables for unknown reason\n");
        return status;
    }

    status = AcpiLoadTables();
    if (status != AE_OK) {
        printf("WARNING: could not load ACPI tables: %d\n", status);
        return status;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not enable ACPI\n");
        return status;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI objects\n");
        return status;
    }

    status = set_apic_irq_mode();
    if (status == AE_NOT_FOUND) {
        printf("WARNING: Could not find ACPI IRQ mode switch\n");
    } else if (status != AE_OK) {
        printf("Failed to set APIC IRQ mode\n");
        return status;
    }

    // TODO(teisenbe): Maybe back out of ACPI mode on failure, but we rely on
    // ACPI for some critical things right now, so failure will likely prevent
    // successful boot anyway.
    return AE_OK;
}

/* @brief Switch interrupts to APIC model (controls IRQ routing) */
static ACPI_STATUS set_apic_irq_mode(void) {
    ACPI_OBJECT selector = {
        .Integer.Type = ACPI_TYPE_INTEGER,
        .Integer.Value = 1, // 1 means APIC mode according to ACPI v5 5.8.1
    };
    ACPI_OBJECT_LIST params = {
        .Count = 1,
        .Pointer = &selector,
    };
    return AcpiEvaluateObject(NULL, (char*)"\\_PIC", &params, NULL);
}

mx_status_t find_iommus(void) {
    ACPI_TABLE_HEADER* table = NULL;
    ACPI_STATUS status = AcpiGetTable((char*)ACPI_SIG_DMAR, 1, &table);
    if (status != AE_OK) {
        printf("could not find DMAR\n");
        return ERR_NOT_FOUND;
    }
    ACPI_TABLE_DMAR* dmar = (ACPI_TABLE_DMAR*)table;
    uintptr_t records_start = ((uintptr_t)dmar) + sizeof(*dmar);
    uintptr_t records_end = ((uintptr_t)dmar) + dmar->Header.Length;
    if (records_start >= records_end) {
        printf("DMAR wraps around address space\n");
        return ERR_IO_DATA_INTEGRITY;
    }
    // Shouldn't be too many records
    if (dmar->Header.Length > 4096) {
        printf("DMAR suspiciously long: %u\n", dmar->Header.Length);
        return ERR_IO_DATA_INTEGRITY;
    }

    uintptr_t addr;
    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        printf("DMAR record: %d\n", record_hdr->Type);
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* dmar_rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;

                printf("DMAR Hardware Unit: %u %#llx %#x\n", dmar_rec->Segment, dmar_rec->Address, dmar_rec->Flags);
                size_t num_scopes = 0;
                uintptr_t scope;
                for (scope = addr + 16; scope < addr + record_hdr->Length; ) {
                    ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
                    printf("  DMAR Scope: %u, bus %u\n", s->EntryType, s->Bus);
                    for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
                        uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
                        printf("    Path %ld: %02x.%02x\n", i, v & 0xffu, (uint16_t)(v >> 8));
                    }
                    scope += s->Length;
                    num_scopes++;
                }

                mx_handle_t iommu_handle;
                const size_t num_records = 3 + num_scopes;
                mx_rrec_t* records = calloc(num_records, sizeof(mx_rrec_t));
                if (!records) {
                    printf("Failed to allocate records\n");
                    return ERR_NO_MEMORY;
                }
                records[0].self.type = MX_RREC_SELF;
                records[0].self.subtype = MX_RREC_SELF_IOMMU;
                records[0].self.options = 0;
                records[0].self.record_count = num_records;
                strncpy(records[0].self.name, "IOMMU", sizeof(records[0].self.name));

                records[1].mmio.type = MX_RREC_MMIO;
                records[1].mmio.subtype = 0;
                records[1].mmio.options = 0;
                records[1].mmio.phys_base = dmar_rec->Address;
                records[1].mmio.phys_size = 4096;

                records[2].data.type = MX_RREC_DATA;
                records[2].data.subtype = MX_RREC_DATA_U32;
                records[2].data.options = 2; /* count */
                records[2].data.u32[0] = dmar_rec->Segment;
                records[2].data.u32[1] = dmar_rec->Flags & ACPI_DMAR_INCLUDE_ALL;

                size_t scope_index = 0;
                for (scope = addr + 16; scope < addr + record_hdr->Length && scope_index < num_scopes; ++scope_index) {
                    ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
                    mx_rrec_t* rec = &records[3 + scope_index];

                    rec->data.type = MX_RREC_DATA;
                    rec->data.subtype = MX_RREC_DATA_U8;
                    rec->data.options = 1 + (s->Length - 6) / 2; /* count */
                    // TOOD: Check that options isn't too big
                    rec->data.u8[0] = s->Bus;
                    for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
                        uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
                        const uint8_t dev = v >> 8;
                        const uint8_t func = v & 0x7;
                        rec->data.u8[1 + i] = (dev << 3) | func;
                    }
                    scope += s->Length;
                }

                mx_status_t mx_status = mx_resource_create(root_resource_handle, records, num_records,
                                                           &iommu_handle);
                free(records);
                if (mx_status != NO_ERROR) {
                    printf("Failed to create\n");
                    return mx_status;
                }

#if 0
                if (dmar_rec->Address == 0xfed90000) {
                    mx_handle_t bti;
                    mx_status = mx_bti_create(iommu_handle, 2, &bti);
                    if (mx_status != NO_ERROR) {
                        printf("Failed to create bti\n");
                        return mx_status;
                    }

                    uint64_t addrs[];
                    uint32_t addrs_len;
                    uint32_t actual_addrs_len;
                    mx_status = mx_bti_pin(bti, vmo, offset, size, MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE, addrs, addrs_len, &actual_addrs_len);
                    if (mx_status != NO_ERROR) {
                        return mx_status;
                    }
                    // Leak BTI
                }
#endif
                mx_handle_close(iommu_handle);
                break;
            }
        }

        addr += record_hdr->Length;
    }
    if (addr != records_end) {
        return ERR_IO_DATA_INTEGRITY;
    }

    return NO_ERROR;
}
