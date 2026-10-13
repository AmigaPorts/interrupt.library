/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "vector.h"

struct ExecBase;
struct Interrupt;

struct InterruptDispatch {
    const struct InterruptVectors* vectors;
    const struct LocalApic* apic;
    struct ExecBase* exec_base;
    struct Interrupt** handlers;
    uint32_t max_irqs;
    uint32_t active_count;
    uint32_t dispatched[8];
    uint32_t calling[8];
};

// One dispatcher per destination CPU, borrowing a stable initialized vector map,
// local APIC and handler table. The owner initializes the table and retains all
// storage until active_count is zero and dispatch entry is excluded. This is
// internal dispatch, not public registration: source routing/masking, priorities
// and handler-table changes remain the controller owner's responsibility.
int32_t initInterruptDispatch(const struct InterruptVectors* vectors, const struct LocalApic* apic,
    struct ExecBase* exec_base, struct Interrupt** handlers, uint32_t max_irqs, struct InterruptDispatch* dispatch);

// Receives its context in A1, following the Exec server convention.
// Run on the owning CPU with native IRQs excluded. Returns 1 after completing an
// owned vector, 0 for empty/foreign ISR, or a negative error. A callback can remove
// itself; its vector must remain bound until EOI and no server is read after the
// call. Re-entry cannot call or complete a server still running. Failed completion
// retains active_count and delivery state: retry EOI without invoking the server
// again. No-handler delivery still completes. Device acknowledgement precedes EOI.
int32_t dispatchInterrupt(struct InterruptDispatch* dispatch asm("a1"));


struct PendingInterrupts;
struct InterruptArrival;
struct PendingInterruptDispatch {
    struct PendingInterrupts* interrupts;
    struct ExecBase* exec_base;
    struct Interrupt** handlers;
    const struct InterruptArrival* arrival;
    uint32_t returned[(INTERRUPT_IRQ_LIMIT + 31) / 32];
};

// Dispatch one queued IRQ through the original callback ABI. The owner retains
// this table (at least interrupts->max_irqs entries) and queue until depth reaches
// zero. A server can remove itself or recursively dispatch a higher-priority
// request. Device acknowledgement remains in the server. Hardware arrival
// collection and APIC completion are separate from callback dispatch. If arrival
// is supplied, it must use this queue: completion also updates its source mask.
// Initialize returned to zero. Retain failed completions and retry them before
// taking another request, without repeating the callback. All controller users
// and native IRQs must be excluded, including across callback completion.
// Returns 1 after a completed callback (also for a removed server), 0 if nothing
// is eligible, or a negative error. No handler is accessed after its callback.
int32_t dispatchPendingInterrupt(struct PendingInterruptDispatch* dispatch asm("a1"));
