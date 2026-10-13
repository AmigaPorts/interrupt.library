/* SPDX-License-Identifier: MPL-2.0 */
#include "domain.h"

#include <libraries/gic400.h>

LONG initInterruptDomains(const struct IoApicDomain* domains, ULONG count, struct InterruptDomains* result)
{
    if (!result || (count && !domains)) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    ULONG limit = INTERRUPT_SHARED_FIRST;

    for (ULONG i = 0; i < count; ++i) {
        const struct IoApicDomain* domain = &domains[i];

        if (!domain->key || !domain->apic.registers || ((uintptr_t)domain->apic.registers & 3) ||
            !domain->apic.version || domain->apic.version == 255 || !domain->apic.input_count ||
            domain->apic.input_count > IOAPIC_INPUTS_MAX ||
            domain->gsi_base >= INTERRUPT_IRQ_LIMIT - INTERRUPT_SHARED_FIRST ||
            domain->apic.input_count > INTERRUPT_IRQ_LIMIT - INTERRUPT_SHARED_FIRST - domain->gsi_base) {
            return GIC400_ERR_DEVTREE;
        }

        ULONG end = domain->gsi_base + domain->apic.input_count;

        for (ULONG j = 0; j < i; ++j) {
            const struct IoApicDomain* previous = &domains[j];

            if (domain->key == previous->key || domain->apic.registers == previous->apic.registers ||
                (domain->gsi_base < previous->gsi_base + previous->apic.input_count &&
                 previous->gsi_base < end)) {
                     return GIC400_ERR_DEVTREE;
                 }
        }

        end += INTERRUPT_SHARED_FIRST;

        if (end > limit) {
            limit = end;
        }
    }

    *result = (struct InterruptDomains) {domains, count, limit};
    return 0;
}

LONG findInterruptSource(const struct InterruptDomains* domains, ULONG irq, struct InterruptSource* source)
{
    if (!domains || (domains->count && !domains->domains)) {
        return GIC400_ERR_NOT_READY;
    }

    if (!source) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (irq < INTERRUPT_SHARED_FIRST || irq >= domains->max_irqs) {
        return GIC400_ERR_INVALID_IRQ;
    }

    ULONG gsi = irq - INTERRUPT_SHARED_FIRST;

    for (ULONG i = 0; i < domains->count; ++i) {
        const struct IoApicDomain* domain = &domains->domains[i];

        if (gsi >= domain->gsi_base && gsi - domain->gsi_base < domain->apic.input_count) {
            *source = (struct InterruptSource) {domain, gsi - domain->gsi_base};
            return 0;
        }
    }

    return GIC400_ERR_INVALID_IRQ;
}

LONG findInterruptIrq(const struct InterruptDomains* domains, APTR key, ULONG pin, ULONG* irq)
{
    if (!domains || (domains->count && !domains->domains)) {
        return GIC400_ERR_NOT_READY;
    }

    if (!key || !irq) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    for (ULONG i = 0; i < domains->count; ++i) {
        const struct IoApicDomain* domain = &domains->domains[i];

        if (domain->key != key) {
            continue;
        }

        if (pin >= domain->apic.input_count) {
            return GIC400_ERR_INVALID_IRQ;
        }

        *irq = INTERRUPT_SHARED_FIRST + domain->gsi_base + pin;
        return 0;
    }

    return GIC400_ERR_NOT_FOUND;
}
