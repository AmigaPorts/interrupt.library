/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include "local_apic.h"
#include "irq.h"

struct InterruptVectors {
    uint32_t available[8];
    uint16_t irq_for_vector[256];
    uint8_t vector_for_irq[INTERRUPT_IRQ_LIMIT];
};

// One map per destination CPU. The caller supplies only vectors it exclusively
// owns, and keeps this map paired with that CPU's local APIC. No native-vector
// range is assumed. Initialize before use; exclude all users during replacement.
int32_t initInterruptVectors(const uint32_t available[8], struct InterruptVectors* vectors);

// Call with native interrupts excluded and the device source quiesced. Bind and
// release require no pending/in-service instance on this CPU. A repeated exact
// binding succeeds; one IRQ cannot acquire two vectors in the same map. Mapping
// does not claim a device input or program a route. Failed operations change no
// mappings. The caller must separately drain delivery on other destination CPUs.
int32_t bindInterruptVector(struct InterruptVectors* vectors, const struct LocalApic* apic,
    uint32_t irq, unsigned vector);
int32_t releaseInterruptVector(struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t irq);

// Identify only the highest in-service vector; never skip a foreign interrupt
// to dispatch a lower one. Lookup changes neither hardware nor output on failure.
// Completion requires the same mapping still to own the highest ISR vector.
// Acknowledge a level-triggered device before completing its interrupt here.
int32_t activeInterruptIrq(const struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t* irq);
int32_t completeInterruptIrq(const struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t irq);
