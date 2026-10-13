/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

struct LocalApic {
    volatile uint32_t* registers;
};

enum LocalApicBitmap { LOCAL_APIC_PENDING, LOCAL_APIC_IN_SERVICE };

// Internal xAPIC MMIO access, not additional public library vectors. The
// caller supplies the device-tree window mapped as uncached device memory.
// This does not enable, reset or reconfigure the controller. Failure leaves
// output unchanged. x2APIC has no MMIO interface and cannot use this backend.
int32_t openLocalApic(volatile void* address, uint32_t size, struct LocalApic* apic);

// These operate on the calling CPU's local controller. The library must keep
// native IRQs masked across identification, dispatch bookkeeping and EOI, and
// ensure that no other handler acknowledges its outstanding interrupt.
// Highest returns a vector or GIC400_ERR_NOT_FOUND when the bitmap is empty.
// IRR is raw hardware pending state, not filtered by TPR or guest priorities.
int32_t highestLocalApic(const struct LocalApic* apic, enum LocalApicBitmap bitmap);
int32_t localApicStatus(const struct LocalApic* apic, unsigned vector, bool* pending, bool* active);

// EOI always clears the highest in-service vector. Refuse an empty ISR or a
// different top vector so an out-of-order completion cannot end another IRQ.
// Device acknowledgement must precede this call for a level-triggered source.
int32_t completeLocalApic(const struct LocalApic* apic, unsigned vector);

// Queue a fixed, edge-triggered self interrupt using the xAPIC ICR shorthand.
// The library must own the vector and serialize local ICR use with native IRQs
// excluded. A busy ICR returns NOT_READY without writing; the caller retains its
// pending request and retries. Success submits the request but does not wait for
// delivery, change masking/priority or acknowledge anything. Repeated requests
// for one vector can coalesce in the APIC; this is a wakeup, not an event counter.
int32_t requestLocalApic(const struct LocalApic* apic, unsigned vector);
