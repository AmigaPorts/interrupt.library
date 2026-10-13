/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "pending.h"
#include "vector.h"
#include <libraries/gic400.h>

struct MessageInterrupts {
    unsigned first_irq, blocks;
    uint8_t allocation[INTERRUPT_IRQ_LIMIT];
};

// Initialize above all firmware-described wired IRQs. The owner retains this
// registry alongside its vector map and pending queue, and excludes native
// arrivals and all other users while allocating/releasing. No device access.
int32_t initMessageInterrupts(unsigned first_irq, struct MessageInterrupts* messages);
static inline bool isMessageInterrupt(const struct MessageInterrupts* messages, unsigned irq)
{
    return messages && irq >= messages->first_irq && irq < INTERRUPT_IRQ_LIMIT && messages->allocation[irq];
}

// Exact power-of-two block (1..32), consecutive and naturally aligned native
// vectors. Use only idle, unbound vectors in the owner's DT-derived bitmap.
// The destination is an explicit xAPIC physical ID, never inferred from IRQs.
// Failure preserves maps, queue, registry and output. Success publishes disabled,
// unrouted IRQs and fixed/edge message values; no device or APIC register writes.
int32_t allocateMessageInterrupts(struct MessageInterrupts* messages, struct InterruptVectors* vectors,
    const struct LocalApic* apic, struct PendingInterrupts* pending, unsigned destination,
    unsigned count, struct InterruptMessage* result);

// Exact original block only. The caller has removed handlers and quiesced the
// device, so no message can still be in flight. Reject pending, active/running,
// enabled or routed IRQs and APIC delivery without partially freeing the block.
int32_t freeMessageInterrupts(struct MessageInterrupts* messages, struct InterruptVectors* vectors,
    const struct LocalApic* apic, struct PendingInterrupts* pending, unsigned first_irq, unsigned count);
