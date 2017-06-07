// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acpi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <launchpad/launchpad.h>

#include <acpisvc/simple.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <mxio/io.h>
#include <mxio/util.h>

#include "devmgr.h"
#include "devhost.h"
#include "devcoordinator.h"

static acpi_handle_t acpi_root;

mx_status_t devhost_launch_acpisvc(mx_handle_t job_handle) {
    const char* binname = "/boot/bin/acpisvc";

    mx_status_t status;
    mx_handle_t logger = MX_HANDLE_INVALID;
    mx_handle_t root = MX_HANDLE_INVALID;
    mx_handle_t rpc[2] = { MX_HANDLE_INVALID, MX_HANDLE_INVALID };
    mx_handle_t acpi_bus_rsrc, acpi_bus_rsrc_clone;
    mx_log_create(0, &logger);
    mx_handle_duplicate(get_root_resource(), MX_RIGHT_SAME_RIGHTS, &root);
    mx_channel_create(0, &rpc[0], &rpc[1]);
    {
        mx_rrec_t records[1] = { { 0 } };
        records[0].self.type = MX_RREC_SELF;
        records[0].self.subtype = MX_RREC_SELF_GENERIC;
        records[0].self.options = 0;
        records[0].self.record_count = 1;
        strncpy(records[0].self.name, "ACPI-BUS", sizeof(records[0].self.name));
        status = mx_resource_create(root, records, countof(records),
                                       &acpi_bus_rsrc);
        if (status != NO_ERROR) {
            mx_handle_close(rpc[0]);
            mx_handle_close(rpc[1]);
            printf("devmgr: failed to create acpi-bus resource\n");
            return status;
        }
        // make a clone to pass to the acpi driver
        status = mx_handle_duplicate(acpi_bus_rsrc, MX_RIGHT_SAME_RIGHTS, &acpi_bus_rsrc_clone);
        if (status != NO_ERROR) {
            mx_handle_close(rpc[0]);
            mx_handle_close(rpc[1]);
            mx_handle_close(acpi_bus_rsrc);
            printf("devmgr: failed to clone acpi-bus resource handle\n");
            return status;
        }
    }

    launchpad_t* lp;
    launchpad_create(job_handle, binname, &lp);
    launchpad_load_from_file(lp, binname);
    launchpad_set_args(lp, 1, &binname);
    launchpad_clone(lp, LP_CLONE_ALL & (~LP_CLONE_MXIO_STDIO));
    launchpad_add_handle(lp, logger, PA_HND(PA_MXIO_LOGGER, MXIO_FLAG_USE_FOR_STDIO | 1));
    launchpad_add_handle(lp, root, PA_HND(PA_USER0, 0));
    launchpad_add_handle(lp, rpc[1], PA_HND(PA_USER1, 0));
    launchpad_add_handle(lp, acpi_bus_rsrc, PA_HND(PA_USER2, 0));

    const char* errmsg;
    status = launchpad_go(lp, NULL, &errmsg);
    if (status < 0) {
        mx_handle_close(rpc[0]);
        mx_handle_close(acpi_bus_rsrc_clone);
        printf("devmgr: acpisvc launch failed: %d: %s\n", status, errmsg);
        return status;
    }

    acpi_handle_init(&acpi_root, rpc[0]);
    devmgr_set_acpi_resource(acpi_bus_rsrc_clone);
    return NO_ERROR;
}

// TODO(teisenbe): Instead of doing this as a single function, give the kpci
// driver a handle to the PCIe root complex ACPI node and let it ask for
// the initialization info.
mx_status_t devhost_init_pcie(void) {
    char name[4] = {0};
    {
        acpi_rsp_list_children_t* rsp;
        size_t len;
        mx_status_t status = acpi_list_children(&acpi_root, &rsp, &len);
        if (status != NO_ERROR) {
            return status;
        }

        for (uint32_t i = 0; i < rsp->num_children; ++i) {
            if (!memcmp(rsp->children[i].hid, "PNP0A08", 7)) {
                memcpy(name, rsp->children[i].name, 4);
                break;
            }
        }
        free(rsp);

        if (name[0] == 0) {
            return ERR_NOT_FOUND;
        }
    }

    acpi_handle_t pcie_handle;
    mx_status_t status = acpi_get_child_handle(&acpi_root, name, &pcie_handle);
    if (status != NO_ERROR) {
        return status;
    }

    acpi_rsp_get_pci_init_arg_t* rsp;
    size_t len;
    status = acpi_get_pci_init_arg(&pcie_handle, &rsp, &len);
    if (status != NO_ERROR) {
        acpi_handle_close(&pcie_handle);
        return status;
    }
    acpi_handle_close(&pcie_handle);

    len -= offsetof(acpi_rsp_get_pci_init_arg_t, arg);
    status = mx_pci_init(get_root_resource(), &rsp->arg, len);

    free(rsp);
    return status;
}

void devhost_acpi_poweroff(void) {
    acpi_s_state_transition(&acpi_root, ACPI_S_STATE_S5);
    mx_debug_send_command(get_root_resource(), "poweroff", sizeof("poweroff"));
}

void devhost_acpi_reboot(void) {
    acpi_s_state_transition(&acpi_root, ACPI_S_STATE_REBOOT);
    mx_debug_send_command(get_root_resource(), "reboot", sizeof("reboot"));
}

void devhost_acpi_ps0(char* arg) {
    acpi_ps0(&acpi_root, arg, strlen(arg));
}
