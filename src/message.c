/* SPDX-License-Identifier: MPL-2.0 */
#include "message.h"

#define MESSAGE_CONTINUATION 255

int32_t initMessageInterrupts(unsigned first_irq, struct MessageInterrupts* messages)
{
    if (!messages || first_irq < INTERRUPT_SHARED_FIRST || first_irq > INTERRUPT_IRQ_LIMIT) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct MessageInterrupts value = {.first_irq = first_irq};
    *messages = value;
    return 0;
}

static bool valid(const struct MessageInterrupts* messages, const struct InterruptVectors* vectors,
    const struct LocalApic* apic, const struct PendingInterrupts* pending, unsigned count)
{
    return messages && vectors && apic && apic->registers && pending &&
        messages->first_irq >= INTERRUPT_SHARED_FIRST && messages->first_irq <= INTERRUPT_IRQ_LIMIT &&
        pending->max_irqs >= messages->first_irq && pending->max_irqs <= INTERRUPT_IRQ_LIMIT &&
        count && count <= 32 && !(count & (count - 1));
}

int32_t allocateMessageInterrupts(struct MessageInterrupts* messages, struct InterruptVectors* vectors,
    const struct LocalApic* apic, struct PendingInterrupts* pending, unsigned destination,
    unsigned count, struct InterruptMessage* result)
{
    if (!result || destination >= 255 || !valid(messages, vectors, apic, pending, count)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    unsigned first;

    for (first = messages->first_irq; first + count <= INTERRUPT_IRQ_LIMIT; ++first) {
        unsigned i;

        for (i = 0; i < count; ++i) {
            unsigned irq = first + i;

            if (messages->allocation[irq] || vectors->vector_for_irq[irq] ||
                (pending->available[irq / 32] & (UINT32_C(1) << (irq & 31)))) {
                break;
            }
        }

        if (i == count) {
            break;
        }
    }

    if (first + count > INTERRUPT_IRQ_LIMIT) {
        return GIC400_ERR_NO_MEMORY;
    }

    struct InterruptVectors candidate;
    unsigned vector;

    for (vector = 32; vector + count <= 255; vector += count) {
        unsigned i;

        for (i = 0; i < count; ++i) {
            unsigned current = vector + i;

            if (!(vectors->available[current / 32] & (UINT32_C(1) << (current & 31))) ||
                vectors->irq_for_vector[current] != UINT16_MAX) {
                break;
            }
        }

        if (i != count) {
            continue;
        }

        candidate = *vectors;

        for (i = 0; i < count; ++i) {
            int32_t status = bindInterruptVector(&candidate, apic, first + i, vector + i);

            if (status == GIC400_ERR_NOT_READY) {
                break;
            }

            if (status) {
                return status;
            }
        }

        if (i == count) {
            break;
        }
    }

    if (vector + count > 255) {
        return GIC400_ERR_NO_MEMORY;
    }

    // Commit only after every hardware check. Native collection and all queue
    // users are excluded, and no device has received these message values yet.
    *vectors = candidate;

    for (unsigned i = 0; i < count; ++i) {
        unsigned irq = first + i, bank = irq / 32;
        uint32_t bit = UINT32_C(1) << (irq & 31);
        messages->allocation[irq] = i ? MESSAGE_CONTINUATION : count;
        pending->available[bank] |= bit;
        pending->enabled[bank] &= ~bit;
        pending->routed[bank] &= ~bit;
        pending->pending[bank] &= ~bit;
        pending->active[bank] &= ~bit;
        pending->running[bank] &= ~bit;
        pending->priority[irq] = 0;
        // Intel xAPIC MSI wire format: fixed prefix, physical destination,
        // fixed delivery, edge trigger, level asserted. This is message encoding,
        // not discovery of an APIC register mapping or an emulator interface.
        result[i] = (struct InterruptMessage) {irq, UINT32_C(0xfee00000) | (destination << 12),
            0, UINT32_C(0x4000) | (vector + i)};
    }

    ++messages->blocks;

    if (pending->max_irqs < first + count) {
        pending->max_irqs = first + count;
    }

    return 0;
}

int32_t freeMessageInterrupts(struct MessageInterrupts* messages, struct InterruptVectors* vectors,
    const struct LocalApic* apic, struct PendingInterrupts* pending, unsigned first_irq, unsigned count)
{
    if (!valid(messages, vectors, apic, pending, count) || first_irq < messages->first_irq ||
        first_irq >= INTERRUPT_IRQ_LIMIT || count > INTERRUPT_IRQ_LIMIT - first_irq ||
        messages->allocation[first_irq] != count || !messages->blocks) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct InterruptVectors candidate = *vectors;

    for (unsigned i = 0; i < count; ++i) {
        unsigned irq = first_irq + i;

        if (i && messages->allocation[irq] != MESSAGE_CONTINUATION) {
            return GIC400_ERR_INVALID_ARGUMENT;
        }

        struct PendingInterruptState current;
        int32_t status = getPendingInterruptState(pending, irq, &current);

        if (status) {
            return status;
        }

        if (current.enabled || current.routed || current.pending || current.active ||
            (pending->running[irq / 32] & (UINT32_C(1) << (irq & 31)))) {
            return GIC400_ERR_NOT_READY;
        }

        status = releaseInterruptVector(&candidate, apic, irq);

        if (status) {
            return status;
        }
    }

    *vectors = candidate;

    for (unsigned i = 0; i < count; ++i) {
        unsigned irq = first_irq + i;
        messages->allocation[irq] = 0;
        pending->available[irq / 32] &= ~(UINT32_C(1) << (irq & 31));
        pending->priority[irq] = 0;
    }

    --messages->blocks;

    while (pending->max_irqs > messages->first_irq && !messages->allocation[pending->max_irqs - 1]) {
        --pending->max_irqs;
    }

    return 0;
}
