// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/iommu/intel.h>
#include <lk/init.h>
#include <magenta/resource_dispatcher.h>
#include "iommu_impl.h"

status_t IntelIommu::Create(const mx_iommu_desc_intel_t* desc, uint32_t desc_len,
                           mxtl::RefPtr<Iommu>* out) {
    return intel_iommu::IommuImpl::Create(desc, desc_len, out);
}

status_t IntelIommu::CreateFromResource(mxtl::RefPtr<ResourceDispatcher> rsrc,
                                        mxtl::RefPtr<Iommu>* out) {
    // Intel IOMMU resources have an MMIO record for the first entry
    // TODO: Validate more aggressively
    mx_rrec_t rec;
    status_t status = rsrc->GetNthRecord(0, &rec);
    if (status != NO_ERROR || rec.type != MX_RREC_MMIO) {
        return ERR_NOT_SUPPORTED;
    }

    mxtl::RefPtr<Iommu> iommu = IntelIommu::Create(rsrc->get_koid(), rec.mmio.phys_base);
    if (!iommu) {
        return ERR_NO_MEMORY;
    }

    // TODO: Remove this
    if (rec.mmio.phys_base == 0xfed90000) {
        uintptr_t vaddr;
        status = iommu->Map(0x2 << 3, 0x8c000000, 1ull<<25, IOMMU_FLAG_PERM_WRITE | IOMMU_FLAG_PERM_READ, &vaddr);
        DEBUG_ASSERT(status == NO_ERROR);
    }

    *out = mxtl::move(iommu);
    return NO_ERROR;
}

IommuDriver IntelIommu::drv_(&IntelIommu::CreateFromResource);

void IntelIommu::RegisterDriver(unsigned int) {
    Iommu::RegisterIommuDriver(&drv_);
}
LK_INIT_HOOK(intel_iommu_register, IntelIommu::RegisterDriver, LK_INIT_LEVEL_KERNEL);
