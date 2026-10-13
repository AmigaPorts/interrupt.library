/* SPDX-License-Identifier: MPL-2.0 */
#include "arrival.h"

#include <libraries/gic400.h>

int32_t collectPendingInterrupt(const struct InterruptArrival* arrival)
{
    if (!arrival) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t irq;
    int32_t status = activeInterruptIrq(arrival->vectors, arrival->apic, &irq);

    if (status == GIC400_ERR_NOT_FOUND) {
        return 0;
    }

    if (status) {
        return status;
    }

    struct PendingInterruptState pending;
    status = getPendingInterruptState(arrival->interrupts, irq, &pending);

    if (status) {
        return status;
    }

    if (isMessageInterrupt(arrival->messages, irq)) {
        status = completeInterruptIrq(arrival->vectors, arrival->apic, irq);

        if (status) {
            return status;
        }

        status = setPendingInterruptState(arrival->interrupts, irq, INTERRUPT_PENDING, 1);
        return status ? status : 1;
    }

    struct InterruptSource source;
    status = findInterruptSource(arrival->domains, irq, &source);

    if (status) {
        return status;
    }

    struct IoApicState hardware;
    status = ioApicState(&source.domain->apic, source.pin, &hardware);

    if (status) {
        return status;
    }

    if (hardware.vector != arrival->vectors->vector_for_irq[irq] || hardware.delivery_mode ||
        hardware.logical_destination) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    if (hardware.level_triggered) {
        status = maskIoApic(&source.domain->apic, source.pin, true);

        if (status) {
            return status;
        }
    }

    status = completeInterruptIrq(arrival->vectors, arrival->apic, irq);

    if (status) {
        return status;
    }

    // Validation preceded all hardware writes, and all users are excluded.
    // Setting pending coalesces additional arrivals without altering active.
    status = setPendingInterruptState(arrival->interrupts, irq, INTERRUPT_PENDING, 1);
    return status ? status : 1;
}

static int32_t updateMask(const struct InterruptArrival* arrival, unsigned irq, bool completing)
{
    if (!arrival || !arrival->vectors) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct PendingInterruptState pending;
    int32_t status = getPendingInterruptState(arrival->interrupts, irq, &pending);

    if (status) {
        return status;
    }

    const struct PendingInterrupts* interrupts = arrival->interrupts;

    if (completing && (!interrupts->depth || interrupts->stack[interrupts->depth - 1] != irq)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    unsigned vector = arrival->vectors->vector_for_irq[irq];

    if (vector < 32 || vector >= 255 || arrival->vectors->irq_for_vector[vector] != irq) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    if (isMessageInterrupt(arrival->messages, irq)) {
        return 0;
    }

    struct InterruptSource source;
    status = findInterruptSource(arrival->domains, irq, &source);

    if (status) {
        return status;
    }

    struct IoApicState hardware;
    status = ioApicState(&source.domain->apic, source.pin, &hardware);

    if (status) {
        return status;
    }

    if (hardware.vector != vector || hardware.delivery_mode || hardware.logical_destination) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    bool running = !!(interrupts->running[irq / 32] & (UINT32_C(1) << (irq & 31)));
    bool masked = !pending.enabled || !pending.routed ||
        (hardware.level_triggered && (pending.pending || (!completing && (pending.active || running))));

    if (hardware.masked != masked) {
        return maskIoApic(&source.domain->apic, source.pin, masked);
    }

    return 0;
}

int32_t updateCollectedInterruptMask(const struct InterruptArrival* arrival, unsigned irq)
{
    return updateMask(arrival, irq, false);
}

int32_t completeCollectedInterrupt(const struct InterruptArrival* arrival, unsigned irq)
{
    int32_t status = updateMask(arrival, irq, true);

    if (status) {
        return status;
    }

    // The top callback was validated before MMIO and all users remain excluded.
    return completePendingInterrupt(arrival->interrupts, irq);
}

int32_t collectInterruptArrivals(const struct InterruptArrival* arrival, unsigned wake_vector,
    bool* wake_outstanding)
{
    if (!arrival || !wake_outstanding || wake_vector < 32 || wake_vector >= 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    int32_t collected = 0;

    for (;;) {
        int32_t vector = highestLocalApic(arrival->apic, LOCAL_APIC_IN_SERVICE);

        if (vector == GIC400_ERR_NOT_FOUND) {
            return collected;
        }

        if (vector < 0) {
            return vector;
        }

        if ((unsigned)vector == wake_vector) {
            int32_t result = completeLocalApic(arrival->apic, wake_vector);

            if (result) {
                return result;
            }

            *wake_outstanding = false;
            ++collected;
        } else {
            int32_t result = collectPendingInterrupt(arrival);

            if (result <= 0) {
                return result < 0 ? result : collected;
            }

            collected += result;
        }
    }
}
