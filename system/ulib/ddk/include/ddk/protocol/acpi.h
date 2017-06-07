// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/compiler.h>
#include <ddk/driver.h>

__BEGIN_CDECLS;

/**
 * protocol/acpi.h - ACPI protocol definitions
 */

typedef struct mx_acpi_protocol {
    // TODO(yky) TBD
} mx_acpi_protocol_t;

#define ACPI_HID_LID_0_3 0x504e5030 // "PNP0"
#define ACPI_HID_LID_4_7 0x43304400 // "C0D"
