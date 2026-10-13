/* SPDX-License-Identifier: MPL-2.0 */
#include "pending.h"

#include <libraries/gic400.h>

static int32_t ready(const struct PendingInterrupts* interrupts)
{
    return interrupts && interrupts->max_irqs && interrupts->max_irqs <= INTERRUPT_IRQ_LIMIT ?
        0 : GIC400_ERR_NOT_READY;
}

static int32_t validate(const struct PendingInterrupts* interrupts, unsigned irq)
{
    int32_t status = ready(interrupts);

    if (status) {
        return status;
    }

    if (irq >= interrupts->max_irqs || !(interrupts->available[irq / 32] & (UINT32_C(1) << (irq & 31)))) {
        return GIC400_ERR_INVALID_IRQ;
    }

    return 0;
}

int32_t initPendingInterrupts(const uint32_t available[INTERRUPT_WORDS], unsigned max_irqs,
    unsigned mask, struct PendingInterrupts* interrupts)
{
    if (!available || !interrupts || !max_irqs || max_irqs > INTERRUPT_IRQ_LIMIT || mask > 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    for (unsigned irq = max_irqs; irq < INTERRUPT_WORDS * 32; ++irq) {

        if (available[irq / 32] & (UINT32_C(1) << (irq & 31))) {
            return GIC400_ERR_INVALID_ARGUMENT;
        }
    }

    struct PendingInterrupts result = { .max_irqs = max_irqs, .mask = mask };

    for (unsigned i = 0; i < INTERRUPT_WORDS; ++i) {
        result.available[i] = available[i];
    }

    *interrupts = result;
    return 0;
}

int32_t setPendingInterruptState(struct PendingInterrupts* interrupts, unsigned irq,
    enum InterruptStateField field, unsigned value)
{
    int32_t status = validate(interrupts, irq);

    if (status) {
        return status;
    }

    if (value > (field == INTERRUPT_PRIORITY ? 255u : 1u)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t* bitmap;

    switch (field) {
        case INTERRUPT_ENABLED:
            bitmap = interrupts->enabled;
            break;
        case INTERRUPT_ROUTED:
            bitmap = interrupts->routed;
            break;
        case INTERRUPT_PENDING:
            bitmap = interrupts->pending;
            break;
        case INTERRUPT_ACTIVE:
            bitmap = interrupts->active;
            break;
        case INTERRUPT_PRIORITY:
            interrupts->priority[irq] = value;
            return 0;
        default:
            return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t bit = UINT32_C(1) << (irq & 31);

    if (value) {
        bitmap[irq / 32] |= bit;
    }
    else {
        bitmap[irq / 32] &= ~bit;
    }

    return 0;
}

int32_t getPendingInterruptState(const struct PendingInterrupts* interrupts, unsigned irq,
    struct PendingInterruptState* state)
{
    int32_t status = validate(interrupts, irq);

    if (status) {
        return status;
    }

    if (!state) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t bit = UINT32_C(1) << (irq & 31);
    unsigned bank = irq / 32;
    *state = (struct PendingInterruptState) {
        .enabled = !!(interrupts->enabled[bank] & bit), .routed = !!(interrupts->routed[bank] & bit),
        .pending = !!(interrupts->pending[bank] & bit), .active = !!(interrupts->active[bank] & bit),
        .priority = interrupts->priority[irq]
    };
    return 0;
}

int32_t setPendingInterruptMask(struct PendingInterrupts* interrupts, unsigned mask)
{
    int32_t status = ready(interrupts);

    if (status) {
        return status;
    }

    if (mask > 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    interrupts->mask = mask;
    return 0;
}

int32_t highestPendingInterrupt(const struct PendingInterrupts* interrupts)
{
    int32_t status = ready(interrupts);

    if (status) {
        return status;
    }

    unsigned limit = interrupts->depth ? interrupts->running_priority[interrupts->depth - 1] : 255;

    if (interrupts->mask < limit) {
        limit = interrupts->mask;
    }

    int32_t best = GIC400_ERR_NOT_FOUND;
    unsigned banks = (interrupts->max_irqs + 31) / 32;

    for (unsigned bank = 0; bank < banks; ++bank) {
        uint32_t candidates = interrupts->pending[bank] & interrupts->enabled[bank] & interrupts->routed[bank] &
            ~(interrupts->active[bank] | interrupts->running[bank]);

        while (candidates) {
            unsigned irq = 32 * bank + __builtin_ctz(candidates);
            candidates &= candidates - 1;
            unsigned priority = interrupts->priority[irq];

            if (priority < limit) {
                best = irq;
                limit = priority;

                if (!limit) {
                    return best;
                }
            }
        }
    }

    return best;
}

int32_t takePendingInterrupt(struct PendingInterrupts* interrupts)
{
    int32_t irq = highestPendingInterrupt(interrupts);

    if (irq < 0) {
        return irq;
    }

    // Priorities decrease strictly at each nesting level; at most 255 can run.
    uint32_t bit = UINT32_C(1) << (irq & 31);
    unsigned bank = (unsigned)irq / 32;
    interrupts->pending[bank] &= ~bit;
    interrupts->active[bank] |= bit;
    interrupts->running[bank] |= bit;
    interrupts->stack[interrupts->depth] = irq;
    interrupts->running_priority[interrupts->depth++] = interrupts->priority[irq];
    return irq;
}

int32_t completePendingInterrupt(struct PendingInterrupts* interrupts, unsigned irq)
{
    int32_t status = validate(interrupts, irq);

    if (status) {
        return status;
    }

    if (!interrupts->depth || interrupts->stack[interrupts->depth - 1] != irq) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t bit = UINT32_C(1) << (irq & 31);
    interrupts->active[irq / 32] &= ~bit;
    interrupts->running[irq / 32] &= ~bit;
    --interrupts->depth;
    return 0;
}
