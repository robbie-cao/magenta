// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// needed to access mx_device_t children list in platform_dev_find_protocol()
#define DDK_INTERNAL 1

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/platform-device.h>
#include <magenta/listnode.h>
#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <mdi/mdi.h>
#include <mdi/mdi-defs.h>

typedef struct {
    mx_device_t* mxdev;
    list_node_t children;
    mx_handle_t resource;   // root resource for platform bus
} platform_bus_t;

typedef struct {
    mx_device_t* mxdev;
    platform_bus_t* bus;
    uint32_t proto_id;
    void* protocol;
    mx_handle_t resource;   // root resource for this device
    list_node_t node;
    mx_device_prop_t props[3];
} platform_dev_t;

static void platform_bus_release(void* ctx) {
    platform_bus_t* bus = ctx;
    free(bus);
}

static mx_protocol_device_t platform_bus_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = platform_bus_release,
};

static void platform_dev_release(void* ctx) {
    platform_dev_t* dev = ctx;
    free(dev);
}

static mx_protocol_device_t platform_dev_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = platform_dev_release,
};

static mx_status_t platform_dev_find_protocol(mx_device_t* dev, uint32_t proto_id,
                                       mx_device_t** out_dev, void** out_proto) {
    platform_dev_t* pdev = dev->ctx;
    platform_bus_t* bus = pdev->bus;

    list_for_every_entry(&bus->children, pdev, platform_dev_t, node) {
        // search children of our platform device nodes for the protocol
        mx_device_t* child;
        list_for_every_entry(&pdev->mxdev->children, child, mx_device_t, node) {
            if (device_op_get_protocol(child, proto_id, out_proto) == MX_OK) {
                *out_dev = child;
                return MX_OK;
            }
        }
    }

    return MX_ERR_NOT_FOUND;
}

static platform_device_protocol_t platform_dev_proto_ops = {
    .find_protocol = platform_dev_find_protocol,
};

static mx_status_t platform_bus_add_mmio(mdi_node_ref_t* node, mx_rrec_t** rec_p) {
    mx_rrec_t* rec = *rec_p;

    uint64_t base = 0;
    uint64_t length = 0;
    mdi_node_ref_t  child;
    mdi_each_child(node, &child) {
        switch (mdi_id(&child)) {
        case MDI_BASE_PHYS:
            mdi_node_uint64(&child, &base);
            break;
        case MDI_LENGTH:
            mdi_node_uint64(&child, &length);
            break;
        default:
            break;
        }
    }

    if (!base || !length) {
        printf("platform_bus_add_mmio: missing base or length\n");
        return MX_ERR_INVALID_ARGS;
    }

    memset(rec, 0, sizeof(*rec));
    rec->mmio.type = MX_RREC_MMIO;
    rec->mmio.phys_base = base;
    rec->mmio.phys_size = length;

    *rec_p = rec + 1;
    return MX_OK;
}

static mx_status_t platform_bus_add_irq(mdi_node_ref_t* node, mx_rrec_t** rec_p) {
    mx_rrec_t* rec = *rec_p;

    uint32_t irq = UINT32_MAX;
    mdi_node_ref_t  child;
    mdi_each_child(node, &child) {
        switch (mdi_id(&child)) {
        case MDI_IRQ:
            mdi_node_uint32(&child, &irq);
            break;
        default:
            break;
        }
    }

    if (irq == UINT32_MAX) {
        printf("platform_bus_add_irq: missing irq\n");
        return MX_ERR_INVALID_ARGS;
    }

    memset(rec, 0, sizeof(*rec));
    rec->irq.type = MX_RREC_IRQ;
    rec->irq.irq_base = irq;
    rec->irq.irq_count = 1;

    *rec_p = rec + 1;
    return MX_OK;
}

static mx_status_t platform_bus_create_resource(mx_handle_t parent, const char* name, mdi_node_ref_t* node, mx_handle_t* out_rsrc) {
    mx_status_t status = MX_OK;

    uint32_t child_count = mdi_child_count(node);
    if (child_count == 0) {
        printf("platform_bus_create_resource: resource node is empty\n");
        return MX_ERR_INVALID_ARGS;
    }

    uint32_t rec_count = child_count + 1;
    mx_rrec_t stack_buf[64];
    mx_rrec_t* records;
    if (rec_count > countof(stack_buf)) {
        records = calloc(1, sizeof(mx_rrec_t) * rec_count);
        if (!records) {
            return MX_ERR_NO_MEMORY;
        }
    } else {
        records = stack_buf;
    }

    mx_rrec_t* rec = records;
    rec->self.type = MX_RREC_SELF;
    rec->self.subtype = MX_RREC_SELF_GENERIC;
    strlcpy(rec->self.name, name, sizeof(rec->self.name));
    rec++;

    mdi_node_ref_t child;
    mdi_each_child(node, &child) {

        switch (mdi_id(&child)) {
        case MDI_PLATFORM_DEVICE_MMIO:
            status = platform_bus_add_mmio(&child, &rec);
            break;
        case MDI_PLATFORM_DEVICE_IRQ:
            status = platform_bus_add_irq(&child, &rec);
            break;
        default:
            printf("platform_bus_create_resource: unexpected node %u\n", mdi_id(&child));
            status = MX_ERR_INVALID_ARGS;
            break;
        }

        if (status != MX_OK) {
            return status;
        }
    }

    status = mx_resource_create(parent, records, rec - records, out_rsrc);

    if (records != stack_buf) {
        free(records);
    }

    return status;
}

static mx_status_t platform_bus_publish_devices(platform_bus_t* bus, mdi_node_ref_t* node) {
    mdi_node_ref_t  device_node;
    mdi_each_child(node, &device_node) {
        if (mdi_id(&device_node) != MDI_PLATFORM_DEVICE) {
            printf("platform_bus_publish_devices: unexpected node %d\n", mdi_id(&device_node));
            continue;
        }
        uint32_t vid = 0;
        uint32_t pid = 0;
        uint32_t did = 0;
        const char* name = NULL;
        mdi_node_ref_t  node;
        mdi_node_ref_t  resource_node;
        bool got_resource = false;
        mdi_each_child(&device_node, &node) {
            switch (mdi_id(&node)) {
            case MDI_NAME:
                name = mdi_node_string(&node);
                break;
            case MDI_PLATFORM_DEVICE_VID:
                mdi_node_uint32(&node, &vid);
                break;
            case MDI_PLATFORM_DEVICE_PID:
                mdi_node_uint32(&node, &pid);
                break;
            case MDI_PLATFORM_DEVICE_DID:
                mdi_node_uint32(&node, &did);
                break;
            case MDI_PLATFORM_DEVICE_RESOURCE:
                memcpy(&resource_node, &node, sizeof(resource_node));
                got_resource = true;
                break;
            default:
                break;
            }
        }

        if (!name || !vid || !pid || !did) {
            printf("platform_bus_publish_devices: missing name, vid, pid or did\n");
            continue;
        }

        platform_dev_t* dev = calloc(1, sizeof(platform_dev_t));
        if (!dev) {
            return MX_ERR_NO_MEMORY;
        }
        dev->bus = bus;

        // create resource for the device
        if (got_resource) {
            mx_status_t status = platform_bus_create_resource(bus->resource, name, &resource_node, &dev->resource);
            if (status != MX_OK) {
                free(dev);
                return status;
            }
        }

        mx_device_prop_t props[] = {
            {BIND_PLATFORM_DEV_VID, 0, vid},
            {BIND_PLATFORM_DEV_PID, 0, pid},
            {BIND_PLATFORM_DEV_DID, 0, did},
        };
        static_assert(countof(props) == countof(dev->props), "");
        memcpy(dev->props, props, sizeof(dev->props));

        device_add_args_t args = {
            .version = DEVICE_ADD_ARGS_VERSION,
            .name = name,
            .ctx = dev,
            .ops = &platform_dev_proto,
            .proto_id = MX_PROTOCOL_PLATFORM_DEV,
            .proto_ops = &platform_dev_proto_ops,
            .props = dev->props,
            .prop_count = countof(dev->props),
        };

        mx_status_t status = device_add(bus->mxdev, &args, &dev->mxdev);
        if (status != MX_OK) {
            printf("platform_bus_publish_devices: failed to create device for %u:%u:%u\n",
                   vid, pid, did);
            mx_handle_close(dev->resource);
            free(dev);
            return status;
        }
        list_add_tail(&bus->children, &dev->node);
    }

    return MX_OK;
}

static mx_status_t platform_bus_bind(void* ctx, mx_device_t* parent, void** cookie) {
    mx_handle_t mdi_handle = device_get_resource(parent);
    if (mdi_handle == MX_HANDLE_INVALID) {
        printf("platform_bus_bind: mdi_handle invalid\n");
        return MX_ERR_NOT_SUPPORTED;
    }

    platform_bus_t* bus = NULL;
    void* addr = NULL;
    size_t size;
    mx_status_t status = mx_vmo_get_size(mdi_handle, &size);
    if (status != MX_OK) {
        printf("platform_bus_bind: mx_vmo_get_size failed %d\n", status);
        goto fail;
    }
    status = mx_vmar_map(mx_vmar_root_self(), 0, mdi_handle, 0, size, MX_VM_FLAG_PERM_READ,
                         (uintptr_t *)&addr);
    if (status != MX_OK) {
        printf("platform_bus_bind: mx_vmar_map failed %d\n", status);
        goto fail;
    }

    mdi_node_ref_t root_node;
    status = mdi_init(addr, size, &root_node);
    if (status != MX_OK) {
        printf("platform_bus_bind: mdi_init failed %d\n", status);
        goto fail;
    }

    mdi_node_ref_t  bus_node;
    if (mdi_find_node(&root_node, MDI_PLATFORM, &bus_node) != MX_OK) {
        printf("platform_bus_bind: couldn't find MDI_PLATFORM\n");
        goto fail;
    }

    bus = calloc(1, sizeof(platform_bus_t));
    if (!bus) {
        status = MX_ERR_NO_MEMORY;
        goto fail;
    }
    list_initialize(&bus->children);

    // TODO(voydanoff) Later this resource will be passed to us from the devmgr
    mx_rrec_t records[1] = { { 0 } };
    records[0].self.type = MX_RREC_SELF;
    records[0].self.subtype = MX_RREC_SELF_GENERIC;
    records[0].self.options = 0;
    records[0].self.record_count = 1;
    strlcpy(records[0].self.name, "PLATFORM-BUS", sizeof(records[0].self.name));
    status = mx_resource_create(get_root_resource(), records, countof(records),
                                   &bus->resource);

    device_add_args_t add_args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "platform-bus",
        .ctx = bus,
        .ops = &platform_bus_proto,
    };

    status = device_add(parent, &add_args, &bus->mxdev);
    if (status != MX_OK) {
        goto fail;
    }

    return platform_bus_publish_devices(bus, &bus_node);

fail:
    if (bus) {
        mx_handle_close(bus->resource);
        free(bus);
    }
    if (addr) {
        mx_vmar_unmap(mx_vmar_root_self(), (uintptr_t)addr, size);
    }
    mx_handle_close(mdi_handle);
    return status;
}

static mx_driver_ops_t platform_bus_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = platform_bus_bind,
};

MAGENTA_DRIVER_BEGIN(platform_bus, platform_bus_driver_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_PLATFORM_BUS),
MAGENTA_DRIVER_END(platform_bus)
