/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <exec/types.h>
#include "io_apic.h"

#include "irq.h"
#include <libraries/gic400.h>

struct IoApicDomain {
    APTR key;
    struct IoApic apic;
    ULONG gsi_base;
};

struct InterruptDomains {
    const struct IoApicDomain* domains;
    ULONG count, max_irqs;
};

struct InterruptSource {
    const struct IoApicDomain* domain;
    ULONG pin;
};

// The library supplies already discovered controllers and explicit firmware GSI
// bases; tree order never assigns IRQ numbers. Retain this array unchanged for
// the registry's lifetime. Serialize initialization against all users. No memory
// is allocated and no registers are accessed. Failure preserves the old registry.
LONG initInterruptDomains(const struct IoApicDomain* domains, ULONG count, struct InterruptDomains* result);

// Convert public IRQ <-> controller-local pin. Holes, missing domains and pins
// outside the probed hardware extent fail without changing output. A mapping
// identifies wiring only: it does not grant ownership of firmware/native inputs,
// allocate an APIC vector, install a handler or enable delivery.
LONG findInterruptSource(const struct InterruptDomains* domains, ULONG irq, struct InterruptSource* source);
LONG findInterruptIrq(const struct InterruptDomains* domains, APTR key, ULONG pin, ULONG* irq);

// Read /emu68's ioapic-gsi-bases pairs through the original resource. platform
// is the caller's opened /emu68 key. Validate the full table, unique phandles
// and bases, and enabled I/O APIC targets before returning the requested base.
// A missing table/entry returns NOT_FOUND; malformed metadata returns DEVTREE.
// The number is a firmware GSI, not a public IRQ or pin count. Probe each input
// extent before initInterruptDomains checks IRQ limits and overlapping ranges.
// No MMIO, allocation or public library vector; failure preserves output.
LONG readIoApicGsiBase(APTR tree_base, APTR root, APTR platform, APTR controller, ULONG* gsi_base);

struct DtInterrupt;

// Library-owned composition of the controller binding decoder and the registered
// hardware domains. The resource/root and immutable domain registry must remain
// alive. Return public IRQ/trigger/polarity only after both validations succeed.
// No MMIO, allocation, handler registration, IRQ ownership or state changes.
// A resolved firmware source can still be reserved/unavailable for registration.
LONG resolveInterruptDescription(APTR tree_base, APTR root, const struct InterruptDomains* domains,
    const struct DtInterrupt* description, struct InterruptRoute* route);
