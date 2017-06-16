// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <magenta/pinned_memory_object.h>

#include <assert.h>
#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_object.h>
#include <magenta/bus_transaction_initiator_dispatcher.h>
#include <mxalloc/new.h>
#include <mxtl/auto_call.h>

namespace {

struct IommuMapPageContext {
    mxtl::RefPtr<Iommu> iommu;
    uint64_t bus_txn_id;
    PinnedMemoryObject::Extent* page_array;
    size_t num_entries;
    uint32_t perms;
};

// Callback for VmObject::Lookup that handles mapping individual pages into the IOMMU.
status_t IommuMapPage(void* context, size_t offset, size_t index, paddr_t pa) {
    IommuMapPageContext* ctx = static_cast<IommuMapPageContext*>(context);

    dev_vaddr_t vaddr;
    status_t status = ctx->iommu->Map(ctx->bus_txn_id, pa, PAGE_SIZE, ctx->perms, &vaddr);
    if (status != MX_OK) {
        return status;
    }

    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    if (ctx->num_entries == 0) {
        ctx->page_array[0] = PinnedMemoryObject::Extent(vaddr, 1);
        ctx->num_entries++;
        return MX_OK;
    }

    PinnedMemoryObject::Extent* prev_extent = &ctx->page_array[ctx->num_entries - 1];
    if (prev_extent->base() + prev_extent->pages() * PAGE_SIZE == vaddr &&
        prev_extent->extend(1) == MX_OK) {

        return MX_OK;
    }
    ctx->page_array[ctx->num_entries] = PinnedMemoryObject::Extent(vaddr, 1);
    ctx->num_entries++;
    return MX_OK;
}

} // namespace {}

status_t PinnedMemoryObject::Create(const BusTransactionInitiatorDispatcher& bti,
                                    mxtl::RefPtr<VmObject> vmo, size_t offset,
                                    size_t size, uint32_t perms,
                                    mxtl::unique_ptr<PinnedMemoryObject>* out) {

    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    // Pin the memory to make sure it doesn't change from underneath us for the
    // lifetime of the created PMO.
    status_t status = vmo->Pin(offset, size);
    if (status != MX_OK) {
        return status;
    }

    // Set up a cleanup function to undo the pin if we need to fail this
    // operation.
    auto unpin_vmo = mxtl::MakeAutoCall([vmo, offset, size]() {
        vmo->Unpin(offset, size);
    });

    // TODO(teisenbe): Be more intelligent about allocating this, since if this
    // is backed by a real IOMMU, we will likely compress the page array greatly
    // using extents.
    AllocChecker ac;
    const size_t num_pages = size / PAGE_SIZE;
    mxtl::unique_ptr<Extent[]> page_array(new (&ac) Extent[num_pages]);
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    mxtl::unique_ptr<PinnedMemoryObject> pmo(
            new (&ac) PinnedMemoryObject(bti, mxtl::move(vmo), offset, size,
                                         mxtl::move(page_array)));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    // Now that the pmo object has been created, it is responsible for
    // unpinning.
    unpin_vmo.cancel();

    status = pmo->MapIntoIommu(perms);
    if (status != MX_OK) {
        return status;
    }

    *out = mxtl::move(pmo);
    return MX_OK;
}

// Used during initialization to set up the IOMMU state for this PMO.
status_t PinnedMemoryObject::MapIntoIommu(uint32_t perms) {
    IommuMapPageContext context = {
        .iommu = bti_.iommu(),
        .bus_txn_id = bti_.bti_id(),
        .page_array = mapped_extents_.get(),
        .num_entries = 0,
        .perms = perms,
    };
    status_t status = vmo_->Lookup(offset_, size_, 0, IommuMapPage, static_cast<void*>(&context));
    if (status != MX_OK) {
        status_t err = UnmapFromIommu();
        ASSERT(err == MX_OK);
        return status;
    }

    return MX_OK;
}

status_t PinnedMemoryObject::UnmapFromIommu() {
    auto iommu = bti_.iommu();
    const uint64_t bus_txn_id = bti_.bti_id();

    status_t status = MX_OK;
    for (size_t i = 0; i < mapped_extents_len_; ++i) {
        // Try to unmap all pages even if we get an error, and return the
        // first error encountered.
        status_t err = iommu->Unmap(bus_txn_id, mapped_extents_[i].base(),
                                    mapped_extents_[i].pages() * PAGE_SIZE);
        if (err != MX_OK && status == MX_OK) {
            status = err;
        }
    }

    return status;
}

PinnedMemoryObject::~PinnedMemoryObject() {
    status_t status = UnmapFromIommu();
    ASSERT(status == MX_OK);
    vmo_->Unpin(offset_, size_);
}

PinnedMemoryObject::PinnedMemoryObject(const BusTransactionInitiatorDispatcher& bti,
                                       mxtl::RefPtr<VmObject> vmo, size_t offset, size_t size,
                                       mxtl::unique_ptr<Extent[]> mapped_extents)
    : vmo_(mxtl::move(vmo)), offset_(offset), size_(size), bti_(bti),
      mapped_extents_(mxtl::move(mapped_extents)), mapped_extents_len_(0) {
}
