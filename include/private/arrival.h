/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "domain.h"
#include "pending.h"
#include "vector.h"
#include "message.h"

struct InterruptArrival {
    const struct InterruptDomains* domains;
    const struct InterruptVectors* vectors;
    const struct LocalApic* apic;
    struct PendingInterrupts* interrupts;
    const struct MessageInterrupts* messages;
};

// Collect one owned shared interrupt on the receiving CPU with native IRQs
// excluded. The owner must retain its domain/vector association until delivery
// is drained and own the entire indexed I/O APIC interface. Mask level inputs
// before EOI, then publish pending state. Edge inputs retain their current mask.
// Callbacks run separately; they must acknowledge the device before the owner
// rearms a level input. Disabled/unrouted software state does not discard an
// arrival already accepted by hardware. No priority decision or callback here.
// Allocated message IRQs use the same queue and local EOI without I/O APIC
// access; device-side masking remains with the PCI owner. A NULL message
// registry preserves the wired-only collector.
// Returns 1 for a collected arrival, 0 for empty/foreign ISR, or a negative error.
// Failure publishes no request; an input masked before failed EOI stays masked
// for retry. A foreign highest ISR is never acknowledged or skipped.
int32_t collectPendingInterrupt(const struct InterruptArrival* arrival);

// Apply software enable/route state to this owned input. Level inputs remain
// masked while pending, active or running (even after ClearIntActive). Priority
// masking does not discard physical arrivals. Retain the domain/vector binding
// until completion, including when a callback removes its server or route.
int32_t updateCollectedInterruptMask(const struct InterruptArrival* arrival, unsigned irq);

// Complete the top queued callback and apply its resulting input mask. The
// server must acknowledge its device first. A new pending request, disable or
// unroute keeps a level input masked; otherwise rearm it. Edge inputs follow
// enable/route state independently of callback activity. No APIC EOI occurs here:
// collection already performed it. Native IRQs and all other users must remain
// excluded through the hardware update and software completion. Failure retains
// the callback stack and active state so completion can be retried without
// calling the server again. Never release a vector on a failed completion.
int32_t completeCollectedInterrupt(const struct InterruptArrival* arrival, unsigned irq);

// Drain the owned ISR entries retained by one native notification, in APIC
// priority order. The separately reserved self-IPI vector completes local
// software wakeups; shared entries use the collection rules above. Stop at an
// empty or foreign highest ISR without acknowledging another owner's vector.
// Call with native arrivals and queue users excluded. Returns the number of
// completed entries or a negative error; already collected entries stay queued.
int32_t collectInterruptArrivals(const struct InterruptArrival* arrival, unsigned wake_vector,
    bool* wake_outstanding);
