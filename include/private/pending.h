/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "irq.h"

#define INTERRUPT_WORDS ((INTERRUPT_IRQ_LIMIT + 31) / 32)

struct PendingInterrupts {
    uint32_t available[INTERRUPT_WORDS];
    uint32_t enabled[INTERRUPT_WORDS], routed[INTERRUPT_WORDS];
    uint32_t pending[INTERRUPT_WORDS], active[INTERRUPT_WORDS], running[INTERRUPT_WORDS];
    uint8_t priority[INTERRUPT_IRQ_LIMIT];
    uint16_t stack[256];
    uint8_t running_priority[256];
    unsigned max_irqs, depth, mask;
};

enum InterruptStateField {
    INTERRUPT_ENABLED, INTERRUPT_ROUTED, INTERRUPT_PENDING, INTERRUPT_ACTIVE, INTERRUPT_PRIORITY
};

struct PendingInterruptState {
    bool enabled, routed, pending, active;
    uint8_t priority;
};

// Library-owned scheduling for one dispatch CPU. Availability is explicit: holes
// and reserved IRQs cannot be queued. The controller owner serializes all calls,
// holds storage for the full callback lifetime and supplies hardware arrival and
// route eligibility. No MMIO, allocation, interrupt injection or public vectors.
// Initialize only before publication, never while callbacks are outstanding.
int32_t initPendingInterrupts(const uint32_t available[INTERRUPT_WORDS], unsigned max_irqs,
    unsigned mask, struct PendingInterrupts* interrupts);
int32_t setPendingInterruptState(struct PendingInterrupts* interrupts, unsigned irq,
    enum InterruptStateField field, unsigned value);
int32_t getPendingInterruptState(const struct PendingInterrupts* interrupts, unsigned irq,
    struct PendingInterruptState* state);
int32_t setPendingInterruptMask(struct PendingInterrupts* interrupts, unsigned mask);

// Lower numeric priority wins; equality with the mask/running priority blocks.
// Equal eligible priorities use the lower IRQ. An empty selection returns
// NOT_FOUND. Pending requests coalesce, including requests made while active.
// Selection visits only pending, enabled, routed, nonactive and nonrunning IRQs.
int32_t highestPendingInterrupt(const struct PendingInterrupts* interrupts);
int32_t takePendingInterrupt(struct PendingInterrupts* interrupts);

// LIFO completion uses priority captured at take, independently of subsequent
// SetIntPriority/SetIntActive calls. Clearing active cannot reenter a running
// callback. Completion clears active but retains requests raised during it.
// Masking, unroute or handler removal cannot discard an outstanding completion.
// Hardware acknowledgement and level-input masking remain the owner's job.
int32_t completePendingInterrupt(struct PendingInterrupts* interrupts, unsigned irq);
