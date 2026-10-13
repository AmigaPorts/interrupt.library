/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <exec/types.h>
#include <stdint.h>

#include "devicetree_interrupt.h"
#include "devicetree_address.h"

enum ApicKind { APIC_LOCAL, APIC_IO };

struct ApicWindow {
    APTR key;
    ULONG address, size;
    BOOL virtual_wire;
};

// Library-owned discovery through the original resource's public vectors.
// previous is NULL initially, then the key returned for that controller kind.
// Success returns zero; NOT_FOUND ends enumeration, DEVTREE rejects a malformed
// matching node. Failure leaves output unchanged. The key is the IRQ domain's
// identity, not an enumeration-based IRQ number. No registers are accessed.
LONG findApicWindow(APTR tree_base, APTR root, enum ApicKind kind, APTR previous,
    struct ApicWindow* window);

struct IoApicInterrupt {
    struct ApicWindow controller;
    ULONG pin;
    BOOL active_low, level_triggered;
};

// Interpret a resolved interrupt in its enabled I/O APIC domain. The ce4100
// binding uses sense codes 0..3, not generic interrupt flag bits. Preserve the
// controller identity and its local pin; this does not allocate a public IRQ,
// select a vector/destination or touch registers. Pin capacity is checked against
// discovered hardware when routing. Failure leaves output unchanged.
LONG readIoApicInterrupt(APTR tree_base, APTR root, const struct DtInterrupt* interrupt,
    struct IoApicInterrupt* result);

// Public route masks have eight CPU-role bits. Firmware may contain additional
// processors that Emu68 does not use; only explicitly assigned roles belong here.
struct InterruptCpus {
    ULONG count;
    ULONG apic_ids[8];
};

// Read /emu68/cpu-apic-ids using the original resource. The caller supplies its
// opened /emu68 key. Preserve full physical IDs; a routing backend must reject
// destinations its hardware cannot address, rather than truncate them. Missing
// metadata returns NOT_FOUND, malformed metadata DEVTREE. Failure preserves
// output. This describes CPU roles only, not vector or controller ownership.
LONG readInterruptCpus(APTR tree_base, APTR platform, struct InterruptCpus* cpus);

// Read the selected CPU role's installed guest-delivery bitmap. Validate every
// row against cpu-apic-ids, including reserved exception/spurious vectors. An
// empty row means that CPU has no guest-delivery entry. Missing metadata returns
// NOT_FOUND; do not infer a usable range. Failure preserves all output words.
// No MMIO, vector allocation, routing or controller ownership is performed.
LONG readInterruptVectors(APTR tree_base, APTR platform, ULONG cpu, uint32_t available[8]);
