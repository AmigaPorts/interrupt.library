/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_INPUTS_MAX ((256 - 0x10) / 2)

struct IoApic {
    volatile uint32_t* registers;
    unsigned input_count;
    uint8_t id, version;
};

struct IoApicState {
    uint8_t destination, vector, delivery_mode;
    bool logical_destination, active_low, level_triggered;
    bool masked, delivery_pending, remote_irr;
};

// Internal register access, not new public vectors. The caller supplies an
// uncached device-tree window and owns the entire selector/window interface.
// Serialize every operation with interrupts excluded; another CPU or native
// driver must not access this controller under an independent lock. Pin numbers
// are local to this controller, not enumeration-derived global IRQ numbers.
// Open probes capabilities without changing any route; failure leaves output
// unchanged. The library's routing/ownership layer must reserve firmware and
// native inputs before exposing any pin to public callers.
int32_t openIoApic(volatile void* address, uint32_t size, struct IoApic* apic);
int32_t ioApicState(const struct IoApic* apic, unsigned pin, struct IoApicState* state);

// Fixed delivery to one physical xAPIC ID. Mask and drain posted writes before
// changing either half of the route. The caller must also drain any vector
// already accepted by a CPU before reusing it. An outstanding delivery or
// level-triggered remote IRR returns NOT_READY and leaves the old route masked,
// awaiting its normal EOI.
// Invalid arguments change no registers. Successful routing also leaves the
// input masked; only maskIoApic(false) enables it. NMI/SMI/ExtINT and logical
// routes are not library-owned device interrupts and cannot be enabled here.
int32_t routeIoApic(const struct IoApic* apic, unsigned pin, unsigned vector,
    unsigned destination, bool active_low, bool level_triggered);
int32_t maskIoApic(const struct IoApic* apic, unsigned pin, bool masked);
