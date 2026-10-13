/* SPDX-License-Identifier: MPL-2.0 */
#include "dispatch.h"

#include <libraries/gic400.h>

#include "server.h"
#include "pending.h"
#include "arrival.h"

int32_t initInterruptDispatch(const struct InterruptVectors* vectors, const struct LocalApic* apic,
    struct ExecBase* exec_base, struct Interrupt** handlers, uint32_t max_irqs, struct InterruptDispatch* dispatch)
{
    if (!vectors || !apic || !apic->registers || !exec_base || !handlers || !dispatch ||
        max_irqs < INTERRUPT_SHARED_FIRST || max_irqs > INTERRUPT_IRQ_LIMIT) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct InterruptDispatch result = {
        .vectors = vectors, .apic = apic, .exec_base = exec_base,
        .handlers = handlers, .max_irqs = max_irqs
    };
    *dispatch = result;
    return 0;
}

int32_t dispatchInterrupt(struct InterruptDispatch* dispatch asm("a1"))
{
    if (!dispatch) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    uint32_t irq;
    int32_t status = activeInterruptIrq(dispatch->vectors, dispatch->apic, &irq);

    if (status == GIC400_ERR_NOT_FOUND) {
        return 0;
    }

    if (status) {
        return status;
    }

    if (irq >= dispatch->max_irqs) {
        return GIC400_ERR_INVALID_IRQ;
    }

    unsigned vector = dispatch->vectors->vector_for_irq[irq];
    unsigned bank = vector / 32;
    uint32_t bit = UINT32_C(1) << (vector & 31);

    if (dispatch->calling[bank] & bit) {
        return GIC400_ERR_NOT_READY;
    }

    if (!(dispatch->dispatched[bank] & bit)) {
        dispatch->dispatched[bank] |= bit;
        dispatch->calling[bank] |= bit;
        ++dispatch->active_count;
        struct Interrupt* interrupt = dispatch->handlers[irq];

        if (interrupt && interrupt->is_Code) {
            callInterruptServer(interrupt, irq, dispatch->exec_base);
        }

        dispatch->calling[bank] &= ~bit;
    }

    status = completeInterruptIrq(dispatch->vectors, dispatch->apic, irq);

    if (status) {
        return status;
    }

    dispatch->dispatched[bank] &= ~bit;
    --dispatch->active_count;
    return 1;
}

int32_t dispatchPendingInterrupt(struct PendingInterruptDispatch* dispatch asm("a1"))
{
    if (!dispatch || !dispatch->interrupts || !dispatch->exec_base || !dispatch->handlers) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    if (dispatch->arrival && dispatch->arrival->interrupts != dispatch->interrupts) {
        return GIC400_ERR_INVALID_ARGUMENT;
    }

    struct PendingInterrupts* interrupts = dispatch->interrupts;
    int32_t irq = interrupts->depth ? interrupts->stack[interrupts->depth - 1] : GIC400_ERR_NOT_FOUND;
    bool returned = irq >= 0 && (dispatch->returned[irq / 32] & (UINT32_C(1) << (irq & 31)));

    if (!returned) {
        irq = takePendingInterrupt(interrupts);

        if (irq == GIC400_ERR_NOT_FOUND) {
            return 0;
        }

        if (irq < 0) {
            return irq;
        }

        struct Interrupt* interrupt = dispatch->handlers[irq];

        if (interrupt && interrupt->is_Code) {
            callInterruptServer(interrupt, irq, dispatch->exec_base);
        }

        dispatch->returned[irq / 32] |= UINT32_C(1) << (irq & 31);
    }

    int32_t status = dispatch->arrival ? completeCollectedInterrupt(dispatch->arrival, irq) :
        completePendingInterrupt(interrupts, irq);

    if (status) {
        return status;
    }

    dispatch->returned[irq / 32] &= ~(UINT32_C(1) << (irq & 31));
    return 1;
}
