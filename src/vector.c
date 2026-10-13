/* SPDX-License-Identifier: MPL-2.0 */
#include "vector.h"

#include <libraries/gic400.h>

#define VECTOR_UNBOUND UINT16_MAX

int32_t initInterruptVectors(const uint32_t available[8], struct InterruptVectors* vectors)
{
    if (!available || !vectors || available[0] || (available[7] & (UINT32_C(1) << 31))) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct InterruptVectors result;

    for (unsigned i = 0; i < 8; ++i) {
        result.available[i] = available[i];
    }

    for (unsigned i = 0; i < 256; ++i) {
        result.irq_for_vector[i] = VECTOR_UNBOUND;
    }

    for (unsigned i = 0; i < INTERRUPT_IRQ_LIMIT; ++i) {
        result.vector_for_irq[i] = 0;
    }

    *vectors = result;
    return 0;
}

static int32_t vectorForIrq(const struct InterruptVectors* vectors, uint32_t irq)
{
    if (!vectors) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (irq < INTERRUPT_SHARED_FIRST || irq >= INTERRUPT_IRQ_LIMIT) {
        return GIC400_ERR_INVALID_IRQ;
    }

    unsigned vector = vectors->vector_for_irq[irq];
    return vector ? (int32_t)vector : GIC400_ERR_NOT_FOUND;
}

static int32_t idleVector(const struct LocalApic* apic, unsigned vector)
{
    bool pending, active;
    int32_t status = localApicStatus(apic, vector, &pending, &active);

    if (status) {
        return status;
    }

    return pending || active ? GIC400_ERR_NOT_READY : 0;
}

int32_t bindInterruptVector(struct InterruptVectors* vectors, const struct LocalApic* apic,
    uint32_t irq, unsigned vector)
{
    if (!vectors || !apic || !apic->registers || vector < 32 || vector >= 255) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (irq < INTERRUPT_SHARED_FIRST || irq >= INTERRUPT_IRQ_LIMIT) {
        return GIC400_ERR_INVALID_IRQ;
    }

    if (!(vectors->available[vector / 32] & (UINT32_C(1) << (vector & 31)))) {
        return GIC400_ERR_NOT_ROUTABLE;
    }

    int32_t existing = vectorForIrq(vectors, irq);

    if (existing == (int32_t)vector) {
        return 0;
    }

    if (existing >= 0 || vectors->irq_for_vector[vector] != VECTOR_UNBOUND) {
        return GIC400_ERR_ALREADY_REGISTERED;
    }

    int32_t status = idleVector(apic, vector);

    if (status) {
        return status;
    }

    vectors->irq_for_vector[vector] = irq;
    vectors->vector_for_irq[irq] = vector;
    return 0;
}

int32_t releaseInterruptVector(struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t irq)
{
    int32_t vector = vectorForIrq(vectors, irq);

    if (vector < 0) {
        return vector;
    }

    int32_t status = idleVector(apic, vector);

    if (status) {
        return status;
    }

    vectors->irq_for_vector[vector] = VECTOR_UNBOUND;
    vectors->vector_for_irq[irq] = 0;
    return 0;
}

int32_t activeInterruptIrq(const struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t* irq)
{
    if (!vectors || !irq) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    int32_t vector = highestLocalApic(apic, LOCAL_APIC_IN_SERVICE);

    if (vector < 0) {
        return vector;
    }

    if (vectors->irq_for_vector[vector] == VECTOR_UNBOUND) {
        return GIC400_ERR_NOT_FOUND;
    }

    *irq = vectors->irq_for_vector[vector];
    return 0;
}

int32_t completeInterruptIrq(const struct InterruptVectors* vectors, const struct LocalApic* apic, uint32_t irq)
{
    int32_t vector = vectorForIrq(vectors, irq);
    return vector < 0 ? vector : completeLocalApic(apic, vector);
}
