// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>

#include <acpisvc/simple.h>
#include <magenta/types.h>
#include <magenta/syscalls.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static mx_protocol_device_t acpi_device_proto = {
    .version = DEVICE_OPS_VERSION,
};

typedef bool (*resource_walk_callback_t)(mx_handle_t resource, mx_rrec_t* child_rec, void* ctx);

static mx_status_t resource_walk(mx_handle_t resource, resource_walk_callback_t per_child_callback, void* ctx) {
    mx_rrec_t rec;
    mx_status_t status = mx_object_get_info(resource, MX_INFO_RESOURCE_RECORDS, &rec, sizeof(rec), 0, 0);
    if (status != NO_ERROR) {
        return status;
    }
    if (rec.type != MX_RREC_SELF) {
        return status;
    }
    size_t count;
    size_t list_size = sizeof(mx_rrec_t) * rec.self.child_count;
    mx_rrec_t* list = malloc(list_size);
    if (!list) {
        return ERR_NO_MEMORY;
    }
    status = mx_object_get_info(resource, MX_INFO_RESOURCE_CHILDREN, list, list_size, &count, 0);
    if (status != NO_ERROR) {
        free(list);
        return status;
    }
    for (size_t i = 0; i < count; i++) {
        bool next = per_child_callback(resource, &list[i], ctx);
        if (!next) {
            break;
        }
    }
    free(list);
    return NO_ERROR;
}

static bool top_level_walk_callback(mx_handle_t resource, mx_rrec_t* child_rec, void* ctx) {
    if (!strcmp(child_rec->self.name, "ACPI:_SB_")) {
        return true;
    }
    mx_handle_t* sb_resource = (mx_handle_t*)ctx;
    mx_object_get_child(resource, child_rec->self.koid, MX_RIGHT_SAME_RIGHTS, sb_resource);
    return false; // break
}

static bool sb_walk_callback(mx_handle_t resource, mx_rrec_t* child_rec, void* ctx) {
    mx_handle_t dev_resource;
    mx_status_t status = mx_object_get_child(resource, child_rec->self.koid, MX_RIGHT_SAME_RIGHTS, &dev_resource);
    if (status != NO_ERROR) {
        printf("acpi: failed to get resource for %s\n", child_rec->self.name);
        return true;
    }
    mx_rrec_t devrec[3] = { { 0 } };
    size_t count;
    status = mx_object_get_info(dev_resource, MX_INFO_RESOURCE_RECORDS, devrec, sizeof(devrec), &count, 0);
    if (status != NO_ERROR) {
        printf("acpi: failed to get resource records for %s\n", child_rec->self.name);
        return true;
    }
    if (count != 3) {
        printf("acpi: unexpected record count for %s (%zd)\n", child_rec->self.name, count);
        return true;
    }
    if (devrec[1].type != MX_RREC_DATA) {
        printf("acpi: no HID/ADR record\n");
        return true;
    }
    uint32_t* hid = (uint32_t*)&devrec[1].data.u64[0];
    // add the device
    // TODO(yky): improve binding
    mx_device_prop_t device_props[] = {
        (mx_device_prop_t){ BIND_PROTOCOL, 0, MX_PROTOCOL_ACPI },
        (mx_device_prop_t){ BIND_ACPI_HID_0_3, 0, (uint32_t)htobe32(*(hid)) },
        (mx_device_prop_t){ BIND_ACPI_HID_4_7, 0, (uint32_t)htobe32(*(hid + 1)) },
    };
    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = child_rec->self.name,
        .ctx = 0,
        .ops = &acpi_device_proto,
        .proto_id = MX_PROTOCOL_ACPI,
        .proto_ops = 0,
        .props = device_props,
        .prop_count = countof(device_props),
        .busdev_args = 0,
        .rsrc = dev_resource,
        .flags = DEVICE_ADD_BUSDEV,
    };
    mx_device_t* parent = (mx_device_t*)ctx;
    mx_device_t* dev;
    if (device_add(parent, &args, &dev) != NO_ERROR) {
        printf("acpi: failed to add device for %s\n", child_rec->self.name);
    }
    return true;
}

static mx_status_t acpi_bus_bind(void* ctx, mx_device_t* dev, void** cookie) {
    mx_handle_t acpi_resource = device_get_resource(dev);
    if (acpi_resource == MX_HANDLE_INVALID) {
        return ERR_NOT_SUPPORTED;
    }

    // look for the _SB_ (system bus) resource. only devices under this scope
    // are published
    mx_handle_t sb_resource = MX_HANDLE_INVALID;
    mx_status_t status = resource_walk(acpi_resource, top_level_walk_callback, &sb_resource);
    if (status != NO_ERROR) {
        return status;
    }
    if (sb_resource == MX_HANDLE_INVALID) {
        return ERR_NOT_SUPPORTED;
    }
    // walk the _SB_ scope and publish top level devices
    status = resource_walk(sb_resource, sb_walk_callback, dev);
    mx_handle_close(sb_resource);
    return status;
}

static mx_status_t acpi_bus_create(void* ctx, mx_device_t* parent, const char* name, const char* args, mx_handle_t resource) {
    if (resource == MX_HANDLE_INVALID) {
        printf("acpi: create with bad resource\n");
    }
    mx_rrec_t devrec[3] = { { 0 } };
    size_t count;
    mx_status_t status = mx_object_get_info(resource, MX_INFO_RESOURCE_RECORDS, devrec, sizeof(devrec), &count, 0);
    if (status != NO_ERROR) {
        printf("acpi: create failed to get resource records for %s\n", name);
        return true;
    }
    if (count != 3) {
        printf("acpi: create unexpected record count for %s (%zd)\n", name, count);
        return true;
    }
    if (devrec[1].type != MX_RREC_DATA) {
        printf("acpi: create no HID/ADR record for %s\n", name);
        return true;
    }
    printf("acpi: create device %s\n", name);
    return NO_ERROR;
}

static mx_driver_ops_t acpi_bus_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = acpi_bus_bind,
    .create = acpi_bus_create,
};

MAGENTA_DRIVER_BEGIN(acpi_bus, acpi_bus_driver_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_ACPI_BUS),
MAGENTA_DRIVER_END(acpi_bus)
